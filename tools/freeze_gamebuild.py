"""Freeze the first-start builder into an executable and its _internal/ folder for release packages.

PyInstaller is only needed on the build machine (pip install pyinstaller); players
run the frozen binary. The result is `build/gamebuild/monado-gamebuild[.exe]`, which
the packagers place beside the resources staged by tools/gamebuild_stage.py.

    python3 tools/freeze_gamebuild.py [--python .venv/bin/python]
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NAME = 'monado-gamebuild'
HIDDEN = ('gen', 'analyze', 'lifter', 'nso', 'patchfmt', 'simd_c', 'sym', 'check_dump', 'manifest')
COLLECT = ('unicorn', 'capstone', 'lz4')
IMAGE_DLLCHARACTERISTICS_GUARD_CF = 0x4000


def clear_guard_cf(binary):
    """Turn off Control Flow Guard in a Windows PE header; return whether it was set.

    PyInstaller's Windows bootloader is built with /guard:cf. In a CFG process the CRT
    validates longjmp targets, and unicorn's longjmp out of guest-fault handling is not a
    registered target, so the frozen builder dies with 0xC0000409 (fast fail) in
    VCRUNTIME140 while lifting. python.exe has no CFG, which is why the same code works
    unfrozen. Only the header flag changes; the appended PyInstaller archive is untouched.
    """
    with open(binary, 'r+b') as f:
        head = f.read(4096)
        if head[:2] != b'MZ':
            raise SystemExit(f'freeze: {binary} is not a PE image')
        pe = int.from_bytes(head[0x3c:0x40], 'little')
        if head[pe:pe + 4] != b'PE\0\0':
            raise SystemExit(f'freeze: {binary} has no PE header')
        offset = pe + 24 + 70  # DllCharacteristics: same offset in PE32 and PE32+
        flags = int.from_bytes(head[offset:offset + 2], 'little')
        if not flags & IMAGE_DLLCHARACTERISTICS_GUARD_CF:
            return False
        f.seek(offset)
        f.write((flags & ~IMAGE_DLLCHARACTERISTICS_GUARD_CF).to_bytes(2, 'little'))
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--python', default=sys.executable, help='interpreter with PyInstaller, capstone, unicorn and lz4')
    parser.add_argument('--output', default=str(ROOT / 'build/gamebuild'))
    args = parser.parse_args(argv)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    work = ROOT / 'build/pyinstaller'
    dist = work / 'dist'
    command = [args.python, '-m', 'PyInstaller', '--noconfirm', '--clean', '--onedir', '--name', NAME,
               '--distpath', str(dist), '--workpath', str(work), '--specpath', str(work),
               '--paths', str(ROOT / 'tools/lifter'), '--paths', str(ROOT / 'tools/extract')]
    for module in HIDDEN:
        command += ['--hidden-import', module]
    for package in COLLECT:  # native wheels (capstone, unicorn) load their own libraries
        command += ['--collect-all', package]
    command.append(str(ROOT / 'tools/gamebuild_entry.py'))
    try:
        subprocess.run(command, check=True)
    except FileNotFoundError:
        raise SystemExit('freeze: PyInstaller is not installed for this interpreter (pip install pyinstaller)')
    except subprocess.CalledProcessError as error:
        raise SystemExit(f'freeze: PyInstaller failed ({error.returncode})')
    bundle = dist / NAME
    shutil.rmtree(output / '_internal', ignore_errors=True)
    for item in bundle.iterdir():
        if item.is_dir():
            shutil.copytree(item, output / item.name)
        else:
            shutil.copy2(item, output / item.name)
    binary = output / (NAME + ('.exe' if os.name == 'nt' else ''))
    if not binary.is_file():
        raise SystemExit(f'freeze: PyInstaller did not produce {binary}')
    if binary.suffix == '.exe' and clear_guard_cf(binary):
        print('freeze: cleared Control Flow Guard in the bootloader (unicorn longjmp)')
    binary.chmod(0o755)
    print(f'frozen builder: {binary}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
