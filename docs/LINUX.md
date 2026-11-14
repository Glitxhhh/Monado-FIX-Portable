# Native Linux build

The same runtime and recompiled game code as the Windows build, compiled natively for Linux x86-64
(no Wine). Windows and Linux objects never mix: everything Linux goes to `build/linux/`.

## Requirements

- x86-64-v2 CPU (SSE4.2, POPCNT, CMPXCHG16B), a Vulkan 1.2+ driver (Mesa RADV/ANV, NVIDIA proprietary).
- `clang`, `pkg-config` and the development packages of SDL3, Vulkan (headers + loader), shaderc, opus,
  FFmpeg (libavcodec, libavformat, libavutil, libswscale, libswresample) and zlib.
- The Vulkan Memory Allocator header `vk_mem_alloc.h` (system package, or `VMA_INCLUDE_DIR=<dir>`).
- Python 3 with `capstone unicorn lz4` for the recompiler:
  `python3 -m venv .venv && .venv/bin/pip install capstone unicorn lz4`.
- Your own dump in `targets/` (`exefs/`, `romfs/`), the same as for the Windows build (version 2.0.0).

Arch / CachyOS: `pacman -S clang sdl3 vulkan-headers vulkan-icd-loader shaderc opus ffmpeg zlib vulkan-memory-allocator`.

## Build and play

```bash
scripts/build_linux.sh --check        # dependencies only
scripts/build_linux.sh                # targets/ -> generated/ -> build/linux/monado
build/linux/monado-launcher           # the launcher: settings, shader precompilation, PLAY
build/linux/monado targets            # or the game directly, from the repository root (xcde.ini, cache/, saves)
```

The launcher alone: `tools/launcher/build_linux.sh`. Its reports are zipped with `bsdtar` (libarchive);
"Open folder" uses the desktop's file manager (`xdg-open`).

`scripts/build_linux.sh --runtime-only` compiles the runtime without game files (CI / quick checks).
`JOBS=<n>` limits parallel game-code compilation; unchanged chunks are not recompiled.

## Differences from Windows

- DLSS (NVIDIA Streamline) is Windows-only. FSR 3.1 and FSR 4 Lite are native Linux components (below).
  Everything else (FSR 1, SMAA, the F1 menu, mods, controls) is the same code.
- Logs go to the terminal or wherever stderr is redirected (the launcher: `build/last_game.log`); started from a
  desktop without either, to `~/.local/share/monado/lasrecomp.log`.
- The watchdog (`XCDE_STALL`, `XCDE_WATCHDOG`) samples one PC per guest thread instead of a full host stack.

## Native temporal upscalers (optional)

`scripts/build_linux.sh` also builds two optional components next to the game; if one cannot be built, that
upscaler is just unavailable (`XCDE_NATIVE_FSR=0` skips both).

- **AMD FSR 3.1** (`build/linux/libmonado_fsr3.so`, `scripts/build_fsr3_linux.sh`): AMD's FSR 3.1.4 upscaler and
  Vulkan backend from the official SDK 1.1.4 (fetched at build time, pinned commit; needs `git` and
  `glslangValidator`). `third_party/fsr3_native/compat.h` supplies the float `abs` overloads the SDK expects:
  without them its Lanczos table was half 1.0 and stationary detail blurred (`tests/native/test_fsr3_temporal.cpp`).
- **AMD FSR 4 Lite INT8** (`build/linux/libmonado_fsr4.so`, `scripts/build_fsr4_linux.sh`): a native Vulkan
  implementation of FSR 4.1.2's Lite graph (GPL-2.0-or-later, `third_party/fsr4_native/`, ADR 0012). It needs the
  Lite model assets, prepared once with `tools/prepare_fsr4_lite.py` from AMD's public SDK 2.4.0 (into
  `build/fsr4/assets`, linked as `build/linux/fsr4`), or `MONADO_FSR4_ASSET_DIR`. GPU: push descriptors, compute
  derivatives, VALVE mixed float dot, int8/int16/fp16 and integer dot product (Vulkan 1.3); 4K output at most.

Both take linear colour (the prep pass decodes sRGB and the result is encoded again before the HUD) and are fed
the Halton jitter (`XCDE_TMP_NOJITTER=1` turns it off). `MONADO_FSR3_LIBRARY` / `MONADO_FSR4_LIBRARY` override the
component paths.

## AppImage (for your own machine)

```bash
scripts/build_appimage_linux.sh       # build_linux.sh + tools/make_appimage.py -> build/Monado-x86_64.AppImage
./Monado-x86_64.AppImage /path/to/dump    # launcher; the dump is remembered
./Monado-x86_64.AppImage --play           # the game directly
```

The AppImage contains the game code recompiled from **your** dump, so it is for your own use: do not share it
(CONTRIBUTING.md, art. I). It bundles the launcher, the native FSR components when they were built (with their
sources and licences) and the non-system libraries; GPU drivers, the Vulkan loader and glibc come from the host
(build on the oldest distribution you want to run it on). Its mount is read-only, so `xcde.ini`, `cache/`, saves
and logs live in `~/.local/share/monado` (or `MONADO_DATA_ROOT`, or a `monado-data` folder you create next to the
AppImage to keep everything beside it; a `~/.local/share/lasrecomp` folder from older versions that holds `xcde.ini`
keeps being used), which gets a `targets` link to the dump and a
`build/linux/monado` link to the packaged game: the launcher and the game use the same layout as a checkout.
The dump: the argument, `MONADO_GAME_ROOT`, a `targets/` folder next to the AppImage, the one chosen with the
launcher's **Select game dump** button (also in a checkout: it links `targets/` there), or the one used last time.
Needs [appimagetool](https://github.com/AppImage/appimagetool).
