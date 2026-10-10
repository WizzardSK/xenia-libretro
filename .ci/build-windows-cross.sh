#!/bin/bash
# The Windows x64 core, cross-built on Linux for the libretro buildbot, whose
# own Windows runner has MSVC 2019 16.8 - too old for Xenia Edge, which the
# project's CI builds with MSVC on windows-2025. Here it is clang-cl and
# lld-link (LLVM 21, as the Linux jobs) against the MSVC CRT and Windows SDK
# that xwin downloads, so the result links against the same runtime as an
# MSVC build. Run from the repository root in ubuntu:24.04, as root:
#
#   BUILD_DIR=build/windows-x86_64 NUMPROC=8 .ci/build-windows-cross.sh
#
# leaves the core at $BUILD_DIR/xenia_edge_libretro.dll.

set -euo pipefail

BUILD_DIR=${BUILD_DIR:-build/windows-x86_64}
NUMPROC=${NUMPROC:-$(nproc)}
[ "$NUMPROC" -gt 0 ] || NUMPROC=1
TOOLS=${XE_CROSS_TOOLS:-/opt/xe-cross}
LLVM=/usr/lib/llvm-21/bin
XWIN_VERSION=0.10.0
DXC_URL=https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609
DXC_LINUX=linux_dxc_2026_09_28.x86_x64.tar.gz
DXC_WINDOWS=dxc_2026_09_29.zip

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
  ca-certificates wget gnupg lsb-release software-properties-common \
  git python3 python3-venv ninja-build file unzip xz-utils
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key > /etc/apt/trusted.gpg.d/apt.llvm.org.asc
apt-add-repository -y --no-update "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-21 main"
apt-get update -qq
apt-get install -y --no-install-recommends \
  clang-21 clang-tools-21 lld-21 llvm-21 libclang-rt-21-dev g++-14

# CMake, meson and Mesa's Python modules; 24.04's pip wants a venv
python3 -m venv "$TOOLS/venv"
"$TOOLS/venv/bin/pip" install -q --upgrade cmake meson mako pyyaml packaging
export PATH="$TOOLS/venv/bin:$PATH"

# The MSVC CRT and the Windows SDK, laid out with lower-case links so the
# case-sensitive file system finds <Windows.h> as well as <windows.h>
mkdir -p "$TOOLS"
wget -q -O "$TOOLS/xwin.tar.gz" \
  "https://github.com/Jake-Shadle/xwin/releases/download/$XWIN_VERSION/xwin-$XWIN_VERSION-x86_64-unknown-linux-musl.tar.gz"
tar -xzf "$TOOLS/xwin.tar.gz" -C "$TOOLS" --strip-components=1
"$TOOLS/xwin" --accept-license --arch x86_64 --cache-dir "$TOOLS/xwin-cache" \
  splat --output "$TOOLS/msvc"
rm -rf "$TOOLS/xwin-cache"
MSVC="$TOOLS/msvc"

# DXC: on Linux for slangc, which compiles the D3D12 shaders to DXIL with
# it, and dxil.dll from the Windows release, which the core carries inside
# (as the project's CI does)
mkdir -p "$TOOLS/dxc-linux"
wget -q -O "$TOOLS/dxc-linux.tar.gz" "$DXC_URL/$DXC_LINUX"
tar -xzf "$TOOLS/dxc-linux.tar.gz" -C "$TOOLS/dxc-linux"
export LD_LIBRARY_PATH="$TOOLS/dxc-linux/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
wget -q -O "$TOOLS/dxc-windows.zip" "$DXC_URL/$DXC_WINDOWS"
python3 -c "import sys, zipfile; open(sys.argv[2], 'wb').write(zipfile.ZipFile(sys.argv[1]).read(r'bin\x64\dxil.dll'))" \
  "$TOOLS/dxc-windows.zip" "$TOOLS/dxil.dll"
export XENIA_LIBRETRO_DXIL="$TOOLS/dxil.dll"

# The submodules the Windows build uses, as the project's CI takes them
git submodule update --init --depth=1 --jobs="$NUMPROC" \
  $(grep -oP '(?<=path = )(?!third_party/(MoltenVK|SPIRV-Cross)$).+' .gitmodules)
export CC=clang-21 CXX=clang++-21
./xenia-build.py slang
./xenia-build.py fetchdata
SLANGC=$(find "$PWD/.slang" -path '*/bin/slangc' -type f | head -1)
export SLANGC_PATH="$SLANGC"

# The shader compiler runs at build time, so it is built for Linux first and
# handed to the Windows build, as for Android
cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Release -DXENIA_ENABLE_LTO=OFF
cmake --build build-host --target xenia-shader-cc -- -j "$NUMPROC"
HOST_SHADER_CC=$(find "$PWD/build-host" -name xenia-shader-cc -type f | head -1)
unset CC CXX

# clang-cl against the downloaded headers and libraries
TOOLCHAIN="$TOOLS/clang-cl-x86_64.cmake"
cat > "$TOOLCHAIN" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER "$LLVM/clang-cl")
set(CMAKE_CXX_COMPILER "$LLVM/clang-cl")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_LINKER "$LLVM/lld-link")
set(CMAKE_AR "$LLVM/llvm-lib")
set(CMAKE_RC_COMPILER "$LLVM/llvm-rc")
set(CMAKE_MT "$LLVM/llvm-mt")
set(XE_CLANG_CL_INCLUDES
  "$MSVC/crt/include" "$MSVC/sdk/include/ucrt" "$MSVC/sdk/include/um"
  "$MSVC/sdk/include/shared" "$MSVC/sdk/include/winrt")
set(XE_CLANG_CL_LIBS
  "$MSVC/crt/lib/x86_64" "$MSVC/sdk/lib/um/x86_64" "$MSVC/sdk/lib/ucrt/x86_64")
set(_xe_flags "")
foreach(_xe_dir IN LISTS XE_CLANG_CL_INCLUDES)
  string(APPEND _xe_flags " /imsvc \"\${_xe_dir}\"")
endforeach()
set(_xe_link "/manifest:no")
foreach(_xe_dir IN LISTS XE_CLANG_CL_LIBS)
  string(APPEND _xe_link " /libpath:\"\${_xe_dir}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "\${_xe_flags}")
set(CMAKE_CXX_FLAGS_INIT "\${_xe_flags}")
set(CMAKE_RC_FLAGS_INIT "\${_xe_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "\${_xe_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "\${_xe_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "\${_xe_link}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

# version.h, which xenia-build.py writes into the build directory
python3 -c "import importlib.util as u, sys; s=u.spec_from_file_location('xb','xenia-build.py'); m=u.module_from_spec(s); s.loader.exec_module(m); m.generate_version_h(sys.argv[1])" "$BUILD_DIR"
cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DXENIA_HOST_SHADER_CC="$HOST_SHADER_CC" -DXENIA_ENABLE_LTO=OFF
cmake --build "$BUILD_DIR" --target xenia-libretro -- -j "$NUMPROC"
mv "$(find "$BUILD_DIR" -name xenia_edge_libretro.dll | head -1)" "$BUILD_DIR/xenia_edge_libretro.dll"
file "$BUILD_DIR/xenia_edge_libretro.dll"
