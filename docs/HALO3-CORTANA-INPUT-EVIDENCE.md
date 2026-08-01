# Halo 3 Cortana / Gravemind input-ownership evidence

Status: implementation candidate; offline build/test complete, headset result
required before acceptance.

## Halo 3 behavior being matched

Halo 3's Cortana and Gravemind channel scripts deliberately ramp **all player
input** down to 15 percent, allow slow movement, then ramp input back to normal.
The scripts do not request forced gaze or an authored cinematic camera. This is
the input behavior this candidate matches; it does not make a broader claim
about the channel's visual render effects.

The VR parity requirement for this interval is therefore:

- keep Halo's left-stick input so the title applies its own 15 percent walking;
- keep tracked HMD look, stereo rendering, and the OpenXR session alive;
- temporarily stop VR-authored right-stick aim and direct snap/smooth yaw, both
  of which otherwise bypass or fight the title's input ownership;
- restore those two VR turn authors only after Halo's native restore ramp ends.

The Cortana overlay/FOV distortion is a separate render-effect problem and is
not part of this control candidate.

## Official H3EK findings

Evidence source:

- `N:\SteamLibrary\steamapps\common\H3EK\halo3_tag_test.exe`
- version `1.40.0.0`
- SHA-256
  `59A78F2C96034D7CEB5D710505B2B36813AA141FC81A083E3F952973DBCE4602`

In `data/globals/global_scripts.hsc`:

- `gs_cortana_header`, lines 802-830, calls
  `player_control_scale_all_input 0.15 0.5` at line 813;
- `gs_cortana_footer`, lines 833-864, restores
  `player_control_scale_all_input 1.0 0.5` at line 842;
- `gs_gravemind_header`, lines 868-891, calls
  `player_control_scale_all_input 0.15 2` at line 879;
- `gs_gravemind_footer`, lines 894-920, restores
  `player_control_scale_all_input 1.0 1` at line 902.

These scripts contain no `player_control_lock_gaze`, forced-facing, or
`camera_control` call. A search of all official H3EK `.hsc` sources finds the
0.15 target only in those two channel headers. Other authored input scales in
the kit include 0.25 and 0.775 and must remain untouched.

The header/footer globals are transient request latches: continuous scripts
sleep until each becomes true and clear it after performing the corresponding
sequence. They are not usable as sustained state. Cortana-effect activity is
also too broad: `110cf_torture.cinematic` intentionally loops the effect
without the input header/footer. Gating on the effect itself could prevent
progression.

H3EK's named `player_control_scale_all_input` core is at RVA `0x44D290`.
It updates the mapped player records in a four-entry, 0x30-byte input-user
array: active byte `+0x1C`, current scale `+0x20`, target scale `+0x24`, and
remaining interpolation duration `+0x28`.

H3EK's named player-control updater is at RVA `0x44E4E0`. It advances current
toward target using the engine's simulation delta, not wall time; a pause or
time freeze therefore also freezes the restore. For the 0.15 hold, active
remains set after the entry interpolation completes. A normal restore reaches
the target and zeroes remaining time, but the active-clear gate itself checks
only that current and target are both within native epsilon of 1.0. A duplicate
unity setter can therefore leave a finite positive remaining value after active
has cleared; remaining time is not a completion predicate.

## Pinned retail match

Evidence source:

- `N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\halo3\halo3.dll`
- version `1.3528.0.0`
- SHA-256
  `B209D8454B12DC77E54CCD2C9924EC8D44B8619D21CF98E36FFAF601E67EFB63`

The named HaloScript definition for `player_control_scale_all_input` resolves
to wrapper RVA `0x1F6D24`, which calls the homologous retail core at RVA
`0xF9304`. The core has the same 0x30-byte record and `+0x1C/+0x20/+0x24/+0x28`
semantics as H3EK. Its complete entry/TLS signature matches exactly once in the
pinned retail module:

```text
48 8B C4 48 89 58 08 57 48 83 EC 70 8B 0D ?? ?? ?? ??
0F 28 E1 0F 29 70 E8 0F 29 78 D8 44 0F 29 40 C8 44 0F
28 C0 65 48 8B 04 25 58 00 00 00 48 8B 1C C8 48 8D 4C
24 28 B8 10 00 00 00 48 8B 04 18
```

The H3EK updater homolog is retail RVA `0xF66AC`. Its 101-byte
entry/TLS/member signature also matches exactly once:

```text
48 8B C4 48 89 58 20 44 89 40 18 89 50 10 89 48 08
55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A9 48 81 EC
00 01 00 00 65 48 8B 0C 25 58 00 00 00 41 8B F8 0F 29
70 B8 0F 28 F3 0F 29 78 A8 4C 63 FA 44 0F 29 40 98
4D 8B E7 44 0F 29 48 88 44 0F 29 90 78 FF FF FF
8B 05 ?? ?? ?? ?? 4B 8D 1C 7F BA C0 00 00 00
```

Its unique interpolation block is at RVA `0xF681C`, exactly entry+`0x170`:

```text
40 38 7B 1C 0F 84 ?? ?? ?? ?? 4A 8B 0C 29
F3 0F 10 51 08 0F 2F D7 76 ??
F3 0F 10 5B 28 0F 2F DA F3 0F 10 4B 24 76 ??
F3 0F 5C 4B 20
```

Both entries independently decode the same established engine TLS-index global
at retail RVA `0xA39F9C`; the updater then loads the record-array pointer from
TLS member `+0xC0`. The module's PE TLS directory names that exact RVA as its
`AddressOfIndex`, proving this is the loader-assigned static-TLS module index
used through `gs:[0x58]`; it is not a `TlsAlloc` slot. The updater has one
direct caller at RVA `0xF73E5`, which loops input users and resolves each
mapped player and controller. Its verified ABI is a void function receiving
player index, input-user index, controller index, and two float arguments.
No Halo 3 offset or structure member was copied from another title.

Retail address-level inspection also proves why remaining time cannot gate
release: `+0xF6834` loads remaining and `+0xF685C` may store a still-positive
decrement, while `+0xF6870..+0xF68A6` test current and target against unity
before `+0xF68A8` clears active. That clear path never tests remaining.

## Candidate implementation

The optional Halo-3-only feature is one two-hook transaction. It detours the
uniquely matched native setter for immediate channel entry/restore signals and
the uniquely matched native updater for exact completion. Both callbacks call
Halo's original function first and then observe only primary input user 0:

- finite target `0.15 +/- 0.001`: suspend VR-authored turning;
- finite target `1.0 +/- 0.001`: mark restore pending but remain suspended;
- every other target: ignore.

After each native input-user-0 update, the feature resumes only when the proven
record says active=0 and current and target are 1.0. Remaining must be finite
and not materially negative, but a positive value is valid because Halo's
active-clear path does not use it. Completion therefore follows Halo's own
simulation-time state exactly, including pauses and time dilation; there is no
wall-clock deadline.

The callbacks publish atomics only. Logging is performed by the existing 50 ms
title worker, never by either native callback, XInput hook, camera hook, or
render hooks. Installation requires both unique entry signatures, the unique
interpolation block at updater+0x170, the same decoded TLS-index global, and
the proven TLS +0xC0 member. The installer bounds-checks every decoded byte and
verifies the decoded index address against Halo 3's PE TLS directory. A
mismatch or hook failure enters `StockFallback`
(or `CleanupRequired` if rollback itself cannot be verified) for this feature
alone; the camera core, stereo, and existing input path stay armed.

A one-shot validation latch is armed before installation is published. The
first primary-input-user update can therefore recover an already-active native
0.15 record if the setter fired in the enable/publication window. A transient
setter-record read failure rearms the same latch. Idle steady state does not
poll the native record.

While the native channel owns input:

- `Game_ComputeAimStick` returns a consumed neutral RX/RY. Returning false
  would leak the raw OpenXR turn stick through the XInput fallback branch.
- `ApplyVrTurn` does not change `g_gameYawRef`.
- movement mapping and the left stick remain unchanged, so Halo still applies
  its native slow-walk magnitude.
- `ApplyHeadLook`, stereo rendering, and the OpenXR lifecycle remain unchanged.

Input user 0 is deliberate: the existing VR camera and input path supports the
primary local player, with OpenXR input merged into XInput controller slot 0.
H3EK proves input-user index and controller index are separate updater
arguments, so this candidate does not claim remapped split-screen support.
Remote network players are not locally authored, and tracking every native
input user could wait on a departed split-screen record whose updater no longer
runs.

Ownership phase and event generation share one atomic state word. Every native
completion CASes the exact observed generation, so a late completion from an
older channel cannot clear a newer 0.15 request. A same-module level restart
also fails open when native input user 0 resets or changes outside the proven
0.15-to-1.0 contract.

Unit coverage in `tests/core_tests.cpp` includes exact/near/outside channel
values, unrelated 0.25/0.775 values, non-finite/corrupt inputs, the persistent
0.15 hold, a paused restore with remaining native time, exact native
completion, completion with positive stale remaining time, inactive 0.15 state
rejection, recovery-record recognition, mixed setter transitions, and unstable
snapshots.

## Root-cause status and acceptance

Established facts:

- the native channel responds to player input at 15 percent;
- the current VR closed-loop aim can inject full RX/RY at roughly 4.8 degrees
  of error;
- direct VR snap/smooth turning changes `g_gameYawRef` outside Halo's scaled
  input path.

It remains a strong hypothesis, pending the headset test, that the closed-loop
aim repeatedly saturates against the channel's reduced native response and
causes Chief's sustained spin. The candidate must not advance
`docs/CURRENT-STATE.md` until the user confirms both:

1. Chief can slow-walk through a Cortana/Gravemind interruption without
   uncontrolled spinning or loss of progression.
2. Ordinary Halo 3 gameplay regains controller aim and snap/smooth turning
   immediately after the native restore ramp.
