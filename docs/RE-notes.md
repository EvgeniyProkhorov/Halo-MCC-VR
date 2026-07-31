# Verified reverse-engineering map

Target used for these findings: MCC Halo 3 build 1.3528.0.0. RVAs are evidence for this build, not stable addresses. Production code must locate each boundary with a unique AOB signature and fail safely if the match is missing or ambiguous.

## Camera and stereo

- halo3+0x2A628C: camera-copy function, fastcall(dst, src). Source projection tangents are at src+0x68/+0x6C; the hook is the stable head/look state boundary.
- halo3+0x17DF44: `observer_apply_camera_effect`. Static disassembly and the
  matching 0x3D0-byte observer stride show that it reads the already-computed
  observer position/forward/up, composes Halo's authored camera-effect
  transform, and writes those three fields back. The production comfort hook
  bypasses this stage only while VR head tracking is active, removing recoil
  and other artificial screen shake without filtering locomotion or headset
  leaning. Its 60-byte AOB is unique in the 1.3528 DLL; a missing or ambiguous
  match leaves stock behavior active. Firing-recoil suppression was
  headset-confirmed on 2026-07-20.
- halo3+0x286A14: inner prepared-view renderer used for the two eye passes.
- The view contains current and first-person camera/derived pairs. FpDriverHook and FpCameraRebuildHook stamp the active eye values immediately before the corresponding first-person draw/upload.
- First-person camera upload is called through Halo's own uploader after a stamp. Do not replace this with a guessed constant-buffer hook.

## First-person skeleton

- Engine TLS +0x568 reaches per-player first-person weapon state; TLS +0x560 reaches the two animation-bank snapshots.
- halo3+0x184B08: first-person interpolation boundary. This supplies the authored render pose used by marker/effect and visible-palette consumers.
- 0x23200C / 0x2320B8: hierarchy composers used by the first-person evaluation path.
- The visible-palette hook is the safe boundary for arm IK and render-palette substitution. Bone count and all indices are bounded to 64.
- The right chain uses the verified runtime hand chain; the left chain is derived from the runtime skeleton topology. Do not substitute guessed string ids.
- Marker/muzzle bones and the visible palette use the same DesiredWristWorld transform so the weapon and effects stay coherent.
- Halo's later weapon-lag pass can separate post-edited composed bones from marker transforms. Do not restore the retired composed-output rewrite.

## Shotgun support hand

H3EK export showed the shotgun has a weapon-IK block mapping left_hand to a pump marker, while the assault rifle comparison has no such block. Runtime disassembly then identified the stock decision at halo3+0x2C393C.

The production startup patch:

- requires a unique signature;
- verifies bytes at signature +3 are exactly 74 05;
- writes EB 18 under temporary writable protection;
- flushes the instruction cache;
- selects Halo's existing no-weapon-IK path at halo3+0x2C3959;
- logs and leaves stock behavior untouched on any mismatch.

The user confirmed that this frees the shotgun left arm. The removed synthetic palette fallback did not.

## HUD and picture

- halo3+0x2EDF24 matches ManagedDonkey's chud_draw_widget: r8w is a runtime
  chud_definition tag index, while descriptor bytes +3/+4 select the widget
  collection/widget. The old picker therefore selected tag indices, not stable
  crosshair element ids.
- A complete H3EK sweep of all 65 ui/chud definitions confirms that native
  reticles use widget-collection scripting class 2 (crosshair). Collection
  indices vary by weapon and non-weapon HUD tags reuse those indices.
- halo3+0x2EDF24 calls game_is_playback first. Normal play short-circuits around
  both the class-2 comparison and halo3+0x2EDE38, explaining why hooking
  0x2EDE38 alone did nothing in-headset.
- The production path in commit c923842 NOPs only that validated short-circuit and hooks
  0x2EDE38. Halo still performs its own tag lookup and class comparison; the
  hook returns hidden only after Halo identifies class 2. No runtime tag-table
  dereferences were added to the render hook.
- Headset testing across multiple weapons confirmed that this removes the native
  crosshairs while preserving the VR reticle and the rest of the HUD, without
  reproducing the earlier black-screen failure.
- The discarded f0d5a88 classifier performed runtime tag-table dereferences from
  the element-submit hook. It caused a black headset view when stereo entered a
  level and was narrowly removed by 7fdf019. Never restore that lookup path.
- The native HUD is the accepted rendering path.
- The function once investigated as CHUD scale near 0x278EE0 controls screen brightness/alpha. It is used only for the brightness setting.
- Direct CHUD state-byte writes and capture/diff HUD extraction are disproven and removed.
- HUD layout size is DATA, not code (2026-07-19): the chud_globals tag's "curvature infos" blocks (one per skin: default/dervish/monitor) hold two "global safe frame" floats (shipped 0.87f, bits 0x3F5EB852) that scale the whole HUD layout toward screen center. Proven twice: H3EK tag_test re-laid the HUD out at 0.5 (desktop observation), and MCC consumes the floats per frame (live pokes resized the HUD instantly in-headset).
- At runtime those floats live in loaded tag data (private read-write heap, not the module image). They are located by the immutable 24-byte prefix [int32 1280][int32 720][55.0f][661.0f][58.0f][4.0f] (virtual canvas, sensor origin/radius, blip radius) with the safe-frame pair at +24/+28. Exactly 3 blocks exist and all 3 were found with bit-exact payloads (log-verified). A hit counts only when the prefix and plausible layout payload agree; every later write repeats those checks and rejects a stale slot.
- The same authoritative `s_chud_curvature_info` layout places `destination_offset_z` four bytes before that immutable prefix (safe-frame slot -28). Quest 3 testing proved this changes HUD curvature/depth, not vertical placement. `hud_curvature` maps 0.00 flat to a +0.30 delta and 1.00 fully curved to -0.30; 0.50 restores the retained per-skin authored baseline. Config version 2 migrates the earlier signed/one-tenth scale and the first-build `hud_height` alias without a visual jump.
- `hud_aspect` is a separate 0.50..2.00 horizontal multiplier applied after the runtime headset-aspect correction. It lets the user counter the safe-frame size control's perceived squeeze without coupling width to curvature or changing the vertical safe-frame value.
- True HUD height (2026-07-21): `chud_compute_anchor_basis` is signature-located at runtime and its matrix position Y (`basis+0x2C`) is translated by `hud_vertical_offset` (-300..+300 virtual pixels). Positive raises and negative lowers the complete CHUD. The hook is VR-only and skips the scripting-class-2 authored reticle while it is redirected into the controller-ray capture, preserving aim placement.
- Runtime chud_definition tag indices such as 0x62C, 0xF70, and 0x1A90 are
  observations, not portable element identifiers. Do not use them as defaults.
- HUD aspect correction is derived from the runtime per-eye FOV tangents. The
  Halo render aspect is compared with the runtime tangent-space aspect, hud_size
  remains the larger safe-frame bound, and only the other axis is reduced.
  Commit 1b53139 was headset-confirmed on Quest 3 and PSVR2 with OpenXR Toolkit
  disabled. It is a substantial improvement, although a mild squeeze remains.

## Resolution and upscaling

- Resolution scaling changes Halo's internal 2912x2100 raster before launch; it
  does not change the runtime-recommended OpenXR stereo-array swapchain or the
  submitted imageRect.
- The launcher scales both dimensions by the same preset and rounds to even
  values. The existing normalized eye blit then expands the complete source eye
  into the full-size OpenXR projection, preserving the lens frame and aspect.
- resolution_scale is free-form (0.35..2.75). Any value in range is honored
  exactly; the named F1 tiers are only shortcuts and do NOT snap the slider or a
  hand-typed value (the pre-2026-07-20 nearest-tier normalization was removed).
  kResolutionScaleMax and the clamp are shared by config.cpp, launcher.cpp, and
  menu.cpp so the launcher and menu can never disagree.
- 8K-class tiers (2026-07-26): Potato 50% (1456x1050), Low 75% (2184x1576),
  Medium 100% (2912x2100), High 130% (3786x2730), Ultra 180% (5242x3780,
  ~5k-class), and Keith David 264% (7688x5544, ~true 8K width). The ceiling is
  275% (~8008x5775). All uniform, so the 2912:2100 VR aspect is preserved.
  kResolutionScaleHeavy (1.76, ~5k width) is the F1/help "very heavy, can crash
  weak GPUs" warning threshold — warning text only, nothing is blocked.
- These tiers are constants-only and headset-pending; do not describe any tier
  as validated until headset coverage confirms it renders and stays stable.
- Enabling OpenXR Toolkit FSR produced tiled/overlapping regions in the observed
  VR View, consistent with an incompatibility around the current stereo-array
  presentation rather than ordinary low-resolution softness. The exact
  third-party cause was not proven.
- Product decision: do not add a built-in FSR path or an FOV slider. The supported
  performance control is the internal resolution preset; third-party upscalers
  remain outside the mod's supported render path.

## Input and firing

- OpenXR actions are merged into XInput slot 0.
- Controller aim is converted into Halo's normal aim steering so projectile direction, target logic, vehicles, and turrets remain game-owned.
- Halo still owns projectile origin. There is no verified fire-origin hook.
- The visible weapon mount trim must not rotate the reticle/projectile ray; otherwise calibration cannot converge.

## Native pause state

- H3EK/HaloScript evidence exposes an external boolean named `game_paused`, but
  live MCC pause/unpause testing proved that record remains zero. It is a
  developer override, not the native MCC pause-menu state.
- Four read-only module snapshots (paused, unpaused, paused, unpaused) reduced
  the state search to repeated candidates. A 2 ms timing trace then showed
  `halo3.dll+0xA3CA9A` changing before MCC's generic suspension flags on both
  entry and exit.
- Production code does not retain that RVA. A unique 45-byte owner signature at
  `halo3.dll+0xB682` resolves the final RIP-relative write target and requires a
  boolean initial value. Missing, ambiguous, out-of-module, or non-boolean
  results disable authority and retain the controller-edge fallback.
- When the native flag is available, it owns the head-locked 2D/stereo 3D
  transition and the camera-heartbeat restart heuristic is disabled. This is
  intended to fix Restart Level clearing Halo's pause state without a matching
  controller edge; headset confirmation is still required.

## Forbidden and fragile boundaries

- Never hook halo3+0x120DF8. A pass-through MinHook detour crashed on level load.
- Do not write gameplay roots, animation banks, or CHUD bytes from guessed offsets.
- Do not perform module scans, logging bursts, file I/O, allocations, COM calls, or resource census work in render/palette hot paths.
- Do not dereference runtime tag-table structures from the CHUD submit/visibility
  hot path, and do not interpret r8w as a stable widget or reticle id.
- Do not implement resolution scaling by shrinking the OpenXR swapchain or
  submitted imageRect. Keep the projection full-sized and scale Halo's source
  raster uniformly.
- Never treat an RVA in this document as a shipping address. The matching signature in source is the implementation authority.

## Halo: Reach HUD layout (2026-07-26)

- Reach's curvature record is NOT Halo 3's `s_chud_curvature_info`. Proven from
  the official HREK `chud_globals_definition` export plus HREK's own code: the
  `chud_curvature_info_block` load-time postprocess function
  (`reach_tag_test.exe 0x8E7170`, primary of the chunks `0x8E71D3` and
  `0x8E7284`; it is the code behind `"Curvature points are invalid.  Defaulting
  to no curvature"`) receives the record in `rdx` and writes the identity
  curvature grid to `+0x04..+0x4B`, which pins the record start.
- Record layout: `+0x00` res flags, `+0x04` nine curvature points,
  `+0x4C` screen transform basis, `+0x94` virtual width, `+0x98` virtual height,
  `+0x9C` sensor origin, `+0xA4` sensor radius, `+0xA8` vehicle 3d sensor radius,
  `+0xAC` blip radius, `+0xB0` four minimap points, `+0xD0` global safe frame
  horiz., `+0xD4` global safe frame vert., `+0xD8/+0xDC` safe-frame dings.
  The safe-frame pair therefore sits 60 bytes after virtual width, not 24.
- Three skins (default, dervish, monitor) each author five curvature records.
  Only `fullscreen wide{720p fullscreen}` applies to a fullscreen VR render, so
  the anchor targets three records - the same one-per-skin rule Halo 3 uses.
  Those three differ in sensor origin Y (656/650/656) and sensor radius
  (72/68/72), and dervish alone authors nonzero minimap points, so the adapter
  wildcards those bytes. Authored safe frames are 0.86/0.87, 0.86/0.87 and
  0.87/0.87 - close enough to Halo 3's 0.87 that one shared `hud_size` value
  lands in the same place in both games.
- **Reach has no `dest offset z`.** Its curvature is the nine-point grid, and
  `0x8E7170` is reached through the block's tag-definition descriptor
  (`.data` qword at `0x208A260`, beside the block's name string at `0x1874120`),
  not from any call site - it is a load-time postprocess, so the derived basis at
  `+0x4C` is computed once when the block loads. Writing the authored points at
  runtime is therefore inert, exactly like the CHUD alpha array. `hud_curvature`
  is deliberately not written for Reach and the log says so. Making it live
  requires writing the derived basis, or re-running Reach's own builder over the
  record after writing points; that is a separate candidate.
- Reach's HUD height has no implementation: `hud_vertical_offset` uses the
  `chud_compute_anchor_basis` hook and Reach's homolog is still unlocated.

## Reach authored CHUD crosshair (2026-07-27) - all verified against retail

- **`chud_draw_widget` in retail is `haloreach.dll+0x2DA364`.** HREK's compiled
  version does NOT byte-match MCC's build: a signature that `reach_tag_play.exe`
  and `sapien_play.exe` agree on byte-for-byte across 49 instructions matches
  retail **zero** times. Do not try to port HREK bytes for this function. The
  address was found by tracing the call graph forward from the already-proven
  `kReachPlayerViewRenderRva` (`0x0026C6DC`): `player_view_render` calls a
  per-player CHUD dispatcher at `0x2C29F8`, which calls the widget draw at
  `0x2C2BF1`.
- **ABI: arguments 3 and 4 are full 32-bit values**, not short/byte. Proven by
  `0x2DA39D mov r12d, r8d`, `0x2DA41D cmp r12d, dword ptr [rdi+4]`, and
  `0x2DA39A mov eax, r9d`. Declaring them narrower truncates the widget index
  on the way back into the engine and crashes at `0x2ED80C`
  (`mov eax,[rcx+0x98]`) on an invalid Blam pool handle. This was four
  identical headset crashes.
- **`descriptor+4` is a WIDGET INDEX, not the scripting class.** `0x2ED80C` is
  a three-tier index accessor: counts at `+0x98`/`+0xA4`/`+0xB4`, strides
  `0x27`/`0x21`/`0x20`, returning a widget record.
- **The scripting class lives on the owning COLLECTION**, reached through
  `descriptor+3`. Reach's own sequence at `0x2DA68A`-`0x2DA6EC`:
  ```
  globalPtr  = *(module + 0xC1A600)
  handle     = *(globalPtr + (arg4 & 0xFFFF)*8 + 4)
  chudDef    = pool[handle>>28] + handle*4
  collHandle = *(chudDef + 4)
  collection = pool[collHandle>>28] + (collHandle + descriptor[3]*0x37)*4
  class      = *(collection + 4)
  ```
  `pool[i]` is `*(module + 0x04E39F20 + i*8)`. The class offset is confirmed
  independently by the official tag definition: a collection begins with a
  4-byte `artist name` string id, then the `scripting class` char enum.
- **Why per-widget class filtering can never work:** across all 64 official CHUD
  tag exports, 1092 of 1143 drawn bitmap widgets author their class as
  `undefined/use parent` and inherit it from the collection. Only 51 carry an
  explicit class. Filtering per widget catches those 51 and nothing else.
- Halo 3 and ODST hide their crosshair by hooking a boolean visibility
  predicate and returning false. Reach has no such predicate, which is why it
  redirects the render target instead.

## Authored crosshair publishing (all titles, 2026-07-27)

- The aim quad renders `g_reticleChain`; `UploadAuthoredReticle` is the only
  writer of captured art into it.
- That upload is a blocking OpenXR swapchain acquire/wait/copy/release. Doing
  it every frame costs ~4-5ms of `renderWindow` - measured, and the entire
  difference between fitting a 120Hz budget (8.33ms) and missing it and halving
  to 60. Reach retains a bounded animation cadence. Halo 3 candidate publishing
  is identity plus discrete colour-state edges. ODST's title-native key already
  folds widget identity and its alternate-path state, so the 2026-07-30
  candidate uses the same identity-held policy without a recurring upload.
  The follow-up candidate also stops ODST's offscreen widget redirect/draw on
  steady frames: after valid art is held, it samples one complete native CHUD
  frame every 30 OpenXR frames, immediately publishes a changed identity, and
  keeps submitting the prior valid quad between samples. Halo 3 and Reach retain
  their existing capture cadence.
- The procedural reticle and the authored art share one swapchain. For a title
  that captures, the procedural reticle is fully transparent, so repainting it
  ERASES the captured widget. Once the swapchain holds authored art it must be
  held, not refreshed.
- `Game_TitleCapturesAuthoredCrosshair()` reports whether the capture hooks are
  actually installed. Use it rather than a title list: the previous hardcoded
  lists went stale for both Reach and ODST and each time produced an opaque
  procedural crosshair painted over art the title had already captured.

## Halo 3 authored CHUD colour-state publishing (2026-07-30)

- Headset result for source `f205ce9048895116babdd5d9ddb617351ecc0112`
  (DLL `31C72CAADEC000761B02A91519C793C6FA2F2417880468C72FCF73B85E20CE24`):
  the authored reticle was visible and the previous performance loss was fixed,
  but its blue/green/red target state was frozen. Preserve the exact SteamVR
  session at
  `out/test-runs/f205ce9-halo3-reticle-visible-fast-color-fail-20260730/halo3xr.log`
  (SHA-256 `A8D9D5E04D52A4A04BD44B139BBF6FD1574BD88A062CA52180EEEA58D5A749EF`).
  The log's steady 120 Hz render windows were about 6-7 ms. This proves the
  zero-steady-upload performance result, not colour correctness.
- Pinned discovery module:
  `N:/SteamLibrary/steamapps/common/Halo The Master Chief Collection/halo3/halo3.dll`,
  SHA-256 `B209D8454B12DC77E54CCD2C9924EC8D44B8619D21CF98E36FFAF601E67EFB63`.
  `chud_draw_widget` at `+0x2EDF24` calls `+0x2EEB80`; at `+0x2EECB2` that
  function calls the placement boundary at `+0x2EEFC8` with its fourth
  argument as a local output. The placement function writes a 16-byte vector
  at output `+0x88` before returning. This agrees with the earlier live probe
  that identified this output as colour/alpha/animation state rather than
  screen coordinates.
- The original 32-byte placement prologue occurs seven times. The extended
  55-byte signature through `48 8B E9` occurs exactly once in the pinned module
  (PE file offset `0x2EE3C8`, runtime RVA `0x2EEFC8`). Missing, ambiguous, or
  failed installation degrades only colour refresh; the held authored reticle
  remains visible.
- The candidate classifies pairwise ordering of all four finite output floats,
  with scale-relative tolerance. It deliberately does not assume ARGB versus
  RGBA: native captured pixels remain the colour source. Uniform opacity/fade
  scaling preserves the classification, while categorical blue/green/red
  changes produce edges. A settled Halo 3 widget uploads once, then only on a
  changed nonzero colour class with a six-frame minimum gap. There is no
  recurring Halo 3 upload and VR scope state is not a trigger.
- Apparent-size parity remains headset-pending: all three titles use the same
  `crosshair_size_deg` and distance-controlled outer quad. Halo 3 and the
  2026-07-30 ODST candidate use 4x internal authored-art occupancy; Reach
  retains 2x. Do not call the cross-title visual calibration accepted until the
  same slider value has been compared in all three headset sessions.

## Halo 3 vehicle detection and observer identities (2026-07-31)

Full evidence, method, AOBs, and derivations live in
`docs/HALO3-VEHICLE-EVIDENCE.md` (kit-first per the editing-kit policy; every
binary claim adversarially re-derived). Verified facts, pinned module build
1.3528 (SHA-256 `B209D845...E67EFB63`):

- `unit_in_vehicle` chain: literal `0x7F16D0` -> descriptor `0x7EB1F8` ->
  evaluator `0x1E5AF8` -> native `0x3A36F4`. The native requires the unit's
  seat word != NONE and a vehicle-kind parent, then returns true iff the
  parent's physics type != 5 (turret); the ultimate parent is preferred over
  the direct one. H3EK carries the identical semantics with named asserts.
- `player_mapping_get_unit_by_output_user` homolog: `0xEE48C`; player-mapping
  struct pointer at engine TLS slot `+0x110`; unit array at `+0xC8 + user*4`.
  The retail body has NO 0..3 bounds check - callers must bound the index.
- Object table: engine TLS slot `+0x38` -> table; entries at `+0x48`, stride
  `0x18`; data ptr entry `+0x10`; kind byte entry `+3` (1 = vehicle). Unit
  seat word at data `+0x24E`; parent handle at data `+0x10`.
- `vehicle_definition_get_type` `0x396D84`: 10 physics blocks at loaded
  definition `+0x2C0` (stride 0xC), returns first non-empty 0..9 or 0xB when
  none is authored (stationary turrets, shade). Flying mask = {2,4,7} from
  the kit's own assert. Turret=5 proven by name tables in both guerilla.exe
  and retail (`0x791EB0`).
- Observer array: NO static RVA - pointer at engine TLS block `+0x578`,
  record stride `0x3D0`, slot 0 = base; position/forward/up written at record
  `+0x11C/+0x144/+0x150` by `observer_apply_camera_effect` (`0x17DF44`, the
  already-hooked `kObserverCameraEffectSig` match; TLS-index decode recipe at
  match+44/+48 with verification bytes at match+86/+106).
- All three vehicle-probe AOBs (native, getter, type accessor) measured
  exactly one raw match, three independent times.
