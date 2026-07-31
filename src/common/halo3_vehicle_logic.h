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
