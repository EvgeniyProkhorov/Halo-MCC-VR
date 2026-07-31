# Halo 3 vehicle evidence

Evidence base for Halo 3 first-person VR vehicles (detection, seat camera,
motion steering). Method mirrors the accepted Reach player-unit method
(`docs/REACH-SIGNATURE-EVIDENCE.md` §All-vehicle controller-layout predicate):
semantics are established from the official Halo 3 Editing Kit first; the
pinned retail module is used only to match and verify; ManagedDonkey
contributes names and code shapes only, never offsets. Every offset below was
read out of actual bytes with the derivation recorded; nothing is copied from
Reach, ODST, or Halo Online.

Evidence sources, with identities:

| Source | Identity |
| --- | --- |
| Pinned retail module | `N:/SteamLibrary/steamapps/common/Halo The Master Chief Collection/halo3/halo3.dll`, 11,127,768 bytes, SHA-256 `B209D8454B12DC77E54CCD2C9924EC8D44B8619D21CF98E36FFAF601E67EFB63` |
| H3EK tool.exe (tag XML exports) | SHA-256 `1DCF51EC39BDF61B6A6AE40917908CF23A247BFEDB7A4BFF07124F508EE0F3B5` |
| H3EK guerilla.exe (enum/flag name tables) | SHA-256 `2CD5F9AF4FCDCF1EC04D21521CD161852490D292E58D37EE2A35068EAEF08C50` |

Retail section mapping used for every file-offset/RVA pair: image base
`0x180000000`; `.text` VA `0x1000` = raw `0x400` (file = RVA − 0xC00);
`.rdata` VA `0x6FA000` = raw `0x6F8600` (file = RVA − 0x1A00).

## E1 — Vehicle tag ground truth (official H3EK tool exports)

Produced with `tool.exe export-tag-to-xml` (22 vehicle/turret tags) and enum
name tables read from guerilla.exe's own pointer arrays (consecutive
name-string arrays at recorded file offsets, so order is proven, not assumed).

### Enums and flags

- **Vehicle physics type** is not a char enum in the tag: the vehicle tag
  stores a "physics types" struct of 10 tag-blocks in fixed order, and the
  authored type is the single block with element count 1:
  `0=human_tank, 1=human_jeep, 2=human_plane, 3=alien_scout, 4=alien_fighter,
  5=turret, 6=mantis, 7=vtol, 8=chopper, 9=guardian`.
  This confirms ManagedDonkey's `e_vehicle_type` names and order exactly.
  Proven by all 22 XML exports plus guerilla.exe consecutive strings
  `type-human_tank`..`type-guardian` at file offsets `0xF1D108..0xF1D1B0`,
  immediately after `vehicle_physics_types_struct` at `0xF1D0E8`.
  **Stationary turrets and vehicle-mounted turret child tags author all 10
  blocks empty** (see the per-vehicle table); what the retail physics-type
  resolver returns for an all-empty struct is a C1 runtime probe question and
  must not be assumed.
- **`unit_seat_flags`** is a 31-bit list (guerilla.exe name array at file
  offset `0x1477EF0`, count 31). The bits this feature relies on:
  `2=driver`, `3=gunner`, `4=third person camera`, `5=allows weapons`,
  `7=first person camera slaved to gun`, `11=boarding seat`,
  `16=invalid for player`, `18=gunner (player only)`.
  Cross-validated against every exported numeric flags value (e.g. warthog
  driver `0x40014` = bits {2,4,18}; machinegun turret `0x5500018` =
  bits {3,4,20,22,24,26}), matching the XML's exported flag names one-to-one.
- **`global_ai_seat_type_enum`** (count 6, names at file offset `0x1477FE8`):
  `0=NONE, 1=passenger, 2=gunner, 3=small cargo, 4=large cargo, 5=driver`.
- The vehicle-word char enums visible in tag XML are **not** physics types:
  `campaign_metagame_bucket_type_enum` (28 values; e.g. 14=warthog,
  24=banshee) and `player training vehicle type` (7 values) are metagame
  bookkeeping. Do not classify vehicles from them.

### Player-seat classification (proven from seat flags)

- **Driver seats** carry bit 2 `driver`. Every drivable vehicle has exactly
  one, always seat index 0 on its tag.
- **Vehicle-weapon control from the driver seat** is bit 3 `gunner` on the
  driver seat (Ghost, Banshee, Chopper, Wraith, Scorpion, Hornet, Shade —
  their guns are fired by the driver). Warthog/Mongoose drivers instead carry
  bit 18 `gunner (player only)` (the horn).
- **Turret gunner seats live on child vehicle tags** mounted on the parent
  vehicle (warthog/turrets/chaingun, warthog/turrets/gauss, wraith/turrets/
  anti_infantry, scorpion/turrets/anti_infantry, mauler/anti_infantry), and on
  the stationary `objects/weapons/turret/*` tags (machinegun_turret,
  plasma_cannon, missile_pod). All are seat 0, flags bit 3 `gunner`, ai seat
  `gunner(2)`, and all author a camera marker (`camera_gunner`, `camera`,
  `*_g_camera`).
- **Seats where the occupant uses their own equipped weapon are exactly the
  seats with bit 5 `allows weapons`.** Player-usable ones: warthog(+gauss,
  +troop) seat 1 `warthog_p` (`0x1070`), mongoose seat 1 `mongoose_p`
  (`0x2008270`), hornet seats 1–2 `hornet_p_l/r` (`0x30`), mauler seats 1–2
  `mauler_p_l/r` (`0x1270`). The scorpion's four rider seats and the troop
  hog's three bed seats also carry bit 5 but add bit 16 `invalid for player`
  (AI-only). No driver or gunner seat anywhere carries bit 5.
- **Every player seat in every vehicle carries bit 4 `third person camera`** —
  Halo 3 authors no first-person vehicle view anywhere; the FP camera must be
  built by the mod.
- **Authored per-seat camera markers are NOT universal.** They exist for
  turret gunner seats, several passenger seats (warthog `camera_rider`,
  mongoose `camera_passenger`, hornet/mauler/scorpion `*_camera`), and some
  driver seats (banshee `banshee_d_camera`, wraith `driver_camera`, shade
  `shade_d_camera`) — but the warthog, mongoose, ghost, hornet, scorpion, and
  chopper DRIVER seats author none. Where the engine's per-seat camera pivot
  actually sits for markerless seats is exactly what the C1 observer capture
  must measure; it must not be assumed.

### Per-vehicle table

Full table (all 22 exports, per-seat flags/markers/ranges) preserved as
the E1 artifacts alongside the raw XML exports. Summary of player-relevant
seats:

| Vehicle (tag) | Physics type | Player seats (index: label, role) |
| --- | --- | --- |
| warthog / warthog_gauss / warthog_troop | human_jeep (1) | 0: warthog_d driver; 1: warthog_p passenger, own weapon. Chaingun/gauss gunner is seat 0 `warthog_g` on the mounted `turrets/chaingun|gauss` child tag (camera_gunner, fp-slaved-to-gun bit 7). Troop-hog bed seats are AI-only |
| mongoose | human_jeep (1) | 0: mongoose_d driver; 1: mongoose_p passenger, own weapon (camera_passenger) |
| ghost | alien_scout (3) | 0: ghost_d driver (+gunner bit) |
| scorpion | human_tank (0) | 0: scorpion_d driver (+gunner bit); riders AI-only; scorpion secondary turret = child tag seat 0 `scorpion_g` |
| wraith | alien_scout (3) | 0: wraith_d driver (+gunner bit, driver_camera); wraith turret = child tag seat 0 `wraith_g` |
| banshee | alien_fighter (4) | 0: banshee_d driver (+gunner bit, banshee_d_camera) |
| hornet | vtol (7) | 0: hornet_d driver (+gunner bit); 1–2: hornet_p_l/r passengers, own weapon |
| brute_chopper | chopper (8) | 0: chopper_d driver (+gunner bit) |
| mauler (Prowler) | alien_scout (3) | 0: mauler_d driver; 1–2: mauler_p_l/r passengers, own weapon; mounted turret = child tag seat 0 `mauler_g` |
| shade | none authored | 0: shade_d driver (+gunner bit, shade_d_camera) |
| machinegun_turret / plasma_cannon / missile_pod (`objects/weapons/turret/*`) | none authored | 0: turret_g gunner (camera marker `camera`) |

Notes that bind later candidates:
- A warthog gunner's **parent unit is the chaingun child vehicle**, not the
  warthog — runtime seat identity is (parent tag, seat index), and turret
  child tags have no physics type of their own.
- The Wraith's physics type is `alien_scout (3)`, not a tank type — physics
  type alone cannot separate "ground vs heavy" classes; it is only needed for
  air (`human_plane 2 / alien_fighter 4 / vtol 7`) vs turret (5) vs ground
  discrimination, which it does support.

## E2 — Editing-kit binary chain (the provenance anchor)

Kit semantics were established before the retail homologs were accepted,
mirroring the HREK→retail order of the Reach method. All claims independently
re-derived by an adversarial second pass — 11/11 CONFIRMED. Kit binaries:
`halo3_tag_test.exe` SHA-256
`59A78F2C96034D7CEB5D710505B2B36813AA141FC81A083E3F952973DBCE4602` (primary)
and `halo3_tag_play.exe` SHA-256
`0830110923897F72853585DB3970D367F85A01AECC7F79BE430A864AEDBE6896`
(cross-check); both image base `0x140000000`. The kit carries its own source
paths (`c:\mcc\release\h3\source\game\player_mapping.cpp` at tag_test file
`0x10588C8`).

- **`unit_in_vehicle` chain (tag_test / tag_play):** single literal →
  descriptor `0x11CA2D0` / `0x11597F0` (0x50 bytes, hash-pinned; return type
  5 = boolean, one parameter of hs type `0x42`) → evaluator `0x7B7810` /
  `0x672380` (0x3B) → native `0xAE5110` (0x9C) / `0xAEE630` (0xED, datum_get
  inlined). Doc string: "returns true if the given unit is seated on a parent
  unit". The verifier located the hs-type name table (tag_test .data RVA
  `0x169EF40`): `0x42` = `unit` by name, `5` = `boolean` — the parameter typing
  is name-proven, not correlated.
- **Kit native semantics** (each from disassembly, corroborated by named
  asserts): NONE → false; object resolve via `object_get_and_verify_type`
  (assert-named); object data pointer at entry `+0x10`; **seat word at unit
  data `+0x24E`** (NONE = unseated); **parent index at unit data `+0x10`**
  (independently confirmed by the kit's `object_get_ultimate_parent`, which
  walks `[data+0x10]` gated on flags bit 26); parent must resolve to
  object-kind vehicle (entry byte `+3` == 1); vehicle definition fetched by
  `'vehi'` fourcc; **active physics type = first non-empty of 10 tag blocks
  (0xC bytes each) at definition `+0x2C0`**; return `type != 5`.
- **Turret = 5 by name:** guerilla.exe's `vehicle_physics_types_struct` field
  table lists `type-human_tank(0)` … `type-turret(5)` … `type-guardian(9)` in
  exactly the 10-entry order the scan loop walks. So the kit's
  `unit_in_vehicle` returns FALSE for a unit whose resolved parent authors
  turret physics — and is otherwise generic across every vehicle type
  including aircraft.
- **`player_mapping_get_unit_by_output_user`:** no standalone literal exists;
  identified by the kit's own assert expressions
  (`player_mapping_get_unit_by_output_user(output_user_index)==player->unit_index`,
  player_mapping.cpp:573 `VALID_INDEX(user_index, k_number_of_users)`).
  Accessor at tag_test `0x3EA720` / tag_play `0x3CEAF0` (0x6E, hash-pinned):
  NONE → NONE; player-mapping globals = qword at kit TLS slot `+0x18E0`;
  **returns unit datum dword at `pm + 0xC8 + output_user*4`** — the kit array
  offset, derived independently of Reach's numerically-equal value.
- Bonus sibling recorded: `unit_in_vehicle_type_mask` (descriptor/evaluator/
  native hash-pinned) compares the resolved ultimate parent's type **by
  equality**, not as a bitmask, despite its help text.

## E3 — Retail homologs in the pinned halo3.dll

All claims in this section were independently re-derived by an adversarial
second pass (fresh scripts, pefile + capstone; none of the original tooling
re-used) — 17/17 CONFIRMED, zero refutations. Method: the accepted
name-string → hs descriptor → evaluator → native chain
(REACH-SIGNATURE-EVIDENCE.md §2242), all offsets read out of the retail
bodies themselves.

### The unit_in_vehicle chain

| Identity | RVA | Body identity |
| --- | ---: | --- |
| `unit_in_vehicle` literal | `0x7F16D0` | .rdata; only other prefix match is `unit_in_vehicle_type_mask` at `0x7F1610` |
| hs descriptor | `0x7EB1F8` | 0x50 bytes, SHA-256 `37E5875F54B774C8F8324E6854D76DB4371696DB2C48D117F7AE528C723B9DE4`; +0x20 → literal, +0x38 → evaluator; stride 0x50 corroborated by the sibling record at `0x7EB3D8` |
| hs evaluator | `0x1E5AF8` | 0x33 bytes, SHA-256 `CFF4BD4D28415CAE2D15AF187387419C39F7862DA41EA1DD265B724B9C120655`; its call at `0x1E5B11` resolves to the native |
| Native predicate | `0x3A36F4` | 0xAA bytes (.pdata-proven), SHA-256 `F61C1963D371346DD1011132E628237F4191120DF3D9E0BE03129139C26BB83C` |
| `object_get_ultimate_parent` | `0x346AFC` | 0x4D bytes (leaf), SHA-256 `FC938779C0FFF9A5C92D72C2D3DCED77E4CC87AB8610C6AF8E7892CC7D0FC07A` |
| `vehicle_definition_get_type` | `0x396D84` | 0x4D bytes (leaf), SHA-256 `DBB2AA229496EFEC34AD546D02612058AD6DDC489C2651F03BAE5211B1279262` |

### MCC offsets, each from a named instruction in those bodies

- Engine TLS-index dword at RVA `0xA39F9C` — decoded identically by the
  native (`0x3A370E`), the ultimate-parent walker (`0x346B04`), the
  player-mapping getter (`0xEE493`), and the player-mapping initializer
  (`0xE1B3D`); the verifier counted 6413 rip-relative references to it across
  `.text`. This is the same global the mod already resolves as
  `g_engineTlsIndex` (game.cpp:7893-7898).
- Object table: engine TLS slot `+0x38` → table; entry array at table
  `+0x48`; **entry stride 0x18**; object data pointer at entry `+0x10`;
  cached object-kind byte at entry `+3` (1 = vehicle); index = handle low 16
  bits (`movzx ecx,cx` at `0x3A3723`) — **no datum-salt verification** in
  this native; the caller owns handle validity.
- **Parent object index = dword at object data `+0x10`** (`0x3A3745`,
  `0x3A3762`); the ultimate-parent walk is gated on bit 26 of the flags dword
  at object data `+0x4` (`0x346B34`).
- **Seat index = word at unit object data `+0x24E`**, `NONE` (0xFFFF) means
  unseated (`cmp word ptr [rdi+0x24E], r9w` at `0x3A373B`, bytes
  `66 44 39 8F 4E 02 00 00`).
- Native decision: seat word must be != NONE; the ultimate parent is used
  when it is object-kind vehicle, else the direct parent (`cmovne` at
  `0x3A3762`); that object must be kind 1; its definition index (word at
  object data `+0x0`) feeds `vehicle_definition_get_type`, and the predicate
  returns true iff the result `!= 5`.
- `vehicle_definition_get_type` (`0x396D84`): resolves the loaded vehicle
  definition via the tag-instance table (qword ptr RVA `0xA49018`, entry
  `idx*8+4` dword) and tag data base (qword ptr RVA `0x1FCF4C8`), then scans
  **10 tag-block records of 0xC bytes at definition `+0x2C0`** and returns
  the index of the first non-empty block — **or 0xB when all 10 are empty**
  (verifier addendum; this is the stationary-turret/shade case from E1).
  Value 5 = turret is proven by name tables in E2/E4, resolving E3's declared
  caveat by cross-stream corroboration.

### The player-mapping unit accessor

- `player mapping globals` literal at `0x76BA70` has exactly one code xref:
  the initializer `0xE1B28` (0x147 bytes, SHA-256
  `7A81795E43A06A57D9BB27C4926A177413D0E637CB417F11F25EFB4D89F260B1`), which
  allocates the 0xE8-byte struct whose pointer lands at engine TLS slot
  `+0x110`.
- Retail `player_mapping_get_unit_by_output_user` homolog at RVA `0xEE48C`
  (0x2F bytes, leaf, SHA-256
  `BF58464B1D03D967DA3B478641CF72CF69569F98D28A61CFC1287C2B0548A573`):
  NONE-guard, TLS decode, `pm = [slot+0x110]`, returns dword
  `[pm + user*4 + 0xC8]`. **The +0xC8 was read from this body** (the
  verifier's whole-.text scan found exactly one non-setter `*4+0xC8` read —
  this one), anchored by 22 verified call sites and the setter homolog
  `0xEE684` writing the same array.
- **The retail getter has NO 0..3 bounds check** — only a NONE guard with
  sign-extension. The runtime sampler must bound the output-user index itself
  (we only ever pass 0).

### Production AOB signatures (whole-file match counts verified twice)

| Target | AOB | Matches |
| --- | --- | ---: |
| Native `unit_in_vehicle` (`0x3A36F4`) | `48 89 5C 24 08 57 48 83 EC 20 45 32 DB 41 83 C9 FF 41 3B C9 0F 84 82 00 00 00 8B 15 ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 41 B8 38 00 00 00 0F B7 C9 48 8B 04 D0 4A 8B 14 00 48 8D 04 49` | 1 |
| Player-unit getter (`0xEE48C`) | `83 C8 FF 3B C8 74 27 8B 15 ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 41 B8 10 01 00 00 48 63 C9 48 8B 04 D0 4A 8B 04 00 8B 84 88 C8 00 00 00 C3` | 1 |
| hs evaluator (`0x1E5AF8`) | `40 53 48 83 EC 20 83 64 24 48 00 8B DA E8 ?? ?? ?? ?? 48 85 C0 74 16 8B 08 E8 DE DB 1B 00 88 44 24 48 8B CB 8B 54 24 48 E8 ?? ?? ?? ?? 48 83 C4 20 5B C3` | 1 |

Ambiguity history worth keeping: with the native-call rel32 also wildcarded,
the evaluator pattern matches 10 identical hs thunks; pinning the rel32
(`E8 DE DB 1B 00`) reduces 10 → 1. A future MCC update shifting that rel32
correctly re-ambiguates the signature and fails open.

## E4 — Observer camera decode and physics-type chain

Adversarially re-derived: observer claims A1–A4 and type claims B1–B5
CONFIRMED byte-exact. The B6 negative's original scan was shown incomplete by
the verifier, who then re-established the same conclusion on its own
exhaustive enumeration (all 383 indirect jumps in .text; 24 bounded jump
tables; none vehicle-related) — recorded here on that stronger evidence.

### Observer camera (the seat-camera anchor source)

- `observer_apply_camera_effect` — the function the mod already matches with
  `kObserverCameraEffectSig` — is at RVA `0x17DF44` (unique match; .pdata
  body 0x27C, SHA-256
  `64E5D16DB65F58FB8BC88F6DA848981732491E811B0745386D317DB081116BE3`).
- **The observer array has NO static RVA.** The array pointer is engine TLS
  block member `+0x578`; records are `0x3D0` bytes (imul at `0x17DFAE`);
  slot 0's record is the loaded base pointer itself. Any code reaching the
  records must run on a thread with live engine TLS.
- Decode recipe from the already-matched signature: the `8B 15 ?? ?? ?? ??`
  at match+42 carries disp32 at match+44, instruction ends match+48, so the
  engine TLS-index dword RVA = match+48+disp32 (this build: `0xA39F9C` — the
  same global three independent functions and the mod's existing
  `g_engineTlsIndex` resolve). Verification bytes: match+86 =
  `BF 78 05 00 00` (member 0x578), match+106 = `48 69 DE D0 03 00 00`
  (stride 0x3D0).
- Record fields written back by the effect stage: **position at record
  `+0x11C`, forward at `+0x144`, up at `+0x150`**. The C1 probe captures the
  float window `+0x100..+0x1A0` so the focus_position / focus_distance
  fields are measured in-game rather than assumed from Halo Online layouts.

### Vehicle physics-type chain (Turret/air/ground discrimination)

- Retail's own `_vehicle_type_*` name table at RVA `0x791EB0` (24-byte
  records) lists the 10 types in order with **turret = 5**; the kit assert
  `!TEST_BIT(_vehicle_mask_flying, vehicle_get_type(...))` compiles to
  `test al, 0x94` — **flying = {human_plane 2, alien_fighter 4, vtol 7}**,
  proven from the engine's own mask.
- `vehicle_definition_get_type` at RVA `0x396D84` (unique AOB, below):
  input cx = vehi tag definition index; resolves the loaded definition via
  the tag-instance table (ptr RVA `0xA49018`) and tag data base (ptr RVA
  `0x1FCF4C8`); scans the 10 physics blocks at definition `+0x2C0` (stride
  0xC) and returns the first non-empty index 0..9 — **or 0xB when a tag
  authors none** (stationary turrets, shade, mounted turret child tags per
  E1). 61 call sites in retail.
  AOB (2 rip disps wildcarded, exactly 1 match, decode: disp32@+3 ends +7 →
  instance table; disp32@+25 ends +29 → tag base):
  `48 8B 05 ?? ?? ?? ?? 0F B7 C9 8B 54 C8 04 85 D2 75 04 33 D2 EB 0B 48 8B 05 ?? ?? ?? ?? 48 8D 14 90 33 C0 45 33 C0 48 81 C2 C0 02 00 00 B9 0B 00 00 00 83 3A 00`
- `object_get_ultimate_parent` at RVA `0x346AFC` also carries its own
  verified-unique 77-byte AOB (recorded in the E4 artifacts) should a later
  candidate need the ultimate-parent walk directly.
- **Clean negative (verifier-established):** retail has no ~10-case
  jump-table vehicle-type dispatcher; per-type behavior selection is
  compiled away. The accessor chain above is the discriminator. Caveat: a
  dispatcher switching on a cached type byte far from any accessor call
  cannot be excluded by proximity alone, but none of the 24 enumerated jump
  tables shows vehicle context.

## Runtime probe results (C1 headset session, 2026-07-31)

Candidate `85575e5` / DLL `847DDAFF…64EF1969`, Steam edition, Forge (Gauss
Warthog driver→gunner→passenger, Ghost, chaingun Warthog driver→gunner→
passenger) then Tsavo Highway (Warthog all three seats, stationary machine-gun
turret). Both launches resolved all three identities unique at exactly the
expected RVAs. Zero faults, zero StockFallback. Perf baseline while driving:
renderWindow p95 4.4–6.2 ms, stalls=0 (the C2+ +0.5 ms gate reference).

1. **Turret discriminator — measured, and it corrects E1's offline
   expectation.** The live `vehicle_definition_get_type` on the DIRECT parent
   returned **5 (turret)** for the Gauss turret, the chaingun turret, AND the
   stationary machine-gun turret — not the 0xB the source-tag XML parse
   predicted (loaded cache tags evidently author the turret physics block
   even where the source XML export showed none; runtime measurement wins).
   `nativeInVehicle` separates the variants exactly as the E3 disassembly
   predicts: **1 for mounted turret seats** (ultimate parent = the carrier
   vehicle) and **0 for the stationary turret** (ultimate parent is the
   turret itself, type 5). So: `directType==5` ⇒ TurretGunner;
   `native==0 && directType==5` ⇒ stationary turret. Shade remains unmeasured.
2. **Seat indices match E1 exactly:** driver = seat 0 on the vehicle
   (directType 1 warthog / 3 ghost), gunner = seat 0 on the turret child tag
   (directType 5), warthog passenger = seat 1 (directType 1).
3. **Observer focus field found: `focus_position` = record `+0x18C`.**
   On foot it equals the camera position exactly (best candidate `+0x18C`,
   d=0.000, residual=0.0000 — the predicted on-foot signature). In every
   vehicle sample it stays the best triplet. The distance scalar
   candidates cluster at `+0x15C` (a recurring constant 2.094 while driving
   the Warthog — plausibly the authored chase distance, sitting immediately
   after up at +0x150), but the residual while driving is 0.7–1.3 wu because
   the chase camera's direction LAGS the focus during turns — confirming the
   risk-table prediction that anchor formula (ii) `pos + fwd·d` is skewed by
   camera swing. **C3 must use formula (i): the absolute focus triplet at
   `+0x18C`.** `+0x15C` = focus_distance is probable but C3 does not need it.
4. Forge monitor mode (user directive: leave it alone) never reported a
   vehicle state — only on-foot with a churning unit handle (the engine
   alternates the player unit while in monitor). No contamination of vehicle
   data; C2's mode publication stays inert there by construction.

## Vehicle world transform (C4 drive session, 2026-07-31, cand 8dc7c06)

Measured from the parent-object-data window across 21 seat sessions
(chopper, warthogs, banshee, hornet all three seats, ghost-class × several,
scorpion, turrets mounted + stationary):

- **World position = object data `+0x1C`** (repeated exactly at `+0x3C`;
  `+0x2C` holds a second, nearly identical center that diverges from `+0x1C`
  by up to ~0.1 wu ∝ speed — consistent with a per-tick vs interpolated pair;
  `+0x1C` ships first, revisit only if fast-vehicle judder is reported).
  **SUPERSEDED by the C5 correction below: `+0x1C` is the bounding-sphere
  center (animated); the authoritative rigid origin is `+0x50`.**
- **Forward unit vector = `+0x5C`**: tracks velocity when flying forward
  (dot 0.92), flips sign in reverse (−1.00), reads exactly (1,0,0) on parked
  map-aligned turrets.
- **Up unit vector = `+0x68`**: (0,0,1) on flat ground, banks with the
  Banshee/Hornet and recovers on leveling. `+0x84` is a world-up/gravity
  reference that goes non-rigid during maneuvers — NOT part of the basis.
- **Handedness: left = up × fwd** — proven by the warthog passenger seat
  resolving to negative Y (right side) in vehicle space, matching its
  authored seat.
- The observer `+0x15C` scalar is a GLOBAL camera constant (2.094 in every
  measured seat of every type), not a per-vehicle chase distance — rejected
  as a vehicle fingerprint. Observer window `+0x100..+0x118` contains
  uninitialized garbage/NaN; meaningful fields start at `+0x11C`.
- Per-seat focus-in-vehicle-space fingerprints extracted for every session
  (analysis scripts + values preserved in the session scratchpad); labeling
  of same-type cousins (ghost/wraith/mauler, mounted turret variants,
  mongoose) awaits the user naming which they drove.

## C5 — authored seat points (headset-pending)

The C3 engine-focus anchor was headset-REJECTED ("inside and under" — most
driver seats author no camera marker, so the focus sits at the chase orbit
pivot in the hull). Replaced by USER-AUTHORED per-seat eye points, authored
in Blender against the exported vehicle meshes (official H3EK tool exports;
kit under out/vehicle-camera-kit) and carried at runtime by the measured
transform above: `world = pos + R · point` with R = [fwd, up×fwd, up].
Seats keyed by (directType, seatIndex, nativeInVehicle); certain identities
baked (chopper, banshee, hornet ×3, scorpion, human_jeep driver+passenger,
mounted turret, stationary turret); unresolved seats stay on the stock chase
view and log `seatCam=none(stock)`.

## C5 headset result and the anchor correction (2026-07-31 08:26 Steam log)

C5 (cand e8581c8 / DLL 55F23CC4…) was headset-REJECTED: seats moved but did
not match the authored points; every camera sat too high. The C5 session log
(source-stamp verified) contains paired object-window and observer dumps for
chopper ×2, warthog driver ×2, warthog passenger, banshee, mongoose, ghost
(stock-cam control) and mounted-turret sessions, which root-caused it:

- **`+0x50` is the object origin** — the rigid tag/authoring-frame position.
  For every session, `R^T · (pos1C − pos50)` (with R from `+0x5C/+0x68`) is
  constant per vehicle and reproduces the render model's **node-0 default
  translation** exactly:
  warthog `(+0.025, 0.000, −0.420)` measured vs hull node `(−0.025, 0, 0.420)`
  authored; mongoose `(+0.051, −0.001, −0.300)` vs hull `(−0.050, 0, 0.300)`;
  banshee `(−0.014, 0.000, −0.652)` vs body `(0.013, 0, 0.652)` — all
  millimetre-exact. This simultaneously proves `+0x5C/+0x68` are the rigid
  tag-frame basis.
- **Name-level confirmation (ManagedDonkey `s_object_data`, offsets
  re-verified by member-size arithmetic):** `+0x1C bounding_sphere_center` /
  `+0x28 bounding_sphere_radius` (the measured 1.3–1.4 scalar) /
  `+0x2C attached_bounds_center` + radius (the speed-divergent twin — it
  includes attached child objects) / `+0x3C attached_bounds_sphere_center` /
  `+0x50 position` ("the object's authoritative world-space origin", written
  by object_new / object_set_position_internal) / `+0x5C forward` /
  `+0x68 up` / `+0x74 transitional_velocity` / `+0x80 angular_velocity`.
- **The C5 bug:** `+0x1C` (bounding center) rides the ANIMATED model — the
  brute chopper measured `(−0.122, −0.020, −0.389)` against a hull-node
  default of `(0.098, 0, 0.471)`, ~0.09 wu of suspension animation — and
  sits above the tag origin by each vehicle's node-0 height. Anchoring
  authored (tag-frame) points at it misplaced every camera upward
  (+0.30 wu mongoose … +0.65 wu banshee) — the reported "so high up".
  C4 picked `+0x1C` because the bounding center tracks the chase focus
  marginally better; SUPERSEDED. **Fix: anchor = `+0x50 position`.**
- **Bonus discriminator:** the same measured offset identified one "warthog"
  session as a MONGOOSE (offset matched the mongoose hull node, not the
  warthog's). `quantize(R^T · (pos1C − pos50))` vs the render-model node-0
  table separates every same-physics-type big-vehicle cousin (ghost / wraith
  / mauler node-0 all >0.4 wu apart) with no new hooks. It does NOT separate
  turret child tags (their node 0 sits at the tag origin) — turret cousins
  need definition-resident data, and note the E1 caveat that shipped cache
  tags author turret physics blocks the H3EK source XML lacks, so those
  values must be measured from the loaded tag, not taken from source exports.
- The Blender kit v1 display was MIRRORED (user report: driver seat on the
  right): the .x export lateral axis was mapped with a negation. The authored
  numbers themselves were unaffected (marker-derived Y; X/Z judged on a
  laterally-symmetric view), but kit v2 re-bakes the meshes in tag space.

## E5 — Vehicle identity discriminators (offline-verified, runtime probe pending)

Workflow-verified (ManagedDonkey struct layouts + H3EK tag values + our own
E2/E3/E4 retail disassembly; full report in the 2026-07-31 session records).
Chain: definition `+0x2C0` is an array of TEN 0xC-byte tag_block records
{count @+0, address @+4, unused @+8} (matches the proven type-getter body:
stride 0xC, `cmp dword [rdx],0`, 10 iterations). Element data =
`tagDataBase + address*4` (address 0 = null); tagDataBase = the qword at
`halo3.dll+0x1FCF4C8`, the same global the proven E4 AOB decodes.

- **Type 1 (human_jeep), warthog vs mongoose:** element (s_vehicle_human_
  jeep_definition, 0x40) field `engine.engine_moment` @ element+0x14 —
  warthog family 2000 (chain/gauss/troop byte-identical) vs mongoose 650.
  Corroborators: turn_rate 540/360 @+0x10, wheel_circumferance 1.25/0.95
  @+0x38, gears count 4/3 @+0x1C.
- **Type 3 (alien_scout):** element (0x70) has a literal
  `specific_type` int8 @ element+0x28: **1=ghost, 3=wraith, 4=hover craft
  (mauler/Prowler)** (enum proven from guerilla.exe + Donkey). Float
  corroborators: speed.acceleration @+0x10 = 6 / 2.25 / 10. wraith vs
  wraith_anti_air are byte-identical (not separable — same seat geometry,
  acceptable).
- **Type 5 (turret) element is 4 pad bytes — useless.** Discriminate via
  definition-TAIL fields (runtime offset = 0x2BC + Donkey _vehicle_definition
  offset): shade uniquely authors minimum/maximum_flipping_angular_velocity
  30/90 @ def+0x39C/+0x3A0 (all turret_g child tags author 0/0);
  hog chaingun blur_speed 0.1 vs gauss 0.0 @ def+0x3B4; mounted turrets also
  resolve via the ultimate-parent walk (parent object data +0x0 = definition
  index word) + the parent's own type-0/1/3 discriminator. The three
  STATIONARY turrets (machinegun/plasma_cannon/missile_pod) are NOT
  offline-separable (all-zero tails in source XML) — C1 proved loaded cache
  tags can author fields the XML lacks, so C7's log probe must dump the
  tails in-game before any of the three is registered individually.
- **Big-vehicle cross-check (already runtime-proven):** R^T·(bounding
  center − position) equals the render-model node-0 default per vehicle
  (C5 analysis) — warthog (−0.025,0,0.42) / mongoose (−0.05,0,0.30) /
  ghost (0.393,0,0.171) / wraith (−0.527,0.004,0.600) / mauler
  (−0.356,0,0.395) / chopper (0.098,0,0.471, suspension-noisy ±0.09) /
  banshee (0.013,0,0.652) — all same-type cousins >0.12 wu apart.

Nothing keys a camera off E5 until a C7 log probe confirms the read values
in-process on known vehicles (fail-open to stock + loud log on mismatch).

## C7 — attached-object frames and the turret fix (2026-07-31 09:08 log)

C6 (cand 28c7b2d) headset result: every ROOT-vehicle driver/passenger seat
landed on its authored point (chopper, warthogs ×3 sessions, banshee — the
anchor correction is CONFIRMED in-headset), but mounted-turret cameras
appeared "deep in the sky" and the mongoose kept the (deliberately shared)
warthog point.

- **Root cause (from the C6 log's turret windows):** an ATTACHED child
  object stores `position/forward/up` (+0x50/+0x5C/+0x68) PARENT-RELATIVE.
  Both hog-turret sessions read position `(-0.500, 0.000, 0.019)`, forward
  `(1,0,0)`, up `(0,0,1)` — the mount offset in the hog's frame — while the
  turret's own `bounding_sphere_center` (+0x1C) carried real world
  coordinates `(9.94, 12.06, -21.44)`. C6 therefore composed the authored
  point against a frame sitting at the MAP origin. The turret's object
  frame is mount-fixed: it did not follow the gun/observer aim in either
  session (gun yaw is bone animation, not an object-frame rotation).
- **Fix:** compose the anchor frame through the parent chain —
  `world = carrier ∘ local` (local coordinates in the carrier's fwd/left/up
  axes), one attachment level deep (deeper chains publish no frame, stock +
  `frame=too-deep` log). Pure logic + tests in halo3_vehicle_logic.h
  (`Halo3ComposeFrame`).
- **Sanity gate (new, all seats):** the anchor frame origin must sit within
  the seated parent's own bounding sphere (+0x1C center, +0x28 radius,
  +2.0 wu margin) — the C6 failure class (wrong frame ⇒ tens of wu off)
  can now only ever produce the stock chase view plus a `frame=INSANE` log,
  never a sky camera.
- The probe line now reports `frame=direct|composed|INSANE|too-deep` and
  the vehicle's definition index.
- **Identity probe (log-only):** on every seat transition the worker
  resolves the LOADED definition via the two globals decoded rip-relatively
  from the matched type-accessor body (instance table / tag data base, E4
  layout) and logs the §E5 discriminator fields (`H3 defProbe:` — physics
  block counts, jeep engine_moment/turn_rate, scout specific_type/
  acceleration, flip/blur tail floats). Nothing keys a camera off these
  values yet; the C7 log must first confirm them against E5's expected
  values on known vehicles (E5 caveat: the tag-block `address*4` scaling
  is proven for definitions generally but not exercised on the physics
  records specifically — hence measure-before-use).
- Mongoose was again identified by its bounding-center offset (`(+0.051,
  −0.001, −0.300)` = mongoose hull node) and stays on the shared hog point
  until the identity build (C8) keys its own authored point.

## C9 — why the seat camera does not turn with the vehicle (code-read, 2026-07-31)

User report: *"the camera does not rotate with the car"*, together with the
observation that the camera and the motion controls are one problem because
"in vanilla halo you use the camera stick to drive around". Both halves are
confirmed by reading the shipped code — no runtime probe needed, and nothing
here is inferred from behaviour.

**Why the view stays pinned to the map.** `ApplyHeadLook` builds the camera
yaw as

```
gy = g_gameYawRef + yawSign * WrapPi(headYaw - g_headYawRef)
```

`g_gameYawRef` is rebased only by a manual recenter or a cinematic shot
boundary. The C5/C6/C7 seat camera substitutes **position** onto the authored
point and deliberately leaves `fwd`/`up` HMD-owned, so the hull rotates
underneath a view whose reference is the map. After a 90° turn "straight
ahead" points out of the side of the vehicle. This is a design gap in the FP
camera, not a bad seat point, and no camera-position change can fix it.

**Why it is the same knob as steering.** `Game_ComputeAimStick` maps the
controller ray into game space through *the same reference*:

```
desiredYaw = g_gameYawRef + yawSign * WrapPi(controllerYaw - g_headYawRef)
errYaw     = desiredYaw - aimYaw            // -> injected right stick
```

so three things are welded to one angle: the view, the hand-aim ray, and —
because Halo 3 vehicles are look-steered and that injected stick *is* the
steering input — the vehicle's heading.

Consequences, and they decide the candidate shape:

- View and aim **must** share the reference. Rotating only the view slides
  the reticle off what the player is looking at by exactly the hull angle.
- Once the reference follows the hull, a driver's aim error becomes
  `handOffset + const`: `aimYaw` for a driver *is* the hull heading, so it
  cancels the hull term and a fixed hand offset produces a fixed stick
  deflection — a constant turn **rate** rather than a heading. Hull-follow
  therefore cannot ship before a motion-control steering author; it would
  turn driving into "hold your hand off-centre to keep turning".
- In a gunner or weapon-capable passenger seat the same change is simply
  correct and needs nothing else: `aimYaw` there is the turret/weapon facing,
  independent of the hull, so holding the hand steady relative to the seat
  swings the gun with the vehicle exactly as a real occupant's arm would.

**Mechanism.** `g_halo3VehicleTransform` already publishes the seated
parent's measured world `fwd` every camera frame (C5), so the hull heading is
`atan2f(fwd[1], fwd[0])` — no new engine read, no new signature. The pure
logic is `Halo3UpdateSeatYaw` (`src/common/halo3_vehicle_logic.h`, tested):
it references the hull heading on the first seated frame, accumulates the
rotation since, and returns the yaw to add to the shared reference, scaled by
a follow weight (0 = today's world-locked view, 1 = locked to the hull).
Accumulate-then-wrap, not wrap-then-scale: a partial weight applied to a
wrapped difference snaps 2π·weight every time the hull crosses the wrap.

**Not attempted:** hull pitch and roll are deliberately excluded. The horizon
stays HMD-stable (the accepted comfort rule), so a flipping vehicle never
rolls the player's world.

Status: logic and tests landed; **nothing calls it yet**. It ships with the
motion-control steering author, after the authored-point camera bake (C8) has
a headset confirmation.

## C8 — vehicle identity confirmed in process (C7 session log, 2026-07-31 09:32 Steam)

The C7 candidate (`ebd0e14`) was driven on Steam under VirtualDesktopXR. The
session is mechanically clean — `renderWindow p95 5.33 ms` (inside the C1
baseline 4.4–6.2), `stalls=0`, no `frame=INSANE`, no `too-deep`, no sampler
fault, and every seat resolved `seatCam=authored`. More importantly the
log-only identity probe answered the open §E5 question.

**§E5 is CONFIRMED in process.** The physics-block count array is indexed
exactly by physics type, and the `address*4` scaling that was unexercised on
the physics records specifically now reads real values:

| def | directType | counts | engine_moment | turn_rate | blur | vehicle |
|---|---|---|---|---|---|---|
| 0517 | 1 | `0100000000` | **650.0** | 6.3 | 2.40 | mongoose |
| 05F9 | 1 | `0100000000` | **2000.0** | 9.4 | 0.55 | warthog |
| 0697 | 5 | `0000010000` | – | – | 0.10 | chaingun (mounted, `frame=composed`) |
| 08C7 | 5 | `0000010000` | – | – | 0.00 | standalone turret (`native=0`) |
| 0BDB | 8 | `0000000010` | – | – | 0.00 | brute chopper |
| 01C2 | 0 | `1000000000` | – | – | 0.55 | scorpion |

`engine_moment` returned exactly the two values E5 predicted offline (2000
warthog / 650 mongoose). `turn_rate` resolves as **radians** — 9.4 rad = 540°
and 6.3 rad = 360°, which is E5's degree pair in the field's own units, so
that discriminator is confirmed too rather than contradicted. The measure-
before-use caveat recorded in E5 is therefore discharged: C8 may key cameras
off these fields.

**The definition INDEX must never be a key.** `def=05F9` identifies the
warthog *in this map's tag table*; Halo 3 cache files carry per-map tag
indices, so the same vehicle can hold a different index on another level.
C8 keys on definition CONTENT (authored per tag, stable everywhere), never on
the index. The index remains in the log purely for correlation.

### Identity model

`Halo3ResolveVehicleId` maps definition fields to a vehicle:

- type 0 → Scorpion, 4 → Banshee, 7 → Hornet, 8 → Chopper (each unique).
- type 1 (human_jeep) → `engine_moment` 2000 ± 200 = Warthog, 650 ± 200 =
  Mongoose. The two constants are 1350 apart, an order of magnitude beyond
  the tolerance.
- type 3 (alien_scout) → `specific_type` 1 = Ghost, 3 = Wraith, 4 = Mauler
  (Prowler). Mechanism confirmed above; these three values are still E5
  offline predictions, and a value matching none of them yields Unknown.
- type 5 (turret) is never a root identity.
- Anything else, any failed read, any unmatched value → **Unknown**, which
  matches no seat point, so the stock chase camera stands. A mislabel puts
  the player's head inside another vehicle's geometry; a missing label only
  costs the effect. The table is built so the first is impossible.

Resolution runs once per distinct definition (an 8-slot direct-mapped cache
keyed by definition index *and* runtime generation), not per frame.

### Mounted turret gunners are keyed and framed by their CARRIER

`native=1` with `directType=5` means the direct parent is the gun but a
carrier sits under it (C7 log: chaingun `directType=5 native=1`, standalone
turret `directType=5 native=0`). The sampler already captured the carrier's
data window for the C7 composition; C8 additionally reads the carrier's own
definition index and physics type from it, so the gunner seat is identified
as "the gunner of a warthog / scorpion / wraith / prowler".

The anchor for those seats now takes the **carrier's frame verbatim** instead
of composing carrier ∘ local. C7's composition is ~0.42 wu (1.3 m) low
because an attached child stores its transform relative to the parent's
ATTACHMENT NODE rather than its object origin — the hog's `turret` marker's
raw node-local translation matched the measured child position exactly, while
the node-composed tag position did not. Authoring and anchoring in carrier
space never touches that node matrix. Nothing is lost: a mounted turret's
object frame is mount-fixed and does not follow gun aim. The bounding-sphere
sanity gate follows the frame's owner, so a carrier-frame anchor is checked
against the carrier's sphere.
