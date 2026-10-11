# Changelog of the libretro core

What changed in the libretro core of this fork, newest first. Changes that come with a merge of upstream Xenia Edge are in upstream's own history; a merge is noted here with the upstream commit it brought in.

## 2026-10-10

### Video

- The Vulkan device is shared with RetroArch: the core makes it through RetroArch's context negotiation, Xenia renders on it and each frame goes GPU to GPU into RetroArch's image in the same frame, with no readback and upload. GPU load is on par with standalone.
- Wait for the Game's Frame (Graphics, off by default): removes one frame of input lag at the possible cost of the game slowing down.
- One guest vblank per retro_run, so frames are no longer doubled by two 60 Hz clocks drifting apart; the guest pauses when RetroArch stops running the core.
- The largest picture RetroArch is told to expect follows Draw Resolution Scale.
- Turnip (Adreno): frames are no longer black below their top 16 rows.

### Audio

- RetroArch gets one frame of audio per run, and the guest's audio waits once 64 ms is queued.
- Audio clients and the media player are mixed, each in its own format and rate; menu music is no longer static. An Audio Channels option (Surround 5.1).

### Games and data

- Per-game data: Unlock Framerate from NNshi's frame rate unlock list.
- Multi-disc games: an `.m3u` list and RetroArch's Disc Control, with the game's request for a disc answered from the list.
- Core options for the settings that game-optimized configurations use, and a Patch Manager; every option's value is logged at load.
- The standard drives are mounted as the standalone does; a missing `cache:` crashed Forza Horizon 2 at its first race.
- Keystrokes from the RetroPad, for titles that run their menus on them.

### Without a window

- Message boxes are answered the way a player would go on: an offline choice first, else the game's own default when it is not a way back, else the first button that is not Cancel, Back or No. What was asked and answered goes to the log and on screen.
- The keyboard and sign-in dialogs are answered too; the profile's name comes from `system/Xenia-Edge/xbl_profilename.txt`.
- No crash at an achievement popup.

### Other

- The core's version names upstream's commit and this repository's.
- lz4 is linked only where it is installed.

## 2026-10-08

- Xenia's log goes to `system/Xenia-Edge/xenia.log`, and its errors and warnings to RetroArch's log as well; a crash names the module it happened in.
- Everything the core keeps is in `system/Xenia-Edge/` (content, cache, D3D12), nothing beside RetroArch's executable. A core info file.
- Games no longer start to a black screen: graphics, audio and input are set up before the title starts.
- A Graphics API option: Vulkan, and on Windows Direct3D 12, shown only where it can be used. `dxil.dll` is carried inside the core.
- Games see the controllers: RetroPad port N is user N.
- Closing content stops the title's threads in order and no longer takes RetroArch with it.
- A profile is signed in for games that ask for a signed-in user, and made when there is none.

## 2026-10-07

- The core builds from upstream's CMake build, at upstream Xenia Edge 8dc77b0, and reports upstream's version. The repository holds the core only: the standalone app, its UI and packaging are removed (`.upstream-excluded`).
- Upstream's XAM dialogs are answered at once where there is no window.
- Linux ARM64 and Android builds, next to Linux x86_64 and Windows; macOS on Apple Silicon.

## February 2026

- libretro core integration, by danprice142: the original port this fork builds on.
