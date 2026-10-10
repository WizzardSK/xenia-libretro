# Suppress warnings for all third-party code — we can't clean them up and
# don't want them contributing to -Werror noise on our own targets.
if(MSVC)
  add_compile_options(/W0)
else()
  add_compile_options(-w)
endif()

# ==============================================================================
# xbyak_aarch64 (ARM64 JIT assembler)
# ==============================================================================
if(XE_TARGET_AARCH64)
  add_library(xbyak_aarch64 STATIC
    xbyak_aarch64/src/xbyak_aarch64_impl.cpp
    xbyak_aarch64/src/util_impl.cpp
  )
  target_include_directories(xbyak_aarch64 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/xbyak_aarch64
    ${CMAKE_CURRENT_SOURCE_DIR}/xbyak_aarch64/src
    ${CMAKE_CURRENT_SOURCE_DIR}/xbyak_aarch64/xbyak_aarch64
  )
  if(MSVC)
    target_compile_definitions(xbyak_aarch64 PRIVATE NOMINMAX)
    target_compile_options(xbyak_aarch64 PRIVATE /w)
  else()
    target_compile_options(xbyak_aarch64 PRIVATE -w)
  endif()
endif()

# ==============================================================================
# aes_128
# ==============================================================================
add_library(aes_128 STATIC
  aes_128/unroll/aes.c
)
target_include_directories(aes_128 PUBLIC aes_128)

# ==============================================================================
# asio (header-only)
# ==============================================================================
add_library(asio INTERFACE)
target_include_directories(asio INTERFACE asio/include)
target_compile_definitions(asio INTERFACE
  ASIO_STANDALONE
  ASIO_NO_DEPRECATED
)
if(WIN32)
  target_compile_definitions(asio INTERFACE _WIN32_WINNT=0x0A00)
endif()

# ==============================================================================
# capstone
# ==============================================================================
add_library(capstone STATIC
  capstone/cs.c
  capstone/MCInst.c
  capstone/MCInstPrinter.c
  capstone/MCInstrDesc.c
  capstone/MCRegisterInfo.c
  capstone/SStream.c
  capstone/utils.c
  capstone/Mapping.c
)
# Architecture-specific capstone files
if(XE_TARGET_AARCH64)
  file(GLOB _capstone_arch_c "capstone/arch/AArch64/*.c")
  file(GLOB _capstone_arch_h "capstone/arch/AArch64/*.h")
  file(GLOB _capstone_arch_inc "capstone/arch/AArch64/*.inc")
  target_sources(capstone PRIVATE ${_capstone_arch_c} ${_capstone_arch_h} ${_capstone_arch_inc})
  target_compile_definitions(capstone PRIVATE
    CAPSTONE_HAS_ARM64
    CAPSTONE_HAS_AARCH64
    CAPSTONE_USE_SYS_DYN_MEM
  )
else()
  file(GLOB _capstone_x86_c "capstone/arch/X86/*.c")
  file(GLOB _capstone_x86_h "capstone/arch/X86/*.h")
  file(GLOB _capstone_x86_inc "capstone/arch/X86/*.inc")
  list(FILTER _capstone_x86_c EXCLUDE REGEX "X86ATTInstPrinter\\.c$")
  list(FILTER _capstone_x86_inc EXCLUDE REGEX "reduce\\.inc$")
  target_sources(capstone PRIVATE ${_capstone_x86_c} ${_capstone_x86_h} ${_capstone_x86_inc})
  target_compile_definitions(capstone PRIVATE
    CAPSTONE_X86_ATT_DISABLE
    CAPSTONE_HAS_X86
    CAPSTONE_USE_SYS_DYN_MEM
  )
endif()
target_include_directories(capstone PUBLIC capstone/include PRIVATE capstone)
# Force all capstone sources to compile as C
file(GLOB_RECURSE _capstone_all_c "capstone/*.c")
set_source_files_properties(${_capstone_all_c} PROPERTIES LANGUAGE C)

# ==============================================================================
# boost_context (fcontext_t fiber context switching)
#
# In its own subdirectory because it builds assembly: the project's global
# C/C++ compile options (/Zc:*, /arch:AVX, ...) are not valid for ml64 and
# silently break the MASM build, so that subdir clears inherited COMPILE_OPTIONS.
# ==============================================================================
add_subdirectory(boost_context)

# ==============================================================================
# cxxopts (header-only)
# ==============================================================================
add_library(cxxopts INTERFACE)
target_include_directories(cxxopts INTERFACE cxxopts/include)

# ==============================================================================
# tomlplusplus (header-only)
# ==============================================================================
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus INTERFACE tomlplusplus/include)

# ==============================================================================
# dxbc
# ==============================================================================
add_library(dxbc STATIC
  dxbc/DXBCChecksum.cpp
)
target_include_directories(dxbc PUBLIC dxbc)

# ==============================================================================
# fmt
# ==============================================================================
add_library(fmt STATIC
  fmt/src/format.cc
  fmt/src/os.cc
)
target_include_directories(fmt PUBLIC fmt/include)

# ==============================================================================
# glslang-spirv (runtime: SPIR-V emission primitives for Xenia's own
# SpirvShaderTranslator; used by the Vulkan backend and by the Metal backend's
# SPIRV-Cross path)
# ==============================================================================
file(GLOB _glslang_spirv_cpp "glslang/SPIRV/*.cpp")
file(GLOB _glslang_spirv_h "glslang/SPIRV/*.h")
file(GLOB _glslang_spirv_hpp "glslang/SPIRV/*.hpp11")
# Excluding GlslangToSpv keeps MachineIndependent out of the runtime link.
list(FILTER _glslang_spirv_cpp EXCLUDE REGEX "GlslangToSpv\\.cpp$")
add_library(glslang-spirv STATIC
  ${_glslang_spirv_cpp}
  ${_glslang_spirv_h}
  ${_glslang_spirv_hpp}
  glslang/glslang/Include/visibility.h
)
target_include_directories(glslang-spirv PUBLIC glslang)

# Cross-compiling x64-host → ARM64-target on Windows: the cross
# toolchain produces an ARM64 PE for xenia-shader-cc that won't run on
# the x64 host, so reuse the binary from a previous native build instead
# (Windows-on-ARM runs x64 fine via Prism, and macOS handles its own
# x86_64↔arm64 via Rosetta — both directions are unaffected).
set(_xe_use_host_shader_cc FALSE)
if(WIN32 AND CMAKE_CROSSCOMPILING AND XE_TARGET_AARCH64
    AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
  set(_xe_use_host_shader_cc TRUE)
endif()
# Any other cross-compile (Android) is given a host build's xenia-shader-cc.
set(XENIA_HOST_SHADER_CC "" CACHE FILEPATH
    "xenia-shader-cc built for the host, for cross-compiling (Android)")
if(XENIA_HOST_SHADER_CC)
  set(_xe_use_host_shader_cc TRUE)
endif()

if(NOT _xe_use_host_shader_cc)

# ==============================================================================
# SPIRV-Headers + SPIRV-Tools (host: SPIR-V optimization + disassembly
# support for glslang's ENABLE_OPT path; used by the xenia-shader-cc tool)
# ==============================================================================
# Point SPIRV-Tools at our in-tree SPIRV-Headers checkout instead of the
# external/spirv-headers path it expects by default.
set(SPIRV-Headers_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SPIRV-Headers"
    CACHE INTERNAL "")
set(SPIRV_SKIP_TESTS ON CACHE INTERNAL "")
set(SPIRV_SKIP_EXECUTABLES ON CACHE INTERNAL "")
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE INTERNAL "")
set(SPIRV_WERROR OFF CACHE INTERNAL "")
set(SPIRV_BUILD_FUZZER OFF CACHE INTERNAL "")
add_subdirectory(SPIRV-Tools EXCLUDE_FROM_ALL)

# SPIRV-Tools unconditionally forces /W2 /WX on MSVC via its own
# target_compile_options, which conflicts with the directory-level /W0 and
# produces D9025 "overriding /W0 with /W2" for every translation unit. Strip
# the /W* and /WX flags it added so only the directory-level /W0 survives.
if(MSVC)
  foreach(_spvt_target
      SPIRV-Tools-static SPIRV-Tools-shared SPIRV-Tools-opt
      SPIRV-Tools-link SPIRV-Tools-reduce SPIRV-Tools-diff)
    if(TARGET ${_spvt_target})
      get_target_property(_opts ${_spvt_target} COMPILE_OPTIONS)
      if(_opts)
        list(FILTER _opts EXCLUDE REGEX "^/[Ww]([0-9]|all|X)$")
        set_target_properties(${_spvt_target} PROPERTIES COMPILE_OPTIONS "${_opts}")
      endif()
    endif()
  endforeach()
endif()

# ==============================================================================
# glslang (host: full frontend + SPIR-V emission for the xenia-shader-cc
# build-time tool; all platforms, including macOS)
# ==============================================================================
# Read glslang's own version file to generate build_info.h in sync with the
# pinned submodule, matching what glslang's own CMakeLists does.
include(${CMAKE_CURRENT_SOURCE_DIR}/glslang/parse_version.cmake)
parse_version(${CMAKE_CURRENT_SOURCE_DIR}/glslang/CHANGES.md GLSLANG)
set(major ${GLSLANG_VERSION_MAJOR})
set(minor ${GLSLANG_VERSION_MINOR})
set(patch ${GLSLANG_VERSION_PATCH})
set(flavor ${GLSLANG_VERSION_FLAVOR})
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/glslang/build_info.h.tmpl
  ${CMAKE_CURRENT_BINARY_DIR}/glslang-build-info/glslang/build_info.h
  @ONLY)

file(GLOB _glslang_mi
  "glslang/glslang/MachineIndependent/*.cpp"
  "glslang/glslang/MachineIndependent/preprocessor/*.cpp"
  "glslang/glslang/GenericCodeGen/*.cpp")
file(GLOB _glslang_spirv_all "glslang/SPIRV/*.cpp")
add_library(glslang STATIC
  ${_glslang_mi}
  ${_glslang_spirv_all}
  glslang/glslang/ResourceLimits/ResourceLimits.cpp
)
if(WIN32)
  target_sources(glslang PRIVATE glslang/glslang/OSDependent/Windows/ossource.cpp)
else()
  target_sources(glslang PRIVATE glslang/glslang/OSDependent/Unix/ossource.cpp)
endif()
target_include_directories(glslang PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/glslang
  ${CMAKE_CURRENT_BINARY_DIR}/glslang-build-info)
target_compile_features(glslang PUBLIC cxx_std_17)
# Enable glslang's SpvTools.cpp optimizer/disassembler bridge to SPIRV-Tools.
target_compile_definitions(glslang PRIVATE ENABLE_OPT=1)
target_link_libraries(glslang PUBLIC SPIRV-Tools-opt SPIRV-Tools-static)
# glslang relies on exceptions internally.
if(MSVC)
  target_compile_options(glslang PRIVATE /EHsc)
endif()
if(NOT WIN32)
  find_package(Threads)
  if(Threads_FOUND)
    target_link_libraries(glslang PUBLIC Threads::Threads)
  endif()
endif()

endif()  # NOT _xe_use_host_shader_cc (SPIRV-Tools + glslang block)

# ==============================================================================
# SPIRV-Cross (Apple only — used at runtime by xenia-gpu-metal for SPIR-V → MSL
# via MslShader, and by MoltenVKShaderConverter below)
# ==============================================================================
if(APPLE)
  add_library(spirv-cross STATIC
    SPIRV-Cross/spirv_cross.cpp
    SPIRV-Cross/spirv_cross_parsed_ir.cpp
    SPIRV-Cross/spirv_parser.cpp
    SPIRV-Cross/spirv_cfg.cpp
    SPIRV-Cross/spirv_cross_util.cpp
    SPIRV-Cross/spirv_glsl.cpp
    SPIRV-Cross/spirv_msl.cpp
    SPIRV-Cross/spirv_reflect.cpp
  )
  target_include_directories(spirv-cross PUBLIC SPIRV-Cross)
  target_compile_features(spirv-cross PUBLIC cxx_std_17)
  if(MSVC)
    target_compile_options(spirv-cross PRIVATE /EHsc)
  endif()
endif()

# ==============================================================================
# MoltenVK (Apple only — Vulkan-on-Metal driver, statically linked into
# xenia-ui-vulkan so the macOS build needs no MoltenVK install at runtime)
# ==============================================================================
# Mirrors the layout of MoltenVK's own CMakeLists.txt (Common +
# MoltenVKShaderConverter + MoltenVK) but builds against our in-tree
# Vulkan-Headers / SPIRV-Cross / SPIRV-Tools / cereal instead of fetching its
# own copies via CPM.cmake. The SPIRV-Cross and cereal submodules are pinned to
# the revisions MoltenVK expects (see MoltenVK/ExternalRevisions/*) so the
# spirv_msl ABI and pipeline-cache serialization format match.
if(APPLE)
  # Record the MoltenVK submodule revision for MVKDevice's identification
  # string (MoltenVK's own CMake runs `git rev-parse` here; we just shell
  # out once at configure time and bake the string into a generated header).
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR}/MoltenVK rev-parse --short HEAD
    OUTPUT_VARIABLE MVK_GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT MVK_GIT_REV)
    set(MVK_GIT_REV "00000000")
  endif()
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/MoltenVK/Templates/cmake/mvkGitRevDerived.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/MoltenVK-gen/mvkGitRevDerived.h
    @ONLY)

  # MoltenVK::Common — OS/Foundation/Metal helpers shared by core + converter.
  file(GLOB _mvk_common_src
    "MoltenVK/Common/*.c"
    "MoltenVK/Common/*.cpp"
    "MoltenVK/Common/*.m"
    "MoltenVK/Common/*.mm")
  add_library(MoltenVK_Common STATIC ${_mvk_common_src})
  add_library(MoltenVK::Common ALIAS MoltenVK_Common)
  target_include_directories(MoltenVK_Common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/MoltenVK/Common
    ${CMAKE_CURRENT_SOURCE_DIR}/MoltenVK/MoltenVK/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-Headers/include)
  target_compile_features(MoltenVK_Common PUBLIC cxx_std_17)
  target_compile_options(MoltenVK_Common PUBLIC
    -Wno-deprecated-declarations
    -Wno-vla-cxx-extension)
  target_compile_definitions(MoltenVK_Common PUBLIC
    MVK_CONFIG_LOG_LEVEL=1          # error-only at runtime
    MVK_USE_METAL_PRIVATE_API=1)    # primitive-restart disable + legacy SPI

  # MoltenVK::ShaderConverter — wraps SPIRV-Cross + SPIRV-Tools to produce MSL.
  file(GLOB _mvk_sc_src
    "MoltenVK/MoltenVKShaderConverter/MoltenVKShaderConverter/*.c"
    "MoltenVK/MoltenVKShaderConverter/MoltenVKShaderConverter/*.cpp"
    "MoltenVK/MoltenVKShaderConverter/MoltenVKShaderConverter/*.m"
    "MoltenVK/MoltenVKShaderConverter/MoltenVKShaderConverter/*.mm")
  add_library(MoltenVK_ShaderConverter STATIC ${_mvk_sc_src})
  add_library(MoltenVK::ShaderConverter ALIAS MoltenVK_ShaderConverter)
  target_include_directories(MoltenVK_ShaderConverter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/MoltenVK/MoltenVKShaderConverter
    ${CMAKE_CURRENT_SOURCE_DIR}/SPIRV-Headers/include)
  target_link_libraries(MoltenVK_ShaderConverter PUBLIC
    MoltenVK::Common spirv-cross SPIRV-Tools-static)

  # MoltenVK — the core driver. .mm files compile with manual retain/release;
  # don't let any inherited -fobjc-arc flag flip them.
  file(GLOB_RECURSE _mvk_core_src
    "MoltenVK/MoltenVK/MoltenVK/*.c"
    "MoltenVK/MoltenVK/MoltenVK/*.cpp"
    "MoltenVK/MoltenVK/MoltenVK/*.m"
    "MoltenVK/MoltenVK/MoltenVK/*.mm")
  add_library(MoltenVK STATIC ${_mvk_core_src})
  target_include_directories(MoltenVK PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/MoltenVK/MoltenVK/include
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/MoltenVK/MoltenVK/MoltenVK/API
    ${CMAKE_CURRENT_SOURCE_DIR}/MoltenVK/MoltenVK/MoltenVK/Commands
    ${CMAKE_CURRENT_SOURCE_DIR}/MoltenVK/MoltenVK/MoltenVK/GPUObjects
    ${CMAKE_CURRENT_SOURCE_DIR}/MoltenVK/MoltenVK/MoltenVK/Layers
    ${CMAKE_CURRENT_SOURCE_DIR}/MoltenVK/MoltenVK/MoltenVK/OS
    ${CMAKE_CURRENT_SOURCE_DIR}/MoltenVK/MoltenVK/MoltenVK/Utility
    ${CMAKE_CURRENT_SOURCE_DIR}/MoltenVK/MoltenVK/MoltenVK/Vulkan
    ${CMAKE_CURRENT_SOURCE_DIR}/cereal/include
    ${CMAKE_CURRENT_BINARY_DIR}/MoltenVK-gen)
  # Unquoted: MoltenVK runs MVK_STRINGIFY on this and compares to
  # "$MAJOR.$MINOR.$PATCH" — quotes here would survive into the stringify.
  target_compile_definitions(MoltenVK PUBLIC
    MVK_FRAMEWORK_VERSION=1.4.2)
  target_link_libraries(MoltenVK PUBLIC
    MoltenVK::ShaderConverter
    "-framework Foundation"
    "-framework Metal"
    "-framework AppKit"
    "-framework IOKit"
    "-framework IOSurface"
    "-framework QuartzCore"
    "-framework CoreGraphics")
  # MoltenVK's OS/ TUs define Objective-C categories on MTLSamplerDescriptor,
  # MTLRenderPipelineDescriptor, CAMetalLayer, and NSString. Static archive
  # members that only contribute categories are dropped by ld (no external
  # symbol references them), so at runtime calls like setBorderColorMVK: raise
  # "unrecognized selector". Force-load the archive so every member ships in
  # the final binary.
  target_link_options(MoltenVK INTERFACE
    "LINKER:-force_load,$<TARGET_FILE:MoltenVK>")
endif()

# ==============================================================================
# xenia-shader-cc (build-time host tool: GLSL/XeSL -> SPIR-V -> .h, and on
# Apple, XeSL -> MSL metallib -> .h via xcrun metal)
# ==============================================================================
if(NOT _xe_use_host_shader_cc)

add_executable(xenia-shader-cc ${PROJECT_SOURCE_DIR}/tools/build/shader_cc.cc)
target_link_libraries(xenia-shader-cc PRIVATE glslang SPIRV-Tools-static)
target_include_directories(xenia-shader-cc PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/glslang/StandAlone)
set_target_properties(xenia-shader-cc PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/host_tools")
if(APPLE)
  target_compile_definitions(xenia-shader-cc PRIVATE
    XE_SHADER_CC_METAL=1
    XE_METAL_MIN_OS="${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
if(MSVC)
  target_compile_options(xenia-shader-cc PRIVATE /EHsc)
endif()

elseif(XENIA_HOST_SHADER_CC)

add_executable(xenia-shader-cc IMPORTED GLOBAL)
set_target_properties(xenia-shader-cc PROPERTIES
  IMPORTED_LOCATION "${XENIA_HOST_SHADER_CC}")

else()  # _xe_use_host_shader_cc: x64→ARM64 cross-compile, use native build's binary

# Native x64 build lives at <repo>/build/ (xenia-build.py: get_build_dir(None)).
set(_xe_host_shader_cc_dir "${CMAKE_SOURCE_DIR}/build/host_tools")
set(_xe_host_shader_cc_any "")
foreach(_cfg Release Checked Debug)
  if(EXISTS "${_xe_host_shader_cc_dir}/${_cfg}/xenia-shader-cc.exe")
    set(_xe_host_shader_cc_any "${_xe_host_shader_cc_dir}/${_cfg}/xenia-shader-cc.exe")
    break()
  endif()
endforeach()
if(NOT _xe_host_shader_cc_any)
  message(FATAL_ERROR
    "Cross-compile to ARM64 needs a host xenia-shader-cc.exe but none was found "
    "under ${_xe_host_shader_cc_dir}/<Config>/.\n"
    "  Run a native build first to produce it, e.g.:\n"
    "    xb build --target xenia-shader-cc --config=release\n"
    "  Then re-run the cross-compile.")
endif()
add_executable(xenia-shader-cc IMPORTED GLOBAL)
set_target_properties(xenia-shader-cc PROPERTIES
  IMPORTED_LOCATION "${_xe_host_shader_cc_any}"
  IMPORTED_LOCATION_RELEASE "${_xe_host_shader_cc_dir}/Release/xenia-shader-cc.exe"
  IMPORTED_LOCATION_CHECKED "${_xe_host_shader_cc_dir}/Checked/xenia-shader-cc.exe"
  IMPORTED_LOCATION_DEBUG "${_xe_host_shader_cc_dir}/Debug/xenia-shader-cc.exe")

endif()

# ==============================================================================
# imgui
# ==============================================================================
add_library(imgui STATIC
  imgui/imgui.cpp
  imgui/imgui_demo.cpp
  imgui/imgui_draw.cpp
  imgui/imgui_tables.cpp
  imgui/imgui_widgets.cpp
)
target_include_directories(imgui PUBLIC imgui PRIVATE stb)

# ==============================================================================
# miniaudio (header-only)
# ==============================================================================
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio INTERFACE miniaudio)

# ==============================================================================
# mspack
# ==============================================================================
add_library(mspack STATIC
  mspack/logging.cc
  mspack/lzxd.c
  mspack/system.c
)
target_compile_definitions(mspack PRIVATE HAVE_CONFIG_H)
target_include_directories(mspack PUBLIC mspack)
target_link_libraries(mspack PUBLIC xenia-base)

# ==============================================================================
# pugixml
# ==============================================================================
add_library(pugixml STATIC
  pugixml/src/pugixml.cpp
)
target_include_directories(pugixml PUBLIC pugixml/src)

# ==============================================================================
# snappy
# ==============================================================================
# snappy and zlib-ng generate their config headers into their source trees
# with a configure of their own. A cross build configures them for the
# target, not for the build machine (Release: the debug CRT is not part of a
# cross toolchain's runtime).
set(_xe_nested_toolchain_flag "")
if(CMAKE_TOOLCHAIN_FILE)
  set(_xe_nested_toolchain_flag "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
    -G "${CMAKE_GENERATOR}" -DCMAKE_BUILD_TYPE=Release)
endif()
# Generate snappy-stubs-public.h if missing
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/snappy/snappy-stubs-public.h")
  if(XE_TARGET_X86_64)
    set(_snappy_avx_flag "-DSNAPPY_REQUIRE_AVX=ON")
  else()
    set(_snappy_avx_flag "-DSNAPPY_REQUIRE_AVX=OFF")
  endif()
  execute_process(
    COMMAND ${CMAKE_COMMAND}
      ${_xe_nested_toolchain_flag}
      -DSNAPPY_BUILD_TESTS=OFF
      -DSNAPPY_BUILD_BENCHMARKS=OFF
      ${_snappy_avx_flag}
      ${CMAKE_CURRENT_SOURCE_DIR}/snappy
      -B${CMAKE_CURRENT_SOURCE_DIR}/snappy
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
endif()
add_library(snappy STATIC
  snappy/snappy-internal.h
  snappy/snappy-sinksource.cc
  snappy/snappy-sinksource.h
  snappy/snappy-stubs-internal.cc
  snappy/snappy-stubs-internal.h
  snappy/snappy-stubs-public.h
  snappy/snappy.cc
  snappy/snappy.h
)
target_include_directories(snappy PUBLIC snappy)

# ==============================================================================
# xxhash
# ==============================================================================
add_library(xxhash STATIC
  xxhash/xxhash.c
)
target_include_directories(xxhash PUBLIC xxhash)

# ==============================================================================
# zstd
# ==============================================================================
file(GLOB_RECURSE _zstd_common "zstd/lib/common/*.c" "zstd/lib/common/*.h")
list(FILTER _zstd_common EXCLUDE REGEX "threading\\.c$")
file(GLOB_RECURSE _zstd_compress "zstd/lib/compress/*.c" "zstd/lib/compress/*.h")
file(GLOB_RECURSE _zstd_decompress "zstd/lib/decompress/*.c" "zstd/lib/decompress/*.h")
add_library(zstd STATIC
  zstd/lib/zstd.h
  ${_zstd_common}
  ${_zstd_compress}
  ${_zstd_decompress}
)
target_include_directories(zstd PUBLIC zstd/lib PRIVATE zstd/lib/common)
target_compile_definitions(zstd PRIVATE
  XXH_NAMESPACE=ZSTD_
  ZSTD_LEGACY_SUPPORT=0
)
if(XE_TARGET_X86_64 AND NOT MSVC)
  enable_language(ASM)
  target_sources(zstd PRIVATE zstd/lib/decompress/huf_decompress_amd64.S)
else()
  target_compile_definitions(zstd PRIVATE ZSTD_DISABLE_ASM)
endif()

# ==============================================================================
# zarchive
# ==============================================================================
add_library(zarchive STATIC
  zarchive/include/zarchive/zarchivecommon.h
  zarchive/include/zarchive/zarchivereader.h
  zarchive/include/zarchive/zarchivewriter.h
  zarchive/src/zarchivereader.cpp
  zarchive/src/zarchivewriter.cpp
  zarchive/src/sha_256.c
  zarchive/src/sha_256.h
)
target_include_directories(zarchive PUBLIC zarchive/include PRIVATE zstd/lib)
target_link_libraries(zarchive PUBLIC zstd)

# ==============================================================================
# zlib-ng
# ==============================================================================
# Generate headers if missing
set(_zlibng_needs_configure FALSE)
foreach(_hdr zlib-ng.h zconf-ng.h zlib_name_mangling-ng.h gzread.c)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/zlib-ng/${_hdr}")
    set(_zlibng_needs_configure TRUE)
    break()
  endif()
endforeach()
if(_zlibng_needs_configure)
  execute_process(
    COMMAND ${CMAKE_COMMAND}
      ${_xe_nested_toolchain_flag}
      -DZLIB_ENABLE_TESTS=OFF
      -DWITH_GTEST=OFF
      ${CMAKE_CURRENT_SOURCE_DIR}/zlib-ng
      -B${CMAKE_CURRENT_SOURCE_DIR}/zlib-ng
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  )
endif()
file(GLOB _zlibng_c "zlib-ng/*.c")
file(GLOB _zlibng_generic "zlib-ng/arch/generic/*.c")
if(XE_TARGET_AARCH64)
  file(GLOB _zlibng_arch "zlib-ng/arch/arm/*.c")
  list(FILTER _zlibng_arch EXCLUDE REGEX "armv6")
  add_library(zlib-ng STATIC
    ${_zlibng_c}
    ${_zlibng_arch}
    ${_zlibng_generic}
  )
  target_compile_definitions(zlib-ng PRIVATE
    ARM_FEATURES
    ARM_NEON
    ARM_NEON_HASLD4
    ARM_ACLE
    HAVE_ARM_ACLE_H
    WITH_GZFILEOP
  )
  if(NOT MSVC)
    target_compile_definitions(zlib-ng PRIVATE HAVE_BUILTIN_CTZ HAVE_BUILTIN_CTZLL HAVE_ATTRIBUTE_ALIGNED)
  endif()
else()
  file(GLOB _zlibng_x86 "zlib-ng/arch/x86/*.c")
  add_library(zlib-ng STATIC
    ${_zlibng_c}
    ${_zlibng_x86}
    ${_zlibng_generic}
  )
  target_compile_definitions(zlib-ng PRIVATE
    X86_FEATURES
    X86_HAVE_XSAVE_INTRIN
    X86_SSE2
    X86_SSSE3
    X86_SSE42
    X86_AVX2
    X86_AVX512
    X86_AVX512VNNI
    WITH_GZFILEOP
  )
  if(NOT MSVC)
    # GCC/Clang have native __builtin_ctz/__builtin_ctzll (MSVC gets these
    # from fallback_builtins.h). Defining these enables optimized compare256
    # and longest_match codepaths. HAVE_ATTRIBUTE_ALIGNED gates GCC-style
    # __attribute__((aligned)); MSVC uses __declspec(align) via its fallback.
    target_compile_definitions(zlib-ng PRIVATE HAVE_BUILTIN_CTZ HAVE_BUILTIN_CTZLL HAVE_ATTRIBUTE_ALIGNED)
    # GCC/Clang need per-file ISA flags; MSVC handles this via runtime dispatch
    set_source_files_properties(
      zlib-ng/arch/x86/adler32_avx2.c
      zlib-ng/arch/x86/chunkset_avx2.c
      zlib-ng/arch/x86/compare256_avx2.c
      zlib-ng/arch/x86/slide_hash_avx2.c
      PROPERTIES COMPILE_OPTIONS "-mavx2;-mbmi2"
    )
    set_source_files_properties(
      zlib-ng/arch/x86/adler32_avx512.c
      zlib-ng/arch/x86/chunkset_avx512.c
      PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512dq;-mavx512vl;-mavx512bw;-mbmi2"
    )
    set_source_files_properties(
      zlib-ng/arch/x86/adler32_avx512_vnni.c
      PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512dq;-mavx512vl;-mavx512bw;-mavx512vnni;-mbmi2"
    )
  endif()
endif()
target_include_directories(zlib-ng PUBLIC zlib-ng)

# ==============================================================================
# libusb (Windows only)
# ==============================================================================
if(WIN32)
  add_library(libusb STATIC
    libusb/libusb/core.c
    libusb/libusb/descriptor.c
    libusb/libusb/hotplug.c
    libusb/libusb/io.c
    libusb/libusb/strerror.c
    libusb/libusb/sync.c
    libusb/libusb/os/events_windows.c
    libusb/libusb/os/events_windows.h
    libusb/libusb/os/threads_windows.c
    libusb/libusb/os/threads_windows.h
    libusb/libusb/os/windows_common.c
    libusb/libusb/os/windows_common.h
    libusb/libusb/os/windows_usbdk.c
    libusb/libusb/os/windows_usbdk.h
    libusb/libusb/os/windows_winusb.c
    libusb/libusb/os/windows_winusb.h
  )
  target_include_directories(libusb PUBLIC libusb/libusb PRIVATE libusb/msvc)
endif()

# ==============================================================================
# FFmpeg — upstream source lives in the FFmpeg submodule; xenia's static
# config.h / asm configuration overlay lives in ffmpeg-xenia alongside it.
# ==============================================================================
set(XE_HAS_GAS_ASM FALSE)
if(XE_TARGET_AARCH64)
  if(MSVC)
    # MSVC can't assemble GAS-syntax .S files. Use clang as the ASM compiler
    # if available — the resulting .obj files link fine with MSVC-compiled code.
    # Pick clang from the host architecture directory so it can actually
    # run on this machine (clang is a cross-compiler — it handles the
    # target arch via --target regardless of host).
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "ARM64")
      set(_clang_host_dir "ARM64")
    else()
      set(_clang_host_dir "x64")
    endif()
    find_program(CLANG_EXE clang HINTS
      "$ENV{VCINSTALLDIR}/Tools/Llvm/${_clang_host_dir}/bin"
      "$ENV{VCINSTALLDIR}/Tools/Llvm/bin"
      "C:/Program Files/LLVM/bin"
    )
    if(CLANG_EXE)
      set(XE_HAS_GAS_ASM TRUE)
      set(XE_CLANG_ASM "${CLANG_EXE}")
      message(STATUS "Using clang for ARM64 assembly: ${CLANG_EXE}")
    else()
      message(WARNING "clang not found — FFmpeg NEON assembly disabled on MSVC ARM64. "
                      "Install LLVM/clang to enable optimized audio decoding.")
    endif()
  else()
    enable_language(ASM)
    set(XE_HAS_GAS_ASM TRUE)
  endif()
endif()

# Helper: compile GAS-syntax .S files with clang on MSVC, or natively on
# GCC/Clang. Adds the resulting objects to the target.
function(xe_add_gas_sources target)
  if(NOT XE_HAS_GAS_ASM)
    return()
  endif()
  foreach(src ${ARGN})
    if(XE_CLANG_ASM)
      # MSVC: use clang to assemble .S -> .obj
      get_filename_component(src_name "${src}" NAME_WE)
      get_filename_component(src_abs "${src}" ABSOLUTE)
      set(obj "${CMAKE_CURRENT_BINARY_DIR}/${src_name}.obj")
      add_custom_command(
        OUTPUT "${obj}"
        COMMAND "${XE_CLANG_ASM}" --target=aarch64-pc-windows-msvc
                -I "${CMAKE_CURRENT_SOURCE_DIR}/FFmpeg"
                -I "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg-xenia"
                -c "${src_abs}" -o "${obj}"
        DEPENDS "${src_abs}"
        COMMENT "Assembling ${src} with clang"
      )
      target_sources(${target} PRIVATE "${obj}")
    else()
      # GCC/Clang: native ASM support
      target_sources(${target} PRIVATE "${src}")
    endif()
  endforeach()
endfunction()

# Shared FFmpeg settings applied to each libav* target (defines, include
# paths, platform-specific link deps, warning suppression, C linkage).
function(ffmpeg_common target)
  target_compile_definitions(${target} PRIVATE
    HAVE_AV_CONFIG_H
    _USE_MATH_DEFINES
  )
  target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg-xenia
    ${CMAKE_CURRENT_SOURCE_DIR}/FFmpeg
  )

  if(WIN32)
    target_include_directories(${target} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/FFmpeg/compat/atomics/win32
    )
    target_link_libraries(${target} PRIVATE bcrypt)
    # FFmpeg in unoptimized builds emits so much dead code that link time
    # blows up; /Os keeps Debug and Checked within sane limits.
    target_compile_options(${target} PRIVATE
      $<$<CONFIG:Debug>:/Os>
      $<$<CONFIG:Checked>:/Os>
    )
  else()
    target_include_directories(${target} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/FFmpeg/compat/atomics/gcc
    )
    # glibc POSIX feature macros — FFmpeg assumes them for certain
    # syscalls and string extensions.
    target_compile_definitions(${target} PRIVATE
      _GNU_SOURCE
      _POSIX_C_SOURCE=200809L
      _XOPEN_SOURCE=700
    )
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /w)
  else()
    target_compile_options(${target} PRIVATE -w)
  endif()

  # All FFmpeg source is C
  set_target_properties(${target} PROPERTIES LINKER_LANGUAGE C)
endfunction()

# --- libavutil ---
add_library(libavutil STATIC
  FFmpeg/libavutil/adler32.c
  FFmpeg/libavutil/aes.c
  FFmpeg/libavutil/aes_ctr.c
  FFmpeg/libavutil/audio_fifo.c
  FFmpeg/libavutil/avstring.c
  FFmpeg/libavutil/avsscanf.c
  FFmpeg/libavutil/base64.c
  FFmpeg/libavutil/blowfish.c
  FFmpeg/libavutil/bprint.c
  FFmpeg/libavutil/buffer.c
  FFmpeg/libavutil/cast5.c
  FFmpeg/libavutil/camellia.c
  FFmpeg/libavutil/channel_layout.c
  FFmpeg/libavutil/csp.c
  FFmpeg/libavutil/cpu.c
  FFmpeg/libavutil/crc.c
  FFmpeg/libavutil/des.c
  FFmpeg/libavutil/dict.c
  FFmpeg/libavutil/display.c
  FFmpeg/libavutil/dovi_meta.c
  FFmpeg/libavutil/downmix_info.c
  FFmpeg/libavutil/encryption_info.c
  FFmpeg/libavutil/error.c
  FFmpeg/libavutil/eval.c
  FFmpeg/libavutil/fifo.c
  FFmpeg/libavutil/file.c
  FFmpeg/libavutil/file_open.c
  FFmpeg/libavutil/float_dsp.c
  FFmpeg/libavutil/fixed_dsp.c
  FFmpeg/libavutil/frame.c
  FFmpeg/libavutil/hash.c
  FFmpeg/libavutil/hdr_dynamic_metadata.c
  FFmpeg/libavutil/hmac.c
  FFmpeg/libavutil/hwcontext.c
  FFmpeg/libavutil/imgutils.c
  FFmpeg/libavutil/integer.c
  FFmpeg/libavutil/intmath.c
  FFmpeg/libavutil/lfg.c
  FFmpeg/libavutil/lls.c
  FFmpeg/libavutil/log.c
  FFmpeg/libavutil/log2_tab.c
  FFmpeg/libavutil/mathematics.c
  FFmpeg/libavutil/mastering_display_metadata.c
  FFmpeg/libavutil/md5.c
  FFmpeg/libavutil/mem.c
  FFmpeg/libavutil/murmur3.c
  FFmpeg/libavutil/opt.c
  FFmpeg/libavutil/parseutils.c
  FFmpeg/libavutil/pixdesc.c
  FFmpeg/libavutil/pixelutils.c
  FFmpeg/libavutil/random_seed.c
  FFmpeg/libavutil/rational.c
  FFmpeg/libavutil/reverse.c
  FFmpeg/libavutil/rc4.c
  FFmpeg/libavutil/ripemd.c
  FFmpeg/libavutil/samplefmt.c
  FFmpeg/libavutil/sha.c
  FFmpeg/libavutil/sha512.c
  FFmpeg/libavutil/slicethread.c
  FFmpeg/libavutil/spherical.c
  FFmpeg/libavutil/stereo3d.c
  FFmpeg/libavutil/threadmessage.c
  FFmpeg/libavutil/time.c
  FFmpeg/libavutil/timecode.c
  FFmpeg/libavutil/tree.c
  FFmpeg/libavutil/twofish.c
  FFmpeg/libavutil/utils.c
  FFmpeg/libavutil/xga_font_data.c
  FFmpeg/libavutil/xtea.c
  FFmpeg/libavutil/tea.c
  FFmpeg/libavutil/tx.c
  FFmpeg/libavutil/tx_float.c
  FFmpeg/libavutil/tx_double.c
  FFmpeg/libavutil/tx_int32.c
  FFmpeg/libavutil/video_enc_params.c
  FFmpeg/libavutil/film_grain_params.c
  FFmpeg/libavutil/side_data.c
  FFmpeg/libavutil/refstruct.c
  FFmpeg/libavutil/container_fifo.c
  FFmpeg/libavutil/float_scalarproduct.c
  FFmpeg/libavutil/timecode_internal.c
  FFmpeg/libavutil/iamf.c
  FFmpeg/libavutil/timestamp.c
)
# x86 platform files
if(XE_TARGET_X86_64)
  target_sources(libavutil PRIVATE
    FFmpeg/libavutil/x86/cpu.c
    FFmpeg/libavutil/x86/fixed_dsp_init.c
    FFmpeg/libavutil/x86/float_dsp_init.c
    FFmpeg/libavutil/x86/imgutils_init.c
    FFmpeg/libavutil/x86/lls_init.c
    FFmpeg/libavutil/x86/tx_float_init.c
  )
endif()
# aarch64 platform files
if(XE_TARGET_AARCH64)
  target_sources(libavutil PRIVATE
    FFmpeg/libavutil/aarch64/cpu.c
  )
  if(XE_HAS_GAS_ASM)
    # NEON init files reference symbols from the .S files, so both must
    # be included together. Requires a GAS-compatible assembler (gcc/clang).
    target_sources(libavutil PRIVATE
      FFmpeg/libavutil/aarch64/float_dsp_init.c
      FFmpeg/libavutil/aarch64/tx_float_init.c
    )
    xe_add_gas_sources(libavutil
      FFmpeg/libavutil/aarch64/float_dsp_neon.S
      FFmpeg/libavutil/aarch64/tx_float_neon.S
    )
  endif()
endif()
ffmpeg_common(libavutil)

# --- libavcodec ---
add_library(libavcodec STATIC
  FFmpeg/libavcodec/ac3_parser.c
  FFmpeg/libavcodec/adts_parser.c
  FFmpeg/libavcodec/allcodecs.c
  FFmpeg/libavcodec/avcodec.c
  FFmpeg/libavcodec/avdct.c
  FFmpeg/libavcodec/packet.c
  FFmpeg/libavcodec/bitstream.c
  FFmpeg/libavcodec/bitstream_filters.c
  FFmpeg/libavcodec/bsf.c
  FFmpeg/libavcodec/codec_desc.c
  FFmpeg/libavcodec/codec_par.c
  FFmpeg/libavcodec/d3d11va.c
  FFmpeg/libavcodec/decode.c
  FFmpeg/libavcodec/dirac.c
  FFmpeg/libavcodec/dv_profile.c
  FFmpeg/libavcodec/encode.c
  FFmpeg/libavcodec/imgconvert.c
  FFmpeg/libavcodec/jni.c
  FFmpeg/libavcodec/mathtables.c
  FFmpeg/libavcodec/mediacodec.c
  FFmpeg/libavcodec/mpeg12framerate.c
  FFmpeg/libavcodec/options.c
  FFmpeg/libavcodec/parser.c
  FFmpeg/libavcodec/parsers.c
  FFmpeg/libavcodec/profiles.c
  FFmpeg/libavcodec/qsv_api.c
  FFmpeg/libavcodec/raw.c
  FFmpeg/libavcodec/utils.c
  FFmpeg/libavcodec/vorbis_parser.c
  FFmpeg/libavcodec/xiph.c
  FFmpeg/libavcodec/dct32_fixed.c
  FFmpeg/libavcodec/dct32_float.c
  FFmpeg/libavcodec/faandct.c
  FFmpeg/libavcodec/faanidct.c
  FFmpeg/libavcodec/fdctdsp.c
  FFmpeg/libavcodec/jfdctfst.c
  FFmpeg/libavcodec/jfdctint.c
  FFmpeg/libavcodec/idctdsp.c
  FFmpeg/libavcodec/simple_idct.c
  FFmpeg/libavcodec/jrevdct.c
  FFmpeg/libavcodec/mpegaudio.c
  FFmpeg/libavcodec/mpegaudiodec_common.c
  FFmpeg/libavcodec/mpegaudiodsp.c
  FFmpeg/libavcodec/mpegaudiodsp_data.c
  FFmpeg/libavcodec/mpegaudiodsp_fixed.c
  FFmpeg/libavcodec/mpegaudiodsp_float.c
  FFmpeg/libavcodec/mpegaudiodecheader.c
  FFmpeg/libavcodec/mpegaudiodata.c
  FFmpeg/libavcodec/sinewin.c
  FFmpeg/libavcodec/wma_freqs.c
  FFmpeg/libavcodec/mpegaudiodec_fixed.c
  FFmpeg/libavcodec/mpegaudiodec_float.c
  FFmpeg/libavcodec/wmaprodec.c
  FFmpeg/libavcodec/wma.c
  FFmpeg/libavcodec/wma_common.c
  FFmpeg/libavcodec/wmadec.c
  FFmpeg/libavcodec/aactab.c
  FFmpeg/libavcodec/mpegaudio_parser.c
  FFmpeg/libavcodec/pthread.c
  FFmpeg/libavcodec/pthread_slice.c
  FFmpeg/libavcodec/pthread_frame.c
  FFmpeg/libavcodec/get_buffer.c
  FFmpeg/libavcodec/vlc.c
  FFmpeg/libavcodec/threadprogress.c
  FFmpeg/libavcodec/bsf/null.c
  FFmpeg/libavcodec/exif.c
  FFmpeg/libavcodec/mpegaudiotabs.c
  FFmpeg/libavcodec/tiff_common.c
)
if(WIN32)
  target_sources(libavcodec PRIVATE FFmpeg/libavcodec/file_open.c)
endif()
# x86 platform files
if(XE_TARGET_X86_64)
  target_sources(libavcodec PRIVATE
    FFmpeg/libavcodec/x86/constants.c
    FFmpeg/libavcodec/x86/fdctdsp_init.c
    FFmpeg/libavcodec/x86/idctdsp_init.c
    FFmpeg/libavcodec/x86/mpegaudiodsp.c
    FFmpeg/libavcodec/x86/fdct.c
  )
endif()
# aarch64 platform files
if(XE_TARGET_AARCH64)
  if(XE_HAS_GAS_ASM)
    target_sources(libavcodec PRIVATE
      FFmpeg/libavcodec/aarch64/idctdsp_init_aarch64.c
      FFmpeg/libavcodec/aarch64/mpegaudiodsp_init.c
    )
    xe_add_gas_sources(libavcodec
      FFmpeg/libavcodec/aarch64/idctdsp_neon.S
      FFmpeg/libavcodec/aarch64/mpegaudiodsp_neon.S
      FFmpeg/libavcodec/aarch64/simple_idct_neon.S
    )
  endif()
endif()
target_include_directories(libavcodec PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/FFmpeg/libavcodec
)
target_link_libraries(libavcodec PRIVATE libavutil)
if(APPLE)
  target_link_libraries(libavcodec PRIVATE iconv)
endif()
ffmpeg_common(libavcodec)

# --- libavformat ---
add_library(libavformat STATIC
  FFmpeg/libavformat/allformats.c
  FFmpeg/libavformat/avio.c
  FFmpeg/libavformat/aviobuf.c
  FFmpeg/libavformat/dump.c
  FFmpeg/libavformat/format.c
  FFmpeg/libavformat/id3v1.c
  FFmpeg/libavformat/id3v2.c
  FFmpeg/libavformat/metadata.c
  FFmpeg/libavformat/mux.c
  FFmpeg/libavformat/options.c
  FFmpeg/libavformat/protocols.c
  FFmpeg/libavformat/riff.c
  FFmpeg/libavformat/sdp.c
  FFmpeg/libavformat/url.c
  FFmpeg/libavformat/utils.c
  FFmpeg/libavformat/riffdec.c
  FFmpeg/libavformat/asfdec_f.c
  FFmpeg/libavformat/asf.c
  FFmpeg/libavformat/asfcrypt.c
  FFmpeg/libavformat/avlanguage.c
  FFmpeg/libavformat/mp3dec.c
  FFmpeg/libavformat/replaygain.c
  FFmpeg/libavformat/file.c
  FFmpeg/libavformat/demux.c
  FFmpeg/libavformat/avformat.c
  FFmpeg/libavformat/seek.c
  FFmpeg/libavformat/demux_utils.c
  FFmpeg/libavformat/urldecode.c
  FFmpeg/libavformat/asf_tags.c
  FFmpeg/libavformat/to_upper4.c
)
if(WIN32)
  target_sources(libavformat PRIVATE FFmpeg/libavformat/file_open.c)
endif()
target_link_libraries(libavformat PRIVATE libavutil libavcodec)
ffmpeg_common(libavformat)

# spirv_to_dxil (Mesa, NIR based SPIR-V to DXIL) is the D3D12 guest shader
# backend. Mesa is meson only, so it is built out of tree by meson and linked
# statically (no runtime DLL, matching the rest of xenia). libspirv_to_dxil.a
# does not bundle its deps, so we link the whole archive set (nir, vtn,
# compiler, dxil_compiler, util, blake3, and on Windows the bundled zlib wrap).
#
# Built on WIN32. The Windows build runs release with /MD to match
# xenia's CRT under an MSVC environment so meson detects cl. Verify the archive
# paths below against the real build output if Mesa is restructured.
if(WIN32)
  # Mesa source tree built for spirv_to_dxil. Defaults to the third_party/mesa
  # submodule (our fork with the barycentric, linking, keep_io_vars and FSI->ROV
  # changes). Repoint this cache variable to build a different Mesa checkout.
  get_filename_component(_xe_mesa_default
      "${CMAKE_SOURCE_DIR}/third_party/mesa" ABSOLUTE)
  set(XENIA_MESA_SOURCE_DIR "${_xe_mesa_default}" CACHE PATH
      "Mesa source tree built for spirv_to_dxil")
  set(_xe_mesa_dir "${XENIA_MESA_SOURCE_DIR}")
  if(NOT EXISTS "${_xe_mesa_dir}/meson.build")
    message(FATAL_ERROR
      "spirv_to_dxil requires a Mesa tree at ${_xe_mesa_dir}. Run: "
      "git submodule update --init third_party/mesa "
      "(or set XENIA_MESA_SOURCE_DIR to your Mesa checkout).")
  endif()
  find_program(XE_MESON_EXECUTABLE meson)
  find_program(XE_NINJA_EXECUTABLE ninja)
  if(NOT XE_MESON_EXECUTABLE OR NOT XE_NINJA_EXECUTABLE)
    message(FATAL_ERROR
      "spirv_to_dxil needs meson>=1.4 and ninja on PATH (and the python mako "
      "module). meson=${XE_MESON_EXECUTABLE} ninja=${XE_NINJA_EXECUTABLE}")
  endif()

  set(_xe_s2d_build "${CMAKE_BINARY_DIR}/third_party/mesa-build")
  set(_xe_s2d_main "${_xe_s2d_build}/src/microsoft/spirv_to_dxil/libspirv_to_dxil.a")
  # Dependency archives, in the layout meson emits under the build tree.
  set(_xe_s2d_deps
    "${_xe_s2d_build}/src/microsoft/compiler/libdxil_compiler.a"
    "${_xe_s2d_build}/src/compiler/spirv/libvtn.a"
    "${_xe_s2d_build}/src/compiler/nir/libnir.a"
    "${_xe_s2d_build}/src/compiler/libcompiler.a"
    "${_xe_s2d_build}/src/util/libmesa_util.a"
    "${_xe_s2d_build}/src/util/libmesa_util_simd.a"
    "${_xe_s2d_build}/src/c11/impl/libmesa_util_c11.a"
    "${_xe_s2d_build}/src/util/blake3/libblake3.a")
  # Windows has no system zlib, so meson builds the bundled wrap subproject.
  # macOS finds the system zlib, so that archive is never emitted there.
  if(WIN32)
    list(APPEND _xe_s2d_deps "${_xe_s2d_build}/subprojects/zlib-1.3.1/libz.a")
  endif()

  # We only static-link the archives above; spirv_to_dxil is a both_libraries()
  # target, so a full "meson compile" also links spirv_to_dxil.dll and builds
  # the spirv2dxil.exe tool, neither of which we consume. Drive ninja against
  # exactly the archive outputs instead (each pulls in its own objects and
  # codegen), skipping the dll link and the exe. Ninja matches target names
  # relative to the build dir, so strip the absolute prefix.
  set(_xe_s2d_ninja_targets "")
  foreach(_xe_ar IN LISTS _xe_s2d_main _xe_s2d_deps)
    file(RELATIVE_PATH _xe_ar_rel "${_xe_s2d_build}" "${_xe_ar}")
    list(APPEND _xe_s2d_ninja_targets "${_xe_ar_rel}")
  endforeach()

  # Meson takes [host_machine] from the build machine, so an off-arch target
  # needs a cross file - which also drops its run-the-sanity-binary probe.
  set(_xe_s2d_host_aarch64 FALSE)
  if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64|arm64")
    set(_xe_s2d_host_aarch64 TRUE)
  endif()
  set(_xe_s2d_cross FALSE)
  if((XE_TARGET_AARCH64 AND NOT _xe_s2d_host_aarch64) OR
     (_xe_s2d_host_aarch64 AND NOT XE_TARGET_AARCH64))
    set(_xe_s2d_cross TRUE)
  endif()
  if(XE_TARGET_AARCH64)
    set(_xe_s2d_cpu "aarch64")
  else()
    set(_xe_s2d_cpu "x86_64")
  endif()

  # Common meson feature toggles. MSVC also needs the CRT selector and a warning
  # override; clang (macOS) rejects the MSVC /wd flag, so those are MSVC only.
  set(_xe_s2d_meson_args
    --default-library=static --buildtype=release
    -Dspirv-to-dxil=true
    -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms=
    -Dglx=disabled -Degl=disabled -Dgbm=disabled
    -Dopengl=false -Dgles1=disabled -Dgles2=disabled
    -Dllvm=disabled -Dvideo-codecs=)
  if(MSVC)
    # /MD matches xenia's CRT; /wd4189 overrides mesa's /we4189: Release strips
    # asserts, leaving assert-only locals that would trip the promoted warning.
    list(APPEND _xe_s2d_meson_args
      -Db_vscrt=md -Dc_args=/wd4189 -Dcpp_args=/wd4189)
    if(NOT CMAKE_HOST_WIN32)
      # Cross-built from Linux with clang-cl (.ci/build-windows-cross.sh): the
      # toolchain's compiler, librarian and linker, with the CRT and SDK
      # directories its toolchain file lists
      set(_xe_s2d_flags "")
      foreach(_xe_dir IN LISTS XE_CLANG_CL_INCLUDES)
        string(APPEND _xe_s2d_flags ", '/imsvc', '${_xe_dir}'")
      endforeach()
      set(_xe_s2d_link_flags "")
      foreach(_xe_dir IN LISTS XE_CLANG_CL_LIBS)
        string(APPEND _xe_s2d_link_flags ", '/libpath:${_xe_dir}'")
      endforeach()
      set(_xe_s2d_cross_file
        "${CMAKE_CURRENT_BINARY_DIR}/mesa-windows-clang-cl-${_xe_s2d_cpu}.ini")
      file(WRITE "${_xe_s2d_cross_file}"
"[binaries]
c = ['${CMAKE_C_COMPILER}', '--target=x86_64-pc-windows-msvc'${_xe_s2d_flags}]
cpp = ['${CMAKE_CXX_COMPILER}', '--target=x86_64-pc-windows-msvc'${_xe_s2d_flags}]
c_ld = '${CMAKE_LINKER}'
cpp_ld = '${CMAKE_LINKER}'
ar = '${CMAKE_AR}'

[built-in options]
c_link_args = ['/manifest:no'${_xe_s2d_link_flags}]
cpp_link_args = ['/manifest:no'${_xe_s2d_link_flags}]

[host_machine]
system = 'windows'
cpu_family = '${_xe_s2d_cpu}'
cpu = '${_xe_s2d_cpu}'
endian = 'little'
")
      list(APPEND _xe_s2d_meson_args --cross-file "${_xe_s2d_cross_file}")
    elseif(_xe_s2d_cross)
      set(_xe_s2d_cross_file
        "${CMAKE_CURRENT_BINARY_DIR}/mesa-windows-cross-${_xe_s2d_cpu}.ini")
      file(WRITE "${_xe_s2d_cross_file}"
"[binaries]
c = 'cl'
cpp = 'cl'
ar = 'lib'

[host_machine]
system = 'windows'
cpu_family = '${_xe_s2d_cpu}'
cpu = '${_xe_s2d_cpu}'
endian = 'little'
")
      list(APPEND _xe_s2d_meson_args --cross-file "${_xe_s2d_cross_file}")
    endif()
  endif()
  if(APPLE)
    # Mesa's meson build sets no deployment target, so its objects get the SDK's
    # and the linker warns about every one of them being newer than what the
    # rest of the tree links against.
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
      list(APPEND _xe_s2d_meson_args
        "-Dc_args=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        "-Dcpp_args=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()

    # Extend the Apple ar/ranlib "has no symbols" quieting we apply to CMake
    # targets (root CMakeLists.txt) to meson's mesa build. Several util/nir
    # sources compile to empty objects on macOS (futex is Linux-only,
    # strndup/u_string/u_atomic/bitscan are libc or inline, nir_validate is
    # NDEBUG-gated). Meson's rule is `ar csr ... && ranlib -c ...` and both
    # steps warn. Meson hardcodes the `csr` args, so route ar through a wrapper
    # that creates with `Scr` (capital S skips the symtab, keeping ar quiet) and
    # override ranlib with -no_warning_for_no_symbols (it writes the symtab) via
    # a native file - the same recipe as CMAKE_*_ARCHIVE_CREATE/FINISH. Written
    # outside _xe_s2d_build, which configure wipes.
    set(_xe_s2d_ar "${CMAKE_CURRENT_BINARY_DIR}/mesa-macos-ar.sh")
    file(WRITE "${_xe_s2d_ar}" [=[#!/bin/sh
# Create static archives with 'Scr' (capital S skips the symbol table, so Apple
# ar stays quiet on zero-symbol objects); the ranlib step writes it afterward.
# Pass meson's `ar --version` linker probe straight through for detection.
case "$1" in
  c*) shift; exec /usr/bin/ar Scr "$@" ;;
  *) exec /usr/bin/ar "$@" ;;
esac
]=])
    file(CHMOD "${_xe_s2d_ar}"
      PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ WORLD_READ)
    set(_xe_s2d_ar_ini
      "ar = '${_xe_s2d_ar}'\nranlib = ['/usr/bin/ranlib', '-no_warning_for_no_symbols']\n")
    set(_xe_s2d_native "${CMAKE_CURRENT_BINARY_DIR}/mesa-macos-native.ini")
    file(WRITE "${_xe_s2d_native}" "[binaries]\n${_xe_s2d_ar_ini}")
    list(APPEND _xe_s2d_meson_args --native-file "${_xe_s2d_native}")

    if(_xe_s2d_cross)
      # meson spells it aarch64, clang -arch spells it arm64.
      string(REPLACE "aarch64" "arm64" _xe_s2d_clang_arch "${_xe_s2d_cpu}")
      set(_xe_s2d_cross_file
        "${CMAKE_CURRENT_BINARY_DIR}/mesa-macos-cross-${_xe_s2d_cpu}.ini")
      file(WRITE "${_xe_s2d_cross_file}"
"[binaries]
c = ['${CMAKE_C_COMPILER}', '-arch', '${_xe_s2d_clang_arch}']
cpp = ['${CMAKE_CXX_COMPILER}', '-arch', '${_xe_s2d_clang_arch}']
objc = ['${CMAKE_C_COMPILER}', '-arch', '${_xe_s2d_clang_arch}']
${_xe_s2d_ar_ini}
[host_machine]
system = 'darwin'
cpu_family = '${_xe_s2d_cpu}'
cpu = '${_xe_s2d_cpu}'
endian = 'little'
")
      list(APPEND _xe_s2d_meson_args --cross-file "${_xe_s2d_cross_file}")
    endif()
  endif()

  include(ExternalProject)
  # Wipe then setup keeps the configure step idempotent across reconfigures.
  ExternalProject_Add(mesa_spirv_to_dxil
    SOURCE_DIR "${_xe_mesa_dir}"
    CONFIGURE_COMMAND
      "${CMAKE_COMMAND}" -E rm -rf "${_xe_s2d_build}"
      COMMAND "${XE_MESON_EXECUTABLE}" setup "${_xe_s2d_build}" "${_xe_mesa_dir}"
        ${_xe_s2d_meson_args}
    BUILD_COMMAND "${XE_NINJA_EXECUTABLE}" -C "${_xe_s2d_build}"
      ${_xe_s2d_ninja_targets}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${_xe_s2d_main}" ${_xe_s2d_deps}
    # Editable source tree, so re-run ninja every build. It is incremental and
    # cheap when nothing changed, and picks up local Mesa edits.
    BUILD_ALWAYS ON
    USES_TERMINAL_BUILD ON)

  add_library(spirv_to_dxil STATIC IMPORTED GLOBAL)
  add_dependencies(spirv_to_dxil mesa_spirv_to_dxil)
  # synchronization/version are the only non-default system libs Mesa needs, and
  # only on Windows (mesa's meson resolves both to nothing off Windows).
  set(_xe_s2d_link ${_xe_s2d_deps})
  if(WIN32)
    list(APPEND _xe_s2d_link synchronization version)
  endif()
  set_target_properties(spirv_to_dxil PROPERTIES
    IMPORTED_LOCATION "${_xe_s2d_main}"
    INTERFACE_INCLUDE_DIRECTORIES
      "${_xe_mesa_dir}/src/microsoft/spirv_to_dxil;${_xe_mesa_dir}/src/microsoft/compiler"
    INTERFACE_LINK_LIBRARIES "${_xe_s2d_link}")
  # Multi-Config evaluates every config, so point each at the same archive.
  foreach(_xe_cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${_xe_cfg}" _xe_cfg_u)
    set_target_properties(spirv_to_dxil PROPERTIES
      IMPORTED_LOCATION_${_xe_cfg_u} "${_xe_s2d_main}")
  endforeach()
endif()
