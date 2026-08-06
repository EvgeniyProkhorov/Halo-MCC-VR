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

## THE GATE WAS A ONE-SHOT (2026-08-06, build `b934b61`)

The user tested `b934b61` and reported it unfixed, adding the crucial detail
that it "happens on every game if you save and quit and try to load a new
level" and that switching to another game clears it - so the main menu should
have nothing of ours loaded.

Preserved:
`out/analysis/odst-level-load-lockout/halo3xr-b934b61-steam-20260806-gate-was-one-shot.log`
(SHA-256 `6CF2C52E74FB3C2962C099EE5A7B7A97466A40BD5F377A28BDB2866D2C1DE9E1`).

**The gate works, and it never ran for the failures.** On ODST's first load it
behaved exactly as designed: held for 7.2 s across four `holding install` lines
and then logged `the engine's camera was frozen and has now started ticking
(7172 ms)` at 07:03:51.555. After that:

| Time | Event | Gate output |
| --- | --- | --- |
| 07:03:44 | first ODST load | 4 holds, then opened at 7172 ms |
| 07:04:07 | Save & Quit teardown | - |
| 07:04:09 | install (1.7 s later) | **none** |
| 07:04:27 | load attempt -> BOUNCED | **none**, install at 1.56 s |
| 07:04:46 | load attempt -> BOUNCED | **none**, install at 1.55 s |

`AllowsInstall` only reset its observation when the title generation or module
base changed. **The ODST generation does not change across a Save & Quit ->
menu -> load cycle**, so `m_live` stayed set from the first load and every later
install went straight through - at the same 1.5 s as before the gate existed.

So the 2026-08-06 stale-camera theory is **not refuted by this run; it was
never tested**. The failing loads were never gated. The defect is in the gate's
own lifetime, not in its mechanism, and the mechanism is proven to work when it
runs.

**Fix:** `PlayerViewLivenessGate::Rearm()`, called wherever a title's hooks come
out - Halo 3 at the title-boundary reset, ODST after `RemoveOdstCameraCore()`,
Reach in `RemoveReachCameraCore()`. Every install must now re-earn the
frozen-then-ticking proof, which is the user's own requirement expressed in
code: on the main menu nothing of ours is installed and the gate is shut. The
re-arm and the live generation number are both logged, so absent gate output
can never again be mistaken for a gate that passed.

## THEORY 7 REFUTED: the load bounces with ZERO hooks installed (build `b70141d`)

Preserved:
`out/analysis/odst-level-load-lockout/halo3xr-b70141d-steam-20260806-bounced-with-ZERO-hooks-installed.log`
(SHA-256 `BDAE266590580C5B567D01AD29C71662D7B210230FA1BC01FCCD18EAE4FD4E15`).

With `Rearm()` in place the gate finally ran on every load, and the log proves
it: `re-armed after teardown` before each one, then a hold, then an open. It
behaved exactly as designed. And the bug still happened.

**The decisive window, 07:14:44 to 07:15:07:**

```
07:14:44.077  ODST camera teardown complete (title exit)
07:14:44.077  ODST level-load gate: re-armed after teardown
07:15:05.308  Title adapter: detected Halo 3: ODST -> Runtime mode: loading
07:15:05.358  ODST level-load gate: holding install ... camera still=1
              (no ODST camera install line - NOTHING was hooked)
07:15:07.275  Runtime mode: loading -> unsupported      <-- bounced to the menu
```

The level bounced with the ODST camera core **entirely absent**: torn down at
07:14:44, gate holding from 07:15:05, no install of any kind for that
generation. **Installing during the loading screen is not what bounces the
load.** The stale-camera finding remains a true and useful description of the
engine's memory, but it is not the cause of this bug.

The user's report matches: "kicks me out once now then works the second time."

**What the successful retry adds.** At 07:15:17 the load worked, and its
readiness line reads `tail=[0,0,0,0,0,0,0,0] ... fov=0.000000` - the camera
array is genuinely ZEROED, which only happens when `halo3odst.dll` is really
unloaded and re-mapped. So the observed pattern is: a load into a *reused*
title module bounces; the bounce is followed by a real module reload; the next
load then works. That is also why switching games "fixes" it - a game switch
forces the same reload.

**What this narrows it to.** Our per-title camera cores are exonerated for the
failing load. What remains live during it is everything global and always-on:
the D3D11 present/swapchain hooks, the XInput hooks, and the
`fit_desktop_window` backbuffer forcing plus its `GetSystemMetrics` /
`GetMonitorInfo` spoofing - which in this session was forcing MCC's backbuffer
to 3262x2352. Two cheap tests separate those from MCC itself, and neither needs
a build:

1. **No-mod control run.** Launch MCC without the VR launcher, repeat
   load/quit/load. Still the only test that settles blame outright.
2. **`fit_desktop_window = 0`.** Repeat the same cycle with the backbuffer
   forcing and metric spoofing disabled. This is the largest thing we do to a
   title while it loads and it is one config line.

**Status of the gate.** ~~Behaviorally reverted~~ **SUPERSEDED - the refutation
misread the log; see the next section.** The gate is re-enabled with corrected
thresholds.

**Separately: Halo 3 was never detected in that session.** The user reported
that hopping to Halo 3 did not hook. The log contains no
`Title adapter: detected supported title Halo 3` line and **no `Halo 3
level-load gate` line at all**, so `AllowsInstall` was never reached and the
gate cannot be what blocked it - the adapter never made Halo 3 the active
title. After the last Reach level exited at 07:21:18 the runtime stayed in
`loading` with Reach detected for the final 64 seconds until MCC closed. That
line does appear normally in older logs (`halo3xr-1d6dcb6-steam-prev.log`), so
this is not a permanent break. It needs a session that actually reaches Halo 3.

## THE THEORY-7 REFUTATION WAS MISREAD (2026-08-06, same log, full timeline)

A second pass over the SAME preserved b70141d log shows the "bounced with zero
hooks" window was not a bounce at all:

- The REAL bounce was at **07:14:42.948** - `Runtime mode: gameplay -> loading
  -> unsupported` with the module list churning - **2.4 s after** the gate's
  12-sample "already running" fast path opened at 07:14:38.580 and installed
  the ten-hook core at 07:14:39.509, ~1.5 s into a `paused -> loading`
  transition. Hooks were installed and armed when the level died.
- The 07:15:05-07:15:07 window the refutation cited is the **menu's module
  churn during the automatic reload that follows a bounce**: ODST was
  transiently re-detected at 07:15:05, the gate held (camera still), and the
  poll flipped to ambiguous at 07:15:07 as more modules loaded. Nothing
  bounced there - that same load continued, froze for **11,766 ms**, ticked,
  installed at 07:15:17-07:15:25 and played fine for two minutes. Reading
  that adapter flap as a second, hook-free bounce is what produced the
  refutation.

**The corrected tally across all preserved captures.** Every bounced load was
preceded by an install into the load window, and every install that waited for
the real frozen-then-ticking transition survived:

| Capture | Install path | Result |
| --- | --- | --- |
| 50ddf56, 3 loads | ungated, 1.5 s in | all 3 BOUNCED |
| b934b61, 07:04:29 + 07:04:47 | one-shot-bypassed, 1.5 s in | both BOUNCED |
| b70141d, 07:14:38 / 07:18:32 / 07:19:26 | 12-sample fast path, 0.5 s in | all 3 BOUNCED ~2.5 s later |
| b934b61 07:03:51 (7172 ms), b70141d 07:14:13 (94 ms resume), 07:15:17 (11766 ms), 07:19:08 (7250 ms), Reach 07:19:41 (5921 ms) | frozen -> ticking | all 5 PLAYED |

The 12-sample fast path (0.6 s of change) is exactly the hole the second
capture predicted: the outgoing level's dying ticks run 3-4 s after a
Save & Quit, so "0.6 s of uninterrupted change" is routinely satisfied by the
camera of the level being LEFT.

**The corrected gate (this candidate).** Decision core extracted to
`src/common/level_load_gate_logic.h` with offline tests in
`tests/core_tests.cpp` replaying these captures:

- FROZEN is satisfied by a single still sample (revised same day: candidate
  19757f6 shipped a 6-sample / 300 ms minimum as theoretical hardening, and
  the user rejected it as far too slow to enter 3D - the captured
  pause-resume witnesses exactly ONE still sample, so any higher minimum
  sends every resume down the 6 s already-running path. No preserved capture
  has ever shown a frozen-then-tick misfire with the 1-sample rule; in every
  bounced load, sampling began inside the dying ticks and no stillness was
  witnessed at all).
- ALREADY RUNNING now requires 120 consecutive changed samples (6 s) - above
  the measured dying-tick window with margin. Cost: a mid-play adapter flap
  re-engages VR in 6 s instead of 0.6 s.
- The unconditional 12 s timeout is REMOVED. The successful retry froze for
  11,766 ms - within 300 ms of that timeout converting it into an
  install-during-load. Menus hold indefinitely (correct - the flat screen
  layer still shows in the headset), and both legitimate open paths cover
  every real level entry and every genuinely-running title.

**What this does NOT explain, kept open.** The Reach kick at 07:21:14 in the
same log came 84 s after a clean frozen-then-ticking install and ~3 s after a
vehicle seat entry, mid-gameplay with no load transition of ours - install
timing cannot account for it. The earlier Reach kick (07:05:18, prev log) came
14.7 s after a fast-path install. So installing into a load is proven
sufficient to bounce a load; it is not proven to be the ONLY trigger of the
kick-to-menu family. If a mid-play kick recurs on this build, its log now
carries the full gate history for that generation.

**REACH MID-PLAY KICK LEAD (2026-08-06, second capture + crash).** Preserved:
`halo3xr-a42e869-steam-20260806-reach-pause-seat-reentry-kick-and-crash.log`
(SHA-256 `06123F50...F795BEF`). Both captured Reach kicks sit on the identical
sequence: pause -> resume -> ~2 s -> a REPLAYED "exact occupied-seat entry"
(recenter count increments for a seat never left, "local player seated;
native LT/X active" re-logs) -> the engine bails to the menu 1.5-3 s later
(07:21:07-14 and 08:36:10-18). Cause of the replay: a natively paused Reach
reports no player unit, so the vehicle samplers published Unknown, the
debounce left Vehicle, the seat lease was RESTORED (an engine-visible
seat-flags write), the exact-seat latch cleared, and resume replayed the full
entry. Fixed in `382e4eb`: both samplers hold every published seat state
across a native pause (the R-V26 suspension rule). Whether removing the
replayed entry also removes the kick is the open headset question. The same
session shows the post-kick dead state: after teardown the display worker
proof waits on engine-fields forever in menus/theatre, so nothing can
reinstall until a real mission load - and MCC itself crashed 2 s after a
pause press in that state (dump 08:37:39, WER unread - archive needs
elevation).

**The session end (the "Halo 3 did not hook" report).** After the 07:21:14
kick, the menu's module churn transiently narrowed to a unique
`haloreach.dll` and Reach was re-detected at 07:21:18; the runtime correctly
sat in `loading` with the gate holding. From then until MCC closed at
07:22:22, `halo3.dll` was never loaded again (the adapter polls every 50 ms
and logs every module-list change; there were none), so no Halo 3 mission
reached gameplay in that session. Nothing of ours blocked it; a session that
actually enters a Halo 3 mission on this build will gate and install normally.

## What is still unsettled

The mod's hooks ARE installed into `halo3odst.dll` during the failing load, so
"passive in the log" is not proof of innocence. The one test that has never
been run is the control: launch MCC WITHOUT the VR launcher so the mod never
injects, then repeat the same load/quit cycle. That settles blame in one
session with no build. The user has chosen to park this and continue the
vehicle work, so it stays open.
