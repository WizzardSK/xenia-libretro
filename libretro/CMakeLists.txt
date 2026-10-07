# xenia_edge_libretro: the libretro core, a shared library linked from the
# emulator's static libraries, without the standalone app's UI.

add_library(xenia-libretro SHARED
  libretro.cpp
  libretro_audio_driver.cc
  libretro_graphics_system.cc
  libretro_headless_stubs.cc
  libretro_hid.cc
  libretro_vk_presenter.cc
)
if(WIN32)
  target_sources(xenia-libretro PRIVATE libretro_d3d12_presenter.cc)
endif()

# RetroArch's names: xenia_edge_libretro.so / .dll / .dylib, and
# xenia_edge_libretro_android.so on Android
set(_xe_libretro_name xenia_edge_libretro)
if(ANDROID)
  set(_xe_libretro_name xenia_edge_libretro_android)
endif()
set_target_properties(xenia-libretro PROPERTIES
  OUTPUT_NAME ${_xe_libretro_name}
  PREFIX ""
)

# As xe_target_defaults, without -Werror: the core's own code is not held
# to it
target_include_directories(xenia-libretro PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/third_party
  ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
)
# The core's version is the xenia-edge release it is merged from (its releases
# are named by commit), from upstream.version, updated with each merge
file(STRINGS ${PROJECT_SOURCE_DIR}/upstream.version _xe_upstream_version LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${PROJECT_SOURCE_DIR}/upstream.version)

target_compile_definitions(xenia-libretro PRIVATE
  XENIA_LIBRETRO_VERSION="${_xe_upstream_version}"
  XBYAK_NO_OP_NAMES
  XBYAK_ENABLE_OMITTED_OPERAND
  XENIA_LIBRETRO=1
)

set(_xe_libretro_libs
  xenia-apu
  xenia-apu-nop
  xenia-base
  xenia-core
  xenia-cpu
  xenia-gpu
  xenia-gpu-null
  xenia-gpu-vulkan
  xenia-hid
  xenia-hid-nop
  xenia-kernel
  xenia-patcher
  xenia-ui
  xenia-ui-vulkan
  xenia-vfs
  aes_128
  capstone
  fmt
  glslang-spirv
  imgui
  libavcodec
  libavutil
  mspack
  snappy
  xxhash
)
if(XE_TARGET_X86_64)
  list(APPEND _xe_libretro_libs xenia-cpu-backend-x64)
elseif(XE_TARGET_AARCH64)
  list(APPEND _xe_libretro_libs xenia-cpu-backend-a64)
endif()
if(WIN32)
  list(APPEND _xe_libretro_libs xenia-gpu-d3d12 xenia-ui-d3d12)
endif()

# On Linux the static libraries' circular references are resolved by the
# link group the top-level CMakeLists.txt wraps every link in
target_link_libraries(xenia-libretro PRIVATE ${_xe_libretro_libs})

if(WIN32)
  target_link_libraries(xenia-libretro PRIVATE d3d12 dxgi)
elseif(ANDROID)
  target_link_libraries(xenia-libretro PRIVATE android log)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(xenia-libretro PRIVATE X11 xcb X11-xcb)
  # Only the libretro API out of the core, and its own symbols bound inside
  # it, so the frontend's libraries don't stand in for them
  target_link_options(xenia-libretro PRIVATE -Wl,-Bsymbolic -Wl,--no-undefined)
endif()
