#pragma once

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
