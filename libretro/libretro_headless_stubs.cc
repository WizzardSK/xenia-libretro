/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Stubs for what the standalone app's wxWidgets UI provides, which the core
 * is built without: file dialogs and the downloads of missing runtimes.
 */

#include <filesystem>
#include <memory>

#include "xenia/base/platform.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/redist_installer_wx.h"

namespace xe {
namespace ui {

#if !XE_PLATFORM_ANDROID
// No file dialogs in a libretro core (Android has its own, file_picker_android).
std::unique_ptr<FilePicker> FilePicker::Create() { return nullptr; }
#endif

// The standalone app offers to download these; the core uses what is there.
bool EnsureShaderCompilerRuntime(const std::filesystem::path&) { return false; }
bool EnsureAgilityRuntime(const std::filesystem::path&) { return false; }
bool EnsureDebugLayer(const std::filesystem::path&) { return false; }
bool EnsureVulkanLoader(const std::filesystem::path&) { return false; }
bool EnsureVCRuntime() { return false; }

}  // namespace ui
}  // namespace xe
