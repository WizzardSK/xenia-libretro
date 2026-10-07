add_library(xenia-ui-vulkan STATIC)
xe_platform_sources(xenia-ui-vulkan ${CMAKE_CURRENT_SOURCE_DIR})
xe_platform_sources(xenia-ui-vulkan ${CMAKE_CURRENT_SOURCE_DIR}/functions)
xe_shader_rules_spirv(xenia-ui-vulkan ${CMAKE_CURRENT_SOURCE_DIR}/../shaders)
xe_shader_rules_slang(xenia-ui-vulkan ${CMAKE_CURRENT_SOURCE_DIR}/../shaders
                      TARGET spirv)
target_include_directories(xenia-ui-vulkan PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
  ${PROJECT_SOURCE_DIR}/third_party/glslang
)
target_link_libraries(xenia-ui-vulkan PUBLIC xenia-base xenia-ui)
if(APPLE)
  target_link_libraries(xenia-ui-vulkan PUBLIC MoltenVK)
endif()
xe_target_defaults(xenia-ui-vulkan)

if(XENIA_BUILD_MISC)
  add_executable(xenia-ui-window-vulkan-demo
    ${CMAKE_CURRENT_SOURCE_DIR}/../window_demo.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_window_demo.cc
  )
  xe_windowed_app_main(xenia-ui-window-vulkan-demo)
  if(WIN32)
    set_target_properties(xenia-ui-window-vulkan-demo PROPERTIES WIN32_EXECUTABLE TRUE)
  endif()
  target_include_directories(xenia-ui-window-vulkan-demo PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
  )
  target_link_libraries(xenia-ui-window-vulkan-demo PRIVATE
    xenia-apu xenia-apu-nop xenia-base xenia-core xenia-cpu
    xenia-gpu xenia-hid xenia-hid-nop xenia-hid-portal
    xenia-kernel xenia-patcher xenia-ui xenia-ui-vulkan xenia-vfs
    aes_128 capstone fmt glslang-spirv imgui libavcodec libavutil mspack
    snappy xxhash
  )
  if(XE_TARGET_X86_64)
    target_link_libraries(xenia-ui-window-vulkan-demo PRIVATE xenia-cpu-backend-x64)
  endif()
  if(XE_TARGET_AARCH64)
    target_link_libraries(xenia-ui-window-vulkan-demo PRIVATE xenia-cpu-backend-a64)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(xenia-ui-window-vulkan-demo PRIVATE X11 xcb X11-xcb xenia-apu-sdl)
  endif()
  xe_target_defaults(xenia-ui-window-vulkan-demo)
endif()
