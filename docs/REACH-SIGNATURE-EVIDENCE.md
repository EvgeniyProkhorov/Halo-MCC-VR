# Halo: Reach signature evidence

The optional first-person vehicle bindings, HREK layouts, 20-identity/25-seat
census, and headset-pending candidate are recorded separately in
`docs/REACH-VEHICLE-EVIDENCE.md`.

Status: **camera/culling and screen-aligned patchy-fog transaction headset-passed;
physical-scale calibration and cumulative regressions pending**. The proven
signatures below are wired into a permanent, all-or-nothing Reach per-eye camera core
(`InstallReachCameraCore` in `src/dll/game.cpp`; see `BUILDING.md`). The current
unaccepted candidate installs the inner `player_view_render`, outer
`main_render_view`, FP interpolation, visible-palette, first-person-camera, and
HREK CHUD hooks as one mandatory six-hook transaction only after the loaded-image
preflight and display proof pass. It arms after the one-second fresh-camera safety
interval. An unmet cold precondition installs no ownership path; after ownership,
an invalid camera or fault rejects the transaction and enters teardown without a
flat rerender. Halo 3 and ODST are not modified. The accepted product pointer
does not move until an exact candidate passes in the headset.

The first armed candidate changed only the compact camera, so the already-built
player-view matrices stayed stock and the headset showed a narrow flat cone.
Source `0f7b6321` reran the proven camera rebuild per eye and produced distinct
stereo plus translation, but its exact headset test exposed a raster/OpenXR FOV
mismatch and competing stock/HMD look. That failure is preserved under
`out/test-runs/0f7b632-reach-3d-warp-input-fail-20260724-094945Z`.

Source `f953bbe3` then bound the actual Reach projections and both eye copies to
the prepared OpenXR frame and gave the HMD exclusive visual look-stick ownership.
Its exact DLL
`2B492F23ECF7CBB158B5EE4072B01CDE1F4BF7439C8BFF8886649547269AC980`
armed at approximately 100 FPS with zero frame-order failures. The user reported
that the Reach 3D looked great, but world culling still followed the gun/stock
aim camera instead of the headset. The log, manifest, build identity, and result
are preserved under
`out/test-runs/f953bbe-reach-stereo-pass-culling-fail-20260724-102643Z`.

Source `065f62a0` moved visibility ownership to the tracked head, but its exact
headset test went black immediately after Reach stereo armed. The live log
showed `stereo on` with `focused shouldRender=1 layers=0`: neither eye copy had
completed. The candidate incorrectly rejected every normal outer transaction
because it required a nonnegative pre-push camera-stack depth. The pinned
retail image initializes the empty stack to `-1`; the exact push increments it
to depth/slot zero. The failure is preserved under
`out/test-runs/065f62a-reach-head-cull-black-20260724-114200Z`. The forward
correction accepts only pre-push depths `-1..2` and still requires the exact
post-push relation and bounds described below.

Source `86864bd0` admitted the proven `-1 -> 0` top-level stack transition. Its
exact DLL
`E66598671EBB602BF5D5B46CAA45F3E0678073603E8150C3A2641723B5DFD209`
submitted true stereo at approximately 94-120 FPS with zero frame-order
failures. The user reported that stereo, projection, 6DOF, stick/head coherence,
and head-owned visibility now looked great. The remaining headset defect was a
fog/haze contribution that appeared eye-swapped and followed head motion. The
run is preserved under
`out/test-runs/86864bd-reach-camera-pass-fog-eye-fail-20260724-120101Z`.

Source `facf6b07` then attempted the shared motion-blur comfort outcome by
writing both Reach controls below to zero. Its exact DLL
`38BEEF66535A01E0AAC76A6FCFA52117183EEE305F2512663654DB29D6C492A0`
ran with the camera/culling transaction intact, but the user still saw a
translucent/alpha texture following the head. That run is preserved under
`out/test-runs/facf6b0-reach-alpha-fog-fail-20260724-072511Z`. Static analysis
after the headset result proved that this was not a valid blur-off experiment:
the Reach distortion pass divides by `motion_blur_scale`, so setting scale and
maximum to zero generated invalid shader constants.

Source `03f0bffb` retained/reasserted the positive authored scale and zeroed only
the maximum. Its exact DLL
`456584DF50DF7B7941008BCF23EBC488F24938EE3D2C5B2E8F6A6FEEB182F6BB`
ran with live values `motion_blur_max=0.0` and `motion_blur_scale=0.35`, one
opaque OpenXR projection layer, current eye caches, and zero frame-order
failures. The user confirmed that the camera/culling/stereo result remained
good, but the translucent fog layer still moved opposite the headset instead of
remaining world-stationary. The run is preserved under
`out/test-runs/03f0bff-reach-alpha-persists-live-20260724-125506Z`. This rules
out native motion blur, invalid distortion constants, and a separate OpenXR
overlay as the remaining cause.

Source `b0710dc0` then set and restored only the proven patchy-fog skip bit around
each admitted eye. Its exact DLL
`FF43BC89C5AFEC799DA43EB78EC58CC173B113DEC208FEE69E5F2B6235376C35`
passed the cold/runtime proofs, retained one opaque projection layer and zero
frame-order failures, and removed the opposite-moving translucent layer in the
headset. The user confirmed that the image looked good. The run is preserved
under `out/test-runs/b0710dc-reach-patchy-fog-headset-20260724-132935Z`.

The preceding culling defect was pinned to an exact pre-inner stage. Retail
`main_render_view` performs its visibility work before calling
`player_view_render`; its cluster lookup at `0x0C3320 -> 0x273458` explicitly
queries `workspace+0x154`, and the visibility build at `0x0C335C -> 0x27F408`
receives the secondary compact/derived pair `+0x154/+0x1E4`. The pinned HREK
homolog names this work `player_view_compute_visibility` and repeats the same
secondary-camera inputs. Installing HMD eyes only in the later inner hook can
therefore draw correct stereo from a visibility set collected in another
direction.

The retained headset-passing camera/culling transaction:

- reads one lock-free, exact-prepared-serial head/pad/two-eye snapshot at the
  exact normal outer boundary;
- converts each asymmetric OpenXR eye to the symmetric fixed-aspect projection
  Reach actually rasterizes, rotates all eight widened corner rays into
  head-centre space, and selects a symmetric binocular angular-union FOV;
- applies shared turn plus HMD orientation/translation once to a local centre
  camera, runs only the proven pre-scope frustum/projection helpers, validates
  the built projection, then mirrors that centre into both workspace pairs;
- lets stock visibility run exactly once from that head-centred secondary camera;
- rebuilds the bounded player-view camera/matrices to the same centre as coherent
  pre-ownership state; a claimed inner failure is rejected and suppressed;
- derives both exact raster eyes from the same saved centre in the inner hook,
  without applying turn, head pose, or room-scale lean twice; and
- restores the bounded player-view state and only the proven `0x2A8` camera-pair
  bytes after the outer call, leaving the engine-owned camera-stack callback and
  final render-owner lifetime untouched.

As in accepted Halo 3/ODST, the visibility camera is head-centred and actual eye
origin translation is applied only in the two eye rasters. The angular union is
conservative for the exact widened FOVs and eye cant, but this candidate does not
claim exact finite-distance coverage of translated eye origins. Close doorway
edges and near peripheral geometry are therefore explicit headset tests rather
than hidden fixed-IPD or guessed-near-plane padding.

### Reach physical world-scale parity

The patchy-fog passing run exposed a much smaller calibration difference: the
user reported that Reach's world felt slightly too small relative to Halo 3 and
ODST. Reach used the shared rounded `0.33` world-units-per-meter value for both
room-scale head displacement and exact OpenXR eye separation. The accepted ODST
adapter uses the authored ten-foot world-unit conversion `1 / 3.048`, or
approximately `0.32808399`. Reach therefore scaled tracked positions 0.584%
too far relative to that exact physical baseline, making the perceived world
approximately 0.584% too small in the reported direction.

The forward calibration uses one Reach-only `kReachWorldUnitsPerMeter =
1 / 3.048` constant in both physical transforms. It does not change Halo 3's
independent calibration, ODST, FOV, culling, or eye orientation, and it never
scales IPD without scaling room motion by the same amount. The controller-driven
weapon, marker graph, support hand, and wrist targets in the current unaccepted
candidate consume this same title-specific conversion. The original pre-head
gameplay camera position remains separate from the tracked head-centre shoulder
root, preventing both head translation and gun calibration from being applied
twice. Per-eye rendering still uses the proven frustum helper
`0x287F58`, projection builder `0x2884BC`, camera-state updater `0x286F9C`, and
projection/matrix builder `0x28AF8C` before each inner render.

### Reach-native temporal motion-blur parity

The accepted Halo 3 and ODST paths disable title-native camera motion blur by
default. Reach had not yet consumed that shared comfort outcome in `86864bd`.
Pinned HREK `player_view_render` reaches `apply_distortions` and
`distortion_apply_and/or_motion_blur` at RVAs `0x8351E6` and `0x835201`.
Separately, pinned retail passes current `player_view+0x490` and previous
`player_view+0x760` matrix blocks at `0x26C956..0x26C967` to
`water_renderer_setup_for_frame` (`0x25795C`, HREK homolog `0x812670`). That
routine reads only the previous origin at `+0xF0..+0xF8` for a greater-than-five
world-unit movement/camera-cut flag. Normal eye/head motion cannot satisfy that
threshold, so it does not justify changing or mirroring camera history.

The exact retail distortion-constant builder begins at `0x287398`. It loads
`motion_blur_max` and `motion_blur_scale`, divides maximum by scale at
`0x287561`, doubles scale, and divides the scaled maximum by that value at
`0x2875AD`. HREK's homolog begins at `0x86B670`; its exact constant block
`0x86BA8F..0x86BC2F` performs the same divisions at `0x86BBA9` and `0x86BBF9`.
HREK profile strings identify the owning work as `apply_distortions` and
`distortion_apply_and/or_motion_blur`. Thus `facf6b07`'s zero/zero policy
provably uploaded NaNs into the screen-space distortion path and made that
experiment invalid. Source `03f0bffb` proved the same visible fog artifact with
finite `0.0/0.35` controls, so the invalid constants did not cause it.

Both exact binaries independently expose unique type-6 float debug-table
entries:

| Control | Retail entry -> value RVA | HREK entry -> value RVA | Authored value |
| --- | --- | --- | ---: |
| `motion_blur_max` | `0x00B3A1C8 -> 0x00B44600` | `0x0201BA70 -> 0x02059054` | `0.08` |
| `motion_blur_scale` | `0x00B3A1E0 -> 0x00B44604` | `0x0201BA88 -> 0x02059058` | `0.35` |

Production resolves both exact names only after the pinned loaded-image
preflight, requires their resolved pointers to equal the proven retail value
RVAs, and rejects aliases, out-of-image values, a non-positive/near-zero scale,
or a non-finite/negative maximum. At each exact normal admitted Reach stereo
boundary, the forward correction preserves/reasserts a positive authored scale
and writes only the maximum to zero while the shared `motion_blur=0` VR default
is active. This makes both proven ratios finite zero without logging,
allocation, scanning, or locking in the render hook. `motion_blur=1` restores
both latest authored values. Title teardown first disables all six Reach hooks and
proves callback plus relay/wrapper RIP quiescence, then restores both authored
values; a failed restore retains the module/state and retries. No camera,
culling, history-matrix, eye-order, FOV, render, capture, depth, or controller
policy changes in this candidate.

The max-only suppression above is now headset-tested and active, but it did not
remove the fog contribution. Native motion blur is therefore excluded without
changing the passing camera, culling, history, projection, eye order, or capture
paths.

### Screen-aligned patchy-fog isolation

Retail `player_view_render` contains a separate patchy-fog gate after its
atmosphere helper. At `0x0026CC59` it tests bit `0x08` of the byte at
`0x00CA0240`; `jne 0x0026CC6A` skips the call at
`0x0026CC65 -> 0x0026EFEC`. The patchy helper queues callback
`0x0026F16C`, whose renderer reaches `0x0029C510` and consumes the current
player-view camera state. The exact 2314-byte `player_view_render` body hash
already pins the test/jump bytes. Production additionally proves the helper
`rel32` edge, the mapped flag byte, and the live RIP-relative target before
arming.

HREK independently exposes the homologous block at `0x0083505F..0x0083516B`,
callback `0x00836B40`, and renderer `0x00894460`. Its strings identify
`Patchy Fog Global Parameters` and `_surface_patchy_fog_buffer0/1`, matching the
headset report of a translucent screen-aligned texture moving opposite the
head. This is distinct from the atmosphere-fog helper, which remains enabled.

The correction sets only skip bit `0x08` immediately around each exact
admitted VR eye's original `player_view_render` call. A per-eye `__finally`
re-reads the byte and restores only bit `0x08`, preserving any unrelated engine
flag changes. Setup or restoration failure rejects the claimed stereo transaction,
suppresses that call, and enters teardown without a flat rerender. Unclaimed,
nested, and screenshot calls never enter the suppression scope. Camera/culling, motion
blur constants, matrices/history, eye order, projection, CHUD, and capture order
are unchanged. Source `b0710dc0` and its exact DLL passed this narrow headset
gate: the opposite-moving translucent layer was gone and the image looked good.

### Atmospheric fog and rain: the player-facing weather switches

Reported 2026-08-02: Reach reads soft and washed out in the headset, blamed by
the user on rain and fog. The screen-aligned patchy fog above was already
suppressed per eye, so it is not the fog being seen. These are the other two.

**Official HREK names both, in one file.** `HREK/bin/debug_menu_init.txt` ships
a `Fog and Weather` menu:

| Entry | Kind | Variable |
| --- | --- | --- |
| `enable render atmosphere fog` | command | `render_atmosphere_fog 1` |
| `disable render atmosphere fog` | command | `render_atmosphere_fog 0` |
| `render rain` | global | `render_rain` |
| `render rain particles` | global | `render_rain_particles` |
| `render rain light volumes` | global | `render_light_volume_rain_particles` |
| `render rain sheets` | global | `render_light_volume_rain_sheets` |

The command/global distinction is load-bearing and measured in the pinned
retail image. `render_rain`'s descriptor at `0x00B40FF0` has type word `5`
(boolean) and value byte `0x00B4444C`. The three other rain globals resolve to a
NULL backing value (`0x00B41008`, `0x00B41020`, `0x00B40F90`), which reconfirms
the 2026-07-27 measurement that only `render_rain` is live. But
`render_atmosphere_fog`'s descriptor at `0x00A0FEB8` has type word `0` and a
"value" pointer of `0x00198F2C` — **inside `.text`**. The name-scan debug-var
path returns that pointer without complaint, so resolving a debug COMMAND that
way hands back CODE. `render_depth_of_field_enable` and `render_depth_of_field`
share the same trap and the same `0x00198F2C`. Production therefore requires the
exact type word and proves the slot is committed, writable, non-executable
memory before returning it.

**Atmospheric fog is a TLS-reached flag bit, not a module global.** The command
handler is `0x001B4CF4` and does one thing: `mov edx, [0x00C17B18]` (the render
TLS index), `mov rcx, gs:[0x58]`, `mov r8d, 0x168`, `mov rcx, [rcx+rdx*8]`,
`mov rdx, [r8+rcx]`, then `or cl, 4` / `and cl, 0xFB` and `mov [rdx], cl`. So it
writes bit `0x04` of `*(renderTls + 0x168)`, and a nonzero argument SETS it.

The consumer is the atmosphere helper `0x0026D5B4`, called from
`player_view_render` at `0x0026CC54` — the call immediately before the patchy-fog
gate above. It opens with the identical TLS walk and then, at `0x0026D5F3`,
`test byte ptr [rax], 4` with `je 0x0026D645` skipping the whole atmosphere fog
upload. A CLEAR bit therefore means "no atmospheric fog", matching the menu
labels exactly. **This is the opposite polarity to the patchy-fog byte, where a
SET bit means skip**; they are different structures — one a module global, one
per-thread TLS — and must never be treated as one flags word.

**Rain has five readers, every one a gate.** `render_rain` is read at
`0x00259A1C`, `0x0025E339`, `0x0026CC92`, `0x0026E822` and `0x0026E978`, each as
`cmp byte ptr [render_rain], 0`. The `0x0026CC92` gate is inside
`player_view_render` and the call it skips is `0x00288D60`, i.e.
`kReachRainParticleRenderRva` — the same renderer the accepted view-decoupling
detour wraps. Clearing the byte removes the rain draw outright and that detour
simply stops being reached.

**The render-pass disable mask, recorded because it was mapped on the way.**
`0x00CA0240` is a dword of skip bits, every reader shaped `test <bit>; jne skip`:
bit `0x02` at `0x0028BDC5`, `0x04` at `0x0027809D`, `0x08` at `0x0026C1B4` and
`0x0026CC59` (patchy fog), `0x10` at `0x00166DB2`/`0x00167E7F`/`0x00168119`,
`0x20` at `0x002A1404` (inside native SSAO), `0x40` at `0x0026C96C`, and `0x100`
at `0x0026CC9B`/`0x002897F4`/`0x002898CB`. Bit `0x100` is a second, independent
way to skip rain. Production uses the by-name `render_rain` boolean instead, so
that the binding carries no hardcoded address; the RVA is only cross-checked
against the pinned image, exactly as the motion-blur slots are.

**What production does.** Both controls are asserted at the top of every owned
VR eye render, before the original `player_view_render`. This is deliberately an
assert rather than the patchy-fog save/suppress/restore scope: patchy fog must
stay on for the title's own authored views and is wrong only inside our stereo
eyes, whereas these two are a player preference that should hold wherever the
player looks. Turning a setting back on writes the title's own value back on the
next eye, which also makes the control self-healing against the tag loader that
rewrites render globals on level load. Each control is independently fail-open:
an unproven or unwritable one leaves that single effect stock, is counted, and is
reported by the worker every ten seconds; it never drops an eye or blocks the
camera core. The rain suppression is handed back at teardown because it is a
module global; the fog bit is not, because it lives in per-thread render TLS that
only the render thread can reach, and a title-module reload re-initialises it.

This document records Reach-specific facts for the cumulative Halo 3 + ODST +
Reach line. Halo 3 and ODST offsets, layouts, bones, markers, and tag meanings
are not Reach evidence. The authoritative accepted baseline remains public
release `MCC_VR_ALPHA_0.2.2`, runtime source
`3a2a11bfc66b36e70f60282e91c9d5436f2e18d1`.

## Pinned inputs

The installed Steam retail module was verified read-only on 2026-07-23:

| Field | Value |
| --- | --- |
| Module | `haloreach.dll` |
| SHA-256 | `738DD2D24EA3AEA12E1EE9AA4A61094BF116027D42004C35A19E5048608B0894` |
| PE timestamp | `0x68A0EFE1` |
| `SizeOfImage` | `0x04EDA000` |

The official installed Halo: Reach Editing Kit is build
`2023.07.17.176677.1-QFE1`. Its `HREK.7z` was extracted only into the installed
`HREK` directory after confirming `tags/` and `data/` were absent. Kit and game
assets are local evidence only and must never be committed or packaged.

The HREK executable used for the camera/frustum corroboration below is
`reach_tag_test.exe`, SHA-256
`CBDD8448A87A433B0DFFC0DE47D06DB7A18B4BF868B96B057135DAA86790ABA8`,
PE timestamp `0x64B47B50`, `SizeOfImage` `0x06247000`. The hash records the
exact local evidence input; the executable is not stored in this repository.

The machine-readable copy of these identities is
`docs/REACH-EVIDENCE-MANIFEST.json`.

## Preliminary function candidates

Unless a row says otherwise, names below remain hypotheses rather than hook
sites. The frustum, FP camera rebuild/upload transaction, FP interpolation, and
visible-palette entries now have the additional title-specific evidence
recorded here. The special composer is a preserved negative result.

| Candidate | RVA | Loaded-image unique | Executable range | ABI | Callers | Data flow | HREK semantics | Layout fields | Status |
| --- | ---: | --- | --- | --- | --- | --- | --- | --- | --- |
| Camera-derived frustum bounds (original viewport hypothesis) | `0x287F58` | Exact stock observer passed the unique loaded-image check twice; production must repeat it | Yes, `.text`, `0x287F58`-`0x2881CC` | Yes | Nine direct callers | Yes | Yes | Static consumers and producer proven; live lifetime still open | Static role proven; hook forbidden |
| First-person camera rebuild/upload | `0x286C6C` -> `0x282D60` | Exact-one production AOBs required | Yes | `void(view,bool)` -> `void(compact,derived)` | Three visible nested-workspace wrappers; exact tail edge | Yes | Exact HREK homologs at `0x87C4A0`/`0x87C4B0` -> `0x8561A0` | Nested compact `+0`, `0x90`; derived `+0x1E4`, `0xC4`; callback `+0x2A8` | `a1dcb7b` partial/failed: outer-workspace gate prevented the swap; corrected candidate pending |
| FP interpolation | `0x0CF1A4` | Exact-one production AOB required | Yes | `bool __fastcall(int outputUserIndex,int animationIdentity,int fpWeaponSlot,BoneMatrix** out,int* count)` | Direct edges recorded below | Yes | Exact HREK/live graph correlation | 120-node bound | Production candidate; headset pending |
| Visible palette | `0x2B4EB0` | Exact-one production AOB required | Yes | `void __fastcall(uint16_t tag,const BoneMatrix* root,BoneMatrix* dst,uintptr_t,const BoneMatrix* source,const int32_t* boneMap)` | Wrapper/call edges recorded below | Yes | Exact HREK/live body maps | 47/41 output layouts | Production candidate; headset pending |
| Special-bone composer | `0x213224` | N/A | Offline file and headset negative result | N/A | N/A | Did not drive visible FP pose | N/A | N/A | Disproven for this feature |

No candidate may become a runtime hook until all columns are independently
proven against HREK and MCC's x64 loaded image. A usable AOB must match exactly
once in the expected executable range; zero or multiple matches install no hook
and acquire no VR ownership. Every consumed field needs finite-value, bounds, index, and count
guards plus an understood teardown boundary.

## First-person interpolation and palette production candidate

This path is implemented but remains **unaccepted until the exact packaged DLL
passes in-headset**. No further probe build is part of the candidate.

The pinned retail interpolation entry is `haloreach.dll+0x0CF1A4`. Its unique
47-byte AOB is:

```text
48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 33 DB 49 63 F8 38 1D ?? ?? ?? ?? 4D 8B E1 8B EA 4C 63 D9
```

The ABI is `bool __fastcall(int outputUserIndex, int animationIdentity,
int fpWeaponSlot, BoneMatrix** outBones, int* outCount)`. The third argument is
the primary/secondary first-person weapon slot, not a stereo eye; Reach enables
only slot zero. The five proven direct call edges are `0x121083` (return
`0x121088`), `0x2AF85A` (return `0x2AF85F`), `0x2AF8F6` (return `0x2AF8FB`),
`0x2AFEB4` (return `0x2AFEB9`), and `0x2B0ADB` (return `0x2B0AE0`). The
`0x121083` path indexes one returned `0x34`-byte matrix for an on-demand
marker/attachment transform. The next two are distinct interpolation -> palette
transactions: `0x2AF85A -> 0x2AF89D` and `0x2AF8F6 -> 0x2AF936`. Both call the
palette wrapper `0x2B52EC`, which calls the final source-to-output builder
`0x2B4EB0`. Their interpolation arguments have the same view/id/slot within one
builder iteration, but the returned source pointer is transaction output and is
not assumed stable or equal. The hook copies each untouched graph before
rigidly applying the right-wrist delta to its live marker/muzzle/attachment
source, bounded to 120. Visible palettes never consume that mutation.

**CORRECTION (2026-07-27): the name below is wrong.** `haloreach.dll+0x120FDC`
is NOT `first_person_weapon_get_marker`. It is a bool-taking wrapper whose
non-first-person path is literally `call 0x120EC4` at `0x1210BE`, and it has
**exactly one caller** (`0x11BFDC`) - so the previously recorded claim that
"both consumers reach it" is also false. The effect-location node-matrix
resolver is `haloreach.dll+0x00120EC4-0x00120FD8`, the instruction-for-
instruction homolog of HREK `0x36AB70-0x36ACF3` (identified by HREK's own
`effects.cpp` asserts at lines 7630/7638/7649/7650). `0x120FDC`'s
first-person path is that getter inlined. Do not adopt a replacement name:
"get_marker" is not HREK's own name for `0x8CEF90` either. The address, entry
bounds and signature below are correct and were re-measured unique; only the
label was wrong.

**CORRECTION (2026-07-29): position is not effect ownership.** The accepted
effect-location hook re-parented every resolved matrix within 0.5 world units
of the published head-to-gun anchor, even when `effect+0x50`'s
`first_person_weapon_user_mask` (low nibble) was zero. The exact target
`E4905011247978F570C17739FB7631FF91CB5F4870ADF5B42DC986E4A1585C88`
runtime log preserved at
`out/deploy-backups/e490501-steam-before-81b6573-20260728-212252666Z/halo3xr.log`
proves the leak. Its final report contains 75,967 re-parent writes, 3,867,980
`world/no-fp-user` calls, 1,355 `world/fp-output-none` calls, and 788
`already-first-person` calls. Even if every call in both first-person buckets
were re-parented, at least 73,824 successful writes necessarily belonged to
the no-owner world bucket.

Candidate `b510b5e` required the engine-owned low nibble to be nonzero before
the existing proximity test could translate the matrix. Its exact PSVR2
headset log proved the gate armed: 215,356 `world/no-fp-user` calls produced
zero out-of-range attempts, while 252 first-person-owned calls produced 224
re-parents. The black world geometry was unchanged. This conclusively rejects
the cross-owner effect writes as its cause. The following revert leaves the
gate dormant and restores the accepted E490 behavior before the next
candidate. Evidence:
`out/test-runs/b510b5e-reach-world-black-effect-gate-fail-20260729-042409`
(log SHA-256
`E6BA52C4671C03002C003D7FF3BAEEA2CF125283E605D6BDC6F5976B6E9C1CFF`).

## Static-world black geometry: post-palette world/local wrist corruption (CANDIDATE)

This candidate removes one active mod write, not a native-renderer feature and
not a stock or 2D fallback. Commit `511eb0b` was a failed muzzle-flash candidate
that remained in every later build, including the requested E490 artifact. It
copies `alignedRight` directly into
`context.source[context.layout.rightWristSource]` after the visible palette has
already been composed.

The coordinate-space violation is explicit in the existing code and official
palette contract:

- `FpExplicitPoseTargets` carries absolute world poses. Reach builds the wrist
  translation as gameplay base plus tracked controller offset and standoff;
  `ReachAlignRightTargetToAuthoredBarrel` preserves that world translation in
  `alignedRight`.
- The live interpolation graph is root-relative record space. The HREK/retail
  palette builds `destination[i] = root * source[boneMap[i]]`. The correct
  earlier `ApplyControllerToMarkerBonesWithTarget` path therefore computes the
  world wrist, forms the world delta, converts it back through the inverse root,
  and applies that record-space transform to every graph record.
- The later `511eb0b` block skips that conversion and replaces only the right
  wrist record with the absolute-world matrix. Any later stock composition sees
  `root * world`, and the wrist is no longer coherent with the already-transformed
  descendants. Halo 3 and ODST have the correct whole-graph transform but no
  equivalent post-palette single-record assignment.
- `main_render_view` performs interpolation and palette work before its two
  `player_view_render` calls. The visible weapon has therefore already consumed
  the private correct palette, while the invalid live record remains resident
  through later world-eye rendering. No success-path restore exists. Worse, the
  pending write was armed before palette reconstruction finished, so a failed
  reconstruction could restore the stock graph and then immediately poison it
  again after the stock palette call.

This block is proven hot in the exact affected runtime. The requested E490
source `9cb88d7` log reaches 118,490 recorded attempts at the write:
`out/deploy-backups/e490501-steam-before-81b6573-20260728-212252666Z/halo3xr.log`
(SHA-256
`87018037E82623984B32828DA1A556D9636628E881E79F7FD08209C37C53B34F`).
The DrawIndexed and SSAO black-world failures independently reached 10,240 and
7,192 attempts. The historical counter increments after `SafeWriteBytes`
without checking its return, so these are formally attempts; the same validated
source buffer had just passed a complete finite read/write transaction.

The write had no remaining feature owner. The next investigation after
`511eb0b` identified the live-skeleton approach as ineffective, and accepted
`b942078` fixed the muzzle flash solely by retargeting loaded tag location data,
explicitly with no bone change. The forward candidate leaves the coherent
whole-graph marker transform, visible controller gun/hands, stereo camera, and
accepted tag retarget intact. It compile-time disables only the late world-to-
local assignment and reports an unconditional generation line plus the first
prevented invocation from the worker. A valid headset run must show prevented
greater than zero, executed zero, and the existing both-eye FP camera ACTIVE
line. Causal acceptance still requires the headset result.

## Static-world black geometry: lightmap-shadow isolation (REJECTED)

The headset boundary is unusually specific. Large first-person weapons and
nearby vehicles trigger hard black regions on authored terrain and other static
world surfaces. The caster itself, characters, vehicles, first-person weapons,
sky, grass blades, and instanced rocks stay lit. The regions differ between the
two eyes but remain attached to world depth. That resembled the receiver/caster
boundary of Reach's dynamic lightmap-shadow renderer, but the exact isolation
test below disproved it as the cause.

Official HREK establishes the system rather than merely naming a retail debug
byte:

- HREK contains the source/assert path `render_lightmap_shadows.cpp`, the
  profile names `lightmap shadow generate` and `lightmap shadow apply`, and the
  object-tag fields `lightmap shadow mode` / `does not cast shadow`.
- HREK `player_view_render` calls the lightmap-shadow wrapper at
  `0x00834CC3 -> 0x008365F0`. The wrapper checks the official
  `render_lightmap_shadows` boolean at `0x02056C75` from instruction
  `0x00836664`, then calls the source-named renderer at `0x00865910`.
- Immediately afterward, HREK's `apply_lightmap_shadows_to_single_pass` path
  at `0x00834D1A-0x00834E0C` applies the generated surfaces before the
  source-named `render_static_lighting` call at `0x00834E31`.
- The official type-5 boolean descriptor is at `0x02014C30`; its live value is
  `0x02056C75` and its authored value is one.

The pinned retail module has the same complete control-flow chain:

| Evidence | Retail RVA |
| --- | --- |
| Name `render_lightmap_shadows` | `0x009EB120` |
| Type-5 descriptor -> live value | `0x00B41080 -> 0x00B4444D` |
| Player-view call -> wrapper | `0x0026CB8F -> 0x0026E7B4` |
| Wrapper compare of live boolean | `0x0026E7C4` |
| Enabled call -> shadow renderer | `0x0026E7E3 -> 0x0028B3D0` |
| Apply call -> optimized apply block | `0x0026CBA9 -> 0x0026E878` |

The disabled branch cannot feed a prior-eye or prior-frame surface into the
later apply. Retail wrapper `0x0026E7BD` first sets untouched sentinel
`0x04E38908 = 1`; false branch `0x0026E7D4` skips the renderer. Every producer
that actually acquires shadow surface 2 goes through helper `0x00252F08`, which
conditionally clears the current surface through `0x002511C4` and then zeros
the sentinel at `0x00252F5D`. Apply gate `0x0026E893-0x0026E89D` skips when the
surface remains untouched. HREK repeats the same protocol at wrapper
`0x00836654`, surface helper `0x007CB0D0`, and apply gate
`0x00834D0A-0x00834D14`. Disabling generation therefore means either no apply,
or application of a surface cleared and touched by another current-frame
producer; stale reuse has no path.

Retail `0x0028B3D0` and descendant `0x0028BAB4` enumerate object casters and
render their lightmap shadows. Its explicit camera-derived read at
`0x0028E171` uses the camera-stack workspace selected through global
`0x00C878A8`. Every field it consumes is inside the secondary-derived `0xC4`
block that the existing eye transaction already refreshes and mirrors for each
eye. The pass therefore is not reading a camera block omitted by the current
per-eye rebuild; the direct isolation boundary is the pass itself.

This is **not** native SSAO/HDAO. It does, however, enclose the real
`render_shadow_screenspace` work; the earlier text that called that a later,
separate gate was wrong. Official HREK publishes `render_shadow_screenspace` at
descriptor `0x02015488` and five reads of its live value `0x02059019`. Its
player-view path is nested under the enabled `render_lightmap_shadows` call:
`0x00865910 -> 0x00867F60 -> 0x00866CA0`. Retail has the homologous chain
`0x0028B3D0 -> 0x0028BF50 -> 0x0028C8E0`; the retail descriptor's value pointer
is optimized to null. The `render_lightmap_shadows` false branch at
`0x0026E7D4` skips the outer call and therefore skips this screen-space apply
block too. Candidate `839aed7` consequently rejects both the object-caster and
native screen-space-shadow work in that transaction.

The old `_surface_shadow_mask` diagnostic cleared the surface before each
`player_view_render`, so native SSAO could and did regenerate it before lighting
applied it. Its asynchronous readback also sampled only the centre 64x64 texels,
not the complete 3262x2352 surface. A clear-off preserved log disproves the stale
blanket claim that the surface was always white: paired samples include
67.45%/63.03%, 27.24%/48.85%, and 24.53%/25.21% lit. That measurement remains a
valid limited observation, but it did not isolate or reject the later SSAO
producer. Evidence:
`out/deploy-backups/681c161-steam-before-fd6da0a-20260729-065406006Z/halo3xr.log`.

Candidate `839aed7` resolved the descriptor, name, type, value pointer,
player-view call, wrapper compare, and renderer call exactly. For each admitted
VR eye it saved the title boolean, cleared it immediately around stock
`player_view_render`, and restored it in `__finally`. The Steam/SteamVR PSVR2
log records the exact candidate BOUND and ARMED, then at least two completed
`1 -> 0 -> 1` eye scopes with zero write or restore failures. The black static
world geometry remained unchanged. This rejects the exact lightmap-shadow
transaction, including its nested native screen-space-shadow apply, as the
cause; the implementation is retained but no longer armed. Evidence:
`out/test-runs/839aed7-reach-world-black-lightmap-shadow-fail-20260729-061119`
(log SHA-256
`40923EF9CDD1BEBECB6D8660CAB98E9385AB5BF81FB1C48986731930051B4A96`).

## Static-world black geometry: native SSAO/HDAO isolation (REJECTED)

This is one behavioral candidate, not an accepted fix. It suppresses Reach's
exact native SSAO/HDAO callee only for the two fully owned VR-eye renders. It
does not change the world or first-person cameras, culling, projection, depth
buffers, OpenXR layers, title TLS flags, or shader constants, and it introduces
no direct shared-surface write or clear. Flat, screenshot, nested, unowned, and
non-Reach rendering calls the original function unchanged.

Official HREK establishes the source-level system and its place in the render
pipeline:

- The player-view wrapper at `0x008365F0` calls native SSAO at
  `0x008366A3 -> 0x007FD180`, after the first-person/depth work and the
  lightmap-shadow producer and before later static lighting.
- HREK names the `_surface_ssao*` downsampled-depth, normal, and mask
  intermediates. HDAO samples shared scene depth; there is no object-class
  caster filter that excludes a long first-person weapon or nearby vehicle.
- Both final HREK paths call `0x007CB0D0` (`0x007FD5F2` and `0x007FD6F9`).
  That helper selects surface index 2 as render target 0; the official surface
  descriptor table maps index 2 to `_surface_shadow_mask`. SSAO therefore uses
  its private intermediates but composites its final result into the shared mask
  that later lighting consumes.

The pinned retail module repeats the complete chain:

| Evidence | Retail RVA |
| --- | --- |
| Sole player-view call -> native SSAO | `0x0026E81D -> 0x002A13A0` |
| SSAO function range | `[0x002A13A0, 0x002A1907)` (`0x567` bytes) |
| SSAO body SHA-256 | `760D2BEC3AA13ABFA0AB2002E2873C9C8A9F1FEA9EE63238585C2A6C92943EE7` |
| Final multipass/direct output calls | `0x002A169A`, `0x002A1755` |
| Both output calls -> surface-2 helper | `0x00252F08` |
| Following `render_rain` gate (not shadow) | `0x0026E822` |

The surface helper begins `sub rsp,38h; mov edx,2; xor ecx,ecx`, then calls the
render-target binder. The later apply block at `0x0026E878` gates on the
current-frame touched flag, binds surface 2, and draws it before static lighting.
The byte immediately after the SSAO call is not that apply gate. HREK descriptor
`0x02014C48 -> 0x02056C74` and retail descriptor
`0x00B40FF0 -> 0x00B4444C` both identify it as `render_rain`. No rain behavior
was built or tested for this black-world defect.

The current Reach VR transaction also supplies the missing symptom link. Its
first-person camera hook replaces the nested weapon camera with the current
world-eye compact and derived/projection pair, then uploads that pair. The long
weapon and nearby vehicle are therefore present in eye-specific foreground
depth before native SSAO runs. SSAO can turn those different per-eye depth
shapes into different mask patterns, while the later receiver paths can leave
sky, first-person weapons, characters, and vehicles visually unaffected and
darken authored static-world lighting. This is a mechanism selected from the
observed receiver boundary, not a claim of headset acceptance.

The production candidate pins two unique signatures (the 35-byte SSAO entry and
the 21-byte player-view call context), verifies the exact rel32 caller edge, and
verifies both final calls to the unique surface-2 helper. Its callee detour
returns only when all of these live conditions agree: exact call return address,
active Reach generation and armed core, current eye 0/1, matching stereo eye,
matching owner/eye workspace and player view, matching prepared-frame serial,
active render access, exact camera-stack depth/top/callback, active view, and
specialization zero. Every mismatch calls the original SSAO function.

Direct return is deliberate callee isolation, not the engine's TLS disable
path. Retail's pre-gate `setne` byte has only three code references: that write
and two later reads inside the same SSAO function. Skipping the whole function
therefore omits no externally consumed state; the next stock call rewrites it
before either read. Temporarily clearing native TLS bit `0x10` would be broader
because the same bit gates cheap-particle spawn/update/render and would add a
restoration hazard.

The hook emits no hot-path logging, allocation, locks, file I/O, or GPU
readback. Lock-free per-eye counters are reported by the worker. The install and
arm logs state zero samples unconditionally, and the first actual suppression
reports eye-0/eye-1 counts so a headset run cannot be mistaken for an executed
candidate if its ownership gate never fired.

Packaged identity: source
`8d7af6e25418c2d2c86aa518d8d8f199a6e7080d`, candidate
`out/candidates/8d7af6e-reach-fp-parity-20260729-140858597Z`, DLL SHA-256
`99693E1488ACA887DEEA09BA4D438EC5D4C446FF5911A9ED04060342D0F7E7A6`.
The exact DLL hash was independently verified after automatic installation to
both Steam and Microsoft Store editions. MCC was not launched and the existing
configuration was not changed.

The exact Steam/SteamVR PSVR2 log proves the behavior executed for both eyes:
the worker first reported zero samples, then 6/6, and later 3,602/3,602
suppressed eye calls with 66 unowned calls passed through to the original. The
per-eye first-person camera reported ACTIVE and stereo held 120 Hz. The user's
headset result was that the black static-world geometry was unchanged. This
rejects native SSAO/HDAO as the cause; the hook remains in source as dormant
evidence and the following revert prevents it from arming. Preserved evidence:
`out/test-runs/8d7af6e-reach-world-black-ssao-fail-20260729-091522` (log
SHA-256 `9567C19D0610E4D7C0651456517ABFF36CA6C15BCE2F33F8FE9661F06CEC7420`).

## Static-world black geometry: stale DrawIndexed diagnostic isolation

The exact E490 baseline and failed `839aed7` log both contain the July 26
`REACHDRAW`/`REACHVTX` HUD-discovery probe. It installs a process-wide
`ID3D11DeviceContext::DrawIndexed` detour, although the probe body acts only
while Reach is active. For each matching draw it creates a staging buffer,
copies the bound vertex buffer, blocks on a CPU read map, formats five log lines,
and flushes them before forwarding the original draw. Until its 200-sample
budget is exhausted, every small indexed Reach draw also performs viewport and
vertex-buffer queries. Neither preserved run exhausted the budget.

The captured stride-24, six-index data is identical in both logs and belongs to
a fullscreen transition quad; it is not world-geometry or eye-differential
evidence. Candidate `a50da4a` removed only this optional detour. Its exact log
contains the disabled status, no draw/vertex samples, and a normally armed Reach
camera/stereo path. The black geometry remained unchanged, rejecting the detour
as its cause. The diagnostic remains disabled because its hot-hook GPU readback
and synchronous logging are independently unsafe; mandatory Present,
ResizeBuffers, and OMSetRenderTargets hooks remain unchanged. Evidence:
`out/test-runs/a50da4a-reach-world-black-draw-hook-removal-fail-20260729-063330`
(log SHA-256
`FD28C568CA1FE55E378B85DFCE8A62FBE33CEDC37CE63942A81FDD5F47604659`).

The same `0x121083` caller, entry
`haloreach.dll+0x120FDC` through `+0x1210D3`. Its exact entry signature is:

```text
48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 49 8B F0 0F B7 FA 45 84 C9 0F 84 C1 00 00 00 66 83 FA FE 0F 8F B7 00 00 00 F6 41 50 0F 0F 84 AD 00 00 00 0F BE 59 50
```

Its ABI is `void __fastcall(firstPersonWeapon, uint16_t markerIndex,
BoneMatrix* outMatrix, bool firstPerson)`. After the interpolation returns at
`0x121088`, retail masks the selected marker index and copies exactly one
`0x34`-byte `BoneMatrix` to `outMatrix`. The direct marker-output hook is
therefore limited to the returned local output-user-0 primary-weapon matrix;
it applies the same already-published record-space right-wrist delta used by
the visible first-person marker graph. It never edits the firing frame, hand
palettes, weapon tags, projectile direction, or an unbounded marker bank.

The ordering relative to the camera transaction is also exact. Retail
`main_render_view` calls `0x256724` at `0x0C32BE`; that calls `0x264530` at
`0x25674F`, which calls the FP builder `0x2AF648` at `0x2645F6`. Its
`0x2AF85A/0x2AF8F6` interpolation calls and immediately following palette
wrappers run before `main_render_view` calls `player_view_render` at
`0x0C33C4`. The prepared FP pair scope must therefore begin at the admitted
outer boundary and end in that outer call's `__finally`; arming it in the inner
player-view detour is too late.

The visible-palette consumer remains at `haloreach.dll+0x2B4EB0`. Its production
AOB is:

```text
48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 49 8B F0 0F B7 C9 4C 8B F2
```

The layout-discovery palette is classified only by the complete exact `boneMap`
fingerprint. Official HREK export proves that
`objects\characters\spartans\fp\fp.render_model` is the 47-node first-person
arms render model. Spartan has 47 outputs with render chains R `6/7/11`, L
`4/10/14`, camera `5`; Elite has 41 with R `6/9/14`, L `4/7/13`, camera `5`.
Mapping those validated outputs into animation-source order gives both layouts
R `6/9/13`, L `5/7/11`, camera `4`. The separate HREK
`objects\characters\spartans\fp_body\fp_body.render_model` has 82 nodes and is
a second visible consumer, not an unrelated attachment. Spartan weapon graphs
range through 65 live source nodes (the plasma-launcher case), and retail bounds
the source at 120, so palette output count and source count are never conflated
and output indices are never transplanted into source space.

The complete ordered output-to-source fingerprints are pinned verbatim:

```text
Spartan[47] = 0,3,1,2,5,4,6,9,10,8,7,13,12,14,11,22,26,18,21,24,19,20,15,23,16,17,25,36,35,32,28,33,29,27,34,31,30,46,38,42,37,40,39,43,45,44,41
Elite[41]   = 0,1,2,3,5,4,6,7,8,9,10,14,12,11,13,22,20,18,21,19,16,23,24,17,15,27,28,25,31,32,30,26,29,35,34,39,33,40,36,38,37
```

Preserved Spartan live evidence independently recorded tag `14674`, model
handle `5F8AEBE0`, count 47, and all 47 values above. The map pointer alternated
between two buffers while its contents remained identical, so production
classification compares the bounded contents rather than pointer identity.

A newly observed exact layout is recorded stock-only with its prepared serial.
It may activate only on a later stereo pair; the frozen pair selection and
explicit head-centre/right/left targets are shared by both eyes. The frozen key
is title generation plus interpolation view/id/slot and complete live source
count. It deliberately excludes the temporary source pointer: retail publishes
a source pointer per interpolation transaction, and pointer identity is used
only to pair that transaction with its immediately following final palette.

The palette half of the implementation mirrors Halo 3/ODST. Retail `0x2AF648`
permits four bounded
interpolation/palette transactions. Each successful Reach interpolation gets a
separate context and untouched source snapshot. `0x2B4EB0` consumes the newest
current context whose source pointer exactly matches its input, then reconstructs
the complete source graph into private 120-record scratch before the stock
builder applies that call's own `boneMap`. Consequently the 47-node arms model
and the separate 82-node first-person body model receive the same solved wrists.
The exact right/left hand descendants follow their respective wrists, while the
verified appended held-object range follows the right wrist inside that same
scratch reconstruction. No visible palette consumes the live marker graph.

A known layout-tag/map mismatch restores the complete untouched source and
invalidates activation at the next pair boundary. Unknown, stale-generation,
non-finite, out-of-range, or unsupported layouts remain wholly stock. There is
no body-only admission, source-owner partition, approximate mapping, or runtime
probe fallback. `arm_ik=1` solves both shoulder-elbow-wrist chains;
`arm_ik=0` uses the same full-source palette transaction with rigid controller
parenting. Missing left tracking leaves the authored left arm.

The Reach candidate path has one user-approved temporary Reach-only presentation
policy: Reach always emits floating hands regardless of the shared
`arm_ik`/`floating_hands` values. After the complete private source
reconstruction, the exact HREK/retail left-hand descendants receive a rigid
delta from their reconstructed wrist to the prepared left-controller wrist.
The visible keep mask remains the two narrow hand descendants plus the appended
held-object range. Immediately before hidden-node collapse, each side's four
auxiliary skin-influence records are co-located at its final solved wrist record
and assigned the same tiny collapse scale as other hidden nodes. The visible
right hand and appended held-object range remain on the right-controller
transaction. This all occurs in the same private scratch before the stock
palette builder; no live graph, Halo 3 path, ODST path, or shared configuration
is changed.

This branch/visibility split is pinned by official mod-tool geometry, not a
runtime ownership guess. The Spartan
`objects\characters\spartans\fp\fp.render_model` SHA-256 is
`FBC63B8C93569EBDBB8002FCAA1916AF6E525659EE43F360CB05CE9D7F2D3D4F`.
In both active arms meshes, 58 glove vertices cross the `l_forearm`/`l_hand`
edge (55 two-node vertices and three three-node blends), and all five
`flair_forearm` permutations are rigid on `l_radius`. Closing every weighted
edge from the visible left hand adds output nodes `l_upperarm`, `l_forearm`,
`l_humerus`, and `l_radius`, which map through the pinned fingerprint to source
nodes `{5,7,8,12}`. The Elite FP render model SHA-256 is
`A98AF9323023FFEB516DFA25525B4F5BAC216B021F1C8D5DD8A78797781BF2FD`;
its skin independently repeats the `l_forearm`/`l_hand` edge and shares that
exact source prefix. The hidden auxiliary mask is therefore `0x11A0`, producing
Spartan controller ownership `0x000003E0F81F99A0` and Elite ownership
`0x0000001E1E0F99A0`. Their original hand-only visibility masks remain
`0x000003E0F81F8800` and `0x0000001E1E0F8800` respectively.

The standard HREK mesh exports independently close the right side rather than
assuming symmetry. The Spartan export SHA-256 is
`CD9D2BE3060767814339E6C2E909E13AB7EDBAD6D07DF47DDFFC7BAA9AC3185A`.
Its glove has 59 `r_forearm`/`r_hand` cross-weight vertices (56 two-node and
three three-node) over 96 triangles, while the right rubber-suit contribution
has 71 vertices over 103 nondegenerate triangles spanning two to four auxiliary
bones. The Elite export SHA-256 is
`892F6E4337AF605FA160B3ED1418DB5785F0D9690F3C692643771D1B36EFDBAE`;
it has 51 right hand/forearm cross-weight vertices over 83 triangles and 75
right body vertices over 99 multi-auxiliary triangles. The right auxiliary
outputs are Spartan `{6,7,8,13}` and Elite `{6,9,10,11}`; both exact fingerprints
map them to source `{6,9,10,14}`
(`r_upperarm/r_forearm/r_humerus/r_radius`). Source 13 is the solved right wrist,
so the exact hidden mask is `0x4640`. It produces Spartan right ownership
`0x00007C1F07E06640` and Elite right ownership `0x000001E1E1F06640` while their
visible right-hand source masks remain `0x00007C1F07E02000` and
`0x000001E1E1F02000`. The mixed geometry shares parts/regions with wanted hand
geometry, so material or region suppression is not a valid substitute. Held
objects begin at sources 47 or 41 and cannot intersect the auxiliary mask.

Candidate `754b34b2acfdae81c6dbd833d3b7bd7b0e1e7b3d` proved that applying
the left rigid delta to that complete branch while later collapsing each hidden
record in place is insufficient. Its exact DLL SHA-256 was
`BE95F23246B5AAAD1C0C492C7C4660D3EEDB075D0A610B47BF924129C899EDD0`;
the headset showed a severe black strip from the left wrist. Direct standard
HREK `tool.exe export-render-model-mesh` output explains it. Both Spartan arms
meshes contain 71 left-side `spartan_rubber_suit` vertices and 103 nondegenerate
triangles, every one spanning two to four auxiliary bones. The glove has the 58
cross-weight vertices above over 96 triangles. Elite independently has 51
`l_forearm`/`l_hand` cross-weight vertices over 83 triangles and 96 body
triangles spanning multiple auxiliary bones. Collapsing the four auxiliary
records at their separate absolute joint translations therefore creates a
skinned strip between those pivots. Co-locating those records at source wrist
11 before applying the tiny hidden scale collapses the strip to one wrist-local
point without admitting arm geometry into the visible mask. The separate
Spartan/Elite `fp_body` meshes use lower-body nodes and are not this artifact.

Candidate `765604cc631a1c0042a468738c7545ffbbd9208a` then proved the
left-wrist anchor in the headset: the left ribbon disappeared. The same test
exposed the previously unanchored right auxiliary records as a severe strip at
the right/weapon wrist. Because those records were still collapsed at their
four separate right-joint translations, the right HREK geometry above identifies
the same mechanism independently. The forward candidate retains source-wrist
11 anchoring on the left and co-locates only right auxiliary mask `0x4640` at
solved source wrist 13 before collapse; hand visibility and held-object records
remain unchanged.

That palette transaction is necessary but not sufficient. Accepted Halo 3 and
ODST also bypass the native flat-screen weapon-IK stage after palette solving,
preventing a weapon-authored support-hand marker from overriding the controller
wrist. Candidate `abea61f0...` omitted this accepted stage and therefore was not
implementation parity despite reconstructing both observed Reach palettes.

Reach's corresponding control is title-proven rather than inferred. In pinned
HREK `reach_tag_test.exe`, the debug-variable descriptor at RVA `0x201AD98`
contains name pointer `0x1417E93B8`, type `5`, and value pointer
`0x144F40A60`; the name is exactly
`debug_animation_fp_weapon_ik_disable`. The homologous post-palette function
compares that byte at RVA `0x008D3162`; its `JNE` at `0x008D3169` targets
`0x008D338B`, the existing epilogue before the support-hand solve.

Pinned retail repeats the exact semantic edge. The debug descriptor is at
`0x00B3AEB8`, its name at `0x009F2AD8`, and its type is `5`. Unlike HREK's
development descriptor, retail leaves the descriptor value-pointer field
unpublished. The shipping consumer nevertheless directly references runtime
value slot `0x04E38B61`. The following 39-byte executable AOB occurs exactly
once at `0x002B506E`:

```text
41 0F B7 86 2C 53 00 00 66 85 C0 0F 8E 52 02 00 00 38 1D DC 3A B8 04 0F 85 46 02 00 00 48 8B 15 C6 88 99 00 0F BF C8
```

Its compare at `0x002B507F` resolves RIP-relative to `0x04E38B61`; its `JNE`
at `0x002B5085` targets the existing no-weapon-IK epilogue at `0x002B52D1`.
Production requires the descriptor name/type identity, unique AOB, decoded exact
compare target, decoded exact branch target, executable epilogue, and bounded
boolean value before setting it to one. It must not require HREK's published
development value pointer from retail. The original value is restored only
after every Reach detour is disabled and quiescent. Any mismatch leaves Reach
wholly stock. This is the Reach title adapter for the same accepted Halo 3/ODST
behavior, not a probe or fallback.

The exact clean runtime `6e31751c...` (installed DLL SHA-256
`49DC585C1E57FB54D197198E2A46AE95AA1CBF0FEE565EDCD62BBE740B9E8715`)
rejected the separated live-source owner in-headset: the log reported the
47-node path active over a 52-node live graph, yet the user observed no change
and the visible left hand remained stuck to the gun/right hand. It rejects the
separated source-owner method. The later exact `abea61f0...` runtime, DLL
SHA-256 `61C70876A8BC883D5277A7070EF38E2CB350476B6BFAFD1943B96C6EF67ADF91`,
reconstructed both palette transactions and produced the same headset result.
That later failure disproves the claim that the second palette transaction alone
was the remaining cause and isolates the omitted native weapon-IK bypass above.
Candidate `cd0a7c136caa2972d9d57f5e44929adb88b96069`, DLL SHA-256
`ECA7202AE4132AC18A6E8C403C0FE4322616E380FC098D05E4B16135FB25D174`,
incorrectly required that HREK-only published value pointer from the retail
descriptor. Its exact headset run passed cold preflight and display proof, then
failed the weapon-IK proof before installing Reach's camera core, producing
stock flat-screen output with no 3D. The run is preserved under
`out/test-runs/cd0a7c1-reach-no-3d-weapon-ik-proof-fail-20260725-053853Z`.
The `cd0a7c1` DLL remains installed unchanged under the user's explicit
no-rollback instruction; none of these failed methods advances the accepted
pointer.

The exact dirty runtime `e08b538f...-dirty` (installed DLL SHA-256
`236D06940F47E38876A54DF4AFE07E4C484AED2E025865AF55121E022E1772AC`)
rejected the previous whole-graph ownership model in-headset. Its log proved
both raw OpenXR poses valid/tracked, a correctly sided and moving left target,
zero target error for both body-palette wrist solves, and an active live-graph
path, while the user still saw the left forearm move with the hand stuck to the
gun/right-hand assembly. That result is preserved under
`out/test-runs/e08b538-dirty-reach-hands-parented-20260725-040508Z`. Together
with the unchanged `6e31751` result, it rules out both live-graph ownership
variants. Together with `abea61f`, it requires both halves of the accepted
H3/ODST architecture above: final-palette reconstruction and native weapon-IK
bypass.

The `f19f39e` root-only assumption is rejected. It called a locking pose getter
from the palette path, reapplied mount trim already present in the shared aim
pose, anchored controller displacement on a head-translated root, and emitted a
hot node dump. The installed test configuration also had `arm_ik=0`, which
explains why that gated root substitution appeared disabled. The production
candidate removes the dump and root builder, snapshots corrected right aim and
raw left tracking during prepared-frame publication, and logs only worker-read
atomic status.

Headset source `7ea6aca845a698f7994ef355e76a7361fe6f154e` ran with the exact
candidate DLL and `arm_ik=1`, but produced no layout or active-path status and
showed no IK. The installed hooks and maps were not the failure: that source
began the FP scope only inside the later player-view detour, after the proven
outer FP preparation chain above had already completed. The production
correction moves begin/end ownership to the admitted outer transaction, retains
only stereo-eye cleanup in the inner transaction, validates the frozen serial
there, and prevents a suppressed nested outer render from clearing the parent
FP context or overwriting its bounded live graph. The accepted pointer remains
unchanged pending a new exact headset test.

Both FP detours participate in callback accounting, relay/wrapper ingress scans,
disable/quiesce/remove rollback, and generation invalidation. Installation
requires exact-one AOB matches at both expected RVAs in executable memory; any
failure leaves stock Reach active.

## Local first-person native projectile origin

The next isolated Reach candidate changes only the projectile start point for
the exact local output-user-0 primary first-person weapon. It does not change
the player/controller aim direction, mutate a shared weapon tag, enable a
marker-direction flag, or admit AI, remote, third-person, or vehicle weapons.
The static proof is complete for the pinned binaries; headset acceptance and
the accepted-build pointer remain unchanged.

### HREK semantics and authored weapon evidence

The official HREK weapon barrel flag definition names bit 2 (mask `0x0004`)
`projectiles use weapon origin`: instead of the flat first-person camera origin,
the stock projectile transaction selects its already-computed weapon-marker
origin. Bit 15 (mask `0x8000`) is the separate
`projectile fires in marker direction` behavior. The candidate excludes bit 15
and therefore preserves the engine's stock player-aim direction.

In pinned `reach_tag_test.exe`, the projectile transaction is
`0xDE4290`-`0xDE8198`, with ABI
`void __fastcall(int weapon_datum_index, int16 barrel_index,
void* firing_context, bool simulation_or_prediction)`. Its weapon-origin
consumer is in `0xDE5289`-`0xDE52CA`. The official assault-rifle tag export has
an empty barrel-flags field; its first- and third-person firing attachments use
`primary_trigger`. HREK marker inspection places `primary_trigger` on the model
centerline with +X forward. The weapon-level `force contrails to come from
weapon barrel` secondary flag affects effects/contrails, not this projectile
origin decision. This evidence rejects both a global tag edit and any lateral
calibration offset.

HREK also supplies the ownership lineage. Call site `0x3723EA` invokes
`first_person_weapon_validate_weapon_index` at `0x8D2670`; that validator calls
the slot lookup at `0x8CEED0` and succeeds only when the incoming full weapon
datum occupies a first-person weapon slot.

### Retail projectile transaction and origin branch

The exact retail homolog is `0x4C2710`-`0x4C4923` (8,723 bytes), with the same
four-argument ABI. Its complete body SHA-256 is:

```text
F924A4C5D333268264654F22746417A7D0D006850150D73C0D5944694EA77F92
```

The manifest records a 71-byte relocation-tolerant entry AOB that matches once
at `0x4C2710` in the pinned retail executable sections. Wrapper
`0x4B6318`-`0x4B640A` directly calls it at `0x4B63BE`; the two proven upstream
wrapper call sites are `0x435EB6` and `0x4B62FE`. Preflight verifies the exact
entry match, complete function hash, and the direct `0x4B63BE -> 0x4C2710`
`rel32` edge.

The origin-selection block starts at `0x4C30AC`. Its exact 72 bytes occur once
in executable data and hash to:

```text
A1353752177431FBE68800410302A23ABB5DBEC5B8384F105E6B0D87C5B84C66
```

At `0x4C30C5`, exact bytes `41 22 C5 75 0A` (`and al,r13b; jne
0x4C30D4`) consume the native bit-2 result. The decision bytes hash to
`9EBF607CD187ED0E26B7FCE95DF3926F3533A2D1B3B25785027B00E34BE3FB67`.
The false continuation is `0x4C30CA`. The true continuation reaches the stock
copy at `0x4C30D8`-`0x4C30F4`, which moves the marker-derived origin from the
transaction's `+0x9F0` frame storage into the projectile-origin local. Nothing
in this bounded choice modifies projectile direction.

### Exact output-user-0 primary-slot admission

Retail leaf `0x2B1218`-`0x2B1273` implements
`int __fastcall(int output_user_index, int weapon_datum_index)`. It scans the
two first-person slots for that output user and returns slot `0`, slot `1`, or
`-1`; the user stride is `0x53A8`, slot stride is `0x2978`, and the full datum
is at slot `+0x3C`. Its exact 91-byte body SHA-256 is:

```text
ADBB51CBA6CBD7628E7D7B94B4AD0B4DF6C431D2F89BE315657A434B8B478DA9
```

The full-function AOB, with only its single RIP-relative displacement
wildcarded, matches once at `0x2B1218`. First-person consumer
`0x120FDC`-`0x1210D3` loads the full weapon datum and calls that helper at
`0x12101A`; preflight verifies the exact `rel32` edge.

The runtime admission is consequently exact: while the Reach VR transaction is
active, call the stock leaf with output user `0` and the projectile function's
incoming full weapon datum, and select the stock weapon-origin branch only when
the result is primary slot `0`. Slot `1`, `-1`, any inactive runtime state, and
all nonmatching datums retain the stock origin path. Static proof does not turn
this pending candidate into a headset-accepted build.

### Shared authored `primary_trigger` transaction

The centimeter-offset experiments are rejected. HREK proves that the official
assault-rifle `primary_trigger` is one marker on node `0`, has local translation
`(0.233427, 1.90735e-08, 0.0512855)`, scale `0.01`, direction `+X`, and an
effectively identity rotation. The projectile transaction's firing-frame
`+0x9F0` value is already world space, so applying that authored local vector
directly to it mixes coordinate spaces and can put the origin behind the
player.

Pinned retail function `0x11BFB0`-`0x11C00E` is the final first-person marker
composer. It reads the marker node index at record `+0x02`, calls the proven
first-person marker-query function `0x120FDC` at `0x11BFDC`, then passes the
returned interpolated node matrix and authored marker matrix at record `+0x0C`
to the matrix composer `0xA90D8` at `0x11BFED`. Its relocation-tolerant entry
AOB matches once at the expected RVA, and installation verifies both `rel32`
edges before any hook is created.

Candidate `2e1cbcba330dd9bc91c50db984680ae189905dca` is rejected. Its exact
installed DLL hash was
`6DC481CBCB7A2BF9CB4055C22D292C7D7539AC983D1EA52ED77F1BDC9B3F3C3F`.
The headset showed both muzzle flash and projectile remained displaced, while
the log proved the composer installed but never published a `primary_trigger`
world matrix and never wrote a firing frame. It does not advance the accepted
pointer and was reverted by `984aa18` before the corrected candidate.

The static control flow also exposes the muzzle error. The stock composer calls
`first_person_weapon_get_marker`, which calls the already-hooked interpolation
function. When that nested interpolation is inside the bounded render scope, it
has already moved the live marker graph exactly once before the stock query
copies its requested node. The old marker-query detour then applied the same
rigid delta again. Some effect queries occur after that scope and still require
the published transform, so removing the detour entirely would leave those
stock. The corrected query snapshots a thread-local source serial before its
stock call. If the nested interpolation advances it, the returned marker is
accepted untouched; otherwise, and only for the exact local slot-0 datum, one
generation-matched published delta is applied. The two branches are exclusive.

The corrected pending candidate hooks the serial-gated query and final
composer. The bounded interpolation publishes its verified record delta and
center root. The query advances a second thread-local corrected serial after
either nested ownership or the exclusive fallback succeeds. The composer
accepts a result only when its own stock call caused that corrected serial to
advance, which proves its result was corrected exactly once even when the outer
render scope is no longer armed. Only an exact HREK assault-rifle `primary_trigger` record for
output user `0`, first-person slot `0`, and the current full weapon datum is
published. Its world translation is copied into the same datum's native
firing-frame marker-origin slot immediately before the stock weapon-origin
branch consumes it. Direction, weapon tags, shared game data, other weapons,
and nonlocal firing transactions remain unchanged. This remains pending
exact-DLL headset validation and does not advance the accepted pointer. The
user explicitly approved this serial-gated once-only marker-query write.

## Camera-derived frustum-bounds proof

The original `viewport` hypothesis is now statically identified as the
camera-derived frustum-bounds helper. This is one building block in Reach's
camera path, not a safe stereo transaction boundary.

### Retail identity, signature, and boundary

The existing production `kBuildViewportSig` is:

```text
40 53 48 83 EC 30 44 0F BF 49 62 4C 8B D9 4C 8B 41 38 48 8B DA 0F BF 51 50
```

On the pinned Reach module it matches exactly once at RVA `0x287F58` when
scanned across executable sections in a Windows
`PAGE_READONLY | SEC_IMAGE_NO_EXECUTE` image mapping. The mapping executes no
code and does not load or attach to MCC. PE exception metadata bounds the
function at `0x287F58`-`0x2881CC`: 628 bytes and 154 instructions. No base
relocation overlaps the AOB or the complete function.

The complete relocation-normalized instruction sequence is identical to the
accepted Halo 3 helper at `0x2A63E4` and the accepted ODST helper at
`0x2CAC5C`. The comparison retained mnemonics, registers, structure
displacements, and non-control-flow constants and ignored only resolved
RIP-relative addresses and branch/call destinations. This cross-title match is
corroboration only; the Reach retail and HREK evidence below establish the
Reach-specific ABI and field meanings.

### ABI, callers, and data flow

The Windows x64 ABI is:

- `RCX`: compact camera input;
- `RDX`: writable four-float frustum-bounds output;
- `AL`: `true` on return.

The retail helper is a leaf with no direct or tail callees. Its nine direct
call sites are `0x1D2F4C`, `0x1D3594`, `0x1D3C4D`, `0x25B47C`, `0x25D41A`,
`0x26C2FF`, `0x26FB1B`, `0x286DD8`, and `0x28DB6D`. At every site, the next
direct call is the projection builder at `0x2884BC`, with the same camera
pointer and the four derived floats. This proves the output's downstream role,
but the nine-call-site fan-out also proves this helper is shared and cannot by
itself identify the player-eye render transaction.

The first-person path is one of those callers. Inside retail function
`0x286C6C`-`0x286EBC`, it copies the `0x90`-byte compact camera from `[RCX+8]`
to the dedicated nested first-person camera-stack workspace at `0xCFAC20+0`,
calls the frustum helper at `0x286DD8`, calls the projection builder with the
same camera/bounds and the current camera-stack workspace's `+0x1E4` derived
block at `0x286DEF`, then reloads the live stack top and tail-jumps to the
two-pointer uploader `0x282D60` at `0x286E6A`. The uploader receives compact
`0xCFAC20` and derived `current_top+0x1E4`; its ABI is
`void __fastcall(const void* compact,const void* derived)`. Its exact retail
body is `0x282D60`-`0x282ED9`.

The pinned retail identities are:

- rebuild entry AOB: `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 4C 8B 41 08 48 8D 05 ?? ?? ?? ?? 48 8B D9 0F 29 74 24 30`, one executable match at `0x286C6C`;
- rebuild body SHA-256: `125656AF65F61F02BA482830D307EBFDD00BBE2DF7264F155613B3FF8FAEFE58`;
- uploader entry AOB: `48 8B C4 48 89 58 08 55 48 8D 68 A1 48 81 EC C0 00 00 00 0F 29 70 E8 4C 8D 45 F7 0F 29 78 D8 48 8B D9 48 8B C2`, one executable match at `0x282D60`;
- uploader body SHA-256: `F9A6578992870A9F5BF8C733944A83A5A834CC95490D8D0CC7573021F452FD31`.

The visible retail wrappers establish the lifetime and exact destination rather
than merely the rebuild's isolated data flow:

| Wrapper | Body SHA-256 | Outer / nested workspace setup | Callback, push, view, rebuild, pop |
| --- | --- | --- | --- |
| `0x26DA08`-`0x26DBFA` (`0x1F2`) | `03BCCB8401EBC487394F49C96DDB5B20309F3E1BB9D16F96DD5860D2155EA175` | `0x26DAB0 -> 0xC9FAE0`; `0x26DACB -> 0xCFAC20` | callback `0x26DB78 -> 0xC380`; push `0x26DB82 -> 0x251C08`; view `0x26DB87 -> 0xBB8F68`; rebuilds `0x26DB93`, `0x26DBAE`, `0x26DBCC -> 0x286C6C`; pop `0x26DBDB -> 0x251C50` |
| `0x26E2A0`-`0x26E4FD` (`0x25D`) | `C36932D4D9D1357AD3D50F750A62C51DAE4530F350A7FA4E96AA04DC284171C2` | `0x26E38C -> 0xC9FAE0`; `0x26E393 -> 0xCFAC20` | callback `0x26E3B2 -> 0xC380`; push `0x26E459`; view/rebuild `0x26E461/0x26E468` and `0x26E4B0/0x26E4B7`; pop `0x26E4DD` |
| `0x26EA78`-`0x26ED8C` (`0x314`) | `2B3033F4D4FB62E0AD709F38688D7A4613B2AFDE5CC67432254B51FB4F35A649` | `0x26EB4C -> 0xC9FAE0`; `0x26EB59 -> 0xCFAC20` | callback `0x26EB78 -> 0xC380`; push `0x26EC22`; view/rebuild `0x26EC34/0x26EC3B` and `0x26EC77/0x26EC7E`; pop `0x26ED67` |

All three wrappers install callback `0xC380` at nested `+0x2A8`, push through
`0x251C08`, pass the one exact FP view `0xBB8F68` to `0x286C6C` while the
nested workspace remains current, and pop through `0x251C50` only after the
rebuild returns. The rebuild independently reloads the live stack top at
`0x286DAA`-`0x286DC5` before selecting `top+0x1E4` at `0x286DCA`, then reloads
it again at `0x286E20`-`0x286E48`; its uploader arguments are compact
`0xCFAC20` at `0x286E4F` and that live `top+0x1E4` before the tail jump at
`0x286E6A`.

Pinned HREK independently exposes the same transaction. Getter `0x87C4A0`
(`48 8D 05 79 DB 5B 04 C3`) returns the dedicated nested workspace
`0x4E3A020`. Source-named `player_view_render` `0x834490`-`0x835598` calls that
getter at `0x834A19`, selects callback `0x87C780` at `0x834A21`, pushes through
`0x7E6E60` at `0x834A28`, loads view `0x4E3A2D0` at `0x834A2F`, calls rebuild
`0x87C4B0` at `0x834A36`, and pops through `0x7E6F50` at `0x834A72`. Two more
homologous wrappers repeat getter/push/rebuild/pop at
`0x836260/0x83626F/0x83627D/0x8362B9` and
`0x8364CE/0x8364DD/0x8364F5/0x8365B4`.

HREK rebuild `0x87C4B0`-`0x87C6FB` (SHA-256
`84A1A55BBDBF98E6496CC47E9F644602B81617AB7283F94C37A88122B628A4B0`)
gets the current stack top through `0x7E7110` at `0x87C639`, uses its `+0x1E4`
at `0x87C655`, gets the top again at `0x87C679`, uses `top+0x1E4` at
`0x87C685`, and calls uploader `0x8561A0` at `0x87C68C`. The uploader body is
`0x8561A0`-`0x8564FD` (SHA-256
`78D0DA6E00ED0E6564AE9FAA78A818EE50243DA2968F7ED55977803B2ABE8AF2`).
This closes the static identity, ABI, nested-workspace layout, and lifetime
proof.

### Rejected correction: native FP rebuild as the black-world fix

The black-static-world investigation tested an assumption in the accepted E490
hook shape: that Reach's title-specific native FP rebuild must remain the final
writer. The static binding facts below remain valid, but the exact candidate was
headset-rejected and must not be treated as a correction or causal finding.

Pinned retail `0x286C6C` and HREK `0x87C4B0` read only the compact source
pointer at `view+0x08` and the weapon/AA scale at `view+0x10` from their view
argument. They then:

- copy the complete `0x90` compact into the active nested workspace;
- publish `render_first_person_fov_scale` and apply it to compact `+0x28`;
- on the first-person path, use the weapon trigger-marker AA inputs to adjust
  compact `+0x64`, which HREK's camera assertions identify as `z_near`;
- rebuild nested derived `+0x1E4`, then run the current-blur transform over its
  `+0x78..+0xB8` matrix region; and
- call the uploader, which dirties renderer constants `0x4D0000`,
  `0x4D0004..7`, and `0x530000`.

Official HREK shader metadata names buffer `0x4D` `ViewVS` and buffer `0x53`
`ViewPS`. The global HLSL list maps those slots to `View_Projection`,
`Camera_To_World`, and `Camera_Position_PS`; compiled terrain shaders consume
both buffers. This is a shader-constant ownership fact, not an SSAO, shadow,
rain, depth-buffer, or culling hypothesis.

The old detour called the native rebuild first, copied the admitted eye's raw
world compact over the finished nested compact, copied the raw world derived
block over the finished native derived block, and manually called the uploader
again. It therefore erased Reach's FOV, weapon-dependent near/AA, and derived
postprocess results. The first-person single-pass draw sits immediately before
the static-lighting transaction. The camera-stack pop does invoke the restored
outer callback, so the evidence does **not** claim that the nested constants
simply remain live forever; the invalid state is active during the FP pass that
precedes and shares the render transaction with static lighting.

Official HREK closes the actual persistence boundary. The
`first_person_single_pass` setup at `0x834877/0x834892` reaches `0x7CB180`, whose
`0x8331C0` target bind uses surface 9 as depth/stencil and surfaces 16/17 as
color targets. The name-pointer table has `0x58`-byte stride from
`_surface_none`: entries 9, 16, and 17 are exactly
`_surface_depth_stencil`, `_surface_albedo`, and `_surface_normal`. It then sets
material pass 6 and performs the nested FP-camera draw. Static-lighting setup
through `0x7CB200` selects surface 15 (`_surface_accum_LDR`) as its color target
but binds the same surface 9 depth/stencil at `0x7CB241`; that setup runs at
`0x834CEA` immediately before first-person static lighting and world
`render_static_lighting`, and `0x8373EE` binds it again inside the latter.
HREK `0x7CAD20` maps FP material pass 6 and apply-static-lighting pass 2 to
their distinct depth/stencil modes. This proves only that both passes share an
attachment under distinct modes. It does not prove that FP coverage caused the
reported pixels. The headset result below rejected the native-rebuild change
while leaving the black static-world geometry unchanged.

The rejected candidate used a local `0x18`-byte proxy: it preserved the real
view header and weapon scale, changes only `+0x08` to the exact admitted eye
compact, and lets Reach perform its native true/false rebuild and upload once.
The proxy has synchronous stack lifetime, the exact hashed body reads no other
view field, and the real view is still used for the full nested-workspace,
callback, active-view, specialization, generation, eye, and serial gate. The
runtime proof separately required draw and post-draw-restore masks `0b11` for
the same prepared serial before reporting ACTIVE. The exact Steam / SteamVR
2.17.6 / PSVR2 120 Hz run did report ACTIVE, but the user observed a complete
regression: the black static-world geometry was unchanged, 3D was broken, and
interior visibility was no longer correct. Candidate source is
`5a4101caab5c9f22262703323caa3cdedef6265e`, package
`out/candidates/5a4101c-reach-fp-parity-20260729-153523252Z`, exact installed
DLL SHA-256
`833B249ED970EC91AA2B2964017427F944596C49CB9C5E5DB8EACCA2E359F06A`.
The preserved log is
`out/test-runs/5a4101c-reach-native-fp-rebuild-fail-20260729-103809/halo3xr.log`
(SHA-256
`ACCABA6DED2F9B9507D9F7711EC2814F9D93AE4A1F856012F42E25BB086907F7`).
The candidate behavior is reverted before any subsequent candidate.

### Outer eye camera commit before world rendering

Halo 3's headset-confirmed behavior being matched is precise: after rebuilding
each eye camera, the hook uploads/prepares that eye through the engine before
calling the eye's world renderer. Halo 3 does so at `game.cpp`'s
`g_fpCameraUpload` / `g_prepareView` edge; ODST performs the homologous
`fpCameraUpload` / `prepareView` edge. Reach's inner stereo path rebuilt its
compact, derived, `player_view+0x3B0`, and `player_view+0x490` CPU blocks but
previously entered `player_view_render` without an equivalent renderer commit.

The pinned retail outer camera-stack callback is
`haloreach.dll+0x0026BFD4..0x0026C040` (`0x6C` bytes, SHA-256
`6E2A249710A53498ADE7AFB12EE7414099D16315B2F06D90D8EC01D185E6B0C4`).
Its 28-byte entry signature, wildcarding only the two RIP-relative global
displacements, has exactly one executable match at that RVA. The stock push at
`0x251C08` stores the callback in workspace `+0x2A8` and invokes it at
`0x251C44`; pop invokes the newly exposed top callback at `0x251C7E`.

The callback reads the active player view, calls `0x2505B0` at `0x26C028`, then
loads active `player_view+0x490`, sets the bank selector to true, and tail-jumps
to `0x282CA8` at `0x26C03B`. Static inspection of `0x2505B0` and its official
HREK homolog shows an idempotent viewport/scissor rebind and dirty-state update,
not a temporal-frame advance. `0x282CA8` uploads ViewVS `0x4D0000/0x4D0004`
and ViewPS `0x530000` from that current matrix bank.

Official HREK independently exposes the same outer transaction. Both normal
`main_render_view` paths install callback `0x837140` and push it through
`0x7E6E60`; the callback calls the viewport/scissor path at `0x7C4900` and the
ViewVS/ViewPS uploader at `0x856500` with active `player_view+0x490`. Its
camera-stack design explicitly makes it a state-rebind callback safe to invoke
whenever the active stack camera changes.

The pre-candidate hook order was therefore:

1. stock outer push commits the head-centre camera;
2. the hook rebuilds the first eye's CPU camera and current-matrix bank;
3. `player_view_render` begins without committing that eye;
4. only the later, weapon-dependent nested FP rebuild uploads an eye camera;
5. the hook restores CPU blocks between eyes without a renderer commit, then
   repeats the omission for the second eye.

The user-provided paired screenshots show the same scene and pose with exact
static rock/terrain receivers black under the selected sniper and normal after
a weapon switch, while sky and HUD remain intact. The user reports retained
geometry/depth and a different pattern per eye. Those observations do not by
themselves prove which renderer state is wrong; they establish the acceptance
symptom. The missing native commit above is a code fact and the candidate under
test, not yet a headset-confirmed cause.

The candidate adds exactly one behavioral edge: after each per-eye
camera-state/matrix rebuild and before `player_view_render`, invoke the verified
no-argument outer callback. It does not alter compact-camera values, projection,
culling policy, first-person rebuilds, depth/stencil modes, SSAO, shadows, or
head/controller transforms. A preallocated atomic status records generation,
prepared serial, and eye mask; the cold worker reports both-eye ACTIVE, including
the armed-but-zero-sample install state, without logging or locking in the
render hook.

Source `74e1477d7ae02ea57f754ac76dfd99678d9028ec`, package
`out/candidates/74e1477-reach-fp-parity-20260729-161950871Z`, exact DLL SHA-256
`5BB673ABBA8BA9D4B0BC667D1009CE41880ABCB1F8EFDC31F7E118659C20C898`
is headset-accepted. The Steam / SteamVR 2.17.6 / PSVR2 120 Hz log reports the
unconditional BOUND line at 11:24:04 and a completed both-eye ACTIVE transaction
at 11:24:10. With the headset on, the same-scene sniper versus alternate-weapon
toggle no longer produced black static rock/terrain surfaces in either eye, and
the previous candidate's 3D/interior regression was absent. The user reported
“ITS FIXED.” The accepted log is preserved at
`out/test-runs/74e1477-reach-outer-camera-commit-pass-20260729-112718/halo3xr.log`
(SHA-256
`064BD5E47FDE39F8E1D66EC4BA376372D5B974EFD32ED2FE4CD79ADFAE0BF3F9`).
This advances the accepted development pointer: the missing outer-eye native
camera commit is the headset-confirmed cause/fix for the reported black static
world surfaces.

The earlier camera-only candidate
`6e12536ce401772876d55de0821780546af04131`, package
`out/candidates/6e12536-reach-fp-parity-20260725-083057894Z`, exact DLL SHA-256
`D5B8479952C5016ED2A636E9D2EFB468DED41C9D1BB44FEDE2BA1779FE0062D2`,
produced no visible first-person camera change in-headset. That exact negative
result independently corroborates the impossible outer-workspace gate found by
the subsequent static review and does not advance the accepted pointer.

Candidate `a1dcb7beeb0bec56b3b7c04a6f15a897eaa63fa4`, package
`out/candidates/a1dcb7b-reach-fp-parity-20260725-085113864Z`, installed DLL
SHA-256 `68793B3052EE2AE60F197526A7A215913FA7EEEFC3BB4177A613EE4AAFFF70A0`,
produced a partial/failed headset result: the hands were a little better, but
the gun/both-hands FOV, projection, apparent location, and tracking-distance
coverage remained incorrect. The log proved hook installation/camera-core
arming and execution of the private palette path (`Reach FP forced
floating-hands active`); it did not prove execution of the post-original camera
substitution.

Static review found the exact cause. `a1dcb7b` required the current camera-stack
top to equal outer default workspace `0xC9FAE0` and selected
`0xC9FAE0+0x1E4`. During every normal visible FP rebuild the top is instead the
pushed nested workspace `0xCFAC20`, so that guard necessarily returned before
the copy and uploader transaction. Even without the guard, the outer derived
destination was wrong: the exact destination is `0xCFAC20+0x1E4 = 0xCFAE04`.
Corrected candidate `fe0c48e4282fc17d05ca2de0d8dcaa36c95483dd`, package
`out/candidates/fe0c48e-reach-fp-parity-20260725-094807875Z`, installed DLL
SHA-256 `5D1767B492A5369EABAD312B10F5528588D76B2AD4B647BCB64284717F310E1A`,
then reported both-eye nested world-camera uploads in-headset. The next user
observation isolated a small left fragment/ribbon following the right
controller and a small hand/gun offset during physical head turns. The runtime
also reported the 47-over-52 private palette and native weapon-IK bypass active,
so the fragment is the hidden left influence leak proven above rather than a
projection failure.

Candidate `754b34b2acfdae81c6dbd833d3b7bd7b0e1e7b3d`, package
`out/candidates/754b34b-reach-fp-parity-20260725-115117345Z`, installed DLL
SHA-256 `BE95F23246B5AAAD1C0C492C7C4660D3EEDB075D0A610B47BF924129C899EDD0`,
then produced a second failed headset result: the left hand was slightly better,
but the black wrist strip remained severe and the physical-head-turn drift was
unchanged. The exact runtime identity, 47-over-52 Spartan palette, weapon-IK
bypass, and both-eye nested projection were verified. HREK mesh evidence above
isolates the remaining strip to the four separately positioned hidden collapse
pivots, so the forward wrist candidate replaces that failed behavior with one
solved-wrist collapse anchor.

Candidate `765604cc631a1c0042a468738c7545ffbbd9208a`, package
`out/candidates/765604c-reach-fp-parity-20260725-121927950Z`, installed DLL
SHA-256 `196BE16C85D2837DA9E8822FC896122F6CFC47653196FD020F9D0109D2F804AE`,
then produced a narrow left-wrist headset pass and residual right-wrist failure:
the left strip was gone, but the screenshot showed the equivalent severe strip
extending from the right/weapon wrist. The exact run log is preserved at
`out/test-runs/765604c-reach-left-pass-right-wrist-fail-20260725-122522Z/halo3xr.log`,
SHA-256 `7E18D3D53127A1B5235B99C4395E01DA9C8296CDE5CFCE55B5B228F02D730F57`.
It again proves the 47-over-52 Spartan private palette, native weapon-IK bypass,
and both-eye nested projection. The independently counted right HREK mesh
weights above isolate the residual to auxiliary source mask `0x4640`; the next
wrist candidate retains the working left anchor and adds only the solved right-
wrist collapse anchor.

Candidate `7467d264957b9753a29f7e003b7415b8d888adfb`, package
`out/candidates/7467d26-reach-fp-parity-20260725-124219703Z`, installed DLL
SHA-256 `7CBED662A7428644FDAAD58A780FEE329900436E620CDBC0D5B65640E710C057`,
then produced the expected cumulative wrist result: the user reported the
right-hand fix was great, with the previously accepted left-hand fix retained.
The only reported first-person ownership residual was the gun and both hands
still following physical head motion slightly. Its exact run log is preserved
at
`out/test-runs/7467d26-reach-both-wrists-pass-head-drift-20260725-125800Z/halo3xr.log`,
SHA-256 `F15C1C1A141E0315F0DF6D3A6E0F8B7238E91E5844A0A490787C372FA4BB1FA0`.
It proves the title-native weapon-IK bypass, forced 47-over-52/53 private
palettes, both-eye nested world projection, zero frame-order failures, and
clean teardown.

The head-turn offset is independent. `ReachBuildPreparedControllerTarget`
already publishes the physically correct absolute target `B+C`, where `B` is
the pre-head gameplay base and `C` is tracked controller displacement. The
Reach-only `ReachRebasePreparedControllerTargets` then substitutes
`R+(B+C-B)=B+H+C` at both marker and palette consumers, where render root
`R=B+H` includes tracked head translation. That double-adds `H`; a physical
turn around the neck produces the reported small drift. Halo 3/ODST have no
equivalent rebase: their accepted controller-world target is built once from
the pre-head gameplay base plus full controller displacement, and the render
root participates only in the world-to-record conversion. Rejected candidate
`f19f39e` already recorded the same head-translated-root error class. The
forward isolated candidate therefore removes the Reach-only rebase at both
marker and final-palette consumption while retaining the absolute targets,
center/render roots for coordinate conversion, both wrist anchors, and the
exact `1/3.048` world-unit mapping. This does not advance the accepted pointer.

The same HREK binary publishes `render_first_person_fov_scale` as a type-6
float debug control (`0x17E2250` name, `0x2015350` descriptor,
`0x205A0C8` value); retail publishes the homolog at `0x9EB9F0`, `0xB408E8`,
and `0xB445F4`. Official `tool.exe export-tag-to-xml` exports of a Reach weapon,
`globals`, and `rasterizer_globals` contain no first-person FOV field. The
narrow viewmodel FOV is therefore engine control state, not a weapon-tag field.

### HREK semantic corroboration

In the pinned HREK `reach_tag_test.exe`, the homologous function is
`0x823930`-`0x823D16`. Its retained assertions identify:

- `RCX` as `camera` and `RDX` as `frustum_bounds`, from
  `render/render_cameras.cpp` lines 443-444;
- the ordered `camera->render_pixel_bounds` x and y ranges at source lines
  451-452.

It performs the same field reads, scaling, four-float writes, optional custom
projection adjustment, and `true` return as the retail function. This is
independent title-specific semantic evidence; the editing-kit executable is
not byte-compatible with the MCC DLL and is not an AOB-count target.

### Consumed compact-camera fields

Reach rectangles use signed 16-bit `y0,x0,y1,x1` order. Retail initialization
at `0x287D60`-`0x287DF9` and the HREK assertions establish the following
consumers and producers:

| Offset | Static Reach role |
| ---: | --- |
| `+0x38..+0x3F` | `window_pixel_bounds`; initialized from `+0x4C` and consumed as the active window rectangle |
| `+0x40..+0x47` | 5%-95% inset/title-safe rectangle copied from `+0x54`; exact member spelling remains open |
| `+0x4C..+0x53` | `render_pixel_bounds`; initialized to zero origin plus current render extents and used as the scaling denominator |
| `+0x54..+0x5B` | 5%-95% inset rectangle; copied to `+0x40` by the initializer |
| `+0x5C..+0x63` | current client/display bounds; producer `0x24F48C` reads `GetClientRect`, applies an eight-pixel minimum, and writes zero origin plus bottom/right extents |
| `+0x7C` | custom projection-window enable |
| `+0x80`, `+0x84` | custom horizontal and vertical center offsets |
| `+0x88`, `+0x8C` | custom horizontal and vertical extent scales |

The helper writes exactly four floats at output offsets `+0x00..+0x0F`.
Retail constants used by the centering/scaling math are `0.5f` and `2.0f`.

### Remaining gate

The exact external stock observer repeated the exactly-one scan and loaded
range checks successfully in two admitted sessions. Any future production
runtime must independently repeat that cold preflight and refuse hook installation
or VR ownership on a mismatch. The player-view transaction proof below still does not make this
shared helper a safe hook. Caller scope, hook-time camera/workspace lifetime
and full snapshot/restore, live capture-target lifetime, production
finite/range guards, teardown, and headset validation remain required.
Therefore `proof_complete` and `hook_eligible` remain false.

## Player-view prepare/render transaction

The stock player-view owner, object stride, same-call freshness, exact normal
and screenshot caller scopes, camera-workspace lifetime, final display target,
late native-CHUD order, and transaction-scoped active-view clear are now
statically identified. This closes the corresponding static questions only.
Stock Reach has output-user/player-view transactions, not VR-eye transactions.
At this stage of the historical static proof no Reach hook or eye capture existed;
the unaccepted candidate described at the top now supplies both behind the
runtime gates derived below.

### Retail owner and boundary

Retail `main_render_game`, bounded by PE exception metadata at
`0x0C33F8`-`0x0C3B1E`, owns the normal player-window loop. For each active
entry it:

1. indexes the player-view array;
2. calls setup `0x26C204`-`0x26C6DC` at `0x0C36D6`;
3. writes the last-window flag at player-view `+0xA30`;
4. calls `main_render_view` `0x0C31F4`-`0x0C33F7` at `0x0C3730`.

The `main_render_view` ABI is `RCX = rasterizer-camera workspace`,
`RDX = player_view*`, and `R8D = player-window index`. The normal caller passes
workspace RVA `0x00C9FAE0`. The function creates frame textures, computes
visibility, and calls `player_view_render` `0x26C6DC`-`0x26CFE6` at
`0x0C33C4`.

Its complete body SHA-256 is
`95DF3EFFF9AC6EE29887D1272CCA8D7BF3E58F87041BAD8032107825B733FE89`.
The following exact 32-byte entry occurs once in the pinned retail `.text`
bytes:

```text
40 53 56 57 48 81 EC 80 00 00 00 0F 29 74 24 70
48 8B 05 05 6E A3 00 48 33 C4 48 89 44 24 68 41
```

That is offline identity evidence, not loaded-image authorization. The pinned
retail image has exactly two direct rel32 callers:

- normal player rendering at `0x0C3730`, return RVA `0x0C3735`;
- screenshot rendering at `0x1D3864`, return RVA `0x1D3869`.

The second call is inside helper `0x1D3784`-`0x1D3892`. It passes workspace
`[RSI+8]`, player view `[RSI+0x10]`, and index zero, so parameter checks cannot
distinguish it from a normal slot-0 transaction by ABI/index alone. Its owners are Reach's
screenshot dispatcher and high-resolution tile/bloom loops. The pinned HREK
independently labels the matching paths `screenshot bloom`, `screenshot tile`,
`render screenshot`, and `display tile`; its corresponding calls to
source-named `main_render_view` `0x1CC690` are at `0x50AC49` and `0x50D481`.
The retained `main_screenshot.cpp` warning that multi-view screenshots capture
only the first player explains the hard-coded index zero.

Future production routing must therefore whitelist exact return RVA
`0x0C3735`. Return RVA `0x1D3869` and every unknown caller must execute the
stock original exactly once, without camera mutation, capture, or stereo
duplication. This resolves the alternate caller's semantics; implementing and
validating that routing remains a hook gate.

### Four player views and the `0xA40` stride

The retail player-view array starts at RVA `0x029F2B90`. Static initializer
`0x6210`-`0x6246` constructs exactly four objects by calling
`0x25B0A8` and advancing `0xA40` at `0x622E`. Independently, the live render
loop multiplies the active index by `0xA40` at `0x0C366A`, passes that object to
setup, then advances its current-view pointer by `0xA40` at `0x0C3879`.

The pinned HREK independently repeats both facts:

- array RVA `0x04D8AED0`;
- initializer `0x20AA0`-`0x20AD6`, four calls to constructor `0x833660`,
  with the `0xA40` increment at `0x20ABE`;
- runtime index multiply at `0x1CBC80` and loop increment at `0x1CBF65`;
- setup call `0x1CBD3A -> 0x837F80`;
- render call `0x1CBF22 -> 0x834490`.

Thus `0xA40` is the Reach player-view object stride and extent. It is **not**
permission to transplant a Halo 3 or ODST prepared-view layout. Reach
player-view `+0x08` is an observer/state pointer, while the compact and derived
cameras live in the separate rasterizer workspace.

### Same-transaction freshness and active-view lifetime

Retail setup materializes the default workspace at RVA `0x00C9FAE0` in the
same player-loop iteration. It initializes the camera, calls the proven
frustum helper at `0x26C2FF`, calls the projection builder at `0x26C316`, and
copies:

- compact camera `+0x000..+0x08F` to secondary `+0x154..+0x1E3`;
- derived block `+0x090..+0x153` to secondary `+0x1E4..+0x2A7`.

The owner calls setup at `0x0C36D6`. From setup's return at `0x0C36DB` to the
`main_render_view` call at `0x0C3730` there are exactly 16 instructions and no
call or branch: four viewport-global writes, the last-window byte write at
player view `+0xA30`, and argument setup. This proves the hook entry receives
the same-iteration workspace. The one global workspace is reused for every
player iteration, so no future eye path may retain it beyond the synchronous
normal render call.

The camera-only workspace occupies `+0x000..+0x2A7`, but the exact render-scope
snapshot is `0x2B0` bytes:

| Offset | Size | Static role |
| ---: | ---: | --- |
| `+0x000` | `0x90` | primary/rasterizer compact camera |
| `+0x090` | `0xC4` | primary derived block |
| `+0x154` | `0x90` | secondary/render compact camera |
| `+0x1E4` | `0xC4` | secondary derived block |
| `+0x2A8` | `0x08` | camera-stack callback written by the render-scope push |

HREK independently repeats that `0x2B0` layout at
`0x04D86C10`-`0x04D86EC0`, and retained assertions name the primary and
secondary members `get_rasterizer_camera()` and `get_render_camera()`.
Normal setup mirrors both compact cameras and both derived blocks. The
screenshot path proves a second intentional policy: it retains the base
secondary compact camera while applying a custom projection to the primary,
then mirrors the rebuilt derived block. Static evidence therefore does not yet
choose the correct secondary-camera policy for OpenXR; every block still has
to be accounted for explicitly.

`main_render_view` stores the current `player_view*` in global RVA
`0x04E389A8` at `0x0C323C`, pushes the workspace/callback scope at
`0x0C3246 -> 0x251C08`, renders at `0x0C33C4`, begins its post-render
continuation at `0x0C33C9`, pops the camera scope at
`0x0C33CE -> 0x251C50`, then clears the active view at `0x0C33D3`. The normal
CFG has no return that bypasses the paired pop and clear after the set.

The push is a bounded stack, not an unconditional assignment. Retail depth is
RVA `0x00B43ABC`, is initialized to the empty sentinel `-1`, and pointer slots
begin at RVA `0x00C878A8`. Valid pre-push depths are `-1` through `2`; the push
increments them to valid current depths/slots 0 through 3. `0x251C08` silently
skips the push when the prior depth is already at least 3. The exact normal call supplies callback
`module+0x0026BFD4`, which a successful push stores at workspace `+0x2A8`.
The outer token must capture the pre-push depth. Production inner admission
must require the pre-push depth to remain within `-1..2`, the active-view global
to equal the candidate `player_view*`, current depth to equal captured pre-push
depth plus one and remain within `0..3`,
the new top slot to point to the admitted normal workspace, and its callback
qword to equal `module+0x0026BFD4`. This detects a silently skipped push even
if a prior top slot happens to alias the workspace. Overflow, nested-owner,
callback, depth, or top-pointer mismatch before an owner token remains untouched;
the same mismatch after ownership rejects and suppresses the claimed transaction.

HREK supplies the semantic proof. Inside `main_render_game`
`0x1CB8D0`-`0x1CC001`, the retained `main_render_view` profile envelope is
`[0x1CBE36,0x1CBF51)`. It sets the active view at
`0x1CBE89 -> 0x837B70`, creates frame textures, computes visibility, calls
source-named `player_view_render` at `0x1CBF22 -> 0x834490`, and clears the
active view with null at `0x1CBF33 -> 0x837B70`. Retained
`render_player_view.cpp` records at lines 2504, 2505, 2507, and 2508 name the
stored value `g_player_view_stack_element` and validate both render-camera and
rasterizer-camera position/up/forward vectors.

This proves fresh setup relative to the immediately following stock
transaction and proves its engine-owned set/use/clear bracket. It does not
authorize a whole-function double call. For slot zero, `main_render_view`
executes a frame-once block at `0x0C3251`-`0x0C3296`; calling the whole function
twice would execute that block twice. It would also pop and clear the
engine-owned active scope after the first call. A future stereo boundary must
stay inside the existing scope and account for all transitive player-view and
global side effects. The observed continuous heartbeat and one-second interval
must still be enforced by production, and transition/device-loss/thread
teardown remain open.

### Pre-inner visibility camera consumer

The failed `f953bbe3` headset result required isolating which of the two proven
workspace cameras drives CPU visibility. Retail `main_render_view` supplies the
default workspace at its cluster lookup:

- `0x0C3319` loads workspace RVA `0x00C9FAE0` into `RDX`;
- `0x0C3320` calls `0x273458`;
- that callee executes `lea rcx,[rdx+0x154]` at `0x273468`; and
- the subsequent visibility call at `0x0C335C -> 0x27F408` receives the
  secondary compact camera at `+0x154` and secondary derived block at `+0x1E4`.

The pinned HREK homolog `0x8339B0` is profile-named
`player_view_compute_visibility`. Its call at `0x833A2E -> 0x839230` repeats the
same `+0x154` compact-camera query and passes the corresponding secondary
derived block. This is independent semantic corroboration that visibility is
camera-derived and consumes the secondary/render pair before
`player_view_render`.

Production rechecks both live `rel32` call targets, the exact
`lea rcx,[rdx+0x154]` bytes, and the RIP-relative secondary-derived address at
install. The normal outer hook may therefore rebuild and mirror one
head-centred binocular-union camera into the already-proven primary/secondary
workspace pairs before calling the original exactly once. It must not call the
whole outer function or visibility builder twice, hook the nine-caller frustum
helper, write a guessed visibility buffer, or apply a fixed angular pad.

Retail visibility exposes only one admitted projection: `0x27F4A0` sets its
projection count to one, and pinned HREK does the same at `0x855FB1`. A generic
lower layer appears multi-projection-capable, but its ABI/layout/lifetime are not
proven and it is not used. Reach-specific evidence does identify compact-camera
`+0x64` as the dynamic near plane: retail projection builder `0x2884F4` reads it,
HREK `0x823553` reads it beside the assertion `camera->z_near>=0.0f`, and the
retail/HREK visibility projection paths consume it at `0x2A2046`/`0x81E8E8`.
That gives a future evidence-backed route for exact translated-eye near-corner
coverage, but stacking it into this direction-ownership candidate would add a
new untested camera-layout behavior. This candidate preserves the accepted
Halo 3/ODST head-centre visibility policy instead.

### Inner stereo candidate and coherent rebuild constraints

PE exception metadata bounds retail `player_view_render` at
`0x26C6DC`-`0x26CFE6`: `0x90A` bytes, with body SHA-256
`2628D1189621EACED7C95A1F295815D70E7783054F1C3CBA46799F838CC33C60`.
The following entry AOB matches exactly once in the pinned retail executable;
only the RIP-relative security-cookie displacement is wildcarded, and no base
relocation overlaps it:

```text
48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55
41 54 41 55 41 56 41 57 48 8D A8 28 FF FF FF 48 81
EC B0 01 00 00 0F 29 70 C8 0F 29 78 B8 48 8B 05
?? ?? ?? ?? 48 33 C4 48 89 85 80 00 00 00 8B 81
A4 03 00 00
```

Its ABI is `void __fastcall player_view_render(player_view*)`. Retail has
exactly one direct rel32 caller, `0x0C33C4`; the HREK source-named homolog is
`0x834490`-`0x835598`. HREK has exactly two direct calls to that homolog: normal
`0x1CBF22` and alternate `0x1CC790`. An inner loop here structurally avoids
repeating the slot-zero block, frame-texture/visibility preparation,
active-view set, and camera-stack push. Stock post-render, pop, and clear still
execute exactly once after the hook returns.

This inner call cannot determine outer ownership by its own immediate return:
normal `0x0C3730` and screenshot `0x1D3864` both funnel through the same
`0x0C33C4` call. Production must validate the exact outer normal return and
propagate a bounded TLS owner token into this inner scope. A screenshot, unknown,
or nested call before ownership remains untouched. A token mismatch after the
active owner is established rejects and suppresses the claimed transaction.

The stock-observed pre-scope camera rebuild sequence is:

1. mutate the primary compact camera;
2. call frustum helper `0x287F58` with `RCX = compact`, `RDX = float[4]`;
3. call projection builder `0x2884BC` with `RCX = compact`,
   `RDX = float[4]`, `R8 = primary derived`, and `XMM3 = 0.0f`;
4. explicitly make the required primary/secondary compact and derived blocks
   coherent;
5. call camera-state updater `0x286F9C` with
   `RCX = player_view+0x3B0`, `RDX = primary compact`;
6. call projection/matrix builder `0x28AF8C` with
   `RCX = player_view+0x490`, `RDX = primary derived`,
   `R8 = primary derived+0x78`, `R9 = compact+0x4C`, and stack argument 5
   pointing to the zero projection-offset pair at `player_view+0x470`;
7. enter the active-view/camera-stack render scope.

Normal setup and the independent retail/HREK screenshot paths corroborate
that ordering. The projection builder writes exactly `0xC4` derived bytes.
The camera-state updater sparsely touches a `0xC8` history/state envelope at
`player_view+0x3B0..+0x477`, including zeroing the projection-offset pair, and
shifts its own history before writing the current camera. The projection/matrix
builder's final direct output byte is `player_view+0x750`;
stock treats `player_view+0x490..+0x75F` as one `0x2D0` current-matrix block.
Setup argument 5 is separately proven by an HREK assertion to be
`output_user_index`; it is not an eye selector. The retail camera-state updater
has only the two arguments above; HREK's extra updater arguments must not be
transplanted into MCC.

If that helper subset is used, its known touched/replay units are:

- workspace `0x00C9FAE0..0x00C9FD8F`, size `0x2B0`;
- player view `+0x3B0..+0x477`, size `0xC8`;
- player view `+0x490..+0x75F`, size `0x2D0`;
- player view `+0x760..+0xA2F`, size `0x2D0`, only if the previous-matrix
  mirror is touched;
- render-camera pointer global RVA `0x04E38A90`, validated as
  `player_view+0x3B0` before admission and re-armed by each eye updater.

The render-camera pointer is not a final rollback byte. Each stock
`player_view_render` clears it at `0x26CE2F`; the final eye's stock zero must
persist. Blindly restoring the inner-entry nonzero value would create a stale
owner, while restoring an arbitrary prior value could clobber a nested owner.
Any inter-eye re-arm must come from the proven updater under a serialized owner
token; an unexpected value rejects and suppresses the claimed transaction.

The whole `0xA40` player view must not be snapshotted or blindly restored.
The owner writes `player_view+0xA30` before rendering. A preceding conditional
at retail `0x26CB54`-`0x26CB5B` can bypass the flag block entirely. When that
block executes, `player_view_render` tests and clears the flag at
`0x26CB73`-`0x26CB7C`; HREK identifies its work as `wait_for_gpu`. The first
candidate policy is `stock_last_window && final_eye`: the first eye receives
false and the final eye receives the original stock byte. The stock original
then decides whether the block executes and consumes the byte. The final eye's
actual post-call byte must persist; it must not be assumed cleared. Static
evidence proves the conditional consumption semantics, not this policy's
runtime/headset correctness.

All proven normal and screenshot rebuilds execute before active-view set and
camera-stack push. The helper inputs still exist at the inner candidate, but
static evidence does not prove that invoking them inside the active scope is
safe. Per-eye serial reuse, temporal/previous-matrix policy, conservative
visibility for both eyes, secondary render-camera policy, OpenXR pose/unit/FOV
mapping, exception cleanup, and headset behavior therefore remain runtime
gates. This section selects the exact inner candidate; it does not make it
hook-eligible.

### World, first-person, native CHUD, capture, and Present

Reach does not have a strict low-level `world -> weapon` total order. World,
effects, and first-person subpasses are interleaved inside
`player_view_render`; claiming otherwise would be false. The following weaker
and sufficient stock ordering is proven:

| Phase | Retail evidence | HREK semantic evidence |
| --- | --- | --- |
| Player world/view transaction begins | `0x0C33C4 -> 0x26C6DC` | `0x1CBF22 -> 0x834490`, source-named `player_view_render` |
| First-person stages | `0x26CAB6 -> 0x26DA08`; final wrappers at `0x26CBD5` and `0x26CDC0 -> 0x26EA78`; camera rebuild at `0x26EC3B`/`0x26EC7E -> 0x286C6C` | Retained `first_person_pass` label and source assertions |
| Late native CHUD | `0x26CE28 -> 0x2C279C`, then `0x26CEC9 -> 0x2C29F8` | `0x8354AE -> 0x8B67C0` is source-named `chud_draw_screen_LDR`; `0x8354E5 -> 0x8355A0` reaches source-named `chud_draw_screen` at `0x835682 -> 0x8B6300` |
| Post-render continuation | `0x0C33C9`, after every phase that executed | Return from source-named `player_view_render` |
| Stock Present | outer frame function `0x0C2DFC`-`0x0C3033` calls main render at `0x0C2FAA`, then `0x0C3000 -> 0x25113C`; Present vcall `+0x40` is at `0x2511AA` | `0x1CB66A -> 0x1CB8D0`, then `0x1CB810 -> 0x7C5F10`, retained `swap_chain_present` |

The first-person and both late CHUD paths are conditional. CFG branches can
skip them, but every path that executes the final first-person work reaches it
before either late CHUD phase. No backward edge after the final first-person
region reverses that order.

### Official-HREK-only authored-crosshair transaction (UNTESTED)

This candidate is governed by the 2026-07-26 evidence policy. Its new
crosshair binding uses only the official Halo: Reach Editing Kit (HREK), build
`2023.07.17.176677.1-QFE1`, and official mod-tool tag exports. No retail-binary
or Reclaimer-derived fact was used to select the hook, descriptor field, class,
or authored widget. Older retail material elsewhere in this ledger remains
historical context and is not proof for this transaction.

The three official HREK executable identities used here are:

| Binary | SHA-256 | Crosshair evidence |
| --- | --- | --- |
| `reach_tag_test.exe` | `CBDD8448A87A433B0DFFC0DE47D06DB7A18B4BF868B96B057135DAA86790ABA8` | Source-rich CHUD enum, ordering, and `chud_draw_widget` semantics |
| `reach_tag_play.exe` | `450DFFE824DDE4C9866E4448491B8B41D82995DC93159260A4DEF07D059E732E` | Optimized production-layout `chud_draw_widget` variant |
| `sapien_play.exe` | `1FDA21569B38C189EC88124C1A682DCCED8FBEE11ACFD4D2605F46663B26175B` | Independent optimized production-layout `chud_draw_widget` variant |

In `reach_tag_test.exe`, the CHUD scripting-class enum pointer array is RVA
`0x0207F340`; its nine-entry definition/count is at RVA `0x0207F388`. The
values are `0` undefined/use parent, `1` weapon stats, `2` crosshair, `3`
shield, `4` grenades, `5` messages, `6` motion sensor, `7` chapter title, and
`8` cinematics. Runtime selection is therefore the signed scripting-class byte
at descriptor `+0x04` with exact value `2`; widget names are not an admissible
selector.

The closed action set includes an explicit `RejectTransaction`. Once Reach owns
the stereo transaction, an unreadable HREK descriptor cannot safely be assumed
non-crosshair, and an eye index outside `0..1` cannot select the one authored
capture eye. Either condition invokes the same per-eye invalidation,
generation-latch, disarm, and teardown path as capture loss. Draws outside Reach
ownership and readable non-class-2 widgets alone take automatic stock drawing;
the separately proven `kill_reticle=0` case remains an explicit user choice.

The official `tool.exe export-tag-to-xml` command exported all 63 top-level
`tags\ui\chud\*.chud_definition` files into the ignored
`out\hrek-evidence\chud` workspace. Those exports contain 283 top-level widget
collections, including 85 scripting-class-`crosshair` collections across 55
definitions. The exports are evidence only and are not committed. The assault
rifle definition independently identifies collection artist name `crosshair`,
scripting class `crosshair`, widget `ar_reticule`, external input A
`weapon autoaim target`, and authored normal/enemy/friendly color outputs.
Capturing the class-2 widget therefore preserves the engine's live normal,
hostile-red, and friendly-green authored state instead of drawing a procedural
replacement.

The development `chud_draw_widget` is RVA `0x00922AF0` in
`reach_tag_test.exe`. It has five arguments; argument 2 arrives in `RDX`, is
retained in `R13`, and supplies the signed class byte at descriptor `+0x04`.
Independent function matching found the two optimized official variants below.
Their common 33-byte entry is unique exactly once in each optimized executable:

```
48 8B C4 44 89 48 20 48 89 50 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A9 48 81 EC C0 00 00 00
```

| Binary | RVA | Exact body size and SHA-256 | Descriptor proof | Fifth-argument proof | Class-read proof |
| --- | --- | --- | --- | --- | --- |
| `reach_tag_play.exe` | `0x0056B15C` | `0x424`; `81EBDE1BB1CF9337C01BA861B0CAF70980EBF6871DE079334B5BFB77ABA8978E` | `+0x45: 4C 8B F2` (`RDX -> R14`) | `+0x4A: 4C 8B 4D 7F` | `+0x2E1: 41 0F BE 56 04 E8`, rel32 wildcard, then `+0x2EB: 48 8B D0` |
| `sapien_play.exe` | `0x008265F4` | `0x483`; `C0EA71FA6BD0D26CA2EBAAED58FA182FE0CD8288274D3459271AD62CA2B9099E` | `+0x45: 4C 8B FA` (`RDX -> R15`) | `+0x4B: 4C 8B 4D 7F` | `+0x33C: 41 0F BE 57 04 E8`, rel32 wildcard, then `+0x346: 48 8B D0` |

Complete disassembly of both hashed bodies proves the detoured transaction is
not recursive. Neither body has a decoded call or tail-jump edge to its own
entry, and no local backward branch reaches the entry. The one indirect call in
the Sapien variant is a `+0x10` dispatch through a stack-local receiver, not the
five-argument CHUD entry. The hook can therefore invoke its original trampoline
once without re-entering through an HREK `chud_draw_widget` self-edge.

The cold runtime resolver admits only one loaded executable match of that exact
entry whose unwind function begins at the match and whose complete function has
one of those two exact HREK body sizes and fixed ownership layouts. The SHA-256
values pin the complete official evidence bodies and their non-recursion proof;
they are not a loaded-title hash requirement because the deliberately wildcarded
rel32 operands differ by official link layout. This bridge is a mandatory part
of the Reach VR transaction, not an optional CHUD add-on. A zero-match,
multiple-match, non-executable, missing/misaligned unwind boundary, body-size
mismatch, or fixed-layout-byte mismatch rejects installation before the Reach
camera/VR core owns anything. No Reach VR feature claims that frame and no
alternate Reach VR mode is armed. There is no
retail RVA, widget-name heuristic, inferred layout, procedural reticle, or
permitted mixed VR-with-flat-stock-crosshair mode.

Cold admission also prepares every authored-reticle resource before the first
hook is created. `VR_PrepareAuthoredReticleResources` holds the Reach display
resource lock, requires the live D3D device/context and OpenXR session, creates
the reticle swapchain when needed, resolves every swapchain RTV, and creates the
private authored texture/RTV. Any failure rejects installation before hook
creation. The Reach hot hook calls only
`VR_BeginPreparedAuthoredReticleCapture`: it verifies the prepared frame and
already-created handles/RTVs, saves fixed D3D state, and performs no allocation,
signature scan, logging, or lazy resource construction. Halo 3/ODST retain
their accepted lazy entry; Reach cannot select it.
Session/device readiness is separately release-published only after
`xrBeginSession` and the exclusive frame-wait worker both succeed, is revoked
before stopping/fatal drain, and participates in the worker's install gate. `NotReady` releases the
retained module and retries without setting the generation-failure latch; only a
post-readiness swapchain/RTV/texture result of `Failed` is terminal for that
loaded Reach generation.

HREK also proves the transaction order independently of any retail image.
Source-named `player_view_render` begins at `0x00834490` (function
`0x834490-0x835598`); its late call `0x008354AE -> 0x008B67C0` is source-named
`chud_draw_screen_LDR`. That call is verified present: `player_view_render`
calls `0x8B67C0` exactly once and never calls `chud_draw_screen`.

**Reach has TWO CHUD draw paths, and only one is inside the per-eye render.**
Re-verified in HREK on 2026-07-26 because the original claim here was
incomplete and misdirected a day of debugging:

| path | entry | called from | camera in VR |
| --- | --- | --- | --- |
| `chud_draw_screen_LDR` | `0x8B67C0` | inside `player_view_render` at `0x8354AE` | per-eye VR camera (correct) |
| `chud_draw_screen` | `0x8B6320` | UI compositing pass at `0x50BD58` | stock camera (WRONG in VR) |

`0x8B6320` is `chud_draw_screen(user_index)`: it opens its own profiler scope
named `chud_draw_screen`, takes a user index, and gates on `!= -1` and `<= 3`.
The compositing caller `0x50B780` runs the stage sequence
`composite tile -> preui screen_shaders -> user_interface_render ->
chud_draw_screen -> overhead_map -> postui screen_shaders`, which is after the
world render, outside any per-eye scope.

Consequence, matching the 2026-07-26 headset report: CHUD elements drawn by the
LDR path render correctly in VR, while elements drawn by the compositing path
are projected with the stock camera - world-anchored navpoints land in the wrong
place and move inversely with the view.

This does NOT explain the muzzle flash. The user corrected an earlier reading:
there is ONE muzzle flash effect with TWO TEXTURES, and one texture is
misaligned from the weapon - not two competing draws. A muzzle flash is an
effect attached to a weapon marker, not a CHUD element, so it is a separate
defect and must be investigated through the first-person marker/palette
transform, not through this CHUD path. With shared `crosshair=1` and
`kill_reticle=1`, one configured eye captures the authored widget while the
opposite eye suppresses its flat copy; `crosshair=0` suppresses it. The shared
explicit `kill_reticle=0` setting preserves stock drawing by user choice, but
an evidence or runtime-capture failure may not silently enter that mode: it
must invalidate Reach ownership and enter verified teardown. No alternate or
procedural reticle mode is selected.
If ownership is revoked while a matching eye callback is already in flight, the
hook marks that eye failed and suppresses the abandoned CHUD pass before policy
can select stock drawing. Ownership includes the adapter's atomic active-title
identity, so a title switch closes capture immediately instead of waiting for
the worker's later teardown poll. It also requires the live adapter generation
to equal the lock-free camera-core generation, closing a same-title module
reload. The eye therefore cannot capture shared art, be copied, or be published.
Every newly admitted outer transaction also invalidates authored readiness from
an earlier qualifying attempt in the same prepared-frame serial. A pair that
observes class 2 while authored capture is configured must complete that capture
before its final eye copy. If Reach emits no class-2 widget in either eye, the
pair deliberately has no reticle quad; it cannot inherit an earlier attempt's
texture.

Loss of a prevalidated authored target is terminal for that exact title-module
generation. A failed prepared capture suppresses the class-2 draw, marks the eye
transaction invalid so the current eye pair cannot publish, immediately
disarms the Reach core, records a per-generation parity-failure latch, and
requests teardown. Teardown disables all six Reach hooks, waits for callback and
wrapper/trampoline relay quiescence, removes them only after the frozen-thread
RIP proof, restores title state, and releases the retained module through the
existing verified path. The latch prevents reinstall churn for that generation;
only a fresh module generation may attempt the cold proof again.
The failed claimed transaction is not rerun through the flat renderer. Matching
inner calls remain suppressed while verified teardown is pending; the untouched
engine call exists only before Reach VR ownership arms.

Authored upload loss uses the same terminal path but is generation-exact. The
compositor passes its captured Reach generation to the rejection entry, which
acts only if the active title, live adapter generation, and installed owner all
still match. A stale upload failure cannot disarm a later generation.
The matching Reach failure branch rejects ownership without calling the legacy
transparent/procedural swapchain-maintenance path. The compositor then rechecks
live exact ownership after upload and clears both copied-eye serials on failure,
so a concurrently disarmed or upload-rejected generation cannot submit either
the world projection or the authored quad.

Reach reticle composition has a closed admission conjunction: Reach must be the
active title; both eye copies must be complete for the exact prepared serial;
the installed/armed core must still own live stereo without teardown requested;
XR acquire/wait and XR world-image release must each return exact `XR_SUCCESS`,
and both world-eye resolves must succeed. When this attempt captured class 2,
the authored texture must be ready with that same current serial and its XR
acquire/wait/upload/release transaction must likewise return exact `XR_SUCCESS`.
A timeout or session-loss-pending result aborts the whole layer set and enters
terminal session recovery. Any successful `xrBeginFrame` is paired with an empty
`xrEndFrame` that references no potentially outstanding image. The
Reach branch queues no world, reticle, or scope layer until every required
result and the post-upload owner proof are known. Its owner selector uses
this conjunction instead of `TitleCapability_ControllerAim`; controller-aim
capability alone can never admit a Reach reticle layer.

This is not Reach HUD-layout authorization. Official HREK data exposes three
skins with five curvature records each (15 total), not a Halo 3/ODST layout.
The five default/dervish pairs are `0.86/0.87`, `0.90/0.90`, `0.87/0.91`,
`0.87/0.85`, and `0.91/0.93`; monitor values differ. No Halo 3/ODST anchor,
record count, or destination offset may be copied. Reach HUD capability remains
off and HUD sizing/aspect/curvature/height is withheld for a separate
HREK-proven candidate.

The authored-crosshair candidate is **UNTESTED**. It does not advance the
accepted pointer until its exact clean source and artifact hash pass headset
testing, including friendly/hostile color changes and a required Halo 3
regression for the shared authored-reticle composition path.

The exact in-scope post-render continuation therefore begins at `0x0C33C9`:
after the stock player renderer, including its executed native CHUD, and before
the active-view clear. A future detour around
`main_render_view` can instead capture post-original; that is still per-view
and before stock Present, but it is after the engine has cleared its active
pointer. The two placements have the same phase order but not the same
lifetime proof.

Static analysis proves placement, not a usable OpenXR capture target or a
successful copy. It also proves why a CHUD callee alone is unsafe:
`0x1D3894`-`0x1D3E27` separately calls `0x2C29F8` at `0x1D3D24` for an
alternate/screenshot path.

Reach's final stock target identity is nevertheless statically resolved.
Surface group 1 is constructed by `0x266F90`-`0x2670FB`: at
`0x266FC2` it reads exact swapchain global RVA `0x04E38868`, calls
`IDXGISwapChain::GetBuffer(0, ...)` at `0x266FE0`, and creates the group's
resource views.
The group-1 descriptor is RVA `0x00BB9288`; its runtime group begins at RVA
`0x00C8E520`. Its byte-identical retail/HREK descriptor specifies flags
`0x21`, `DXGI_FORMAT_R8G8B8A8_UNORM`, full-resolution scales, and one sample.
Flag `0x20` creates four `0x88`-byte specializations. The runtime group records
their count at `+0x58` and array pointer at `+0x60`; record 0 holds the
swapchain RTV at `+0x08` and SRV at `+0x18`. Creation calls
`CreateRenderTargetView(texture, nullptr, ...)` and
`CreateShaderResourceView(texture, nullptr, ...)`, so both default views
inherit the swapchain texture's UNORM Texture2D identity.

The selected specialization index is global RVA `0x04E38A08`, written from
player view `+0x3A4` at `0x26C779`. The selected record is therefore
`*(group+0x60) + index*0x88`; the stock cache for its current RTV is RVA
`0x00CA02E0`. When they execute, both conditional late native-CHUD phases bind
group 1/display through `0x274524` before the final CHUD draw. The accepted
external observer saw only player slot zero but did not sample `+0x3A4`, so
normal specialization index zero remains a required runtime check.

Swapchain creation `0x250C4C` proves one buffer, DISCARD swap effect, UNORM
format, sample count one, and shader-input plus render-target-output usage.
Stock wrapper `0x25113C` reads that same swapchain at `0x251195` and calls
Present at `0x2511AA`.

Cleanup `0x2670FC`-`0x26724F` releases and nulls the group resources and views.
Any retained engine RTV/view must follow that view-generation lifetime.
Whole-rasterizer dispose/init, ResizeBuffers, and a separate reset/recovery path
can each recreate the views, so ResizeBuffers alone is not a sufficient
generation boundary for a retained engine RTV. A cold-retained swapchain
buffer has the narrower resource lifetime and must be released before
ResizeBuffers, swapchain replacement, title teardown, or module unload.

This is a direct-to-display path, not Halo 3's internal scene-color path. The
shared Halo 3 learner requires a full-resolution slot-0
`R8G8B8A8_TYPELESS` resource with RT, SRV, and UAV bind flags; applying that
identity rule to Reach would be unsupported and could omit late native CHUD.

The smallest conservative capture route is to validate record 0 against
`swapchain->GetBuffer(0)` once per Reach generation in the existing cold
pre-Present path, create matching eye caches there, then let each eye render
normally into Reach's display buffer. Immediately after each original
`player_view_render` returns, the candidate would use same-context
`CopyResource` to snapshot the intended completed world, first-person, and
executed native-CHUD phases into that eye cache. The second eye would remain in
the stock display buffer for desktop continuation and Present. Single-sample
identical descriptors statically select copy rather than resolve; live content,
ordering, copy success, and no-added-frame-latency still require runtime proof.

Direct final-RTV redirection is not the first candidate: direct clears/copies
or SRV feedback could bypass it, and it would leave the Presented stock buffer
unwritten without a copy-back. Production still has to prove the cold resource
identity, live specialization index zero, pointer continuity, same-context copy
success, and exact generation lifetime before admitting the display copy.

### Remaining runtime gate

The static owner, stride, two caller semantics, within-call freshness,
workspace lifetime, final display owner, late-CHUD order, pre-Present capture
placement, and transaction-scoped clear are closed. Before a Reach hook is
eligible, the remaining runtime-evidence and implementation gates are:

- production exact-return routing for the normal caller, stock-only routing for
  screenshot and unknown callers, and repetition of the exactly-one
  loaded-image checks;
- production enforcement of the now-observed continuous camera freshness and
  one-second safety interval;
- production propagation of the outer normal-owner token into the proven inner
  candidate, plus serial-reuse, temporal, transitive-side-effect, and
  reentrancy guards;
- inside-active-scope safety and OpenXR mapping for the stock-observed
  pre-scope rebuild,
  plus byte-exact rollback of every touched region;
- cold record-0/buffer-0 identity, specialization-index-zero continuity, and a
  successful same-context per-eye `CopyResource`;
- pause, cinematic, split-screen, unload/reload, device-loss, and title-module
  transition behavior;
- callback quiescence and complete detour teardown;
- finite-value, range, index, count, pre-ownership refusal, and claimed-transaction rejection guards;
- exact-DLL headset validation plus Halo 3 regression.

Accordingly, the evidence-manifest `proof_complete` and `hook_eligible` fields
remain false even though an unaccepted runtime candidate exercises the bounded
implementation behind pre-ownership admission gates and terminal claimed-path rejection.

## Historical controller-input-only candidate behavior

`HALOMCCVR_EXPERIMENTAL_REACH_BRINGUP=ON` now compiles a
controller-input-only adapter. When module resolution identifies explicit
Reach, it grants only shared virtual-controller admission. Reach remains
`runtimeSupported=false`, its runtime capability mask remains
`TitleCapability_None`, it receives `TitleHookPlan::None`, and
`ReachAdapter_RuntimeHooksPermitted()` remains false.

This admission carries ordinary virtual XInput buttons and sticks into stock
Reach. It does not grant a Reach runtime owner or enable camera or render
changes, controller-aim or movement transforms, HUD, arm IK, haptics, runtime
mode publication, lifecycle publication, or any runtime hook. A
multi-resident `Unknown` frontend cannot claim the Reach-specific admission
because it cannot be identified as explicit Reach. Any title-independent
frontend controller-continuity fallback is separate and is not Reach gameplay
support.

The offline `tools/reach-preflight.ps1` command reads the retail file and HREK
identity only. It performs no injection, process attach, memory write,
protection change, debug attach, or detour.

## Standalone runtime observer

Status: **OBSERVATIONS_REVIEWED_INCOMPLETE**.
`reach-runtime-observer.exe` is an external read-only diagnostic, not an
injected Reach candidate. It requests only
`PROCESS_QUERY_INFORMATION | PROCESS_VM_READ`, installs no hook or detour,
injects no code, and writes no process memory or MCC file. Its only file write
is a new, exclusively created self log; it never overwrites an existing log.
It must be run against stock anti-cheat-disabled MCC and refuses a process with
`halo3xr.dll` loaded. It resolves the process image, observer executable,
derived installation root, and output parent through file handles to normalized
volume-GUID paths before refusing either its executable or output inside MCC.
The output-parent handle remains open without delete sharing and is the
`RootDirectory` for a single-component `NtCreateFile` log open using the
`FILE_CREATE` disposition and `FILE_OPEN_REPARSE_POINT` option, so aliases,
symlinks, junctions, ancestor renames, a raced leaf reparse point, and an
existing path cannot bypass the refusal. The observer hashes its already-open
running-image file handle, denies write/delete sharing on that handle, and
retains it through the first log line.

Before sampling, it verifies the pinned backing-file hash, loaded PE identity,
exactly-one executable-section matches at the expected `main_render_view` and
frustum RVAs, the complete `main_render_view` body hash, six proven `rel32`
edges, and committed readable `MEM_IMAGE` ownership for the fixed data ranges.
Its final snapshot rechecks the same Reach mapping, absence of `halo3xr.dll`,
and Reach as the sole resident title module. It re-runs that preflight after
every observed Reach unload/reload and pauses when another title module makes
ownership ambiguous. Because module state is polled every 100 ms, it can miss
a complete unload/reload between polls when the replacement has the same base
and mapping identity; only logged reloads are corroborated.

The observer samples active-view global RVA `0x04E389A8` and accepts only exact
starts of the four slots at RVA `0x029F2B90` with stride `0xA40`. A new session
or continuity reset must witness a clear value before any non-null value can
count, so attaching mid-transaction cannot seed evidence. For each later
observed normal-slot transition, it uses a pointer/workspace/pointer read to
discard torn snapshots and validates both the primary Reach compact camera at
workspace `+0x000` and the secondary compact camera at `+0x154`. Both must pass
before the sample is usable; byte equality is counted but is not required. The
runtime guards cover finite position/forward/up, normalized and orthogonal
axes, vertical FOV, ordered signed `y0,x0,y1,x1` bounds, and the proven
zero-origin/eight-pixel-minimum client bounds. A nonzero observed slot can
runtime-corroborate the static `0xA40` spacing; slot 0 alone corroborates only
the array base and is insufficient runtime evidence for the stride.

Its per-slot freshness ledger treats a sampled transaction as fresh for less
than 500 ms and reports a stable observation only when exactly one slot is
fresh and that slot has at least two usable transactions whose last-minus-first
span is strictly greater than 1000 ms, without a 500 ms gap. Two simultaneously
fresh slots fail closed and reset every slot window; interleaved split-screen
work cannot establish ownership. This is observer evidence only and never arms
VR. Polling can miss the short engine-owned active-view pulse, so absence is
inconclusive. A preflight-only run exits nonzero and is likewise inconclusive.

The exact source-`5d34180` executable, SHA-256
`AC43FA4F65256DF1CB46B9C0471DDA97E3120265AEDC876E0A3A73FC6A86CF6A`,
was run against stock MCC for 480,000 ms. Its retained log at
`out/test-runs/5d34180-stock-reach-observer-20260724-025036448Z/reach-runtime-observer.log`
has SHA-256
`3C36AF1F06FC428E914AB0C71330838587B020335EFBF2B017F8EF178768212D`.
Two preflights passed; 29,507 accepted exact-slot observer transactions, all
at slot 0, yielded 29,496 valid camera samples and seven stable windows, with no invalid cameras,
outside-array pointers, multi-owner intervals, contamination, or snapshot
failures. The observer safely reset on ambiguous residency, re-ran preflight
when Reach became sole-resident again, and later recorded one Reach
unload/title exit. Only slot 0 appeared, so the array base is
runtime-corroborated but the `0xA40` stride is not.

The complete safety contract, procedure, exact counters, and interpretation
limits are in `docs/REACH-RUNTIME-OBSERVER.md`. The pass runtime-corroborates
the pinned loaded image and observed single-owner camera freshness only. The
tool cannot prove exact caller execution or alternate-caller semantics, GPU
target/copy lifetime, split-screen, an unload/reload pair, device-loss or
detour teardown, stereo-eye behavior, headset parity, or a player-visible
Reach path. `proof_complete` and `hook_eligible` remain false.

## Native pause flag (live-observed, 2026-07-27)

`haloreach.dll+0x00C1A0E2` is Reach's native pause byte. **1 = paused,
0 = running.**

This one was not derived from HREK, and deliberately so. HREK names the system
but does not expose a readable flag: `game_paused` is a registered debug
variable whose entry exists in retail (`.rdata` RVA `0x009E90F8`, table entry
`.data` RVA `0x00B42748`, type `0x5`) but whose value pointer is **null even at
runtime**, so it is function-backed and cannot be read the way
`render_far_clip_distance` (type `0x6`, value `0x00B43A8C`) can. HREK also shows
the pause is owned by a UI component - `c_start_menu_pause_component`,
documented "Pauses the game while the component exists", with an `IsPaused`
property documented "Is the game paused" - and those symbol names are stripped
from retail. The flag was therefore established by **observing the running
title**, which the project's no-guessing rule explicitly permits ("confirm by
observation"), and only then matched in the pinned module.

Three independent lines agree:

1. **Memory differential.** A read-only scan of the writable committed pages
   inside the loaded `haloreach.dll` (67.35 MB), captured across three paused
   and two unpaused states, with the paused captures taken at different places
   in the level so that position-correlated bytes fall out. A survivor had to be
   identical across every paused capture, identical across every unpaused
   capture, boolean-valued, and different between the groups. 2175 bytes
   survived.
2. **Code-reference filter.** Of those 2175, exactly **four** are referenced by
   any instruction at all, scanning `.text` for 23 rip-relative byte-access
   forms (`mov`/`cmp`/`test`/`and`/`or`/`xor`/`movzx`/`movsx`/`setcc`, load and
   store). This flag has one writer and eight readers spread across the engine -
   the profile of a real global pause gate. The other three were: one write-1 /
   write-0 / compare cluster that looked textbook but did not track pause, and
   two per-frame churn bytes.
3. **Live watch.** A 10 Hz poll of all four candidates while the player paused
   and unpaused on a five-second cadence. This flag produced six clean
   alternating transitions 4.9-6.2 s apart, matching the requested cadence
   exactly and matching the differential's polarity. The two churn bytes flipped
   several times per second; the textbook-shaped candidate moved once in 67 s
   and read `0` while the game was paused, which eliminated it.

The runtime binding is a signature, not the RVA. The owner instruction is

```
haloreach+0x0000F5AD  8B 15 ?? ?? ?? ??            mov edx, [module TLS index]
                      65 48 8B 04 25 58 00 00 00   mov rax, gs:[0x58]
                      B9 A0 00 00 00               mov ecx, 0xA0   (member slot)
                      48 8B 04 D0                  mov rax, [rax+rdx*8]
                      48 8B 14 08                  mov rdx, [rax+rcx]
                      B8 00 02 00 00               mov eax, 0x200
                      66 42 09 04 32               or  word ptr [rdx+r14], ax
haloreach+0x0000F5D3  44 88 2D ?? ?? ?? ??         mov byte ptr [rip+d32], r13b
```

45 bytes, **exactly one match** in the pinned module, and its decoded disp32
resolves to `0x00C1A0E2`. The context is required: the bare store
`44 88 2D ?? ?? ?? ??` matches **39** sites. The flag address is decoded from
the matched store, never hardcoded - the same discipline as Halo 3's
`LocateNativePauseFlag` and ODST's owner proof, which resolves its flag from a
`C6 05 disp32 01` at owner+0x20.

The flag lives in `.data` beyond that section's raw file size, so it is
zero-filled at load. That is correct for "not paused at startup" and is why the
static file shows nothing there.

Constants: `kReachNativePauseOwnerRva`, `kReachNativePauseFlagRva`,
`kReachNativePauseOwnerSigLength`, `kReachNativePauseStoreOffset` in
`src/common/reach_render_logic.h`.

## Camera-attached model parallax (HREK-derived headset candidate, 2026-07-29)

Status: **headset-untested candidate**, not an accepted finding.

The reported boundary is not a general reversed skybox. In Night of Solace,
the `space_veins_ring` and `space_veins_ring_close` layers remain positioned in
the scene but create a binocular conflict, while ordinary skies in other levels
have appeared correct. Official HREK tags put both layers in ordinary transparent
render-model parts; neither has a special runtime shader class that justifies a
name-specific fix.

The enclosing official models do identify the larger semantic class:
`sky_wafer.model` and `sky_corvette.model` set the model flag documented as
`If sky attaches to camera#parallax % between sky pos and camera pos below` and
set `Sky parallax percent` to `0.6`. Checked ordinary mission, crater, cloud, and
orbit sky models leave that flag clear and the percentage at zero. HREK's model
tag-definition metadata gives `s_model_definition` size `0x220`, flags at
`+0x15C`, and sky parallax at `+0x1B4`; the named camera-attachment flag is bit
6 (`0x40`).

HREK function `0x1406EDF90..0x1406EE525` supplies the consumer, rather than an
inference from retail. At `0x1406EE28E` it obtains the model tag, reads flags at
`+0x15C`, tests `0x40`, sets the corresponding generic object-property flag,
loads the float at `+0x1B4`, multiplies it by 255, converts it to an integer, and
stores the byte at object property `+0x0B`. Thus this is a generic
model-to-object camera-attachment/parallax path, not a Night of Solace texture
or material path.

Only after deriving that behavior from HREK, the exact homolog was matched in
the pinned retail module at RVA `0x0024B5C4`. This sequence is unique in the raw
image:

```
A8 40 74 24 66 83 4B 04 40 F3 41 0F 10 84 90 B4 01 00 00
F3 0F 59 C6 F3 0F 2C C0 88 43 0B 41
```

The candidate changes only the four-byte conversion at RVA `0x0024B5DB` from
`F3 0F 2C C0` (`cvttss2si eax,xmm0`) to `31 C0 90 90`
(`xor eax,eax; nop; nop`). The existing store then writes the documented zero
parallax endpoint while leaving the generic camera-attachment flag and the rest
of object construction intact. Installation requires the exact-RVA signature,
uniqueness, and exact original bytes. Failure is loud and fail-open to authored
stock behavior; teardown restores the original bytes. No tag, mission, shader,
or texture name participates, and models outside this semantic class do not use
the modified result.

The remaining hypothesis is deliberately narrow: Reach's non-zero authored
camera-attached parallax conflicts with sequential stereo eyes, and its zero
endpoint resolves the reported cross-eyed layer without destabilizing its world
placement. That is not established until the user checks the same Night of
Solace scene. Acceptance additionally needs a normal Reach sky comparison and a
Halo 3 regression result; until then `docs/CURRENT-STATE.md` must remain on the
accepted build.

## All-vehicle controller-layout predicate (HREK-derived, 2026-07-29)

Halo 3 behavior being matched: vehicle controls receive the title's native
XInput LT/X layout. Reach's VR-only on-foot convenience swap remains active for
armour ability/grenade, but must not leak into any driver, gunner, passenger, or
aircraft seat.

Official HREK supplies both required semantic identities. Its
`player_mapping_get_unit_by_output_user(int)` is RVA `0x1D24F0`, size `0x56`,
body SHA-256
`167D2975D4D5E005076C458D9BC5ABACEC443E8A21661D5D37C2FFFDEE988702`.
It bounds the output-user index to 0..3, reads the player-mapping TLS member,
and returns the unit datum from `+0xC8 + output_user*4` or `NONE`.

HREK's script descriptor literally names `unit_in_vehicle` and describes it as
true when the given unit is seated on a parent unit. Its evaluator at RVA
`0x698EA0` calls the native predicate at RVA `0xE23780`; that predicate is size
`0x88`, body SHA-256
`46AE78D4D3FD272B89E45E5B37BB5FB27C97FD1950B5DA71790D56170EA2A10D`.
This is the title's generic seated-parent predicate, not a vehicle tag, seat,
or ground-vehicle special case, so it includes Banshees and every other vehicle
type.

Only after those HREK semantics were established were their pinned-retail
homologs matched:

| Retail identity | RVA | Body/range identity |
| --- | ---: | --- |
| `player_mapping_get_unit_by_output_user` | `0x00053EF8` | `0x30` bytes, SHA-256 `AC88BBC2B04311476DA9030986E4FEDB47BCE2BBCCE5CDFF9931D7755E958B25` |
| `unit_in_vehicle` script evaluator | `0x0019EF28` | `0x50` bytes, SHA-256 `28196326897CD53E67B11925A2691E2383DF4772FC5ECD68AE78D37AE3DF380F` |
| Native `unit_in_vehicle` | `0x004F9368` | `0xB2` bytes, SHA-256 `BCAA7DA0395FF9329104A47624CA7209A9321EBFA03688359884FBD41C0013DD` |

Each production entry AOB has exactly one raw-image match in the pinned retail
module. The evaluator's call at `0x0019EF5E` resolves to `0x004F9368`; its
descriptor at `0x00A22B60` points both to the literal `unit_in_vehicle` at
`0x009FB710` and to evaluator `0x0019EF28`. Both native helpers independently
decode the same engine TLS-index target at `0x00C17B18`. Production repeats all
of these checks cold before publishing either function pointer.

The functions are invoked only from the already-proven normal-player
`main_render_view` render thread, where Reach engine TLS is live. That hook
publishes one generation-keyed atomic state. The XInput hook calls no title
function and restores native LT/X only for the exact current generation's
`Vehicle` state. Invalid units and stale/unknown samples preserve the existing
on-foot swap. Signature failure or a guarded runtime call fault falls back only
this optional refinement, logs the fallback, and never disarms the Reach camera
core.

Status: **implemented, headset acceptance pending**. Required check is on-foot
swap, Banshee, at least one ground vehicle, and exit back to on-foot in the same
session.

## Evidence still required

The unaccepted camera candidate now implements the exact outer-owner token,
head-centred pre-visibility workspace rebuild, binocular angular-union FOV,
inner `player_view_render` stereo loop, stock-last-window policy, bounded
rollback, and cold-validated group-1 copy behind identity, scope, finite/range,
specialization-zero, and current-frame gates. Those mechanics are not accepted
merely because they build or launch.

The camera/culling gate passed in source `86864bd`, remained passing in
`03f0bff`, and source `b0710dc` passed the screen-aligned patchy-fog gate. The
immediate gate is now an exact-DLL headset result for the Reach-only
`1 / 3.048` physical-scale correction, including 20-25 cm lateral/vertical head
motion with no world swim, while atmospheric depth fog, stereo depth, projection
coverage, head-owned visibility, and visual snap/smooth turning remain coherent.
Halo 3 and ODST regressions are then required because shared
input/submission code remains cumulative. Broader
pause/cinematic/split-screen, unload/reload, device-loss, callback quiescence,
and detour-teardown coverage also remains open. First-person weapon/body aim,
HUD anchor, skeleton and weapon-marker facts, and brightness must
be independently derived before enabling those player-visible behaviors. Xbox
360 map symbols may supply names or call-graph hints only; every fact must be
re-proven against HREK and the pinned MCC x64 module.
