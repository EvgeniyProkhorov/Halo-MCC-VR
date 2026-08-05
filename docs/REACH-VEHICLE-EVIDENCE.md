# Halo: Reach first-person vehicle evidence

Evidence record for the Halo: Reach first-person vehicle candidate built on
source 118b8bde850d17a147e99ecff7f9d9214b6ca380
(halo3xr.dll SHA-256
7CB49B543A9D70B3AF2B60847A4618980A5D7E0E24B5A4DC24726FCEBC2B9690).
That source and DLL are the accepted ODST-vehicle development baseline in
docs/CURRENT-STATE.md. This document does not advance that pointer.

Status: **implemented candidate, headset acceptance pending**. The native
bindings, layouts, identity table, configuration round trip, and Blender
authoring artifact are offline-verifiable. They do not prove that the camera
is comfortable, that the body and hands look correct, or that every Reach
vehicle behaves correctly in a headset. Those are acceptance results, not
binary findings.

The method follows AGENTS.md:

1. establish Reach semantics and layouts from the official HREK executables
   and tags;
2. use the pinned retail haloreach.dll only to locate and verify homologs;
3. isolate this optional feature from the accepted Reach stereo camera core;
4. match the Halo 3/ODST player experience, not their engine-specific code.

Related records:

- docs/HALO3-VEHICLE-EVIDENCE.md is the headset-accepted Halo 3 reference;
- docs/ODST-VEHICLE-EVIDENCE.md records the accepted ODST port and its
  title-specific differences;
- docs/REACH-SIGNATURE-EVIDENCE.md pins the existing Reach module, render
  owner, player-unit, and input contracts;
- docs/MCC-EDITIONS-EVIDENCE.md proves that Steam and Microsoft Store game
  code is byte-identical apart from signing data.

## R-V0 - player experience being matched

Halo 3 and ODST establish the following vehicle experience. Reach must produce
the same result from its own native constructs:

| Player-visible behavior | Reach candidate |
| --- | --- |
| First-person view at an authored point for the occupied seat | The native seat camera marker (or the seat attachment marker fallback) is the seed. Reach-only forward/right/up trims move it in the rendered vehicle-root basis. |
| Normal headset look and physical lean | The seat point replaces only the gameplay camera origin; the existing Reach head-look and stereo-eye transaction remains authoritative. Authored/cinematic camera ownership still wins. |
| Camera rides the rendered vehicle instead of swimming against it | The stock marker resolver supplies either rendered/interpolated or raw node matrices. vehicle_cam_smoothing selects that native A/B. |
| Vehicle bounce without making the authored seat point drift | Occupant head motion is measured relative to a settled seat-local head pose. vehicle_bounce scales only that residual. |
| Own body hidden in first person | A pointer-free one-frame-interval lease clears the occupied seat's native third-person-camera bit 4 from the final stock outer-render call until the next proven outer boundary, so Reach's between-frame perspective consumers see the same first-person seat state. No map file is changed. |
| Arms and gun ride the seat rather than the player's face | vehicle_hands_follow_body selects the rigid authored seat base for the Reach first-person model/controller roots. The view may still receive the configured head-bounce residual. |
| Per-seat live placement | The existing F1 forward, height, and left/right controls bind to a Reach-only 34-by-16 bank keyed by the exact current identity and raw seat index. |
| Optional vehicle-frame view | vehicle_view_follow follows yaw and roll-stable pitch for ground vehicles, yaw only for aircraft, and no carrier frame for walk-up turrets. Attached guns follow their ultimate carrier. |
| Entry/exit comfort | The shared debounced vehicle transition requests the existing position-only recenter on settled entry and exit when vehicle_recenter_on_seat is enabled. |
| Native vehicle controls and honest aim | Supported Reach seats publish the same vehicle state used by the shared controller-layout, steering, VR-turn ownership, movement, and clamped-reticle paths. Look-steered drivers use native turn input or the virtual wheel according to the same policy as Halo 3/ODST. |

This is experience parity, not implementation parity. Reach does not share
Halo 3's vehicle definition layout, seat stride, string IDs, marker resolver,
or object collection layout, and no such value is copied.

One limitation was deliberate at first writing: the initial candidate did
**not** claim a Reach passenger shot-origin transaction, and its bit-4 write
existed only inside one stock outer-render call. The shot-origin limitation was
superseded 2026-08-04 by R-V12, which establishes that transaction from
Reach-native HREK evidence rather than by copying ODST's O6 work. The transient
perspective lifetime was separately disproven by headset and is superseded by
R-V20's pointer-free one-frame-interval lease. R-V20 does not replace or widen
R-V12's projectile/effect-origin ownership.

## R-V1 - official HREK discovery

The authoring source is Halo: Reach Editing Kit build
2023.07.17.176677.1-QFE1 at
N:/SteamLibrary/steamapps/common/HREK. tools/export_reach_vehicle_kit.ps1
uses that kit's tool.exe to export each official vehicle, model, and
render-model tag plus its DirectX mesh. The exact tag paths are the manifest in
tools/reach_vehicle_kit_manifest.json. The resulting XML, mesh hashes, node
counts, marker counts, and attachment frames are embedded in the Blender file
as REACH_SOURCE_EVIDENCE.json and copied to reach_vehicle_scene.json.

The following facts were established in HREK before looking for retail
homologs:

- unit_get_camera_info is Reach's native seated-camera transaction. Its HREK
  entry is RVA 0x00D76980. It accepts a unit datum and fills a 0x20-byte output
  containing the direct seated parent, signed raw seat index, a pointer to the
  occupied seat's unit-camera member, the engine-computed world camera point,
  and a related datum. It does not use the type-6 rejection present in the
  older unit_in_vehicle helper, so mounted and walk-up turrets are not silently
  excluded from camera ownership. The HREK function is split across unwind
  fragments and branches through approximately 0x00D76E60; the entry
  fragment's size/hash is therefore not presented as a false whole-body
  identity.
- The bit named third person camera is seat-flags bit 4. Reach's native
  first-person branch first resolves the seat camera marker and then resolves
  the occupant head marker. The candidate changes no other seat flag;
  in particular, bit 5 allows weapons and the unit-camera hide fields are
  untouched.
- object_get_markers_by_string_id is the authored-marker primitive used by
  unit_get_camera_info. Its last boolean selects the live
  render-interpolated-node bank. With string ID 0 and local-only true it
  returns the direct object's root matrix without extending through a parent.
  This is the correct basis for a direct child gun seat. Its official-HREK
  body is [0x00D3B0E0, 0x00D3B3D2) (0x2F2 bytes), SHA-256
  1088E52269CE3C92E29DC86A07FFBB1150D1943A710B6401726EE8D6C51A499B.
- Reach's head string ID is 0xC2. It was derived for Reach, not copied from
  Halo 3 or ODST (where head is 0x9F).
- A vehicle definition's seats tag block is at definition +0x444. Its records
  are 0x12C bytes. The seat flag word is +0x00, attachment marker string ID is
  +0x08, unit-camera member is +0x70, and camera marker string ID is +0x74.
- The four early vehicle-definition bounds fields at +0x0C, +0x10, +0x14,
  and +0x18 provide a stable runtime identity tuple. Reach's postprocess keeps
  an authored positive vehicle radius; otherwise it copies the referenced
  model's exact bounds tuple into those fields. Exact four-word comparison is
  therefore required. A single radius, a tag datum, or a physics type is not a
  sufficient identity.
- The native vehicle-type accessor independently validates the expected
  physics family. The tuple and type must both agree. Its official-HREK
  logical leaf is [0x00D88670, 0x00D886E3) (0x73 bytes), SHA-256
  0589529D559273EDC08DC5944D3805E43951777D00B7ACECC35F7AF8A5792585.
- HREK tag_get is RVA 0x000EE120. Its pdata interval covers only one fragment,
  so this record deliberately does not mislabel that interval as a complete
  function or publish a false full-body hash.
- HREK object_get_ultimate_parent is RVA 0x00D48A20. Its logical body is
  [0x00D48A20, 0x00D48B1A) (0xFA bytes), SHA-256
  E1669AE880AE31E5A7948B714D8A8621E9A40EE9E92A3F25F8866E3D84CD9310.
  The official assert literal
  object_get_ultimate_parent(object_index)==object_index at HREK RVA
  0x01958230 names simulation_game_position_updates.cpp line 0x6A7; caller
  0x00B5349B calls 0x00D48A20 and compares the returned datum.
- Six child-frame identities stay in their direct child-unit frame: Wraith
  gunner, the three Warthog guns, and the two Shade cannons. Their seat anchor
  must not be flattened into carrier coordinates. The four attached gun
  identities may use the ultimate parent only for optional carrier follow; the
  two walk-up Shade identities deliberately claim no hull-follow frame.
- Wraith seat 0 declares driver_camera, but that marker is absent from the
  exported render-model marker groups. Its declared seat attachment marker is
  therefore the evidenced fallback seed. The runtime applies the same ordered
  fallback to every seat: valid camera marker first, then valid attachment
  marker.

### Native layouts consumed by the candidate

All offsets in this subsection are Reach-specific HREK layouts matched to the
pinned retail module:

| Structure | Proven fields |
| --- | --- |
| unit camera output (0x20 bytes) | direct parent +0x00; signed int16 seat +0x04; seat-camera pointer +0x08; world point +0x10; related datum +0x1C |
| engine TLS | TLS index at module +0xC17B18; object collection pointer at TLS +0x10 |
| data collection | element stride qword +0x20 must be 0x18; initialized byte +0x31; count +0x44; entries pointer +0x50 |
| object entry (0x18 bytes) | salt int16 +0x00; kind byte +0x04 (1 = vehicle); object-data pointer +0x10 |
| object data | definition datum +0x00 |
| vehicle definition | identity tuple +0x0C..+0x18; seats tag block +0x444 |
| seat record (0x12C bytes) | flags +0x00; attachment marker SID +0x08; unit-camera +0x70; camera marker SID +0x74 |
| marker real_matrix4x3 | scale +0x00; forward +0x04; left +0x10; up +0x1C; position +0x28 |

Object lookup verifies initialization, count, stride, handle salt, kind, and
data pointer before a definition is read. tag_get is called with the complete
definition datum and the Reach vehi group value 0x76656869, preserving its
salt, bounds, and group checks. The candidate then rechecks seat count, raw
seat membership, packed-block element bounds, and that seat +0x70 is exactly
the pointer returned by unit_get_camera_info.

## R-V2 - pinned retail verification

The production gate remains the exact Reach retail image already recorded in
docs/REACH-SIGNATURE-EVIDENCE.md:

| Property | Value |
| --- | --- |
| SizeOfImage | 0x04EDA000 |
| PE timestamp | 0x68A0EFE1 |
| Steam SHA-256 | 738DD2D24EA3AEA12E1EE9AA4A61094BF116027D42004C35A19E5048608B0894 |
| Microsoft Store SHA-256 | F9F39CF058FF28C298CC05964BC40898A57C30D33848FA54D950D8D6C2697E20 |

The storefront files have identical code. The two hashes differ only because
the editions are signed differently. Every feature binding below is checked
cold only after this complete-image identity succeeds.

| HREK-established construct | Pinned-retail RVA | ABI / range identity |
| --- | ---: | --- |
| player_mapping_get_unit_by_output_user | 0x00053EF8 | int32 __fastcall(int32 output_user); existing Reach proof in docs/REACH-SIGNATURE-EVIDENCE.md |
| unit_get_camera_info | 0x0048A4B4 | void __fastcall(int32 unit, output*); body [0x0048A4B4, 0x0048A90E), 0x45A bytes; SHA-256 455196B8372C26DBED9B067D42DC146FD7C71F3762C9AC784099C683A065E601 |
| object_get_markers_by_string_id | 0x0047044C | int16 __fastcall(object, marker_sid, out, max, local_only, interpolate); body [0x0047044C, 0x00470727), 0x2DB bytes; SHA-256 10656BEFDF01B629240E0AD0C3E4774844232EC66C8DB810A2616B550BD8160E |
| interpolated-node provider | 0x000CEE24 | body 0x1EA bytes; SHA-256 400C6D4FB06ED6A8FFA67A5E3610D96DA987E3BA56604736B74443602999E4 |
| object_get_ultimate_parent | 0x00473DC0 | int32 __fastcall(int32 object); 0x3F-byte leaf; SHA-256 4786C758FBDA4D57A52956164BA701811FC414BF980CE9F92DA7357F4DA13568 |
| object_get_world_matrix | 0x004720E0 | current/simulation matrix evidence only; body 0xA2 bytes; SHA-256 961D5459708A642F640E313C07057D6EA1296C1DEEC6216E9036F42B8FAC0F53 |
| vehicle type accessor | 0x004AC1E4 | int32 __fastcall(definition datum); leaf [0x004AC1E4, 0x004AC235), 0x51 bytes; SHA-256 61C18948736356611DF19F8DF2C0E2254B1C7D97D84DEF24EE0AA2B4496FECF4 |
| tag_get | 0x00031AE8 | void* __fastcall(group, datum); leaf [0x00031AE8, 0x00031B63), 0x7B bytes; SHA-256 1319ABAA28A84AA78172538E6D84A90D437BA9A8C5E4729C2FD769DA41792EC1 |

object_get_world_matrix is recorded to prevent a false claim: it returns the
proven current/simulation attachment-composed matrix, not the rendered
interpolated matrix this feature needs. The candidate does not use it for seat
placement. It uses object_get_markers_by_string_id with the requested native
raw/interpolated selection instead.

### Production signatures

ReachColdExactSignatureAt requires each pattern's first full-image match to be
the expected RVA, requires executable memory, and rejects a second match.
Wildcards cover relocatable displacements only.

~~~text
player_mapping_get_unit_by_output_user @ 0x00053EF8
83 C8 FF 83 F9 03 77 27 8B 15 12 3C BC 00 65 48 8B 04 25 58 00 00 00 41 B8 58 01 00 00 48 63 C9 48 8B 04 D0 4A 8B 04 00 8B 84 88 C8 00 00 00 C3

unit_get_camera_info @ 0x0048A4B4
48 8B C4 48 89 58 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC E0 00 00 00 41 83 CE FF 8B D9 33 FF 0F 29 70 B8 48 89 7A 08

object_get_markers_by_string_id @ 0x0047044C
48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 66 44 89 48 20 57 41 54 41 55 41 56 41 57 48 81 EC 80 00 00 00 45 33 D2 8B E9 49 8B F8 44 8B F2 8B D9 41 0F B7 F2

object_get_ultimate_parent @ 0x00473DC0 (optional)
83 C8 FF 3B C8 74 37 8B 15 ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 41 B8 10 00 00 00 48 8B 04 D0 4A 8B 14 00 4C 8B 42 50 0F B7 D1 8B C1 48 8D 0C 52 49 8B 54 C8 10 8B 4A 14 83 F9 FF 75 EA C3

vehicle type accessor @ 0x004AC1E4
48 8B 05 ?? ?? ?? ?? 0F B7 C9 8B 54 C8 04 48 8D 0D ?? ?? ?? ?? 8B C2 48 C1 E8 1C 4C 8D 82 34 01 00 00 48 8B 0C C1 33 C0 45 33 C9 44 8D 50 10 4E 8D 04 81 41 83 38 00 75 11 FF C0 49 FF C1 49 83 C0 0C 49 83 F9 0F 7C EB EB 03 44 8B D0 41 8B C2 C3

tag_get @ 0x00031AE8
4C 8B 15 ?? ?? ?? ?? 45 33 DB 4C 0F BF C2 45 8B CB 49 63 82 F8 FF 03 00 4C 3B C0 73 17 48 8B 05 ?? ?? ?? ?? C1 FA 10 4E 8D 04 C0 66 41 3B 50 02 4D 0F 44 C8 4D 85 C9 74 3E 49 0F BF 01 48 03 C0 41 39 8C C2 FC FF 03 00 74 14 41 39 8C C2 00 00 04 00 74 0A 41 39 8C C2 04 00 04 00 75 19 41 8B 49 04 48 8D 15 ?? ?? ?? ?? 8B C1 48 C1 E8 1C 48 8B 04 C2 4C 8D 1C 88 49 8B C3 C3
~~~

The five runtime dependencies
player_mapping_get_unit_by_output_user, unit_get_camera_info,
object_get_markers_by_string_id, the vehicle-type accessor, and tag_get are
required for the Reach vehicle-camera transaction. The interpolated-provider
and world-matrix rows above are explanatory negative/consumer evidence, not
extra runtime calls. ultimate-parent is optional: if it fails, ordinary seats
and direct-child seat placement remain installed, while only attached-gun
carrier view-follow stays stock.

## R-V3 - exact vehicle identities and player-seat census

The table order is the runtime ReachVehicleId contract, the configuration-bank
order, and the Blender collection order. The fingerprint is the bit-exact
uint32 tuple {radius, offset X, offset Y, offset Z} read from vehicle definition
+0x0C..+0x18. Type is the independently expected result from Reach's native
vehicle-type accessor.

| ID | Config identity | Proven raw player seats | Type | Exact fingerprint tuple |
| ---: | --- | --- | ---: | --- |
| 1 | banshee | 0 | 5 | 3FE3DDB8, BEAA6EFF, BAE51357, 3F13532A |
| 2 | space_banshee | 0 | 5 | 3FB33333, 00000000, 00000000, 00000000 |
| 3 | ghost | 0 | 4 | 3F7C19A7, BCD7C712, 38612D0C, 3E1972D5 |
| 4 | revenant | 0, 1 | 4 | 3F59999A, BDCCCCCD, 00000000, BDCCCCCD |
| 5 | wraith | 0 | 4 | 3FE66666, 00000000, 00000000, 00000000 |
| 6 | wraith_gunner | 0 | 16 | 3F4CCCCD, 00000000, 00000000, 3F000000 |
| 7 | mongoose | 0, 1 | 1 | 3F2ABABB, 39815C03, 39CE6FFD, 3E7776E0 |
| 8 | warthog | 0, 1 | 1 | 3F978072, BD406A82, BAD03632, 3EC25783 |
| 9 | warthog_chaingun | 0 | 16 | 3F1CC397, 3E86AFD5, B3A6AFD5, 3E9C5885 |
| 10 | warthog_gauss | 0 | 16 | 3F28C016, 3E8E27FC, BC86EDEB, 3EA9A1F3 |
| 11 | warthog_rocket | 0 | 16 | 3F12852A, 3DC5E04F, 3BE1BCFB, 3EA9708D |
| 12 | falcon | 0, 3, 4 | 8 | 4037DF8F, BE781BAB, 363BE06E, 3F2D5046 |
| 13 | sabre | 0 | 13 | 40A69CFE, BF19A963, BB2A0199, 3F8AF1EF |
| 14 | scorpion | 0 | 0 | 400D30E2, 3D70D852, 3D28430C, 3EBBF788 |
| 15 | forklift | 0 | 1 | 3F971D82, 3E707E5B, BC9C748D, 3EF3BE41 |
| 16 | cart | 0 | 1 | 3F786DCF, 3C8C975D, BC8111A2, 3EE5C245 |
| 17 | shade_plasma | 0 | 16 | 3F7A9153, 3E1B9697, BC935063, 3F1EF5FC |
| 18 | shade_flak | 0 | 16 | 3F73045B, 3DD8D24F, BC93505D, 3F1EA4E9 |
| 19 | plasma_turret | 0 | 16 | 3F000000, 00000000, 00000000, 3DCCCCCD |
| 20 | machinegun | 0 | 16 | 3EE99B18, 3D9811F2, B9914460, 3E8589DB |

This is exactly **20 identities and 25 player seats**. Falcon is the important
tripwire: it authors more seats, but the proven player-seat indices are 0, 3,
and 4, not a guessed contiguous 0, 1, 2. Attached vehicle-weapon tags are real
HREK identities with their own definitions and seat parents; they are not
synthetic Halo 3 gunner slots.

Five rows retain positive vehicle-authored bounds and therefore intentionally
do not use the referenced model tuple:

1. space_banshee - 3FB33333, 0, 0, 0;
2. revenant - 3F59999A, BDCCCCCD, 0, BDCCCCCD;
3. wraith - 3FE66666, 0, 0, 0;
4. wraith_gunner - 3F4CCCCD, 0, 0, 3F000000;
5. plasma_turret - 3F000000, 0, 0, 3DCCCCCD.

The other 15 rows use the exact model-derived tuple produced by HREK's
postprocess branch. A tuple mutation, a type mismatch, an unknown or modified
tag, or a raw seat outside the explicit census returns Unknown and leaves that
seat's optional first-person vehicle feature stock.

### View and steering policy from the Reach definitions

- Aircraft: banshee, space_banshee, falcon, sabre. View-follow is yaw-only.
- Attached weapons: wraith_gunner and the three Warthog gun identities. Seat
  placement stays in the child frame; optional yaw/pitch follow comes from the
  ultimate carrier.
- Walk-up turrets: shade_plasma, shade_flak, plasma_turret, machinegun. They
  do not claim a hull-follow frame.
- Look-steered drivers: banshee, space_banshee, ghost, revenant, mongoose,
  warthog, falcon, sabre, forklift, cart.
- Virtual-wheel ground identities: ghost, revenant, mongoose, warthog,
  forklift, cart. Aircraft keep their native hand-aim climb/dive behavior;
  Scorpion and Wraith keep their independently aimed turret behavior.

These are Reach-specific policies backed by its vehicle definitions and
native controller path. They are not inferred from similarly named Halo 3
vehicles.

## R-V4 - camera composition

The runtime frame is constructed only on Reach's already-proven normal-player
outer-render thread, where engine TLS is live:

1. Get output user 0's unit and call unit_get_camera_info.
2. Validate the direct parent as a live vehicle object and resolve its vehi
   definition with native tag_get.
3. Match the complete four-word identity tuple and the native physics type,
   then require the exact raw seat in the 25-seat census.
4. Re-resolve the 0x12C seat record and require seat +0x70 to equal the
   camera-info pointer.
5. Resolve marker SID 0 with local-only true for the identity-root basis.
6. Resolve the declared camera marker; if it is absent, zero, minus one, or
   invalid, resolve the declared seat attachment marker.
7. Keep the anchor marker's animated position, but use the root marker's
   forward/left/up axes for authored trims. Config and Blender axes are
   +forward, +right, +up, while Reach's matrix supplies +left, so the local
   point is {forward, -right, up}.
8. Resolve occupant head SID 0xC2. After a stable seat-local reference settles,
   add only its relative motion times vehicle_bounce to the view base. Until it
   settles, the rigid authored point wins.
9. Use the rigid base for hands when vehicle_hands_follow_body is enabled.
10. For an attached gun, resolve the ultimate parent's SID-0 rendered root
    only for optional follow. Never convert the child seat point into carrier
    coordinates.

Reach world units convert at **3.048 metres per world unit**. Config values are
metres and are multiplied by the runtime world-units-per-metre constant at the
composition boundary.

The seat origin is not injected during an authored-locked cinematic. Reach's
existing cutscene/theatre ownership remains authoritative. A missing or stale
prepared VR frame likewise takes the established stock-render fallback and
does not create a partial vehicle transaction.

## R-V5 - configuration and F1 contract

Reach honors the same universal vehicle settings as Halo 3 and ODST:

- vehicle_first_person
- vehicle_cam_forward_m, vehicle_cam_up_m, vehicle_cam_right_m
- vehicle_view_follow
- vehicle_cam_smoothing
- vehicle_hide_body
- vehicle_hands_follow_body
- vehicle_bounce
- vehicle_recenter_on_seat
- vehicle_motion, vehicle_wheel_max_deg, vehicle_wheel_deadzone_deg

Reach adds an independent per-seat bank. Exact key grammar is:

~~~text
vehicle_cam_forward_m_reach_<identity>_seatN
vehicle_cam_up_m_reach_<identity>_seatN
vehicle_cam_right_m_reach_<identity>_seatN
~~~

identity is one of the 20 names in R-V3 and N is 0 through 15. Names are
intentionally role-neutral: a later role correction cannot rename an existing
configuration key. The bank reserves 20 x 16 slots because Falcon authors
eleven raw seats, although only the 25 explicitly proven player seats are
consumed by this candidate.

Each axis has its own set flag. An unset Reach axis follows the corresponding
universal trim live; setting forward does not freeze up or right. Save writes
only axes that were explicitly set. Fresh Reach slots are all unset, so no
unverified Reach seat placement is shipped as a tuned default.

Halo 3, ODST, and the universal placement values retain their close-range
clamps. Reach's validated user-authored lineup requires larger marker-to-eye
deltas (most notably the Sabre), so only Reach's per-seat bank uses the wider
limits below. This does not alter another title's F1 controls.

| Axis | Minimum | Maximum |
| --- | ---: | ---: |
| forward | -1.00 m | +1.50 m |
| up | -1.00 m | +1.50 m |
| right | -1.00 m | +1.00 m |

Reach per-seat limits are forward -64..+64 m, up -16..+16 m, and right
-16..+16 m. These bounds contain all 25 strict Blender exports while retaining
finite-value, identity, seat, and vehicle-bounds validation at runtime.

The F1 panel selects one of three independent banks. While a generation-valid
Reach seat snapshot is active, the same three sliders bind to that exact
identity/raw-seat slot and the reset button clears its three set flags back to
the universal fallback. On foot, the sliders edit the universal values.

The Blender exporter emits 75 flat values (25 cameras times three axes) using
this exact grammar. The parser and save path round-trip the Reach prefix before
the generic Halo 3 prefix, preventing a Reach line from being consumed by the
wrong bank.

## R-V6 - historical transient first-person-seat scope (superseded)

The original vehicle_hide_body transaction cleared the HREK-named
third-person-camera bit 4 immediately before one synchronous
main_render_view call and restored it in that call's finally path. It
re-resolved the live object, definition, packed seat and flags pointer for each
write, used complete-word compare/exchange, yielded to concurrent changes, and
never let a body-hide fault disarm Reach stereo.

That pointer lifetime was safe, but the player-visible lifetime was wrong.
The rejected 2f03c82 run and the accepted-partial 619644c run both observed
the transaction executing and restoring without a logged fault while the
player model remained visible. Reach's native perspective/control consumers
run between outer-render calls, so a write restored before that interval could
not reproduce Halo 3's first-person-seat behavior. R-V20 supersedes only this
lifetime. Its key, state, teardown, and no-pointer-across-frame rules replace
the bullets that used to define R-V6; the HREK bit identity and complete-word
CAS evidence remain valid.

## R-V7 - failure isolation

This work is an optional transaction layered onto the accepted Reach camera
core. Its states follow the ODST StockFallback / CleanupRequired / Installed
shape.

| Failure | Result |
| --- | --- |
| Full module identity or required entry AOB is wrong/non-unique | Vehicle-camera transaction remains StockFallback. Existing Reach stereo continues. |
| Optional ultimate-parent binding fails | Direct seat camera remains installed; only attached-gun carrier follow stays stock and is logged. |
| Unknown tuple, modified tag, type mismatch, unsupported raw seat, invalid tag block, or missing marker in one frame | That frame/seat stays stock. Identity/marker counters advance. Other vehicles and Reach stereo continue. |
| Guarded native call faults | The vehicle-camera binding degrades to StockFallback for that module generation. The accepted camera core and OpenXR session stay armed. |
| Seat bit compare/write/restore fails | Body hiding degrades independently. Camera placement, stereo, and other optional features continue. |
| Stale generation or sample older than 500 ms | Published vehicle state is rejected; F1 binding, follow, steering ownership, and input consumers do not use stale data. |
| Per-eye render rejection | The established Reach path drops that VR frame or takes its bounded stock fallback; the session is not torn down. |

Hot code performs no signature scanning, file I/O, logging, COM, allocation,
or locks. It publishes generation-keyed atomics and counters; worker-side log
ticks report transitions and faults outside the render/marker hooks.

The older HREK-derived unit_in_vehicle binding remains a separate optional
controller-layout refinement. It excludes one native type-6 path and therefore
is not camera ownership. Once the richer camera frame is installed, its
structural seated state covers the supported turret census without calling
Reach code from the MCC XInput thread.

## R-V8 - Blender authoring artifact

The ready-to-place artifact is:

~~~text
out/reach-vehicle-camera-kit/reach_vehicle_cameras_v1.blend
size:   4,550,158 bytes
SHA-256 1A9EFD14A293CA7C57FE565D314D8C4881A8729CBE3E79F391752CECC404C1AC
~~~

It was generated and validated with the user's Steam Blender 5.1.2 at:

~~~text
C:/Program Files (x86)/Steam/steamapps/common/Blender/blender.exe
~~~

The scene contains exactly 20 ordered identity collections, 25 player-seat
cameras, 21 official HREK reference assets (including the Shade base), and six
direct-child attachment frames. Reference meshes, nodes, markers, scale,
rotation, and attachment frames are locked. Only each cam:<identity>:seatN
object's translation is authorable.

Each camera begins at an **HREK marker seed**, not at a claimed eye position,
and every camera starts marked NEEDS USER EYE PLACEMENT. This is intentional:
offline tag evidence can prove the seat frame and marker, but only the user can
set a comfortable eye point in VR. Wraith seat 0 is visibly labelled as the
seat-marker fallback because its declared driver_camera marker is absent.

The external and embedded reach_vr_seats.py helper builds 25 VR landmarks,
steps between seats, disables positional tracking for exact-base inspection,
and exposes the active camera's Forward (+X), Left (+Y), and Up (+Z)
translation fields directly in the Reach Seats panel. Rotation and scale stay
locked. Rebuild and Prev/Next never move a camera, and placement acknowledgement
remains explicit. In Blender, run the embedded text, press N in the 3D view,
and use the Reach Seats panel.

The source manifest and embedded REACH_SOURCE_EVIDENCE.json use schema 1.
HREK's DirectX mesh axes map as (x, y, z) to Reach/Blender (x, z, y), with
face winding reversed for that reflection. The authored-point exporter uses
schema 2; the two schemas describe different stages and are not interchangeable.

Scene axes and export math are:

| Meaning | Blender / Reach tag frame | Runtime config |
| --- | --- | --- |
| forward | +X | +forward |
| lateral | +Y is left | +right is -Y |
| up | +Z | +up |
| scale | metres; 3.048 m per Reach world unit | metres |

The initial seed is the runtime marker seed. Exported settings are
authored-minus-seed deltas, not absolute camera coordinates:

~~~text
effective = seed + { forward, -right, up }
~~~

The strict extractor is:

~~~powershell
& 'C:\Program Files (x86)\Steam\steamapps\common\Blender\blender.exe' --background --factory-startup 'out\reach-vehicle-camera-kit\reach_vehicle_cameras_v1.blend' --python 'out\reach-vehicle-camera-kit\parse_reach_vehicle_points.py' -- --output 'out\reach-vehicle-camera-kit\reach_vehicle_camera_points.json'
~~~

It emits schema 2 with seed points, absolute authored points for audit only,
authored-minus-seed deltas, runtime trims in metres, and the 75-key flat
halomccvr_cfg_values map. By default it refuses a changed camera set,
non-finite/evaluated transforms, altered identity/frame/key metadata, or even
one camera still marked NEEDS PLACEMENT. --allow-unplaced exists only for a
clearly labelled draft and must not be mistaken for a tuned configuration.

tools/validate_reach_vehicle_blend.py independently checks the exact identity
and camera counts, collection order, node counts, source hashes, locked
references, six direct-child frames, metric scale, embedded tools/evidence,
and translation-only camera locks. The current file reports VALIDATION_OK.

### User-authored 25-seat lineup (2026-08-04)

The user completed and explicitly marked all 25 cameras placed in Blender.
The strict extractor and independent validator both passed against the saved
artifact:

~~~text
reach_vehicle_cameras_v1.blend
SHA-256 3937AF50C963A94D9440FCA9DA338739D6C5FD3E4070F2DFCF3A1B9126E50CDF

reach_vehicle_camera_points.json (schema 2, 75 config values)
SHA-256 3CBA4AF66A83B04138F816F965EBB1F1056088A3B38E0E371CDD04C61D9AF574

REACH_POINTS_READY: cameras=25 placed=25
VALIDATION_OK: collections=20 cameras=25 source_assets=21 child_frames=6
~~~

The exported marker-to-eye deltas range from -3.70 to +42.43 m forward,
-8.18 to +3.52 m up, and -0.73 to +0.21 m right. The earlier shared
close-range clamp would have silently truncated valid authored seats. The
Reach bank now has title-specific bounds large enough for the complete strict
export; Halo 3, ODST, and universal placement limits are unchanged.

## R-V10 - 2026-08-04: every structurally proven seat gets the camera

The 2026-08-04 Steam session on ca741d9 (DLL 0208E2EF...05BB9B) is the
controlling runtime evidence: of ~30 seat entries, only eight went active
(ghost, revenant x2, forklift, wraith, banshee, falcon seat 0) and **21 seated
periods stayed stock with no logged reason** - the census-mismatch line dedups
once per generation and names nothing. Two logged faults: one exact-identity
mismatch (07:48:46) and one bounds-guard rejection during the wraith ride
(07:52:33). This violated the no-silent-fallbacks policy in exactly the way
the log could not attribute.

Three changes, in one candidate line:

1. **The census no longer gates the camera.** Halo 3 and ODST switch the FP
   camera on seat flags alone and key only authored eye points and policy on
   identity (HALO3-VEHICLE-EVIDENCE.md C20; the mod's own H3 seat patch
   consults no identity). Reach now matches that experience: an unmatched
   tuple or an out-of-census seat keeps the camera, provided **every
   structural proof still passes** - object kind, vehi tag fetch, seat count
   1..16, packed 0x12C seat element whose +0x70 equals the pointer the
   engine's own unit_get_camera_info returned (the strongest live layout
   proof), marker validity, finite composition, bounds guard. Unmatched
   identities ride the universal trims with no-follow/no-steer/no-wheel
   policy and publish the sentinel identity code 0xFF in the trim snapshot,
   which keeps FpActive/recenter/body-hide live while every identity-keyed
   decoder (F1 slot, wheel, steering, seat log name) fails closed. A seat
   whose authored camera and attachment markers are both missing (the
   space_phantom beam turret authors neither) degrades to the proven
   identity-root basis instead of losing the camera.
2. **Loud per-vehicle diagnostics.** The frame builder publishes each
   distinct unmatched tuple / out-of-census seat into one of eight lock-free
   fixed slots (tuple words, observed native type, raw seat, seat count); the
   50 ms worker logs each exactly once per generation:
   `unmatched vehicle tuple XXXXXXXX/... type T raw seat S (of N authored)`.
   No allocation, no logging on the hot path.
3. **The bounds guard honors the authored ranges.** ca741d9 widened only the
   config clamps and F1 sliders; the runtime guard still allowed only
   `bounding radius + 2 wu`, so the authored Sabre eye (+42.43 m forward =
   13.92 wu against a 7.21 wu limit) could never arm, and the wraith slider
   hit the 3.8 wu = 11.58 m ceiling the user's config still shows. The guard
   now widens its sphere by exactly the trim magnitude this frame applies:
   the pre-trim anchor stays held to the 2 wu wrong-frame margin, and a
   config-legal authored trim can no longer be rejected.

Also from the census sweep: a walk-up turret tag can ship mounted on a moving
carrier - all four falcon side-turret tags author plasma_turret's exact
tuple+type. The frame builder now consults the engine's own
object_get_ultimate_parent: a real carrier (ultimate parent != direct parent)
grants the walk-up seat the attached-weapon carrier frame and yaw follow; a
self-parented emplacement claims no hull frame, exactly as before.

## R-V11 - 2026-08-04: the complete official vehicle census

out/hrek-evidence/reach-vehicle-census/ holds the machine-readable sweep of
**all 69 official .vehicle tags** in the pinned HREK tree (census_tool.py,
reach_vehicle_census.json, seats_report.txt, plus every newly exported
vehicle/model XML). The pipeline recomputed all 20 known R-V3 rows first and
reproduced every tuple bit-exactly, every physics type, and the exact 25-seat
census before any new row was trusted. Because tool.exe prints only six
significant digits, bit-exact words were recovered from the raw tag binaries.

Fourteen new player-usable identities join the table (IDs 21-34):

| ID | Config identity | Player seats | Type | Tuple | Class |
| ---: | --- | --- | ---: | --- | --- |
| 21 | mac_cannon | 0 | 16 | 41200000,0,0,0 | walk-up (the Pillar of Autumn Onager) |
| 22 | scorpion_anti_infantry | 0 | 16 | 3ED28405,3DB8AD5D,B8A6B78A,3D8A1BD8 | attached (Scorpion MG) |
| 23 | seraph | 0 | 13 | 410F9AAB,C02DAE74,B625B5D0,3F8295A1 | aircraft (covers seraph_in_atmosphere - bit-identical) |
| 24 | pelican | 4,5,9,10 | 2 | 40B33333,0,0,0 | aircraft (four benches; driver seat authored invalid-for-player) |
| 25 | pelican_chin_gun | 0 | 16 | 3F2BC474,3EC52CD6,B3C52CD6,BE7D391F | attached |
| 26 | corvette_cannon | 0 | 6 | 40400000,0,0,0 | walk-up (Long Night of Solace) |
| 27 | space_phantom_chin_gun | 0 | 16 | 3F666666,3F666666,0,0 | attached |
| 28 | space_phantom_beam_turret | 0 | 16 | 3F906FE2,3F8275C2,374C2C51,3BA3D9C0 | attached (authors NO markers - root-basis fallback) |
| 29 | cargo_truck | 0,1 | 1 | 3FAE222F,3C709BBD,BC1C81E3,3F19FF3B | ground, look-steer + wheel |
| 30 | military_truck | 0,1 | 1 | 3FD4EDB9,BDAC5C6F,3BA35FAA,3F39CE19 | ground, look-steer + wheel |
| 31 | oni_van | 0,1 | 1 | 3FD46CE8,BDB33F17,3BA3612C,3F39C761 | ground, look-steer + wheel |
| 32 | pickup | 0,1 | 1 | 3F9FCD09,BDA6FD71,B329BF5D,3ECB6694 | ground, look-steer + wheel |
| 33 | truck_cab_large | 0,1 | 1 | 3FE2557B,BD4C676F,3ACF7265,3F6831B4 | ground, look-steer + wheel |
| 34 | squad_drop_pod | 0-3 | 16 | 3FDAB04B,BD31519F,390792C5,3F912A02 | scripted prop, no hull frame |

That is 34 identities and 50 tag-authored player seats. New identities ship
with **unset** trim slots (universal fallback, live F1 tuning); no Blender
cameras exist for them yet. Types 2 (human plane) and 6 (turret physics) have
no prior R-V3 policy precedent; their classifications are tag-family
inferences pending headset proof.

Negative findings that explain "missing" vehicles honestly: **warthog_troop's
three rear seats are authored invalid-for-player in Reach's own tags**, as is
the Pelican driver seat, every phantom/spirit seat, and every AI emplacement
(anti-air/anti-infantry/frigate/POA/scarab/BFG turrets). Those staying stock
is tag-correct. test_fbx_mongoose differs from mongoose by one ULP and is
deliberately not a row - the generic path covers it. Aliases resolve to one
row on purpose: falcon side guns = plasma_turret, shade_anti_air_cannon =
shade_flak, seraph_in_atmosphere = seraph, and each alias group shares its
per-seat F1 trims.

## R-V12 - 2026-08-04: the shot line

Reach parallel of ODST O-E6c, proven from HREK before touching retail. Inside
the pinned projectile transaction (HREK 0xDE4290, weapons.cpp; retail homolog
0x4C2710), the per-barrel flow queries weapon marker SID 0x103
(`safe_trigger`), walks the deepest +0x390 occupant, and calls the
unit-adjust function (HREK 0xD67EE0 - named by the engine's own
`unit adjusted origin (offset? %s)` debug strings), whose body proves:
camera := marker-0x103, else head 0xC2 when anim-mode==6, else **call the
unit camera-position evaluator**; forward := unit aiming vector [unit+0x214];
then `origin := camera + forward * dot(origin - camera, forward)` -
byte-for-byte the same projection O-E6c proved for Halo 3/ODST - then the
authored barrel FP offset and a camera->origin collision clip.

The evaluator (HREK 0xD76E60, consumed by pinned unit_get_camera_info at
0xD76BBE, containing the `player->last_controlled_unit_data.valid` assert of
units.cpp:9178) is the Reach homolog of ODST's unit_get_camera_position:
seated, it reads the parent's 0x12C seat record camera marker (+0x74) and,
when seat bit 4 is clear - the bit vehicle_hide_body clears - substitutes the
occupant's own head 0xC2. Retail verification on the pinned image:

| Construct | Retail RVA | Identity |
| --- | ---: | --- |
| unit camera-position evaluator | 0x48A1C0 | body [0x48A1C0,0x48A4B3) 0x2F3 bytes, SHA-256 9C9344F0D83EEABBD035A426F6469B49C848DE78C9C5634DBB108674F6175895; sits immediately before pinned unit_get_camera_info 0x48A4B4, which calls it at 0x48A696 |
| unit-adjust (firing consumer A) | 0x484F24 | body [0x484F24,0x48529F) 0x37B bytes, SHA-256 6EF679F2C9553B7F9CD942DAEF78CD1F17F262C93AB340C6E9B56B6ECDFEEFA3; exactly ONE caller in the whole image (the projectile transaction at 0x4C303A) |
| evaluator call inside A | 0x484FDC | inside the unique block `83 3C 30 06 75 07 E8 .. EB 05 E8 .. 80 7D 77 00` @ 0x484FCF; return address 0x484FE1 |
| direct evaluator call (consumer B) | 0x4C3107 | inside the unique block `8B 5C 24 68 39 7C 24 7C 7F 0E ... E8 .. 8B CB E8 .. 44 8B C8` @ 0x4C30F4; return address 0x4C310C; its second E8 targets pinned object_get_ultimate_parent 0x473DC0 |

An exhaustive rel32 census over retail .text: the evaluator has **34 direct
call sites, of which exactly two are firing**; both copy the result into
private stack buffers, so substituting at those two return addresses cannot
feed back into the engine camera - the exact property O-E6c's design needs.

**The implementation** detours the evaluator and substitutes the published
Reach rendered center-eye (Reach world units, published by the camera
transaction after head look; Reach does not feed the shared g_camX globals)
only when ALL of: the return address matches one of the two runtime-derived
firing returns; no authored cinematic (published per frame from the camera
thread); the unit is the seated local player's own (low-16 handle compare
against the frame-published seat state); and the occupied seat's flags author
bit 5 `allows weapons` - drivers and mounted gunners fire from vehicle
barrels and keep the stock origin, as do all 32 non-firing consumers, remote
units, AI, and every seat on foot. The original always runs first and its
register result is preserved. Counters (moves/skips) are worker-logged once
per generation. Teardown releases the detour with the module.

**Headset-unverified.** Nothing above proves the substituted origin feels
right; ODST's own O6 equivalent is itself still headset-unverified. The
`safe_trigger`-marker path and anim-mode-6 path bypass both call sites by
design and keep stock behavior; whether any player-usable Reach weapon
authors them was not censused.

## R-V13 - 2026-08-04: the seat is parented to the car

User headset report against fd32fa3 (Steam, 08:58 session): "sliding around
when driving forward in every vehicle", with the explicit directive to build
it like Halo 3. The session log is clean - no bounds, marker, bounce or
recenter faults - so the sliding is not a failure path; it is the placement
design itself.

**Mechanism.** R-V4 step 7 deliberately kept "the anchor marker's animated
position". Reach's seat camera/attachment markers ride the vehicle and
occupant animation graphs - the sit markers sway with the driving
animations - so the eye anchored to the live marker is dragged around the
cabin exactly while driving, invisible when parked. The Blender authoring
placed every camera against the marker's REST seed, so runtime placement did
not even match the authored intent under animation. Halo 3's accepted design
does not do this: its camera hangs off the rigid seat node, and its C21
result explicitly moved OFF animated markers ("arms ride the seat, not the
occupant's head"). This supersedes R-V4 step 7.

**Change.** The anchor marker's offset inside the rendered vehicle-root
frame is latched once per seat occupation (keyed on generation, unit, parent,
raw seat, identity and node-bank selection); every subsequent frame composes
the seat point from the rigid root matrix and that latched local offset, plus
the authored trims in the root basis as before. Hull physics reaches the head
completely; marker/occupant animation does not. Occupant motion re-enters
only through the scaled vehicle_bounce residual, measured against the rigid
seat frame - exactly the Halo 3 contract. If the latch or its transform
cannot be proven finite in a frame, that frame falls back to the animated
marker point (strictly better than losing the camera), a counter advances,
and the worker logs the fallback once per generation.

Headset-unverified: whether rigid parenting removes the reported sliding is
exactly what the next session decides. If sliding survives THIS change, the
next suspect in the evidence chain is the pre-render sampling phase of the
outer hook, not the marker animation.

## R-V14 - 2026-08-04: the hull never chases the resting hand

Second user headset report, against d032252: "sliding everywhere cant drive
in a stright line". The session log is again clean (no faults, no anchor
fallbacks - R-V13's rigid parenting held every frame), the live config is
bit-identical across all three sliding sessions, so the defect is in the
input design, not a failure path or a setting.

**Mechanism.** With vehicle_view_follow OFF (the user's setting), no Reach
seat authored steering: ReachVehicleSeatAuthorsSteering requires
followEnabled, mirroring Halo 3's coupling ("steering ownership switches on
exactly the condition that removes the closed loop's feedback"). In every
look-steered driver seat the closed-loop hand aim therefore kept writing the
right stick - and in Reach's look-steered ground vehicles the right stick IS
hull steering, so the hull permanently steered toward wherever the right
controller happened to point. A controller resting in the lap is a standing
turn command: "cannot drive in a straight line". The loop is stable (its
feedback exists) - it is simply steering a vehicle with the player's idle
hand. Halo 3's ACCEPTED driving sessions ran with follow ON, i.e. with
native-stick steering; the coupling, not the loop, is what Reach inherited
wrongly for a follow-off player.

**Change.** A look-steered GROUND driver seat (ReachVehicleSeatIsDriver and
ReachVehicleUsesWheel - ghost, revenant, mongoose, warthog, forklift, cart
and the R-V11 civilian line) now authors steering whenever its frame is
active, independent of view follow. Through the existing shared ownership
plumbing that means: Halo's own turn stick (or the two-hand wheel when
vehicle_motion grants it) steers the hull, ApplyVrTurn releases the stick
(the world-locked view stays HMD-owned), and the closed-loop hand aim keeps
only the pitch/gun. A centred stick is now a straight line by construction.
Aircraft (banshee, falcon, sabre, seraph) deliberately keep their accepted
hand-aim climb/dive flying; Wraith and Scorpion keep their independently
aimed turrets; passengers and gunners keep hand aim, which never steered the
hull. Halo 3 and ODST are untouched and keep their accepted follow-coupled
ownership.

Headset-unverified: whether stick-owned steering reads as "like Halo 3" in
the user's hands. If ground driving is cured but the user also wants the
view to travel with the hull, that is vehicle_view_follow = 1, which now
composes with this ownership rule unchanged.

## R-V15 - 2026-08-04: the follow integrator eats raw hull forward only

Third user report, against 83c1c84: "my camera is stuck on one spot - make it
behave like Halo 3". Diagnosis: the accepted Halo 3 driving experience turns
the VIEW with the hull (view follow); the user's shared config has
vehicle_view_follow = 0, so the Reach view stays world-locked while the hull
turns - and R-V14 correctly gave the turn stick to the steering, so the view
cannot be stick-turned in a driver's seat either. The remedy is the existing
switch (F1 -> Vehicles -> "View follows the vehicle"), not new machinery.

Before pointing the user at that switch, one hard Halo 3 rule is applied to
Reach's follow path: **the yaw/pitch follow integrator must be fed the RAW
hull forward** (HALO3-VEHICLE-EVIDENCE C14: integrating the render-
interpolated forward folds interpolation error into view yaw, headset-
rejected; "keep that forever" per the project record). Reach filled
frame.rawCarrierForward from the same rendered/interpolated marker bank the
camera placement uses whenever vehicle_cam_smoothing = 1. The frame builder
now re-resolves the hull (or carrier) root with the raw node bank purely for
the follow input, gated on vehicle_view_follow so the extra native call costs
nothing while follow is off. Camera placement keeps the rendered bank.
Follow with smoothing OFF already fed raw and is unchanged.

**R-V16 overturns this section's diagnosis.** The remedy named above (tick the
F1 switch) rests on the claim that the accepted Halo 3 driving experience is
view-follow ON. The repository's own record says the opposite - see R-V16.

## R-V16 - 2026-08-05: rejected raw-hull follow-off servo

Fourth user report, against 83c1c84 (the log's source stamp for the 22:46-22:49
session; the R-V15 DLL was installed at 22:52 and has never been driven):
"THE CAMERA GETS STUCK TO ONE SPOT IT NEEDS TO BE ORIENTED FOWARD ... i am NOT
TALKING ABOUT VIEW FOLLOW OPTION IM TALKING ABOUT THE WHOLE MECHANISM IN
ACTION IS NOT LIKE HALO 3".

### The checkbox diagnosis was wrong

efe8bdb (C24), the accepted Halo 3 vehicle baseline, states in its own commit
text: "vehicle_view_follow now defaults to false; the maintainer drove every
[seat] ... the follow ships off." src/common/config.h keeps that default. The
preserved deploy-backup configuration trail agrees: follow was 1 through the
Jul-31/Aug-1-morning Halo 3 candidates, flipped to 0 during the C23 tuning
session that produced C24's shipped defaults, and has read 0 in every session
since. Every Halo 3 seat the user accepted, they accepted with follow OFF. The
user is not describing a missing checkbox; they are describing a different
mechanism, and they are right.

### What the two titles actually did under the same setting

Halo 3, follow = 0 (accepted): Halo3SeatAuthorsSteering requires followEnabled
(src/common/halo3_vehicle_logic.h), so no seat authors steering, ApplyVrTurn
keeps the right stick and the stick turns the VR view exactly as on foot. The
hull is steered by the closed-loop hand aim - a CONVERGING direction target:
the error shrinks as the vehicle turns onto the hand's heading and the
deflection decays to nothing, so a steady hand drives a steady line.

Reach at 83c1c84/1acb1f6, follow = 0: R-V14's groundDriverAuthors was
unconditional, so ApplyVrTurn released the stick to native hull steering and
the follow integrator never ran. Nothing stick-driven could rotate view yaw:
the camera was pinned to the world while the hull turned underneath it. That
is the reported "stuck on one spot", and it is Halo 3's follow-ON stick
ownership fused to Halo 3's follow-OFF view - a combination neither title ever
shipped.

### The change

1. Ownership is follow-coupled again (game.cpp ReachApplySeatYawFollow), so a
   follow-off ground driver's stick returns to ApplyVrTurn and the view turns.
   Follow ON is unchanged and keeps R-V14's authored stick steering, matching
   Halo 3 with the switch on.
2. The hand steers the hull through a converging heading servo, but the loop
   closes on **the hull's own raw forward**, not on a camera facing. R-V13's
   headset verdict against the camera-fed loop was "sliding everywhere, cannot
   drive in a straight line"; the hull node matrix is the car itself, so the
   error genuinely reaches zero. A resting controller names one fixed world
   heading, the vehicle settles onto it and the command goes to zero -
   straight by construction, which was R-V14's requirement.
   Deflection is continuous out of a 3-degree deadband and saturates about 19
   degrees off the wanted heading (kReachHandSteerDeadbandRadians /
   kReachHandSteerGain); the vehicle's own turn rate remains the real ceiling.
3. The raw hull forward re-resolve is no longer gated on vehicle_view_follow,
   because the steering servo needs it in exactly the seats follow does not
   cover. R-V15's rule (the follow integrator eats raw forward only) is intact
   and now also protects the steering loop from interpolation noise.
4. The seat activation log names the mechanism in force ("view follow OFF,
   steering by hand heading (stick turns the view)"), so the next driving
   report can be read off the log instead of inferred from the config.

Scope: look-steered GROUND drivers only (warthog, ghost, mongoose, revenant,
forklift, cart, civilian line). Aircraft keep their accepted hand-aim
climb/dive; Wraith and Scorpion keep independent turret aim with the hull on
the move stick; passengers and gunners keep hand aim; Halo 3 and ODST are
untouched.

Rejected in the exact Steam/VirtualDesktopXR/Quest 3 90 Hz headset run of
commit 2f03c82, DLL SHA-256
C7F93E2F6D9420AD7C8A4B38469185B24CD20C552DEAA74E3DC7BB77D4BCDB0F.
The preserved log is
`out/test-runs/2f03c82-reach-vehicles-steam-quest3-rejected-20260805-024428/halo3xr.log`
(SHA-256 05B6CC91AB1DC2842E943E2C509A37418846179BD7003FB882A80344342F5D8A).
The user tested both Ghost and Warthog and rejected the complete driving and
weapon behavior, explicitly clarifying that view follow is only a secondary
option and the base mechanism must work under both settings. The special
3-degree-deadband/gain-3 raw-hull servo is therefore disabled, not deleted;
R-V17's retail identity aliases remain valid independently.

## R-V17 - 2026-08-05: retail maps are one ULP off the HREK tuples

The 2026-08-04 session logged "unmatched vehicle tuple
3F978071/BD406A82/BAD03632/3EC25783 type 1 raw seat 0 (of 4 authored)" while
the user drove a Warthog. That is the Warthog row
(src/common/reach_vehicle_logic.h) in every word except the bounding-sphere
radius, which is exactly one ULP lower than HREK's 0x3F978072 (1.18360722 vs
1.18360734). The 2026-08-04 09:28 session logged the same one-ULP miss for the
Mongoose in offsetX (39815C02 vs the table's 39815C03) - and HREK's own
test_fbx_mongoose, a re-import of identical geometry, authors bit-for-bit the
retail value. The retail maps were compiled from a different model-import
generation than the shipped source tags, and the last mantissa bit moved.

Consequence: "active warthog" and "active mongoose" appear in no session log
ever recorded. Both vehicles have always fallen to the unmatched path, whose
deliberate fail-closed policy is no follow, no steering authorship, no wheel
and universal trims - so the two most-driven vehicles in the game were the two
that could never behave. Ghost, revenant, forklift, wraith, banshee, falcon
and pickup all match retail, so this is not a general fingerprint failure.

Fix: both retail tuples are added as full-tuple ALIAS rows of their existing
identities (kReachVehicleFingerprintCount = identities + 2). The resolver
already supported many tuples per identity and still rejects a tuple claimed
by two identities; the physics-type gate still has to agree. No identity,
seat census, trim slot or config key changes. Any model-derived row not yet
observed at retail carries the same latent risk; a tolerant radius compare
would immunize all of them and is deliberately NOT done here, because a
bit-exact table is what makes the match provable.

## R-V18 - 2026-08-05: actual unit aim under both view-follow modes (historical candidate)

The rejected 2f03c82 candidate changed Reach seat bit 4 only around one
main_render_view call. It restored the bit in the call's __finally, so Reach's
simulation, player-control, aiming and firing work between render calls still
saw a third-person seat. That could hide the body for one render, but it could
not reproduce a tag-authored first-person vehicle.

The locally installed, deliberately inactive Workshop item 3775043720 is a
9.066 GB set of rebuilt campaign maps with no DLL, controller profile, scripts
or source tags. Read-only cache comparison is corroboration, not a source of
bindings: its Warthog, Ghost, Revenant, Banshee, Falcon and several gunner/turret
seats chiefly clear the same HREK-named third person camera bit 4. For example,
the stock Warthog words 04020014 00000870 08001C10 08001C10 become
04020004 00000820 08001C00 08001C00. The item is not activated, copied or
redistributed, and cannot be a Store-edition dependency.

Official HREK explains why lifetime matters. The unit camera-position evaluator
at HREK 0xD76E60 reads the live 0x12C seat record and tests bit 4. It is consumed
outside rendering by the player-control camera/desired-angle path
(0x1E770F/0x1E7934), the unit firing adjustment
(0xD67EE0/0xD67F8E), unit aiming-origin paths
(0xE5C440/0xE5C543 and 0xE5C6D1/0xE5C704), and the projectile transaction
(0xDE4290/0xDE52DD). A render-scoped write therefore cannot establish the
native state those consumers require.

HREK weapons.cpp also proves the ordinary projectile forward independently:
inside the unit-adjust body, 0xD67FAB reads unit+0x214 x/y and 0xD67FB8 reads
unit+0x21C z. This is the actual unit aiming vector recorded already in R-V12.
Reach previously fed the shared VR controller loop the compact render-camera
forward instead. In a seated chase-camera engine those are not the same state.

R-V18 made one player-visible change: a structurally proven Reach seat feeds
the VR controller loop the same actual unit aim state Reach uses for firing.
The persistent seat lifetime was deliberately not stacked into that candidate;
the following list records its tested historical scope, not the current R-V20
body-hide implementation.

1. In R-V18, the then-existing bit-4 body-hide write remained scoped to one
   synchronous render call and restored in its finally path. No loaded tag
   pointer survived the frame. The installed Workshop maps proved a persistent
   native-camera lead, but that separate lifetime/fallback risk was deferred to
   the later R-V20 candidate.
2. Every seated frame reads that same local unit's +0x214 vector through the
   proven object collection, with an isolated SEH boundary, finite and broad
   length guard, then normalizes it. Failure falls back to the compact camera
   for that frame and is counted; it never faults the vehicle frame or camera
   core. The legacy shared aim publication remains the compact camera, so a
   stale or torn coherent-unit read also fails back to compact rather than
   retaining stale steering feedback.
3. vehicle_view_follow does not select either mechanism. OFF retains the
   accepted Halo 3 closed-loop hand-aim behavior and leaves the turn stick on
   the world-locked view. ON retains Halo 3's stick/wheel steering ownership
   while the view follows the hull. Both modes use the same native Reach aim
   source for weapons and controller feedback.
4. The R-V16 raw-hull/deadband/gain servo remains dormant. R-V17's exact retail
   Warthog and Mongoose aliases remain active.

The reason it was withheld remains the controlling safety constraint: the
loaded seat record is definition-shared, and a cached tag pointer can become
stale across a map-cache reload without the Reach module generation changing.
R-V20 is the later candidate that satisfies that constraint with a value-only
key/lease, ephemeral re-resolution on Reach's proven engine thread, and
hook/module retention instead of a teardown-worker write.

## R-V19 - 2026-08-05: render-matched View Follow

The exact R-V18 headset run used commit
619644c754aeb0f62c1c3cd55ef320539ba96ed2, DLL SHA-256
525309022E880F4A31BFBDF83BB9E246D1A8B4A7809DA78783F6BF8CFEDDA85D,
Steam, VirtualDesktopXR 1.0.10, Meta Quest 3, and 90 Hz. Its preserved log is
out/test-runs/619644c-reach-vehicles-steam-quest3-partial-accepted-20260805-071537/halo3xr.log
(SHA-256 BF8E2AD9EC4005FDC7A4E2E6066EFFEF7E98A75D21B22C7CA0B6B1A005934118).
The user reported that driving without View Follow is now great. That is
positive headset evidence for retaining R-V18's native unit-aim feedback.
They separately reported View Follow as nauseating, jittery and stuttering,
and the player model as not properly hidden. Neither failed feature is stacked
into R-V18.

The run rules out a throughput stall as the View Follow cause. It held a
90.0 Hz app cadence, reported no render stalls, and its ten-second timing
windows stayed around 12-13 ms frame-interval p95 and 3-5 ms renderWindow p95.
The symptom changes with the follow toggle, so the failure is the carrier
transform supplied to follow rather than missed display deadlines.

R-V15's premise was false. It called raw-only follow a headset-accepted Halo 3
C14 rule, but C14 documents positional photon lead, not yaw sampling. The
current accepted Halo 3 path replaces meshFrame with the live rendered node
and then publishes xf.rawFwd = meshFrame.fwd; ODST likewise publishes its
selected live rendered node forward. In both accepted titles, seat placement
and follow consume one phase-matched rendered basis.

Reach did the opposite whenever vehicle_cam_smoothing = 1: placement used
rootMarker/carrierRoot from objectMarkers(..., interpolate=true), then R-V15
overwrote only frame.rawCarrierForward with a second
objectMarkers(..., interpolate=false) sample. At a 90 Hz render cadence over
Reach's simulation ticks, position and the vehicle mesh therefore glide while
follow yaw/pitch advance in raw steps. That phase split is the observed
rotation staircase.

R-V19 disables the two R-V15 raw-node re-resolves behind
kReachR_V15RawHullFollowEnabled = false; the disproven code remains dormant.
The already-copied rendered carrier forward now feeds yaw and roll-stable
pitch, matching placement and Halo 3/ODST. View Follow OFF consumes neither
follow publication, the rejected R-V16 hull servo remains disabled, and R-V18
unit aim is independent, so the accepted OFF driving path is unchanged.
There is no 90 Hz constant, resampler or rate-specific gain in this change:
each rendered frame consumes its own phase-matched carrier basis throughout
the required 72-144 Hz headset range.

Proper body hiding remains a separate candidate. The R-V18 log proves the
then-current transient bit-4 transaction executed and restored cleanly, while the
headset still sees the model. That is direct evidence that a one-render-call
write cannot reproduce Halo 3's seat-lifetime first-person state. R-V20 below
is that separate candidate; this paragraph remains the headset evidence that
motivated it, not a description of the current implementation.

## 2026-08-05: exact retail authored-camera identity aliases (post R-V19 evidence)

The R-V18 headset run is also the decisive authored-camera failure capture.
Its source commit, DLL, edition, runtime, headset and refresh rate are pinned in
R-V19. The preserved log is
out/test-runs/619644c-reach-vehicles-steam-quest3-partial-accepted-20260805-071537/halo3xr.log
(SHA-256 BF8E2AD9EC4005FDC7A4E2E6066EFFEF7E98A75D21B22C7CA0B6B1A005934118)
and its exact config is the halomccvr.cfg beside it (SHA-256
6A4D053A3317092A6EEFF3112A9A96C95F2093B62B0D40AEF672CC7A38D37323).

The save path was not the defect. That config contains the intended Blender
values for the reported seats: Scorpion seat0 is 0.00 forward / 2.69 up / 0.00
right, Shade Plasma is 0.00 / 2.41 / 0.00, Shade Flak is 0.11 / 2.27 / 0.00,
and Plasma Turret is 0.00 / 0.24 / 0.00. The log instead reports each as an
unmatched identity. An unmatched identity has no Reach trim slot, so F1 names
the universal trim and the camera consumes vehicle_cam_forward_m/up/right_m
instead of the saved reach_<identity>_seat0 row. This exactly explains both
"did not save my camera position" reports without changing the config format.

The fixed eight-slot unmatched diagnostic captured exactly eight distinct
retail identities in that run:

| log line | identity | official HREK tuple / type | observed retail tuple / type | bounded disposition |
|---:|---|---|---|---|
| 439 | Shade Flak | 3F73045B/3DD8D24F/BC93505D/3F1EA4E9 / 16 | 3F73045A/3DD8D24E/BC93505D/3F1EA4E9 / 6 | exact tuple alias plus verified type-6 allowance |
| 578 | Shade Plasma | 3F7A9153/3E1B9697/BC935063/3F1EF5FC / 16 | 3F7A9153/3E1B9697/BC935062/3F1EF5FC / 6 | exact tuple alias plus verified type-6 allowance |
| 864 | Scorpion Anti Infantry | 3ED28405/3DB8AD5D/B8A6B78A/3D8A1BD8 / 16 | same tuple / 6 | verified type-6 allowance only |
| 1800 | Scorpion | 400D30E2/3D70D852/3D28430C/3EBBF788 / 0 | 400D30E2/3D70D852/3D28430B/3EBBF788 / 0 | exact tuple alias |
| 2714 | Machinegun | 3EE99B18/3D9811F2/B9914460/3E8589DB / 16 | 3EE99B18/3D9811F2/B991445F/3E8589DB / 6 | exact tuple alias plus verified type-6 allowance |
| 2841 | Plasma Turret | 3F000000/00000000/00000000/3DCCCCCD / 16 | same tuple / 6 | verified type-6 allowance only |
| 3102 | Sabre | 40A69CFE/BF19A963/BB2A0199/3F8AF1EF / 13 | 40A69CFE/BF19A963/BB2A0198/3F8AF1EF / 13 | exact tuple alias |
| 3604 | Cart | 3F786DCF/3C8C975D/BC8111A2/3EE5C245 / 1 | 3F786DCE/3C8C975D/BC8111A1/3EE5C245 / 1 | exact tuple alias |

This remains HREK-first evidence. The official accessor pinned in R-V1 walks
the fifteen type-* blocks from the vehicle definition's 0x134 member, returns
the first non-empty block index, and returns sentinel 16 when none is authored.
The official source tags for Shade Plasma, Shade Flak, Machinegun, Plasma
Turret and Scorpion Anti Infantry contain none of those blocks. Official
type-turret is index 6. Retail is used only to verify the homologous compiled
representation: the pinned native accessor returned 6 for exactly those five
identities in the log.

A read-only scan of the stock Steam forge_halo.map corroborates the tuple side
(332,328,960 bytes, SHA-256
12270DDAD452BCE9AFAEF6000D5A18F2BA7C363FFBE7FCB59375619DBEEBFCF4).
The six retail alias tuples occur in the cache while their HREK variants do
not: Shade Flak 3/0 occurrences, Shade Plasma 2/0, Scorpion 2/0, Machinegun
2/0, Sabre 2/0 and Cart 2/0. Plasma Turret's exact tuple occurs nine times and
Scorpion Anti Infantry's exact tuple twice. No cache field-layout theory is
derived from that scan; the native accessor is the retail type proof.

The implementation is deliberately narrow:

1. Six explicit complete retail tuples join R-V17's Warthog and Mongoose rows,
   making the fingerprint table identities + 8. There is no ULP tolerance or
   partial-word match.
2. ReachVehiclePhysicsTypeMatches consumes the identity, complete fingerprint
   and observed type as one pair-bound proof. Shade Plasma, Shade Flak and
   Machinegun accept canonical tuple + type 16 or exact retail alias + type 6;
   both cross-products are rejected. Plasma Turret and Scorpion Anti Infantry
   have one bit-identical tuple and accept its two evidenced types, 16 and 6.
   Other HREK type-16 tags, wrong types on ordinary vehicles, mismatched tuples
   and unknown identities still fail closed.
3. The sole finite-point bounds rejection in the run immediately follows the
   unmatched Sabre at line 3103. Resolving Sabre selects its authored +42.43 m
   forward / -8.18 m up trim, which the existing trim-expanded guard already
   admits. The bounds guard is not widened.
4. No identity, seat index, existing numeric config key, seed point or authored
   value changes.

The Blender artifact itself is
out/reach-vehicle-camera-kit/reach_vehicle_camera_points.json (43,235 bytes,
SHA-256 3CBA4AF66A83B04138F816F965EBB1F1056088A3B38E0E371CDD04C61D9AF574).
It records camera_count 25 and all_cameras_marked_placed true. Those 25
runtime_config_trims_metres rows now also ship as the explicit
kConfigReachShippedSeatTrims built-in table, at the config file's two-decimal
persistence precision. Config::Config stamps all three intentional axes,
including zero, into Reach's independent bank. Existing config rows still win
when parsed; a missing or title-local Store config now receives the same
user-authored lineup without depending on the Steam config. These placements
are the user's Blender work, not HREK-derived eye points, and remain
headset-unverified. Later live-F1 drift in the 619644c config is not silently
substituted for the export.

That built-in changes the meaning of an absent numeric row: absence now selects
the shipped Blender placement, so deleting the row cannot persist F1's
"Back to the universal trim" action. Reach therefore has one explicit
seat-level tombstone:
vehicle_cam_use_universal_reach_<vehicle>_<seat> = 1. ConfigSave emits that
single line and no numeric rows for the seat. ConfigLoad clears all three axes
and selects the universal triplet; any valid numeric row for the same seat
wins independently of file ordering. On the first later F1 slider move, the
menu copies all three currently displayed universal axes into a new seat
override before changing the moved axis. The untouched two axes therefore do
not snap back to the baked Blender placement. Halo 3 and ODST serialization is
unchanged.

Headset acceptance still requires entering Scorpion, Shade Plasma, Shade Flak
and Plasma Turret and confirming that F1 names the correct Reach seat and that
the exported placement appears. Sabre should also be entered to prove the
existing bounds guard admits its large authored delta. This evidence and a
passing build do not advance CURRENT-STATE.

## R-V20 - 2026-08-05: pointer-free one-frame-interval first-person seat

The Halo 3 behavior being matched is C20: while first-person vehicles and body
hiding are enabled, the occupied seat reports native first person continuously
enough for the title's perspective/control consumers to hide the local body;
the original complete seat flags return on exit, fallback, config change, or
teardown. View Follow is an independent camera-orientation option and cannot
select this lifetime.

The accepted-partial 619644c headset result is the runtime reason for this
candidate. Its log proves R-V18's transient bit-4 transaction installed and
restored without fault, yet the user still saw the player model. Official HREK
already proves that Reach's unit camera-position, desired-angle, aiming-origin,
firing-adjustment, and projectile paths read the live seat record and test the
third-person-camera bit outside one render call. The deliberately inactive
Workshop item 3775043720 corroborates the content-side result by persistently
clearing that same HREK-named bit in rebuilt maps. It supplies no executable
binding or controller logic and is neither activated nor redistributed.

R-V20 changes only body-hide lifetime:

1. The lease payload contains values only: Reach module generation, full salted
   local-unit handle, full salted direct-parent handle, definition datum, raw
   seat index, original complete flags word, written complete flags word, and
   explicit state. No seat, tag, definition, flags, object, or camera pointer
   survives an outer-render call.
2. Immediately after nested-call handling at every outermost proven
   normal-player/window-0 callback, before vehicle/input sampling and before
   every later early return, the engine thread re-resolves the previous exact
   unit, parent, definition, packed seat, and ephemeral flags address. If the
   word is the exact written value, one complete-word compare/exchange restores
   the original. If it is already original, ownership ends without a write.
3. The camera samples and builds after that reconciliation. Only after the last
   possible early return, immediately before the stock main_render_view call,
   the same engine thread re-resolves the complete current key and atomically
   clears only bit 4. A normal return deliberately leaves that word clear
   through the following one-frame interval, so between-frame native consumers
   see first-person state. The next proven callback restores it before doing
   new work.
4. An abnormal original/rollback path is different: the outer detour's finally
   restores the just-installed lease while Reach engine TLS is still valid.
   Nested or non-player calls are never granted cleanup authority, so they
   cannot restore a parent transaction accidentally.
5. Failed re-resolution retains the value-only lease as RestorePending and
   installs no replacement. Later proven engine-thread callbacks retry it.
   The worker never writes a tag word. If no callback arrives during teardown,
   cleanup retains the outer hook and exact title-module reference rather than
   risking a stale write or silently forgetting ownership.
6. A current word that is neither the exact written value nor the exact
   original belongs to an engine/tag writer. That writer wins: the key becomes
   External, is never overwritten or retried, and body hiding alone falls back.
   A naturally clear bit with no owned write is likewise External/native and is
   never restored. A different full key may start a later lease.
7. Installing and CleanupLocked are opposing claims on the same lease-state
   atomic; the independent CleanupRequired/teardown flags are not the race
   arbiter. The worker may compare/exchange only Empty or External into
   CleanupLocked before disabling any hook. If a callback wins
   Empty/External-to-Installing first, the worker observes Installing (or its
   later Active/RestorePending state), retains the outer hook/module, and waits
   for engine-thread restoration. If the worker wins, CleanupLocked blocks
   every callback install even when that callback observed stale lifecycle
   flags. The lock survives a partial hook-disable/removal failure so the worker
   can retry; only successful hook removal plus callback quiescence clears the
   payload and returns the state to Empty. Body-hide failure never disarms the
   Reach camera core, unit-aim feedback, steering, or View Follow.

No map or tag file is patched on disk, Easy Anti-Cheat is not touched, and no
Workshop content is a runtime dependency. The pure tests pin full-key equality,
invalid-key rejection, teardown-retention states, exact-key External blocking,
same-atomic cleanup-lock admission/blocking, and the three restore word
dispositions. This candidate is still
headset-unverified. One deliberate hard fail-safe remains explicit: restore
requires the old full-salt unit and parent objects to remain live. Death,
vehicle destruction, or a map-cache transition can therefore leave
RestorePending indefinitely; the worker retains the hook/module and never
guesses that the old loaded word disappeared. Only the user's test can prove
the longer native perspective lifetime hides the Reach player model and that
ordinary exit, config disable, and title teardown restore promptly; the
destruction/transition cases must also confirm the loud pending behavior rather
than a stale write or silent ownership discard.

## 2026-08-05: exact-pair truthful seated-aim reticle (post R-V20 evidence)

The normal candidate policy is one behavioral change per headset candidate.
For this installation the user explicitly rejected further partial builds and
required the next artifact to contain the five outstanding vehicle fixes
together. This is therefore one cumulative acceptance candidate: View Follow
OFF preservation, render-matched View Follow ON, Blender camera
defaults/retail aliases, the interval body-hide lease, and weapon/reticle
truth. Each feature still retains its independent fail-open boundary, and no
individual result is accepted until this exact cumulative DLL passes headset
testing.

The Halo 3 behavior being matched is player-visible: after entering any
first-person vehicle seat, the floating sight immediately shows the direction
the engine has actually reached, with View Follow OFF or ON. View Follow is a
secondary carrier transform; it cannot select a different weapon truth. A
bounded seat must not leave the controller sight promising an angle the native
weapon has not reached.

The official HREK control flow narrows what can be claimed. The pinned
projectile transaction begins at 0xDE4290. Its caller at 0xDE4BD3-0xDE4BDC
derives the unit-adjust fires-from-camera argument from unit-definition flags
bit 3; the official unit_flags enum names that bit exactly. The unit-adjust
body at 0xD67EE0 receives that flag as argument 7, unit-aim selection as
argument 8, and collision handling as argument 9. It selects the explicit
camera/evaluator path at 0xD67F2A-0xD67F93, reads the unit aiming vector from
unit+0x214 at 0xD67FAB and 0xD67FB8, and projects the origin onto the
camera/+0x214 ray at 0xD67FDB-0xD6804C only when fires-from-camera is true.
The direct evaluator call at 0xDE52DD happens later for the collision-camera
operation; detouring it for every vehicle barrel would change clipping state,
not prove a barrel origin.

That corrects the compressed R-V12 description without invalidating its
existing bounded hook. The HREK unit_get_camera_position evaluator at
0xD76E60 resolves the live 0x12C seat and authored camera marker, tests seat
bit 4 at 0xD77057-0xD7705F, and can select the occupant/head marker when that
bit is clear. The desired-angle consumer at 0x1E7934 calls the same evaluator
and subtracts its result from the target. These are native first-person camera
consumers. They do not prove that every mounted vehicle weapon fires from that
eye.

An official HREK tag export census covered 28 representative supported Reach
vehicle weapons across Warthog, Scorpion, Shade, Wraith, Ghost, Revenant,
Falcon, Banshee, Sabre, plasma/machine-gun turret, MAC and Corvette families.
The official barrel flags name bit 2 projectiles-use-weapon-origin and bit 15
projectile-fires-in-marker-direction. None of the 28 authored bit 15. Most
mounted barrels explicitly authored bit 2; Gauss, mounted machine-gun and some
other rows did not. Thus unit+0x214 is the supported categories' truthful
forward direction, while the origin remains title- and barrel-specific.
ReachWeaponAnchor is only the rendered personal first-person gun/muzzle effect
and supplies no vehicle barrel handle. No retail binding is invented from that
unrelated object, and the rejected historical marker-origin lineage in
REACH-EVIDENCE-MANIFEST.json remains rejected.

The implementation is therefore direction-truthful and origin-conservative:

1. The earlier generation-plus-time aim publication remains R-V18's independent
   input/steering feedback and does not drive this sight. After both Reach eyes
   have rendered and copied successfully, the stereo transaction publishes a
   separate seqlock record carrying the title generation, exact prepared-frame
   serial, source, full-salt unit/parent/definition/seat key and camera-local
   direction. Every completed pair publishes, including an on-foot or fallback
   source that revokes native sight ownership for that serial. A later failed
   retry cannot overwrite an earlier completed pair; a later completed retry
   replaces it. The compositor accepts only exact generation + exact serial +
   SeatedUnitAim + valid full key and vector.
2. The final validated compact camera already contains yaw references, title
   signs, pitch trim and clamp, head roll, View Follow OFF/ON, and headset
   smoothing. The reticle publisher expresses unit+0x214 in that final basis:
   local x is dot with forward-cross-up (right), local y is dot with up, and
   local z is negative dot with forward. This avoids reconstructing or
   inverting any of those states. Pure tests cover all three axis signs, a
   rolled basis round trip, normalization, invalid/non-orthogonal bases, exact
   serial/source/key admission, and completed-versus-failed retry semantics.
3. The Reach-only compositor builds one sign-correct normalized stereo-center
   quaternion and midpoint from the exact two projection views being submitted.
   It rotates the completed pair's camera-local aim by that quaternion and uses
   the same midpoint as the quad origin. This makes the overlay match the
   raster/submission relationship even when the rendered head pose is smoothed
   and the runtime reports canted eye orientations. Failure falls through to
   the existing controller/clamped sight; it never rejects Reach stereo.
4. Allows-weapons personal firing now requires two independent coherent
   seqlocks: the current full-salt occupation (including the authored
   allows-weapons bit) and the completed pair's rendered world-space eye.
   Both must match generation and the complete unit/parent/definition/seat key,
   remain within the rate-independent 100 ms freshness bound, and match the
   engine's firing unit. The entire hook, including its original title call, is
   covered by the Reach active-callback counter; teardown disables it, proves
   detour/trampoline quiescence, and retains pointers on removal failure.
   Drivers, mounted gunners, attached turrets and walk-up turrets retain their
   native barrel origin; the sight reports their true angle, but close-range
   eye-to-barrel parallax remains stock and is not misrepresented as solved.
5. No vehicle-barrel origin, evaluator-call census, steering policy, seat
   limits, camera trim, authored tag, or Workshop content changes in this
   transaction. The new runtime branch is gated to Halo Reach. Halo 3 and ODST
   continue through the existing controller ray and shared clamped fallback
   byte-for-behavior.

All math is per sample/per predicted frame and contains no refresh-rate
constant, so the design is invariant across the required 72-144 Hz headset
range. Headset acceptance must exercise a personal-weapon passenger, a
Warthog/Scorpion mounted gun, and a walk-up or attached Covenant turret with
View Follow OFF and ON, including near and far targets. It must record edition,
runtime, headset and refresh rate and confirm both that the sight moves
immediately onto the native weapon direction and that close barrel parallax
has not been falsely removed. This section does not advance CURRENT-STATE.

## R-V9 - acceptance still required

Nothing in this document changes the accepted pointer. Packaging, successful
tests, a clean launch, and correct logs are supporting evidence only.

The minimum headset pass for this candidate is:

1. identify MCC edition, OpenXR runtime, headset, refresh rate, source commit,
   and installed halo3xr.dll hash;
2. enter and exit a Warthog driver and passenger, checking camera placement,
   body, hands/gun, lean, recenter, steering, and per-seat F1 persistence;
3. enter Wraith driver and Wraith gunner, proving the marker fallback and
   child-frame/carrier split;
4. test one walk-up turret, one Falcon player position, one Banshee or Sabre,
   and one Scorpion, covering type 16, non-contiguous seats, aircraft
   yaw-only, and independent turret aim;
5. compare vehicle_cam_smoothing on/off and vehicle_bounce at 0 and a nonzero
   value;
6. compare vehicle_view_follow off/on, including a hill and a hard yaw change;
   exercise the runtime's available refresh rates across the supported
   72-144 Hz range, with endpoint coverage where the headset exposes it;
7. verify vehicle_hide_body and vehicle_hands_follow_body independently;
8. run on-foot before and after the seats, then a cutscene/theatre transition,
   confirming no stale seat state or camera-core teardown;
9. run the required Halo 3 regression and, because shared vehicle/input code
   changed, an ODST first-person vehicle regression.

Only the user's explicit headset result can advance docs/CURRENT-STATE.md.
Until then, all 25 Blender cameras remain authoring candidates and every
player-visible Reach vehicle behavior above remains headset-unverified.
