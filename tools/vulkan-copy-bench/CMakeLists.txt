# Not referenced from the root CMakeLists - add_subdirectory this to build the
# benchmark when it is needed again.
add_executable(xenia-vulkan-copy-bench
  ${CMAKE_CURRENT_SOURCE_DIR}/vulkan_copy_bench_main.cc
)
target_include_directories(xenia-vulkan-copy-bench PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
)
if(NOT WIN32)
  target_link_libraries(xenia-vulkan-copy-bench PRIVATE ${CMAKE_DL_LIBS})
endif()
xe_target_defaults(xenia-vulkan-copy-bench)
