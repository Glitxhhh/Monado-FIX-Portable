"""Build the game code library the runtime loads (release packages only).

Source builds compile the whole game into the executable. A release package ships
this builder instead: on first start it lifts the guest code from the player's own
`exefs/` into C (`tools/lifter`), compiles it with the bundled `zig cc` and links
`lasgame.so` / `lasgame.dll` in the writable data folder. Nothing derived from the
game is distributed, and the runtime refuses a library built by another generator
(GAMELIB_ID, tools/game_id.py).

Layout expected in the resources folder (`--resources`, default: `gamebuild/`
beside the executable, or `../share/monado/gamebuild` in an AppImage):

    link.json              compiler, guest flags, extra link inputs
    gamelib-id.txt         GAMELIB_ID the runtime was built with
    zig[.exe]              bundled compiler
    entry/gamelib_entry.c  the library's exported entry point
    include/               cpu.h, simd.h, simd_fp.h (the guest ABI)
    games/                 profile.json, patches.py, hle_symbols.txt

Usage: monado-gamebuild --root <dump> [--out DIR] [--resources DIR] [--jobs N]
                        [--force] [--dry-run]
Exit codes: 0 ok, 1 usage, 2 bad game files, 3 generation failed, 4 compile failed,
5 link failed. Progress is written for the launcher: `<out>/progress.txt`.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
for _sub in ('lifter', 'extract'):
    _path = HERE / _sub
    if _path.is_dir() and str(_path) not in sys.path:
        sys.path.insert(0, str(_path))

import gen  # noqa: E402  (tools/lifter/gen.py)

# GitHub #86: zig could sit idle for good on some Windows PCs and the log showed nothing past the lifting. Every step
# is now logged with a timestamp and flushed at once, each compiler command is written before it runs, the final link
# runs with -v into the log, and a stuck compiler ends in an error instead of a build that never finishes.
COMPILE_TIMEOUT = 20 * 60  # one chunk (they take seconds)
LINK_TIMEOUT = 45 * 60     # the library (minutes on a slow PC, including zig's one-time C runtime build)
_log_lock = threading.Lock()
_log_start = time.time()


def note(log, text):
    with _log_lock:
        log.write(f'[{time.strftime("%H:%M:%S")} +{time.time() - _log_start:.0f}s] {text}\n')
        log.flush()


def quoted(command):
    return ' '.join(f'"{part}"' if ' ' in str(part) else str(part) for part in command)


def run_tool(command, env, log, what, timeout, stream=False):
    """Run the compiler. stream: its output goes straight into the log (visible while it runs)."""
    note(log, f'{what}: {quoted(command)}')
    started = time.time()
    try:
        if stream:
            with _log_lock:
                log.flush()
                result = subprocess.run(command, text=True, stdout=log, stderr=subprocess.STDOUT, env=env,
                                        timeout=timeout, stdin=subprocess.DEVNULL)
        else:
            result = subprocess.run(command, text=True, capture_output=True, env=env, timeout=timeout,
                                    stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        note(log, f'{what}: no result after {timeout // 60} minutes: stopped (the compiler hung)')
        raise SystemExit(f'gamebuild: {what} hung for {timeout // 60} minutes; see the log')
    note(log, f'{what}: exit {result.returncode} after {time.time() - started:.1f}s')
    return result


def describe_compiler(resources, out, link, log):
    """The compiler's version and settings (its cache folder above all) at the top of the log."""
    compiler = resources / link['compiler']
    env = compiler_env(resources, out, link)
    note(log, f'platform {sys.platform}, {os.cpu_count()} CPUs, compiler {compiler}')
    note(log, f'ZIG_LIB_DIR={env.get("ZIG_LIB_DIR", "(unset)")} ZIG_GLOBAL_CACHE_DIR={env["ZIG_GLOBAL_CACHE_DIR"]} '
              f'link: {"prebuilt C runtime" if link.get("lld") else "zig cc"}')
    for args in (['version'], ['env']):
        try:
            result = run_tool([str(compiler)] + args, env, log, f'zig {args[0]}', 120)
            with _log_lock:
                log.write((result.stdout or '') + (result.stderr or ''))
                log.flush()
        except SystemExit:
            pass  # (already logged; the build itself decides)

def default_data_root():
    override = os.environ.get('LARECOMP_DATA_ROOT') or os.environ.get('MONADO_DATA_ROOT')
    if override:
        return Path(override)
    if sys.platform == 'win32':
        base = os.environ.get('LOCALAPPDATA') or os.environ.get('APPDATA') or str(Path.home())
        return Path(base) / 'Monado'
    if sys.platform == 'darwin':
        return Path.home() / 'Library' / 'Application Support' / 'Monado'
    xdg = os.environ.get('XDG_DATA_HOME')
    return (Path(xdg) if xdg else Path.home() / '.local' / 'share') / 'monado'


def resources_dir(explicit):
    here = Path(sys.executable if getattr(sys, 'frozen', False) else __file__).resolve().parent
    for candidate in (explicit, os.environ.get('MONADO_GAMEBUILD_DIR'),
                      here, here / 'gamebuild', here.parent / 'share' / 'monado' / 'gamebuild',
                      here.parent / 'gamebuild'):
        if candidate and (Path(candidate) / 'link.json').is_file():
            return Path(candidate)
    raise SystemExit('gamebuild: no game-build resources found (link.json); '
                     'pass --resources <package gamebuild folder>')


def progress_file(out):
    return out / 'progress.txt'


def report(out, phase, done, total, text=''):
    path = progress_file(out)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix('.tmp')
        temporary.write_text(f'{phase} {done} {total} {text}\n', encoding='utf-8')
        os.replace(temporary, path)
    except OSError:
        pass  # a progress file the launcher cannot read must never fail the build


def fingerprint(dump, resources, link):
    """Everything a rebuild depends on: the generator/ABI id, the dump and the link inputs."""
    import hashlib
    h = hashlib.sha256()
    h.update((resources / 'gamelib-id.txt').read_text(encoding='utf-8').strip().encode())
    h.update(json.dumps(link, sort_keys=True).encode())
    for name, relative in (('main', 'exefs/main'), ('sdk', 'exefs/sdk')):
        path = dump / relative
        st = path.stat()
        h.update(f'{name} {st.st_size} {int(st.st_mtime)}\n'.encode())
    for extra in link.get('link_files', []):
        path = resources / extra
        if path.is_file():
            h.update(path.name.encode())
            h.update(str(path.stat().st_size).encode())
    crt = resources / 'crt'
    if crt.is_dir():
        for path in sorted(crt.iterdir()):
            h.update(f'{path.name} {path.stat().st_size}\n'.encode())
    return h.hexdigest()


def load_profile(resources):
    return json.loads((resources / 'games/profile.json').read_text(encoding='utf-8'))


def validate(dump, resources):
    import check_dump
    result = check_dump.check_dump(str(dump), load_profile(resources))
    if not result.ok:
        raise SystemExit(f'gamebuild: {dump}: ' + '; '.join(result.messages))


def generate_modules(dump, resources, out, jobs, log):
    """Lift every module the profile marks `lift` into <out>/generated/<module>/*.c."""
    profile = load_profile(resources)
    modules = [m for m in profile['modules'] if m.get('lift')]
    generated = out / 'generated'
    generated.mkdir(parents=True, exist_ok=True)
    for index, module in enumerate(modules):
        report(out, 'generate', index, len(modules), f'Reading {module["name"]}')
        argv = [module['name'], str(dump / 'exefs' / module['name']), str(module['base']),
                str(generated / module['name']),
                '--simd', 'c',
                '--patches', str(resources / 'games/patches.py'),
                '--hle', str(resources / 'games/hle_symbols.txt')]
        log.write(f'\n=== generate {module["name"]} ===\n')
        log.flush()
        saved_argv, saved_stderr = sys.argv, sys.stderr
        try:
            sys.argv = ['gen.py'] + argv
            sys.stderr = log
            gen.main(argv)
        except SystemExit as error:
            if error.code:
                raise SystemExit(f'gamebuild: lifting {module["name"]} failed: {error.code}')
        except Exception as error:  # noqa: BLE001 - reported to the player, not raised
            raise SystemExit(f'gamebuild: lifting {module["name"]} failed: {error}')
        finally:
            sys.argv, sys.stderr = saved_argv, saved_stderr
    report(out, 'generate', len(modules), len(modules), 'Game code read')
    return modules


def compiler_env(resources, out, link):
    """The bundled zig finds its trimmed lib/ folder here (the binary alone would not)."""
    cache = str(out / 'zig-cache')
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=cache, ZIG_LOCAL_CACHE_DIR=cache)
    if link.get('lib_dir'):
        env['ZIG_LIB_DIR'] = str(resources / link['lib_dir'])
    return env


def compile_units(resources, out, link, jobs, log):
    """Compile every generated chunk with the bundled compiler, in parallel."""
    compiler = resources / link['compiler']
    generated = out / 'generated'
    units = sorted(generated.glob('*/*.c'))
    if not units:
        raise SystemExit('gamebuild: no generated sources')
    include = [str(resources / item) for item in link.get('include', ['include'])]
    objects = out / 'obj'
    objects.mkdir(parents=True, exist_ok=True)
    stamp = out / 'build.json'
    previous = {}
    if stamp.is_file():
        try:
            previous = json.loads(stamp.read_text(encoding='utf-8')).get('units', {})
        except (OSError, ValueError):
            previous = {}
    done = [0]
    failures = []

    def build(source):
        target = objects / source.parent.name / (source.stem + '.o')
        target.parent.mkdir(parents=True, exist_ok=True)
        key = source.relative_to(generated).as_posix()
        signature = f'{source.stat().st_size} {int(source.stat().st_mtime)}'
        if previous.get(key) == signature and target.is_file():
            done[0] += 1
            return key, signature, target
        command = ([str(compiler)] + list(link.get('compile', ['cc'])) + list(link.get('guest_flags', [])) +
                   [f'-I{item}' for item in include] + ['-c', str(source), '-o', str(target)])
        result = run_tool(command, compiler_env(resources, out, link), log, f'compile {key}', COMPILE_TIMEOUT)
        if result.returncode != 0:
            failures.append(f'{key}:\n{result.stdout}{result.stderr}')
        done[0] += 1
        report(out, 'compile', done[0], len(units), f'Compiling {key}')
        return key, signature, target

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(build, units))
    if failures:
        log.write('\n'.join(failures))
        log.flush()
        raise SystemExit(f'gamebuild: {len(failures)} chunk(s) failed to compile; see {out / "gamebuild.log"}')
    return results


def link_library(resources, out, link, units, log):
    """Link the compiled chunks and the entry point into the game code library."""
    compiler = resources / link['compiler']
    entry = out / 'obj' / 'gamelib_entry.o'
    command = [str(compiler)] + list(link.get('compile', ['cc'])) + list(link.get('guest_flags', [])) + [
        f'-I{resources / item}' for item in link.get('include', ['include'])] + [
        '-DGAMELIB_ID="{}"'.format((resources / 'gamelib-id.txt').read_text(encoding='utf-8').strip()),
        '-c', str(resources / 'entry/gamelib_entry.c'), '-o', str(entry)]
    env = compiler_env(resources, out, link)
    result = run_tool(command, env, log, 'compile the entry point', COMPILE_TIMEOUT)
    if result.returncode != 0:
        log.write(result.stdout + result.stderr)
        log.flush()
        raise SystemExit('gamebuild: linking the entry point failed')
    library = out / os.path.basename(link['library'])
    inputs = ([str(entry)] + [str(target) for _, _, target in units] +
              [str(resources / extra) for extra in link.get('link_files', [])])
    if link.get('lld'):
        args = []
        for arg in link['lld']:
            if arg == '{inputs}':
                args += inputs
            else:
                args.append(arg.replace('{crt}', str(resources / 'crt')).replace('{dir}', str(out))
                            .replace('{entry}', entry.stem).replace('{name}', library.stem))
        response = out / 'link.rsp'
        response.write_text(''.join(f'"{arg}"\n' for arg in args), encoding='utf-8')
        command = [str(compiler), 'lld-link', f'@{response}']
    else:
        command = [str(compiler)] + list(link.get('link', ['cc', '-shared'])) + inputs + ['-v', '-o', str(library)]
    result = run_tool(command, env, log, 'link', LINK_TIMEOUT, stream=True)
    if result.returncode != 0:
        raise SystemExit('gamebuild: linking the game code library failed')
    # What the runtime reads before loading it (runtime/gamelib.c), and the launcher's own record.
    library.with_name(library.name + '.id').write_text(
        (resources / 'gamelib-id.txt').read_text(encoding='utf-8').strip() + '\n', encoding='utf-8')
    return library


def main(argv=None):
    parser = argparse.ArgumentParser(description='Build the game code library from a local game dump.')
    parser.add_argument('--root', required=True, help='folder holding exefs/ and romfs/')
    parser.add_argument('--out', help='build folder (default: <data root>/gamebuild)')
    parser.add_argument('--resources', help='shipped gamebuild resources')
    parser.add_argument('--jobs', type=int, help='parallel compilations (default: up to 8)')
    parser.add_argument('--force', action='store_true', help='rebuild even when nothing changed')
    parser.add_argument('--dry-run', action='store_true', help='print the steps instead of building')
    args = parser.parse_args(argv)

    resources = resources_dir(args.resources)
    link = json.loads((resources / 'link.json').read_text(encoding='utf-8'))
    dump = Path(args.root).expanduser().resolve()
    out = Path(args.out).expanduser().resolve() if args.out else default_data_root() / 'gamebuild'
    jobs = args.jobs or min(8, os.cpu_count() or 4)
    library = out / os.path.basename(link['library'])
    current = fingerprint(dump, resources, link)
    stamp = out / 'build.json'
    if not args.force and stamp.is_file() and library.is_file():
        try:
            if json.loads(stamp.read_text(encoding='utf-8')).get('fingerprint') == current:
                print(f'gamebuild: up to date: {library}')
                report(out, 'link', 1, 1, 'Ready')
                return 0
        except (OSError, ValueError):
            pass
    validate(dump, resources)
    if args.dry_run:
        print(f'gamebuild: would build {library} from {dump} with {resources / link["compiler"]}')
        return 0
    started = time.time()
    out.mkdir(parents=True, exist_ok=True)
    for name in ('progress.txt', 'gamebuild.log'):
        (out / name).unlink(missing_ok=True)
    with (out / 'gamebuild.log').open('w', encoding='utf-8', errors='replace') as log:
        log.write(f'gamebuild: {dump} -> {out}\n')
        log.flush()
        describe_compiler(resources, out, link, log)
        note(log, 'generate: start')
        modules = generate_modules(dump, resources, out, jobs, log)
        note(log, f'generate: done; compile: start ({jobs} at a time)')
        units = compile_units(resources, out, link, jobs, log)
        note(log, 'compile: done; link: start')
        report(out, 'link', 0, 1, 'Linking')
        link_library(resources, out, link, units, log)
        note(log, 'link: done')
        stamp.write_text(json.dumps({
            'fingerprint': current,
            'gamelib_id': (resources / 'gamelib-id.txt').read_text(encoding='utf-8').strip(),
            'library': str(library),
            'units': {key: signature for key, signature, _ in units},
            'modules': [m['name'] for m in modules],
        }, indent=2) + '\n', encoding='utf-8')
        # The generated C is large and reproducible from the dump; the objects keep
        # rebuilds cheap, so only the sources are dropped.
        shutil.rmtree(out / 'generated', ignore_errors=True)
    report(out, 'link', 1, 1, 'Ready')
    print(f'gamebuild: {library} ({time.time() - started:.0f}s)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
