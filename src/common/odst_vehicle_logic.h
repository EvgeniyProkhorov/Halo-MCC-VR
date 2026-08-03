#pragma once

#include <cmath>
#include <cstdint>

// Halo 3: ODST vehicle-state logic. Like halo3_vehicle_logic.h this file
// contains no Windows, MinHook, logging or engine access: it is constants and
// pure functions only, so the unit tests can exercise every decision without a
// game.
//
// EVERY constant here is ODST-specific and was derived from the pinned
// halo3odst.dll (SHA-256 5BB20976...829A) and H3ODSTEK, then independently
// re-derived by an adversarial verification pass. See
// docs/ODST-VEHICLE-EVIDENCE.md O-E2. Halo 3's values are NOT reusable: ODST
// grew _object_definition 0xF8 -> 0x108 and its unit block 0x1C4 -> 0x1D4, so
// the whole layout sits +0x20 further out, and the object-table TLS slot moved
// independently of that shift. The Halo 3 byte patterns match ZERO times in
// halo3odst.dll (and these match zero times in halo3.dll), which is what makes
// a mis-bound title fail loud instead of reading garbage.

// Expected RVAs in the pinned module. Logging and cross-check only: the
// runtime binds by unique signature and never ships a hardcoded address.
inline constexpr uintptr_t kOdstUnitInVehicleNativeRva = 0x3C54CC;
inline constexpr uintptr_t kOdstPlayerUnitGetterRva = 0x109260;
inline constexpr uintptr_t kOdstVehicleTypeAccessorRva = 0x3DB46C;
inline constexpr uintptr_t kOdstEngineTlsIndexRva = 0xA8FB1C;
inline constexpr uintptr_t kOdstInterpolatedNodesRva = 0x1B3938;
inline constexpr uintptr_t kOdstMarkersInternalRva = 0x37F514;

// Engine TLS block -> object table. ODST uses +0x20 where Halo 3 uses +0x38;
// this is not part of the +0x20 definition shift, it is an independent
// difference (proven twice: unit_in_vehicle at 0x3C54F5 and
// object_get_ultimate_parent at 0x38234B both `mov r32, 0x20`).
inline constexpr size_t kOdstTlsObjectTableOffset = 0x20;
// The object table itself is laid out exactly as Halo 3's.
inline constexpr size_t kOdstObjectTableEntriesOffset = 0x48;
inline constexpr size_t kOdstObjectEntryStride = 0x18;
inline constexpr size_t kOdstObjectEntryDataOffset = 0x10;
inline constexpr size_t kOdstObjectEntryKindOffset = 3;
inline constexpr unsigned kOdstObjectKindVehicle = 1;
inline constexpr size_t kOdstObjectParentOffset = 0x10;
// Unit seat word: 0xFFFF (read as int16 -1) when the unit is not seated.
// Halo 3 reads +0x24E; ODST's own seat-mode chooser at 0x1535D3 reads +0x262.
inline constexpr size_t kOdstUnitSeatWordOffset = 0x262;

// Live interpolated node bank on the object. Halo 3's cluster sits at
// +0x132/+0x134/+0x136/+0x138; ODST's is the same cluster shifted -0xC.
inline constexpr size_t kOdstObjectOrientationBankSizeOffset = 0x126;
inline constexpr size_t kOdstObjectOrientationBankRelOffset = 0x128;
inline constexpr size_t kOdstObjectNodeBankSizeOffset = 0x12A;
inline constexpr size_t kOdstObjectNodeBankRelOffset = 0x12C;
// Node matrices are the same 0x34-byte real_matrix4x3 Halo 3 uses, so
// Halo3Matrix4x3 and its pure transform helpers are shared, not duplicated.
inline constexpr size_t kOdstNodeMatrixStride = 0x34;

// The object table is the engine's generic 0x60-byte "data array" header,
// created as data_new_full("object", 0x800, 0x18). Every field below was
// proven from data_initialize/datum_new/datum_delete and is byte-identical in
// Halo 3 — only the TLS slot that reaches the table differs between titles.
// This is what lets a probe enumerate EVERY live object (including AI-crewed
// and parked vehicles) instead of only the one the player is handle-linked to.
inline constexpr size_t kOdstDataArrayNameOffset = 0x00;
inline constexpr size_t kOdstDataArrayNameSize = 0x20;
inline constexpr size_t kOdstDataArrayMaxCountOffset = 0x20;
inline constexpr size_t kOdstDataArrayElementSizeOffset = 0x24;
inline constexpr size_t kOdstDataArrayValidOffset = 0x29;
inline constexpr size_t kOdstDataArraySignatureOffset = 0x2C;
// Iteration bound: the high-water mark. Slots below it may still be holes, so
// a walk must skip them; actual_count is NEVER a valid bound.
inline constexpr size_t kOdstDataArrayFirstUnallocatedOffset = 0x3C;
inline constexpr size_t kOdstDataArrayActualCountOffset = 0x40;
inline constexpr size_t kOdstDataArrayElementsOffset = 0x48;
inline constexpr uint32_t kOdstDataArraySignature = 0x64407440;  // 'd@t@'
inline constexpr uint32_t kOdstObjectTableCapacity = 0x800;
// An entry's identifier word. Zero means the slot is free; live identifiers
// are always >= 0x8000 (the allocator wraps 0xFFFF back to 0x8000), and a
// handle is (identifier << 16) | index. 0xFFFF is NOT a free sentinel here.
inline constexpr size_t kOdstObjectEntryIdentifierOffset = 0x00;

// Everything a probe must confirm before walking the table. Pure so the rule
// is unit-testable: a header that fails any part of this is not the object
// table this build expects, and the walk must not happen at all.
struct OdstDataArrayHeaderView
{
    bool nameIsObject = false;
    uint32_t signature = 0;
    uint32_t maximumCount = 0;
    uint32_t elementSize = 0;
    uint32_t firstUnallocated = 0;
    uint8_t valid = 0;
};

inline bool OdstObjectTableIsWalkable(const OdstDataArrayHeaderView& header)
{
    if (!header.nameIsObject)
        return false;
    if (header.signature != kOdstDataArraySignature)
        return false;
    if (header.elementSize != kOdstObjectEntryStride)
        return false;
    if (header.maximumCount != kOdstObjectTableCapacity)
        return false;
    if (header.valid != 1)
        return false;
    return header.firstUnallocated <= header.maximumCount;
}

inline bool OdstObjectEntryIsLive(uint16_t identifier)
{
    return identifier != 0;
}

// Loaded vehicle definition. Seat stride and flag bit order are identical to
// Halo 3; the block offsets are not.
inline constexpr size_t kOdstVehicleSeatsCountOffset = 0x278;
inline constexpr size_t kOdstVehicleSeatsDataOffset = 0x27C;
inline constexpr size_t kOdstVehicleSeatStride = 0xD4;
inline constexpr size_t kOdstSeatFlagsOffset = 0x00;
inline constexpr uint32_t kOdstSeatThirdPersonCameraBit = 1u << 4;
inline constexpr uint32_t kOdstSeatDriverBit = 1u << 2;
inline constexpr uint32_t kOdstSeatGunnerBit = 1u << 3;
inline constexpr uint32_t kOdstSeatInvalidForPlayerBit = 1u << 16;
inline constexpr size_t kOdstSeatCameraMarkerNameOffset = 0x6C;
// Physics-type blocks: 10 records of 0xC. The accessor returns the first
// non-empty index, or 0xB when the definition authors none (walk-up turrets
// and the shade both author none).
inline constexpr size_t kOdstVehiclePhysicsBlocksOffset = 0x2E0;
inline constexpr size_t kOdstVehiclePhysicsRecordStride = 0xC;
inline constexpr int kOdstPhysicsTypeCount = 10;
inline constexpr int kOdstPhysicsTypeNoneAuthored = 0xB;
inline constexpr int kOdstPhysicsTypeTurret = 5;
// Discriminator fields inside those records (kit-metadata derived; the O1
// probe confirms them against a live loaded definition).
inline constexpr size_t kOdstJeepEngineMomentOffset = 0x14;
inline constexpr size_t kOdstScoutSpecificTypeOffset = 0x28;

// Loaded-tag chain to the render model. Identical to Halo 3.
inline constexpr size_t kOdstVehicleModelRefOffset = 0x40;
inline constexpr size_t kOdstModelRenderRefOffset = 0x0C;
inline constexpr size_t kOdstRenderModelNodesCountOffset = 0x30;
inline constexpr size_t kOdstRenderModelNodesDataOffset = 0x34;
inline constexpr size_t kOdstRenderModelNodeStride = 0x60;
inline constexpr size_t kOdstRenderModelNodeInverseOffset = 0x28;

// Occupant head marker. Independently derived from ODST's own static string-id
// table (base RVA 0x7C4E80, record #159), NOT carried over from Halo 3 —
// built-in ids demonstrably drift between the titles past id 0xC5. It happens
// to land on the same value.
inline constexpr uint32_t kOdstHeadMarkerStringId = 0x9F;

// Seat NAMES (driver/passenger/gunner/camera) are absent from the static
// string-id table entirely: they are tag-content ids registered per map cache,
// so they can never be compile-time constants. Seats are keyed by index.

enum class OdstVehicleBindingState : uint8_t
{
    NotInstalled = 0,
    Installed,
    StockFallback,
};

// What the probe observed about one object-table entry. Pure data so the
// classification below is unit-testable without an engine.
struct OdstProbeVehicleRecord
{
    bool dataValid = false;
    uint32_t definitionIndex = 0xFFFFFFFFu;
    int physicsType = -1;
    // Node bank as read from the object.
    int nodeBankByteSize = -1;
    int nodeBankRelOffset = 0;
    // Nodes the render model declares for this definition.
    int tagNodeCount = -1;
};

// A node bank is trustworthy only when its byte size is a positive whole
// number of 0x34-byte matrices AND that count matches the render model's own
// node count. Anything else means the offsets are wrong for this title and the
// runtime must stay stock rather than transform a garbage matrix.
inline bool OdstNodeBankIsCoherent(const OdstProbeVehicleRecord& record)
{
    if (record.nodeBankByteSize <= 0)
        return false;
    if (static_cast<size_t>(record.nodeBankByteSize) % kOdstNodeMatrixStride)
        return false;
    const int matrices =
        record.nodeBankByteSize / static_cast<int>(kOdstNodeMatrixStride);
    if (matrices <= 0 || matrices > 255)
        return false;
    if (record.tagNodeCount <= 0)
        return false;
    return matrices == record.tagNodeCount;
}

// Halo 3's identities, reused where ODST ships the same vehicle, plus the two
// ODST additions. Kept as a separate enum so a future Halo 3 enum change can
// never silently re-key ODST seats.
enum class OdstVehicleId : uint8_t
{
    Unknown = 0,
    Scorpion,
    Warthog,
    Mongoose,
    Ghost,
    Wraith,
    Mauler,
    Banshee,
    Hornet,
    Chopper,
    StationaryTurret,
    // ODST additions. The shade is a real vehicle the player drives (5
    // missions); Halo 3's campaign never let the mod meet one.
    Shade,
};

struct OdstDefinitionFields
{
    int physicsType = -1;
    bool jeepValid = false;
    float engineMoment = 0.0f;
    bool scoutValid = false;
    int specificType = -1;
    // Seat 0's flag word, used only to separate the shade (a driven vehicle)
    // from a walk-up turret: both author zero physics blocks.
    bool seatFlagsValid = false;
    uint32_t seat0Flags = 0;
    // The native predicate's answer for the occupant. A walk-up turret reports
    // 0; a real vehicle reports 1.
    bool inVehicle = false;
};

inline constexpr float kOdstWarthogEngineMoment = 2000.0f;
inline constexpr float kOdstMongooseEngineMoment = 650.0f;
inline constexpr float kOdstEngineMomentTolerance = 200.0f;

// Identity resolution. Physics types and discriminator semantics were proven
// from ODST's own accessor and tag exports; the values coincide with Halo 3's
// because both titles ship the same authored vehicles, not because Halo 3's
// were assumed.
//
// The shade/turret split is the one ODST-specific rule: neither authors a
// physics block (accessor returns 0xB, or 5 for the explicit turret type), so
// the seat's own driver bit separates a driven shade from a walk-up gun.
inline OdstVehicleId OdstResolveVehicleId(const OdstDefinitionFields& def)
{
    switch (def.physicsType)
    {
    case 0: return OdstVehicleId::Scorpion;      // human_tank
    case 4: return OdstVehicleId::Banshee;       // alien_fighter
    case 7: return OdstVehicleId::Hornet;        // vtol
    case 8: return OdstVehicleId::Chopper;       // chopper
    case 1:                                       // human_jeep
        if (!def.jeepValid || !std::isfinite(def.engineMoment))
            return OdstVehicleId::Unknown;
        if (std::fabs(def.engineMoment - kOdstWarthogEngineMoment) <=
            kOdstEngineMomentTolerance)
            return OdstVehicleId::Warthog;
        if (std::fabs(def.engineMoment - kOdstMongooseEngineMoment) <=
            kOdstEngineMomentTolerance)
            return OdstVehicleId::Mongoose;
        return OdstVehicleId::Unknown;
    case 3:                                       // alien_scout
        if (!def.scoutValid)
            return OdstVehicleId::Unknown;
        if (def.specificType == 1) return OdstVehicleId::Ghost;
        if (def.specificType == 3) return OdstVehicleId::Wraith;
        if (def.specificType == 4) return OdstVehicleId::Mauler;
        return OdstVehicleId::Unknown;
    case kOdstPhysicsTypeTurret:                  // explicit turret
    case kOdstPhysicsTypeNoneAuthored:            // nothing authored
        if (!def.seatFlagsValid)
            return OdstVehicleId::Unknown;
        // A driven vehicle: seat 0 is a driver seat and the engine agrees the
        // occupant is in a vehicle. Walk-up turrets satisfy neither.
        if ((def.seat0Flags & kOdstSeatDriverBit) && def.inVehicle)
            return OdstVehicleId::Shade;
        if (!(def.seat0Flags & kOdstSeatDriverBit))
            return OdstVehicleId::StationaryTurret;
        return OdstVehicleId::Unknown;
    default:
        return OdstVehicleId::Unknown;
    }
}

inline const char* OdstVehicleIdName(OdstVehicleId id)
{
    switch (id)
    {
    case OdstVehicleId::Scorpion: return "scorpion";
    case OdstVehicleId::Warthog: return "warthog";
    case OdstVehicleId::Mongoose: return "mongoose";
    case OdstVehicleId::Ghost: return "ghost";
    case OdstVehicleId::Wraith: return "wraith";
    case OdstVehicleId::Mauler: return "prowler";
    case OdstVehicleId::Banshee: return "banshee";
    case OdstVehicleId::Hornet: return "hornet";
    case OdstVehicleId::Chopper: return "chopper";
    case OdstVehicleId::StationaryTurret: return "turret";
    case OdstVehicleId::Shade: return "shade";
    case OdstVehicleId::Unknown: break;
    }
    return "unknown";
}

// A seat word of -1 (0xFFFF) means on foot. The probe uses this as its
// negative control: if the offset were wrong for this title, an on-foot player
// would not read exactly -1 frame after frame.
inline bool OdstSeatWordMeansUnseated(int seatWord)
{
    return seatWord == -1;
}
