#pragma once

#include <array>
#include <cmath>
#include <cstdint>

// Reach vehicle identities are an explicit contract shared by the runtime,
// per-seat config bank and Blender authoring kit. The order mirrors the
// official HREK tag census; zero always means that identity could not be
// proven, in which case the seat camera runs generically on universal trims.
// The first 20 rows are the original Blender-authored lineup; the rows from
// MacCannon on are the 2026-08-04 full-census sweep of every official HREK
// .vehicle tag (out/hrek-evidence/reach-vehicle-census/), whose pipeline
// reproduced all 20 known tuples, types and the 25-seat census bit-exactly
// before any new row was trusted. Aliases resolve to one row on purpose:
// the four falcon side turrets author plasma_turret's exact tuple+type,
// shade_anti_air_cannon authors shade_flak's, seraph covers
// seraph_in_atmosphere, and no row exists for tags whose every seat is
// authored invalid-for-player (phantoms, troop-hog bed, AI emplacements).
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
    MacCannon,
    ScorpionAntiInfantry,
    Seraph,
    Pelican,
    PelicanChinGun,
    CorvetteCannon,
    SpacePhantomChinGun,
    SpacePhantomBeamTurret,
    CargoTruck,
    MilitaryTruck,
    OniVan,
    Pickup,
    TruckCabLarge,
    SquadDropPod,
};

inline constexpr int kReachVehicleIdentityCount =
    static_cast<int>(ReachVehicleId::SquadDropPod);

// The fingerprint table carries MORE rows than identities: retail-compiled
// maps hold model bounding spheres from a different HREK import generation
// than the shipped source tags, and for the Warthog (radius) and Mongoose
// (offsetX) the recomputed sphere landed exactly one ULP away. HREK's own
// test_fbx_mongoose re-import of identical geometry authors the retail
// Mongoose word bit-for-bit, proving the last bit is import rounding noise,
// not different content. Each retail tuple is a full-tuple alias of its
// identity row; the physics-type gate still has to agree.
inline constexpr int kReachVehicleFingerprintCount =
    kReachVehicleIdentityCount + 2;

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
                            kReachVehicleFingerprintCount>
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
        {ReachVehicleId::MacCannon,
         {0x41200000, 0x00000000, 0x00000000, 0x00000000}},
        {ReachVehicleId::ScorpionAntiInfantry,
         {0x3ED28405, 0x3DB8AD5D, 0xB8A6B78A, 0x3D8A1BD8}},
        {ReachVehicleId::Seraph,
         {0x410F9AAB, 0xC02DAE74, 0xB625B5D0, 0x3F8295A1}},
        {ReachVehicleId::Pelican,
         {0x40B33333, 0x00000000, 0x00000000, 0x00000000}},
        {ReachVehicleId::PelicanChinGun,
         {0x3F2BC474, 0x3EC52CD6, 0xB3C52CD6, 0xBE7D391F}},
        {ReachVehicleId::CorvetteCannon,
         {0x40400000, 0x00000000, 0x00000000, 0x00000000}},
        {ReachVehicleId::SpacePhantomChinGun,
         {0x3F666666, 0x3F666666, 0x00000000, 0x00000000}},
        {ReachVehicleId::SpacePhantomBeamTurret,
         {0x3F906FE2, 0x3F8275C2, 0x374C2C51, 0x3BA3D9C0}},
        {ReachVehicleId::CargoTruck,
         {0x3FAE222F, 0x3C709BBD, 0xBC1C81E3, 0x3F19FF3B}},
        {ReachVehicleId::MilitaryTruck,
         {0x3FD4EDB9, 0xBDAC5C6F, 0x3BA35FAA, 0x3F39CE19}},
        {ReachVehicleId::OniVan,
         {0x3FD46CE8, 0xBDB33F17, 0x3BA3612C, 0x3F39C761}},
        {ReachVehicleId::Pickup,
         {0x3F9FCD09, 0xBDA6FD71, 0xB329BF5D, 0x3ECB6694}},
        {ReachVehicleId::TruckCabLarge,
         {0x3FE2557B, 0xBD4C676F, 0x3ACF7265, 0x3F6831B4}},
        {ReachVehicleId::SquadDropPod,
         {0x3FDAB04B, 0xBD31519F, 0x390792C5, 0x3F912A02}},
        // Retail-map aliases (see kReachVehicleFingerprintCount): the same
        // vehicles as their HREK rows above, one mantissa bit adrift. Logged
        // unmatched in every retail session ever recorded — 'active warthog'
        // and 'active mongoose' appear in no log before these rows existed.
        {ReachVehicleId::Warthog,
         {0x3F978071, 0xBD406A82, 0xBAD03632, 0x3EC25783}},
        {ReachVehicleId::Mongoose,
         {0x3F2ABABB, 0x39815C02, 0x39CE6FFD, 0x3E7776E0}},
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
// inferred roles: Falcon's player seats are 0, 3 and 4, and Pelican's are
// its four passenger benches - its driver seat is authored invalid-for-player.
// Since the 2026-08-04 generic-coverage change this census no longer gates
// the camera; it names which seats the official tags author player-valid so
// the worker can log an out-of-census seat precisely.
inline constexpr bool ReachVehicleSeatIsPlayer(ReachVehicleId id, int seat)
{
    if (seat < 0 || seat >= 16)
        return false;
    switch (id)
    {
    case ReachVehicleId::Revenant:
    case ReachVehicleId::Mongoose:
    case ReachVehicleId::Warthog:
    case ReachVehicleId::CargoTruck:
    case ReachVehicleId::MilitaryTruck:
    case ReachVehicleId::OniVan:
    case ReachVehicleId::Pickup:
    case ReachVehicleId::TruckCabLarge:
        return seat == 0 || seat == 1;
    case ReachVehicleId::Falcon:
        return seat == 0 || seat == 3 || seat == 4;
    case ReachVehicleId::Pelican:
        return seat == 4 || seat == 5 || seat == 9 || seat == 10;
    case ReachVehicleId::SquadDropPod:
        return seat >= 0 && seat <= 3;
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
    case ReachVehicleId::MacCannon:
    case ReachVehicleId::ScorpionAntiInfantry:
    case ReachVehicleId::Seraph:
    case ReachVehicleId::PelicanChinGun:
    case ReachVehicleId::CorvetteCannon:
    case ReachVehicleId::SpacePhantomChinGun:
    case ReachVehicleId::SpacePhantomBeamTurret:
        return seat == 0;
    default:
        return false;
    }
}

inline constexpr bool ReachVehicleIsAircraft(ReachVehicleId id)
{
    return id == ReachVehicleId::Banshee ||
        id == ReachVehicleId::SpaceBanshee ||
        id == ReachVehicleId::Falcon || id == ReachVehicleId::Sabre ||
        id == ReachVehicleId::Seraph || id == ReachVehicleId::Pelican;
}

// SquadDropPod is a scripted prop, not a turret, but it claims no hull-follow
// frame either, which is exactly this class's behavior. A walk-up tag can
// also ship mounted on a moving carrier (the falcon side guns author
// plasma_turret's exact tuple); the frame builder detects that through the
// engine's own ultimate-parent chain and grants the attached-weapon frame.
inline constexpr bool ReachVehicleIsWalkUpTurret(ReachVehicleId id)
{
    return id == ReachVehicleId::ShadePlasma ||
        id == ReachVehicleId::ShadeFlak ||
        id == ReachVehicleId::PlasmaTurret ||
        id == ReachVehicleId::Machinegun ||
        id == ReachVehicleId::MacCannon ||
        id == ReachVehicleId::CorvetteCannon ||
        id == ReachVehicleId::SquadDropPod;
}

inline constexpr bool ReachVehicleIsAttachedWeapon(ReachVehicleId id)
{
    return id == ReachVehicleId::WraithGunner ||
        id == ReachVehicleId::WarthogChaingun ||
        id == ReachVehicleId::WarthogGauss ||
        id == ReachVehicleId::WarthogRocket ||
        id == ReachVehicleId::ScorpionAntiInfantry ||
        id == ReachVehicleId::PelicanChinGun ||
        id == ReachVehicleId::SpacePhantomChinGun ||
        id == ReachVehicleId::SpacePhantomBeamTurret;
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
    case ReachVehicleId::Seraph:
    case ReachVehicleId::CargoTruck:
    case ReachVehicleId::MilitaryTruck:
    case ReachVehicleId::OniVan:
    case ReachVehicleId::Pickup:
    case ReachVehicleId::TruckCabLarge:
        return true;
    default:
        return false;
    }
}

// Hull follow is keyed on the identity's policy class, not on census
// membership: a proven identity can expose more seats than the 25-seat
// census (campaign variants), and those seats ride the same hull. Unknown
// identities claim no hull frame at all.
inline constexpr bool ReachVehicleSeatFollowsHull(
    ReachVehicleId id, int seat)
{
    return id != ReachVehicleId::Unknown && seat >= 0 && seat < 16 &&
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
    case ReachVehicleId::Cart:
    case ReachVehicleId::CargoTruck:
    case ReachVehicleId::MilitaryTruck:
    case ReachVehicleId::OniVan:
    case ReachVehicleId::Pickup:
    case ReachVehicleId::TruckCabLarge: return 1;
    case ReachVehicleId::Pelican: return 2;
    case ReachVehicleId::Ghost:
    case ReachVehicleId::Revenant:
    case ReachVehicleId::Wraith: return 4;
    case ReachVehicleId::Banshee:
    case ReachVehicleId::SpaceBanshee: return 5;
    case ReachVehicleId::CorvetteCannon: return 6;
    case ReachVehicleId::Falcon: return 8;
    case ReachVehicleId::Sabre:
    case ReachVehicleId::Seraph: return 13;
    case ReachVehicleId::WraithGunner:
    case ReachVehicleId::WarthogChaingun:
    case ReachVehicleId::WarthogGauss:
    case ReachVehicleId::WarthogRocket:
    case ReachVehicleId::ShadePlasma:
    case ReachVehicleId::ShadeFlak:
    case ReachVehicleId::PlasmaTurret:
    case ReachVehicleId::Machinegun:
    case ReachVehicleId::MacCannon:
    case ReachVehicleId::ScorpionAntiInfantry:
    case ReachVehicleId::PelicanChinGun:
    case ReachVehicleId::SpacePhantomChinGun:
    case ReachVehicleId::SpacePhantomBeamTurret:
    case ReachVehicleId::SquadDropPod: return 16;
    default: return -1;
    }
}

inline constexpr uint32_t kReachSeatThirdPersonCameraBit = 1u << 4;
// R-V1: seat-flags bit 5 is Reach's authored "allows weapons". Only such a
// seat fires the occupant's own weapon from the camera ray; every other seat
// fires from a vehicle barrel and its shot origin must not be re-aimed.
inline constexpr uint32_t kReachSeatAllowsWeaponsBit = 1u << 5;
inline constexpr int kReachVehicleSeatLimit = 16;

// HREK weapons.cpp reads the ordinary unit aiming vector from the live unit
// object at +0x214 when it builds the projectile line (R-V12/R-V18). This is
// engine aim state, not a camera marker or the compact render camera.
inline constexpr uintptr_t kReachUnitAimingVectorOffset = 0x214;

enum class ReachAimFeedbackSource : uint8_t
{
    OnFootCompact = 0,
    SeatedUnitAim,
    SeatedCompactFallback,
};

// View follow is deliberately an input here and deliberately does not select
// the aim source. Both view modes close on the same native unit aim; the option
// controls only how the rendered view follows the carrier.
inline constexpr ReachAimFeedbackSource ReachSelectAimFeedbackSource(
    bool seated, bool unitAimValid, bool /*viewFollowEnabled*/)
{
    if (!seated)
        return ReachAimFeedbackSource::OnFootCompact;
    return unitAimValid ? ReachAimFeedbackSource::SeatedUnitAim
                        : ReachAimFeedbackSource::SeatedCompactFallback;
}

inline bool ReachNormalizeUnitAimingVector(
    const float input[3], float output[3])
{
    if (!input || !output)
        return false;
    float lengthSquared = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        if (!std::isfinite(input[i]))
            return false;
        lengthSquared += input[i] * input[i];
    }
    if (!std::isfinite(lengthSquared) ||
        lengthSquared < 0.25f || lengthSquared > 4.0f)
    {
        return false;
    }
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    for (int i = 0; i < 3; ++i)
    {
        output[i] = input[i] * inverseLength;
        if (!std::isfinite(output[i]))
            return false;
    }
    return true;
}

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
// An unmatched (generic) vehicle publishes the sentinel identity code so the
// consumer chain (FpActive, recenter, body hide) still sees a live seat while
// every identity-keyed decoder fail-closes: ReachVehicleTrimSnapshotSlot and
// ReachCurrentSeat both reject codes above kReachVehicleIdentityCount, which
// binds the F1 sliders to the universal trim and keeps wheel/steering off.
inline constexpr uint8_t kReachVehicleGenericIdentityCode = 0xFF;

inline constexpr uint64_t ReachVehicleTrimSnapshot(
    uint32_t generation, ReachVehicleId id, int seat)
{
    return generation && seat >= 0 && seat < 16
        ? (static_cast<uint64_t>(generation) << 32) |
              (static_cast<uint64_t>(
                   id != ReachVehicleId::Unknown
                       ? static_cast<uint8_t>(id)
                       : kReachVehicleGenericIdentityCode) << 8) |
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
