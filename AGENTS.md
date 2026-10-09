# Rules for coding agents

These apply to any AI agent working in this repository (Claude Code reads them through `CLAUDE.md`). They came out of testing the libretro cores with their testers.

## Branches and history

- The `libretro` branch is never deleted, and nothing that rewrites its history is run on it: no force-push, no rebase, no amending or squashing of commits that are already pushed. It is the fork's only long-lived branch.
- New features and experimental changes go on their own branch, and testers test builds of that branch. When testing is done, the related commits are squashed and merged into `libretro`, so the main branch does not collect commits that were superseded midway.
- A fix that users of the `libretro` builds need before the branch is merged is cherry-picked into `libretro`, in a way that does not break the later merge of the branch.

## Merging upstream Xenia Edge

This repository builds the libretro core and nothing else. The standalone's parts - its UI, packaging, upstream's CI and its contributor docs - were deleted on purpose, and must not come back with a merge. The paths are listed in `.upstream-excluded`.

- Upstream is has207/xenia-edge, branch `edge` (remote `edge`). Only full merges of it, no cherry-picked upstream commits.
- Right after `git merge` of upstream, before resolving anything else, run:
  `git rm -r -q --ignore-unmatch --pathspec-from-file=.upstream-excluded`
  This settles the modify/delete conflicts in favour of the deletion, and it also removes files upstream newly added under those paths, which git would otherwise bring in without any conflict.
- Never resolve a conflict on one of those paths by restoring the file.
- When upstream changes a build file around one of the removed parts, keep them removed and take the rest of the change.
- When something new is deleted for the same reason, add its path to `.upstream-excluded` in the same commit.
- After a merge, `upstream.version` names the upstream version merged; the core reports that version, and a new release is built.

## These files

- Only the developer writes to or deletes `AGENTS.md` and `CLAUDE.md`. An agent does not change them on its own; it proposes the change to the developer instead.

## Code

- Keep comments true. When a change makes a comment describe behaviour that no longer exists, fix or remove the comment in the same commit.
- Look at the big picture, not just the function being changed. For example, resetting the content and closing it have to release the game's resources through the same path; a reset is an unload and a load.
- Stop and join threads the way upstream Xenia Edge does in its own shutdown, not with methods made up for the libretro port. Ad-hoc ones are what cause shutdown and reset bugs.
- Every core option has to be connected to something. Do not add an option the core does not read, and remove one that turns out to do nothing. Option defaults follow the standalone's defaults unless there is a reason, written down beside the option.
- Xenia's own config file is not where the core's settings live. If you need a setting, use a core option (the `.opt` file) and set the cvar from it.

## Testing

- Ask testers for RetroArch's log and Xenia's own log (`system/Xenia-Edge/xenia.log`), and check which build a log came from before drawing conclusions from it.
- Compare with standalone Xenia Edge at the same upstream version before calling something a core bug; when standalone fails the same way, it is upstream's.
- Each Windows CI build uploads its `.pdb` as `<artifact>-symbols`; a crash offset in xenia.log (`xenia_edge_libretro.dll+0x...`) is resolved against the symbols of the same build.


## Libretro pitfalls already hit in the other cores

Each of these was a bug in at least one of the cemu, rpcs3, vita3k or xenia cores. Check new code against them before asking testers.

- Audio: each retro_run hands the frontend exactly one frame's worth (sample rate / declared fps), never "whatever is queued". The game's audio thread waits once about 64 ms is queued; no bigger buffer on the core's side. Several streams (audio ports, clients, a music player) are mixed, not appended one after another, and each is taken in its own format and sample rate.
- Geometry: the size the frame is handed over at goes to the frontend: base geometry at load, SET_GEOMETRY when it changes, SET_SYSTEM_AV_INFO when it would exceed the max. The max has to cover the highest internal resolution.
- Pacing: the guest's vblank follows retro_run, and the core declares the guest's refresh rate; the frontend paces display and audio, the core does not sleep to pace itself.
- Files: everything the core writes goes under system/<core>/ (or saves/); nothing next to the RetroArch executable. The emulator's own log goes there too, and its warnings and errors also go to RetroArch's log.
- No window: the standalone's UI (on-screen keyboard, notifications, message boxes, profile dialogs) has no window in a core. Every path that reaches for the window or its UI thread needs a headless branch that answers the way the user most likely would.
- Unload: never end the process (no exit, no TerminateTitle-style shutdown). Stop and join every thread in upstream's order, and never wait without a timeout on a fence, event or thread the frontend has to drive. No vkQueueWaitIdle/vkDeviceWaitIdle while RetroArch holds its queue lock.
- Libraries that pin themselves (statically linked OpenSSL) keep the core loaded after dlclose; build them so they do not.
- No downloads at run time; data files ship with the core.
