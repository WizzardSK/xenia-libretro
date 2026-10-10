/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Per-game data: the frame rate unlock list and the patch manager
 */

#include "libretro_gamedb.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/config.h"
#include "xenia/patcher/patch_db.h"
#include "xenia/patcher/patch_file_editor.h"
#include "xenia/vfs/iso_metadata.h"
#include "xenia/vfs/stfs_metadata.h"
#include "xenia/vfs/xbe_metadata.h"
#include "xenia/vfs/xex_metadata.h"
#include "xenia/vfs/zar_metadata.h"

// The unlock list, embedded by libretro/CMakeLists.txt
extern const unsigned char xenia_libretro_framerate_unlock[];
extern const size_t xenia_libretro_framerate_unlock_size;

namespace gamedb {

namespace fs = std::filesystem;

static const char kUnlockFile[] = "xenia_framerate_unlock.txt";

static void install(const fs::path &path, const unsigned char *data,
                    size_t size) {
  std::error_code ec;
  if (fs::exists(path, ec)) return;
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(data),
            static_cast<std::streamsize>(size));
  if (out) XELOGI("Wrote {}", path.string());
}

void install_files(const fs::path &root) {
  std::error_code ec;
  fs::create_directories(root, ec);
  install(root / kUnlockFile, xenia_libretro_framerate_unlock,
          xenia_libretro_framerate_unlock_size);
}

uint32_t title_id_of(const fs::path &game) {
  // Standalone's own: reads the ISO, XEX, STFS, ZAR or XBE header, and drops
  // the previous title's game config values
  uint32_t title_id = config::LoadGameConfigForFile(game);
  if (title_id) return title_id;
  // It goes by the extension, and an XBLA package renamed to .xex (to get
  // past RetroArch's file filter) is read as an XEX and gives nothing: every
  // kind then, whatever the name (Minecraft, NNshi)
  if (auto m = xe::vfs::ExtractStfsMetadata(game)) return m->title_id;
  if (auto m = xe::vfs::ExtractXexMetadata(game)) return m->title_id;
  if (auto m = xe::vfs::ExtractZarMetadata(game)) return m->title_id;
  if (auto m = xe::vfs::ExtractIsoMetadata(game)) return m->title_id;
  if (auto m = xe::vfs::ExtractXbeMetadata(game)) return m->title_id;
  return 0;
}

// ---- Unlock Framerate -----------------------------------------------------

Unlock unlock_for(const fs::path &root, uint32_t title_id) {
  if (!title_id) return Unlock::kNone;
  std::ifstream in(root / kUnlockFile);
  std::string line;
  Unlock section = Unlock::kNone;
  while (std::getline(in, line)) {
    if (auto hash = line.find('#'); hash != std::string::npos)
      line.erase(hash);
    while (!line.empty() && isspace(static_cast<unsigned char>(line.back())))
      line.pop_back();
    size_t start = 0;
    while (start < line.size() &&
           isspace(static_cast<unsigned char>(line[start])))
      ++start;
    line.erase(0, start);
    if (line.empty()) continue;
    if (line[0] == '[') {
      std::string name;
      for (char c : line)
        if (isalnum(static_cast<unsigned char>(c)))
          name += static_cast<char>(tolower(static_cast<unsigned char>(c)));
      section = name == "30fps"      ? Unlock::k30to60
                : name == "60fps"    ? Unlock::k60to120
                : name == "uncapped" ? Unlock::kUncapped
                                     : Unlock::kNone;
      continue;
    }
    if (section == Unlock::kNone) continue;
    if (strtoul(line.c_str(), nullptr, 16) == title_id) return section;
  }
  return Unlock::kNone;
}

// ---- Patch Manager --------------------------------------------------------

static std::vector<xe::patcher::PatchSourceFile> s_files;
static std::vector<PatchOption> s_options;

static uint32_t fnv1a(const std::string &text) {
  uint32_t h = 2166136261u;
  for (unsigned char c : text) {
    h ^= c;
    h *= 16777619u;
  }
  return h;
}

const std::vector<PatchOption> &patches() { return s_options; }

void forget_patches() {
  s_files.clear();
  s_options.clear();
}

const std::vector<PatchOption> &find_patches(uint32_t title_id,
                                             std::optional<uint64_t> hash) {
  forget_patches();
  for (auto &file : xe::patcher::EnumerateBundledPatchesForTitle(title_id)) {
    const auto &hashes = file.entry.hashes;
    if (hash && std::find(hashes.begin(), hashes.end(), *hash) == hashes.end())
      continue;
    s_files.push_back(std::move(file));
  }
  for (size_t f = 0; f < s_files.size(); ++f) {
    // As the editor reads them, which is the order SetEnabled counts in
    xe::patcher::PatchFileEditor editor(s_files[f].toml_content, fs::path());
    const auto &list = editor.patches();
    for (size_t i = 0; i < list.size(); ++i) {
      PatchOption option;
      char key[64];
      // Keyed by file and patch name, so an update that adds or reorders
      // patches keeps each one's setting
      // "zpatch": sorted after every other option in the .opt file (NNshi)
      snprintf(key, sizeof(key), "xenia_zpatch_%08x",
               fnv1a(s_files[f].filename + "/" + list[i].name));
      option.key = key;
      option.label = list[i].name;
      if (s_files.size() > 1)
        option.label = xe::patcher::PatchDisplayName(s_files[f]) + ": " +
                       option.label;
      option.info = list[i].description;
      if (!list[i].author.empty())
        option.info += (option.info.empty() ? "By " : " By ") + list[i].author;
      option.file = f;
      option.index = i;
      option.default_on = list[i].is_enabled;
      s_options.push_back(std::move(option));
    }
  }
  if (!s_options.empty())
    XELOGI("Patch Manager: {} patches in {} file(s) for {:08X}",
           s_options.size(), s_files.size(), title_id);
  return s_options;
}

void write_patch_files_impl(const fs::path &patches_dir,
                            const std::vector<bool> &values) {
  for (size_t f = 0; f < s_files.size(); ++f) {
    xe::patcher::PatchFileEditor editor(s_files[f].toml_content,
                                        patches_dir / s_files[f].filename);
    for (size_t o = 0; o < s_options.size() && o < values.size(); ++o) {
      if (s_options[o].file != f) continue;
      editor.SetEnabled(s_options[o].index, values[o]);
    }
  }
}

}  // namespace gamedb
