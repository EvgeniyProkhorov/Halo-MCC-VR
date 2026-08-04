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
| Own body hidden in first person | During the one synchronous stock outer-render call, the occupied seat's native third-person-camera bit 4 is cleared and restored in a finally path. No map file is changed. |
| Arms and gun ride the seat rather than the player's face | vehicle_hands_follow_body selects the rigid authored seat base for the Reach first-person model/controller roots. The view may still receive the configured head-bounce residual. |
| Per-seat live placement | The existing F1 forward, height, and left/right controls bind to a Reach-only 20-by-16 bank keyed by the exact current identity and raw seat index. |
| Optional vehicle-frame view | vehicle_view_follow follows yaw and roll-stable pitch for ground vehicles, yaw only for aircraft, and no carrier frame for walk-up turrets. Attached guns follow their ultimate carrier. |
| Entry/exit comfort | The shared debounced vehicle transition requests the existing position-only recenter on settled entry and exit when vehicle_recenter_on_seat is enabled. |
| Native vehicle controls and honest aim | Supported Reach seats publish the same vehicle state used by the shared controller-layout, steering, VR-turn ownership, movement, and clamped-reticle paths. Look-steered drivers use native turn input or the virtual wheel according to the same policy as Halo 3/ODST. |

This is experience parity, not implementation parity. Reach does not share
Halo 3's vehicle definition layout, seat stride, string IDs, marker resolver,
or object collection layout, and no such value is copied.

One limitation is deliberate: this candidate does **not** claim a Reach
passenger shot-origin transaction. The bit-4 change exists only while the
stock outer render call is executing. It is not left installed for simulation
or firing code, and no claim is made that it moves projectile or effect
origins. ODST's separate passenger-shot-origin work is itself listed as
headset-unverified in docs/CURRENT-STATE.md, so copying it would not be a valid
parity target.

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

All universal, Halo 3, ODST, and Reach placement values share these clamps:

| Axis | Minimum | Maximum |
| --- | ---: | ---: |
| forward | -1.00 m | +1.50 m |
| up | -1.00 m | +1.50 m |
| right | -1.00 m | +1.00 m |

The F1 panel selects one of three independent banks. While a generation-valid
Reach seat snapshot is active, the same three sliders bind to that exact
identity/raw-seat slot and the reset button clears its three set flags back to
the universal fallback. On foot, the sliders edit the universal values.

The Blender exporter emits 75 flat values (25 cameras times three axes) using
this exact grammar. The parser and save path round-trip the Reach prefix before
the generic Halo 3 prefix, preventing a Reach line from being consumed by the
wrong bank.

## R-V6 - transient native first-person-seat scope

vehicle_hide_body uses the title's own third-person-camera selector, but the
write has deliberately narrow ownership:

- It is attempted only for a complete, supported, generation-current Reach
  vehicle frame while vehicle_first_person and vehicle_hide_body are enabled.
- The live object, definition datum, tag, seat count, raw seat, and flags
  pointer are re-resolved immediately before the write.
- InterlockedCompareExchange clears only flags bit 4 and succeeds only if the
  complete original flags word still matches the sampled value.
- If bit 4 was already clear, no write is owned.
- The patch begins immediately before the synchronous stock
  main_render_view call and is restored in that call's finally path. It never
  spans two outer-render calls.
- Restore re-resolves the same live seat and restores the original complete
  flags word only if the current word still equals the value this feature
  wrote. A concurrent engine or tag change wins; stale bits are never written
  over it.
- A surviving unexpected owner is reconciled at the next normal-player frame.
  Failure is recorded as a body-hide feature fault, not as a reason to disarm
  Reach stereo.
- No on-disk tag or map file is patched, and Easy Anti-Cheat is not involved.

This scope is strong enough to let the native first-person render path observe
the occupied seat during that render. It is intentionally **not** evidence that
Reach will hide every part of the body correctly, build the desired weapon
models, or move firing origins. Those results require the headset.

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
and lets the user mark a translated camera as placed. It never moves a camera
itself. In Blender, run the embedded text, press N in the 3D view, and use the
Reach Seats panel.

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
7. verify vehicle_hide_body and vehicle_hands_follow_body independently;
8. run on-foot before and after the seats, then a cutscene/theatre transition,
   confirming no stale seat state or camera-core teardown;
9. run the required Halo 3 regression and, because shared vehicle/input code
   changed, an ODST first-person vehicle regression.

Only the user's explicit headset result can advance docs/CURRENT-STATE.md.
Until then, all 25 Blender cameras remain authoring candidates and every
player-visible Reach vehicle behavior above remains headset-unverified.
