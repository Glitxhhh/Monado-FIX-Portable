"""Stage the first-start game code builder inside a release package.

A release package has no recompiled game code. It carries, under `gamebuild/`, the
generator's inputs, the guest headers, a compiler and the instructions the builder
(tools/gamebuild.py) needs; the player's own dump supplies the actual game code.

Both packagers call `stage()`. Run it directly to inspect the folder:

    python3 tools/gamebuild_stage.py --dest build/gamebuild --platform linux
"""
import argparse
import json
import os
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import game_id  # noqa: E402

HEADERS = ('cpu.h', 'simd.h', 'simd_fp.h', 'gamelib.h')
GAMES = ('profile.json', 'patches.py', 'hle_symbols.txt')
# Everything the zig frontend needs to compile and link C for one platform. The rest of
# the bundled toolchain (other targets, C++ standard library, debugger) is dropped:
# `zig cc` is all the guest code ever uses.
ZIG_DROP = ('docs', 'compiler', 'build-web', 'fuzzer.zig', 'lldb', 'libcxx', 'libcxxabi', 'libtsan')
LIBC_DROP = ('wasi', 'darwin', 'freebsd', 'netbsd', 'openbsd')
# zig 0.17 keeps x86-64's glibc headers (bits/wordsize.h ...) in x86-linux-gnu / x86-linux-any; there is no
# x86_64-linux-gnu folder any more. Without them every guest file failed to compile on systems without their own
# development headers (SteamOS): a native zig target falls back to /usr/include, which hid the problem elsewhere.
LIBC_HEADERS = {'linux': ('any-linux-any', 'generic-glibc', 'x86-linux-any', 'x86-linux-gnu', 'x86_64-linux-gnu'),
                'windows': ('any-windows-any',)}
# An explicit target: the bundled headers and libraries, never the player's system ones (and the same objects on
# every PC; the CPU level comes from GUEST_FLAGS' -march).
TARGET = {'linux': 'x86_64-linux-gnu', 'windows': 'x86_64-windows-gnu'}


def find_zig():
    """The zig binary of the ziglang package (or XCDE_ZIG / PATH)."""
    explicit = os.environ.get('XCDE_ZIG')
    if explicit and Path(explicit).is_file():
        return Path(explicit)
    candidates = []
    try:  # the interpreter running this script is the reliable source (any venv)
        import ziglang
        package = Path(ziglang.__file__).resolve().parent
        candidates += [package / 'zig', package / 'zig.exe']
    except ImportError:
        pass
    for venv in (ROOT / '.venv', ROOT / 'venv'):
        candidates += list(venv.glob('lib/python*/site-packages/ziglang/zig'))
        candidates += list(venv.glob('Lib/site-packages/ziglang/zig.exe'))
        candidates += list(venv.glob('Scripts/../Lib/site-packages/ziglang/zig.exe'))
    for name in ('zig.exe', 'zig'):
        found = shutil.which(name)
        if found:
            candidates.append(Path(found))
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit('gamebuild: zig not found. Install it (pip install ziglang) or set XCDE_ZIG=<path>.')


def stage(dest, platform, zig=None, link_files=()):
    """Write the staged resources; returns the folder. `zig` defaults to the ziglang package."""
    dest = Path(dest)
    if dest.exists():
        shutil.rmtree(dest)
    (dest / 'entry').mkdir(parents=True)
    (dest / 'include').mkdir()
    (dest / 'games').mkdir()
    for name in HEADERS:
        shutil.copy2(ROOT / 'runtime' / name, dest / 'include' / name)
    for name in GAMES:
        shutil.copy2(ROOT / 'games/xcde' / name, dest / 'games' / name)
    shutil.copy2(ROOT / 'tools/gamelib_entry.c', dest / 'entry' / 'gamelib_entry.c')
    compiler = Path(zig) if zig else find_zig()
    # The zig binary finds its lib/ folder beside itself, so bundle both and trim.
    toolchain = dest / 'compiler'
    toolchain.mkdir()
    compiler_name = 'zig.exe' if platform == 'windows' else 'zig'
    shutil.copy2(compiler, toolchain / compiler_name)
    (toolchain / compiler_name).chmod(0o755)
    library = compiler.resolve().parent / 'lib'
    if not library.is_dir():
        raise SystemExit(f'gamebuild: {compiler} has no lib/ folder beside it; bundle the ziglang package')
    shutil.copytree(library, toolchain / 'lib')
    for name in ZIG_DROP:
        shutil.rmtree(toolchain / 'lib' / name, ignore_errors=True)
        (toolchain / 'lib' / name).unlink(missing_ok=True)
    libc = toolchain / 'lib/libc'
    for name in LIBC_DROP + ('musl',):
        shutil.rmtree(libc / name, ignore_errors=True)
    if platform == 'windows':  # natives use UCRT through mingw-w64; glibc is not involved
        shutil.rmtree(libc / 'glibc', ignore_errors=True)
    keep = LIBC_HEADERS[platform]
    for entry in (libc / 'include').iterdir():
        if entry.name not in keep:
            shutil.rmtree(entry, ignore_errors=True) if entry.is_dir() else entry.unlink()
    (dest / 'gamelib-id.txt').write_text(game_id.fingerprint() + '\n', encoding='utf-8')
    link = {
        'compiler': f'compiler/{compiler_name}',
        'lib_dir': 'compiler/lib',
        # Guest translation units are plain C; the library only needs libc and the
        # runtime's exported symbols (resolved from the process that loads it).
        'compile': ['cc', '-target', TARGET[platform]] + ([] if platform == 'windows' else ['-fPIC']),
        'guest_flags': list(game_id.GUEST_FLAGS),
        'include': ['include'],
        'link': ['cc', '-target', TARGET[platform], '-shared'] + ([] if platform == 'windows' else ['-fPIC']),
        'library': 'lasgame.dll' if platform == 'windows' else 'lasgame.so',
        # e.g. the import library of the executable on Windows. It is produced while the
        # executable is linked, after this staging step, so only the packager checks it.
        'link_files': list(link_files),
    }
    (dest / 'link.json').write_text(json.dumps(link, indent=2) + '\n', encoding='utf-8')
    return dest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dest', default=str(ROOT / 'build/gamebuild'))
    parser.add_argument('--platform', choices=('linux', 'windows'), default='linux')
    parser.add_argument('--zig', help='zig binary to bundle (default: the ziglang package)')
    parser.add_argument('--link-file', action='append', default=[],
                        help='extra link input produced later (Windows: xcde_imports.a)')
    args = parser.parse_args(argv)
    dest = stage(args.dest, args.platform, args.zig, args.link_file)
    print(f'gamebuild resources: {dest} (GAMELIB_ID {game_id.fingerprint()})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
