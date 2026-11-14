#!/usr/bin/env python3
"""Prepare FSR 4.1.2 Lite's model files on this PC (the launcher's "Prepare FSR 4 Lite").

FSR 4 Lite's native component (third_party/fsr4_native) replays AMD's Lite INT8 network. Its model files are recorded
from AMD's own upscaler DLL, so they are made here, on the player's PC, and never shipped (the way bbport does it,
third_party/fsr4cap):

1. download amd_fidelityfx_upscaler_dx12.dll and the SDK licence from AMD's FidelityFX SDK 2.4.0 on GitHub (pinned
   commit, SHA-256 checked) - or use --dll;
2. driver_model.py force ... lite: a copy that offers only the Lite INT8 model;
3. record its passes and translate them: build_assets.sh under the player's Proton (Linux), build_assets_win.py on
   vkd3d-proton (Windows), with the prebuilt tools in --tools/bin;
4. check the result (four tiers, 12 shaders and 27,392-byte weights each) and install it as <out>/lite, with
   provenance.json and AMD's licence.

    monado-fsr4prep --out <data>/fsr4 [--work <data>/fsr4-work] [--tools <package>/fsr4prep] [--dll <file>]
Progress for the launcher: <out>/../fsr4-progress.txt ("<phase> <done> <total>"). Exit status: 0 ok, 2 download or
DLL problem, 3 no Proton (Linux), 4 tools missing, 5 FSR 4 did not run, 6 the result is incomplete.
--run-script <script> [args]: run one of the tooling's Python scripts with this interpreter (the frozen tool has no
separate Python).
"""
import argparse
import hashlib
import json
import os
import re
import runpy
import shutil
import ssl
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

SDK_COMMIT = '7539ae8b6568ad29ed78a753404351daf3b5cca7'  # FidelityFX SDK 2.4.0
SDK_RAW = f'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/{SDK_COMMIT}/'
DLL = ('Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll',
       'aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace')  # upscaler 4.1.2.4555
LICENSE = 'Kits/FidelityFX/docs/license.md'
TOOLS_COMMIT = '0fc2921c45f8ff3900f6bfa43a299e9a4f532af4'  # bbport tools/fsr4cap
TIERS = ('t1080_m0', 't1080_m1', 't2160_m0', 't2160_m1')
FROZEN = getattr(sys, 'frozen', False)


def say(text):
    print(text, flush=True)


def default_tools():
    here = Path(sys.executable if FROZEN else __file__).resolve().parent
    for candidate in (os.environ.get('MONADO_FSR4PREP_DIR'), here, here.parent / 'third_party' / 'fsr4cap'):
        if candidate and (Path(candidate) / 'extract.py').is_file():
            return Path(candidate)
    raise SystemExit('fsr4prep: the capture tooling (extract.py) was not found; pass --tools')


class Progress:
    def __init__(self, path):
        self.path = path

    def __call__(self, phase, done, total):
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.path.with_suffix('.tmp')
            tmp.write_text(f'{phase} {done} {total}\n', encoding='utf-8')
            os.replace(tmp, self.path)
        except OSError:
            pass


def ssl_context():
    """A TLS context that also works where the frozen build's OpenSSL finds no trust store.

    The packaged tool carries the build runner's OpenSSL (Ubuntu), compiled with
    OPENSSLDIR=/usr/lib/ssl. Arch and SteamOS keep their bundle in /etc/ssl and Fedora in
    /etc/pki/tls, so create_default_context() can end up with an empty store there and every
    download fails with 'certificate verify failed: unable to get local issuer certificate'.
    """
    context = ssl.create_default_context()
    if context.cert_store_stats().get('x509_ca', 0):
        return context
    for bundle in ('/etc/ssl/certs/ca-certificates.crt', '/etc/pki/tls/certs/ca-bundle.crt',
                   '/etc/ssl/cert.pem', '/etc/ssl/certs/ca-bundle.crt', '/etc/ssl/ca-bundle.pem'):
        if os.path.isfile(bundle):
            context.load_verify_locations(bundle)
            break
    return context


def download(rel, dest, sha256=None):
    request = urllib.request.Request(SDK_RAW + rel, headers={'User-Agent': 'Monado-FSR4-Prepare'})
    with urllib.request.urlopen(request, timeout=120, context=ssl_context()) as response, open(dest, 'wb') as out:
        shutil.copyfileobj(response, out, 1 << 20)
    if sha256:
        digest = hashlib.sha256(Path(dest).read_bytes()).hexdigest()
        if digest != sha256:
            Path(dest).unlink(missing_ok=True)
            raise SystemExit(f'fsr4prep: {rel}: SHA-256 {digest}, expected {sha256}')


def python_command():
    """How the tooling's scripts run: this interpreter (frozen: this executable with --run-script)."""
    return [sys.executable, '--run-script'] if FROZEN else [sys.executable]


def run_script(tools, name, *args, env=None):
    return subprocess.run([*python_command(), str(tools / name), *map(str, args)], env=env).returncode


def clean_env(tools_bin):
    """The tools first on PATH; an AppImage's library path must not reach Proton or the tools."""
    env = dict(os.environ)
    for name in ('LD_LIBRARY_PATH', 'PYTHONHOME', 'PYTHONPATH'):
        env.pop(name, None)
    env['PATH'] = str(tools_bin) + os.pathsep + env.get('PATH', '')
    return env


def record_linux(tools, forced, work, assets, progress):
    tools_bin = tools / 'bin'
    for name in ('fsr4cap.exe', 'dxil-spirv', 'spirv-as', 'spirv-dis'):
        if not (tools_bin / name).is_file():
            say(f'fsr4prep: {tools_bin / name} is missing (the package brings it)')
            return 4
    # build_assets.sh runs "$PYTHON script.py": one command, so a small wrapper for the frozen interpreter
    wrapper = work / 'python'
    wrapper.write_text('#!/bin/sh\nexec ' + ' '.join(f'"{c}"' for c in python_command()) + ' "$@"\n')
    wrapper.chmod(0o755)
    env = clean_env(tools_bin)
    env.update(BB_FSR4CAP_TOOLS=str(tools_bin), PYTHON=str(wrapper), BB_FSR4CAP_WORK=str(work / 'capture'),
               BB_FSR411_OUT=str(assets), FSR4CAP_VERSION='4.1.2', FSR4CAP_MODEL='lite', BB_FSR4CAP_FP8='0',
               BB_FSR4CAP_RUNNER=os.environ.get('BB_FSR4CAP_RUNNER', 'direct'))
    proc = subprocess.Popen(['bash', str(tools / 'build_assets.sh'), str(forced)], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    for line in proc.stdout:
        say(line.rstrip())
        m = re.match(r'\s*\[\s*(\d+)/(\d+)\]', line)
        if m:
            progress('record', int(m[1]), int(m[2]))
        elif line.startswith('Translating'):
            progress('translate', 0, 1)
    return proc.wait()


def record_windows(tools, forced, work, assets, progress):
    tools_bin = tools / 'bin'
    env = clean_env(tools_bin)
    env.update(BB_FSR4CAP_TOOLS=str(tools_bin), BB_FSR4CAP_WORK=str(work / 'capture'), BB_FSR411_OUT=str(assets),
               FSR4CAP_VERSION='4.1.2', FSR4CAP_MODEL='lite', BB_FSR4CAP_FP8='0')
    proc = subprocess.Popen([*python_command(), str(tools / 'build_assets_win.py'), str(forced)], env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    for line in proc.stdout:
        say(line.rstrip())
        m = re.match(r'\s*\[\s*(\d+)/(\d+)\]', line)
        if m:
            progress('record', int(m[1]), int(m[2]))
        elif line.startswith('Translating'):
            progress('translate', 0, 1)
    return proc.wait()


def check(assets):
    for tier in TIERS:
        d = assets / 'lite' / tier
        init = d / 'initializer.bin'
        if not init.is_file() or init.stat().st_size != 27392 or len(list(d.glob('*.spv'))) != 12:
            return f'incomplete Lite model: {d}'
    return None


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ['--run-script']:  # (the frozen tool as the tooling's Python interpreter)
        script = Path(argv[1]).resolve()
        sys.argv = [str(script), *argv[2:]]
        sys.path.insert(0, str(script.parent))
        runpy.run_path(str(script), run_name='__main__')
        return 0
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, required=True, help='where lite/ goes (the runtime reads <data>/fsr4)')
    ap.add_argument('--work', type=Path, help='work folder (default: <out>-work; Proton prefix and captures)')
    ap.add_argument('--tools', type=Path, help='the capture tooling (default: beside this program)')
    ap.add_argument('--dll', type=Path, help="AMD's amd_fidelityfx_upscaler_dx12.dll 4.1.2 instead of downloading it")
    ap.add_argument('--keep-work', action='store_true', help='keep the work folder (by default removed on success)')
    a = ap.parse_args(argv)
    tools = (a.tools or default_tools()).resolve()
    out = a.out.resolve()
    work = (a.work or out.with_name(out.name + '-work')).resolve()
    progress = Progress(out.parent / 'fsr4-progress.txt')
    work.mkdir(parents=True, exist_ok=True)
    progress('download', 0, 1)
    dll = work / 'amd_fidelityfx_upscaler_dx12.dll'
    licence = work / 'LICENSE-AMD-SDK.md'
    try:
        if a.dll:
            shutil.copyfile(a.dll, dll)
        elif not (dll.is_file() and hashlib.sha256(dll.read_bytes()).hexdigest() == DLL[1]):
            say("Downloading AMD's FSR 4 upscaler DLL (FidelityFX SDK 2.4.0, 58 MB) from AMD's GitHub...")
            download(DLL[0], dll, DLL[1])
        download(LICENSE, licence)
    except OSError as error:
        say(f'fsr4prep: download failed: {error}')
        return 2
    progress('download', 1, 1)
    forced = work / 'lite' / 'amd_fidelityfx_upscaler_dx12.dll'
    forced.parent.mkdir(exist_ok=True)
    if run_script(tools, 'driver_model.py', 'force', dll, forced, 'lite') != 0:
        say('fsr4prep: this DLL does not hold the FSR 4.1.2 Lite model')
        return 2
    with tempfile.TemporaryDirectory(prefix='.fsr4-stage-', dir=out.parent) as staging:
        assets = Path(staging) / 'assets'
        record = record_windows if os.name == 'nt' else record_linux
        status = record(tools, forced, work, assets, progress)
        if status:
            say(f'fsr4prep: recording failed (status {status}); log above')
            return status
        problem = check(assets)
        if problem:
            say(f'fsr4prep: {problem}')
            return 6
        provenance = {'sdk': {'version': '2.4.0', 'commit': SDK_COMMIT, 'upscaler': '4.1.2.4555',
                              'dll_sha256': hashlib.sha256(dll.read_bytes()).hexdigest()},
                      'model': 'ffxProvider_FSR4Lite_Int8', 'native_tools_commit': TOOLS_COMMIT,
                      'prepared_on': 'this PC (tools/fsr4_prepare.py)',
                      'files': {p.relative_to(assets).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in sorted((assets / 'lite').rglob('*')) if p.is_file()}}
        (assets / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf-8')
        shutil.copyfile(licence, assets / 'LICENSE-AMD-SDK.md')
        # replace the previous set only now that the new one is complete
        old = Path(staging) / 'old'
        if out.exists():
            out.rename(old)
        out.parent.mkdir(parents=True, exist_ok=True)
        assets.rename(out)
    if not a.keep_work:
        shutil.rmtree(work, ignore_errors=True)
    progress('done', 1, 1)
    say(f'FSR 4 Lite model files ready: {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
