/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Multi-disc games (NNshi): an .m3u lists a game's discs, and the frontend's
 * Disc Control swaps them, as in the Beetle cores. The 360 asks for a disc
 * by number (XamSwapDisc), so the list's disc of that number goes in on its
 * own; the player only has to pick one when the list has none or the game
 * turned it down.
 */

#include "libretro_disc.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

#include "xenia/base/filesystem.h"

namespace disc {
namespace {

std::mutex s_mutex;
std::vector<std::string> s_paths;
size_t s_index = 0;
bool s_ejected = false;
// The frontend's disc from last time (set_initial_image), for load.
size_t s_initial_index = 0;
std::string s_initial_path;
// The game's wait for a disc, answered when the tray closes.
std::function<void(std::filesystem::path)> s_pending;
std::function<void(const std::string&)> s_notice;
// Between release and resume: the emulator is stopping.
bool s_closing = false;

void notice(const std::string& text) {
  if (s_notice) s_notice(text);
}

std::string label_of(const std::string& path) {
  return xe::path_to_utf8(xe::to_path(path).stem());
}

// What the game is given when it can't wait any longer: the disc in, else
// the first.
std::string current_path_locked() {
  if (s_index < s_paths.size() && !s_paths[s_index].empty())
    return s_paths[s_index];
  return s_paths.empty() ? std::string() : s_paths.front();
}

bool RETRO_CALLCONV set_eject_state(bool ejected) {
  std::function<void(std::filesystem::path)> answer;
  std::string path;
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_ejected = ejected;
    if (!ejected && s_pending && s_index < s_paths.size() &&
        !s_paths[s_index].empty()) {
      answer = std::move(s_pending);
      s_pending = nullptr;
      path = s_paths[s_index];
    }
  }
  if (answer) {
    notice("Disc " + label_of(path) + " inserted");
    answer(xe::to_path(path));
  }
  return true;
}

bool RETRO_CALLCONV get_eject_state() {
  std::lock_guard<std::mutex> lock(s_mutex);
  return s_ejected;
}

unsigned RETRO_CALLCONV get_image_index() {
  std::lock_guard<std::mutex> lock(s_mutex);
  return unsigned(s_index);
}

bool RETRO_CALLCONV set_image_index(unsigned index) {
  std::lock_guard<std::mutex> lock(s_mutex);
  if (index > s_paths.size()) return false;
  s_index = index;
  return true;
}

unsigned RETRO_CALLCONV get_num_images() {
  std::lock_guard<std::mutex> lock(s_mutex);
  return unsigned(s_paths.size());
}

bool RETRO_CALLCONV replace_image_index(unsigned index,
                                        const struct retro_game_info* info) {
  std::lock_guard<std::mutex> lock(s_mutex);
  if (index >= s_paths.size()) return false;
  if (!info || !info->path) {
    s_paths.erase(s_paths.begin() + index);
    if (s_index > index || s_index > s_paths.size()) --s_index;
  } else {
    s_paths[index] = info->path;
  }
  return true;
}

bool RETRO_CALLCONV add_image_index() {
  std::lock_guard<std::mutex> lock(s_mutex);
  s_paths.emplace_back();
  return true;
}

bool RETRO_CALLCONV set_initial_image(unsigned index, const char* path) {
  std::lock_guard<std::mutex> lock(s_mutex);
  s_initial_index = index;
  s_initial_path = path ? path : "";
  return true;
}

bool RETRO_CALLCONV get_image_path(unsigned index, char* path, size_t len) {
  std::lock_guard<std::mutex> lock(s_mutex);
  if (index >= s_paths.size() || s_paths[index].empty() || !len) return false;
  std::strncpy(path, s_paths[index].c_str(), len - 1);
  path[len - 1] = '\0';
  return true;
}

bool RETRO_CALLCONV get_image_label(unsigned index, char* label, size_t len) {
  std::lock_guard<std::mutex> lock(s_mutex);
  if (index >= s_paths.size() || s_paths[index].empty() || !len) return false;
  std::strncpy(label, label_of(s_paths[index]).c_str(), len - 1);
  label[len - 1] = '\0';
  return true;
}

retro_disk_control_ext_callback s_callback = {
    set_eject_state,     get_eject_state,     get_image_index,
    set_image_index,     get_num_images,      replace_image_index,
    add_image_index,     set_initial_image,   get_image_path,
    get_image_label,
};

bool has_extension(const std::string& path, const char* ext) {
  const size_t n = std::strlen(ext);
  if (path.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(path[path.size() - n + i])) !=
        ext[i])
      return false;
  }
  return true;
}

}  // namespace

void set_environment(retro_environment_t environ_cb) {
  if (!environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE,
                  &s_callback)) {
    static retro_disk_control_callback basic = {
        set_eject_state, get_eject_state,     get_image_index,
        set_image_index, get_num_images,      replace_image_index,
        add_image_index,
    };
    environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &basic);
  }
}

std::string load(const char* path) {
  std::vector<std::string> paths;
  if (has_extension(path, ".m3u")) {
    // One disc a line, relative to the .m3u's folder; # starts a comment.
    const std::filesystem::path m3u = xe::to_path(std::string(path));
    std::ifstream file(m3u);
    std::string line;
    while (std::getline(file, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                               line.back() == '\t'))
        line.pop_back();
      size_t start = line.find_first_not_of(" \t");
      if (start == std::string::npos || line[start] == '#') continue;
      if (start == 0 && line.size() >= 3 &&
          std::memcmp(line.data(), "\xEF\xBB\xBF", 3) == 0)
        start = 3;
      std::filesystem::path disc = xe::to_path(line.substr(start));
      if (disc.is_relative()) disc = m3u.parent_path() / disc;
      paths.push_back(xe::path_to_utf8(disc));
    }
  } else {
    paths.push_back(path);
  }

  std::lock_guard<std::mutex> lock(s_mutex);
  s_paths = std::move(paths);
  s_index = 0;
  s_ejected = false;
  s_pending = nullptr;
  s_closing = false;
  // The disc the frontend had in last time, if it's still that one.
  if (s_initial_index < s_paths.size() &&
      s_paths[s_initial_index] == s_initial_path)
    s_index = s_initial_index;
  s_initial_path.clear();
  s_initial_index = 0;
  return s_paths.empty() ? std::string() : s_paths[s_index];
}

void release() {
  std::function<void(std::filesystem::path)> answer;
  std::string path;
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    answer = std::move(s_pending);
    s_pending = nullptr;
    path = current_path_locked();
    s_closing = true;
  }
  // A game still waiting for a disc gets the one in, so its thread can
  // finish before the emulator shuts down.
  if (answer) answer(xe::to_path(path));
}

void resume() {
  std::lock_guard<std::mutex> lock(s_mutex);
  s_closing = false;
}

void unload() {
  release();
  std::lock_guard<std::mutex> lock(s_mutex);
  s_paths.clear();
  s_index = 0;
  s_ejected = false;
}

void set_notice(std::function<void(const std::string&)> notice_fn) {
  std::lock_guard<std::mutex> lock(s_mutex);
  s_notice = std::move(notice_fn);
}

void request(uint8_t disc_number, const std::string& message,
             std::function<void(std::filesystem::path)> answer) {
  const bool refused = message.find("ERROR:") != std::string::npos;
  std::string path;
  bool closing = false;
  {
    std::lock_guard<std::mutex> lock(s_mutex);
    closing = s_closing;
    if (closing) {
      path = current_path_locked();
    } else if (!refused && disc_number >= 1 && disc_number <= s_paths.size() &&
               !s_paths[disc_number - 1].empty()) {
      s_index = disc_number - 1;
      s_ejected = false;
      path = s_paths[s_index];
    } else {
      s_pending = std::move(answer);
    }
  }
  if (closing) {
    answer(xe::to_path(path));
    return;
  }
  if (!path.empty()) {
    notice("Disc " + std::to_string(disc_number) + " inserted (" +
           label_of(path) + ")");
    answer(xe::to_path(path));
    return;
  }
  notice(refused ? "Wrong disc - pick disc " + std::to_string(disc_number) +
                       " in Disc Control and close the tray"
                 : "The game asks for disc " + std::to_string(disc_number) +
                       " - pick it in Disc Control and close the tray");
}

}  // namespace disc
