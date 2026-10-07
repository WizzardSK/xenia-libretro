# Xenia libretro core

A [libretro](https://www.libretro.com/) core of [Xenia Edge](https://github.com/has207/xenia-edge), the Xbox 360 emulator, for RetroArch and other libretro frontends.

The emulator is upstream's: this repository follows xenia-edge and adds the libretro frontend in `libretro/`. It holds the core only; upstream's standalone UI, packaging and CI are not part of it (the paths are listed in `.upstream-excluded`). The version the core reports is upstream's, with the upstream commit it is merged to (`upstream.version`).

## Downloads

Builds for Windows, Linux (x86_64, arm64), macOS (Apple Silicon) and Android (arm64-v8a, x86_64) are on the [Releases](https://github.com/WizzardSK/xenia-libretro/releases) page.

- **Windows:** `xenia_edge_libretro.dll`, which needs the Visual C++ 2015-2022 runtime.
- **Linux:** `xenia_edge_libretro.so`, glibc 2.35 or newer; besides the C/C++ runtime it uses only lz4, X11/xcb and fontconfig. ARM64 uses the a64 CPU backend.
- **macOS:** `xenia_edge_libretro.dylib`, Apple Silicon only. The x64 CPU backend needs the address space below 4 GB, which a core loaded into RetroArch cannot free, so there is no Intel build.
- **Android:** `xenia_edge_libretro_android.so`.

## Video

Use RetroArch's `vulkan` video driver; on Windows `d3d12` works too. The core runs Xenia's own GPU emulation and hands RetroArch the finished frames.

## Content

Xbox 360 games as a disc image (`.iso`), an executable (`.xex`), or a `.zar` / `.xcp` package. No BIOS or firmware is needed.

Saves go to RetroArch's save directory. Title updates and DLC are read from Xenia's content folder, which is RetroArch's system directory, laid out as in standalone Xenia; the core option Apply Title Updates turns updates off.

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
