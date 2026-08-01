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

// ---- C10: render-rate smoothing of the sampled hull frame ----------------
// MEASURED (2026-07-31 Steam session, source b25a414): the camera hook runs at
// 240/sec and the seated parent's +0x50/+0x5C/+0x68 change on exactly 25% of
// those calls — 60 Hz — with position steps up to 0.1238 wu (0.38 m) and
// orientation steps up to 16 deg. Halo 3 simulates at a fixed 60 Hz and MCC
// interpolates the drawn MESH up to the presented rate, so a camera pinned to
// the raw sampled value staircases against a model that glides: the user's
// "the vehicles are shaking and vibrating ... the car gyrates".
//
// The fix is to reconstruct the same thing the renderer does — walk linearly
// from the previous simulation state to the current one across one tick. This
// is continuous by construction: when a new state arrives, the point we have
// just reached (the old current) becomes the new previous, so the output never
// jumps. A t past 1 extrapolates linearly beyond the newest state; the caller
// only ever asks for that by the amount the renderer itself was MEASURED to
// draw ahead (the C12 phase lock below), and only at speed, so a collision
// tick can never fling the camera further than the renderer flings the mesh.
inline Halo3Frame Halo3LerpFrame(const Halo3Frame& a, const Halo3Frame& b,
                                 float t)
{
    Halo3Frame out{};
    for (int i = 0; i < 3; ++i)
    {
        out.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
        out.fwd[i] = a.fwd[i] + (b.fwd[i] - a.fwd[i]) * t;
        out.up[i] = a.up[i] + (b.up[i] - a.up[i]) * t;
    }
    // A component-wise blend of two rotations is not a rotation. Renormalize,
    // then take the up vector perpendicular to forward (Gram-Schmidt). The
    // per-tick angle is small, so this is indistinguishable from a slerp.
    const float fLen = std::sqrt(out.fwd[0] * out.fwd[0] +
                                 out.fwd[1] * out.fwd[1] +
                                 out.fwd[2] * out.fwd[2]);
    if (!(fLen > 1e-3f))
        return b;
    for (int i = 0; i < 3; ++i) out.fwd[i] /= fLen;
    const float fu = out.fwd[0] * out.up[0] + out.fwd[1] * out.up[1] +
        out.fwd[2] * out.up[2];
    for (int i = 0; i < 3; ++i) out.up[i] -= out.fwd[i] * fu;
    const float uLen = std::sqrt(out.up[0] * out.up[0] +
                                 out.up[1] * out.up[1] +
                                 out.up[2] * out.up[2]);
    if (!(uLen > 1e-3f))
        return b;
    for (int i = 0; i < 3; ++i) out.up[i] /= uLen;
    return out;
}

struct Halo3FrameInterp
{
    bool valid = false;
    Halo3Frame prev{};
    Halo3Frame cur{};
    double lastTime = 0.0;      // previous call, for the free-running phase
    double curTime = 0.0;       // when `cur` was first seen, seconds
    double phase = 0.0;         // ticks elapsed since `cur` became current
    double tick = 1.0 / 60.0;   // measured simulation interval
    bool tickKnown = false;
    float correction = 0.0f;    // last phase nudge, diagnostic only
    float lastStepLen = 0.0f;   // |cur - prev| position step, wu
    // Glides 1 -> 0 when simulation ticks stop arriving, so the moment the
    // sim stalls the camera stops extrapolating over ~40 ms instead of
    // snapping back to the newest state.
    float live = 0.0f;
    // C15: the photon-time lead actually in effect, glided. This lives on the
    // interpolator, NOT on the diagnostic lock — until C15 it sat inside the
    // `if (lock)` branch, so passing no lock silently disabled the whole
    // prediction. The game always passes one, so it worked in the headset,
    // but it made the prediction depend on an unrelated diagnostic object
    // and it made every offline measurement of the prediction read zero.
    float leadApplied = 0.0f;
    // Last published blend fraction. Diagnostic: >1 means the seat is being
    // extrapolated past the newest simulation state.
    float lastAlpha = 0.0f;
};

// ---- C12: measured display-phase lock ------------------------------------
// C11 paced the camera evenly but could not know WHERE inside the tick the
// renderer draws the mesh, so the constant offset between the two was left as
// a hand-tuned config knob — and because the blend was clamped at the newest
// state, any non-zero knob slid for part of each tick and froze for the rest,
// a 60 Hz sawtooth ("bounces repeatedly at rapid small speeds", user,
// 2026-07-31). Both are replaced by measuring the renderer directly.
//
// The witness is the object's bounding-sphere centre (+0x1C), sampled every
// camera call in the same capture window as the hull frame. It is ANIMATED
// data: if MCC render-interpolates the model, this value glides between the
// two simulation states we already hold — so projecting it onto the segment
// between those states reads off the exact sub-tick fraction the renderer is
// displaying right now. Every assumption is checked at runtime:
//  - the centre's hull-frame offset is latched only while PARKED, when sim
//    and render provably show the same pose;
//  - the phase is measured only when the centre was seen moving BETWEEN hull
//    ticks (a 60 Hz value cannot), only at a driving pace, and only against a
//    per-tick travel big enough to project on;
//  - the learned lead is a 2%-per-sample average, applied only at speed, so
//    a parked or crawling vehicle never extrapolates its own physics jitter.
// Anything unproven leaves lead at zero, which is exactly C11.
struct Halo3PhaseLock
{
    // Bounding-centre offset in hull axes, latched at rest.
    bool refValid = false;
    float refLocal[3] = {0.0f, 0.0f, 0.0f};
    double stillSeconds = 0.0;
    // Previous call's raw centre, for change detection.
    bool lastBValid = false;
    float lastB[3] = {0.0f, 0.0f, 0.0f};
    // Render-rate proof. C13: counted over calls on which the HULL did NOT
    // change, accumulated across ticks — see kHalo3PhaseProofCalls.
    int quietCalls = 0;
    int quietMoves = 0;
    bool smooth = false;
    // The learned constant: how far ahead of the free-running phase the
    // renderer actually draws, in ticks.
    bool leadValid = false;
    float lead = 0.0f;
    // What is actually added to the blend this call: glides toward
    // lead*ramp (or zero when ticks stop) a step per call, so engaging and
    // disengaging is a ~40 ms slide, never a one-frame snap.
    float leadApplied = 0.0f;
    // True once at least one tick window was long enough to judge `smooth`,
    // so the log can tell "not render-interpolated" from "not measured yet".
    bool smoothEvaluated = false;
    float lastMeasured = 0.0f;  // diagnostic only
};

// C13 — HEADSET REFRESH RATE. Halo 3's 60 Hz is the ENGINE's simulation
// clock, identical on every machine; the camera hook runs at whatever rate
// the headset presents (72, 80, 90, 120, 144...). Nothing here may assume a
// call rate: the tick length is measured, the phase free-runs on the
// performance counter, and the constants below are in SECONDS or in events,
// never in frames. The user asked for a refresh-rate selector; there is
// nothing for one to select, but their question did find the one place that
// had baked a rate in — the render-smoothness proof below, which needed
// three camera calls inside one 16.7 ms tick and so could never complete on
// a 72-90 Hz headset (1.2 to 1.5 calls per tick).
//
// The rate-independent form: judge on calls where the HULL DID NOT CHANGE.
// A 60 Hz witness cannot move on those; a render-interpolated one always
// does. Rare on a slow headset, common on a fast one, correct on both, and
// accumulated across ticks rather than inside one.
inline constexpr int kHalo3PhaseProofCalls = 12;
// Time (not frames) of stillness that counts as parked.
inline constexpr double kHalo3PhaseStillSeconds = 0.15;
// Bounding-centre movement below this is standing still (wu).
inline constexpr float kHalo3PhaseStillEpsilon = 1e-4f;
// Per-tick travel (wu) below which no phase is measured and no lead applied:
// 0.01 wu/tick is walking pace, where a phase error is invisible anyway.
inline constexpr float kHalo3PhaseMinStep = 0.01f;
inline constexpr float kHalo3PhaseLeadTrack = 0.02f;  // EMA per fresh sample
// Time constant for gliding the APPLIED lead in and out. In seconds, so a
// hitch or a stop takes the same ~40 ms to fade at 72 Hz as at 144 Hz; a
// per-call factor would have been twice as abrupt on a fast headset.
inline constexpr double kHalo3PhaseApplySeconds = 0.04;
inline constexpr float kHalo3PhaseLeadMin = -0.5f;
inline constexpr float kHalo3PhaseLeadMax = 1.5f;
// Hard ceiling on the displayed blend fraction while ticks are arriving.
inline constexpr double kHalo3PhaseAlphaMax = 1.75;

// How hard each detected tick pulls the free-running phase back into lock.
// Small on purpose: the whole point is that the phase must NOT be yanked to a
// new value every tick.
inline constexpr double kHalo3PhaseTrack = 0.05;

// Position change (wu) below which two samples count as the same state.
inline constexpr float kHalo3FrameSameEpsilon = 1e-5f;
// A single tick can never move a vehicle this far or turn it this hard; that
// is a teleport, a respawn or a seat change, and it must snap, not slide.
inline constexpr float kHalo3FrameSnapDistance = 2.0f;   // wu
inline constexpr float kHalo3FrameSnapDot = 0.5f;        // ~60 degrees
// Plausible simulation intervals. Anything outside is a hitch, not a tick.
inline constexpr double kHalo3TickMin = 0.004;
inline constexpr double kHalo3TickMax = 0.050;

// Feed the RAW sampled frame every camera call with a monotonic clock in
// seconds (the tick counter is far too coarse — a 60 Hz tick is under two of
// its 15.6 ms steps). Returns the frame to publish.
// C14 — PHOTON-TIME PREDICTION, and why the headset rate genuinely matters.
//
// The user insisted the refresh rate belongs in this feature and I argued it
// did not. Their log settled it: `appHz=120.000 periodNs=8333333
// predDeltaMs=8.334`. Every frame we sample is DISPLAYED one display period
// in the future — 8.33 ms on their 120 Hz headset, 13.9 ms at 72, 6.9 at 144.
// OpenXR already predicts the HEAD pose to that photon moment (that is what
// xrLocateSpace's predictedDisplayTime does), but the vehicle was placed at
// the sample instant, so the seat lagged the head by a whole display period,
// systematically, and worse the faster the vehicle moved. Half a 60 Hz tick
// at 120 Hz. That is the real content of "60hz is not enough".
//
// `leadSeconds` is that interval, taken from the runtime's own reported
// period (or a user-pinned rate when a runtime misreports), and converted
// here into a fraction of the MEASURED simulation tick. It is not a knob to
// tune: it is the same photon time the head is already predicted to.
inline Halo3Frame Halo3InterpolateFrame(Halo3FrameInterp& state,
                                        const Halo3Frame& raw, double nowSec,
                                        bool enabled,
                                        Halo3PhaseLock* lock = nullptr,
                                        const float* rawBounds = nullptr,
                                        double leadSeconds = 0.0)
{
    if (!enabled || !std::isfinite(nowSec))
    {
        state.valid = false;
        state.tickKnown = false; // the log mirror keys off this: OFF is OFF
        if (lock)
            *lock = Halo3PhaseLock{};
        return raw;
    }
    if (!state.valid)
    {
        state.valid = true;
        state.prev = raw;
        state.cur = raw;
        state.lastTime = nowSec;
        state.curTime = nowSec;
        state.phase = 0.0;
        state.tickKnown = false;
        state.correction = 0.0f;
        state.lastStepLen = 0.0f;
        state.live = 0.0f;
        state.leadApplied = 0.0f;
        if (lock)
            *lock = Halo3PhaseLock{};
        return raw;
    }
    // The phase FREE-RUNS on the wall clock. Restarting the blend at zero on
    // every detected tick is what the C10 video showed as a residual shake:
    // detection lands 0 to one camera-interval after the real tick and a
    // different amount late each time, so each restart moved the camera by up
    // to a quarter of a tick's worth of travel. Measured on the 2026-07-31
    // 13:16 session: the vehicle still moved on 49% of frames in which the
    // distant world was stationary, mean zero, sign flipping nearly every
    // frame — an oscillation about one sixth of a tick in size.
    const double step = nowSec - state.lastTime;
    state.lastTime = nowSec;
    if (state.tickKnown && step > 0.0 && step < 0.25)
        state.phase += step / state.tick;
    bool changed = false;
    float moved = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        const float d = raw.pos[i] - state.cur.pos[i];
        moved += d * d;
        if (std::fabs(d) > kHalo3FrameSameEpsilon ||
            std::fabs(raw.fwd[i] - state.cur.fwd[i]) > kHalo3FrameSameEpsilon ||
            std::fabs(raw.up[i] - state.cur.up[i]) > kHalo3FrameSameEpsilon)
            changed = true;
    }
    // C12 bookkeeping: the raw bounding centre is the render-side witness.
    const bool haveB = lock && rawBounds &&
        std::isfinite(rawBounds[0]) && std::isfinite(rawBounds[1]) &&
        std::isfinite(rawBounds[2]);
    bool bMoved = false;
    if (lock)
    {
        if (haveB)
        {
            if (lock->lastBValid)
                bMoved =
                    std::fabs(rawBounds[0] - lock->lastB[0]) >
                        kHalo3PhaseStillEpsilon ||
                    std::fabs(rawBounds[1] - lock->lastB[1]) >
                        kHalo3PhaseStillEpsilon ||
                    std::fabs(rawBounds[2] - lock->lastB[2]) >
                        kHalo3PhaseStillEpsilon;
            // C13 rate-independent proof: only calls on which the hull held
            // still can tell a render-interpolated centre from a 60 Hz one,
            // and they exist at every headset rate. Counted only while
            // driving, so a parked vehicle's stillness cannot be read as
            // "not interpolated".
            if (!changed && lock->lastBValid &&
                state.lastStepLen >= kHalo3PhaseMinStep)
            {
                ++lock->quietCalls;
                if (bMoved)
                    ++lock->quietMoves;
                if (lock->quietCalls >= kHalo3PhaseProofCalls)
                {
                    // A 60 Hz witness moves on NO quiet call. An interpolated
                    // one moves on the fraction of them that carry a present,
                    // which is the present rate over the camera rate and can
                    // legitimately be well under half (MCC presenting at 120
                    // while the hook runs at 240). A quarter separates "some
                    // of the time" from "never" without demanding a rate.
                    lock->smooth =
                        lock->quietMoves * 4 >= lock->quietCalls;
                    lock->smoothEvaluated = true;
                }
            }
            if (!changed && !bMoved && lock->lastBValid)
            {
                // Parked, the simulation and the renderer provably show the
                // same pose, so the centre's hull-frame offset can be read
                // exactly — the reference every moving measurement projects
                // against.
                if (step > 0.0 && step < 0.25)
                    lock->stillSeconds += step;
                if (lock->stillSeconds >= kHalo3PhaseStillSeconds)
                {
                    float rel[3], left[3];
                    for (int i = 0; i < 3; ++i)
                        rel[i] = rawBounds[i] - state.cur.pos[i];
                    Halo3FrameLeft(state.cur, left);
                    lock->refLocal[0] = rel[0] * state.cur.fwd[0] +
                        rel[1] * state.cur.fwd[1] + rel[2] * state.cur.fwd[2];
                    lock->refLocal[1] =
                        rel[0] * left[0] + rel[1] * left[1] + rel[2] * left[2];
                    lock->refLocal[2] = rel[0] * state.cur.up[0] +
                        rel[1] * state.cur.up[1] + rel[2] * state.cur.up[2];
                    lock->refValid = true;
                }
            }
            else
                lock->stillSeconds = 0.0;
            for (int i = 0; i < 3; ++i)
                lock->lastB[i] = rawBounds[i];
            lock->lastBValid = true;
        }
        else
        {
            lock->lastBValid = false;
            lock->stillSeconds = 0.0;
        }
    }
    if (changed)
    {
        const float dot = raw.fwd[0] * state.cur.fwd[0] +
            raw.fwd[1] * state.cur.fwd[1] + raw.fwd[2] * state.cur.fwd[2];
        if (moved > kHalo3FrameSnapDistance * kHalo3FrameSnapDistance ||
            dot < kHalo3FrameSnapDot)
        {
            state.prev = raw;
            state.cur = raw;
            state.curTime = nowSec;
            state.phase = 0.0;
            state.lastStepLen = 0.0f;
            if (lock)
            {
                // A teleport invalidates the accumulated proof statistics but
                // not the vehicle-shape reference or the learned lead.
                lock->quietCalls = 0;
                lock->quietMoves = 0;
                lock->stillSeconds = 0.0;
                lock->smooth = false;
                lock->smoothEvaluated = false;
            }
            return raw;
        }
        const double interval = nowSec - state.curTime;
        if (interval >= kHalo3TickMin && interval <= kHalo3TickMax)
        {
            // Detection is quantized to the camera's own rate, so smooth the
            // estimate rather than trusting one interval.
            state.tick = state.tickKnown
                ? state.tick * 0.9 + interval * 0.1
                : interval;
            state.tickKnown = true;
        }
        // C12: was the centre seen moving BETWEEN hull ticks? Only a
        // render-interpolated value can be; a 60 Hz value moves only on the
        // tick itself.
        state.prev = state.cur;
        state.cur = raw;
        state.curTime = nowSec;
        {
            float d2 = 0.0f;
            for (int i = 0; i < 3; ++i)
            {
                const float d = state.cur.pos[i] - state.prev.pos[i];
                d2 += d * d;
            }
            state.lastStepLen = std::sqrt(d2);
        }
        // Consume the tick we just crossed, then pull the phase gently toward
        // where it should be at this instant: the real tick happened somewhere
        // inside the last camera interval, so on average half of one. A gentle
        // pull keeps the sub-tick offset the phase already has instead of
        // re-randomising it, which is the whole fix.
        const double target = state.tick > 0.0
            ? 0.5 * step / state.tick
            : 0.0;
        state.phase -= 1.0;
        if (state.phase < -0.5 || state.phase > 1.5)
            state.phase = target;                 // hitch: relock immediately
        else
            state.phase += (target - state.phase) * kHalo3PhaseTrack;
        state.correction = static_cast<float>(state.phase - target);
        if (state.phase < 0.0)
            state.phase = 0.0;
    }
    if (!state.tickKnown)
        return state.cur;
    // C12 measurement: project the render-side centre onto the segment
    // between the same centre computed from the two simulation states. The
    // result is the blend fraction the renderer is displaying RIGHT NOW;
    // its constant offset from our free-running phase is the lead. Sampled
    // only on calls where the centre freshly moved (a stale read would bias
    // the phase low), only while ticks are actually arriving, and only when
    // the tick's travel is long enough to project against.
    const bool ticksLive = nowSec - state.curTime <= state.tick * 1.5;
    if (haveB && bMoved && ticksLive && lock->refValid && lock->smooth)
    {
        float bPrev[3], bCur[3], rot[3];
        Halo3FrameRotate(state.prev, lock->refLocal, rot);
        for (int i = 0; i < 3; ++i)
            bPrev[i] = state.prev.pos[i] + rot[i];
        Halo3FrameRotate(state.cur, lock->refLocal, rot);
        for (int i = 0; i < 3; ++i)
            bCur[i] = state.cur.pos[i] + rot[i];
        float dir[3], d2 = 0.0f, num = 0.0f;
        for (int i = 0; i < 3; ++i)
        {
            dir[i] = bCur[i] - bPrev[i];
            d2 += dir[i] * dir[i];
            num += (rawBounds[i] - bPrev[i]) * dir[i];
        }
        if (d2 > kHalo3PhaseMinStep * kHalo3PhaseMinStep)
        {
            const float alphaMeas = num / d2;
            if (alphaMeas > -0.5f && alphaMeas < 2.5f)
            {
                const float sample =
                    alphaMeas - static_cast<float>(state.phase);
                lock->lead = lock->leadValid
                    ? lock->lead + (sample - lock->lead) * kHalo3PhaseLeadTrack
                    : sample;
                if (lock->lead < kHalo3PhaseLeadMin)
                    lock->lead = kHalo3PhaseLeadMin;
                if (lock->lead > kHalo3PhaseLeadMax)
                    lock->lead = kHalo3PhaseLeadMax;
                lock->leadValid = true;
                lock->lastMeasured = alphaMeas;
            }
        }
    }
    // C15 — the forward/back bounce, "most prominent during boosting and
    // flying" (user, 2026-08-01).
    //
    // The camera hook runs about four times per 60 Hz simulation tick, and a
    // tick is only NOTICED on the first call after it lands. So on the last
    // call or two of every tick the free-running phase has already passed
    // 1.0. Clamping the blend there (`phase < 1 ? phase : 1`, C10 through
    // C14) FROZE the seat for those calls while MCC kept gliding the mesh,
    // then let it jump when the tick was finally seen: a 60 Hz forward/back
    // judder whose size is a fraction of a tick's travel, so it is invisible
    // when parked and violent under boost. The blend must stay continuous
    // through the tick boundary instead — the newest state is not a wall.
    double alpha = state.phase;
    // Extrapolation authority. It is 1 while ticks keep arriving and glides
    // away when the simulation stalls, so a stall stops the extrapolation
    // smoothly rather than snapping the seat back to the newest state.
    {
        const float liveTarget = ticksLive ? 1.0f : 0.0f;
        double glide = step > 0.0 && step < 0.25
            ? step / kHalo3PhaseApplySeconds
            : 1.0;
        if (glide > 1.0) glide = 1.0;
        state.live += static_cast<float>((liveTarget - state.live) * glide);
    }
    {
        // The lead engages only at a driving pace: below walking speed a
        // phase error is invisible, while extrapolating erratic
        // sub-centimetre physics steps is exactly the bouncing the manual
        // knob produced. The APPLIED value glides toward its target a step
        // per call rather than switching, so a simulation hitch, a stop or
        // an engage can never move the camera by the whole lead in one
        // headset frame.
        float target = 0.0f;
        if (ticksLive && state.tick > 0.0 && leadSeconds > 0.0)
        {
            // C14: advance to the moment these photons actually reach the
            // eye, in ticks. The bounding-centre witness (C12) stays a
            // DIAGNOSTIC only — the user's own log reported it "not
            // render-smooth" on real hardware, so it has never been observed
            // to measure anything, and a mechanism that has never worked
            // must not steer the camera.
            float ramp =
                (state.lastStepLen - kHalo3PhaseMinStep) / kHalo3PhaseMinStep;
            if (ramp < 0.0f) ramp = 0.0f;
            if (ramp > 1.0f) ramp = 1.0f;
            double ticksAhead = leadSeconds / state.tick;
            if (ticksAhead > kHalo3PhaseLeadMax)
                ticksAhead = kHalo3PhaseLeadMax;
            target = static_cast<float>(ticksAhead) * ramp;
        }
        double glide = step > 0.0 && step < 0.25
            ? step / kHalo3PhaseApplySeconds
            : 1.0;
        if (glide > 1.0) glide = 1.0;
        state.leadApplied += static_cast<float>(
            (target - state.leadApplied) * glide);
        alpha += static_cast<double>(state.leadApplied);
        if (lock)
            lock->leadApplied = state.leadApplied;   // diagnostic mirror
    }
    if (alpha < -0.25) alpha = -0.25;
    if (alpha > kHalo3PhaseAlphaMax) alpha = kHalo3PhaseAlphaMax;
    state.lastAlpha = static_cast<float>(alpha);
    // Between the two known states, the plain blend IS the answer.
    if (alpha <= 1.0)
        return Halo3LerpFrame(state.prev, state.cur, static_cast<float>(alpha));
    // Past the newest state, fade the extrapolation out with `live` so a
    // stalled simulation parks on that state instead of sailing on.
    double over = (alpha - 1.0) * static_cast<double>(state.live);
    if (over <= 0.0)
        return state.cur;
    // Continue along the last tick's motion. An acceleration-aware
    // (second-difference) prediction was built and MEASURED against this one
    // on a straight boost and on a hard banking turn: it was worth 0% and
    // 2.5% respectively, which does not justify the extra state or the risk
    // of a collision spike bending the camera, so it was dropped.
    Halo3Frame next = state.cur;
    for (int i = 0; i < 3; ++i)
    {
        next.pos[i] = state.cur.pos[i] + (state.cur.pos[i] - state.prev.pos[i]);
        next.fwd[i] = state.cur.fwd[i] + (state.cur.fwd[i] - state.prev.fwd[i]);
        next.up[i] = state.cur.up[i] + (state.cur.up[i] - state.prev.up[i]);
    }
    return Halo3LerpFrame(state.cur, next, static_cast<float>(over));
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

// C8 identity — the bounding-center fingerprint. Physics type + seat index is
// NOT a vehicle identity: (1,0) is both warthog and mongoose, (3,0) is ghost,
// wraith and mauler, and every mounted turret is (5,0,native=1). Expressing
// (bounding_sphere_center - position) in the object's OWN axes recovers that
// tag's render_model node-0 default translation — a per-tag constant, free
// from data the sampler already captures, and proven in a live session (it
// identified a Mongoose in a drive logged as a "warthog"). R^T * v, with R's
// columns the object's (fwd, left, up).
inline bool Halo3ComputeFingerprint(const Halo3Frame& frame,
                                    const float center[3], float out[3])
{
    if (!Halo3FrameOrthonormal(frame))
        return false;
    float left[3];
    Halo3FrameLeft(frame, left);
    float v[3];
    for (int i = 0; i < 3; ++i)
    {
        if (!std::isfinite(center[i]))
            return false;
        v[i] = center[i] - frame.pos[i];
    }
    out[0] = v[0] * frame.fwd[0] + v[1] * frame.fwd[1] + v[2] * frame.fwd[2];
    out[1] = v[0] * left[0] + v[1] * left[1] + v[2] * left[2];
    out[2] = v[0] * frame.up[0] + v[1] * frame.up[1] + v[2] * frame.up[2];
    return true;
}

inline float Halo3FingerprintDistance(const float a[3], const float b[3])
{
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]))
            return 1e9f;
        const float d = a[i] - b[i];
        d2 += d * d;
    }
    return std::sqrt(d2);
}

// A fingerprint identifies a cousin only when ONE candidate is close and
// every other is comfortably far. Ambiguity must never pick a winner: the
// caller then publishes no point and the stock chase camera stands, which is
// the whole safety property of this feature (a mislabel puts the player's
// head inside another vehicle's geometry; no label merely costs the effect).
inline constexpr float kHalo3FingerprintAccept = 0.15f;   // wu
inline constexpr float kHalo3FingerprintSeparate = 0.30f; // wu

inline bool Halo3FingerprintIsUnique(float bestDistance, float runnerUpDistance,
                                     float accept = kHalo3FingerprintAccept,
                                     float separate = kHalo3FingerprintSeparate)
{
    if (!std::isfinite(bestDistance))
        return false;
    if (bestDistance > accept)
        return false;
    if (!std::isfinite(runnerUpDistance))
        return true;   // only one candidate at all
    return runnerUpDistance >= separate;
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
    Carrier = 5,    // C8: mounted gunner anchored in the carrier's own frame
};

// C9 seat yaw — "the camera does not rotate with the car" (user, 2026-07-31).
// The FP seat camera moves the eye onto the authored point but leaves facing
// entirely HMD-owned in WORLD space (ApplyHeadLook builds its yaw from
// g_gameYawRef, which only a recenter or a cutscene boundary ever rebases).
// Drive a circle and the hull turns underneath a view pinned to the map, so
// "straight ahead" ends up out of the side of the vehicle. The fix is to add
// the hull's rotation since you sat down to that yaw reference.
//
// The rotation is applied by REBASING g_gameYawRef itself, one frame step at a
// time, rather than by publishing an offset consumers add. Everything the
// player sees is already expressed against that one reference — the view, the
// hand-aim ray (Game_ComputeAimStick), the rendered hands and gun
// (BuildTrackedGameBasis), the crosshair and the move stick — so rebasing it
// turns them all together and none can be forgotten. It also leaves the player
// facing the hull's final heading when they climb out, with no snap.
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
    float total = 0.0f;     // rotation folded into the reference (diagnostic)
};

// A hull heading needs a horizontal forward to exist at all. A Banshee pulled
// through the vertical has none, and atan2 of a near-zero pair swings wildly,
// so the follow re-references there instead of accumulating that swing.
inline constexpr float kHalo3SeatYawMinHorizontal = 0.25f;
// Ceiling on one frame's hull rotation. Halo 3's fastest authored turn rate is
// 9.4 rad/s (540 deg/s), which is 0.16 rad even across a whole 60 Hz sim tick,
// so anything past this is a teleport, respawn or seat swap — never steering.
inline constexpr float kHalo3SeatYawMaxStep = 0.60f;

// fwd = the seated parent's measured world forward (the CARRIER's for a mounted
// gunner). active = the FP seat camera owns the view this frame; a stale, torn
// or unproven transform must pass false, which re-references on the next entry.
// weight = vehicle_view_follow, 0 = the world-locked view Alpha 0.3.1 shipped,
// 1 = welded to the hull.
//
// Returns this frame's yaw step to fold into the shared view/aim reference —
// a delta, not a total. Rebasing the one reference every consumer already
// shares (view, hand aim, rendered hands, crosshair, move stick) is what keeps
// them together; publishing a separate offset would have to be added at each
// of those sites and any one that was missed would slide by exactly the hull
// angle. That same reference is the vehicle's steering channel, which is why
// this cannot ship without the driver-seat steering author below: once the
// reference follows the hull, the closed loop's error stops depending on the
// hull's heading and a fixed hand offset becomes a constant turn RATE.
inline float Halo3SeatYawDelta(Halo3SeatYaw& state, const float fwd[3],
                               bool active, float weight)
{
    if (!active || !std::isfinite(weight) || !std::isfinite(fwd[0]) ||
        !std::isfinite(fwd[1]))
    {
        state.armed = false;
        state.total = 0.0f;
        return 0.0f;
    }
    const float horizontal =
        std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1]);
    if (horizontal < kHalo3SeatYawMinHorizontal)
    {
        state.armed = false;
        return 0.0f;
    }
    const float yaw = std::atan2(fwd[1], fwd[0]);
    if (!state.armed)
    {
        // First seated frame: this heading is "straight ahead" from now on.
        state.armed = true;
        state.lastYaw = yaw;
        return 0.0f;
    }
    const float step = Halo3WrapPi(yaw - state.lastYaw);
    state.lastYaw = yaw;
    if (std::fabs(step) > kHalo3SeatYawMaxStep)
        return 0.0f;
    const float w = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
    const float delta = step * w;
    state.total = Halo3WrapPi(state.total + delta);
    return delta;
}


// ---- Virtual steering wheel --------------------------------------------
// Two gripped hands define a wheel by the line between them: its tilt out of
// horizontal, measured in the head's own heading plane, is the wheel angle. No
// fixed pivot, so the wheel is wherever the player's hands are and a grab
// never snaps the steering.
struct Halo3Wheel
{
    bool bothHeld = false;      // both grips are down right now
    bool engaged = false;       // the wheel is in your hands
    int clicks = 0;             // clicks banked toward a double-click
    uint64_t lastReleaseMs = 0;
    float steer = 0.0f;         // last authored steer, [-1, 1], + = right
};

inline constexpr float kHalo3WheelPressGrip = 0.6f;
inline constexpr float kHalo3WheelReleaseGrip = 0.4f;
inline constexpr uint64_t kHalo3WheelDoubleClickMs = 450;
inline constexpr float kHalo3WheelMinSpan = 0.15f;  // metres between hands

// Taking the wheel is a DOUBLE-CLICK of both grips, and letting go is another;
// it is not a hold. Two reasons, both from the user (2026-07-31):
//   * the right grip is already the dismount, so a sustained two-hand squeeze
//     would either eject the player or have to disable dismounting outright;
//   * nothing should have to be squeezed for the length of a drive.
// The grip BUTTONS are withheld from the game only while BOTH grips are down
// at once (Halo3WheelSwallowsGrips) — which is exactly this gesture and
// nothing else, so a lone right grip always dismounts, wheel on or off.
inline bool Halo3UpdateWheelToggle(Halo3Wheel& state, bool available,
                                   float gripLeft, float gripRight,
                                   uint64_t nowMs)
{
    if (!available || !std::isfinite(gripLeft) || !std::isfinite(gripRight))
    {
        state.bothHeld = false;
        state.engaged = false;
        state.clicks = 0;
        state.steer = 0.0f;
        return false;
    }
    // Hysteresis on the pair: down past 0.6, up below 0.4, so a hand resting
    // near the threshold cannot rattle out a click it never meant.
    const bool both = state.bothHeld
        ? (gripLeft >= kHalo3WheelReleaseGrip &&
           gripRight >= kHalo3WheelReleaseGrip)
        : (gripLeft > kHalo3WheelPressGrip &&
           gripRight > kHalo3WheelPressGrip);
    bool toggled = false;
    if (state.bothHeld && !both)
    {
        // A click completes on release, so the second release is the toggle.
        if (state.clicks == 1 &&
            nowMs - state.lastReleaseMs <= kHalo3WheelDoubleClickMs)
        {
            state.engaged = !state.engaged;
            state.clicks = 0;
            if (!state.engaged)
                state.steer = 0.0f;
            toggled = true;
        }
        else
        {
            state.clicks = 1;
            state.lastReleaseMs = nowMs;
        }
    }
    state.bothHeld = both;
    return toggled;
}

// The one condition under which the input hook withholds the grip buttons.
inline constexpr bool Halo3WheelSwallowsGrips(const Halo3Wheel& state)
{
    return state.bothHeld;
}

// left/right = tracked hand positions in OpenXR room space (Y up, -Z forward).
// headYaw = the room-space head heading (atan2(fwd.x, -fwd.z)), which is the
// plane the wheel is read in, so it works whichever way the player is facing.
// Returns true while the wheel owns steering; state.steer holds the value.
inline bool Halo3ComputeWheelSteer(Halo3Wheel& state, const float left[3],
                                   const float right[3], float headYaw,
                                   float maxDeg, float deadzoneDeg)
{
    const bool finite = std::isfinite(headYaw) && std::isfinite(left[0]) &&
        std::isfinite(left[1]) && std::isfinite(left[2]) &&
        std::isfinite(right[0]) && std::isfinite(right[1]) &&
        std::isfinite(right[2]);
    if (!state.engaged || !finite)
    {
        state.steer = 0.0f;
        return false;
    }

    const float dx = right[0] - left[0];
    const float dy = right[1] - left[1];
    const float dz = right[2] - left[2];
    const float span = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (span < kHalo3WheelMinSpan)
    {
        // Hands together is not a wheel. Because this is a toggle rather than
        // a hold, hands dropped into a lap must read as straight ahead — a
        // held-over steer would keep turning with nobody driving.
        state.steer = 0.0f;
        return true;
    }
    // Room right at the head's heading: forward is (sin, 0, -cos), so right is
    // (cos, 0, sin).
    const float lateral = dx * std::cos(headYaw) + dz * std::sin(headYaw);
    constexpr float kHalfPi = 1.57079633f;
    const float angle = lateral > 0.05f * span
        ? std::atan2(dy, lateral)
        : (dy >= 0.0f ? kHalfPi : -kHalfPi); // past 87 deg / hands crossed
    const float degrees = angle * 57.2957795f;
    const float dead = deadzoneDeg < 0.0f ? 0.0f : deadzoneDeg;
    const float full = maxDeg > dead + 1.0f ? maxDeg : dead + 1.0f;
    float magnitude = std::fabs(degrees) - dead;
    if (magnitude < 0.0f) magnitude = 0.0f;
    magnitude /= (full - dead);
    if (magnitude > 1.0f) magnitude = 1.0f;
    // Right hand UP tilts the wheel counter-clockwise, which is a left turn.
    state.steer = degrees > 0.0f ? -magnitude : magnitude;
    return true;
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

// C8 vehicle identity. Physics type is a CLASS, not a vehicle: (1,seat 0) is
// warthog AND mongoose, (3,seat 0) is ghost, wraith AND mauler. The C7
// session log (2026-07-31 09:32, Steam, cand ebd0e14) confirmed IN PROCESS
// that the §E5 definition reads work — the block-count array indexed exactly
// by physics type (jeep [0100000000], turret [0000010000], chopper
// [0000000010], tank [1000000000]) and `engine_moment` came back 2000.0 on
// the warthog and 650.0 on the mongoose, the two values E5 predicted offline.
// `turn_rate` resolved as radians (9.4 = 540 deg hog, 6.3 = 360 deg
// mongoose), which is the same fact in the field's own units.
enum class Halo3VehicleId : uint8_t
{
    Unknown = 0,   // never keys a camera: no point -> stock chase view
    Scorpion,
    Warthog,
    Mongoose,
    Ghost,
    Wraith,
    Mauler,        // the Prowler
    Banshee,
    Hornet,
    Chopper,
    // A turret the player walks up to and uses (physics type 5 with no
    // carrier under it). Not resolved from definition content: the machinegun
    // turret, plasma cannon and missile pod are not separable offline, and
    // their authored eye points sit within 0.09 wu (27 cm) of each other, so
    // one shared point is within authoring noise for all three.
    StationaryTurret,
};

// Definition-resident fields the sampler resolves ONCE per seat session (on a
// definition-index change), never per frame.
struct Halo3DefinitionFields
{
    int physicsType = -1;
    bool jeepValid = false;
    float engineMoment = 0.0f;
    bool scoutValid = false;
    int specificType = -1;
};

// Tolerances are wide enough for any float representation of the authored
// constant and far narrower than the gap between the two authored values
// (650 vs 2000). Anything unrecognised stays Unknown — a wrong identity puts
// the player's head inside another vehicle, a missing one costs the effect.
inline constexpr float kHalo3WarthogEngineMoment = 2000.0f;
inline constexpr float kHalo3MongooseEngineMoment = 650.0f;
inline constexpr float kHalo3EngineMomentTolerance = 200.0f;

inline Halo3VehicleId Halo3ResolveVehicleId(const Halo3DefinitionFields& def)
{
    switch (def.physicsType)
    {
    case 0: return Halo3VehicleId::Scorpion;      // human_tank
    case 4: return Halo3VehicleId::Banshee;       // alien_fighter
    case 7: return Halo3VehicleId::Hornet;        // vtol
    case 8: return Halo3VehicleId::Chopper;       // chopper
    case 1:                                        // human_jeep
        if (!def.jeepValid || !std::isfinite(def.engineMoment))
            return Halo3VehicleId::Unknown;
        if (std::fabs(def.engineMoment - kHalo3WarthogEngineMoment) <=
            kHalo3EngineMomentTolerance)
            return Halo3VehicleId::Warthog;
        if (std::fabs(def.engineMoment - kHalo3MongooseEngineMoment) <=
            kHalo3EngineMomentTolerance)
            return Halo3VehicleId::Mongoose;
        return Halo3VehicleId::Unknown;
    case 3:                                        // alien_scout
        if (!def.scoutValid)
            return Halo3VehicleId::Unknown;
        if (def.specificType == 1) return Halo3VehicleId::Ghost;
        if (def.specificType == 3) return Halo3VehicleId::Wraith;
        if (def.specificType == 4) return Halo3VehicleId::Mauler;
        return Halo3VehicleId::Unknown;
    default:
        return Halo3VehicleId::Unknown;            // incl. 5 (turret)
    }
}

// User-authored first-person seat points (Blender kit, model-space world
// units, relative to the DIRECT parent object's origin and basis).
// Keyed by RESOLVED VEHICLE IDENTITY, not physics type, so a mongoose can
// never wear the warthog's eye point. `carrierFrame` marks the mounted-turret
// gunner seats: those points are authored in the CARRIER's frame (and
// `vehicle` is then the CARRIER's identity), because an attached child stores
// its transform relative to the parent's ATTACHMENT NODE, which left C7's
// carrier-origin composition ~0.42 wu (1.3 m) low. Authoring in carrier space
// sidesteps the node matrix entirely; nothing is lost, as a mounted turret's
// object frame is mount-fixed and does not follow gun aim.
//
// Values are the user's own Blender placements (kit v3, every vehicle at the
// world origin, metres / 3.048), read back 2026-07-31 with the parse guard
// that refuses any camera whose evaluated and stored transforms disagree.
struct Halo3SeatPoint
{
    Halo3VehicleId vehicle;
    int8_t seatIndex;
    bool carrierFrame;
    float x, y, z;
};

inline constexpr Halo3SeatPoint kHalo3SeatPoints[] = {
    // Root vehicles — the seated parent owns the frame.
    {Halo3VehicleId::Warthog,  0, false,  0.0299f,  0.1683f, 0.6663f},
    {Halo3VehicleId::Warthog,  1, false, -0.0139f, -0.1903f, 0.6601f},
    {Halo3VehicleId::Mongoose, 0, false, -0.0476f,  0.0000f, 0.5207f},
    {Halo3VehicleId::Mongoose, 1, false, -0.4368f,  0.0000f, 0.5967f},
    {Halo3VehicleId::Ghost,    0, false, -0.1078f,  0.0000f, 0.4491f},
    {Halo3VehicleId::Wraith,   0, false,  0.3060f,  0.0000f, 1.0573f},
    {Halo3VehicleId::Mauler,   0, false, -1.0738f,  0.0000f, 0.6381f},
    {Halo3VehicleId::Mauler,   1, false, -0.2252f,  0.4659f, 0.7430f},
    {Halo3VehicleId::Mauler,   2, false, -0.2389f, -0.4790f, 0.6764f},
    {Halo3VehicleId::Chopper,  0, false, -0.4396f,  0.0000f, 0.7348f},
    {Halo3VehicleId::Banshee,  0, false,  0.4190f,  0.0000f, 0.4119f},
    {Halo3VehicleId::Hornet,   0, false,  1.0059f, -0.0098f, 0.6517f},
    {Halo3VehicleId::Hornet,   1, false,  0.5215f,  0.3566f, 0.5583f},
    {Halo3VehicleId::Hornet,   2, false,  0.5208f, -0.3379f, 0.5830f},
    // Scorpion driver: the user's forward/height, with the lateral the tag
    // itself authors (+0.189, the hatch is left of the turret ring). Their
    // saved file still had this one camera untouched on the centre line.
    {Halo3VehicleId::Scorpion, 0, false,  0.0326f,  0.1893f, 1.0226f},

    // Mounted turret gunners — identified and anchored by the CARRIER.
    {Halo3VehicleId::Warthog,  0, true,  -0.5832f,  0.0000f, 1.0624f},
    {Halo3VehicleId::Scorpion, 0, true,   0.4673f, -0.1903f, 0.8659f},
    {Halo3VehicleId::Wraith,   0, true,  -0.1886f,  0.0000f, 1.1956f},
    {Halo3VehicleId::Mauler,   0, true,   0.0054f,  0.0000f, 0.9307f},

    // Walk-up turrets — the machinegun turret's point, shared (see the enum).
    {Halo3VehicleId::StationaryTurret, 0, false, -0.2368f, 0.0000f, 0.5743f},
};

// `vehicle` is the seated parent's identity for a normal seat, or the
// CARRIER's identity when the player is a mounted-turret gunner. Unknown
// never matches, so an unidentified vehicle always falls back to stock.
inline constexpr const Halo3SeatPoint* Halo3FindSeatPoint(
    Halo3VehicleId vehicle, int seatIndex, bool mountedTurret)
{
    if (vehicle == Halo3VehicleId::Unknown)
        return nullptr;
    for (const Halo3SeatPoint& p : kHalo3SeatPoints)
    {
        if (p.vehicle == vehicle && p.carrierFrame == mountedTurret &&
            static_cast<int>(p.seatIndex) == seatIndex)
            return &p;
    }
    return nullptr;
}

// The seat that steers. Every vehicle in the authored table puts its driver at
// seat 0 of the vehicle itself; a mounted turret gunner is seat 0 of the GUN,
// and a walk-up turret has no vehicle under it at all. Unknown never steers —
// an unidentified vehicle keeps the stock closed-loop aim it has today.
inline constexpr bool Halo3SeatIsDriver(Halo3VehicleId id, int seatIndex,
                                        bool mountedTurret)
{
    return !mountedTurret && seatIndex == 0 &&
        id != Halo3VehicleId::Unknown &&
        id != Halo3VehicleId::StationaryTurret;
}

// Halo 3 drives its vehicles two different ways. Most are LOOK-STEERED: the
// right stick turns the vehicle, and the engine's "aim" for that seat is the
// hull's own heading. The Scorpion and the Wraith are not — their hull turns
// from the left stick and the right stick aims a weapon that swings
// independently of the hull.
//
// Only the first family may have its steering authored, and it is exactly the
// family the hull-follow view breaks: when the aim IS the hull, rebasing the
// reference by the hull's rotation cancels the closed loop's feedback. In the
// second family the aim is a separate heading, so the loop keeps real feedback
// and must be left alone — the driver is aiming a turret, not steering.
// Getting this wrong for a vehicle costs that vehicle its motion steering,
// never its controls.
inline constexpr bool Halo3VehicleIsLookSteered(Halo3VehicleId id)
{
    switch (id)
    {
    case Halo3VehicleId::Warthog:
    case Halo3VehicleId::Mongoose:
    case Halo3VehicleId::Ghost:
    case Halo3VehicleId::Mauler:
    case Halo3VehicleId::Chopper:
    case Halo3VehicleId::Banshee:
    case Halo3VehicleId::Hornet:
        return true;
    default: // Scorpion, Wraith, turrets, Unknown
        return false;
    }
}

// A wheel is a ground control. Aircraft are look-steered too, so they also need
// their steering authored, but a wheel is the wrong shape for one: their stick
// is a flight control and its vertical axis is climb. They take the plain stick
// here and a two-hand yoke later.
inline constexpr bool Halo3VehicleUsesWheel(Halo3VehicleId id)
{
    return Halo3VehicleIsLookSteered(id) &&
        id != Halo3VehicleId::Banshee && id != Halo3VehicleId::Hornet;
}

// Which seats may take the hull-follow view at all. The hazard the follow
// creates is specific: if the frame it follows is the same heading the engine's
// aim channel drives, the closed-loop hand aim loses its feedback and a fixed
// hand offset becomes a runaway turn. Seat by seat:
//   * driver of a look-steered vehicle — the aim IS the hull, so it is only
//     allowed because the steering author below replaces that loop;
//   * every other seat in the table — the frame is the hull (or, for a mounted
//     gunner, the CARRIER) while the aim is a turret or the occupant's own
//     weapon, so the loop keeps real feedback;
//   * a walk-up turret has no vehicle under it, so the frame published for that
//     seat is the turret's own object, which may well be what its aim turns.
//     Nothing authors its steering, so it is left exactly as it is;
//   * Unknown has no authored point, so its camera is the stock chase view and
//     there is no "the camera does not turn" to fix.
inline constexpr bool Halo3SeatFollowsHull(Halo3VehicleId id, int seatIndex,
                                           bool mountedTurret)
{
    if (id == Halo3VehicleId::Unknown ||
        id == Halo3VehicleId::StationaryTurret)
        return false;
    return Halo3FindSeatPoint(id, seatIndex, mountedTurret) != nullptr;
}

// The one gate that hands a seat's steering to the wheel/stick author instead
// of the closed-loop hand aim. It must switch on exactly the same condition
// that makes the view follow the hull, which is why the follow weight is an
// argument: at weight 0 nothing rotates, the closed loop still has its
// feedback, and every control stays exactly as Alpha 0.3.1 shipped it.
inline constexpr bool Halo3SeatAuthorsSteering(Halo3VehicleId id, int seatIndex,
                                               bool mountedTurret,
                                               float followWeight)
{
    return followWeight > 0.01f && Halo3SeatIsDriver(id, seatIndex,
                                                     mountedTurret) &&
        Halo3VehicleIsLookSteered(id);
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
