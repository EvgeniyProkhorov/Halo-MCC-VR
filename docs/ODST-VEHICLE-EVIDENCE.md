# ODST first-person vehicle evidence

Evidence record for porting the headset-accepted Halo 3 first-person vehicle
system (C20-C24, baseline `efe8bdb`) to Halo 3: ODST. Method mirrors
`docs/HALO3-VEHICLE-EVIDENCE.md`: official editing-kit tag exports first,
then kit-binary homologs, then unique signatures in the pinned retail
`halo3odst.dll`. Halo 3 values are leads only; nothing ships on a Halo 3
offset without ODST-specific proof (AGENTS.md).

Offline artifacts live in the ignored `out/analysis/odst_vehicle_markers/`
(sibling of the Halo 3 dump set `out/analysis/h3_vehicle_markers/`).

## O-E0 - ODST vehicle census (tags + campaign scenarios, 2026-08-03)

Source: `N:\SteamLibrary\steamapps\common\H3ODSTEK` with `H3ODSTEK.7z`
extracted in place (first extraction on this machine; the kit previously had
no `tags\` tree). 48 `.vehicle` tags exist under `tags\objects\vehicles\`.

Campaign presence was measured by scanning each `tags\levels\atlas\*.scenario`
binary for embedded vehicle tag-path strings (palette references; presence
does not by itself prove player-enterable - seat flags decide that, see O-E1):

| Vehicle | Appears in (atlas scenarios) |
| --- | --- |
| warthog | h100, l300, sc100, sc110, sc130 |
| mongoose | h100, l300 |
| ghost | h100, l300, sc110, sc120, sc130, sc150 |
| scorpion | h100, l300, sc120, sc130 |
| wraith | h100, l300, sc100, sc110, sc120, sc130, sc150 |
| banshee | h100, l300, sc100, sc110, sc120, sc130, sc140, sc150 |
| brute_chopper | sc110 only |
| shade | l300, sc110, sc120, sc140, sc150 |
| olifaunt | h100, l300 |
| odst_pod | c100, c200, h100, sc100, sc110 |
| machinegun_turret (weapon) | every mission except c100/c200 |
| plasma_cannon (weapon) | every mission except c100/c200/l300-lite |
| missile_pod (weapon) | sc140 only |
| phantom / pelican | AI/cinematic only |

**Never referenced by any ODST campaign scenario:** `hornet`, `mauler`
(Prowler), `warthog_gauss`, `wraith_anti_air`, and the `turret_standalone`
chaingun/gauss family. The Halo 3 hornet/mauler seat entries therefore never
resolve in ODST; no ODST authoring is needed for them.

**New versus the Halo 3 support set:** `shade` (Halo 3's `Halo3VehicleId` has
no Shade entry - the retail H3 campaign never let the mod meet one) and
`olifaunt` (ODST-only; expected scripted/non-player - seat flags in O-E1
decide). `odst_pod` is the intro drop pod; it has seats but no driving
gameplay and is out of scope for seat parenting.

First structural comparison (ODST `warthog.vehicle` export): 6 seats -
`warthog_d` (flags `262164` = `0x40014`), `warthog_p` (flags `4208` =
`0x1070`), plus four boarding seats `warthog_b_*`. Driver authors no camera
marker; passenger authors `camera_rider`. Field names, seat labels, flag
words and camera-marker pattern are all identical to the Halo 3 dump, and the
ODST banshee render-model node 0 default translation `(0.0131864, ~0,
0.652337)` equals Halo 3's. The tag format and authoring are the same
generation; the full mechanical diff is O-E1.

## O-E1 - per-seat tag diff vs Halo 3 (2026-08-03)

280 tags exported via `tool.exe export-tag-to-xml` (absolute tag paths, cwd at
the kit root - the tool resolves `tags\` from the current directory). Note the
tool emits non-well-formed XML: null tag references print as raw `0xFF` bytes
and `<unnamed>` / `<unavailable>` appear unescaped; any strict parser must
scrub these first (the Halo 3 dumps have the identical quirk). Diff artifacts:
`out/analysis/odst_vehicle_markers/odst_vs_h3_diff.{json,md}`.

### Headline result

For every vehicle ODST shares with Halo 3 - warthog (+chaingun/gauss turrets),
mongoose, ghost, brute chopper, banshee, hornet, mauler (+turret), scorpion
(+anti-infantry turret), wraith (+turret), and the three walk-up turret
weapons - the render-model node hierarchies are byte-identical (max default
translation delta 0.0), all shared marker translations agree within 1e-4 wu,
and every player seat's label, seat marker, flag word and camera marker are
IDENTICAL to Halo 3. (Cosmetic-only deltas: FX markers `fx_gun_l/r`,
`fx_treads`, `fx_cin_impact`, ODST-variant foot markers on the hornet, and a
`rednifhtap` marker on the chaingun.)

**Verdict: REUSE `kHalo3SeatPoints` and the shipped Halo 3 seat trims
verbatim for every shared seat.** The Blender kit does not need to be re-run
for these.

### The one real behavioral delta: scorpion rider seats

ODST `scorpion.vehicle` rider seats (`scorpion_p_lf/rf/lb/rb`, seat indices
1-4) drop Halo 3's `invalid for player` bit: flags `0x10270` (H3) ->
`0x270` (ODST). ODST's squad rides the tank in Kizingo Boulevard, and the
player may take those seats too. Each rider seat authors a camera marker
(`passenger_lf_camera` etc., on the per-corner rider nodes). These need
authored ODST seat points (O-E1b). `scorpion_snow` keeps the H3 flags
(riders invalid), matching its Halo-3-side use.

### New ODST-only player seats

| Vehicle | Seat | Flags | Camera marker | Player-enterable |
| --- | --- | --- | --- | --- |
| shade | `shade_d` (0) | `0x1C` (driver+gunner+3rd person cam) | `shade_d_camera` | YES - needs authored point |
| warthog troop turret | `warthog_p_l/r/b` (0-2) | `0x70` | `warthog_p_*_camera` | YES (if a troop hog is placed) - author from markers |
| olifaunt | `driver` (0) | `0x10005` (**bit 16 invalid for player**) | `driver_camera` | **NO** - excluded, recorded |
| odst_pod | `pod_d` (0) | `0x4` | `camera` | cinematic ride - excluded |
| turret_standalone chaingun/gauss | `warthog_g` | `0x40001318` | `camera_gunner` | tags exist but never scenario-referenced; identical seats to the hog turrets |
| wraith_anti_air (+AA turret) | as wraith | - | - | never scenario-referenced; skip |

Hornet and mauler seats are byte-identical to Halo 3 but neither vehicle is
referenced by any atlas scenario; the shared table entries cover them at zero
cost if a mod ever spawns one.

## O-E1b - authored ODST seat points (2026-08-03)

Method: seat markers composed through the render-model default node chain
(quaternion parent-chain composition; every load-bearing node's default
rotation is identity, so this equals the kit's node-world + marker-translation
formula), then the Halo 3 kit's seated-eye convention `EYE_UP_WU = 0.33`
above the seat marker. Two validation cross-checks passed:
- mauler turret `mauler_g` (node 1, local `-0.0104,-0.0003,+0.0047`) composes
  to `(-0.2081, -0.0002, +0.2504)` = the engine's own root-frame
  `mauler_g_camera` marker exactly;
- scorpion `anti_infantry_turret` attach composes to `(0.4673, -0.1900,
  0.5982)` - x/y equal the headset-accepted Halo 3 mounted-gunner carrier
  point `(0.4673, -0.1903, 0.8659)` (z there includes the authored eye
  height above the attach).
The XML `inverse *` node fields are degenerate in tool.exe exports (near-zero
scales) and were not used.

Candidate eye points, vehicle tag space, world units (same convention as
`kHalo3SeatPoints`; trims apply as `{x+fwd, y-right, z+up}`):

| Vehicle | Seat | Frame | x | y | z | Basis |
| --- | --- | --- | --- | --- | --- | --- |
| scorpion | 1 (`scorpion_p_lf`) | vehicle | 0.3217 | 0.8483 | 0.7864 | seat marker (node 3) + 0.33 |
| scorpion | 2 (`scorpion_p_rf`) | vehicle | 0.5289 | -0.8483 | 0.7890 | seat marker (node 5) + 0.33 |
| scorpion | 3 (`scorpion_p_lb`) | vehicle | -0.7294 | 0.9483 | 0.7910 | seat marker (node 2) + 0.33 |
| scorpion | 4 (`scorpion_p_rb`) | vehicle | -0.5277 | -0.9483 | 0.7896 | seat marker (node 4) + 0.33 |
| shade | 0 (`shade_d`) | vehicle | -0.1252 | 0.0000 | 0.9791 | seat marker (node 1 `floater`) + 0.33 |
| troop hog | 0 (`warthog_p_l`) | carrier | -0.5215 | 0.0900 | 0.9686 | hog `turret` attach + marker + 0.33 |
| troop hog | 1 (`warthog_p_r`) | carrier | -0.5215 | -0.0900 | 0.9686 | hog `turret` attach + marker + 0.33 |
| troop hog | 2 (`warthog_p_b`) | carrier | -0.7872 | 0.0000 | 0.9583 | hog `turret` attach + marker + 0.33 |

Notes for O2 design:
- `shade_d_camera` is authored at the ORIGIN of node 0 (degenerate) - the
  seat-marker point above is the only usable anchor. The shade seat rides
  node 1 (`floater`), which rotates with aim; resolving through the live
  node matrix (the accepted C18 math) makes the eye follow the gun mount.
- Scorpion rider camera markers sit only ~0.22 wu above the seat (chest
  height); the kit's 0.33 eye convention was kept instead, matching how all
  accepted Halo 3 root-seat points were authored.
- Config slot math today keys seats 0-2 plus one mounted gunner. ODST's new
  cases exceed it: scorpion riders are seat indices 1-4 (index 3 and 4 have
  no slot), and the troop hog has THREE mounted seats on one attachment
  (H3 assumed one). The ODST trim bank needs its own slot mapping for these;
  decide in O2.
- Warthog `turret` attach marker world: `(-0.5250, 0.0000, 0.4386)`.

### Identity design for the new vehicles (pending O1 probe confirmation)

Physics-block facts from the tag dumps: the shade authors ZERO physics-type
blocks - the same shape as the walk-up turrets in BOTH titles (H3 `mg_turret`
and ODST `machinegun_turret` both author `turret_block,0`), so the engine's
type accessor normalizes that case itself; whatever it returns for walk-up
turrets in Halo 3 (the shipped rule is `directType == 5 && unit_in_vehicle
== 0` -> StationaryTurret, game.cpp:6492) it will also return for the shade.
The troop-hog turret authors zero physics blocks too and resolves through its
CARRIER (warthog, engine_moment 2000) via the mounted flag, exactly like the
chaingun - no new identity needed.

Proposed shade discriminator, all definition/native-resident: shade is a
root vehicle whose seat 0 carries the DRIVER flag bit (flags `0x1C` = driver
+gunner+3rd-person-cam; walk-up turrets lack the driver bit, e.g. mg
`0x5500018`), and being a real vehicle it is expected to report
`unit_in_vehicle == 1` where walk-up turrets report 0. The O1 probe must log,
for AI-crewed shades (present on l300/sc110/sc120/sc140/sc150 without the
player entering one): the accessor's returned type, the native's return for
the AI unit, and the seat-0 flags dword - the discriminator is finalized
from that log, not from this expectation.

## O-E2 / O-E3 - halo3odst.dll binary evidence (2026-08-03)

Produced by a six-family derivation pass, each family independently
re-derived by an adversarial verifier with fresh scripts (12 analyses total;
6 proven, 5 verdicts CONFIRMED, 1 PARTIAL on narrative only). Full structured
results + per-agent journals preserved in
`out/analysis/odst_vehicle_markers/odst-binary-evidence-{workflow-full.json,
journal.jsonl}`. Module identity re-hashed by every agent: `halo3odst.dll`
SHA-256 `5BB20976EFDFD9E1CE59C589339804725FEC239021027C8D65B2733EAB94829A`,
11,496,920 bytes, timestamp `0x68A0F232`, SizeOfImage `0x4797000`, ImageBase
`0x180000000`. Method: the accepted name-string -> hs descriptor -> evaluator
-> native chain, instruction-level capstone disassembly against the halo3.dll
homolog, kit-first tag-layout proof (atlas_tag_test.exe struct defs +
H3ODSTEK guerilla.exe field tables, method validated by reproducing H3's
proven values first), and whole-file AOB uniqueness counts in BOTH titles.

### Proven values (ODST vs Halo 3)

Unit/object access chain (family CONFIRMED):

| Item | Halo 3 | ODST | Notes |
| --- | --- | --- | --- |
| `unit_in_vehicle` literal / hs descriptor / evaluator | `0x7F16D0`/`0x7EB1F8`/`0x1E5AF8` | `0x812F38` / `0x837BF8` / `0x2188AC` | descriptor layout same: name +0x20, evaluator +0x38, stride 0x50 |
| native `unit_in_vehicle` | `0x3A36F4` | `0x3C54CC` | body len 0xAA both; instruction-homologous |
| engine TLS index global | `0xA39F9C` | `0xA8FB1C` | derived from two independent bodies, agree |
| TLS slot of object table | `+0x38` | **`+0x20`** | DIFFERS |
| object-table entries / stride / data / kind | `+0x48` / 0x18 / `+0x10` / byte +3 (1=vehicle) | same | |
| unit seat word | `+0x24E` | **`+0x262`** | 0xFFFF = unseated |
| unit parent handle | `+0x10` | `+0x10` | |
| `object_get_ultimate_parent` | `0x346AFC` | `0x382334` | flags-bit-26 gated walk, same shape |
| `vehicle_definition_get_type` | `0x396D84` | `0x3DB46C` | rip disp32s at body +3/+7 and +25/+29, same as H3 |
| tag instance table / tag data base globals | `0xA49018` / `0x1FCF4C8` | `0xA9F0A8` / `0x2022AA8` | BSS-resident: contents runtime-only, consumers MUST null-check |
| physics-type block array | def`+0x2C0` | **def`+0x2E0`** | 10 records stride 0xC; first non-empty index, 0xB if none |

Player unit getter (CONFIRMED): ODST `player_mapping_get_unit_by_output_user`
= `0x109260`; reads engine TLS block `+0x108` -> player-mapping globals
(0xE8-byte struct), unit-handle array at `+0xC8`, stride 4 per output user.
CAUTION: a byte-identical sibling at `0x109230` returns the `+0xB8`
player-index array - never hook by prologue similarity alone.

Vehicle definition layout (CONFIRMED, kit-first, key anchors retail-pinned):

| Item | Halo 3 | ODST |
| --- | --- | --- |
| seats tag block | def`+0x258` | **def`+0x278` (count) / `+0x27C` (data dword; ptr = tagBase + dword*4)** |
| seat record stride | 0xD4 | 0xD4 (same) |
| seat flags dword / bit 4 = third person camera | seat`+0x00` | same; full 31-bit flag order identical |
| unit-camera struct / camera marker name | - | seat`+0x68` / seat`+0x6C` |
| camera tracks block | seat`+0x80` | seat`+0x80` (count) / `+0x84`; element 0x10 |
| jeep `engine_moment` / scout `specific_type` | elem`+0x14` / elem`+0x28` | same (element sizes 0x40 / 0x70) |

Why everything shifted: ODST grew `_object_definition` 0xF8 -> 0x108 and the
unit block 0x1C4 -> 0x1D4 (+0x20 total); loaded vehicle definition totals
0x41C. The retail seated-camera chooser at `0x1535D3` reads seat word
unit`+0x262`, seats offset def`+0x27C`, seat stride 0xD4 and returns flags
bit 4 - one instruction run pinning three of these at once.

Node banks and render-model chain (CONFIRMED):

| Item | Halo 3 | ODST |
| --- | --- | --- |
| node-bank byte-size word / rel-offset word | obj`+0x136` / `+0x138` | **obj`+0x12A` / `+0x12C`** |
| orientation-bank size / offset words | obj`+0x132` / `+0x134` | **obj`+0x126` / `+0x128`** |
| node matrix stride | 0x34 | 0x34 (same) |
| interpolated node-bank provider | `0x1846AC` | `0x1B3938` (same body size 0x1EA, 4 callsites each title) |
| internal marker resolver (6-arg, 0x70 output record) | `0x343D74` | `0x37F514` (same body size 0x281; record: nodeIndex +0x00, nodeMatrix +0x04, matrix +0x38, world translation +0x60, radius +0x6C) |
| loaded-tag chain vehi`+0x40` -> hlmt`+0x0C` -> mode`+0x30`, node stride 0x60 | same | same |
| render-model marker groups | - | mode`+0x40`, group record 0x10, marker record 0x24 (node byte +0x2, translation +0x4) |
| provider internals (interp record stride / bank / buffer stride / TLS interp slot) | 0x340C / +0x44 / 0x1A23EF4 / +0x88 | **0x3410 / +0x48 / 0x1A28474 / +0x78** |

String ids (values CONFIRMED; drift narrative per verifier): ODST `head` =
**0x9F** (independently proven from the static string-id table at RVA
`0x7C4E80`, 0xF99 records of {u64 id, char* name}, record #159; H3 re-proven
0x9F by the same walk). But per-title drift is REAL: `primary_trigger` is H3
0xD4 vs ODST 0xD5; ODST makes 145 set-0 insertions at multiple points (first
at 0xC5), so set-0 ids must NEVER be carried across titles - only ids
re-derived per title (or read from live registries) are valid. `driver` /
`passenger` / `gunner` / bare `camera` are NOT in the static table at all
(tag-content dynamic ids; cannot be offline constants).

### Production AOBs (uniqueness verified against both full modules)

- ODST `unit_in_vehicle` native, anchors native+0xA (1 hit ODST, 0 in H3):
  `45 32 DB 41 83 C9 FF 41 3B C9 0F 84 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 41 B8 20 00 00 00 0F B7 C9 48 8B 04 D0 4A 8B 14 00 48 8D 04 49 48 8B 5A 48 48 8B 7C C3 10 66 44 39 8F 62 02 00 00`
  (TLS index global = matchRVA+6+dword@match+2 when anchored at the `8B 15`.)
- ODST `vehicle_definition_get_type` (1 hit ODST, 0 H3; instance table from
  disp32@+3, tag base from disp32@+25):
  `48 8B 05 ?? ?? ?? ?? 0F B7 C9 8B 54 C8 04 85 D2 75 04 33 D2 EB 0B 48 8B 05 ?? ?? ?? ?? 48 8D 14 90 33 C0 45 33 C0 48 81 C2 E0 02 00 00 B9 0B 00 00 00 83 3A 00`
- ODST player-unit getter full body (1 hit ODST, 0 H3; +0xC8 pinned to
  exclude the +0xB8 sibling):
  `83 C8 FF 3B C8 74 27 8B 15 ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 41 B8 08 01 00 00 48 63 C9 48 8B 04 D0 4A 8B 04 00 8B 84 88 C8 00 00 00 C3`
- Seated-camera chooser (1 hit ODST, 0 H3; pins seat word 0x262 + seats
  0x27C + stride 0xD4 + flag bits):
  `44 0F B7 85 62 02 00 00 83 C8 FF 66 44 3B C0 74 ?? 41 0F B7 07 41 8B 4C C4 04 85 C9 75 04 33 C0 EB 04 49 8D 04 8E 8B 88 7C 02 00 00 85 C9 75 04 33 D2 EB 04 49 8D 14 8E 49 0F BF C0 BD 01 00 00 00 48 69 C8 D4 00 00 00 44 8B 1C 11 41 8B C3 41 C1 EB 04 44 23 DD C1 E8 06`
- Interpolated node-bank provider entry (1 hit each title):
  `48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 54 41 55 41 56 41 57 48 83 EC 30 4C 8B EA 0F 29 70 C8 48 8D 50 20 8B F9 4D 8B F0 E8 ?? ?? ?? ?? 33 DB 48 85 C0`
- Marker resolver entry (1 hit each title):
  `48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 66 44 89 48 20 57 41 54 41 55 41 56 41 57 48 83 EC 70 45 33 F6 8B E9 49 8B F8 44 8B FA 8B D9 45 0F B7 DE`
- Root-fetch bank cluster, ODST-exact (1 hit ODST, 0 H3 - usable as a title
  discriminator because it pins both the `+0x12C` bank read and TLS slot 0x20):
  `4C 0F BF A3 2C 01 00 00 4C 03 E3 B8 20 00 00 00`
- String-id table loader prologue (1 hit each title; the `lea` at match+0x1C
  carries disp32 to the table base, so a runtime self-check can confirm record
  0x9F reads `head` before trusting the constant):
  `48 89 5C 24 08 48 89 74 24 20 57 48 83 EC 20 33 DB 48 8B F1 39 1D ?? ?? ?? ?? 7E 7A 48 8D 3D ?? ?? ?? ?? 48 63 06 48 8B D0 89`
- Seated-camera evaluator core (1 hit each title, byte-identical across them;
  pins `seat*0xD4` and the camera marker string id at `seat+0x6C`):
  `4C 69 F0 D4 00 00 00 4C 03 F1 41 38 9A 96 00 00 00 75 0B 45 33 D2 45 39 56 6C 74 33 EB 03 45 33 D2 41 8B 56 6C`

Every ODST-specific pattern above matches exactly once in `halo3odst.dll` and
zero times in `halo3.dll`, so install-time resolution fails loud rather than
binding the wrong title. The two cross-title patterns are marked as such and
are only safe where the caller already knows which module it is scanning.

### O-E2f - object table is the engine's generic data array (proven)

Derived after the six-family pass, because enumerating live objects (rather
than only the player's handle chain) is what lets the O1 probe validate these
offsets against AI-crewed and parked vehicles with no player seat entry.

The pointer at engine-TLS `+0x20` is a `data_new_full("object", 0x800, 0x18)`
array (ODST creation site `0x37A7A8`; Halo 3's homolog at `0x33F5D0` stores to
TLS `+0x38`). Header layout, proven field-by-field from `data_initialize`
(`0x142008`), `datum_new` (`0x142270`), `datum_delete` (`0x142574`) and
`data_make_valid` (`0x14209C`) - **byte-identical in both titles**:

| Offset | Field |
| --- | --- |
| `+0x00` | name, `strncpy`'d into 0x20 bytes - reads `"object"` |
| `+0x20` | maximum count = `0x800` |
| `+0x24` | element size = `0x18` |
| `+0x29` | valid/connected flag (1 once live) |
| `+0x2C` | signature `0x64407440` (`'d@t@'`) |
| `+0x3C` | **first_unallocated - the only valid iteration bound** |
| `+0x40` | actual live count (NEVER a bound) |
| `+0x48` | element array pointer |
| `+0x50` | in-use bitvector |

Entry validity: the identifier word at `entry+0x00`; **zero means free**.
Live identifiers are always `>= 0x8000` (the allocator wraps `0xFFFF` back to
`0x8000`), and a handle is `(identifier << 16) | index` - so `0xFFFF` is a
LIVE identifier here, not a sentinel. The array is flat but **sparse**: holes
below the high-water mark must be skipped (`datum_delete` only shrinks
`+0x3C` when the top slot dies).

This gives the probe a self-verifying walk: it checks the header's own name
and `'d@t@'` signature, element size and capacity before iterating, so a wrong
TLS slot produces a loud log line instead of a bad read. Anchor AOB (1 hit in
each title, `datum_new`): `44 8B 41 3C 83 CA FF 8B 59 38 4C 8B D1`.

### O-E2g - parent-node byte and the two discriminators (proven 2026-08-03)

Closed three of the four gaps below by direct retail disassembly.

**Object parent-node byte = `objectData+0x14`, UNCHANGED from Halo 3.**
Signed int8; `0xFF` (-1) means "no specific node" and the engine falls back to
the parent's own orientation. ODST body `0x3CDEB8-0x3CDF6B` is an
instruction-for-instruction twin of Halo 3's `0x38C804`: `0x3CDEF2`
`cmp dword [rdi+0x10], -1` (parent handle), `0x3CDF0F`
`cmp byte [rdi+0x14], 0xFF`, `0x3CDF2F` `movsx rax, byte [rdi+0x14]` followed
by `imul rax,rax,0x34` and `movsx r9, word [rdx+0x12C]`. That last operand is
ODST's OWN node-bank offset, so the same body proves the surrounding layout
really did shift while this field really did not. A family scan finds 13 read
sites in each title, all at `+0x14`. AOB (ODST-exact, `h3=0`):
`48 0F BE 47 14 48 8B 54 CA 10 48 6B C0 34 4C 0F BF 8A 2C 01 00 00`.

**human_jeep `engine_moment` = element `+0x14`** (retail-confirmed). The jeep
element accessor is `0x3DB4F0`; `0x4147C2` takes `lea rdx,[elem+0x14]` as the
engine sub-struct base and the torque update divides its first float at
`0x4276ED` (`divss xmm7, [rsi]`). Neighbours line up too (max angular velocity
`+0x18`, gears `+0x1C/+0x20`).

**alien_scout `specific_type` = element `+0x28`, int8** (retail-confirmed).
Scout accessor `0x3DB558`; `0x417432` compares `cmp byte [rax+0x28], 4`,
byte-for-byte the same as Halo 3's `0x3BF61E`.

**Clean negative: seat camera-tracks `+0x80/+0x84` has no retail consumer** in
EITHER title. Camera tracks are opened from a global tag table, not from the
seat: ODST `0x39C3F8` walks a cached definition at `[0xA9C9B8]`, block at
`+0x278`, element stride `0x58`, track tag index at element `+0x34`, and
stores the result into the unit's runtime array at `unit+0x27C`. The
kit-derived seat sub-offsets are therefore kit-only and must never be treated
as engine-validated. Nothing in this port reads them.

### Open gaps

1. ~~Object parent-node byte~~ - PROVEN above at `+0x14`.
2. **Render-model node inverse at record `+0x28`.** Byte-verified from the kit
   field table and guerilla's own serialization comment, but no retail ODST
   instruction consuming it was found (three systematic scans returned only
   unrelated families). The structure is sound; the runtime consumer is not
   identified.
3. ~~Three kit-metadata-only seat sub-offsets~~ - the two this port actually
   reads (jeep `engine_moment`, scout `specific_type`) are retail-confirmed in
   O-E2g; camera-tracks `seat+0x80` is proven to have no retail consumer and
   is not read by anything here.
4. **Shade and troop-hog identity discriminators** (see the O-E1 identity
   section): the shade authors zero physics blocks, so what
   `vehicle_definition_get_type` returns for it must be observed, together
   with the native's answer for an AI shade gunner and the seat-0 flags.

All four are readable from AI-crewed and parked vehicles in an ordinary ODST
session - none requires the player to enter a seat.

## O3 - view follow and the virtual wheel (2026-08-03)

Brings the last two pieces of the 0.3.2 vehicle package to ODST. Both are
behavior, not new engine evidence: they consume the seat state O2 already
publishes, and they reuse the accepted Halo 3 composition rather than
re-deriving it.

**One publication, two titles.** The roll-stable follow record gains an
`odstOwner` tag. Only one title's vehicle transaction can ever be armed, so a
single publication serves both, and every shared consumer - the tracked
controller basis, the hand room-translation, the closed-loop aim - keeps
working with no edit at all. The reader validates the tag against whichever
title is actually driving, so a stale cross-title sample can never pitch the
wrong camera.

**What follows, and what deliberately does not.** ODST reuses Halo 3's
families: ground vehicles follow yaw and pitch with roll always excluded, so
suspension bank or a side impact cannot tip the horizon; Banshee and Hornet
stay yaw-only, because their flight crosses vertical where forward alone
cannot pick a continuous roll-free up. The ODST-specific rule is the **Shade**:
it has a seat point but its own body is what its aim turns, so following it
would cancel the closed loop's feedback exactly as a walk-up turret would.
It is excluded from follow, from pitch, from driver status and from steering
authorship.

**Steering.** `OdstSeatAuthorsSteering` gates on the same follow toggle, so
with follow OFF every control stays exactly as it shipped. The wheel and the
"who owns the turn stick" gates now ask Halo 3 first and fall through to ODST,
so a look-steered ODST driver takes the wheel and an ODST aircraft correctly
keeps the plain stick.

## O4 - passenger aim truth and steady seat exposure (2026-08-03)

### Passenger shots not following the crosshair - ROOT CAUSE FOUND

Not a placement or parallax problem. **The seat itself bounds the weapon.**
The warthog rider seat authors, in its own tag:

| field | value |
| --- | --- |
| `yaw minimum` / `yaw maximum` | `-90` / `+90` degrees |
| `pitch range` | `-45` / `+45` degrees |

Those limits are relative to the HULL. With a world-locked view the allowed
cone rotates with the vehicle while the VR crosshair stays fixed in the world,
so as soon as the hand points outside the cone the engine simply refuses to
aim there. The closed loop then holds full stick deflection indefinitely while
the reticle keeps promising a shot that lands somewhere else - exactly the
reported "shots dont follow the crosshair at all times", and exactly why the
DRIVER seat is unaffected (its aim is steering, with no such clamp).

**Fix:** nothing can make the gun exceed its seat's limit, so the crosshair is
made honest instead. When the aim error stalls past normal tracking lag
(>~7 degrees, not shrinking, for 250 ms) while a first-person seat owns the
view, the reticle is drawn along the engine's REAL aim rather than the hand
ray. Shots then follow the crosshair by construction. The reticle is
documented as "the truth"; while clamped, the hand ray is the one thing that
is not. Gated to vehicle seats, so on-foot aiming is untouched.

### Dashboard auto-exposure - ROOT CAUSE FOUND

Looking down at a vehicle dash swung the whole scene's brightness. That is
ODST's eye-adaptation integrator (`halo3odst.dll+0x2B1E90-0x2B2226`) reacting
to a large dark surface filling its luminance measurement - something flat
play never provokes because nobody looks at the dash.

The integrator's blend store at `+0x2B2201` is guarded at `+0x2B21BD` by a
static byte; nonzero skips the write and freezes adaptation. That byte is the
engine's own **`render_exposure_lock`**: named in HREK's
`bin/debug_menu_init.txt`, present in H3ODSTEK's unstripped debug-var table,
and matched to retail through the per-view save/restore of the render-debug
block next to the `"player_view %d"` string. Retail default 0.

MCC ships the exposure debug-var records with NULL value pointers, so the
storage is decoded from the instruction instead - AOB unique (1 hit in each
title): `40 38 2D ?? ?? ?? ?? 0F 57 C9 0F 28 C4 F3 0F 5C C6 F3 0F 5A C8`,
lock byte = `matchRva + 7 + disp32@match+3` (ODST `+0x8F0D70`, Halo 3
`+0x8AD070`).

**Fix:** assert that byte while a first-person seat owns the view, and assert
the known stock 0 otherwise - asserted, never restored from a captured value,
because it is a module global that outlives the camera generation and the
engine's own per-view save/restore copies the block around it. Engaging only
once seated means the held level is one the engine already adapted to, not a
load-time initial value. Scripted/cinematic exposure deliberately bypasses
this lock in the engine, so cutscenes still light themselves. New config key
and F1 toggle `vehicle_steady_exposure` (default on).

Rejected as more invasive or unsafe: the adaptation RATE is a shared `.rdata`
constant (patching it is unattributable); exposure min/max arrive as tag-fed
stack arguments (would need a tag patch or an argument hook); the
`render_exposure_stops` float is a possible companion trim but was not needed.
Nothing here touches `halo3odst.dll+0x2A6308`, the function whose earlier
scaling deleted the native HUD.

## SUPERSEDED: passenger-seat shots do not always follow the crosshair

User report 2026-08-03, on **O2** (`40bd330`) with `vehicle_view_follow = 0`
and `crosshair_distance_m = 29.0`: the passenger seat placement "looks great"
but "shots dont follow the crosshair at all times". Two candidate mechanisms,
distinguishable by one test; do not fix before the test says which.

**1. Hull rotation versus a world-locked view (fits "not at all times").**
Riding as a passenger, the engine rotates the occupant's aim WITH the vehicle.
With the view world-locked, the VR crosshair stays fixed in the world while
the game's aim is dragged by the hull, so the closed-loop aim author is
permanently cancelling the vehicle's yaw rate instead of just tracking the
player's hand. Its output is a stick deflection clamped to +/-1 against the
game's own maximum look rate, so a hard corner saturates it and the aim lags
behind the crosshair; driving straight lets the error settle and shots land
true. That is exactly the intermittent pattern reported, and it is precisely
what the C25 follow was built to remove: with follow ON the aim reference
rotates with the hull, so the loop only has to track the hand.
**O3 already ships this for ODST** - the test is simply to enable the toggle.

**2. Seat-anchor parallax (would NOT be intermittent).** The VR camera now
sits at the authored seat point while Halo spawns first-person bullets from
its own camera ([[bullet-origin-runtime-only]]). Any offset between them makes
the aim ray and the bullet ray converge only at `crosshair_distance_m`, so
shots miss by an amount that varies with target RANGE, including while parked.

**Discriminating test:** park the vehicle and shoot a near and a far target
from the passenger seat. Accurate while stationary = mechanism 1 (enable
follow). Off while stationary, and off by a different amount near versus far =
mechanism 2 (tune `crosshair_distance_m`, currently 29.0).

### O-E3 status

Offline evidence is COMPLETE and sufficient to author candidate O1 with
title-specific proof for every offset it touches, and sufficient for O2 on
everything except the four gaps above. No Halo 3 constant is carried into ODST
anywhere: the `+0x20` structure shift, the differing TLS slot, seat word, node
bank cluster and physics/seat block offsets are each independently derived,
and the H3-exact byte patterns provably match zero times here.
