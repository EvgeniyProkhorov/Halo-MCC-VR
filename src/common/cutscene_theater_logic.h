#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "runtime_types.h"

inline constexpr uint64_t kCutsceneTheaterFadeMs = 100;
inline constexpr uint64_t kCinematicControlFreshMs = 500;

struct CinematicControlPublication
{
    GameTitle title = GameTitle::None;
    uint32_t generation = 0;
    CinematicControlState state = CinematicControlState::Unknown;
    uint64_t heartbeatMs = 0;
};

struct CutsceneTheaterProjectionPublication
{
    GameTitle title = GameTitle::None;
    uint32_t generation = 0;
    float authoredAspect = 0.0f;
    uint64_t heartbeatMs = 0;
};

inline float CutsceneTheaterAspectFromTangents(
    float horizontalTangent, float verticalTangent) noexcept
{
    if (!std::isfinite(horizontalTangent) ||
        !std::isfinite(verticalTangent) ||
        horizontalTangent <= 0.0001f || verticalTangent <= 0.0001f)
    {
        return 0.0f;
    }
    const float aspect = horizontalTangent / verticalTangent;
    return std::isfinite(aspect) && aspect >= 0.25f && aspect <= 4.0f
        ? aspect : 0.0f;
}

inline float CutsceneTheaterAspectFromProjectionScales(
    float projectionX, float projectionY) noexcept
{
    if (!std::isfinite(projectionX) || !std::isfinite(projectionY) ||
        std::fabs(projectionX) <= 0.0001f ||
        std::fabs(projectionY) <= 0.0001f)
    {
        return 0.0f;
    }
    // tan(horizontal half-FOV) / tan(vertical half-FOV) = P[5] / P[0].
    return CutsceneTheaterAspectFromTangents(
        std::fabs(projectionY), std::fabs(projectionX));
}

inline float CutsceneTheaterAspectFromDimensions(
    uint32_t width, uint32_t height) noexcept
{
    if (!width || !height)
        return 0.0f;
    const float aspect = static_cast<float>(width) /
        static_cast<float>(height);
    return std::isfinite(aspect) && aspect >= 0.25f && aspect <= 4.0f
        ? aspect : 0.0f;
}

inline bool CutsceneTheaterProjectionMatchesAspect(
    float authoredAspect, float projectionX, float projectionY,
    float tolerance = 0.001f) noexcept
{
    const float actualAspect =
        CutsceneTheaterAspectFromProjectionScales(projectionX, projectionY);
    return std::isfinite(authoredAspect) && authoredAspect >= 0.25f &&
        authoredAspect <= 4.0f && actualAspect > 0.0f &&
        std::isfinite(tolerance) && tolerance >= 0.0f &&
        std::fabs(actualAspect - authoredAspect) <= tolerance;
}

inline bool CutsceneTheaterRequested(
    bool enabled, CinematicControlState state) noexcept
{
    return enabled && state == CinematicControlState::AuthoredLocked;
}

inline bool ShouldDisableWidescreenCinematicFov(
    bool modEnabled, bool stereoEnabled, bool theaterActive) noexcept
{
    // The legacy widening remains exactly the immersive-VR policy. Theatre
    // restores the engine's stock authored cinematic FOV instead.
    return modEnabled && stereoEnabled && !theaterActive;
}

inline bool UseAuthoredCutsceneProjection(
    bool theaterActive, float authoredAspect) noexcept
{
    return theaterActive && std::isfinite(authoredAspect) &&
        authoredAspect >= 0.25f && authoredAspect <= 4.0f;
}

inline CinematicControlState ClassifyHalo3FamilyCinematicControl(
    bool globalsAvailable, bool cinematicInProgress,
    bool shotStateAvailable) noexcept
{
    if (!globalsAvailable)
        return CinematicControlState::Unknown;
    if (!cinematicInProgress)
        return CinematicControlState::PlayerControlled;
    return shotStateAvailable
        ? CinematicControlState::AuthoredLocked
        : CinematicControlState::Unknown;
}

inline CinematicControlState ClassifyReachCinematicControl(
    bool cinematicGlobalsProven, uint32_t stateWord) noexcept
{
    if (!cinematicGlobalsProven)
        return CinematicControlState::Unknown;
    return (stateWord & 0xFFu) != 0
        ? CinematicControlState::AuthoredLocked
        : CinematicControlState::PlayerControlled;
}

inline CinematicControlState ResolveCinematicControl(
    const CinematicControlPublication& publication,
    GameTitle activeTitle, uint32_t activeGeneration,
    uint32_t activeCapabilities, uint64_t nowMs,
    uint64_t freshForMs = kCinematicControlFreshMs) noexcept
{
    if ((activeCapabilities & TitleCapability_CutsceneTheater) == 0 ||
        activeTitle == GameTitle::None || activeTitle == GameTitle::Unknown ||
        publication.title != activeTitle || !activeGeneration ||
        publication.generation != activeGeneration ||
        !publication.heartbeatMs || nowMs < publication.heartbeatMs ||
        nowMs - publication.heartbeatMs > freshForMs)
    {
        return CinematicControlState::Unknown;
    }
    return publication.state;
}

inline bool ResolveCutsceneTheaterProjection(
    const CutsceneTheaterProjectionPublication& publication,
    GameTitle activeTitle, uint32_t activeGeneration,
    uint32_t activeCapabilities, uint64_t nowMs,
    float& authoredAspect,
    uint64_t freshForMs = kCinematicControlFreshMs) noexcept
{
    authoredAspect = 0.0f;
    if ((activeCapabilities & TitleCapability_CutsceneTheater) == 0 ||
        activeTitle == GameTitle::None || activeTitle == GameTitle::Unknown ||
        publication.title != activeTitle || !activeGeneration ||
        publication.generation != activeGeneration ||
        !publication.heartbeatMs || nowMs < publication.heartbeatMs ||
        nowMs - publication.heartbeatMs > freshForMs ||
        !std::isfinite(publication.authoredAspect) ||
        publication.authoredAspect < 0.25f ||
        publication.authoredAspect > 4.0f)
    {
        return false;
    }
    authoredAspect = publication.authoredAspect;
    return true;
}

inline void ApplyCutsceneTheaterEyeTransform(
    bool active, float depth,
    float position[3], float orientation[4]) noexcept
{
    if (!active || !position || !orientation)
        return;
    const float scale = std::clamp(
        std::isfinite(depth) ? depth : 1.0f, 0.0f, 2.0f);
    for (int axis = 0; axis < 3; ++axis)
        position[axis] *= scale;
    // A virtual stereo screen needs parallel cameras. Runtime eye cant belongs
    // to immersive projection submission, not to the movie encoded in a quad.
    orientation[0] = 0.0f;
    orientation[1] = 0.0f;
    orientation[2] = 0.0f;
    orientation[3] = 1.0f;
}

inline uint32_t CutsceneTheaterImageIndex(
    uint32_t visibleEye, bool flipDepth) noexcept
{
    visibleEye = visibleEye > 1 ? 1 : visibleEye;
    return flipDepth ? 1u - visibleEye : visibleEye;
}

inline float CutsceneTheaterHeight(
    float widthMeters, uint32_t imageWidth, uint32_t imageHeight) noexcept
{
    if (!std::isfinite(widthMeters) || widthMeters <= 0.0f ||
        !imageWidth || !imageHeight)
    {
        return 0.0f;
    }
    return widthMeters * static_cast<float>(imageHeight) /
        static_cast<float>(imageWidth);
}

inline float CutsceneTheaterHeightFromAspect(
    float widthMeters, float authoredAspect) noexcept
{
    if (!std::isfinite(widthMeters) || widthMeters <= 0.0f ||
        !std::isfinite(authoredAspect) || authoredAspect < 0.25f ||
        authoredAspect > 4.0f)
    {
        return 0.0f;
    }
    return widthMeters / authoredAspect;
}

struct CutsceneTheaterTransitionOutput
{
    bool active = false;
    bool switched = false;
    float fadeAlpha = 0.0f;
};

// A continuous fade controller: take 100 ms to black, switch presentation
// there, then take 100 ms back to clear. Retargeting while in flight changes
// direction from the current alpha instead of jumping.
class CutsceneTheaterTransition
{
public:
    CutsceneTheaterTransitionOutput Update(
        uint64_t nowMs, bool requested) noexcept
    {
        if (!m_haveTime)
        {
            m_haveTime = true;
            m_lastMs = nowMs;
        }
        const uint64_t elapsed = nowMs >= m_lastMs ? nowMs - m_lastMs : 0;
        m_lastMs = nowMs;
        const float step = std::min(
            1.0f, static_cast<float>(elapsed) /
                static_cast<float>(kCutsceneTheaterFadeMs));

        CutsceneTheaterTransitionOutput result{};
        if (requested != m_active)
        {
            m_alpha = std::min(1.0f, m_alpha + step);
            if (m_alpha >= 1.0f)
            {
                m_active = requested;
                result.switched = true;
            }
        }
        else
        {
            m_alpha = std::max(0.0f, m_alpha - step);
        }
        result.active = m_active;
        result.fadeAlpha = m_alpha;
        return result;
    }

    void Reset(bool active = false) noexcept
    {
        m_active = active;
        m_alpha = 0.0f;
        m_lastMs = 0;
        m_haveTime = false;
    }

private:
    bool m_active = false;
    bool m_haveTime = false;
    float m_alpha = 0.0f;
    uint64_t m_lastMs = 0;
};
