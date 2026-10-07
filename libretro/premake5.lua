project_root = ".."
include(project_root.."/tools/build")

local ui_src = project_root.."/src/xenia/ui"

--------------------------------------------------------------------------------
-- xenia-ui-headless: same as xenia-ui but without Qt, ImGui, or moc files.
-- Mirrors platform_files.lua: include all, remove all platform suffixes,
-- then add back only the current platform via filter.
--------------------------------------------------------------------------------
group("src")
project("xenia-ui-headless")
  uuid("d0407c25-aaaa-4444-846c-82c46fbd9fa2")
  kind("StaticLib")
  language("C++")
  links({
    "xenia-base",
  })
  -- Include all base files
  files({
    ui_src.."/*.h",
    ui_src.."/*.c",
    ui_src.."/*.cc",
    ui_src.."/*.inc",
  })
  -- Remove ALL platform-specific files first (same as platform_files.lua)
  removefiles({
    ui_src.."/**_main.cc",
    ui_src.."/**_test.cc",
    ui_src.."/**_posix.h",  ui_src.."/**_posix.cc",
    ui_src.."/**_linux.h",  ui_src.."/**_linux.cc",
    ui_src.."/**_gnulinux.h", ui_src.."/**_gnulinux.cc",
    ui_src.."/**_x11.h",   ui_src.."/**_x11.cc",
    ui_src.."/**_gtk.h",   ui_src.."/**_gtk.cc",
    ui_src.."/**_android.h", ui_src.."/**_android.cc",
    ui_src.."/**_mac.h",   ui_src.."/**_mac.cc",
    ui_src.."/**_win.h",   ui_src.."/**_win.cc",
  })
  -- Remove Qt, moc, demo files
  removefiles({
    ui_src.."/*_qt.*",
    ui_src.."/moc_*",
    ui_src.."/*_demo.cc",
    ui_src.."/windowed_app_main_qt.cc",
    ui_src.."/ui_resources_qrc.cpp",
    -- Remove ImGui dialogs that include emulator_window.h (Qt dependency)
    ui_src.."/imgui_performance_dialog.cc",
    ui_src.."/imgui_postprocessing_dialog.cc",
    ui_src.."/imgui_xmp_dialog.cc",
    ui_src.."/profile_dialogs.cc",
  })
  -- Add back current platform files
  filter("platforms:Windows")
    files({ ui_src.."/*_win.h", ui_src.."/*_win.cc" })
    links({ "dwmapi", "dxgi", "winmm" })
  filter("platforms:Linux")
    files({
      ui_src.."/*_posix.h", ui_src.."/*_posix.cc",
      ui_src.."/*_linux.h", ui_src.."/*_linux.cc",
      ui_src.."/*_gnulinux.h", ui_src.."/*_gnulinux.cc",
      ui_src.."/*_x11.h", ui_src.."/*_x11.cc",
      ui_src.."/*_gtk.h", ui_src.."/*_gtk.cc",
    })
    links({ "xcb", "X11", "X11-xcb", "fontconfig" })
  filter({})

-- D3D12 exists on Windows only
if os.istarget("windows") then
--------------------------------------------------------------------------------
-- xenia-ui-d3d12-headless: same as xenia-ui-d3d12 but links headless UI.
--------------------------------------------------------------------------------
project("xenia-ui-d3d12-headless")
  uuid("f93dc1a8-aaaa-4444-b0fc-ae3eefbe836b")
  kind("StaticLib")
  language("C++")
  defines({ "XENIA_LIBRETRO=1" })
  links({
    "xenia-base",
    "xenia-ui-headless",
  })
  files({
    ui_src.."/d3d12/*.h",
    ui_src.."/d3d12/*.cc",
    ui_src.."/shaders/bytecode/d3d12_5_1/*.h",
  })
  -- Remove platform suffixes then add back current
  removefiles({
    ui_src.."/d3d12/**_win.h",  ui_src.."/d3d12/**_win.cc",
    ui_src.."/d3d12/**_linux.h", ui_src.."/d3d12/**_linux.cc",
    ui_src.."/d3d12/**_android.h", ui_src.."/d3d12/**_android.cc",
    ui_src.."/d3d12/*_demo.cc",
  })
  filter("platforms:Windows")
    files({ ui_src.."/d3d12/*_win.h", ui_src.."/d3d12/*_win.cc" })
  filter({})

end

--------------------------------------------------------------------------------
-- xenia-ui-vulkan-headless: same as xenia-ui-vulkan but links headless UI.
--------------------------------------------------------------------------------
project("xenia-ui-vulkan-headless")
  uuid("4933d81e-aaaa-4444-b104-3c0eb9dc2f00")
  kind("StaticLib")
  language("C++")
  defines({ "XENIA_LIBRETRO=1" })
  links({
    "xenia-base",
    "xenia-ui-headless",
  })
  includedirs({
    project_root.."/third_party/Vulkan-Headers/include",
    project_root.."/third_party/glslang",
  })
  filter("platforms:Windows")
    includedirs({ "$(VULKAN_SDK)/Include" })
    libdirs({ "$(VULKAN_SDK)/Lib" })
    links({ "SPIRV-Tools-opt.lib", "SPIRV-Tools.lib" })
  filter("platforms:Linux")
    links({ "SPIRV-Tools-opt", "SPIRV-Tools" })
  filter({})
  files({
    ui_src.."/vulkan/*.h",
    ui_src.."/vulkan/*.cc",
    ui_src.."/vulkan/functions/*.h",
    ui_src.."/vulkan/functions/*.cc",
    ui_src.."/shaders/bytecode/vulkan_spirv/*.h",
  })
  -- Remove platform suffixes then add back current
  removefiles({
    ui_src.."/vulkan/**_win.h",  ui_src.."/vulkan/**_win.cc",
    ui_src.."/vulkan/**_linux.h", ui_src.."/vulkan/**_linux.cc",
    ui_src.."/vulkan/**_android.h", ui_src.."/vulkan/**_android.cc",
    ui_src.."/vulkan/*_demo.cc",
  })
  filter("platforms:Windows")
    files({ ui_src.."/vulkan/*_win.h", ui_src.."/vulkan/*_win.cc" })
  filter("platforms:Linux")
    files({
      ui_src.."/vulkan/*_linux.h", ui_src.."/vulkan/*_linux.cc",
      ui_src.."/vulkan/*_posix.h", ui_src.."/vulkan/*_posix.cc",
    })
  filter({})

--------------------------------------------------------------------------------
-- xenia-gpu-headless: same as xenia-gpu but links headless UI.
--------------------------------------------------------------------------------
local gpu_src = project_root.."/src/xenia/gpu"

project("xenia-gpu-headless")
  uuid("0e8d3370-aaaa-4444-a2e8-39ebbcdf9b17")
  kind("StaticLib")
  language("C++")
  links({
    "dxbc",
    "fmt",
    "glslang-spirv",
    "snappy",
    "xenia-base",
    "xenia-ui-headless",
    "xxhash",
  })
  includedirs({
    project_root.."/third_party/Vulkan-Headers/include",
    project_root.."/third_party/glslang",
  })
  filter("platforms:Windows")
    includedirs({ "$(VULKAN_SDK)/Include" })
  filter({})
  -- Use same files as xenia-gpu with platform filtering
  files({
    gpu_src.."/*.h", gpu_src.."/*.cc", gpu_src.."/*.inc",
  })
  removefiles({
    gpu_src.."/**_main.cc", gpu_src.."/**_test.cc",
    gpu_src.."/**_posix.h", gpu_src.."/**_posix.cc",
    gpu_src.."/**_linux.h", gpu_src.."/**_linux.cc",
    gpu_src.."/**_gnulinux.h", gpu_src.."/**_gnulinux.cc",
    gpu_src.."/**_x11.h", gpu_src.."/**_x11.cc",
    gpu_src.."/**_gtk.h", gpu_src.."/**_gtk.cc",
    gpu_src.."/**_android.h", gpu_src.."/**_android.cc",
    gpu_src.."/**_mac.h", gpu_src.."/**_mac.cc",
    gpu_src.."/**_win.h", gpu_src.."/**_win.cc",
  })
  filter("platforms:Windows")
    files({ gpu_src.."/*_win.h", gpu_src.."/*_win.cc" })
  filter("platforms:Linux")
    files({
      gpu_src.."/*_posix.h", gpu_src.."/*_posix.cc",
      gpu_src.."/*_linux.h", gpu_src.."/*_linux.cc",
    })
  filter({})

-- D3D12 exists on Windows only
if os.istarget("windows") then
--------------------------------------------------------------------------------
-- xenia-gpu-d3d12-headless
--------------------------------------------------------------------------------
project("xenia-gpu-d3d12-headless")
  uuid("c057eae4-aaaa-4444-9a69-1fe07b735c49")
  kind("StaticLib")
  language("C++")
  links({
    "fmt",
    "xenia-base",
    "xenia-gpu-headless",
    "xenia-ui-headless",
    "xenia-ui-d3d12-headless",
    "xxhash",
  })
  files({
    gpu_src.."/d3d12/*.h",
    gpu_src.."/d3d12/*.cc",
    gpu_src.."/shaders/bytecode/d3d12_5_1/*.h",
  })
  removefiles({
    gpu_src.."/d3d12/**_win.h",  gpu_src.."/d3d12/**_win.cc",
    gpu_src.."/d3d12/**_linux.h", gpu_src.."/d3d12/**_linux.cc",
    gpu_src.."/d3d12/**_android.h", gpu_src.."/d3d12/**_android.cc",
    gpu_src.."/d3d12/*_demo.cc",
    gpu_src.."/d3d12/**_main.cc", gpu_src.."/d3d12/**_test.cc",
  })
  filter("platforms:Windows")
    files({ gpu_src.."/d3d12/*_win.h", gpu_src.."/d3d12/*_win.cc" })
  filter({})

end

--------------------------------------------------------------------------------
-- xenia-gpu-vulkan-headless
--------------------------------------------------------------------------------
project("xenia-gpu-vulkan-headless")
  uuid("717590b4-aaaa-4444-8f23-0624e87d6cca")
  kind("StaticLib")
  language("C++")
  links({
    "fmt",
    "glslang-spirv",
    "xenia-base",
    "xenia-gpu-headless",
    "xenia-ui-headless",
    "xenia-ui-vulkan-headless",
    "xxhash",
  })
  includedirs({
    project_root.."/third_party/Vulkan-Headers/include",
    project_root.."/third_party/glslang",
  })
  filter("platforms:Windows")
    includedirs({ "$(VULKAN_SDK)/Include" })
  filter({})
  files({
    gpu_src.."/vulkan/*.h",
    gpu_src.."/vulkan/*.cc",
    gpu_src.."/shaders/bytecode/vulkan_spirv/*.h",
  })
  removefiles({
    gpu_src.."/vulkan/**_win.h",  gpu_src.."/vulkan/**_win.cc",
    gpu_src.."/vulkan/**_linux.h", gpu_src.."/vulkan/**_linux.cc",
    gpu_src.."/vulkan/**_android.h", gpu_src.."/vulkan/**_android.cc",
    gpu_src.."/vulkan/*_demo.cc",
    gpu_src.."/vulkan/**_main.cc", gpu_src.."/vulkan/**_test.cc",
  })
  filter("platforms:Windows")
    files({ gpu_src.."/vulkan/*_win.h", gpu_src.."/vulkan/*_win.cc" })
  filter("platforms:Linux")
    files({ gpu_src.."/vulkan/*_linux.h", gpu_src.."/vulkan/*_linux.cc" })
  filter({})

--------------------------------------------------------------------------------
-- xenia-gpu-null-headless
--------------------------------------------------------------------------------
project("xenia-gpu-null-headless")
  uuid("42FCA0B3-aaaa-4444-95E9-07D297013BE4")
  kind("StaticLib")
  language("C++")
  links({
    "xenia-base",
    "xenia-gpu-headless",
    "xenia-ui-headless",
    "xenia-ui-vulkan-headless",
    "xxhash",
  })
  includedirs({
    project_root.."/third_party/Vulkan-Headers/include",
  })
  files({
    gpu_src.."/null/*.h",
    gpu_src.."/null/*.cc",
  })
  removefiles({
    gpu_src.."/null/**_win.h",  gpu_src.."/null/**_win.cc",
    gpu_src.."/null/**_linux.h", gpu_src.."/null/**_linux.cc",
    gpu_src.."/null/**_android.h", gpu_src.."/null/**_android.cc",
  })
  filter("platforms:Windows")
    files({ gpu_src.."/null/*_win.h", gpu_src.."/null/*_win.cc" })
  filter({})


--------------------------------------------------------------------------------
-- Patch xenia-kernel: replace Qt-dependent UI files with stubs for libretro.
--------------------------------------------------------------------------------
project("xenia-kernel")
  removefiles({
    project_root.."/src/xenia/kernel/xam/ui/*.cc",
    project_root.."/src/xenia/kernel/xam/ui/*.h",
    project_root.."/src/xenia/kernel/xam/xam_ui.cc",
    project_root.."/src/xenia/kernel/xam/xam_ui.h",
    project_root.."/src/xenia/kernel/xam/xam_nui.cc",
    project_root.."/src/xenia/kernel/xam/xam_nui.h",
  })
  files({
    project_root.."/src/xenia/kernel/xam/xam_ui_stub.cc",
    project_root.."/src/xenia/kernel/xam/xam_nui_stub.cc",
    project_root.."/src/xenia/kernel/xam/ui/disc_swap_ui_stub.cc",
  })

--------------------------------------------------------------------------------
-- xenia-libretro: the headless libretro core.
--------------------------------------------------------------------------------
group("src")
project("xenia-libretro")
  uuid("a1b2c3d4-e5f6-7890-abcd-ef1234567890")
  kind("SharedLib")
  language("C++")
  targetname("xenia_edge_libretro")

  files({
    "libretro.cpp",
    "libretro_audio_driver.cc",
    "libretro_graphics_system.cc",
    "libretro_hid.cc",
    "libretro_vk_presenter.cc",
    "libretro_d3d12_presenter.cc",
    "libretro_headless_stubs.cc",
    "libretro.h",
    "libretro_core_options.h",
    "libretro_audio_driver.h",
    "libretro_graphics_system.h",
    "libretro_hid.h",
    "libretro_vulkan.h",
    "libretro_d3d12.h",
    "libretro_vk_presenter.h",
    "libretro_d3d12_presenter.h",
  })
  filter("platforms:not Windows")
    removefiles({ "libretro_d3d12_presenter.cc" })
  filter({})

  includedirs({
    ".",
    project_root.."/third_party/Vulkan-Headers/include",
  })
  filter("platforms:Windows")
    includedirs({ "$(VULKAN_SDK)/Include" })
  filter({})

  -- Xenia libraries ??? all headless variants, zero Qt/ImGui transitive deps
  links({
    "xenia-apu",
    "xenia-apu-nop",
    "xenia-base",
    "xenia-core",
    "xenia-cpu",
    "xenia-gpu-headless",
    "xenia-gpu-vulkan-headless",
    "xenia-gpu-null-headless",
    "xenia-hid",
    "xenia-hid-nop",
    "xenia-kernel",
    "xenia-patcher",
    "xenia-ui-headless",
    "xenia-ui-vulkan-headless",
    "xenia-vfs",
  })

  -- Third-party deps (imgui is static C++ lib with zero DLL deps,
  -- needed by xenia-kernel's gamercard_ui/passcode_ui objects)
  links({
    "aes_128",
    "capstone",
    "fmt",
    "dxbc",
    "glslang-spirv",
    "imgui",
    "libavcodec",
    "libavutil",
    "mspack",
    "snappy",
    "xxhash",
  })

  defines({
    "XBYAK_NO_OP_NAMES",
    "XBYAK_ENABLE_OMITTED_OPERAND",
    "XENIA_LIBRETRO=1",
  })

  apu_transitive_deps()

  filter("architecture:x86_64")
    links({
      "xenia-cpu-backend-x64",
    })
  filter("architecture:ARM64")
    links({
      "xenia-cpu-backend-a64",
    })

  filter("platforms:Windows")
    links({
      "xenia-gpu-d3d12-headless",
      "xenia-ui-d3d12-headless",
      "d3d12",
      "dxgi",
      "vulkan-1",
      "d3dcompiler",
    })
    libdirs({ "$(VULKAN_SDK)/Lib" })
    defines({
      "XENIA_LIBRETRO=1",
    })

  filter("platforms:Linux")
    links({
      "X11",
      "xcb",
      "X11-xcb",
    })
    defines({
      "XENIA_LIBRETRO=1",
    })

  -- The NDK's Vulkan loader, and SPIRV-Tools built for the ABI by vcpkg, put
  -- where SPIRV_TOOLS_ANDROID points by CI (ndk-build expands both variables)
  filter("platforms:Android-*")
    links({ "vulkan", "log", "android" })
    linkoptions({
      "$(SPIRV_TOOLS_ANDROID)/$(TARGET_ARCH_ABI)/libSPIRV-Tools-opt.a",
      "$(SPIRV_TOOLS_ANDROID)/$(TARGET_ARCH_ABI)/libSPIRV-Tools.a",
    })

  filter({})
