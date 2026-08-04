#pragma once

#include <array>
#include <cmath>
#include <cstdint>

// Reach vehicle identities are an explicit contract shared by the runtime,
// per-seat config bank and Blender authoring kit. The order mirrors the
// official HREK tag census; zero always means that identity could not be
// proven, in which case the optional vehicle feature stays stock.
enum class ReachVehicleId : uint8_t
{
    Unknown = 0,
    Banshee,
    SpaceBanshee,
    Ghost,
    Revenant,
    Wraith,
    WraithGunner,
    Mongoose,
    Warthog,
    WarthogChaingun,
    WarthogGauss,
    WarthogRocket,
    Falcon,
    Sabre,
    Scorpion,
    Forklift,
    Cart,
    ShadePlasma,
    ShadeFlak,
    PlasmaTurret,
    Machinegun,
};

inline constexpr int kReachVehicleIdentityCount =
    static_cast<int>(ReachVehicleId::Machinegun);

// Reach's vehicle postprocess retains a positive vehicle-authored bounding
// sphere, otherwise it copies the referenced model's exact runtime sphere into
// these four early vehicle-definition fields. The values below follow that
// official HREK branch; each complete tuple is unique and compares bit-for-bit.
// Unknown or modified content keeps only this optional feature stock.
struct ReachVehicleFingerprint
{
    uint32_t radius = 0;
    uint32_t offsetX = 0;
    uint32_t offsetY = 0;
    uint32_t offsetZ = 0;
};

struct ReachVehicleFingerprintEntry
{
    ReachVehicleId identity = ReachVehicleId::Unknown;
    ReachVehicleFingerprint fingerprint{};
};

inline constexpr std::array<ReachVehicleFingerprintEntry,
                            kReachVehicleIdentityCount>
    kReachVehicleFingerprints{{
        {ReachVehicleId::Banshee,
         {0x3FE3DDB8, 0xBEAA6EFF, 0xBAE51357, 0x3F13532A}},
        {ReachVehicleId::SpaceBanshee,
         {0x3FB33333, 0x00000000, 0x00000000, 0x00000000}},
        {ReachVehicleId::Ghost,
         {0x3F7C19A7, 0xBCD7C712, 0x38612D0C, 0x3E1972D5}},
        {ReachVehicleId::Revenant,
         {0x3F59999A, 0xBDCCCCCD, 0x00000000, 0xBDCCCCCD}},
        {ReachVehicleId::Wraith,
         {0x3FE66666, 0x00000000, 0x00000000, 0x00000000}},
        {ReachVehicleId::WraithGunner,
         {0x3F4CCCCD, 0x00000000, 0x00000000, 0x3F000000}},
        {ReachVehicleId::Mongoose,
         {0x3F2ABABB, 0x39815C03, 0x39CE6FFD, 0x3E7776E0}},
        {ReachVehicleId::Warthog,
         {0x3F978072, 0xBD406A82, 0xBAD03632, 0x3EC25783}},
        {ReachVehicleId::WarthogChaingun,
         {0x3F1CC397, 0x3E86AFD5, 0xB3A6AFD5, 0x3E9C5885}},
        {ReachVehicleId::WarthogGauss,
         {0x3F28C016, 0x3E8E27FC, 0xBC86EDEB, 0x3EA9A1F3}},
        {ReachVehicleId::WarthogRocket,
         {0x3F12852A, 0x3DC5E04F, 0x3BE1BCFB, 0x3EA9708D}},
        {ReachVehicleId::Falcon,
         {0x4037DF8F, 0xBE781BAB, 0x363BE06E, 0x3F2D5046}},
        {ReachVehicleId::Sabre,
         {0x40A69CFE, 0xBF19A963, 0xBB2A0199, 0x3F8AF1EF}},
        {ReachVehicleId::Scorpion,
         {0x400D30E2, 0x3D70D852, 0x3D28430C, 0x3EBBF788}},
        {ReachVehicleId::Forklift,
         {0x3F971D82, 0x3E707E5B, 0xBC9C748D, 0x3EF3BE41}},
        {ReachVehicleId::Cart,
         {0x3F786DCF, 0x3C8C975D, 0xBC8111A2, 0x3EE5C245}},
        {ReachVehicleId::ShadePlasma,
         {0x3F7A9153, 0x3E1B9697, 0xBC935063, 0x3F1EF5FC}},
        {ReachVehicleId::ShadeFlak,
         {0x3F73045B, 0x3DD8D24F, 0xBC93505D, 0x3F1EA4E9}},
        {ReachVehicleId::PlasmaTurret,
         {0x3F000000, 0x00000000, 0x00000000, 0x3DCCCCCD}},
        {ReachVehicleId::Machinegun,
         {0x3EE99B18, 0x3D9811F2, 0xB9914460, 0x3E8589DB}},
    }};

inline constexpr bool ReachVehicleFingerprintEqual(
    const ReachVehicleFingerprint& a, const ReachVehicleFingerprint& b)
{
    return a.radius == b.radius && a.offsetX == b.offsetX &&
        a.offsetY == b.offsetY && a.offsetZ == b.offsetZ;
}

inline constexpr ReachVehicleId ReachResolveVehicleFingerprint(
    const ReachVehicleFingerprint& fingerprint)
{
    ReachVehicleId result = ReachVehicleId::Unknown;
    for (const auto& entry : kReachVehicleFingerprints)
    {
        if (!ReachVehicleFingerprintEqual(entry.fingerprint, fingerprint))
            continue;
        if (result != ReachVehicleId::Unknown)
            return ReachVehicleId::Unknown;
        result = entry.identity;
    }
    return result;
}

inline constexpr uintptr_t kReachVehicleBoundsRadiusOffset = 0x0C;
inline constexpr uintptr_t kReachVehicleBoundsOffsetX = 0x10;
inline constexpr uintptr_t kReachVehicleBoundsOffsetY = 0x14;
inline constexpr uintptr_t kReachVehicleBoundsOffsetZ = 0x18;
inline constexpr uint8_t kReachObjectKindVehicle = 1;
inline constexpr uint32_t kReachVehicleGroupTag = 0x76656869;

// Raw seat indices come from each Reach unit tag. Do not replace these with
// inferred roles: Falcon's player seats, for example, are 0, 3 and 4.
inline constexpr bool ReachVehicleSeatIsPlayer(ReachVehicleId id, int seat)
{
    if (seat < 0 || seat >= 16)
        return false;
    switch (id)
    {
    case ReachVehicleId::Revenant:
    case ReachVehicleId::Mongoose:
    case ReachVehicleId::Warthog:
        return seat == 0 || seat == 1;
    case ReachVehicleId::Falcon:
        return seat == 0 || seat == 3 || seat == 4;
    case ReachVehicleId::Banshee:
    case ReachVehicleId::SpaceBanshee:
    case ReachVehicleId::Ghost:
    case ReachVehicleId::Wraith:
    case ReachVehicleId::WraithGunner:
    case ReachVehicleId::WarthogChaingun:
    case ReachVehicleId::WarthogGauss:
    case ReachVehicleId::WarthogRocket:
    case ReachVehicleId::Sabre:
    case ReachVehicleId::Scorpion:
    case ReachVehicleId::Forklift:
    case ReachVehicleId::Cart:
    case ReachVehicleId::ShadePlasma:
    case ReachVehicleId::ShadeFlak:
    case ReachVehicleId::PlasmaTurret:
    case ReachVehicleId::Machinegun:
        return seat == 0;
    default:
        return false;
    }
}

inline constexpr bool ReachVehicleIsAircraft(ReachVehicleId id)
{
    return id == ReachVehicleId::Banshee ||
        id == ReachVehicleId::SpaceBanshee ||
        id == ReachVehicleId::Falcon || id == ReachVehicleId::Sabre;
}

inline constexpr bool ReachVehicleIsWalkUpTurret(ReachVehicleId id)
{
    return id == ReachVehicleId::ShadePlasma ||
        id == ReachVehicleId::ShadeFlak ||
        id == ReachVehicleId::PlasmaTurret ||
        id == ReachVehicleId::Machinegun;
}

inline constexpr bool ReachVehicleIsAttachedWeapon(ReachVehicleId id)
{
    return id == ReachVehicleId::WraithGunner ||
        id == ReachVehicleId::WarthogChaingun ||
        id == ReachVehicleId::WarthogGauss ||
        id == ReachVehicleId::WarthogRocket;
}

// Reach's official vehicle tags put the controllable seat at raw seat 0. An
// attached gun and a walk-up turret also expose seat 0, but that seat aims the
// gun instead of steering a carrier, so neither may take hull steering.
inline constexpr bool ReachVehicleSeatIsDriver(ReachVehicleId id, int seat)
{
    return seat == 0 && id != ReachVehicleId::Unknown &&
        !ReachVehicleIsAttachedWeapon(id) && !ReachVehicleIsWalkUpTurret(id) &&
        id != ReachVehicleId::WraithGunner;
}

// HREK's Reach vehicle definitions divide the same way the native controller
// path does: these drivers turn their carrier through the view/turn channel.
// Wraith and Scorpion keep an independently aimed turret and therefore retain
// the closed-loop hand aim while their hull follows the move stick.
inline constexpr bool ReachVehicleIsLookSteered(ReachVehicleId id)
{
    switch (id)
    {
    case ReachVehicleId::Banshee:
    case ReachVehicleId::SpaceBanshee:
    case ReachVehicleId::Ghost:
    case ReachVehicleId::Revenant:
    case ReachVehicleId::Mongoose:
    case ReachVehicleId::Warthog:
    case ReachVehicleId::Falcon:
    case ReachVehicleId::Sabre:
    case ReachVehicleId::Forklift:
    case ReachVehicleId::Cart:
        return true;
    default:
        return false;
    }
}

inline constexpr bool ReachVehicleSeatFollowsHull(
    ReachVehicleId id, int seat)
{
    return ReachVehicleSeatIsPlayer(id, seat) &&
        !ReachVehicleIsWalkUpTurret(id);
}

inline constexpr bool ReachVehicleSeatFollowsPitch(
    ReachVehicleId id, int seat)
{
    return ReachVehicleSeatFollowsHull(id, seat) &&
        !ReachVehicleIsAircraft(id);
}

inline constexpr bool ReachVehicleSeatAuthorsSteering(
    ReachVehicleId id, int seat, bool followEnabled)
{
    return followEnabled && ReachVehicleSeatIsDriver(id, seat) &&
        ReachVehicleIsLookSteered(id);
}

inline constexpr bool ReachVehicleUsesWheel(ReachVehicleId id)
{
    return ReachVehicleIsLookSteered(id) && !ReachVehicleIsAircraft(id);
}

inline constexpr int ReachVehicleExpectedPhysicsType(ReachVehicleId id)
{
    switch (id)
    {
    case ReachVehicleId::Scorpion: return 0;
    case ReachVehicleId::Mongoose:
    case ReachVehicleId::Warthog:
    case ReachVehicleId::Forklift:
    case ReachVehicleId::Cart: return 1;
    case ReachVehicleId::Ghost:
    case ReachVehicleId::Revenant:
    case ReachVehicleId::Wraith: return 4;
    case ReachVehicleId::Banshee:
    case ReachVehicleId::SpaceBanshee: return 5;
    case ReachVehicleId::Falcon: return 8;
    case ReachVehicleId::Sabre: return 13;
    case ReachVehicleId::WraithGunner:
    case ReachVehicleId::WarthogChaingun:
    case ReachVehicleId::WarthogGauss:
    case ReachVehicleId::WarthogRocket:
    case ReachVehicleId::ShadePlasma:
    case ReachVehicleId::ShadeFlak:
    case ReachVehicleId::PlasmaTurret:
    case ReachVehicleId::Machinegun: return 16;
    default: return -1;
    }
}

inline constexpr uint32_t kReachSeatThirdPersonCameraBit = 1u << 4;
inline constexpr int kReachVehicleSeatLimit = 16;

// HREK's generic datum collection and tag-block layouts, matched to the pinned
// retail homologs. These bounds are consumed only from Reach's proven render
// thread; any failed validation leaves the optional vehicle frame stock.
inline constexpr uintptr_t kReachTlsObjectCollectionOffset = 0x10;
inline constexpr uintptr_t kReachDataCollectionStrideOffset = 0x20;
inline constexpr uintptr_t kReachDataCollectionInitializedOffset = 0x31;
inline constexpr uintptr_t kReachDataCollectionCountOffset = 0x44;
inline constexpr uintptr_t kReachDataCollectionEntriesOffset = 0x50;
inline constexpr size_t kReachObjectEntryStride = 0x18;
inline constexpr uintptr_t kReachObjectEntrySaltOffset = 0x00;
inline constexpr uintptr_t kReachObjectEntryKindOffset = 0x04;
inline constexpr uintptr_t kReachObjectEntryDataOffset = 0x10;
inline constexpr uintptr_t kReachObjectDefinitionDatumOffset = 0x00;
inline constexpr uintptr_t kReachVehicleSeatsBlockOffset = 0x444;
inline constexpr size_t kReachVehicleSeatStride = 0x12C;
inline constexpr uintptr_t kReachSeatFlagsOffset = 0x00;
inline constexpr uintptr_t kReachSeatAttachmentMarkerOffset = 0x08;
inline constexpr uintptr_t kReachSeatCameraOffset = 0x70;
inline constexpr uintptr_t kReachSeatCameraMarkerOffset = 0x74;
inline constexpr uint32_t kReachOccupantHeadMarkerStringId = 0xC2;

struct ReachSeatCameraBasis
{
    float scale = 1.0f;
    float forward[3]{};
    float left[3]{};
    float up[3]{};
};

inline bool ReachSeatCameraBasisValid(const ReachSeatCameraBasis& basis)
{
    if (!std::isfinite(basis.scale) || basis.scale <= 1.0e-5f ||
        basis.scale > 1.0e3f)
    {
        return false;
    }
    for (const float* axis : {basis.forward, basis.left, basis.up})
    {
        float lengthSquared = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            if (!std::isfinite(axis[i]))
                return false;
            lengthSquared += axis[i] * axis[i];
        }
        if (!std::isfinite(lengthSquared) || lengthSquared < 0.25f ||
            lengthSquared > 4.0f)
            return false;
    }
    return true;
}

// Config axes match the authoring kit: +forward, +right, +up. Reach exposes a
// left axis in its real_matrix4x3, so right is the negated left column.
inline bool ReachComposeSeatCameraPoint(
    const float base[3], const ReachSeatCameraBasis& basis,
    float forwardUnits, float rightUnits, float upUnits, float out[3])
{
    if (!base || !out || !ReachSeatCameraBasisValid(basis) ||
        !std::isfinite(forwardUnits) || !std::isfinite(rightUnits) ||
        !std::isfinite(upUnits))
        return false;
    for (int i = 0; i < 3; ++i)
    {
        if (!std::isfinite(base[i]))
            return false;
        out[i] = base[i] + basis.scale *
            (basis.forward[i] * forwardUnits -
             basis.left[i] * rightUnits + basis.up[i] * upUnits);
        if (!std::isfinite(out[i]))
            return false;
    }
    return true;
}

// Menu readers need only identity and raw seat; render-thread transforms stay
// in their bounded same-thread transaction. The generation in the upper half
// prevents a stale Reach module from naming a seat after title teardown.
inline constexpr uint64_t ReachVehicleTrimSnapshot(
    uint32_t generation, ReachVehicleId id, int seat)
{
    return generation && id != ReachVehicleId::Unknown && seat >= 0 && seat < 16
        ? (static_cast<uint64_t>(generation) << 32) |
              (static_cast<uint64_t>(static_cast<uint8_t>(id)) << 8) |
              static_cast<uint8_t>(seat + 1)
        : 0;
}

inline constexpr int ReachVehicleTrimSnapshotSlot(
    uint64_t snapshot, uint32_t generation, int seatsPerVehicle)
{
    if (!generation || static_cast<uint32_t>(snapshot >> 32) != generation ||
        seatsPerVehicle <= 0)
        return -1;
    const int id = static_cast<int>((snapshot >> 8) & 0xFFu);
    const int seat = static_cast<int>(snapshot & 0xFFu) - 1;
    if (id <= 0 || id > kReachVehicleIdentityCount || seat < 0 ||
        seat >= seatsPerVehicle)
        return -1;
    return (id - 1) * seatsPerVehicle + seat;
}
