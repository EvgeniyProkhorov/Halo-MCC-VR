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
