# Xenia libretro core

A [libretro](https://www.libretro.com/) core of [Xenia Edge](https://github.com/has207/xenia-edge), the Xbox 360 emulator, for RetroArch and other libretro frontends.

The emulator is upstream's: this repository follows xenia-edge and adds the libretro frontend in `libretro/`. It holds the core only; upstream's standalone UI, packaging and CI are not part of it (the paths are listed in `.upstream-excluded`). The version the core reports is upstream's, with the upstream commit it is merged to (`upstream.version`).

## Downloads

Builds for Windows, Linux (x86_64, arm64), macOS (Apple Silicon) and Android (arm64-v8a, x86_64) are on the [Releases](https://github.com/WizzardSK/xenia-libretro/releases) page.

- **Windows:** `xenia_edge_libretro.dll`, which needs the Visual C++ 2015-2022 runtime. It carries `dxil.dll`, Direct3D 12's shader validator, inside and writes it to `system/Xenia-Edge/D3D12/` on start.
- **Linux:** `xenia_edge_libretro.so`, glibc 2.35 or newer; besides the C/C++ runtime it uses only lz4, X11/xcb and fontconfig. ARM64 uses the a64 CPU backend.
- **macOS:** `xenia_edge_libretro.dylib`, Apple Silicon only. The x64 CPU backend needs the address space below 4 GB, which a core loaded into RetroArch cannot free, so there is no Intel build.
- **Android:** `xenia_edge_libretro_android.so`.

## Video

The core option Graphics API picks Xenia's GPU backend: Vulkan, or on Windows Direct3D 12. The core asks RetroArch for that kind of context, and RetroArch switches its video driver to match when it is allowed to (Settings > Video > Output). Direct3D 12 runs on the system's Direct3D 12 runtime: the Agility SDK runtime that standalone Xenia uses can only be loaded by an executable that exports it, which RetroArch does not.

## Content

Xbox 360 games as a disc image (`.iso`), an executable (`.xex`), or a `.zar` / `.xcp` package. No BIOS or firmware is needed.

Everything the core keeps is in `system/Xenia-Edge/`: saves, title updates and DLC under `content/`, laid out as in standalone Xenia's content folder; the shader and other caches under `cache/`; and Xenia's log, `xenia.log`. Errors and warnings from it also appear in RetroArch's log. The core option Apply Title Updates turns updates off.

## Building

With upstream's build dependencies (see upstream's documentation), from a checkout with its submodules:

```
git clone --recursive https://github.com/WizzardSK/xenia-libretro.git
cd xenia-libretro
./xenia-build.py slang
./xenia-build.py fetchdata
./xenia-build.py build --config=Release --target=xenia-libretro
```

The CI workflow `.github/workflows/libretro.yml` builds every platform, including the Android cross build.

## License

BSD, as Xenia. See [LICENSE](LICENSE).
