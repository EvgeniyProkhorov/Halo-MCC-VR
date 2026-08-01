#pragma once

#include <cmath>
#include <cstdint>

// Halo 3's Cortana and Gravemind channels are the only H3EK scripts that set
// player_control_scale_all_input to 0.15. Observe that title-native signal
// without treating ordinary scripted slowdowns as VR input ownership.
inline constexpr float kHalo3ScriptedInputSuppressionScale = 0.15f;
inline constexpr float kHalo3ScriptedInputRestoreScale = 1.0f;
inline constexpr float kHalo3ScriptedInputScaleTolerance = 0.001f;

// H3EK and the pinned retail homologue both use four 0x30-byte input-user
// records. In pinned retail, the array is reached through the pointer at engine
// TLS +0xC0 (H3EK's development layout uses +0x1960). These constants describe
// the retail fields observed by the optional native updater; none is modified.
inline constexpr uint8_t kHalo3ScriptedInputUserCount = 4;
inline constexpr uintptr_t kHalo3ScriptedInputRecordsTlsOffset = 0xC0;
inline constexpr uintptr_t kHalo3ScriptedInputRecordStride = 0x30;
inline constexpr uintptr_t kHalo3ScriptedInputActiveOffset = 0x1C;
inline constexpr uintptr_t kHalo3ScriptedInputCurrentOffset = 0x20;
inline constexpr uintptr_t kHalo3ScriptedInputTargetOffset = 0x24;
inline constexpr uintptr_t kHalo3ScriptedInputRemainingOffset = 0x28;
inline constexpr float kHalo3ScriptedInputRemainingTolerance = 0.001f;

enum class Halo3ScriptedInputAction : uint8_t
{
    Ignore = 0,
    SuppressVrTurning,
    RestoreVrTurningWhenNativeComplete,
};

struct Halo3ScriptedInputPolicy
{
    Halo3ScriptedInputAction action = Halo3ScriptedInputAction::Ignore;
};

enum class Halo3ScriptedInputPhase : uint8_t
{
    Idle = 0,
    Suppressed,
    Restoring,
};

enum class Halo3ScriptedInputObservation : uint8_t
{
    Hold = 0,
    NativeSuppressed,
    NativeRestoring,
    Restored,
    Invalid,
};

struct Halo3ScriptedInputRecordState
{
    bool stable = false;
    uint8_t active = 0;
    float currentScale = 1.0f;
    float targetScale = 1.0f;
    float remainingSeconds = 0.0f;
};

inline bool Halo3ScriptedInputScaleMatches(float targetScale,
                                            float authoredScale) noexcept
{
    return std::isfinite(targetScale) &&
        std::fabs(targetScale - authoredScale) <=
            kHalo3ScriptedInputScaleTolerance;
}

inline bool Halo3ScriptedInputRecordIsFinite(
    const Halo3ScriptedInputRecordState& record) noexcept
{
    return record.active <= 1 &&
        std::isfinite(record.currentScale) &&
        std::isfinite(record.targetScale) &&
        std::isfinite(record.remainingSeconds);
}

inline bool Halo3ScriptedInputRecordShowsNativeSuppression(
    const Halo3ScriptedInputRecordState& record) noexcept
{
    return record.stable && Halo3ScriptedInputRecordIsFinite(record) &&
        record.remainingSeconds >= -kHalo3ScriptedInputRemainingTolerance &&
        record.active != 0 &&
        Halo3ScriptedInputScaleMatches(
            record.targetScale, kHalo3ScriptedInputSuppressionScale);
}

inline Halo3ScriptedInputPolicy ClassifyHalo3ScriptedInputScale(
    float targetScale, float nativeDurationSeconds) noexcept
{
    if (!std::isfinite(nativeDurationSeconds) ||
        nativeDurationSeconds < 0.0f)
        return {};

    if (Halo3ScriptedInputScaleMatches(
            targetScale, kHalo3ScriptedInputSuppressionScale))
    {
        return { Halo3ScriptedInputAction::SuppressVrTurning };
    }

    if (Halo3ScriptedInputScaleMatches(
            targetScale, kHalo3ScriptedInputRestoreScale))
    {
        return {
            Halo3ScriptedInputAction::RestoreVrTurningWhenNativeComplete
        };
    }

    return {};
}

// Interpret a coherent snapshot of the native records selected when the 0.15
// request was observed. Mixed 0.15/1.0 targets are a setter-in-progress race,
// so the native observer holds its exact versioned state and retries. A restore
// is complete only when Halo itself clears active with target/current at 1.0.
// Remaining duration is validated for finiteness, but is not a completion gate:
// retail can clear active with positive duration after a duplicate unity setter.
inline Halo3ScriptedInputObservation ObserveHalo3ScriptedInputRecords(
    Halo3ScriptedInputPhase phase,
    const Halo3ScriptedInputRecordState* records,
    uint8_t userMask) noexcept
{
    if (phase == Halo3ScriptedInputPhase::Idle || !records || userMask == 0)
        return Halo3ScriptedInputObservation::Invalid;

    bool sawSuppressed = false;
    bool sawRestoring = false;
    bool restoreComplete = true;
    for (uint8_t user = 0; user < kHalo3ScriptedInputUserCount; ++user)
    {
        if ((userMask & static_cast<uint8_t>(1u << user)) == 0)
            continue;
        const Halo3ScriptedInputRecordState& record = records[user];
        if (!record.stable)
            return Halo3ScriptedInputObservation::Hold;
        if (!Halo3ScriptedInputRecordIsFinite(record))
            return Halo3ScriptedInputObservation::Invalid;
        if (record.remainingSeconds <
            -kHalo3ScriptedInputRemainingTolerance)
            return Halo3ScriptedInputObservation::Invalid;

        if (Halo3ScriptedInputScaleMatches(
                record.targetScale,
                kHalo3ScriptedInputSuppressionScale))
        {
            if (record.active == 0)
                return Halo3ScriptedInputObservation::Invalid;
            sawSuppressed = true;
            continue;
        }
        if (!Halo3ScriptedInputScaleMatches(
                record.targetScale, kHalo3ScriptedInputRestoreScale))
            return Halo3ScriptedInputObservation::Invalid;

        sawRestoring = true;
        if (record.active != 0)
            restoreComplete = false;
        else if (!Halo3ScriptedInputScaleMatches(
                     record.currentScale,
                     kHalo3ScriptedInputRestoreScale))
            return Halo3ScriptedInputObservation::Invalid;
    }

    if (sawSuppressed && sawRestoring)
        return Halo3ScriptedInputObservation::Hold;
    if (sawSuppressed)
        return Halo3ScriptedInputObservation::NativeSuppressed;
    if (!sawRestoring)
        return Halo3ScriptedInputObservation::Invalid;
    return restoreComplete
        ? Halo3ScriptedInputObservation::Restored
        : Halo3ScriptedInputObservation::NativeRestoring;
}
