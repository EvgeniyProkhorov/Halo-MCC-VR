#pragma once

#include <algorithm>
#include <cmath>

// Halo 3's authored theatre framing needs a headset-tuned correction.
// Scaling both tangents equally preserves the authored aspect ratio. This is
// title-adapter policy, not part of the universal theatre compositor.
inline constexpr float kHalo3CutsceneTheaterFovScale = 0.85f;

struct Halo3CutsceneTheaterFrustum
{
    bool valid = false;
    float projectionTangentX = 0.0f;
    float projectionTangentY = 0.0f;
    float cullingTangentX = 0.0f;
    float cullingTangentY = 0.0f;
};

inline bool Halo3ProjectionTangentValid(float tangent) noexcept
{
    return std::isfinite(tangent) && tangent > 0.0001f && tangent < 100.0f;
}

inline Halo3CutsceneTheaterFrustum SelectHalo3CutsceneTheaterFrustum(
    bool theaterActive,
    float authoredTangentX, float authoredTangentY,
    float immersiveTangentX, float immersiveTangentY) noexcept
{
    Halo3CutsceneTheaterFrustum result{};
    if (!Halo3ProjectionTangentValid(immersiveTangentX) ||
        !Halo3ProjectionTangentValid(immersiveTangentY))
    {
        return result;
    }

    result.projectionTangentX = immersiveTangentX;
    result.projectionTangentY = immersiveTangentY;
    result.cullingTangentX = immersiveTangentX;
    result.cullingTangentY = immersiveTangentY;
    result.valid = true;
    if (!theaterActive)
        return result;

    if (!Halo3ProjectionTangentValid(authoredTangentX) ||
        !Halo3ProjectionTangentValid(authoredTangentY))
    {
        return {};
    }

    result.projectionTangentX =
        authoredTangentX * kHalo3CutsceneTheaterFovScale;
    result.projectionTangentY =
        authoredTangentY * kHalo3CutsceneTheaterFovScale;
    if (!Halo3ProjectionTangentValid(result.projectionTangentX) ||
        !Halo3ProjectionTangentValid(result.projectionTangentY))
    {
        return {};
    }

    // Halo 3 derives visibility from the compact-camera tangents before it
    // consumes the derived projection matrix. Keep that visibility volume at
    // least as wide as both the authored camera and the already proven
    // immersive OpenXR cover, while the derived matrix retains the tighter
    // theatre framing.
    result.cullingTangentX =
        std::max(authoredTangentX, immersiveTangentX);
    result.cullingTangentY =
        std::max(authoredTangentY, immersiveTangentY);
    return result;
}
