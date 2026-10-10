/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Multi-disc games: the disc list (an .m3u, or the one disc loaded), the
 * frontend's disk control interface, and the answer to the game's disc swap
 * (XamSwapDisc) when there is no window to ask in.
 */

#ifndef LIBRETRO_DISC_H
#define LIBRETRO_DISC_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "libretro.h"

namespace disc {

// Gives the frontend the disk control interface; from retro_set_environment.
void set_environment(retro_environment_t environ_cb);

// The content loaded: an .m3u's discs, or the one disc. Returns the disc to
// start (the first, or the one the frontend last had in), empty when an .m3u
// lists none.
std::string load(const char* path);
void unload();
// Answers a game still waiting for a disc with the one in, before the
// emulator stops, and every request after it the same way until resume (or
// the next load): nothing may wait on the player while it shuts down.
void release();
void resume();

// Messages for the frontend's screen.
void set_notice(std::function<void(const std::string&)> notice);

// Emulator::DiscRequest: the game asks for disc `disc_number`. The list's
// disc of that number is put in at once; else, or when the game turned the
// disc down (an "ERROR:" message), the answer waits for the player to put a
// disc in through Disc Control.
void request(uint8_t disc_number, const std::string& message,
             std::function<void(std::filesystem::path)> answer);

}  // namespace disc

#endif  // LIBRETRO_DISC_H
