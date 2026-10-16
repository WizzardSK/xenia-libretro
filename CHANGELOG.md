# Changelog of the libretro core

What changed in the libretro core of this fork, newest first. Changes that come with a merge of upstream Xenia Edge are in upstream's own history; a merge is noted here with the upstream commit it brought in.

## Unreleased

- Linux: the core exports only the libretro API (a version script); it exported some sixteen thousand of its own symbols, which could clash with libraries the frontend loads.
- Music Player Volume is 70% by default, as in standalone Xenia Edge; at 100% the music of a game that does not set its own volume could clip (NNshi).
- Emulation Speed is gone: it scaled Xenia's clock, but the game's frames come from RetroArch, so games did not get faster, and slower made the sound break up; RetroArch's Fast-Forward and Slow-Motion change the speed. License Mask is Full by default, so Xbox Live Arcade games run as their full version, not their trial. Frame Wait's description says it can make a game stutter, not slow down (NNshi).
- Core options: every option that takes effect only after Reset, or after the content is loaded again, says "(Restart)" in the name RetroArch shows; most said nothing, and the five that were marked had the mark only in the name RetroArch shows without categories. Mute Audio and Fuzzy Alpha Epsilon are gone: neither did anything (Xenia Edge no longer reads either setting in the core; RetroArch has its own mute, and Audio Output turns the sound off). The Debug category is gone too, and its one option is now Emulation > Log Level (NNshi).
- Wait for the Game's Frame is now called [Libretro] Frame Wait, which says that it is the core's and not a setting of the standalone. Its saved value carries over (NNshi).
- A full dump of an original Xbox disc (a Redump image, with the game partition at 0x18300000) is recognised as a disc image; only images rebuilt to hold the game partition alone were. Original Xbox games run through XeFu, the console's own emulator, as in upstream. No one has opened a full dump with it yet.
- Video > Widescreen is now Aspect Ratio, 16:9 (default) or 4:3, and with 4:3 the picture is shown at 4:3: RetroArch was told 16:9 whatever the option said, so a game with a native 4:3 mode came out stretched. A change while a game runs is passed on at once (NNshi).
- Dialog Notifications (Emulation): the notification that shows a system dialog's title and the answer the core gave can be turned off; it stays on by default (NNshi).
- A game opening the achievements list, the gamer card or the profile editor no longer crashes the core: those dialogs have no window to be shown in and are dismissed at once (NNshi, Dead or Alive 5 Ultimate's Achievements menu).
- Linux: the core no longer links libX11 and libxcb, and builds without the X11 and Wayland headers. They came with the standalone's Vulkan surface creation, which the core never does: the frontend owns the window. The core now loads where those libraries are absent.
- The libretro buildbot builds the Windows core too: its MSVC is too old for Xenia Edge, so the job cross-builds on Linux with clang-cl against the MSVC runtime and Windows SDK (`.ci/build-windows-cross.sh`). A GitHub workflow replays the job. The Windows builds from GitHub Actions are still made with MSVC.
- The core option texts can be translated through libretro's Crowdin project: the options header is on the standard template, with the `intl/` scripts and the upload and sync workflows. All texts are English until the first sync.

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
