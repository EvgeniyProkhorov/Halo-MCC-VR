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

## C8 confirmed in the headset (2026-07-31, Steam, source 6b08269)

The user drove every root driver seat and reported the placements correct. The
session log (`6b082698a949cdb730c384865d70868c336f6fc0`, Steam edition) shows
`seatCam=authored frame=direct` on the warthog, mongoose, ghost, wraith,
prowler, chopper, banshee and hornet — eight distinct identities, including
both same-physics-type cousin groups that C6/C7 could not separate. No
`INSANE`, no `too-deep`, no faults.

Two things that sitting did **not** cover, so they remain unproven: every
sampled seat was `seat=0` (no passenger seat was ridden) and no line carries
`frame=carrier` (no mounted turret was manned). The C8 carrier fix is still
awaiting its first headset evidence.

## The bounding-centre fingerprint is a dead end — do not wire it

`Halo3ComputeFingerprint` / `Halo3FingerprintIsUnique` exist in
`halo3_vehicle_logic.h` with tests, and **nothing calls them**. Keep it that
way. An offline pass over the H3EK render_model and model tags for all 19
vehicle/turret tags, cross-checked against the raw object windows in three
existing session logs, established:

- The recovered value is `R^T · (+0x1C − +0x50)`, and for warthog, mongoose,
  banshee, wraith and scorpion it is the render_model node-0 default
  translation to <= 0.0006 wu. For **chopper, mauler and hornet it is not** —
  those match the `hlmt`'s authored bounding sphere centre instead (to 0.0009
  wu). The **ghost matches neither** (0.089 wu from the closest), consistently
  across two sessions and two object handles, so its value is obtainable only
  by measurement.
- It cannot separate the cousins it would exist to separate. warthog vs
  mongoose is 0.1226 wu apart; the shipped accept/separate pair (0.15 / 0.30)
  needs a minimum pair distance of >= 0.45 wu to be safe.
- `warthog_gauss` and `warthog_troop` share the warthog's model, and
  `wraith_anti_air` shares the wraith's, so those pairs are at distance 0.0 —
  a zero-information case no tolerance can fix.
- On a mounted turret the expression is not merely uninformative but
  undefined: an attached child stores `+0x50/+0x5C/+0x68` parent-relative
  while `+0x1C` is world, so all 15 measured turret samples produced 26-33 wu
  of garbage. (Safe by construction — it can only fail to match.)

The definition-content identity C8 ships is the correct mechanism and is
confirmed in process. One genuinely useful by-product: three of four measured
turret mount offsets (the child's own `+0x50`) match the carrier's authored
turret marker to 0.0005 wu, which is a far better turret discriminator than
the fingerprint if the Shade ever needs one.

## C9 — the view turns with the vehicle, and the driver stops steering by gaze

**Report (user, 2026-07-31, after the C8 sitting):** "the camera doesn't follow
the rotation of the vehicle", and "the mesh vibrates fighting against the
camera override".

### Why the view did not turn, from the shipped code

`ApplyHeadLook` builds view yaw as
`gy = g_gameYawRef + yawSign * WrapPi(headYaw - g_headYawRef)`, and
`g_gameYawRef` is rebased only by a manual recenter or a cinematic shot
boundary. C8's seat camera substitutes **position** and leaves facing entirely
HMD-owned in world space, so the hull turns underneath a map-pinned view.

### The fix is a rebase of the one shared reference, not a separate offset

`Halo3SeatYawDelta` returns the hull's rotation *step*, and
`Halo3ApplySeatYawFollow` folds it into `g_gameYawRef` on the camera thread
before anything consumes it that frame. View, hand-aim ray
(`Game_ComputeAimStick`), rendered hands and gun (`BuildTrackedGameBasis`,
`DesiredWristWorld`), crosshair and move stick are all already expressed
against that one reference, so they turn together and none can be forgotten;
publishing an offset would have to be added at each site and any miss would
slide by exactly the hull angle. It also leaves the player facing the hull's
final heading on foot when they climb out, with no snap.

Guards, all tested: a hull whose forward has no horizontal component (a Banshee
through the vertical) re-references instead of accumulating a degenerate
`atan2`; a step past 0.60 rad in one frame is a teleport/respawn/seat swap, not
steering — Halo 3's fastest authored `turn_rate` is 9.4 rad/s, which is 0.16 rad
across a whole 60 Hz tick — and is refused; weight 0 restores Alpha 0.3.1
exactly.

### Why steering had to change in the same candidate

`Game_ComputeAimStick` maps the controller through the **same** reference:
`desiredYaw = g_gameYawRef + yawSign * WrapPi(controllerYaw - g_headYawRef)`.
For a seat where the engine's aim IS the hull heading, following the hull makes
`errYaw = desiredYaw - aimYaw` independent of the hull entirely: the loop loses
its feedback and a fixed hand offset becomes a constant turn RATE. So the
follow cannot ship without replacing that author, and the two switch on one
condition (`Halo3SeatAuthorsSteering`, gated on the follow weight itself).

### Halo 3 has two steering families, and only one may be authored

- **Look-steered** — the right stick turns the vehicle and the seat's aim is
  the hull's own heading: warthog, mongoose, ghost, prowler, chopper, banshee,
  hornet.
- **Turret-aimed** — the hull turns from the left stick and the right stick
  aims a weapon that swings independently: **scorpion and wraith**. Their loop
  keeps real feedback under the follow, so it is left alone; taking their right
  stick would be taking their gun.

Every other seat (passengers, mounted gunners, walk-up turrets) aims something
independent of the frame being followed, so the closed loop stays there too and
on-foot shooting mechanics are unchanged — the user's standing requirement.

`Halo3SeatFollowsHull` additionally withholds the follow from a **walk-up
turret**: that seat is published in the turret's *own* object frame, which may
be the very thing its aim turns, and nothing authors its steering. A mounted
gunner is published in the CARRIER's frame, so it is unaffected.

### What authors a look-steered driver's yaw now

The virtual wheel when it has been taken (ground only), Halo's own turn stick
otherwise. `ApplyVrTurn` stops consuming the turn stick in those seats so one
flick cannot both spin the view and swerve.

**The wheel is a double-click toggle, not a hold, and that is a control-mapping
requirement rather than a preference.** The right grip is `RB`, which is how the
player gets out of a vehicle (user, 2026-07-31). A hold-to-grip wheel therefore
had to choose between ejecting the driver the moment they took it and disabling
the dismount for as long as they held it. The shipped rule avoids both: the grip
buttons are withheld **only while both grips are down at once**, which is
precisely the wheel's own gesture, so a lone right grip always reaches the game
— wheel taken or not. Double-click both grips to take the wheel, double-click
again to let go; in between, nothing is squeezed, so a long drive costs no grip
strength. One haptic blip through the peak-holding `VR_SetGameHaptics` is the
only feedback a toggle can give.

Two consequences of the toggle that the hold model did not have: the pad must be
sampled **once** and shared between the wheel update and the button build (two
samples could straddle the 0.6 press threshold, and that frame is exactly the
one that fires the dismount), and hands brought closer than 0.15 m must read as
straight ahead rather than holding the last steer — hands dropped into a lap
would otherwise keep the vehicle turning with nobody driving.

**Pitch keeps the closed loop everywhere** — it
never passed through the yaw reference — so a Banshee still climbs and dives
from the hand, and aircraft take the plain stick for yaw until a two-hand yoke
lands.

`Game_MapMoveStick` no longer rotates the left stick head-relative in a
first-person vehicle seat. That rotation is the walk-where-you-look feature; in
a vehicle the stick is throttle, and on a scorpion or wraith it is the hull's
own steering, while `aimYaw` there is the turret.

### The mesh vibration is not yet root-caused — C9 ships a diagnostic, not a fix

Two candidates, and they are distinguishable from one line of log:

1. **Simulation staircase.** If the engine advances the object's `+0x50` only
   on its own tick while MCC renders faster and interpolates the *mesh*, a
   camera pinned to the raw value steps while the mesh slides.
2. **Suspension rock.** The anchor tracks the hull's pitch and roll (correct —
   your head really does rock) while the view is deliberately horizon-stable,
   so the mesh appears to rotate about the seat point.

`H3 seat motion:` counts, per 10 s of driving, how many sampled camera frames
saw the hull's position change at all and how many saw its up vector tilt, plus
the largest step of each. Near 100 % moved means the hull advances every frame
and (1) is refuted; well under that with a camera rate far above the move rate
confirms it. A high move percentage with a large tilt is (2). Nothing in this
diagnostic feeds behavior.

## C10 — the shaking is MEASURED, not theorised: a 60 Hz hull under a 120 fps mesh

The C9 diagnostic settled it in one drive (2026-07-31 12:41-12:47, Steam,
source `b25a414`). While driving:

```
H3 seat motion: 2396 sampled frames, hull moved on 544 (23%), tilted on 649
(27%); largest step 0.1238 wu, largest tilt 4.78 deg
M1 timing: camera transforms 239.9/sec ... HMD pose samples 119.8/sec
```

2396 samples per 10 s is the camera hook's own 240/sec, and the hull's
`+0x50/+0x5C/+0x68` change on 23-25% of them — **exactly 60 per second**, in
every driving window of the session. Halo 3 simulates at a fixed 60 Hz; MCC
interpolates the drawn mesh up to the presented 120 fps. A camera pinned to the
raw sampled value therefore holds still for three frames and then jumps up to
0.1238 wu (0.38 m), and the orientation jumps with it (up to 16 deg in one
step, which swings a 0.67 wu seat point through ~0.19 wu). That is the user's
"the vehicles are shaking and vibrating ... the car gyrates".

Everything else in that session was clean, so nothing else is a candidate:
frame interval p95 9.7-11.9 ms, `stalls=0`, `missed` flat, prediction error
p95 under 0.1 ms, no `INSANE`, no `too-deep`, no faults.

**Theory 2 (suspension rock) is refuted as the cause.** Tilt changes on the
same 27-29% of frames as position — it is the same 60 Hz staircase, not a
separate physical wobble.

### The fix walks the frame across the tick

`Halo3InterpolateFrame` keeps the previous and current simulation states and
publishes `lerp(prev, cur, alpha)` with `alpha` running 0 to 1 across one
measured tick. It is continuous by construction: when a new state arrives the
point just reached (the old current) becomes the new previous, so the published
frame never jumps. It cannot overshoot, unlike a velocity extrapolation, which
would fling the camera through a wall on a collision tick. A component-wise
blend of two rotations is not a rotation, so the result is renormalised and
Gram-Schmidt'd back into a basis; the per-tick angle is small enough that this
is indistinguishable from a slerp.

Details that matter:

- **The clock must be the performance counter.** A 60 Hz tick is under two
  steps of `GetTickCount`'s 15.6 ms resolution, so the sub-tick blend cannot be
  driven from it (the same reason `ApplyVrTurn` uses QPC for smooth turning).
- **The tick length is measured, not assumed** (EMA over observed intervals,
  clamped to 4-50 ms), because detection is quantised to the camera's own rate.
- **A teleport, respawn or seat change snaps.** Past 2.0 wu or 60 degrees in
  one tick, the interpolator resets instead of sliding a camera across the
  level through the geometry between the two ends.
- The bounding-sphere sanity gate and the motion counters both stay on the RAW
  frame — the gate because that is what the engine actually said, the counters
  because they are measuring the tick rate itself.
- Published, so every consumer is smooth for free: the seat anchor, and the
  hull-follow yaw, which was stepping at 60 Hz too.

`vehicle_cam_smoothing` (default 1) turns it off for an A/B.

## C10 — seat entry aligns to the vehicle's nose, not the entry swing

**Report (user, 2026-07-31):** "the angle in which you enter overrides the
direction of where the camera is supposed to be facing".

C3 asked for a generic recenter on a settled seat entry, and `ApplyHeadLook`
implements a recenter as `g_gameYawRef = atan2f(fwd[1], fwd[0])` off the
**engine camera's** forward. At seat entry that camera is still swinging around
the vehicle from the entry animation, so whichever way the player walked in
became their forward. The hull's own forward is unambiguous and is already
published, so entry now sets `g_gameYawRef` to the hull heading and
`g_headYawRef` to the current head yaw: looking straight ahead is the nose.

Only on the way **in**. On the way out the follow has already left the player
facing the hull's final heading, and a recenter there would snap that away.

**Rate-limited on purpose.** The same session logged **42** `head tracking
recentered` lines, several under 200 ms apart, because the debounced seat state
flaps whenever the player-unit read blips. A re-snap mid-drive would yank the
world around to put the nose under wherever the head happened to be pointing,
which is worse than the bug being fixed. Only an entry at least 1.5 s after the
last alignment counts; a flap is consumed and ignored, and a request that never
finds a live seat expires after 2 s rather than firing later on foot. (The
underlying flap is pre-existing and untouched here.)

At `vehicle_view_follow = 0` the old generic recenter is kept, so weight 0
remains exactly what Alpha 0.3.1 shipped.

## C11 — the residual shake, measured from the user's own video

The C10 build still shook. The user supplied a 21.6 s screen capture
(`Recording 2026-07-31 132458.mp4`, 2994x2160 at 30 fps) recorded inside the
C10 session — log source `37b83d7`, `smoothing ON across a 16.67 ms tick`, so
the interpolation was running and locked onto the right rate.

### Measuring it without the head confounding the answer

Head rotation sweeps the vehicle across the view exactly as a camera fault
would, so a naive "did the mesh move" test cannot separate them. Two passes
were needed:

1. Tracking a vehicle patch against a far-world patch gave a differential with
   a *larger* standard deviation (5.66 px) than either input (3.78 / 4.76 px),
   which is the signature of two independent noise sources, not a shared
   rotation. **That measurement was discarded as unreliable** rather than
   reported.
2. The reliable form: keep only frame pairs in which the **distant world is
   stationary** (both components of its tracked shift within 1 px), so the view
   is not rotating and any remaining vehicle motion is real.

Over 491 tracked pairs, 325 had a stationary world. In those:

| | value |
| --- | --- |
| vehicle dx | mean −0.08 px, std 2.26, peak 14 |
| vehicle dy | mean −0.27 px, std 2.60, peak 13 |
| magnitude | mean 2.34 px/frame, peak 18 of a 400 px frame |
| frames moving 2 px or more | 159 / 325 (**49 %**) |

Mean zero with the sign flipping nearly every sampled frame: an oscillation,
not a drift, and one aliased down from above 15 Hz by the 30 fps capture. Scaled
against a full tick's travel (up to 0.1238 wu), the residual is roughly one
sixth of a tick.

### Cause: the blend restarted at zero on a jittery detection

A simulation tick is only *noticed* on the first camera call after it happens.
At 240 camera calls/sec against a 60 Hz tick that is 0 to 4.17 ms late, and a
different amount late every time, because the two clocks are independent. C10
measured `alpha` from the detection instant, so every tick restarted the blend
from a freshly randomised sub-tick offset — and that restart is a step of up to
a quarter of a tick's travel, at 60 Hz. One sixth of a tick measured; up to a
quarter predicted. That is the shake.

### Fix: a free-running phase, gently tracked

`Halo3FrameInterp` now advances `phase` on the wall clock and, on each detected
tick, subtracts one and pulls the remainder only 5 % of the way toward where it
should be (half a camera interval, the mean detection lag). The sub-tick offset
therefore persists instead of being re-randomised, and the correction is spread
over ~20 ticks instead of landing in one frame. A hitch outside [−0.5, 1.5]
ticks relocks immediately.

Tested by simulation, and the test was **verified to fail** with the tracking
constant set to 1.0 (which reproduces C10's hard re-lock): with a camera clock
running 1.7 % off 240 Hz so the detection lag walks through its whole range, no
published frame may advance by more than a quarter of the even share.

`vehicle_cam_lead` (default 0, range 0–1 tick) is exposed for the one thing
this cannot determine offline: whether MCC draws the mesh at the same sub-tick
phase we now reconstruct. A constant phase difference shows up as the vehicle
dragging at speed, not as shake, and one config line answers it without a
rebuild.

## C12 — the lead knob replaced by a measured display-phase lock, and per-vehicle seat trim

The user rejected `vehicle_cam_lead` outright: "a weird method i dont like it
... doesnt make the vehicle drag but also not bouces repeatily from the origin
to behing me at rapid small speeds". Both halves of that sentence are real
defects of the knob, not taste:

- **Drag at 0**: C11 paces the blend evenly but displays between the two most
  recent simulation states, on average roughly half a tick behind. Wherever
  the renderer actually draws the mesh inside the tick, a constant phase
  difference remains, and at speed it reads as the vehicle sitting displaced.
- **Sawtooth at any other value**: the C11 blend clamped its fraction at 1.0
  (the newest state). Any positive lead therefore SLID for the first part of
  each tick and FROZE at the newest state for the rest — a 60 Hz
  slide-and-freeze, worst at low speeds where the freeze is a visible
  fraction of the motion. That is the reported "bounces repeatedly ... at
  rapid small speeds".

No fixed number can be right, because the quantity is unknowable offline:
which sub-tick fraction MCC's renderer displays. C12 measures it instead.

### The witness: the bounding-sphere centre

The sampler already reads the object's bounding-sphere centre (`+0x1C`) every
camera call, from the same capture window as the hull frame, to sanity-gate
the anchor. That centre is ANIMATED data. If MCC render-interpolates the
model, the centre glides BETWEEN the 60 Hz hull states — which makes it a
per-call witness of exactly what the renderer is displaying. The lock uses it
in three runtime-proved steps (`Halo3PhaseLock`, halo3_vehicle_logic.h):

1. **Latch at rest.** While neither the hull frame nor the centre has moved
   for ~1/8 s, sim and render provably show the same pose, so the centre's
   hull-frame offset is read exactly. No authored data, no guess.
2. **Prove render-rate.** Per tick window, count camera calls vs calls on
   which the centre moved. Only a render-interpolated value moves BETWEEN
   hull ticks; a 60 Hz value moves only on the tick itself. No proof, no
   lock — and the worker log says so in words ("bounds not render-smooth,
   C11 pacing"), per the no-silent-fallback rule.
3. **Measure and learn.** Each call where the centre freshly moved (a stale
   read biases the phase low), project the live centre onto the segment
   between the centres computed from the two held sim states: the result is
   the blend fraction the renderer is displaying RIGHT NOW. Its offset from
   the free-running C11 phase is EMA-learned (2% per sample) as `lead`.

The displayed fraction becomes `min(phase, 1) + lead·ramp`, allowed past 1.0
(a true extrapolation of the newest state) up to 1.75 — but only while ticks
are actually arriving, and scaled by `ramp`, which is zero below ~walking
pace per tick of hull travel and full above roughly twice that. So:

- at speed, the camera rides wherever the renderer draws the mesh (drag gone);
- at rest or a crawl, the lead disengages entirely — erratic millimetre
  physics steps are interpolated, never extrapolated (bounce impossible);
- with the witness absent, unproven, or the vehicle unidentified, lead stays
  0 and the behaviour is bit-for-bit C11.

Frame status nuance: the witness and the published frame always come from the
same object pair — a mounted gunner's frame IS the carrier frame and its
bounds window IS the carrier's, so the latch/projection stay self-consistent
in every branch (Direct/Carrier/Composed).

Tests (core_tests.cpp): a simulated renderer presenting at 120 fps drawing
0.6 of a tick AHEAD of the sim, on a camera clock 1.7% off — the lock latches
the offset from rest, learns a lead in (0.2, 1.1), and tracks the on-screen
position within 0.45 of a tick worst-case / 0.15 mean (VERIFIED to FAIL with
the witness removed: the same drive then drags by the full renderer lead).
A rocking vehicle (±4 mm steps) produces no amplified motion, and a 60 Hz
witness leaves the output bit-identical to C11 with the lock never armed.

`vehicle_cam_lead` is retired: parsed quietly from old configs, never written
back, no longer anywhere in the runtime.

### Per-vehicle seat trim behind the same two sliders

The seat trim pair (`vehicle_cam_forward_m` / `vehicle_cam_up_m`) stays the
universal base. New sparse overrides `vehicle_cam_forward_m_<vehicle>` /
`vehicle_cam_up_m_<vehicle>` (scorpion, warthog, mongoose, ghost, wraith,
prowler, banshee, hornet, chopper, turret — mirroring `Halo3VehicleId`, with
a `static_assert` tripwire in game.cpp) exist only for the axis and vehicle
the user actually adjusted. The F1 menu keeps exactly the two existing
sliders: seated, they bind to the occupied vehicle's own trim (label names
it; a mounted gunner binds the carrier; a button hands the vehicle back to
the universal pair); on foot they edit the universal base. ApplyHeadLook
resolves the effective trim per anchor identity via `ConfigVehicleCamForward/
Up`. An unknown vehicle name in the config is dropped, not misfiled; the
overrides round-trip through save/load (tested).

### C12 adversarial review (16-agent, pre-package)

Confirmed and FIXED before packaging: the applied lead was a step function at
the ticks-stopped boundary (a sim hitch at speed would have snapped the
camera by up to 0.75 tick of travel in one frame — it now glides over ~40 ms,
regression-tested); the F1 seat binding could rebind to the universal trim on
a one-frame stale-snapshot blip mid-drag (now sticky: only a sustained
on-foot read rebinds); a malformed per-vehicle config value invented an
override and NaN passed the clamp (now ParseFloatSetting-validated, loudly);
an unknown vehicle name was swallowed silently (now logged); and the phase
log could claim "bounds not render-smooth" before any tick window had
judged it, or claim anything at all with smoothing toggled off (states split,
tickKnown cleared on disable).

Confirmed and ACCEPTED as a bounded limitation: the bounding centre rides the
ANIMATED model, so a sustained animation displacement along the travel
direction (the chopper's ~0.09 wu suspension drift is the measured worst
case) biases the measured phase by up to a few tenths of a tick while it
lasts — a slow cm-scale seat offset at speed, not a shake, self-correcting
within ~0.4 s of the pose relaxing, bounded by the [-0.5, 1.5] lead clamp,
and visible in the logged lead value. Rejecting it would need a hull-rigid
render-rate witness, which is exactly the render-transform RE this design
avoided.

## C13 — headset refresh rate is measured, not chosen; trim is per SEAT

Two user requests, 2026-07-31. One turned out to need a fix that was not the
one asked for, the other was a straight redesign.

### "list different Hz compensation to toggle from ... my headset is 120"

There is nothing for such a list to select, and shipping one would have been a
row of fake buttons. **The 60 Hz is Halo 3's own simulation clock**, fixed in
the engine and identical on every machine; the mod's camera hook runs at
whatever rate the headset presents. The interpolator already measures BOTH:
the tick length is EMA-learned from observed hull changes
(`kHalo3TickMin/Max`), and the phase free-runs on the performance counter in
seconds. Nothing consumed a nominal camera rate.

Except in one place, which the question found. The render-smoothness proof
counted camera calls **inside one tick** and required three of them. At the
measured 240 calls/sec that is trivially satisfied; at 72-90 Hz there are only
1.2-1.5 calls per 16.7 ms tick, so `smooth` could never be evaluated, the lock
would never arm, and every 72-90 Hz headset would have silently sat on C11
pacing forever — the exact silent-degradation the project forbids.

The rate-independent replacement judges on **calls where the hull did not
change** ("quiet calls"), accumulated across ticks:

- a 60 Hz witness cannot move on a quiet call — it only moves on the tick;
- a render-interpolated witness moves on the fraction of quiet calls that
  carry a present, which is present-rate over camera-rate and may legitimately
  be well under half (MCC presenting at 120 while the hook runs at 240);
- so the discriminator is `quietMoves * 4 >= quietCalls` over at least 12
  quiet calls — "sometimes" versus "never", with no rate in it.

Quiet calls are rare on a slow headset and common on a fast one; the proof
just takes a few seconds longer at 72 Hz. Two remaining per-frame constants
became per-second ones so they mean the same thing at every rate: the parked
latch (0.15 s of stillness) and the applied-lead glide (0.04 s time constant;
as a per-call factor it was twice as abrupt at 144 Hz as at 72).

Tested by replaying the same drive — a renderer presenting at the headset rate
0.6 of a tick ahead of the sim — at **72, 80, 90, 120 and 144 Hz**: every rate
must reach the locked state and track the drawn position. VERIFIED to fail
with the proof counter disabled. The F1 and config prose were reworded: they
said "the vehicle's 60 Hz motion", which is what made the user read it as a
headset setting.

### "the config overides every set per vehcile i need it per seat"

Correct, and C12 was wrong: the Warthog driver, its passenger and its gunner
sit in completely different places, so one trim per vehicle cannot serve them.
The trim table is now (vehicle x seat slot): 10 vehicles x 4 slots — `driver`,
`passenger`, `passenger2`, `gunner`. The gunner slot exists because a mounted
turret gunner reports **seat index 0 on the gun tag**, the same index as the
driver on the hull, so the two must be separated by the mounted flag rather
than the index; `ConfigSeatTrimSlot` therefore keys mounted turrets to the
reserved slot whatever index they report, and refuses to key a rider seat onto
it.

Keys are `vehicle_cam_forward_m_<vehicle>_<seat>` / `vehicle_cam_up_m_...`,
sparse (written only where set). A C12-era whole-vehicle key means what it
said and migrates onto all four slots of that vehicle, logged. Unknown vehicle
names, unknown seat names and malformed values are each reported and dropped
without inventing an override. The F1 menu is unchanged in shape — the same
two sliders, now labelled with the seat they are bound to ("Warthog gunner"),
sticky against a one-frame snapshot blip, with the button returning that seat
to the universal base.

## C14 — the headset rate DOES belong here, and C12's witness was dead

The user pushed back hard on being told there was no rate to select. They were
right, on evidence from their own log, and C12's central mechanism was proved
dead in the same file.

### The log, from the 21:53 Steam session on the C12 build

```
H3 seat motion: 2395 sampled frames ... smoothing ON across a 16.62 ms tick;
phase bounds not render-smooth, C11 pacing, render lead +0.00 ticks
PACING F ... periodNs=8333333 appHz=120.000 predDeltaMs=8.334
```

Two facts:

1. **The C12 bounding-centre witness never armed on real hardware.** It
   reported `bounds not render-smooth` and `render lead +0.00 ticks` for the
   whole session, i.e. MCC's bounding-sphere centre is NOT observably
   render-interpolated, so C12 delivered exactly C11 and nothing more. The
   loud-failure reporting built into C12 is what made this visible in one
   grep — but a mechanism that has never once been observed to work must not
   steer the camera. It is now **diagnostic-only**: still measured, still
   logged, no longer an input.
2. **The runtime reports the headset at 120.000 Hz with an 8.333 ms period,
   and OpenXR hands us the predicted display time for every frame.**

### Why the rate genuinely matters

Every frame is *displayed* one refresh period after it is built. OpenXR
already predicts the HEAD pose to that photon moment — that is what
`predictedDisplayTime` is for, and the head has always used it. The vehicle
was placed at the *sample* instant instead. So the seat trailed the head by a
full display period, systematically, and proportionally worse the faster the
vehicle moved: 8.3 ms at 120 Hz is **half a 60 Hz simulation tick**.

That is the real content of "my headset is 120 and 60hz is not enough". Not a
smoothing strength, not a resample rate — a missing prediction interval that
is literally the headset's refresh period.

C14 advances the seat by `leadSeconds / measuredTick` ticks, where
`leadSeconds` is one period of the rate the runtime reports
(`VR_HeadsetRefreshHz`, already present). It rides on top of the accepted C11
pacing, keeps the driving-pace ramp (so a parked or crawling vehicle is never
extrapolated), and keeps the ~40 ms glide (so a hitch cannot snap it out).
Unknown rate ⇒ zero lead ⇒ exactly the previous build.

`vehicle_smooth_hz` (0 = Auto) exists for a runtime that misreports, with the
F1 list the user asked for: Auto / 60 / 72 / 80 / 90 / 96 / 100 / 110 / 120 /
144 / 165 / 240, any other integer hand-typable. Auto is correct wherever the
runtime is honest, which is why it is the default rather than a required
choice.

Tested by differencing the same drive with and without prediction at 72, 80,
90, 120 and 144 Hz — differencing because the camera and the 60 Hz simulation
lock into a fixed ratio (exactly 2:1 at 120 Hz) whose constant sub-tick
alignment says nothing about the lead. Measured at 120 Hz: 0.049997 wu against
0.050000 expected. Also pinned: 72 Hz predicts ~2x as far as 144; a WRONG pin
visibly mispredicts (which is why Auto is the default); a crawl never
extrapolates; a hitch glides. VERIFIED to fail with the lead disabled.

## C15 — the forward/back bounce: the blend was hitting a wall at the newest state

"the car is trying to keep up with the camera but it keeps bouncing forward
and back, most prominent during boosting and flying" (user, 2026-08-01).

`Halo3InterpolateFrame` published `alpha = phase < 1 ? phase : 1`. The camera
hook runs ~251 calls/sec against a 60 Hz simulation, and C14 adds a half-tick
photon lead, so the blend fraction genuinely wants to sit past 1.0 for **52%
of frames while driving** (measured: alpha ranges 0.525 to 1.522 at boost
speed). Clamping there pinned the seat AT the newest simulation state for
those frames while MCC kept gliding the mesh, then let it catch up in a lump
— a 60 Hz forward/back judder whose size scales with speed, i.e. invisible
when parked and worst under boost and in flight. Exactly the report.

### Measured, three ways, before changing the default

An offline probe against the real hook rate (251/sec), the real tick
(16.7 ms) and the shipped 120 Hz prediction:

| policy | frame-step spread, constant boost | per-frame correction, accelerating | per-frame correction, hard banking turn |
| --- | --- | --- | --- |
| clamped (C10-C14, shipped) | 0.923x - 1.060x | 0.137x | 0.206x |
| continuous (C15) | 0.970x - 1.031x | **0.034x** | **0.162x** |
| continuous + curved extrapolation | 0.970x - 1.031x | 0.034x | 0.158x |

Removing the clamp is the whole win: a 4x reduction in per-frame correction
under acceleration and 21% on a banking turn. An acceleration-aware
(second-difference) extrapolation was built and measured too — worth 0% on a
straight boost and 2.5% on the turn, which does not justify the extra state
or the risk of a collision spike bending the camera, so it was **dropped**.

Past the newest state the extrapolation is faded by a `live` factor that
glides to zero over ~40 ms when simulation ticks stop, so a stall parks the
seat on the newest state instead of sailing on, and does so smoothly rather
than snapping (the C12 review's finding, preserved).

### Two methodology failures worth remembering

1. **A silent no-op patch produced a false null result.** The first A/B said
   the clamp made no difference; the "before" header had not actually been
   patched (a `str.replace` that matched nothing). Every generated variant is
   now asserted to have changed before it is compiled. The false null nearly
   led to abandoning the correct fix.
2. **The prediction was wired through the diagnostic object.** `leadApplied`
   lived on `Halo3PhaseLock`, inside `if (lock)`, so any caller that passed
   no lock silently got no prediction at all. The game always passes one, so
   it worked in the headset — but every offline measurement of the prediction
   read zero until this was found. It now lives on the interpolator, with the
   lock keeping only a mirror for logging.

The 10-second log line now carries `blend N.NN`. Above 1.00 the seat is being
extrapolated; pinned exactly at 1.00 while driving is this defect returning.

## C17 — native seat ownership replaces object-sampler camera motion

The C16 headset report rejected the architecture, not another interpolation
constant: the player felt the car and camera alternately catching up and wanted
the natural bounce of being inside the rendered vehicle. The attached Nexus
mod archive (`Halo 3 first person warthog run-544-1-0-1615062444 (1).zip`,
SHA-256 `9BA49F10BD44BE88AF917C991A9F6F273A1A5A00637804B9032FD8F49BF34E5F`)
contains only a replacement `120_halo.map`. It is from an older MCC cache build,
so a whole-file binary diff against the current stock map is not valid evidence.

The relevant method is independently bounded by two readable sources:

- Official H3EK exports already recorded in E1 prove every stock player seat
  sets seat flag bit 4, `third person camera`, and that seat camera markers are
  optional.
- Assembly's open-source `Formats/Engines/Halo3MCC/Plugins/vehi.xml` describes
  the MCC cache layout used by map mods: the vehicle seats block is at `0x258`,
  each seat is `0xD4` bytes, flags are at seat `+0x00`, and the camera-tracks
  block is at seat `+0x80`. This layout agrees with the E1 field ordering and
  flag values rather than supplying semantics of its own.

C17 applies that map-mod method only to the currently occupied loaded tag:
clear bit 4 and temporarily zero the seat camera-track count. Both original
values are saved and restored on seat exit, seat change, or feature disable;
the map on disk is never touched. Counts, indices, pointers, the expected bit,
and post-write values are validated under SEH. Any mismatch is a loud
`StockFallback` for this feature only and leaves the Halo 3 camera core armed.

Most importantly, the engine-computed native seated camera position is now the
motion parent. It supplies translation, suspension/occupant bounce and MCC's
render-rate hierarchy directly. On the first stable native frame C17 measures
that point in vehicle-local coordinates, then applies only the fixed local
difference to the existing Blender-authored point. The sampled object origin is
not used in the camera result; the sampled basis rotates only that small
placement correction and continues to serve seat identity/yaw behavior.

The pure regression proves two invariants: initial calibration lands exactly on
the Blender point, and any later native-camera translation passes through to the
final anchor exactly while the sampled object frame remains unchanged.

### Headset rejection and rollback

Candidate `ee5351a` / DLL `23695970…74A2CD6D`, Steam edition, SteamVR/OpenXR
2.17.6, Oculus headset at 120 Hz. The runtime log proves the transaction was
actually active on Warthog and Prowler seats; it never entered StockFallback.
The headset result was an absolute regression: vehicle-mesh behavior remained
incorrect and every authored camera placement shifted deeply out of place.
Therefore this is not a layout-validation failure and must not be tuned past.

Status: **HEADSET REJECTED**. The next candidate disables both C17 consumers:
the loaded-seat tag transaction never runs and `ApplyHeadLook` again consumes
the C5-C16 authored anchor. C17 code remains dormant, per the project rollback
rule; deleting it would be a separate headset-tested cleanup candidate.

## C17 post-rejection correction and C18 live-node design - 2026-08-01

This section supersedes the pre-test inference above about how the attached mod
works. C17 did not reproduce that mod: it used the wrong cache layout, removed
camera tracks the mod retains, and mixed a native world position with an
object-root basis. Candidate `ee5351a` proved that transaction active and the
headset rejected it. Source `ce26903` disables the behavior and is the current
rollback base.

### Exact attached-artifact comparison

The attached ZIP has SHA-256
`9BA49F10BD44BE88AF917C991A9F6F273A1A5A00637804B9032FD8F49BF34E5F`.
Its only payload, `120_halo.map`, is 513,187,840 bytes with SHA-256
`0CC42E7E785832FE95D765879A276283FBBC624AF06EE85EE7DDBC33B706106A`.
It is cache version 11 from 2020-11-24. The matching dddb334-era cache schema,
not the current Assembly plugin cited above, places the vehicle seats block at
`vehi+0x2A0`, uses a `0xE4` seat stride, puts the seat camera-marker string ID at
seat `+0x6C`, and puts the camera-tracks block at seat `+0x80`.

After parsing both caches through their corresponding layouts, the relevant
Warthog changes are:

| Seat | Flags | Camera marker | Camera track |
| --- | --- | --- | --- |
| driver | `0x48014 -> 0x48004` (clear bit 4 only) | null -> `camera_driver` | count remains 1 |
| passenger | `0x1070 -> 0x1020` (clear bits 4 and 6) | keeps `camera_rider` | count remains 1 |
| chaingun gunner | `0x101198 -> 0x100108` (clear bits 4, 7 and 12) | keeps `camera_gunner` | count remains 1 |

The referenced `warthog_d_camera` track (13 points) and
`warthog_g_camera` track (15 points) are bit-for-bit unchanged from current
Update 14 stock. All 16 main-Warthog nodes and all five chaingun nodes are also
unchanged. Apart from their string IDs, only these marker translations differ:

| Marker / node | Attached mod | Current stock |
| --- | --- | --- |
| `camera_driver` / Warthog node 0 | `(-0.010000, +0.1680864, +0.230000)` | `(+0.007445268, +0.1680864, +0.2062165)` |
| `camera_rider` / Warthog node 0 | `(-0.010000, -0.1586443, +0.3855522)` | `(+0.07150621, -0.1586443, +0.3855522)` |
| `camera_gunner` / chaingun node 3 | `(-0.300000, 0, +0.170000)` | `(-0.003213291, -0.000000093, -0.005836391)` |

The mod contains no renderer hook and no object-transform rewrite. It keeps
Halo's native seat-camera and camera-track machinery and authors marker data on
model nodes. C17's zero-track transaction therefore was not implementation
parity.

### What the native camera and renderer actually consume

Official H3EK and the pinned retail DLL independently identify the same
read-only node provider and marker resolver:

| Function | H3EK RVA | Retail `halo3.dll` RVA | Verified role |
| --- | --- | --- | --- |
| interpolated node-bank provider | `+0x4D7C20` | `+0x1846AC` | returns the live `0x34`-byte node-matrix bank and count |
| internal marker resolver | `+0xA3ABE0` | `+0x343D74` | resolves a marker; argument 6 requests interpolated nodes |
| public marker wrapper | `+0xA42F40` | - | non-interpolated public wrapper is not the C18 source |
| native unit-camera evaluator | `+0xA61330` | `+0x3555EC` | invokes the internal resolver with `localOnly=false`, `interpolateNodes=true` |

The pinned Steam DLL is SHA-256
`B209D8454B12DC77E54CCD2C9924EC8D44B8619D21CF98E36FFAF601E67EFB63`.
The provider body is `0x1EA` bytes with SHA-256
`9F8CB292784AABA32078A9D52CC72744763126111092A886A58002022C0EEAF7`;
the resolver body is `0x281` bytes with SHA-256
`28C4C37308F338B38F243A564E6E85BB6A806E301DAEB0E14E2A4440E4314C7E`.
Relocation-tolerant entry signatures each match exactly once in both the Steam
and Microsoft Store modules, at the same RVAs. The Store module checked here is
SHA-256 `FCE2D92F06FCCEF3C4A66933AB280E90EC1AD705CFE71E68107816324D76FF6B`.
The RVAs remain diagnostics; runtime ownership requires those unique matches.

The internal resolver ABI is
`int16(object, string_id, object_marker*, maximum_count, local_only,
interpolate_nodes)`. Its `object_marker` is `0x70` bytes: node index `+0x00`,
node matrix `+0x04`, final matrix `+0x38`, final world translation `+0x60`, and
radius `+0x6C`. Retail's argument-6 branch calls the provider at
`+0x343E6F`; H3EK does so at `+0xA3ACE4`.

This provider is not merely camera-adjacent. The visible retail object renderer
calls the same `+0x1846AC` at `+0x3496F3`; the H3EK renderer calls
`+0x4D7C20` at `+0x96716A`. Both take the raw node bank only when the provider
reports no interpolated bank. This closes the ownership edge C10-C16 guessed:
the camera can consume the same live node matrices as the visible mesh without
predicting an object root.

Clearing seat bit 4 does **not** make the edited vehicle camera marker the final
normal first-person position for a live biped occupant. The native evaluator
first resolves the vehicle marker, then its bit-4-clear branch resolves the
occupant's `head` marker (string ID `0x9F`) with interpolated nodes and overwrites
the prior result. Retail performs the condition at `+0x3557EC`, the head query
at `+0x355843..+0x35585B`, and the overwrite at `+0x355865`; H3EK's homolog
starts at `+0xA614CD`. Thus the attached mod's vehicle-marker translation edits
are vestigial in normal live-biped first person after bit 4 is cleared. Tracks
remain relevant to the forced-following and bit-6 entry-camera states, which is
why their unchanged presence cannot be treated as dead data.

The official exported tags under `out/analysis/h3_vehicle_markers` provide a
second reason not to key the implementation on a camera-marker name. All 20
supported seat records have camera tracks, but only 15 have a dedicated
render-model camera group. The five without one are the Wraith, Prowler,
Banshee, Hornet and Scorpion drivers. Every occupant does, however, name the
node on which Halo attached it.

### Proven layouts and placement-preserving math

For an attached object, object byte `+0x14` is its signed parent-node index. The
signed word at object `+0x136` is the node-bank **byte size**, which must be
positive and divisible by `0x34`; dividing by `0x34` gives the node count. The
signed relative offset at `+0x138` locates the raw bank, whose matrix stride is
`0x34`. For loaded tags, the verified chain is vehicle `vehi+0x40` model datum
-> model `hlmt+0x0C` render-model datum -> render-model `mode+0x30` nodes
block. A node is `0x60` bytes and stores its inverse default
`real_matrix4x3` at node `+0x28`. A `real_matrix4x3` is scale followed by
forward, left, up and position, total size `0x34`.

Let `P` be the unchanged Blender-authored point plus the configured forward/up
trim in vehicle/tag space, `D^-1`
the selected node's stored inverse default matrix, `W(t)` the vehicle's live
model-to-world transform, and `L(t)` the provider's live world-space matrix
for that node. Resolve once per stable seat:

`P_node = D^-1 * P`

Then the exact base camera point is:

`P_base(t) = L(t) * P_node`

At the default pose `L(t)=W(t)*D`, so
`P_base=W(t)*D*(D^-1*P)=W(t)*P`: the camera is exactly the authored local
placement carried into the world, instead of a recalibrated or guessed offset.
Later translation, suspension, bank and animation come only from Halo's live
node.

The mod's natural occupant bounce is added without moving that baseline. The
15-sample enum-only seat debounce is not a head-reference gate: at the measured
camera-call rate it can finish in about 62 ms, while boarding and seat-switch
animation is still moving the occupant toward the destination. Freezing that
transient was a direct explanation for C17-style deep offsets and visible
catch-up.

C18 keys the reference to the concrete
`(generation, unit, direct parent, node object, seat, node, source)` identity.
On any identity change it keeps `P_base` unchanged for at least 1.5 seconds and
until the head has stayed within a 0.015-wu node-local sphere for a continuous
500 ms. Invalid/missing samples restart that pre-latch observation. Only then
does it latch the occupant-head position `H_world(t0)` in the same node space:

`H0 = L(t0)^-1 * H_world(t0)`

For later frames, `H(t)=L(t)^-1*H_world(t)` and:

`P_camera(t) = L(t) * (P_node + H(t) - H0)`

At the latch frame `H(t0)-H0=0`, so the result is exactly the authored point.
Only subsequent occupant-head motion is added; vehicle motion has a single
parent, `L(t)`, and cannot be double-counted. A later head-local displacement
over 0.08 wu is treated as another animation transition, not bounce: C18
immediately returns to `P_base` and requires a new settle window.

The resolver can independently fall back to a raw unit node even when its
`interpolate_nodes` argument is true. Therefore an interpolated vehicle node
does not accept the returned head position on the argument alone. C18 obtains
the provider's unit bank, recomposes
`unit_node[marker.node_index] * marker.node_matrix.position`, and requires that
translation to match the resolver's final `marker.matrix.position` within eight
scaled float epsilons. A mismatch omits head bounce for that frame and retains
the exact node-authored baseline. With a raw vehicle node, the resolver is
explicitly forced raw.

Position neutralization is independent of yaw follow. On the first valid anchor
for each concrete `(runtime generation, parent handle, seat, vehicle identity,
mounted state)` identity, C18 captures the current OpenXR head position as the
neutral room-space origin. That includes seat switches, rapid real re-entry and
stationary turrets; the old 1.5-second anti-flap limit remains only on the yaw
snap. A missing/torn frame retains the concrete key and cannot repeatedly erase
leaning. Without this split, room translation accumulated since an older
on-foot/manual recenter would be added after C18 and shift every Blender point
uniformly. Leaning after the concrete-seat capture remains live; only stale
pre-entry translation is removed.

Normal seats select the direct vehicle and the occupant unit's parent-node byte.
A mounted gunner selects the grandparent carrier and the mounted gun child's
parent-node byte, because those Blender points were authored in carrier tag
space. This covers the Warthog `turret`, Scorpion/Wraith
`anti_infantry_turret`, and Prowler `mauler_g` attachment nodes without copying
a marker, bone or offset between vehicles.

### Negative finding and transaction boundary

The discarded rigid-root proposal used retail `+0x20D6C` and `+0x20E0C`.
Their shared helper at `+0x20CD8` requires object-table kind byte `+3 == 0`;
the verified vehicle kind is 1, so both accessors return false for the vehicle
handles in scope. Even without that gate, they provide only the root
`+0x50/+0x5C/+0x68` frame before node assembly and omit suspension/body-node
motion. They are not a vehicle-mesh camera solution.

C18 therefore leaves loaded seat, marker and track tags untouched. The object
sampler remains only for bounded seat identity, parent selection, sanity bounds,
yaw follow and controls; it is not the camera translation parent. The live-node
binding and occupant-head binding are separate optional transactions. A zero or
multiple signature match, fault, invalid byte size/count/index/matrix, torn
sample or out-of-bounds anchor falls back only that camera-parent feature to
stock. Head failure retains the node-authored baseline; neither failure disarms
the Halo 3 camera core, OpenXR, or unrelated vehicle controls.
When the interpolated-node source is absent, smoothing-on placement stays stock;
the explicit smoothing-off raw-node A/B remains available.

**Status: C18 DESIGN/CANDIDATE ONLY - NOT HEADSET ACCEPTED.** It must not advance
`docs/CURRENT-STATE.md` until the user accepts the exact packaged DLL in the
headset. The accepted-build pointer remains unchanged.

## C18 headset result and C19 exact-follow controls - 2026-08-01

Candidate `9df5ec3`, DLL SHA-256
`8B2D2616F9B0CB5400C43350C4BC366B3F8A41FE541AB00B972650B79646F640`,
was tested in the Steam edition on SteamVR/OpenXR 2.17.6, reported headset
`SteamVR/OpenXR : oculus`, at 120 Hz. The preserved live log has SHA-256
`8C1AEB40DC84CC5BF3BF85C179C51BEEC012B553FD6ABD2A56A58466CF3B6B87`.
It reports the rendered-node path on every sampled frame, zero anchor
fallbacks, and long settled runs with occupant-head parenting. The user
explicitly confirmed that the vibrating/incorrect vehicle mesh behavior is
gone. This is positive headset evidence for retaining C18's rendered-node
placement path.

The same test rejected the remaining turning behavior: after a hard collision
or spin, the view can remain rotationally offset from the vehicle. The run had
`view follow 1.00`, so fractional weighting is not a sufficient explanation
for this result. Static inspection identifies a direct loss mechanism in the
active path: `Halo3SeatYawDelta` updated `lastYaw` but returned zero for any
single sample above 0.60 radians. That rotation could never be recovered, so
the shared view reference permanently lagged the car by the discarded angle.
A transient invalid/near-vertical sample also disarmed the accumulator and
lost the angle traversed before the next valid sample.

C19 makes follow an ON/OFF control, ON by default. It retains compatibility
with existing configs: every finite old non-zero value, including 0.82,
loads as ON; zero loads as OFF; saving writes 1 or 0. ON consumes every finite
wrapped heading change from the same concrete
`(runtime generation, parent handle, seat index, vehicle identity, mounted)`
seat. That same key, rather than angular magnitude, separates a seat/vehicle
switch from a violent rotation of the current car. Missing, torn, invalid or
near-vertical samples retain the last valid heading so the next valid sample
catches up. OFF clears the yaw ownership and preserves the prior world-locked
behavior.

C19 also adds a third per-seat placement axis without changing the C18 parent.
`vehicle_cam_right_m` and
`vehicle_cam_right_m_<vehicle>_<seat>` use positive values toward the
vehicle's right (negative tag-left); they are added to the Blender point before
the stored-default-inverse -> live-rendered-node transform. Forward, height and
right therefore remain tag-space placement coordinates and cannot slide
against the mesh as the vehicle turns or banks. The F1 sliders remain bound to
the concrete current seat. Forward and height ranges expand from -0.5..1.0 m
to -1.0..1.5 m; lateral range is -1.0..1.0 m.

**Status: C18 rendered-node mesh behavior HEADSET CONFIRMED; C19 candidate
changes NOT YET HEADSET ACCEPTED.** The accepted-build pointer remains
unchanged pending the exact packaged C19 headset result.

## C20 — hide the occupant's character model in a first-person seat

The C19 headset session left one thing unaddressed: with the VR camera sitting
at the authored seat point, Halo is still drawing the player's own character
model in the space the headset looks out of. Halo already has a state that
suppresses it — its own first-person state — and a single seat field selects it.

### The verified live chain (pinned retail `halo3.dll`)

The pinned Steam module is SHA-256
`B209D8454B12DC77E54CCD2C9924EC8D44B8619D21CF98E36FFAF601E67EFB63`.
All four sites below were read from that module's on-disk image by RVA.

| Retail RVA | Role | What the disassembly shows |
| --- | --- | --- |
| `+0x1326E8` | seated camera-mode chooser | Computes the occupied seat record as `seats + seat*0xD4` (`+0x1327D0 imul rcx, rax, 0xD4`), loads its flags dword at seat `+0x00` (`+0x1327D7`), extracts bit 4 (`shr esi,4; and esi,1`) and bit 6 (`shr eax,6`), writes a mode enum through its out-parameter and **returns bit 4 in `eax`** |
| `+0x212198` | camera-update dispatcher | Calls `+0x1326E8` (`+0x2121B8`), then `test eax,eax` → **zero goes to the first-person camera update `+0x17DAEC`, non-zero to the following/chase camera `+0x2144C0`** |
| `+0x17DAEC` | first-person camera update | Calls the seated camera evaluator `+0x3555EC`, then tests seat flag bit 7 `first person camera slaved to gun` (`+0x17DBDB shr eax,7`) |
| `+0x3555EC` | seated camera evaluator | Recomputes the same `seat*0xD4` record (`+0x35579D`), and at `+0x3557F5` loads seat flags and `shr eax,4; test bl,al; jne <end>` — the occupant `head` marker query at `+0x355853` (`edx = 0x9F`) runs **only when bit 4 is clear** |

Two facts follow directly, without inference:

1. **The seat's `third person camera` bit is the switch** between Halo's
   first-person camera and its chase camera for a seated player.
2. **It is re-read from the loaded tag on every camera evaluation** — nothing
   is latched at seat entry — so patching the loaded seat takes effect on the
   next frame and restoring it is complete.

This closes the layout question C17 left open from a second direction: the
`vehi+0x258` seats block, the `0xD4` stride, and flags at seat `+0x00` are now
confirmed by the engine's own indexing arithmetic, not only by the E1 exports
and Assembly's plugin.

### Why this is the right field

- Every stock Halo 3 player seat sets bit 4 (E1 official H3EK exports), which
  is exactly why the title ships no first-person vehicle view.
- The community first-person-vehicle map mod already dissected in this document
  clears exactly this bit on the Warthog driver, passenger and gunner seats.
  It is the only field all three of its edited seats have in common.
- Requiring the bit to be SET before writing doubles as the live proof that the
  tag walk landed on a real player seat; anything else is a loud
  `StockFallback` for this feature alone.

### What C20 changes, and what it deliberately does not

While the player is seated and the VR vehicle camera owns the view, the
currently occupied loaded seat record has bit 4 cleared; the original value is
restored on seat exit, seat change, feature disable, probe teardown and title
teardown, and only if the live value is still the one we wrote. No map file is
touched.

C20 is **not** C17. C17 also zeroed the seat's camera-track count and then made
the resulting native camera the motion parent, and was headset-rejected for
placement. Here the camera remains the accepted C18/C19 authored-node anchor —
`ApplyHeadLook` overwrites position from that anchor and orientation from the
headset, so which camera Halo selects underneath cannot move the view. The
camera-track count is read as a bounds check and never written; the reference
mod keeps those tracks. `Halo3ComputeNativeParentedAnchor` remains dormant and
uncalled.

New config `vehicle_hide_body` (default 1) gates the transaction, and
`vehicle_first_person = 0` suppresses it entirely, so third-person driving is
byte-for-byte unaffected.

### The open question this candidate answers

Bit 4 provably selects Halo's first-person *camera*. Whether that state also
suppresses the occupant's third-person model — and, in the `allows weapons`
passenger seats (bit 5: Warthog seat 1, Mongoose seat 1, Hornet seats 1–2,
Prowler seats 1–2), whether it brings up the first-person weapon that the
existing `floating_hands` and controller-aim path would then own — is a
runtime property the headset test resolves. Both outcomes are informative and
neither can disarm the camera core.

**Status: C20 candidate — NOT HEADSET ACCEPTED.** The accepted-build pointer is
unchanged.

## C20 headset result, and C21 — the arms ride the seat, not the face

**C20 (`8547b7d`, DLL `19089BF4…C656F5B`) is HEADSET ACCEPTED.** Steam edition,
run at 05:40–05:51 on 2026-08-01. The user's words: "vehicles feel great." The
log carries `H3 first-person seat: Active` on every seat entry and its matching
`restored the seat's own camera flag` on every exit, with no `StockFallback`.
Two things are now confirmed at runtime rather than predicted:

1. Clearing seat flag bit 4 does suppress the occupant's character model.
2. It also brings up the **first-person weapon** in `allows weapons` passenger
   seats — the user reports hands and gun present there.

Note the C20 run never entered a passenger seat (every `H3 vehicle probe` line
is `seat=0`); the passenger observation came from a later sitting on the same
build.

### Why the engine builds first-person weapons for a vehicle occupant

The gate is the director's PERSPECTIVE, and nothing in the chain asks whether
the unit is in a vehicle:

| Retail RVA | Role |
| --- | --- |
| `+0x131B40` | `set_camera_mode`. Constructs the camera for the requested mode, then stores `c_director::get_perspective()` at user record `+0x604` and the mode itself at `+0x608` (per-user stride `0xC`, block at TLS `+0x188`) |
| `+0x131A00` | `c_director::get_perspective()` — calls the live camera's vtable slot 1, defaulting to `_director_perspective_neutral (3)` |
| `+0x7961D0` | first-person camera vtable: `get_type` → `3` (`_camera_mode_first_person`), `get_perspective` → **`0`** (`_director_perspective_first_person`) |
| `+0x28ADDC` | the first-person model cache. Zeroes `first_person_model_count`, sets `first_person_camera_object_index` to `-1`, and only when `+0x20F494` returns `-1` **and** user `+0x604 == 0` does it store the player's unit into `first_person_camera_object_index` and call `+0x2C0D20` to build the first-person weapon models |
| `+0x2C0D20` → `+0x184B08` | builds those models through the exact first-person interpolation function this mod already hooks |

The two render globals are `halo3+0xADAC20` (`first_person_camera_object_index`)
and `halo3+0xADAC24` (`first_person_model_count`), decoded from three
independent RIP-relative stores in `+0x28ADDC` that resolve to the same pair.
The first is what makes the world renderer skip the player's own object — the
mechanism behind C20's accepted result. `+0x20F494` is a scripted/cinematic
first-person override that returns `-1` in ordinary gameplay.

So: seat bit 4 clear → first-person camera → perspective `0` → the occupant's
character model is skipped **and** their first-person weapon is built, in a
vehicle exactly as on foot. This layout also independently corroborates
ManagedDonkey's `s_render_object_first_person_globals` field order.

### The defect C21 fixes

The user's report, in a passenger seat: *"hands and guns show, the tracking
sorta works, but as I rotate the camera I lose sync to the guns — the arms need
to follow my chest, be careful not to chain it to my face."*

`ControllerWorldPoseEx` places the wrists at `base + R(controller room offset)`,
where `base` is `g_baseCam`, snapshotted from the engine's camera SOURCE
position before `ApplyHeadLook` runs. On foot that source is carried by the
player's body and is a sound origin.

In a first-person vehicle seat it is not. The seated camera evaluator's
bit-4-clear branch resolves the occupant's own `head` marker (`+0x355853`,
string id `0x9F`) and overwrites the camera position with it. So after C20 the
hand origin in a seat is literally the seated biped's head: turning the view
drives the occupant's aim, which animates the head and neck, which moves the
marker, which drags the arms and gun. That is precisely "chained to my face".

C21 anchors the arms to the seat instead. The sampler already computes the
authored placement twice — once as `P_base(t) = L(t) * P_node`, rigid in the
vehicle's frame, and once with the C18 occupant-head bounce added. Only the
second was published. C21 publishes both and gives the hand origin the rigid
one, so the vehicle's motion, suspension and banking still carry the arms while
head rotation cannot. The camera keeps its bounce; nothing about placement,
yaw follow, trims or the C18 parent changes.

It is deliberately the **same point** the camera uses, not a lowered chest
position. The controller displacement added to it is measured from the
room-space head reference captured at that exact seat, so moving the origin
down would drop the hands by that amount rather than re-parent them. What
changes is what the origin FOLLOWS, which is now the body.

Selection is conservative and tested: the rigid anchor is taken only when the
feature is on and the seat published a finite placement this frame. Off foot,
on a torn or missing sample, or with the feature off, the existing camera
origin is left byte-for-byte alone — the fallback is exactly the behavior C21
replaces, never an invented position. New config `vehicle_hands_follow_body`
(default 1) plus an F1 checkbox gives a one-click A/B.

Coverage is already complete for the ask: every player passenger seat in the
table has an authored point — Warthog seat 1, Mongoose seat 1, Hornet seats
1–2, Prowler seats 1–2 — so all of them take both the first-person seat flag
and the rigid hand anchor.

**Status: C20 ACCEPTED; C21 candidate NOT YET HEADSET ACCEPTED.**

## C22 — the occupant-head contribution saturates; it never drops out

The C21 headset sitting reported the arms and gun "losing tracking and then
resetting", and "it's weird how the whole car resets". The cause is measured,
not inferred, and it is **not** specific to passenger seats or to C21 — it is
the shared seat mechanism, so this fixes every seat in every vehicle.

### The measurement

`H3 seat motion` in the C21 run (`8fdd238`, Steam, 2026-08-01 06:19) reports the
head-parented share of each ten-second window:

| Window (06:2x) | head-parented | anchor fallback |
| --- | --- | --- |
| 1:19 | 0% | 0% |
| 1:29 | 35% | 0% |
| 1:39 | 19% | 0% |
| 1:49 | 27% | 0% |
| 1:59 | 0% | 0% |
| 2:09 | 0% | 0% |
| 2:19 | 0% | 0% |
| 2:29 | 42% | 0% |
| 2:39 | 0% | 0% |
| 2:49 | 16% | 0% |
| 2:59 | 0% | 0% |

Node interpolation is 100% and anchor fallback is 0% throughout, so the seat
placement itself never failed. What toggled is the occupant-head contribution,
and **every toggle steps the camera by the whole accumulated bounce**.

### Why it toggled

C18 gated head parenting on `Halo3HeadLocalDeltaWithinLimit` (0.08 wu) and, on
a breach, threw the settle latch away and demanded a fresh window before the
contribution could return. That window is `kHalo3HeadSettleMinimumAgeMs` 1500 ms
of seat age plus `kHalo3HeadSettleQuietMs` 500 ms with the head inside a
`kHalo3HeadSettleRadius` 0.015 wu (4.5 cm) sphere.

Driving breaches 0.08 wu routinely, and a head on a moving vehicle never holds
inside 4.5 cm for half a second — so once dropped it stayed dropped until the
car was calm. That is exactly the pattern in the table: partial windows while
manoeuvring gently, whole windows at 0% while actually driving, and a step at
each edge. With C21 the arms hold the rigid seat point while the camera does
not, so each step also read as the hands jumping against the view.

### The change

`Halo3ClampedHeadLocalDelta` clamps the delta to the same 0.08 wu instead of
gating on it, and the in-limit test and its re-settle branch are gone. The
clamp is the identity inside the limit — so it agrees exactly with the boundary
the old gate enforced — and scales to exactly the limit outside it, which makes
it continuous at `|d| = L`. A camera step therefore cannot occur at any head
displacement.

A large entry or seat-switch animation is still bounded to the same distance it
was before; it now arrives and leaves smoothly rather than snapping to zero,
which is strictly less jarring than the behavior it replaces. Once a seat has
settled, the contribution stays on for that whole seat session.

This is **less** state and less work per frame than the drop-and-re-settle it
replaces: one clamp versus a distance gate, a latch reset and a re-observation.
There is no filter, no smoothing window, no predictor and no new clock — the
2026-07-31/08-01 rejections of phase, lead and prediction machinery all stand.

Tested for the properties that matter rather than for a value: identity inside
the limit, exact saturation on it, agreement between just-inside and on-limit
(the continuity that forbids a step), and refusal on a non-finite sample so the
caller keeps the exact authored point.

**Status: C22 candidate — NOT HEADSET ACCEPTED.**

## C23 — bounce strength, and a play-space re-centre on both seat edges

Two user-requested changes, taken together at their explicit request. They are
independently observable: one has a slider and one has a log line.

### 1. The bounce was measured in world units, and 0.08 wu is 24 cm

`kHalo3HeadMaximumLocalDelta` is `0.08` **world units**, and the authoring kit
fixes 1 wu = 3.048 m, so the occupant contribution was allowed to carry the view
**24 cm**. Before C22 that rarely mattered, because the contribution spent most
of any drive switched off (see the C22 table). C22 made it continuous and always
on, so the user felt the full travel for the first time and reported it as
nauseating — "smoothing is not the solution for this ... tone that down".

`vehicle_bounce` (0..1, default **0.35**) scales the contribution after the
clamp. It is a strength control, not a filter: no averaging, no delay, no
history, so none of the rejected phase/lead/prediction lineage returns. Clamp
then scale keeps both operations continuous, so no setting can reintroduce the
C22 step. 1.0 is exactly the C22 build the user tested; 0 bolts the view to the
seat, which is the C16 behavior; 0.35 caps the travel at about 8.5 cm.

Tested as properties rather than as a value: half gain is exactly half travel,
zero gain is exactly zero, an out-of-range gain cannot amplify past full travel,
and a non-finite gain is refused so the caller keeps the authored point.

### 2. Re-neutralise the room origin on entry AND exit

The user had to reach for the L3+R3 chord after boarding a Banshee to get back
into place. Position neutralisation previously happened only on the first valid
anchor of a new concrete seat; nothing re-neutralised on the way out, so any
physical movement made while seated stayed baked in as a standing lean once back
on foot, and any step taken while walking up to the vehicle could be carried in.

`vehicle_recenter_on_seat` (default ON) raises the existing position-only
request on **both** settled debounce edges. This is deliberately the position
half of the L3+R3 chord and never the yaw half, so it cannot snap the facing
that the entry nose-align establishes on the way in, or that the hull follow has
already given the player on the way out — the C10 reasoning for excluding a full
recenter on exit is preserved exactly.

Ordering is safe: on entry the C18 first-anchor capture may consume the request
in the same frame, which performs the identical capture; on exit no seat is
published, so `ApplyHeadLook` consumes it and captures at the settled moment.

The sampler is log-free by contract, so it increments a counter and the 50 ms
worker emits `H3 vehicle: play space re-centred on a settled seat entry/exit`
with the running total.

**Status: C23 candidate — NOT HEADSET ACCEPTED.**

## C24 — the maintainer's tuned vehicle configuration becomes the default

C23 was tested on the **Microsoft Store / Game Pass** edition and accepted
("works great"). That session's tuning is the product's tuning, per the policy
in force since Alpha 0.3.0: the shipped ZIP carries the maintainer's own live
`halomccvr.cfg`, and users are told to replace theirs.

Two things follow from that, and this candidate does both.

**The tuned config now lives in both editions.** The Store file
(SHA-256 `D0F12A32716385739051B5DF45AF634FB5AE9B3C1B2EC1DA33F0219DD48392E5`)
was copied verbatim over the Steam one; both editions now hash identically, so
testing continues on Steam from exactly the state that was accepted. The
previous Steam config is preserved under
`out/deploy-backups/cfg-steam-before-gamepass-copy-20260801`. Note it was the
larger of the two and carried per-seat trims the Store file does not — those
were superseded by the Game Pass pass, not lost by accident.

**The tuning is also baked into the built-in defaults.** Config parsing performs
no migration, so any key a user's older file lacks silently falls back to the
compiled default rather than to the shipped value — the exact trap
`fit_desktop_window` fell into before 0.3.0. Leaving the tuning only in the
config file would mean a retained old file quietly reverts to untuned vehicles.

- `vehicle_view_follow` now defaults to **false**. The maintainer drove every
  seat and turned it off; it remains available in the config and in F1.
- `kConfigShippedSeatTrims` carries the tuned per-seat placements — scorpion
  driver, warthog driver, warthog passenger, prowler gunner, hornet driver and
  chopper driver — and `Config::Config` stamps them into the per-seat arrays.

Only the axes actually moved are marked set, so every other axis of those seats
still follows the universal trim and still moves with it. A vehicle nobody
tuned keeps a completely bare slot. An entry naming a seat the slot table
cannot key is skipped rather than written elsewhere, so a future enum change
cannot silently land one vehicle's trim on another.

Test coverage was strengthened rather than relaxed. The existing per-seat
override test kept its meaning by moving its universal-fallback assertion onto
the **mongoose**, which no shipped trim touches, and it now also proves that a
config value beats a shipped seat default (the warthog passenger's lateral trim
ships at -0.10 and the test file sets -0.27). A new check asserts the shipped
table lands on the right slots, leaves untouched axes following the universal
trim, leaves untuned vehicles bare, and that the follow ships off.

**Status: C24 candidate — NOT HEADSET ACCEPTED.** C23's behavior was accepted on
the Store edition; this candidate changes defaults and configuration only.

## C25 - optional roll-stable pitch follow and wheel-active VR turn

This candidate starts from source `be5abcf79d2fd5dc93be03db14c58cc5a977ab84`
(the baseline named by the user) and bundles the two related vehicle-camera
changes the user explicitly requested for the next build. They share one
orientation/control contract, but remain separately observable in the headset.

### The existing checkbox remains the only follow switch

`vehicle_view_follow` still defaults to **false**, still round-trips through the
same config key, and is still the F1 `View follows the vehicle` checkbox. OFF
does not consume the C25 publication and reaches the established world-locked
camera/controller/aim math. A missing, stale (over 100 ms), cross-generation,
non-finite, or torn pitch publication fails only the new pitch component back
to the accepted yaw-only follow; it cannot disarm the Halo 3 camera core. The
50 ms worker logs Active, intentional YawOnly, and YawOnlyFallback transitions.

ON now follows the rendered seat node's **yaw and pitch**, but deliberately not
its roll. The live `rawFwd` vector already used by the accepted yaw path gives
pitch as `atan2(z, length(xy))`. Suspension bank, a side impact, or a vehicle
roll is therefore not copied into the player's horizon. Headset roll still
works. This ground-vehicle candidate makes no new promise across a ground
vehicle's extreme end-over-end projected-yaw singularity.

The order is `Rvehicle(yaw,pitch,roll=0) * Rlocal(tracked pose)`, where local yaw
contains the existing smooth/snap-turn offset. This order matters: on an uphill
vehicle, looking 90 degrees sideways looks across the vehicle rather than
remaining pitched uphill in every world direction. The same roll-free frame is
used by the camera, both controller bases, head/controller room translation,
and the parallax-corrected aim ray so every consumer uses the same coordinate
contract. A controller pose reuses one exact publication for orientation and
translation, and pitched frames cannot train the persistent gravity estimator.

Every Banshee and Hornet seat remains on proven yaw-only follow. Their normal
flight crosses vertical, where forward alone cannot choose a continuous
roll-free up vector; driver pitch is also vertical aim feedback, so adding it
would cancel feedback and turn a held hand angle into continuous climb/dive.
A transported aircraft frame/yoke author is a separate behavior and candidate.

### Smooth turn resumes only when the wheel owns steering

In a look-steered ground driver seat, the right stick remains Halo's steering
fallback while the virtual wheel is released, so consuming it for VR turn would
still make one input both turn the view and swerve. Once the two-hand wheel is
actively publishing steering, the stick is free and `ApplyVrTurn` resumes the
user's existing `turn_smooth` or snap setting. Its performance-counter timestamp
now advances even while steering owns the stick, preventing a one-frame turn
jump when the wheel is taken. Releasing the wheel restores stick steering and
suppresses VR turn again.

Unit coverage fixes the hill composition order, invalid-sample fallback, full
aircraft-family pitch exclusion, snap takeover re-arming, and the four-state
steering/wheel turn-ownership truth table.

**Status: C25 candidate - user-stated working, not formally accepted.**

2026-08-03: while approving the ODST vehicle plan the user stated "view follow
works" and directed development to continue on this branch. The last build the
user ran is source `1d6dcb6d6a15a8d3664cf4a0d77c7fcb6b36b5b7` (contains C25;
DLL SHA-256 `B5B5CAC121BB934213DB82536CB3580F1A5592C58466B5495227A10ABF22A369`,
Steam edition, VirtualDesktopXR 1.0.10, Quest 3, session 2026-08-02 15:07).
Evidence caveat, recorded so this is never mistaken for a component-level
acceptance: that session's log shows a vehicle entry but no
`H3 vehicle pitch follow:` transition, no preserved log (live, `.prev`, or any
`out/deploy-backups` capture) contains one either, and the live config ends
with `vehicle_view_follow = 0`. The statement therefore confirms the follow
experience the user has exercised, but no logged run demonstrates the C25
pitch component specifically. The Microsoft Store edition's live log
(`N:\XBOX\Halo- The Master Chief Collection\Content\Halo_MCC_VR\halo3xr.log`,
same `1d6dcb6` build, session 2026-08-02 14:56) and its `.prev` were also
checked: no pitch-follow transition there either. Treat the C25 pitch-follow
and wheel-turn behaviors as user-endorsed to build on, and fold their formal
confirmation into the next headset session that runs with the follow toggle
on.
