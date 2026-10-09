# Rules for coding agents

These apply to any AI agent working in this repository (Claude Code reads them through `CLAUDE.md`).

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
