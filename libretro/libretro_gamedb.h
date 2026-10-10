/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Per-game data: the frame rate unlock list and the patch manager (NNshi,
 * 9 Oct 2026)
 */

#ifndef LIBRETRO_GAMEDB_H
#define LIBRETRO_GAMEDB_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gamedb {

// xenia_framerate_unlock.txt in root, written from the copy inside the core
// when missing and never overwritten: it is the user's to edit
void install_files(const std::filesystem::path &root);

// The title ID of a game file before it is launched, 0 when it cannot be read
uint32_t title_id_of(const std::filesystem::path &game);

// Unlock Framerate: what root's xenia_framerate_unlock.txt says for a title
enum class Unlock { kNone, k30to60, k60to120, kUncapped };
Unlock unlock_for(const std::filesystem::path &root, uint32_t title_id);

// Patch Manager: the patches bundled in the core for a title and module hash,
// one core option each
struct PatchOption {
  std::string key;    // core option key, stable across patch updates
  std::string label;  // the patch's name
  std::string info;   // its description and author
  size_t file;        // index into patch_files()
  size_t index;       // the patch's index in that file
  bool default_on;
};
const std::vector<PatchOption> &find_patches(uint32_t title_id,
                                             std::optional<uint64_t> hash);
const std::vector<PatchOption> &patches();
void forget_patches();
// Writes each matched patch file to patches_dir with is_enabled set from
// enabled(option)
template <typename F>
void write_patch_files(const std::filesystem::path &patches_dir, F enabled);
void write_patch_files_impl(const std::filesystem::path &patches_dir,
                            const std::vector<bool> &values);

template <typename F>
void write_patch_files(const std::filesystem::path &patches_dir, F enabled) {
  std::vector<bool> values;
  for (const auto &option : patches()) values.push_back(enabled(option));
  write_patch_files_impl(patches_dir, values);
}

}  // namespace gamedb

#endif  // LIBRETRO_GAMEDB_H
