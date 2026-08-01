#pragma once

#include <cmath>
#include <cstdint>

inline uint32_t ClassifyAuthoredReticleColorOrdering(const float channel[4])
{
    if (!channel)
        return 0;
    for (unsigned int i = 0; i < 4; ++i)
    {
        const float value = channel[i];
        if (!std::isfinite(value) || value < -0.01f || value > 4.0f)
            return 0;
    }

    // Encode pairwise ordering instead of naming channels. Halo 3's placement
    // output is known to contain four colour/alpha floats, but its alpha slot
    // need not be assumed here. Uniform opacity scaling preserves ordering and
    // therefore cannot create recurring swapchain uploads.
    uint32_t ordering = 1;
    unsigned int shift = 1;
    for (unsigned int i = 0; i < 4; ++i)
    {
        for (unsigned int j = i + 1; j < 4; ++j)
        {
            const float tolerance = 0.05f *
                std::fmax(std::fabs(channel[i]), std::fabs(channel[j]));
            if (channel[i] > channel[j] + tolerance)
                ordering |= 1u << shift;
            else if (channel[j] > channel[i] + tolerance)
                ordering |= 2u << shift;
            shift += 2;
        }
    }
    return ordering;
}

enum class AuthoredReticleRefreshPolicy : uint8_t
{
    IdentityAndColorState,
    IdentityImmediate,
    BoundedAnimation,
};

// Floor on how often the blocking OpenXR acquire/wait/copy/release may run.
// Measured at ~4-5 ms of the render window, which is the whole margin between
// a 120 Hz budget (8.33 ms) and missing it, so it must never run every frame.
inline constexpr uint64_t kMinimumUploadGapFrames = 6;

struct AuthoredReticleRefreshState
{
    uint64_t ownerEpoch = 0;
    uint64_t lastPublishedKey = 0;
    uint64_t lastUploadFrame = 0;
    uint64_t settlingKey = 0;
    uint64_t settlingSinceFrame = 0;
    uint32_t lastPublishedColorState = 0;
};

inline void ResetAuthoredReticleRefreshState(
    AuthoredReticleRefreshState& state, uint64_t ownerEpoch)
{
    state = {};
    state.ownerEpoch = ownerEpoch;
}

inline bool ShouldUploadAuthoredReticle(
    AuthoredReticleRefreshPolicy policy,
    bool capturedThisFrame,
    bool uploadAdmitted,
    uint64_t capturedKey,
    uint32_t capturedColorState,
    uint64_t frameSerial,
    uint64_t ownerEpoch,
    AuthoredReticleRefreshState& state)
{
    if (state.ownerEpoch != ownerEpoch)
        ResetAuthoredReticleRefreshState(state, ownerEpoch);

    if (!capturedThisFrame || !uploadAdmitted || capturedKey == 0)
        return false;

    if (state.lastUploadFrame != 0 && frameSerial >= state.lastUploadFrame &&
        frameSerial - state.lastUploadFrame < kMinimumUploadGapFrames)
        return false;

    if (policy == AuthoredReticleRefreshPolicy::BoundedAnimation)
        return true;
    if (policy == AuthoredReticleRefreshPolicy::IdentityImmediate)
        return capturedKey != state.lastPublishedKey;

    // Once Halo 3 has published a settled widget, a categorical CHUD colour
    // transition is safe to publish immediately. It costs one upload per real
    // blue/green/red edge and no work while the state is unchanged.
    if (capturedKey == state.lastPublishedKey && capturedColorState != 0 &&
        capturedColorState != state.lastPublishedColorState)
        return true;

    if (capturedKey == state.lastPublishedKey)
        return false;

    // A new widget identity can precede visible pixels during level/weapon
    // transitions. Settle that identity before its first publish so a blank
    // capture cannot replace good held art.
    constexpr uint64_t kHalo3IdentitySettleFrames = 24;
    if (capturedKey != state.settlingKey || frameSerial < state.settlingSinceFrame)
    {
        state.settlingKey = capturedKey;
        state.settlingSinceFrame = frameSerial;
        return false;
    }
    return frameSerial - state.settlingSinceFrame >=
        kHalo3IdentitySettleFrames;
}

inline void MarkAuthoredReticleUploaded(
    AuthoredReticleRefreshState& state,
    uint64_t capturedKey,
    uint32_t capturedColorState,
    uint64_t frameSerial)
{
    state.lastPublishedKey = capturedKey;
    state.lastPublishedColorState = capturedColorState;
    state.lastUploadFrame = frameSerial;
    state.settlingKey = 0;
    state.settlingSinceFrame = 0;
}

inline bool AuthoredReticleLayerHasContent(
    bool titleCapturesAuthoredArt,
    bool authoredArtHeld)
{
    return !titleCapturesAuthoredArt || authoredArtHeld;
}

// Offscreen CHUD capture cadence.
//
// Capturing is not free: every class-2 widget piece redirects the render
// target, saves and restores viewport/scissor/RTV state on the immediate
// context, and draws itself into the private reticle texture. A title that
// already holds valid released art only has to re-sample the game's widget
// often enough to see it change - between samples the compositor keeps
// showing the last released image at no cost. This is the mechanism ODST
// already ships (`60f3929`); the gap is the only per-title difference.
//
// Every widget drawn during a selected frame must be admitted, so a compound
// reticle is captured whole rather than in pieces from different frames.
inline bool ShouldSampleAuthoredCapture(
    uint64_t gapFrames, uint64_t lastSampleSerial, uint64_t frameSerial)
{
    if (gapFrames == 0 || lastSampleSerial == 0)
        return true;
    if (frameSerial == lastSampleSerial)
        return true;
    // A serial restart (title change, session restart) re-samples immediately
    // rather than waiting out a gap measured against a stale frame number.
    if (frameSerial < lastSampleSerial)
        return true;
    return frameSerial - lastSampleSerial >= gapFrames;
}

// Halo 3's authored crosshair animates - it kicks when the weapon fires and
// turns red on a hostile / green on a friendly target - and none of that
// changes which widgets drew, so an identity-keyed publish freezes one
// snapshot. Publishing on a bounded cadence is what makes it live again, and
// the same cadence throttles the capture that feeds it, so the animation is
// paid for out of work the title was already doing every frame.
//
// 0 disables the animation entirely and holds one captured image, which is
// the cheapest possible behavior. Anything else is clamped to at least
// `kMinimumUploadGapFrames`, because a shorter gap cannot produce extra
// publishes anyway - it would only spend capture work that the upload floor
// then throws away.
inline uint64_t ResolveAuthoredAnimationGapFrames(int configuredFrames)
{
    constexpr int kMaximumAnimationGapFrames = 60;
    if (configuredFrames <= 0)
        return 0;
    if (configuredFrames < static_cast<int>(kMinimumUploadGapFrames))
        return kMinimumUploadGapFrames;
    if (configuredFrames > kMaximumAnimationGapFrames)
        return static_cast<uint64_t>(kMaximumAnimationGapFrames);
    return static_cast<uint64_t>(configuredFrames);
}
