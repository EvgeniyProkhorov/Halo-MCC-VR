#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

// Halo 3-only vehicle-state snapshot logic. This file contains no Windows,
// MinHook, logging, or engine access, so the packing, staleness, and debounce
// rules can be exhaustively tested before any detour is authorized. The Halo 3
// retail identities (RVAs, AOBs, derived offsets) arrive with the Phase 0
// evidence in docs/HALO3-VEHICLE-EVIDENCE.md; nothing here is engine-derived.
//
// Shape mirrors ReachVehicleInputSnapshot (reach_render_logic.h): the sampler
// on the proven render thread publishes one generation-keyed 64-bit atomic,
// consumers unpack it lock-free, and a stale or zero generation always reads
// as Unknown so no consumer can act on a previous title session's state.

// Halo 3 retail identities, all byte-proven in docs/HALO3-VEHICLE-EVIDENCE.md
// and independently re-derived by an adversarial second pass. The expected
// RVAs are logging/cross-check values only; the runtime binding is always the
// unique AOB match in the loaded module, never these numbers.
inline constexpr uintptr_t kHalo3UnitInVehicleNativeRva = 0x3A36F4;
inline constexpr uintptr_t kHalo3PlayerUnitGetterRva = 0xEE48C;
inline constexpr uintptr_t kHalo3VehicleTypeAccessorRva = 0x396D84;
inline constexpr uintptr_t kHalo3EngineTlsIndexRva = 0xA39F9C;

// Object-table facts, each read from a named instruction in the native
// unit_in_vehicle body (§E3): engine TLS slot +0x38 -> object table; entry
// array at table+0x48; 0x18-byte entries; data pointer at entry+0x10; cached
// object-kind byte at entry+3 (1 = vehicle). The unit's seat word and parent
// handle live in the unit's object data.
inline constexpr uintptr_t kHalo3TlsObjectTableOffset = 0x38;
inline constexpr uintptr_t kHalo3ObjectTableEntriesOffset = 0x48;
inline constexpr size_t kHalo3ObjectEntryStride = 0x18;
inline constexpr uintptr_t kHalo3ObjectEntryDataOffset = 0x10;
inline constexpr uintptr_t kHalo3ObjectEntryKindOffset = 3;
inline constexpr uint8_t kHalo3ObjectKindVehicle = 1;
inline constexpr uintptr_t kHalo3ObjectParentOffset = 0x10;
inline constexpr uintptr_t kHalo3UnitSeatWordOffset = 0x24E;

// Observer facts (§E4): the observer array pointer is engine TLS member
// +0x578 (no static RVA exists); records are 0x3D0 bytes; the effect stage
// writes position/forward/up at these record offsets. The C1 probe captures a
// float window around them so the seat-camera focus fields can be measured,
// not assumed.
inline constexpr uintptr_t kHalo3TlsObserverOffset = 0x578;
inline constexpr size_t kHalo3ObserverStride = 0x3D0;
inline constexpr uintptr_t kHalo3ObserverCaptureBase = 0x100;
inline constexpr size_t kHalo3ObserverCaptureFloats = 40; // +0x100..+0x1A0
inline constexpr uintptr_t kHalo3ObserverPosOffset = 0x11C;
inline constexpr uintptr_t kHalo3ObserverFwdOffset = 0x144;
inline constexpr uintptr_t kHalo3ObserverUpOffset = 0x150;
// Measured in the C1 headset session (evidence doc, runtime results): the
// engine-evaluated per-seat camera pivot. On foot it equals the camera
// position exactly (focus distance 0); in a seat it is the authored pivot the
// chase camera hangs behind. The C3 first-person camera anchors on this
// absolute triplet — never on pos+fwd*d, which C1 proved lags during turns.
inline constexpr uintptr_t kHalo3ObserverFocusOffset = 0x18C;

// Vehicle physics-type facts (§E1/§E2/§E4): 10 authored blocks in enum order
// human_tank..guardian; the retail accessor returns the first non-empty block
// index 0..9, or 0xB when a tag authors none (stationary turrets, shade, and
// mounted turret child tags). Flying = {human_plane, alien_fighter, vtol},
// proven from the kit's own _vehicle_mask_flying assert (test al,0x94).
inline constexpr int kHalo3VehicleTypeTurret = 5;
inline constexpr int kHalo3VehicleTypeNoneAuthored = 0xB;
inline constexpr uint32_t kHalo3VehicleFlyingTypeMask =
    (1u << 2) | (1u << 4) | (1u << 7);

enum class Halo3VehicleState : uint32_t
{
    Unknown = 0,
    OnFoot = 1,
    Vehicle = 2,
};

// Low-32 bit layout. The high 32 bits carry the runtime generation.
//   bits 0-1   Halo3VehicleState
//   bits 2-6   vehicle type + 1 (0 = type unproven this sample)
//   bits 7-13  seat index + 1 (0 = seat unproven this sample)
// Type and seat are optional refinements: a sampler that can only prove the
// seated-parent predicate publishes them as unproven, and every consumer must
// treat unproven exactly like "feature stays stock".
inline constexpr int kHalo3VehicleTypeMax = 29;
inline constexpr int kHalo3VehicleSeatMax = 125;

inline constexpr uint64_t Halo3VehicleSnapshot(
    uint32_t generation, Halo3VehicleState state,
    int vehicleType = -1, int seatIndex = -1)
{
    const uint32_t typePlus1 =
        (vehicleType >= 0 && vehicleType <= kHalo3VehicleTypeMax)
            ? static_cast<uint32_t>(vehicleType) + 1u
            : 0u;
    const uint32_t seatPlus1 =
        (seatIndex >= 0 && seatIndex <= kHalo3VehicleSeatMax)
            ? static_cast<uint32_t>(seatIndex) + 1u
            : 0u;
    return (static_cast<uint64_t>(generation) << 32) |
        (static_cast<uint32_t>(state) & 0x3u) |
        (typePlus1 << 2) | (seatPlus1 << 7);
}

inline constexpr Halo3VehicleState Halo3VehicleSnapshotState(
    uint64_t snapshot, uint32_t generation)
{
    if (generation == 0 ||
        static_cast<uint32_t>(snapshot >> 32) != generation)
        return Halo3VehicleState::Unknown;
    const uint32_t state = static_cast<uint32_t>(snapshot) & 0x3u;
    return state <= static_cast<uint32_t>(Halo3VehicleState::Vehicle)
        ? static_cast<Halo3VehicleState>(state)
        : Halo3VehicleState::Unknown;
}

// -1 = unproven or stale; consumers must degrade that refinement to stock.
inline constexpr int Halo3VehicleSnapshotType(
    uint64_t snapshot, uint32_t generation)
{
    if (Halo3VehicleSnapshotState(snapshot, generation) ==
        Halo3VehicleState::Unknown)
        return -1;
    const uint32_t typePlus1 = (static_cast<uint32_t>(snapshot) >> 2) & 0x1Fu;
    return static_cast<int>(typePlus1) - 1;
}

inline constexpr int Halo3VehicleSnapshotSeat(
    uint64_t snapshot, uint32_t generation)
{
    if (Halo3VehicleSnapshotState(snapshot, generation) ==
        Halo3VehicleState::Unknown)
        return -1;
    const uint32_t seatPlus1 = (static_cast<uint32_t>(snapshot) >> 7) & 0x7Fu;
    return static_cast<int>(seatPlus1) - 1;
}

// Vehicle world transform. The C5 drive session (2026-07-31 08:26 log)
// corrected the C4 pick, and ManagedDonkey's s_object_data confirms the
// names (offsets re-verified by member-size arithmetic): +0x50 is
// `position` — the object's authoritative world origin, i.e. the rigid
// tag/authoring frame the Blender seat points live in. R^T*(+0x1C - +0x50)
// reproduced each render model's node-0 default translation to the
// millimetre on warthog, mongoose and banshee. +0x1C is
// `bounding_sphere_center` (+0x3C/+0x2C the attached-bounds variants),
// which rides the ANIMATED model (brute chopper suspension drifts it
// ~0.09 wu) — never anchor authored points to it. +0x5C `forward` /
// +0x68 `up` are the rigid frame's basis, contiguous with `position`.
// left = up x fwd (right-handed, proven by the passenger seat at -Y).
inline constexpr uintptr_t kHalo3ObjectPositionOffset = 0x50;
inline constexpr uintptr_t kHalo3ObjectBoundingCenterOffset = 0x1C;
inline constexpr uintptr_t kHalo3ObjectForwardOffset = 0x5C;
inline constexpr uintptr_t kHalo3ObjectUpOffset = 0x68;

// C7: object frames and parent-chain composition. A ROOT object stores its
// world transform at +0x50/+0x5C/+0x68 (C6 headset-proven on every driver
// seat). An ATTACHED child object (mounted turret) stores the SAME fields
// PARENT-RELATIVE — the C6 log's hog chaingun read position (-0.5, 0, 0.019)
// / forward (1,0,0) / up (0,0,1) while riding at world (9.9, 12.1, -21.4),
// which is why the C6 turret camera composed against the map origin ("deep
// in the sky"). The anchor frame must therefore be composed through the
// parent chain: world = parent ∘ local, with local coordinates expressed in
// the parent's (fwd, left, up) axes.
struct Halo3Frame
{
    float pos[3];
    float fwd[3];
    float up[3];
};

inline void Halo3FrameLeft(const Halo3Frame& f, float out[3])
{
    out[0] = f.up[1] * f.fwd[2] - f.up[2] * f.fwd[1];
    out[1] = f.up[2] * f.fwd[0] - f.up[0] * f.fwd[2];
    out[2] = f.up[0] * f.fwd[1] - f.up[1] * f.fwd[0];
}

inline bool Halo3FrameOrthonormal(const Halo3Frame& f)
{
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(f.pos[i]) || !std::isfinite(f.fwd[i]) ||
            !std::isfinite(f.up[i]))
            return false;
    const float fLen = f.fwd[0] * f.fwd[0] + f.fwd[1] * f.fwd[1] +
        f.fwd[2] * f.fwd[2];
    const float uLen = f.up[0] * f.up[0] + f.up[1] * f.up[1] +
        f.up[2] * f.up[2];
    const float fu = f.fwd[0] * f.up[0] + f.fwd[1] * f.up[1] +
        f.fwd[2] * f.up[2];
    return std::fabs(fLen - 1.0f) <= 0.05f && std::fabs(uLen - 1.0f) <= 0.05f &&
        std::fabs(fu) <= 0.15f;
}

// Map a point/vector expressed in `frame` axes (x fwd, y left, z up) to world.
inline void Halo3FrameRotate(const Halo3Frame& frame, const float v[3],
                             float out[3])
{
    float left[3];
    Halo3FrameLeft(frame, left);
    for (int i = 0; i < 3; ++i)
        out[i] = frame.fwd[i] * v[0] + left[i] * v[1] + frame.up[i] * v[2];
}

inline Halo3Frame Halo3ComposeFrame(const Halo3Frame& parent,
                                    const Halo3Frame& local)
{
    Halo3Frame world{};
    Halo3FrameRotate(parent, local.pos, world.pos);
    for (int i = 0; i < 3; ++i)
        world.pos[i] += parent.pos[i];
    Halo3FrameRotate(parent, local.fwd, world.fwd);
    Halo3FrameRotate(parent, local.up, world.up);
    return world;
}

// Anchor-frame sanity: the frame origin must sit inside the seated parent's
// own bounding sphere (+0x1C center / +0x28 radius) plus a margin generous
// enough for every authored point yet far below any wrong-frame miss (the C6
// sky camera was tens of wu off). Insane -> the stock chase view stands.
inline bool Halo3AnchorWithinBounds(const float pos[3], const float center[3],
                                    float radius, float margin)
{
    if (!std::isfinite(radius) || radius < 0.0f || radius > 100.0f)
        return false;
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        if (!std::isfinite(pos[i]) || !std::isfinite(center[i]))
            return false;
        const float d = pos[i] - center[i];
        d2 += d * d;
    }
    const float limit = radius + margin;
    return d2 <= limit * limit;
}

// Frame provenance, published for the probe log so a headset report can be
// matched to how its anchor was built.
enum class Halo3FrameStatus : uint32_t
{
    None = 0,
    Direct = 1,     // root object: +0x50 frame used as-is
    Composed = 2,   // attached child: parent ∘ local
    Insane = 3,     // sanity gate rejected the frame -> stock
    TooDeep = 4,    // more than one attachment level -> stock
};

// C9 seat yaw — "the camera does not rotate with the car" (user, 2026-07-31).
// The FP seat camera moves the eye onto the authored point but leaves facing
// entirely HMD-owned in WORLD space (ApplyHeadLook builds its yaw from
// g_gameYawRef, which only a recenter or a cutscene boundary ever rebases).
// Drive a circle and the hull turns underneath a view pinned to the map, so
// "straight ahead" ends up out of the side of the vehicle. The fix is to add
// the hull's rotation since you sat down to that yaw reference.
//
// CRITICAL: the SAME offset must reach the hand-aim ray (Game_ComputeAimStick
// maps the controller through g_gameYawRef too). View and aim sharing one
// reference is what keeps the reticle on what you are looking at; rotating
// only the view would slide them apart by exactly this angle. That shared
// reference is also the vehicle's steering channel — which is why this cannot
// ship before the motion-control steering author (see §C9 in the evidence
// doc): with the reference following the hull, a driver's fixed hand offset
// becomes a constant turn RATE instead of a heading.
//
// Continuous accumulation, not a wrapped difference: a partial follow weight
// applied to a wrapped angle would snap 2*pi*weight every time the hull passed
// the wrap point. The total is wrapped only on the way out, where it is
// consumed as cos/sin and a 2*pi difference is invisible.
inline float Halo3WrapPi(float a)
{
    constexpr float kPi = 3.14159265f;
    constexpr float kTwoPi = 6.28318531f;
    if (!std::isfinite(a))
        return 0.0f;
    while (a > kPi) a -= kTwoPi;
    while (a < -kPi) a += kTwoPi;
    return a;
}

struct Halo3SeatYaw
{
    bool armed = false;
    float lastYaw = 0.0f;   // previous hull heading
    float total = 0.0f;     // hull rotation accumulated since seat entry
};

// hullYaw = atan2(fwd[1], fwd[0]) of the seated parent's measured frame.
// active = the FP seat camera owns the view this frame (a stale, torn or
// unproven transform must pass false, which re-references on the next entry).
// weight = vehicle_view_follow, 0 = today's world-locked view, 1 = locked to
// the hull. Returns the yaw to ADD to the shared reference.
inline float Halo3UpdateSeatYaw(Halo3SeatYaw& state, float hullYaw,
                                bool active, float weight)
{
    if (!active || !std::isfinite(hullYaw) || !std::isfinite(weight))
    {
        state.armed = false;
        state.total = 0.0f;
        return 0.0f;
    }
    if (!state.armed)
    {
        // First seated frame: this heading is "straight ahead" from now on.
        state.armed = true;
        state.lastYaw = hullYaw;
        state.total = 0.0f;
        return 0.0f;
    }
    state.total += Halo3WrapPi(hullYaw - state.lastYaw);
    state.lastYaw = hullYaw;
    const float w = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
    return Halo3WrapPi(state.total * w);
}

// C7 identity-probe field map (log-only; §E5, offline-verified, nothing keys
// a camera off these until a headset log confirms the in-process reads).
// Definition-relative tag_block records: +0x2C0 + type*0xC {count, address};
// element = tagDataBase + address*4. Discriminating fields:
inline constexpr uintptr_t kHalo3VehiclePhysicsBlocksOffset = 0x2C0;
inline constexpr size_t kHalo3VehiclePhysicsBlockStride = 0xC;
inline constexpr uintptr_t kHalo3JeepEngineMomentOffset = 0x14;   // element
inline constexpr uintptr_t kHalo3JeepTurnRateOffset = 0x10;       // element
inline constexpr uintptr_t kHalo3ScoutSpecificTypeOffset = 0x28;  // int8
inline constexpr uintptr_t kHalo3ScoutAccelerationOffset = 0x10;  // element
inline constexpr uintptr_t kHalo3DefMinFlipVelocityOffset = 0x39C;
inline constexpr uintptr_t kHalo3DefMaxFlipVelocityOffset = 0x3A0;
inline constexpr uintptr_t kHalo3DefBlurSpeedOffset = 0x3B4;

// User-authored first-person seat points (Blender kit, model-space world
// units, relative to the DIRECT parent object's origin and basis).
// nativeRequired: -1 = any, else the exact nativeInVehicle flag (separates
// mounted turrets from stationary ones). Entries cover the seats whose
// vehicle identity is certain from type/seat alone; same-type cousins
// (mongoose, other mounted turrets) intentionally share the closest authored
// point until their fingerprints are labeled.
struct Halo3SeatPoint
{
    uint8_t directType;
    int8_t seatIndex;
    int8_t nativeRequired;
    float x, y, z;
};

inline constexpr Halo3SeatPoint kHalo3SeatPoints[] = {
    {8, 0, -1, -0.512f, 0.000f, 0.738f}, // brute chopper driver
    {4, 0, -1, 0.563f, 0.005f, 0.435f},  // banshee pilot
    {7, 0, -1, 0.993f, -0.003f, 0.612f}, // hornet pilot
    {7, 1, -1, 0.521f, 0.250f, 0.524f},  // hornet left passenger
    {7, 2, -1, 0.521f, -0.250f, 0.524f}, // hornet right passenger
    {0, 0, -1, 0.033f, 0.189f, 1.023f},  // scorpion driver
    {1, 0, -1, 0.053f, 0.166f, 0.614f},  // warthog driver (all human_jeep)
    {1, 1, -1, 0.013f, -0.159f, 0.660f}, // warthog passenger (all human_jeep)
    {5, 0, 0, -0.391f, 0.000f, 0.519f},  // stationary turret (mg point)
    {5, 0, 1, -0.123f, 0.000f, 0.573f},  // mounted turret (chaingun point)
};

inline constexpr const Halo3SeatPoint* Halo3FindSeatPoint(
    int directType, int seatIndex, int nativeInVehicle)
{
    for (const Halo3SeatPoint& p : kHalo3SeatPoints)
    {
        if (static_cast<int>(p.directType) == directType &&
            static_cast<int>(p.seatIndex) == seatIndex &&
            (p.nativeRequired < 0 || p.nativeRequired == nativeInVehicle))
            return &p;
    }
    return nullptr;
}

// C4 transform probe: bounded float window of the seated DIRECT parent's
// object data, captured so a drive session can measure where the vehicle's
// world position and orientation vectors live (the Blender-authored seat
// points are in the direct parent's model space and need that transform).
// Nothing downstream consumes offsets until the log proves them.
inline constexpr uintptr_t kHalo3ParentCaptureBase = 0x14;
inline constexpr size_t kHalo3ParentCaptureFloats = 56; // +0x14..+0xF4

// Pure analysis for the C4 worker: find float triplets of ~unit length (the
// signature of orientation basis vectors) and the triplet nearest a reference
// point (the signature of the object position, referenced against the
// C1-proven observer focus).
struct Halo3UnitTripletScan
{
    int count = 0;
    int indices[12] = {};
};

inline Halo3UnitTripletScan Halo3FindUnitTriplets(
    const float* window, size_t floatCount, float tolerance)
{
    Halo3UnitTripletScan scan{};
    if (!window)
        return scan;
    for (int i = 0; i + 2 < static_cast<int>(floatCount); ++i)
    {
        const float a = window[i], b = window[i + 1], c = window[i + 2];
        if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c))
            continue;
        const float len = std::sqrt(a * a + b * b + c * c);
        if (std::fabs(len - 1.0f) <= tolerance)
        {
            if (scan.count < 12)
                scan.indices[scan.count] = i;
            ++scan.count;
        }
    }
    return scan;
}

struct Halo3NearestTriplet
{
    int index = -1;
    float distance = 0.0f;
};

inline Halo3NearestTriplet Halo3FindNearestTriplet(
    const float* window, size_t floatCount, const float reference[3])
{
    Halo3NearestTriplet best{};
    if (!window || !reference)
        return best;
    for (int i = 0; i + 2 < static_cast<int>(floatCount); ++i)
    {
        const float dx = window[i] - reference[0];
        const float dy = window[i + 1] - reference[1];
        const float dz = window[i + 2] - reference[2];
        if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz))
            continue;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (best.index < 0 || d < best.distance)
        {
            best.index = i;
            best.distance = d;
        }
    }
    return best;
}

// C2 mode refinement: how the Present-tick publisher upgrades Gameplay when
// the sampler proves a seated state. 0 = leave Gameplay (not seated, unknown,
// or stale), 1 = Vehicle, 2 = Turret. Turret requires the measured physics
// type 5 (C1-confirmed live for mounted AND stationary turrets); an unproven
// type upgrades only to Vehicle so a missing refinement stays conservative.
inline constexpr int Halo3ClassifyGameplayUpgrade(
    Halo3VehicleState state, int vehicleType)
{
    if (state != Halo3VehicleState::Vehicle)
        return 0;
    return vehicleType == kHalo3VehicleTypeTurret ? 2 : 1;
}

// Focus-candidate scan for the C1 observer capture. The chase camera is
// modeled as position = focus - forward * focus_distance, where focus is the
// engine-evaluated per-seat camera pivot. Given the captured float window and
// the proven position/forward offsets, find the (triplet, scalar) pair that
// best satisfies |position + forward * d - triplet|. Pure and worker-only;
// the result names the focus_position and focus_distance offsets by
// measurement instead of assumption.
struct Halo3FocusCandidate
{
    int tripletIndex = -1;   // float index of the candidate focus triplet
    int distanceIndex = -1;  // float index of the candidate distance scalar
    float residual = 0.0f;
    float distance = 0.0f;
};

inline Halo3FocusCandidate Halo3FindFocusCandidate(
    const float* window, size_t count, size_t posIndex, size_t fwdIndex)
{
    Halo3FocusCandidate best{};
    if (!window || count < 3 || posIndex + 2 >= count || fwdIndex + 2 >= count)
        return best;
    const float* pos = window + posIndex;
    const float* fwd = window + fwdIndex;
    for (int i = 0; i + 2 < static_cast<int>(count); ++i)
    {
        // A focus triplet must be distinct from the position and forward
        // fields themselves (overlap would trivially "solve" with d == 0).
        if (i + 2 >= static_cast<int>(posIndex) &&
            i <= static_cast<int>(posIndex) + 2)
            continue;
        if (i + 2 >= static_cast<int>(fwdIndex) &&
            i <= static_cast<int>(fwdIndex) + 2)
            continue;
        const float* cand = window + i;
        if (!std::isfinite(cand[0]) || !std::isfinite(cand[1]) ||
            !std::isfinite(cand[2]))
            continue;
        for (int j = 0; j < static_cast<int>(count); ++j)
        {
            const float d = window[j];
            if (!std::isfinite(d) || d < 0.0f || d > 100.0f)
                continue;
            const float ex = pos[0] + fwd[0] * d - cand[0];
            const float ey = pos[1] + fwd[1] * d - cand[1];
            const float ez = pos[2] + fwd[2] * d - cand[2];
            const float residual = std::sqrt(ex * ex + ey * ey + ez * ez);
            if (best.tripletIndex < 0 || residual < best.residual)
            {
                best.tripletIndex = i;
                best.distanceIndex = j;
                best.residual = residual;
                best.distance = d;
            }
        }
    }
    return best;
}

// Entry/exit debounce. Halo 3 swings its chase camera for a moment when a seat
// is entered or exited; the first-person substitution and its one-shot
// recenter must trigger on a settled seat, not mid-swing. The raw per-frame
// freshness gate is separate and immediate: a stale snapshot always fails open
// to stock the same frame, regardless of what this debounce last reported.
inline constexpr uint32_t kHalo3VehicleDebounceFrames = 15;

struct Halo3VehicleDebounce
{
    Halo3VehicleState stable = Halo3VehicleState::Unknown;
    Halo3VehicleState candidate = Halo3VehicleState::Unknown;
    uint32_t count = 0;

    // Feed one sampled state per frame. Returns true exactly when `stable`
    // changes, i.e. the sampled state held for `threshold` consecutive frames;
    // that edge is the recenter trigger. A flapping sample restarts the count.
    constexpr bool Update(Halo3VehicleState sampled, uint32_t threshold)
    {
        if (sampled == stable)
        {
            candidate = stable;
            count = 0;
            return false;
        }
        if (sampled != candidate)
        {
            candidate = sampled;
            count = 1;
        }
        else
        {
            ++count;
        }
        if (count >= threshold)
        {
            stable = candidate;
            count = 0;
            return true;
        }
        return false;
    }
};
