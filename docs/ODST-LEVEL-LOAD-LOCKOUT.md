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

## SECOND CAPTURE (2026-08-06): the discriminator is a STALE CAMERA

Build `50ddf56`, Steam, VirtualDesktopXR 1.0.10, Quest 3 at 90 Hz. Preserved as
`out/analysis/odst-level-load-lockout/halo3xr-50ddf56-steam-20260806-stale-camera-discriminator.log`
(SHA-256 `AA172A4D1265146B2ADE4C9E22D8F7C69040CA7612AA5672ABEA55D017D31A47`)
with its configuration beside it. **The user also states the bug is not
ODST-specific**, which the finding below explains.

This session contains one SUCCESSFUL load and three FAILURES, and the readiness
data separates them cleanly.

**Successful load, 06:27:13 - the FIRST level of the session.** The camera
readiness probe reads the four-slot player-view array and finds it genuinely
empty: `tail=[0,0,0,0,0,0,0,0] mode=0x00000000 blend=0.000000 fov=0.000000
ref=0.000000 offset=0.000000 near=0.000000 far=0.000`. The core correctly logs
`ODST camera preflight passed; waiting for the proven slot-0/user-0 ordinary
camera mode` and holds for **7.6 s** (06:27:13.901 -> 06:27:21.532) before
installing at 06:27:22.478. It arms normally and plays.

**Failing loads, 06:29:08, 06:29:24, 06:29:41.** The same probe reads
`fov=1.641310 ref=1.345714 near=0.007812 far=10240.000` - the exact values of
the level just exited. The readiness check is satisfied **0.6 s** after the
title appears, install completes **1.5 s** after it, i.e. during the loading
screen, and then the engine's camera never ticks. All six modules reload
1.3-1.5 s later, which is the bounce to the menu.

**Why "one WAIT line then silence" is proof, not absence of data.** The
`ODST camera WAIT` line is logged on CHANGE of its four-bit state signature, or
at most every 2000 ms (`game.cpp`, `sig == lastSig && now - lastLogMs < 2000`).
In the successful load that signature changes every 50-250 ms. In each failing
load it produced exactly one line and the level died 1.3-1.5 s later - inside
the 2 s re-log floor, so the only way a second line could have been suppressed
is if the state never changed. **The engine's camera was frozen.**

**The defect this exposes.** "Non-zero" was being used as a proxy for "alive".
The player-view array is module-global data that survives a level unload, so
after the first level of a session it always looks populated. The check can
therefore only ever work on the very first load.

**Not ODST-specific, confirmed in code.** `InstallHook` (Halo 3) has no
readiness check at all - it installs the moment `halo3.dll` is detected and its
signature resolves. Reach's "one-second fresh-camera interval" is an ARMING
debounce applied after installation, not an install gate. ODST was the only
title with any install-time check, and that check reads stale data. All three
titles could therefore hook into a loading screen.

**Still not proven.** This is a strong measured correlation from one session,
not causation. The mod remains passive in the log during the failures, so
"we hooked early and broke the load" and "the load was already dying, and a
dying load looks exactly like this" both still fit. The no-mod control run
below remains the only test that settles blame outright.

## The user's pattern, and what the log says about it (2026-08-06)

After reading the finding above the user reported the shape of the bug from
play: **"it seems to happen if you load back into the same game, not when
switching games."** The same capture explains both halves, and this is
independent support for the stale-camera mechanism rather than a restatement of
it - the theory predicts exactly this pattern.

`halo3odst.dll` appears in **16 of the 17** module snapshots in the session.
The single snapshot without it (`halo3.dll,halo1.dll,halo2.dll`, 06:29:54) is
the moment the user finally left ODST for good. So:

- **Loading back into the same game.** The title module is never unloaded
  across a Save & Quit -> menu -> load cycle. Its `.data`, including the
  four-slot player-view array, is CONTINUOUS - it still holds the level you
  just left. Our readiness check sees populated memory, calls it ready, and
  hooks into the loading screen. Bug.
- **Switching games.** The module genuinely unloads. When it comes back, its
  `.data` is restored from the file image and the array is zeroed, so the
  readiness check correctly waits for the engine to fill it in. No bug.

**This also exposed a hole in the first version of the fix.** Module continuity
means the OUTGOING level's camera can still be ticking when the new title
generation is detected: in this same capture the Save & Quit at 06:28:28 left
the camera changing for roughly three more seconds (06:28:31.442 -> 06:28:32.354
shows `tailValid` alternating normally). A gate that opened on the first
observed change would have accepted those dying ticks as the new level's
liveness and installed into the loading screen anyway - reproducing the bug
while appearing to guard against it.

## The fix under test (2026-08-06)

`PlayerViewLivenessGate` in `src/dll/game.cpp`. No title installs any hook until
the engine's own player-view memory has produced the sequence a real level load
must produce: **frozen, then ticking**. It interprets nothing about the camera -
it fingerprints the whole of player view 0 - so one implementation serves all
three titles.

"Frozen then ticking" rather than merely "changed" is what closes the
same-game hole above. A loading screen leaves the previous level's camera
untouched, which supplies the frozen half; the new level's first frame supplies
the tick. A cold zeroed array is frozen too, so a first load still works. A
title that was already running when observation began never freezes, so a run
of 12 consecutive changing samples (about 0.6 s at the 50 ms worker poll, which
no loading screen produces) is separately accepted as "already running".

All three engines build their four player views with the same constructor
shape; only the view stride differs, and the array base is that constructor's
own `lea rbx,[rip+disp32]`:

| Title | Constructor | View stride | Player-view array |
| --- | --- | --- | --- |
| Halo 3 | `halo3.dll+0x68BC` | `0x2820` | `+0x2D2F680` |
| ODST | `halo3odst.dll+0x6B60` | `0x2810` | `+0x2D73590` |
| Reach | `haloreach.dll+0x6210` | `0x0A40` | `+0x29F2B90` |

Each module contains one unrelated four-slot constructor of the identical
shape, so the stride bytes are part of the signature; with them pinned each
title matches exactly once. **The method is validated against a known-good
result**: decoding ODST's array this way yields `+0x2D73590`, which is exactly
the `array=2D73590` the accepted runtime logs already report. Reach's `0xA40`
is the same four-player-view stride already documented in
`docs/REACH-SIGNATURE-EVIDENCE.md`.

Failure modes are deliberate and loud. If the array cannot be proven the gate
opens immediately and logs that it did - identical to pre-gate behavior. If the
camera has not moved after 12 s (comfortably above the 7.6 s a cold first load
took) the gate opens anyway and logs that it did, so VR can never be denied
outright by an unforeseen case.

What the next headset session settles: if the load/quit/load cycle stops
bouncing, the early install was the cause and this is the fix. If it still
bounces, early installation is exonerated and the no-mod control run below is
the remaining test.

## What is still unsettled

The mod's hooks ARE installed into `halo3odst.dll` during the failing load, so
"passive in the log" is not proof of innocence. The one test that has never
been run is the control: launch MCC WITHOUT the VR launcher so the mod never
injects, then repeat the same load/quit cycle. That settles blame in one
session with no build. The user has chosen to park this and continue the
vehicle work, so it stays open.
