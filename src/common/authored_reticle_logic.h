#pragma once

#include <cstdint>

enum class AuthoredReticleRefreshPolicy : uint8_t
{
    IdentityChange,
    BoundedAnimation,
};

inline bool ShouldUploadAuthoredReticle(
    AuthoredReticleRefreshPolicy policy,
    bool capturedThisFrame,
    bool uploadAdmitted,
    uint64_t capturedKey,
    bool keyAlreadyPublished,
    bool uploadTooSoon)
{
    if (!capturedThisFrame || !uploadAdmitted || capturedKey == 0 ||
        uploadTooSoon)
    {
        return false;
    }

    return policy == AuthoredReticleRefreshPolicy::BoundedAnimation ||
        !keyAlreadyPublished;
}

inline bool AuthoredReticleLayerHasContent(
    bool titleCapturesAuthoredArt,
    bool authoredArtHeld)
{
    return !titleCapturesAuthoredArt || authoredArtHeld;
}
