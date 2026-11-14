"""[XCDE] T-LINUX-APPIMAGE: package this checkout's Linux build as one AppImage (never the dump).

The package holds the recompiled game code of THIS build (generated from your own dump), so it is for your own use:
do not redistribute it (CONTRIBUTING.md, art. I). It contains build/linux/monado, the launcher, the optional native
FSR 3 / FSR 4 Lite components when they were built (with their sources and licences), the shader variant seed, and
the non-system shared libraries they need. GPU drivers, the Vulkan loader and glibc always come from the host.

  python3 tools/make_appimage.py [--output build/Monado-x86_64.AppImage] [--appimagetool appimagetool]

--release packages a runtime built with CMake -DMONADO_RELEASE=ON instead: it holds NO game code and can be shared.
The launcher builds the game code once from the player's own dump with the packaged builder (build/gamebuild:
tools/gamebuild_stage.py + tools/freeze_gamebuild.py). The packaged tree is checked with tools/package_guard.py.
Needs appimagetool (https://github.com/AppImage/appimagetool) on PATH or given with --appimagetool.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# Host ABI / graphics driver components must not be replaced by packaged copies. That includes what the GPU drivers
# themselves link against: AppRun puts usr/lib first on LD_LIBRARY_PATH, so a bundled copy older than the host's (the
# CI builds on Ubuntu 24.04) is what Mesa gets too. RADV then failed to load (libxcb-dri3: undefined symbol
# xcb_dri3_import_syncobj_checked) and the launcher found no GPU on SteamOS and other current distributions.
HOST = re.compile(r"^(ld-linux|lib(c|m|pthread|dl|rt|resolv|util|anl)\.so|libnss_|libvulkan\.so|"
                  r"lib(GL|EGL|GLX|OpenGL|GLdispatch|drm|gbm)[_.]|"
                  r"lib(stdc\+\+|gcc_s|z|expat|ffi|lzma|zstd|elf|udev|xshmfence)\.so|"
                  r"lib(X11|X11-xcb|Xau|Xdmcp|Xext|Xfixes|Xrender|Xxf86vm|xcb|xcb-[a-z0-9]+|wayland-[a-z]+)\.so)")


def dependencies(binary):
    result = subprocess.run(['ldd', str(binary)], text=True, capture_output=True, check=True)
    if 'not found' in result.stdout:
        raise RuntimeError('Unresolved shared libraries:\n' + result.stdout)
    paths = []
    for line in result.stdout.splitlines():
        match = re.search(r'=> (/\S+)', line) or re.match(r'\s*(/\S+)', line)
        if match and not HOST.match(Path(match[1]).name):
            paths.append(Path(match[1]))
    return paths


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', type=Path, default=ROOT / 'build/linux', help='scripts/build_linux.sh output')
    ap.add_argument('--fsr4-assets', type=Path, default=ROOT / 'build/fsr4/assets', help='prepared FSR 4 Lite assets')
    ap.add_argument('--output', type=Path, default=ROOT / 'build/Monado-x86_64.AppImage')
    ap.add_argument('--appimagetool', default='appimagetool')
    ap.add_argument('--release', action='store_true', help='runtime without game code + the first-start builder')
    ap.add_argument('--gamebuild', type=Path, default=ROOT / 'build/gamebuild', help='staged builder (--release)')
    ap.add_argument('--fsr4prep', type=Path, default=ROOT / 'build/fsr4prep',
                    help='FSR 4 Lite preparation tool (tools/build_fsr4prep.py): ships FSR 4 without AMD-derived files')
    a = ap.parse_args()
    build, output = a.build.resolve(), a.output.resolve()
    for name in ('monado', 'monado-launcher'):
        if not (build / name).is_file():
            ap.error(f'missing {build / name}: run scripts/build_linux.sh first (docs/LINUX.md)')
    if shutil.which(a.appimagetool) is None and not Path(a.appimagetool).is_file():
        ap.error(f'{a.appimagetool} not found (https://github.com/AppImage/appimagetool)')
    if a.release:
        gamebuild = a.gamebuild.resolve()
        for name in ('link.json', 'gamelib-id.txt', 'monado-gamebuild'):
            if not (gamebuild / name).is_file():
                ap.error(f'missing {gamebuild / name}: run tools/gamebuild_stage.py and tools/freeze_gamebuild.py')
        # The release runtime must not contain lifted game functions (f_<guest address>).
        symbols = subprocess.run(['nm', '--defined-only', str(build / 'monado')], text=True, capture_output=True).stdout
        if re.search(r' f_7[0-9a-f]{9}\b', symbols):
            ap.error(f'{build / "monado"} contains game code: build it with CMake -DMONADO_RELEASE=ON')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='monado-appdir-', dir=output.parent) as temp:
        appdir = Path(temp)  # (fresh: no stale files, caches or dumps can enter the package)
        bin_, lib, share = appdir / 'usr/bin', appdir / 'usr/lib', appdir / 'usr/share/monado'
        docs = appdir / 'usr/share/doc/monado'
        for d in (bin_, lib, share, docs):
            d.mkdir(parents=True)
        payloads = []
        for name in ('monado', 'monado-launcher'):
            shutil.copy2(build / name, bin_ / name)
            payloads.append(build / name)
        fsr3, fsr4 = build / 'libmonado_fsr3.so', build / 'libmonado_fsr4.so'
        if fsr3.is_file():
            shutil.copy2(fsr3, lib / fsr3.name)
            payloads.append(fsr3)
            shutil.copytree(ROOT / 'third_party/fsr3_native', docs / 'fsr3-source')
            shutil.copy2(ROOT / 'tools/build_fsr3_linux.py', docs / 'fsr3-source/build_fsr3_linux.py')
        else:
            print('note: no native FSR 3 component (scripts/build_fsr3_linux.sh): packaged without it')
        assets = a.fsr4_assets.resolve()
        if fsr4.is_file() and (assets / 'provenance.json').is_file():
            manifest = json.loads((assets / 'provenance.json').read_text())
            if manifest.get('model') != 'ffxProvider_FSR4Lite_Int8' or manifest.get('sdk', {}).get('version') != '2.4.0':
                raise RuntimeError('expected verified SDK 2.4.0 Lite INT8 assets')
            for name, digest in manifest['files'].items():  # (only verified files, never strays beside them)
                source = (assets / name).resolve()
                if not source.is_relative_to(assets) or hashlib.sha256(source.read_bytes()).hexdigest() != digest:
                    raise RuntimeError(f'FSR 4 asset hash mismatch: {name}')
                target = share / 'fsr4' / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
            for name in ('provenance.json', 'LICENSE-AMD-SDK.md'):
                shutil.copy2(assets / name, share / 'fsr4' / name)
            shutil.copy2(fsr4, lib / fsr4.name)
            payloads.append(fsr4)
            # the GPL component's corresponding source, licence and build instructions
            shutil.copytree(ROOT / 'third_party/fsr4_native', docs / 'fsr4-source')
            shutil.copy2(ROOT / 'runtime/nvn/vulkan/fsr4_api.h', docs / 'fsr4-source/fsr4_api.h')
            shutil.copy2(ROOT / 'scripts/build_fsr4_linux.sh', docs / 'fsr4-source/build_fsr4_linux.sh')
            shutil.copy2(ROOT / 'docs/adr/0012-native-fsr4-lite.md', docs / 'FSR4-LICENSING.md')
        elif fsr4.is_file() and (a.fsr4prep / 'monado-fsr4prep').is_file():
            # no model files: the launcher prepares them on the player's PC (AppRun exports XCDE_FSR4PREP_DIR)
            shutil.copytree(a.fsr4prep.resolve(), share / 'fsr4prep', symlinks=False)
            for exe in [share / 'fsr4prep/monado-fsr4prep', *(share / 'fsr4prep/bin').iterdir()]:
                exe.chmod(0o755)
            shutil.copy2(fsr4, lib / fsr4.name)
            payloads.append(fsr4)
            shutil.copytree(ROOT / 'third_party/fsr4_native', docs / 'fsr4-source')
            shutil.copy2(ROOT / 'runtime/nvn/vulkan/fsr4_api.h', docs / 'fsr4-source/fsr4_api.h')
            shutil.copy2(ROOT / 'scripts/build_fsr4_linux.sh', docs / 'fsr4-source/build_fsr4_linux.sh')
        else:
            print('note: no FSR 4 Lite component or assets (scripts/build_fsr4_linux.sh, tools/prepare_fsr4_lite.py): '
                  'packaged without it')
        for dep in {d for p in payloads for d in dependencies(p)}:
            target = lib / dep.name
            if target.exists() and target.read_bytes() != dep.read_bytes():
                raise RuntimeError(f'library name collision: {dep.name}')
            shutil.copy2(dep, target)
        for name in ('AppRun', 'monado.desktop', 'monado.svg'):
            shutil.copy2(ROOT / 'packaging/linux' / name, appdir / name)
        shutil.copy2(ROOT / 'data/variants_seed.bin', share / 'variants_seed.bin')
        if a.release:  # the first-start builder and its resources (AppRun exports XCDE_GAMEBUILD_DIR)
            shutil.copytree(a.gamebuild.resolve(), share / 'gamebuild', symlinks=False,
                            ignore=shutil.ignore_patterns('obj', 'generated', 'progress.txt', '*.log'))
            (share / 'gamebuild/monado-gamebuild').chmod(0o755)
            guard = subprocess.run(['python3', str(ROOT / 'tools/package_guard.py'), str(appdir)], text=True)
            if guard.returncode != 0:
                raise SystemExit('package_guard: the AppImage tree holds game or derived files (see above)')
        for name in ('LICENSE', 'NOTICE.md'):
            shutil.copy2(ROOT / name, docs / name)
        for source in (ROOT / 'third_party').rglob('*'):  # (bundled source dependencies' licence notices)
            if source.is_file() and source.name.lower().startswith(('license', 'copying', 'copyright')):
                target = docs / source.relative_to(ROOT)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
        # Never truncate a running AppImage: build beside it, then replace it in one step.
        with tempfile.TemporaryDirectory(prefix='monado-output-', dir=output.parent) as dest:
            staged = Path(dest) / output.name
            subprocess.run([a.appimagetool, str(appdir), str(staged)], env=dict(os.environ, ARCH='x86_64'), check=True)
            os.replace(staged, output)
    print(f'Created {output} ' + ('(release: no game code; the launcher builds it from the player\'s dump)' if a.release
                                  else '(contains the game code recompiled from your dump: for your own use only)'))


if __name__ == '__main__':
    main()
