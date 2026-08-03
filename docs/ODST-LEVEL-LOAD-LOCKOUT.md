# ODST level-load lockout - captured event (2026-08-03)

Long-standing open bug, user-confirmed as predating all vehicle work
("sometimes levels wont load since the start"): after some number of
load/exit cycles, ODST stops loading levels - the loading screen appears and
bounces back to the main menu until MCC is restarted.

Until now no log of the real ODST event existed (see the project memory note
`odst-cross-title-kick-to-menu`), which is why four earlier theories could
only be refuted rather than replaced. **This session captured it.**

Preserved evidence (ignored `out/`, so it cannot be lost to a log rotation):
`out/analysis/odst-level-load-lockout/`
- `halo3xr-40bd330-steam-20260803-lockout.log` - the session containing three
  consecutive failed loads AND three successful ones
- `halo3xr-1d6dcb6-steam-prev.log` - the prior session for comparison
- `halomccvr-at-lockout.cfg` - the exact configuration in force

## The event

Steam edition, build `40bd330`. A Y+B pause and Save & Quit at 07:59:07, then
three failed ODST level loads at 07:59:57, 08:00:15 and 08:01:08.

## The discriminator (success and failure in the same log)

**Successful load** (07:53:53, 07:57:57, 07:59:10): after the camera core
installs, the `ODST camera WAIT` poll logs repeatedly with `tailValid`
alternating 1 -> 0 -> 1 within 400-900 ms - the engine's camera is ticking -
then `ODST camera bring-up: stable stock camera detected` and it arms.

**Failed load** (07:59:58, 08:00:16, 08:01:09): exactly ONE `WAIT` line
(`tailValid=1`), then silence. The engine camera tail never changes again, no
camera heartbeat is ever published (the teardown diagnostic reports
`heartbeat age = 18446744073709551615` - i.e. never), and 1.7-3 s later all six
game modules reload, which is the bounce back to the menu.

**The level therefore dies before the game's own camera loop ever runs.** The
mod installs cleanly - every signature resolves, no error - logs once, and is
completely silent for the whole window.

## Two more theories killed by this log

5. **The ODST first-person vehicle work causes it.** No. The O2 sampler, probe
   and object census emitted ZERO lines during all three failures: they only
   run on an active camera, and there was never one. The user also reports the
   bug predates the feature entirely.
6. **`ODST camera presentation: detach primed during title load` is the
   trigger.** No. That same line precedes the SUCCESSFUL load at 07:53:44.

(Theories 1-4 - Reach preflight, SAFEFRAME copy-on-write, per-generation
leaks, and a stranded module pin - were refuted earlier; see the memory note.)

## What is still unsettled

The mod's hooks ARE installed into `halo3odst.dll` during the failing load, so
"passive in the log" is not proof of innocence. The one test that has never
been run is the control: launch MCC WITHOUT the VR launcher so the mod never
injects, then repeat the same load/quit cycle. That settles blame in one
session with no build. The user has chosen to park this and continue the
vehicle work, so it stays open.
