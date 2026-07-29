#include <windows.h>
#include <tlhelp32.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <vector>
#include <MinHook.h>
#include "game.h"
#include "sigscan.h"
#include "vr.h"
#include "ik.h"
#include "title_adapter.h"
#include "../common/reach_chud_logic.h"
#include "../common/reach_render_logic.h"
#ifndef HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
#define HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE 0
#endif
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
#include "reach_render_candidate.h"
#include "../common/reach_observer_logic.h"
#endif
#include "../common/log.h"
#include "../common/config.h"
#include "../common/hud_layout_logic.h"
#include "../common/input_logic.h"
#include "../common/odst_bringup_logic.h"
#include "../common/scope_logic.h"

#ifndef HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
#define HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP 0
#endif
// M1 head tracking. We hook the game's per-frame camera-update function and,
// each frame, overwrite the authoritative camera's forward/up vectors with the
// direction of the headset. Writing from inside the game's own frame (rather
// than poking memory from a thread) means our value lands at exactly the right
// moment and holds steady instead of flickering.
//
// The hooked function (RVA 0x2A628C for build 1.3528.0.0) is __fastcall(dst,
// src): it copies the camera from src (a double-buffered heap struct) into the
// static gun-camera. src+0x28 = forward (3 floats), src+0x34 = up (3 floats).
// See docs/RE-notes.md. These offsets are build-specific and must become AOB
// signatures before shipping.

namespace
{
    constexpr uint32_t kHalo3RuntimeCapabilities =
        TitleCapability_Stereo |
        TitleCapability_ControllerAim |
        TitleCapability_Hud |
        TitleCapability_ArmIk |
        TitleCapability_RuntimeModes |
        TitleCapability_RoomScale |
        TitleCapability_ControllerInput |
        TitleCapability_Haptics;
    constexpr uint32_t kOdstRuntimeCapabilities =
        kHalo3RuntimeCapabilities;
    // Matches kReachCapabilities in title_registry.cpp. Hud is granted: Reach's
    // own curvature record is located by kReachHudLayoutAdapter, so hud_size
    // and hud_aspect drive its native layout exactly as they do for Halo 3.
    // ArmIk is deliberately NOT granted. Reach reported no capabilities at all
    // until PublishReachLifecycle existed, so every arm-gated capability turned
    // on at once the first time it did. ControllerAim was the one the VR
    // crosshair needed and it is headset-confirmed working; ArmIk immediately
    // attached the left hand to the player's face, because Reach's arm IK is
    // not solved yet. Grant it only once that is proven in the headset.
    constexpr uint32_t kReachRuntimeCapabilities =
        TitleCapability_Stereo |
        TitleCapability_ControllerAim |
        TitleCapability_Hud |
        TitleCapability_RuntimeModes |
        TitleCapability_RoomScale |
        TitleCapability_ControllerInput |
        TitleCapability_Haptics;
    constexpr uint32_t kRuntimeCapabilitiesRequiringArm =
        TitleCapability_Stereo |
        TitleCapability_ControllerAim |
        TitleCapability_Hud |
        TitleCapability_ArmIk |
        TitleCapability_RoomScale |
        TitleCapability_Haptics;
    constexpr uint64_t kTitleRuntimeHeartbeatFreshMs = 500;

    constexpr uintptr_t kCamCopyRva = 0x2A628C; // fastcall(dst, src) camera copy
    constexpr uintptr_t kSrcFwd = 0x28;         // forward vec offset in src
    constexpr uintptr_t kSrcUp = 0x34;          // up vec offset in src

    constexpr uintptr_t kSrcPos = 0x00;         // camera position (x,y,z) in src
    constexpr uintptr_t kSrcProjX = 0x68;       // horizontal projection/FOV scale
    constexpr uintptr_t kSrcProjY = 0x6C;       // vertical projection/FOV scale

    // Gun/overlay camera: first element of the engine's 4-slot camera-object
    // array (0x2820 bytes each; see docs/RE-notes.md). +0x30/+0x34 hold the
    // overlay frustum tangents (~0.858/0.874 = ~81 deg). The overlay is
    // stretched over the whole frame, so in the widened stereo raster
    // (~123 deg) the first-person weapon and HUD appear ~2x oversized unless
    // these tangents are rewritten to match the world projection.
    constexpr uintptr_t kGunCamRva = 0x2D2F680; // expected for build 1.3528
    constexpr uintptr_t kGunProjX = 0x30;
    constexpr uintptr_t kGunProjY = 0x34;

    std::atomic<bool> g_hooked{false};
    std::atomic<uint32_t> g_halo3RuntimeGeneration{0};
    std::atomic<bool> g_renderHooked{false};
    // True only when both halves of the visible-palette path are live:
    // 0x184B08 identifies the interpolated slot, and 0x2C561C consumes a
    // private reconstructed copy with the exact render root. While live, all
    // older sim-bank/root experiments are bypassed so no controller pose can
    // feed back into the gameplay camera.
    std::atomic<bool> g_fpInterpolatorHooked{false};
    std::atomic<bool> g_enabled{false};      // F2
    std::atomic<bool> g_autoVrUserVeto{false};
    std::atomic<bool> g_autoVrOwned{false};
    // Process-lifetime fail-stop latch. Once OpenXR session ownership fails,
    // no title worker may re-arm a camera transaction behind State::Failed.
    std::atomic<bool> g_vrRuntimeFailureLatched{false};
    std::atomic<bool> g_needRecenter{true};   // F3 (yaw + position)
    std::atomic<bool> g_needPosRecenter{false}; // enabling leaning: position only, no yaw snap
    std::atomic<float> g_yawSign{-1.0f};       // F4  (default matches PSVR2 mapping)
    std::atomic<float> g_pitchSign{1.0f};      // F5
    std::atomic<float> g_pitchTrim{0.0f};      // F8/F9, radians
    std::atomic<bool> g_writeUp{true};         // F7
    std::atomic<bool> g_positional{true};      // M2: 6DOF head translation on by default; F6 toggles
    std::atomic<int> g_stereoEye{-1};           // M2: -1 mono, 0 left, 1 right

    // World scale: Halo world units per real meter (1 wu ~= 3.05 m), so ~0.33
    // gives roughly 1:1 leaning. Offset is clamped so a bad value can't fling
    // the camera through the level.
    std::atomic<float> g_worldScale{0.33f};
    std::atomic<float> g_projectionTanX{1.091595f};
    std::atomic<float> g_projectionTanY{1.114286f};
    std::atomic<float> g_zoomFactor{1.0f}; // >1 while the player is zoomed (scope)
    std::atomic<float> g_renderHalfFovX{atanf(1.091595f)};
    std::atomic<float> g_renderHalfFovY{atanf(1.114286f)};

    // Frame-pacing transition telemetry. These are monotonic relaxed counters:
    // hot hooks never format, allocate, lock, or perform file I/O for the new
    // capture. VR samples them at exact OpenXR frame boundaries and the title
    // worker writes a completed transition only after its post-roll is frozen.
    std::atomic<uint64_t> g_perfViewRenders{0};
    std::atomic<uint64_t> g_perfFpPaletteRequests[3]{};
    std::atomic<uint64_t> g_perfFpPaletteFullSolves[3]{};
    std::atomic<uint64_t> g_perfFpPaletteCacheHits[3]{};
    std::atomic<uint64_t> g_perfFpPaletteCacheStores[3]{};
    std::atomic<uint64_t> g_perfFpPaletteCacheFull[3]{};
    std::atomic<uint64_t> g_perfZoomLogWrites{0};
    std::atomic<uint64_t> g_perfViewRateLogWrites{0};
    std::atomic<uint64_t> g_perfPaletteRateLogWrites{0};
    std::atomic<uint64_t> g_perfCameraRateLogWrites{0};
    std::atomic<uint64_t> g_perfFpDriverRateLogWrites{0};

    int FramePerfEyeBucket()
    {
        const int eye = g_stereoEye.load(std::memory_order_relaxed);
        return eye == 0 || eye == 1 ? eye : 2;
    }

    // The exact camera state the engine consumed this tick (position from
    // kSrcPos, fwd/up = the very floats ApplyHeadLook wrote). Captured in
    // CamCopyHook; consumed by the first-person transform (head-bake
    // cancellation MUST use these, not a re-derivation) and by FpRootShim.
    std::atomic<float> g_camX{0}, g_camY{0}, g_camZ{0};
    // Gameplay camera origin before ApplyHeadLook adds headset leaning. A
    // controller world position is based on this stable origin plus
    // controller-minus-recenter, never camera-plus-controller-minus-current-
    // head (which mixes samples and shortens apparent reach).
    std::atomic<float> g_baseCamX{0}, g_baseCamY{0}, g_baseCamZ{0};
    std::atomic<bool> g_baseCamValid{false};
    std::atomic<float> g_camFwd[3] = {{1},{0},{0}};
    std::atomic<float> g_camUp[3] = {{0},{0},{1}};
    std::atomic<bool> g_camValid{false};

    // MEASURED world-up (gravity axis) for shoulder leveling. We do NOT assume it
    // — the earlier hardcoded (0,0,1) broke the arm. Instead we EMA the engine's
    // own camera-up each tick; over normal (mostly level) play that average
    // converges to true vertical regardless of the engine's axis convention.
    std::atomic<float> g_worldUp[3] = {{0},{0},{1}};
    std::atomic<bool> g_worldUpInit{false};

    std::atomic<uintptr_t> g_gunCamera{0};   // resolved from kGunCamRefSig
    // The overlay frustum is pinned to the exact world projection: the
    // first-person bones are camera-space positions in world units, so only an
    // exact match projects the weapon at the controller's true screen position.
    // Weapon size is a MESH scale (config gun_scale, Home/End), never a
    // frustum scale — 07-15 shipped both at once (2.0 frustum x 0.33 mesh) and
    // the gun shrank to ~1/6 size ("barely visible").

    // M3 VR aim: the game's own aim-driven camera forward, captured each frame
    // BEFORE the head-look overwrite. The XInput hook steers the game's aim
    // toward the right controller by comparing this with the controller ray.
    std::atomic<bool> g_vrAim{true};         // on by default; Insert toggles
    std::atomic<float> g_aimFwdX{1}, g_aimFwdY{0}, g_aimFwdZ{0};
    std::atomic<bool> g_aimSeen{false};

    // Reach has no controller-aim body-heading contract yet, so its stock
    // movement heading is the yaw of the pristine per-frame compact camera (the
    // engine's own facing), published from the Reach visibility build. Head-
    // relative locomotion rotates the move stick by (gaze - this) so forward
    // walks where the headset looks. This is the Reach analog of g_aimFwd; it is
    // NOT published as aim and writes nothing back into the game.
    std::atomic<float> g_reachStockHeadingYaw{0.0f};
    std::atomic<bool> g_reachMoveHeadingValid{false};

    // Yaw is relative (the game's heading is arbitrary, so we recenter it to
    // the head). Pitch is absolute (head-level == game-level), which avoids
    // capturing a bad reference on recenter.
    float g_headYawRef = 0;
    float g_gameYawRef = 0;
    float g_headPosRef[3] = {0, 0, 0}; // headset position (m) captured at recenter

    using CamCopyFn = void*(__fastcall*)(void* dst, void* src);
    CamCopyFn g_origCamCopy = nullptr;
    using ObserverCameraEffectFn = void(__fastcall*)(int userIndex);
    ObserverCameraEffectFn g_origObserverCameraEffect = nullptr;
    using RenderViewFn = void(__fastcall*)(void* view);
    RenderViewFn g_origRenderView = nullptr;
    using PrepareViewFn = void(__fastcall*)(void* view, int viewIndex);
    PrepareViewFn g_prepareView = nullptr;
    using BuildViewportFn = void(__fastcall*)(void* camera, void* temporary);
    using BuildMatricesFn = void(__fastcall*)(void* camera, void* temporary, void* output, float scale);
    BuildViewportFn g_buildViewport = nullptr;
    BuildMatricesFn g_buildMatrices = nullptr;
    using FpCameraRebuildFn = void(__fastcall*)(void* view, unsigned char flag);
    using FpCameraUploadFn = void(__fastcall*)(void* compactCamera, void* derivedBlock);
    using FpDriverFn = void(__fastcall*)(void* view, unsigned char flag);
    // ODST's two prepare-view native-CHUD phases both take the local user index.
    // Their target functions are separately proven by unique ODST signatures.
    using OdstHudPhaseFn = void(__fastcall*)(int userIndex);
    // Engine target copy used by the secondary phase: source ID 1 into ODST's
    // title-specific temporary ID 0x35. The caller ignores the COM release count.
    using OdstHudTargetCopyFn = uint32_t(__fastcall*)(
        int sourceTargetId, int destinationTargetId);

#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    struct CameraRuntimeLayout
    {
        size_t compactSize;
        size_t derivedSize;
        uintptr_t sourcePosition;
        uintptr_t sourceForward;
        uintptr_t sourceUp;
        uintptr_t sourceFpBlend;
        uintptr_t sourceVerticalOffset;
        uintptr_t sourceVerticalFov;
        uintptr_t sourceReferenceFov;
        uintptr_t compactPosition;
        uintptr_t compactForward;
        uintptr_t compactUp;
        uintptr_t compactModeFlags;
        uintptr_t compactFpBlend;
        uintptr_t rootCurrentCompact;
        uintptr_t rootCurrentDerived;
        uintptr_t rootSecondaryCompact;
        uintptr_t rootSecondaryDerived;
        uintptr_t nestedFpBase;
        uintptr_t nestedCurrentCompact;
        uintptr_t nestedCurrentDerived;
        uintptr_t nestedSecondaryCompact;
        uintptr_t nestedSecondaryDerived;
        uintptr_t nestedSourceCamera;
        uintptr_t windowBounds;
        uintptr_t renderBounds;
        uintptr_t activeBounds;
        uintptr_t verticalFov;
        uintptr_t referenceFov;
        uintptr_t verticalOffset;
        uintptr_t nearClip;
        uintptr_t farClip;
        uintptr_t obliquePlane;
        uintptr_t customProjection;
        uintptr_t customProjectionData;
        uintptr_t projectionMatrix;
        uintptr_t normalizedViewport;
        uintptr_t constructedViewSlot;
        uintptr_t viewCount;
        uintptr_t additionalContext;
        uintptr_t renderUserIndex;
        uintptr_t constructorResult;
        uintptr_t tableDerivedField;
        uintptr_t initializedZero;
        uintptr_t finalTailBoolean;
        size_t viewStride;
    };

    struct CameraRuntimeProfile
    {
        const wchar_t* moduleName;
        const char* displayName;
        uint32_t expectedTimestamp;
        size_t expectedImageSize;
        CameraRuntimeLayout layout;
        const char* camCopyPattern;
        const char* renderViewPattern;
        const char* prepareViewPattern;
        const char* buildViewportPattern;
        const char* buildMatricesPattern;
        const char* fpCameraRebuildPattern;
        const char* fpCameraUploadPattern;
        const char* fpDriverPattern;
        const char* fpDriverGuardPattern;
        const char* gunCameraConstructorPattern;
        const char* nativePauseOwnerPattern;
    };

    enum class OdstFallbackReason : int
    {
        None,
        LevelUnloaded,
        EyeRedirectUnavailable,
        UnsupportedCameraMode,
        NoCameraHeartbeat,
        InstallFailure,
        TitleLeft,
        NativePause,
    };

    struct OdstMotionBlurVar
    {
        float* slot = nullptr;
        float original = 0.0f;
    };

    struct OdstCameraRuntimeState
    {
        std::atomic<bool> installed{false};
        std::atomic<bool> armed{false};
        std::atomic<bool> cameraArrayReady{false};
        std::atomic<bool> teardownRequested{false};
        std::atomic<bool> sawValidCamera{false};
        std::atomic<bool> waitingLogged{false};
        std::atomic<int> fallbackReason{static_cast<int>(OdstFallbackReason::None)};
        std::atomic<int> activeCallbacks{0};
        std::atomic<int> activeRenderCallbacks{0};
        std::atomic<unsigned> captureFailures{0};
        std::atomic<uint64_t> installedAtMs{0};
        std::atomic<uint64_t> presentationDetachRequested{0};
        std::atomic<uint64_t> presentationDetachCompleted{0};
        std::atomic_flag presentationDetachInProgress = ATOMIC_FLAG_INIT;
        uintptr_t moduleBase = 0;
        size_t moduleSize = 0;
        HMODULE moduleReference = nullptr;
        CamCopyFn originalCamCopy = nullptr;
        ObserverCameraEffectFn originalObserverCameraEffect = nullptr;
        RenderViewFn originalRenderView = nullptr;
        PrepareViewFn prepareView = nullptr;
        BuildViewportFn buildViewport = nullptr;
        BuildMatricesFn buildMatrices = nullptr;
        FpCameraRebuildFn originalFpCameraRebuild = nullptr;
        FpCameraUploadFn fpCameraUpload = nullptr;
        FpDriverFn originalFpDriver = nullptr;
        OdstHudPhaseFn originalHudPhasePrimary = nullptr;
        OdstHudPhaseFn originalHudPhaseSecondary = nullptr;
        OdstHudTargetCopyFn originalHudTargetCopy = nullptr;
        // ODST's title-proven chud_compute_anchor_basis equivalent. Kept
        // separate from Halo 3's trampoline so cross-title teardown can never
        // leave a stale original pointer.
        void* originalHudAnchorBasis = nullptr;
        // ODST FP weapon/arm hook originals. Stored as void* because the typed
        // aliases are declared after this lifecycle state.
        void* originalFpInterpolate = nullptr;
        void* originalFpVisiblePalette = nullptr;
        uint8_t* nativeWeaponIkBranch = nullptr;
        bool nativeWeaponIkPatched = false;
        // ODST class-2 CHUD crosshair hider (parity with Halo 3). The playback
        // short-circuit inside chud_draw_widget is NOP'd here and restored on
        // teardown, exactly like nativeWeaponIkBranch above.
        uint8_t* crosshairClassGate = nullptr;
        bool crosshairClassGatePatched = false;
        DWORD crosshairClassGateOriginalProtect = 0;
        uintptr_t gunCameraArray = 0;
        std::atomic<void*> eyeView{nullptr};
        alignas(16) unsigned char eyeCompactCamera[0x90]{};
        alignas(16) unsigned char eyeDerivedBlock[0xC0]{};
        OdstMotionBlurVar motionBlurVars[2]{};
        bool motionBlurResolved = false;
        bool motionBlurZeroed = false;
        // 10 camera/weapon/CHUD core hooks + optional HUD height + two
        // all-or-nothing crosshair hooks. Trampolines are recorded alongside
        // targets so optional-hook absence can never shift lifecycle mapping.
        void* hookTargets[13]{};
        void* hookTrampolines[13]{};
        size_t hookTargetCount = 0;
        void* renderHookTarget = nullptr;
    };

    OdstCameraRuntimeState g_odstCamera;
    std::atomic<uint32_t> g_odstRuntimeGeneration{0};
    thread_local bool g_odstPreparingEyeHud = false;
    std::atomic<uintptr_t> g_odstNativePauseFlag{0};
    std::atomic<float> g_odstRenderHalfFovX[2] = {{1.07338f}, {1.07338f}};
    std::atomic<float> g_odstRenderHalfFovY[2] = {{0.92502f}, {0.92502f}};
    extern const CameraRuntimeProfile kOdstCameraProfile;
#endif

    // Final first-person pose records proved by offline RE: four players, two
    // held-weapon slots, up to 64 composed 0x34-byte bone matrices per slot.
    struct BoneMatrix;
    using ComposeBonesFn = void(__fastcall*)(void*, int, int, BoneMatrix*, void*, void*);
    using ComposeSpecialBonesFn = void(__fastcall*)(void*, BoneMatrix*, void*, void*, int, int);
    ComposeBonesFn g_origComposeBones = nullptr;
    ComposeSpecialBonesFn g_origComposeSpecialBones = nullptr;

    uint32_t* g_engineTlsIndex = nullptr;
    uint32_t* g_cinematicTlsIndex = nullptr;
    // The cinematic shot-state pointer lives at this TLS byte offset. Halo 3
    // uses 0x90; ODST recompiled the same function with 0xA0. LocateCinematicState
    // reads the exact value from the setter's `mov edx, imm32`, so the offset is
    // evidenced per title rather than hardcoded.
    uint32_t g_cinematicShotStateOffset = 0x90;
    std::atomic<int32_t> g_cinematicRebaseScene{-1};
    std::atomic<int32_t> g_cinematicRebaseShot{-1};
    std::atomic<uint32_t> g_cinematicRebaseSerial{0};
    unsigned char** g_animationTagData = nullptr;
    // Halo real_matrix4x3: uniform scale, then forward/left/up basis vectors,
    // then translation. The first headset build incorrectly put scale last,
    // shifting every basis read by one float and making weapon pieces diverge.
    struct BoneMatrix { float scale; float rotation[9]; float translation[3]; };
    static_assert(sizeof(BoneMatrix) == 0x34);

    using FpInterpolateFn = bool(__fastcall*)(int view, int id, int slot,
                                              BoneMatrix** outBones, int* outCount);
    FpInterpolateFn g_origFpInterpolate = nullptr;
    using FpVisiblePaletteFn = void(__fastcall*)(uint16_t tag, const BoneMatrix* root,
        BoneMatrix* destination, uintptr_t unused, const BoneMatrix* source,
        const int32_t* boneMap);
    FpVisiblePaletteFn g_origFpVisiblePalette = nullptr;
    std::atomic<int> g_fpBoneCount[2] = {{0},{0}};
    std::atomic<int> g_fpWristIndex[2] = {{-1},{-1}};
    std::atomic<int> g_fpOrientationIndex[2] = {{-1},{-1}};
    // Arm-IK chain, walked up the node parent table from the wrist:
    //   wrist (r_hand) -> parent = elbow (r_forearm) -> parent = shoulder
    //   (r_upperarm). Node names confirmed from the H3EK fp_body render_model.
    std::atomic<int> g_fpElbowIndex[2] = {{-1},{-1}};
    std::atomic<int> g_fpShoulderIndex[2] = {{-1},{-1}};
    // Bones at or below the wrist (hand, fingers, held weapon). They ride the
    // wrist rigidly so the gun stays gripped while the arm articulates.
    std::atomic<uint64_t> g_fpWristDescendants[2] = {{0},{0}};
    // LEFT arm chain (l_hand global string id 0xA1, derived offline from the
    // engine string list, anchored on r_hand=0xA6). Same parent-walk capture.
    std::atomic<int> g_fpLWristIndex[2] = {{-1},{-1}};
    std::atomic<int> g_fpLElbowIndex[2] = {{-1},{-1}};
    std::atomic<int> g_fpLShoulderIndex[2] = {{-1},{-1}};
    std::atomic<uint64_t> g_fpLWristDescendants[2] = {{0},{0}};
    // (Weapon-node anchoring for the dual seat was tried and headset-DISPROVEN
    // 2026-07-19 23:3x: the weapon node's origin/axes are arbitrary per weapon
    // — plasma rifle center, spiker backwards. The universal anchor is the
    // carrier hand bone, whose authored grip is correct for every weapon.)

    // The composer sees authored bones immediately before Halo applies its
    // camera-lag rotation to every bone except camera_control. Cache the full
    // wrist->camera_control relation with an atomic seqlock. The visible
    // palette hook uses it to synthesize a lag-consistent camera_control bone,
    // then removes the common lag as one rigid transform without changing the
    // weapon's already-correct authored barrel alignment.
    struct AtomicBoneMatrix
    {
        std::atomic<uint32_t> sequence{0};
        std::atomic<float> value[13]{};
    };
    AtomicBoneMatrix g_fpWristToCamera[2];

    struct FpInterpolationContext
    {
        const BoneMatrix* source = nullptr;
        int count = 0;
        int player = -1;
        int slot = -1;
        int wrist = -1;
        int cameraControl = -1;
        int elbow = -1;
        int shoulder = -1;
        uint64_t wristDescendants = 0;
        // Reach uses this exact appended held-object boundary; H3/ODST
        // leave it disabled and retain their authored descendant masks.
        int heldObjectStart = -1;
        int lWrist = -1;
        int lElbow = -1;
        int lShoulder = -1;
        uint64_t lWristDescendants = 0;
        bool valid = false;
    };

    // Optional immutable inputs for title adapters whose render hooks already
    // own one exact prepared-frame controller/head snapshot. Halo 3 and ODST
    // continue through DesiredWristWorld and the accepted camera atomics when
    // this is null; Reach supplies all three absolute world poses explicitly
    // so neither eye can re-read tracking or solve from a per-eye root.
    struct FpExplicitPoseTargets
    {
        BoneMatrix centerRoot{};
        BoneMatrix rightWrist{};
        BoneMatrix leftWrist{};
        float rightScale = 1.0f;
        float leftScale = 1.0f;
        bool centerRootValid = false;
        bool rightWristValid = false;
        bool leftWristValid = false;
    };
    // One context per held-weapon slot: slot 0 is the primary (right-hand)
    // weapon, slot 1 is the dual-wield secondary (left-hand) weapon. The
    // palette hook matches its `source` pointer against both, so any
    // interpolate/palette call ordering pairs correctly.
    thread_local FpInterpolationContext g_fpInterpolationContexts[2];
    thread_local BoneMatrix g_fpUnmodifiedInterpolations[2][64];
    thread_local BoneMatrix g_fpPaletteScratch[kReachFpMaxSourceNodeCount];
    thread_local BoneMatrix g_scopeHiddenPalette[64];
    // The render-thread IK path publishes only pointer-sized diagnostics.
    // Present consumes them and owns all logging, keeping file I/O and log
    // locks out of the palette hot path.
    thread_local const char* g_armFailWhy = nullptr;
    std::atomic<const char*> g_armFailurePublished{nullptr};
    // 0 = both arms applied, 1 = right-arm/fallback failure, 2 = left-arm failure.
    std::atomic<int> g_armFailureSide{-1};
    std::atomic<uint64_t> g_fpSkeletonKey{0};
    struct FpBoneMapSnapshot
    {
        std::atomic<uint32_t> sequence{0};
        std::atomic<uint64_t> skeletonKey{0};
        std::atomic<uint32_t> tag{0};
        std::atomic<int> count{0};
        std::atomic<int> reconstructed{0};
        std::atomic<int32_t> map[64]{};
    };
    FpBoneMapSnapshot g_fpBoneMapSnapshots[16];
    std::atomic<int> g_fpBoneMapSnapshotCount{0};

    // One visible-palette pose per stereo pair. Halo calls the interpolation
    // and palette path from each eye render, but an articulated body must be
    // posed once and merely viewed from two cameras. Re-solving independently
    // lets tiny input/timing differences put an arm in two places and wastes
    // the analytic IK work. The cache remains thread-local because the measured
    // FP prepare/palette/render sequence is synchronous on one render thread.
    struct FpStereoPaletteCache
    {
        bool valid = false;
        uint16_t tag = 0;
        int player = -1;
        int slot = -1;
        int count = 0;
        int wrist = -1;
        int elbow = -1;
        int shoulder = -1;
        uint64_t wristDescendants = 0;
        // Reach uses this exact appended held-object boundary; H3/ODST
        // leave it disabled and retain their authored descendant masks.
        int heldObjectStart = -1;
        int lWrist = -1;
        int lElbow = -1;
        int lShoulder = -1;
        uint64_t lWristDescendants = 0;
        bool armIk = false;
        BoneMatrix root{};
        BoneMatrix original[kReachFpMaxSourceNodeCount]{};
        BoneMatrix solved[kReachFpMaxSourceNodeCount]{};
    };

    struct FpStereoSolveScope
    {
        bool armed = false;
        bool centerRootValid = false;
        BoneMatrix centerRoot{};
        FpStereoPaletteCache palettes[4]{};
    };
    thread_local FpStereoSolveScope g_fpStereoSolveScope;

    // THE FLAT-GUN FIX (2026-07-18, proven offline). The engine renders the
    // first-person layer (gun + arms + CHUD) through the view's SECOND camera
    // pair {compact @view+0x08, derived @view+0x1E8}, rebuilt by 0x279BEC
    // inside every per-view render immediately before each first-person draw
    // pass (6 call sites in the render driver 0x2837xx-0x283Bxx). Each call
    // re-copies the compact camera from the pointer at view+0x2A8 (the
    // CENTER-eye pose CamCopy left), forces the tangents to a fixed viewmodel
    // FOV (publishing render_first_person_fov_scale), derives matrices into
    // view+0x1E8 via the same buildViewport/buildMatrices helpers we already
    // use, and tail-jumps into the shader-constant uploader 0x2770F0. So the
    // gun/HUD layer was drawn IDENTICALLY in both eyes (zero stereo disparity,
    // wrong FOV) no matter what any bone hook did: a flat mono layer over a
    // stereo world. The fix: after the engine's rebuild, overwrite the pair
    // with the CURRENT EYE's world camera + derived block (snapshotted by
    // RenderViewHook) and re-run the uploader so the constants match.
    FpCameraRebuildFn g_origFpCameraRebuild = nullptr;
    FpCameraUploadFn g_fpCameraUpload = nullptr;

    // GAME BRIGHTNESS (2026-07-19). 0x278EE0 was MISIDENTIFIED as the HUD-scale
    // transform. In the headset, multiplying its two float args changes the GAME
    // BRIGHTNESS/gamma, not the HUD size — a0/a1 feed a screen color/gamma
    // constant (slots 0x280000/0x2D0000), NOT a HUD geometry transform. The user
    // liked the effect and asked for it as its own control, so we keep the hook
    // but drive it from `game_brightness` (default 1.0 = untouched). This does
    // NOT resize the HUD; real HUD scaling needs a different mechanism (a
    // captured VR panel — the deferred 2D HUD has no single geometry lever).
    typedef void (__fastcall *HudXformFn)(float, float, float);
    HudXformFn g_realHudXform = nullptr;
    void __fastcall HudXformHook(float x, float y, float z)
    {
        const float b = g_config.game_brightness;
        if (isfinite(b) && b > 0.05f && b != 1.0f) { x *= b; y *= b; }
        g_realHudXform(x, y, z);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            LOG("M3: brightness hook active (game_brightness %.2f)", b);
    }

    // HUD CROSSHAIR CLASS HIDER.
    // H3EK's complete ui/chud tag set marks native reticles with scripting
    // class 2. Halo already resolves that class safely inside chud_draw_widget;
    // normal gameplay merely short-circuits around the check. The install code
    // below enables the existing check and hooks its visibility predicate.
    typedef bool (__fastcall *HudCrosshairVisibleFn)(int);
    typedef bool (__fastcall *GameIsPlaybackFn)();
    typedef void (__fastcall *HudDrawWidgetFn)(
        int, void*, unsigned short, unsigned char, void*);
    HudCrosshairVisibleFn g_realHudCrosshairVisible = nullptr;
    GameIsPlaybackFn g_gameIsPlayback = nullptr;
    HudDrawWidgetFn g_realHudDrawWidget = nullptr;
    thread_local bool g_insideHudDrawWidget = false;
    // Identity of the crosshair art most recently captured, for ANY title.
    // The compositor re-uploads the authored reticle only when this changes, so
    // a static crosshair costs no swapchain work at all.
    std::atomic<uint64_t> g_authoredCrosshairKey{0};
    thread_local uint64_t g_authoredCrosshairKeyAccum = 0;

    inline uint64_t FoldAuthoredCrosshairKey(
        uint64_t accum, unsigned int widgetIndex, unsigned int variant)
    {
        uint64_t k = accum * 1099511628211ull;
        k ^= static_cast<uint64_t>(variant) * 0x9E3779B97F4A7C15ull;
        k ^= static_cast<uint64_t>(widgetIndex) + 0x165667B19E3779F9ull;
        return k ? k : 1;  // 0 is reserved for "nothing captured"
    }
    thread_local bool g_authoredReticleCaptureStarted = false;
    // Shared because Halo can execute first-person work on a render worker.
    std::atomic<bool> g_scopeRenderActive{false};

    // chud_compute_anchor_basis produces a real_matrix4x3-like basis. Its final
    // vector starts at +0x28, with the vertical screen coordinate at +0x2C.
    // Moving that coordinate translates every native HUD widget without
    // changing its scale/aspect or the curvature tag data.
    using HudAnchorBasisFn = bool(__fastcall*)(int, void*, int, void*);
    HudAnchorBasisFn g_realHudAnchorBasis = nullptr;

    bool __fastcall HudAnchorBasisHook(int userIndex, void* drawWidgetData,
                                       int anchorType, void* basis)
    {
        const bool result = g_realHudAnchorBasis(
            userIndex, drawWidgetData, anchorType, basis);
        if (result && basis && !g_authoredReticleCaptureStarted &&
            g_enabled.load(std::memory_order_relaxed) && VR_IsStereoEnabled())
        {
            const float height = g_config.hud_vertical_offset;
            if (isfinite(height) && height >= kHudHeightMin && height <= kHudHeightMax)
            {
                // Halo screen Y grows downward. The user-facing setting follows
                // normal height semantics: positive raises, negative lowers.
                reinterpret_cast<float*>(basis)[0x2C / sizeof(float)] -= height;
            }
        }
        return result;
    }

    void __fastcall HudDrawWidgetHook(int userIndex, void* descriptor,
                                      unsigned short widgetIndex,
                                      unsigned char useAlternatePath,
                                      void* drawState)
    {
        // The scope has its own centered crosshair in the upload shader. Keep
        // every native CHUD widget out of the magnified world-only picture.
        if (g_scopeRenderActive.load(std::memory_order_acquire))
            return;
        const bool previousInside = g_insideHudDrawWidget;
        const bool previousCapture = g_authoredReticleCaptureStarted;
        g_insideHudDrawWidget = true;
        g_authoredReticleCaptureStarted = false;
        g_realHudDrawWidget(userIndex, descriptor, widgetIndex,
                            useAlternatePath, drawState);
        if (g_authoredReticleCaptureStarted)
        {
            // This widget's art was redirected into the authored texture, so
            // fold its identity in and publish it. Same weapon and state ->
            // same key -> the compositor skips the blocking swapchain upload.
            g_authoredCrosshairKeyAccum = FoldAuthoredCrosshairKey(
                g_authoredCrosshairKeyAccum, widgetIndex, useAlternatePath);
            g_authoredCrosshairKey.store(
                g_authoredCrosshairKeyAccum, std::memory_order_release);
            VR_EndAuthoredReticleCapture();
        }
        g_insideHudDrawWidget = previousInside;
        g_authoredReticleCaptureStarted = previousCapture;
    }

    bool __fastcall HudCrosshairVisibleHook(int userIndex)
    {
        // Halo has already proved this widget is scripting class 2 before this
        // predicate runs. During VR, redirect that one widget into the hand-ray
        // texture. Never leave the player with no aiming reference: if capture
        // is unavailable, fail open to Halo's native center reticle. Setting
        // kill_reticle=0 deliberately selects that native fallback as well.
        if (g_enabled.load(std::memory_order_relaxed) && VR_IsStereoEnabled())
        {
            if (!g_config.crosshair)
                return false;
            if (!g_config.kill_reticle)
                return true;

            const int eye = g_stereoEye.load(std::memory_order_relaxed);
            const int captureEye = g_config.right_eye_first ? 1 : 0;
            if (eye < 0 || eye == captureEye)
            {
                if (g_insideHudDrawWidget && VR_BeginAuthoredReticleCapture())
                {
                    g_authoredReticleCaptureStarted = true;
                    return true;
                }
                static std::atomic<bool> loggedFallback{false};
                if (!loggedFallback.exchange(true))
                    LOG("M3: authored crosshair redirect unavailable; keeping native crosshair visible");
                return true;
            }
            return false;
        }

        // Flat/non-VR rendering retains Halo's stock crosshair behavior.
        if (!g_gameIsPlayback)
            return true;
        if (!g_gameIsPlayback())
            return true;
        return g_realHudCrosshairVisible(userIndex);
    }

#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    __declspec(noinline) bool __fastcall OdstHudAnchorBasisHook(
        int userIndex, void* drawWidgetData, int anchorType, void* basis)
    {
        g_odstCamera.activeCallbacks.fetch_add(
            1, std::memory_order_acq_rel);
        HudAnchorBasisFn original = reinterpret_cast<HudAnchorBasisFn>(
            g_odstCamera.originalHudAnchorBasis);
        const bool result = original &&
            original(userIndex, drawWidgetData, anchorType, basis);
        if (result && basis && !g_authoredReticleCaptureStarted &&
            g_odstCamera.armed.load(std::memory_order_acquire) &&
            !g_odstCamera.teardownRequested.load(
                std::memory_order_acquire) &&
            g_enabled.load(std::memory_order_relaxed) &&
            VR_IsStereoEnabled())
        {
            const float height = g_config.hud_vertical_offset;
            if (isfinite(height) &&
                height >= kHudHeightMin && height <= kHudHeightMax)
                reinterpret_cast<float*>(basis)[0x2C / sizeof(float)] -=
                    height;
        }
        g_odstCamera.activeCallbacks.fetch_sub(
            1, std::memory_order_acq_rel);
        return result;
    }

    static bool OdstOwnsHudStereo()
    {
        return g_odstCamera.armed.load(std::memory_order_acquire) &&
            !g_odstCamera.teardownRequested.load(
                std::memory_order_acquire) &&
            g_enabled.load(std::memory_order_relaxed) &&
            VR_IsStereoEnabled();
    }

    // ODST uses the shared Halo 3 crosshair behavior only while its own stereo
    // lifecycle is armed. The wrappers retain title-module/trampoline ownership
    // until callbacks return and fail open to stock rendering at every edge.
    __declspec(noinline) bool __fastcall OdstHudCrosshairVisibleHook(
        int userIndex)
    {
        g_odstCamera.activeCallbacks.fetch_add(
            1, std::memory_order_acq_rel);
        bool result = true;
        if (OdstOwnsHudStereo())
            result = HudCrosshairVisibleHook(userIndex);
        else if (g_gameIsPlayback && g_gameIsPlayback())
            result = g_realHudCrosshairVisible(userIndex);
        g_odstCamera.activeCallbacks.fetch_sub(
            1, std::memory_order_acq_rel);
        return result;
    }

    __declspec(noinline) void __fastcall OdstHudDrawWidgetHook(
        int userIndex, void* descriptor, unsigned short widgetIndex,
        unsigned char useAlternatePath, void* drawState)
    {
        g_odstCamera.activeCallbacks.fetch_add(
            1, std::memory_order_acq_rel);
        if (OdstOwnsHudStereo())
            HudDrawWidgetHook(
                userIndex, descriptor, widgetIndex,
                useAlternatePath, drawState);
        else
            g_realHudDrawWidget(
                userIndex, descriptor, widgetIndex,
                useAlternatePath, drawState);
        g_odstCamera.activeCallbacks.fetch_sub(
            1, std::memory_order_acq_rel);
    }
#endif
    // (HudPlaceHook removed 2026-07-19: the 0x2EEFC8 out-struct was MEASURED —
    // user sliders + log dump — to hold colors/alpha/animation state only, no
    // screen coordinates. Halo's HUD has no per-element position to edit; the
    // HUD panel in vr.cpp (capture diff) is the real fix.)

    // Per-eye snapshot handed from RenderViewHook to the FP hooks below. The
    // buffers are written before g_origRenderView and read during it (same
    // thread ordering via the render call; atomic pointer pairs visibility).
    std::atomic<void*> g_eyeFpView{nullptr};
    alignas(16) unsigned char g_eyeCompactCamera[0x90];
    alignas(16) unsigned char g_eyeDerivedBlock[0x90];

    // RECONSTRUCTION Phase 0 (2026-07-19): the engine's FP render driver
    // (0x2835D4; contains all six FP camera rebuild calls + the FP passes).
    // Instrumented to answer, from one desktop run: does it execute inside our
    // per-eye windows (g_stereoEye 0/1) or outside (-1), on which thread, how
    // often, with which flag. Phase A then invokes it per eye ourselves.
    FpDriverFn g_origFpDriver = nullptr;
    int32_t* g_fpDriverGuard = nullptr; // zero-init global gating call site 1
    constexpr size_t kMaxInstalledGameHooks = 16;
    void* g_installedGameHooks[kMaxInstalledGameHooks]{};
    size_t g_installedGameHookCount = 0;

    void RememberInstalledGameHook(void* target)
    {
        if (!target)
            return;
        for (size_t i = 0; i < g_installedGameHookCount; ++i)
            if (g_installedGameHooks[i] == target)
                return;
        if (g_installedGameHookCount < kMaxInstalledGameHooks)
            g_installedGameHooks[g_installedGameHookCount++] = target;
    }

    void RemoveInstalledGameHooks()
    {
        // Called only after the Halo camera has stopped and before a reloaded
        // Halo renderer starts. Remove in reverse installation order so no
        // outer render detour can enter a dependency while it is being reset.
        for (size_t i = g_installedGameHookCount; i > 0; --i)
        {
            void* target = g_installedGameHooks[i - 1];
            MH_DisableHook(target);
            MH_RemoveHook(target);
            g_installedGameHooks[i - 1] = nullptr;
        }
        g_installedGameHookCount = 0;
        g_renderHooked = false;
        g_fpInterpolatorHooked = false;
    }
    void __fastcall FpDriverHook(void* view, unsigned char flag)
    {
        // During the scope pass this driver must still run so the palette and
        // HUD hooks can suppress their final submissions. Returning here left
        // previously prepared gun/HUD packets available to the renderer.
        VR_TraceEvent("fp-driver", (int)flag, g_stereoEye.load());
        // (The hud_zoom layout-factor poke that lived here is retired
        // 2026-07-19: [view+0x2B0]+0x174 never resized the visible HUD. HUD
        // sizing is now the vr.cpp HUD panel — capture, erase, re-present.)
        // RECONSTRUCTION Phase A (2026-07-19). Measured architecture: the
        // engine STAGES the FP camera once per frame outside the eye windows
        // (center pose, crushed depth) and its in-window driver runs (~2 per
        // eye, this exact call) DRAW using that staged camera — hence the flat
        // zero-disparity gun. Fix: immediately before each in-window run,
        // stamp BOTH staged FP camera pairs ({view+0x158,view+0x1E8} and the
        // sub-view {+0x6C8+0x08,+0x6C8+0x1E8}) with THIS EYE's world camera +
        // full world projection; the driver's own apply/upload then pushes OUR
        // values to the GPU through the engine's own path. Per-frame weapon
        // pose, per-eye camera — the standard VR renderer architecture. The
        // out-of-window staging runs are left untouched (they also rebuild
        // packets/palette); their center camera is re-stamped here before any
        // in-window draw, same thread, so ordering is deterministic.
        char* eyeView = static_cast<char*>(g_eyeFpView.load(std::memory_order_acquire));
        if (eyeView && view)
        {
            // Stamp the DRIVER'S OWN view object (the earlier == eyeView gate
            // silently never matched — different object; identity settled by
            // the one-shot below). Both FP pairs + the engine's own constant
            // upload, so recorded FP draws executing later in this eye window
            // bind this eye's camera.
            static std::atomic<bool> loggedIdentity{false};
            if (!loggedIdentity.exchange(true))
                LOG("P0: in-window driver view=%p vs eye window view=%p (delta=0x%llX)",
                    view, eyeView,
                    (unsigned long long)((char*)view > eyeView ? (char*)view - eyeView
                                                               : eyeView - (char*)view));
            char* base = static_cast<char*>(view);
            // The FP driver runs ~3x per eye; after the first stamp of a given
            // eye the pairs already hold this eye's camera. Skip the re-stamp +
            // (costly) constant upload when nothing changed. memcmp is cheap and
            // self-correcting: if the engine rewrote the pair between runs the
            // compare fails and we stamp + upload exactly as before. Counters
            // below prove how often we stamp vs skip.
            static std::atomic<uint32_t> stamps{0}, skips{0};
            const bool needStamp =
                memcmp(base + 0x158, g_eyeCompactCamera, sizeof(g_eyeCompactCamera)) != 0 ||
                memcmp(base + 0x1E8, g_eyeDerivedBlock, sizeof(g_eyeDerivedBlock)) != 0 ||
                memcmp(base + 0x6C8 + 0x08, g_eyeCompactCamera, sizeof(g_eyeCompactCamera)) != 0 ||
                memcmp(base + 0x6C8 + 0x1E8, g_eyeDerivedBlock, sizeof(g_eyeDerivedBlock)) != 0;
            if (needStamp)
            {
                stamps.fetch_add(1);
                memcpy(base + 0x158, g_eyeCompactCamera, sizeof(g_eyeCompactCamera));
                memcpy(base + 0x1E8, g_eyeDerivedBlock, sizeof(g_eyeDerivedBlock));
                memcpy(base + 0x6C8 + 0x08, g_eyeCompactCamera, sizeof(g_eyeCompactCamera));
                memcpy(base + 0x6C8 + 0x1E8, g_eyeDerivedBlock, sizeof(g_eyeDerivedBlock));
                if (g_fpCameraUpload)
                    g_fpCameraUpload(base + 0x6C8 + 0x08, base + 0x6C8 + 0x1E8);
            }
            else
            {
                skips.fetch_add(1);
            }
            {
                static std::atomic<DWORD> lastLog{GetTickCount()};
                const DWORD now=GetTickCount(); DWORD last=lastLog.load();
                if (now-last>=10000 && lastLog.compare_exchange_strong(last,now))
                {
                    g_perfFpDriverRateLogWrites.fetch_add(
                        1, std::memory_order_relaxed);
                    LOG("PERF: FP driver camera stamps=%u skips=%u per 10s "
                        "(skips = uploads avoided)",
                        stamps.exchange(0),skips.exchange(0));
                }
            }
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true))
                LOG("M3: per-eye FP render active — eye camera stamped into both FP pairs "
                    "before the in-window driver (true stereo weapon)");
        }
        g_origFpDriver(view, flag);
    }
    // Motion blur (2026-07-19): Halo 3's multi-tap camera motion blur derives
    // its blur vector from a previous-frame camera. With two eye renders per
    // frame, an eye's "previous" camera is the OTHER eye's — a constant fake
    // velocity that smears bright content into discrete repeated echoes even
    // with the head still (the long-standing "left-eye ghost", reopened by the
    // user 2026-07-18). The engine's live tuning globals are exposed through
    // its own debug-var table ({name_ptr, type, value_ptr} entries in .data);
    // we resolve them BY NAME at runtime — no hardcoded RVAs — and force the
    // blur scales/max to zero while the user has motion blur off (default:
    // off, the VR-comfort standard). The tag loader at ~0x28D3E0 rewrites
    // these globals when effect params load, so they are re-zeroed each frame.
    struct MotionBlurVar { float* slot; float original; };
    MotionBlurVar g_motionBlurVars[4]{};
    std::atomic<int> g_motionBlurVarCount{-1}; // -1 = not yet resolved
    std::atomic<bool> g_motionBlurZeroed{false};

    float* FindDebugVarFloat(uintptr_t base, size_t size, const char* name)
    {
        const size_t nameLen = strlen(name);
        const uint8_t* module = reinterpret_cast<const uint8_t*>(base);
        uintptr_t nameVa = 0;
        for (size_t i = 1; i + nameLen + 1 < size; ++i)
        {
            if (module[i] == static_cast<uint8_t>(name[0]) && module[i - 1] == 0 &&
                module[i + nameLen] == 0 && !memcmp(module + i, name, nameLen))
            {
                nameVa = base + i;
                break;
            }
        }
        if (!nameVa) return nullptr;
        for (size_t i = 0; i + 24 <= size; i += 8)
        {
            if (*reinterpret_cast<const uintptr_t*>(module + i) != nameVa) continue;
            const uintptr_t value = *reinterpret_cast<const uintptr_t*>(module + i + 16);
            // A real value slot points back into the module's data, never at
            // code below the data sections and never outside the module.
            if (value > base && value < base + size)
                return reinterpret_cast<float*>(value);
        }
        return nullptr;
    }

    void ResolveMotionBlurVars(uintptr_t base, size_t size)
    {
        static const char* kNames[4] = {
            "motion_blur_scale_x", "motion_blur_scale_y",
            "motion_blur_max_x", "motion_blur_max_y"};
        int count = 0;
        for (const char* name : kNames)
        {
            if (float* slot = FindDebugVarFloat(base, size, name))
                g_motionBlurVars[count++] = {slot, *slot};
        }
        g_motionBlurVarCount.store(count, std::memory_order_release);
        if (count == 4)
            LOG("M3: motion-blur tuning vars resolved (scale/max x+y); toggle is live");
        else
            LOG("M3: motion-blur vars: only %d of 4 resolved; toggle disabled", count);
    }

    // Halo applies an extra 25% widescreen FOV reduction only while
    // cinematic_in_progress() is true. That flat-screen composition choice
    // also narrows the visibility projection, so geometry is rejected inside
    // the OpenXR eye frustum during cutscenes. This engine boolean is exposed
    // through the debug-var table; resolve it by name and disable only the
    // cinematic reduction while stereo VR is active. Halo's ordinary gameplay
    // projection and the authored cinematic camera pose remain untouched.
    std::atomic<uint8_t*> g_reduceCinematicFov{nullptr};
    std::atomic<uint8_t> g_reduceCinematicFovOriginal{1};
    std::atomic<bool> g_reduceCinematicFovApplied{false};

    static int SafeReadByte(const uint8_t* slot, uint8_t* value)
    {
        __try
        {
            *value = *reinterpret_cast<const volatile uint8_t*>(slot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return 1;
    }

    static int SafeWriteByte(uint8_t* slot, uint8_t value)
    {
        __try
        {
            *reinterpret_cast<volatile uint8_t*>(slot) = value;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return 1;
    }

    static int SafeReadFloat(const float* slot, float* value)
    {
        __try
        {
            *value = *reinterpret_cast<const volatile float*>(slot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return 1;
    }

    static int SafeWriteFloat(float* slot, float value)
    {
        __try
        {
            *reinterpret_cast<volatile float*>(slot) = value;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return 1;
    }

    void InvalidateCinematicFovVar(uint8_t* staleSlot)
    {
        if (g_reduceCinematicFov.compare_exchange_strong(staleSlot, nullptr,
                std::memory_order_acq_rel))
            g_reduceCinematicFovApplied.store(false, std::memory_order_release);
    }

    // Shared draw-distance trim for all three titles. The engine's render
    // far-clip plane is the debug var "render_far_clip_distance" (stock 10240
    // world units), the same name in Halo 3, ODST, and Reach. Trimming it makes
    // the engine cull distant geometry earlier -> fewer draw calls -> CPU
    // headroom in sequential stereo, where the render thread (not the GPU) is
    // the limit. Resolved BY NAME (no RVAs), cached per module so the whole-
    // module name scan runs only when the title/instance changes, and re-
    // asserted from the 20 ms worker so it survives level and tag loads.
    // draw_distance 1.00 writes the captured stock value back, so the default
    // trims nothing. Fail-open: unresolved or an insane stock value is a no-op.
    struct DrawDistanceControl
    {
        float* slot = nullptr;
        float stock = 0.0f;
        uintptr_t base = 0;
        uint32_t generation = 0;
    };
    DrawDistanceControl g_drawDistance;

    void Game_ApplyDrawDistance(
        uintptr_t base, size_t size, uint32_t generation)
    {
        if (!base || !size)
            return;
        if (!g_drawDistance.slot || g_drawDistance.base != base ||
            g_drawDistance.generation != generation)
        {
            // Module changed: re-resolve exactly once. This whole-module name
            // scan does NOT run every tick -- only on a title/instance change.
            g_drawDistance = {};
            float* slot =
                FindDebugVarFloat(base, size, "render_far_clip_distance");
            float stock = 0.0f;
            if (!slot || !SafeReadFloat(slot, &stock) ||
                !(stock > 1.0f) || !isfinite(stock))
                return; // leave unresolved; retry on a later tick
            g_drawDistance.slot = slot;
            g_drawDistance.stock = stock;
            g_drawDistance.base = base;
            g_drawDistance.generation = generation;
        }
        float scale = g_config.draw_distance;
        if (scale < kDrawDistanceMin) scale = kDrawDistanceMin;
        if (scale > kDrawDistanceMax) scale = kDrawDistanceMax;
        const float target = g_drawDistance.stock * scale;
        float current = 0.0f;
        if (SafeReadFloat(g_drawDistance.slot, &current) && current != target)
            SafeWriteFloat(g_drawDistance.slot, target);
    }

    void ResolveCinematicFovVar(uintptr_t base, size_t size)
    {
        auto* slot = reinterpret_cast<uint8_t*>(
            FindDebugVarFloat(base, size, "reduce_widescreen_fov_during_cinematics"));
        uint8_t initial = 0;
        const bool valid = slot && SafeReadByte(slot, &initial) && initial <= 1;
        g_reduceCinematicFov.store(nullptr, std::memory_order_release);
        g_reduceCinematicFovApplied.store(false, std::memory_order_release);
        if (!valid)
        {
            LOG("cutscene culling: cinematic FOV policy unavailable; stock behavior retained");
            return;
        }
        g_reduceCinematicFovOriginal.store(initial, std::memory_order_relaxed);
        g_reduceCinematicFov.store(slot, std::memory_order_release);
        LOG("cutscene culling: cinematic FOV policy resolved (stock value %u)",
            static_cast<unsigned>(initial));
    }

    void UpdateCinematicFovPolicy()
    {
        uint8_t* slot = g_reduceCinematicFov.load(std::memory_order_acquire);
        if (!slot)
            return;

        const bool vrActive = g_enabled.load(std::memory_order_relaxed) &&
            VR_IsStereoEnabled();
        if (vrActive)
        {
            if (!g_reduceCinematicFovApplied.exchange(true))
            {
                LOG("cutscene culling: disabled Halo's widescreen cinematic FOV reduction");
            }
            // Halo normally leaves this global alone, but reassert it from the
            // Present thread in case a map transition restores debug globals.
            if (!SafeWriteByte(slot, 0))
                InvalidateCinematicFovVar(slot);
        }
        else if (g_reduceCinematicFovApplied.exchange(false))
        {
            if (!SafeWriteByte(slot,
                    g_reduceCinematicFovOriginal.load(std::memory_order_relaxed)))
            {
                InvalidateCinematicFovVar(slot);
                return;
            }
            LOG("cutscene culling: restored Halo's cinematic FOV policy");
        }
    }

    // VRIK stage: the engine's own switches for body-in-first-person, found in
    // the same debug-var table (resolved BY NAME, no RVAs):
    //   director_disable_first_person — the camera director stops treating the
    //     view as first person, which is the engine's condition for drawing
    //     the player biped (running legs, crouch — game-animated).
    //   render_first_person — master switch for the old viewmodel layer.
    // Applied per frame while the F1 "Show body" WIP toggle is on; original
    // dwords are captured on first apply and restored when toggled off.
    // Each lever: value slot + captured original + the value to force when the
    // "Show body" toggle is ON. onValue is a best guess pending the live poke
    // session (docs/VRIK-ROADMAP.md); the toggle now works if ANY lever
    // resolves, instead of the old all-or-nothing that disabled everything
    // because director/render_first_person were null at install time.
    struct EngineVarSlot { int32_t* slot=nullptr; int32_t original=0; int32_t onValue=0; };
    EngineVarSlot g_bodyVars[3];
    std::atomic<int> g_bodyVarCount{0};
    std::atomic<bool> g_bodyApplied{false};

    // DIAGNOSTIC (hud_probe): dump engine debug-var NAMES that look HUD-related,
    // so we can find a safe-area / HUD-scale / crosshair lever for the edge-crop
    // fix (the HUD gets cut off at the headset lens edges). Read-only one-time
    // scan of the module's strings; logs any name containing a HUD-ish token
    // that also resolves to a live value slot (name -> resolvable = a real var).
    void DumpHudDebugVars(uintptr_t base, size_t size)
    {
        if (!g_config.hud_probe) return;
        static const char* kTokens[] = {
            "safe_area","safe_frame","hud_scale","hud_","chud","reticle",
            "crosshair","widescreen","aspect","overscan","fov_scale"};
        const uint8_t* m=reinterpret_cast<const uint8_t*>(base);
        int dumped=0;
        LOG("HUD-VARS: scanning module for HUD-related debug-var names...");
        for (size_t i=1; i+4<size && dumped<80; ++i)
        {
            // Start of a C string (preceded by a null, printable ASCII run).
            if (m[i-1]!=0 || m[i]<0x20 || m[i]>0x7E) continue;
            size_t len=0;
            while (i+len<size && m[i+len]>=0x20 && m[i+len]<=0x7E && len<64) ++len;
            if (len<4 || i+len>=size || m[i+len]!=0) { i+=len; continue; }
            const char* s=reinterpret_cast<const char*>(m+i);
            bool hit=false;
            for (const char* tok : kTokens) if (strstr(s,tok)) { hit=true; break; }
            if (hit)
            {
                float* slot=FindDebugVarFloat(base,size,s);
                if (slot)
                {
                    LOG("HUD-VARS: '%s' = %.4f (@%p)", s, *slot, (void*)slot);
                    ++dumped;
                }
            }
            i+=len;
        }
        LOG("HUD-VARS: %d resolvable HUD-related var(s) logged", dumped);
    }

    // HUD SAFE-FRAME LOCATOR. Halo 3 and ODST use the same
    // s_chud_curvature_info field order and the same shared slider math, but
    // their immutable tag payloads differ. The adapter anchors below are
    // title-proven; cached heap addresses and scan publications are tagged with
    // a title generation so one resident MCC module can never satisfy another.
    constexpr int kMaxSafeFrameHits = 16;
    std::atomic<uintptr_t> g_safeFrameSlots[kMaxSafeFrameHits]{};
    std::atomic<uint32_t> g_safeFrameBaseCurvatureBits[kMaxSafeFrameHits]{};
    std::atomic<int> g_safeFrameHitCount{-1}; // -1 none, -2 scanning, >0 verified
    std::atomic<uint32_t> g_hudAppliedBits{0};
    std::atomic<uint32_t> g_hudAppliedAspectBits{0};
    std::atomic<uint32_t> g_hudAppliedCurvatureBits{0};
    std::atomic<HudLayoutProfile> g_hudLayoutOwner{HudLayoutProfile::None};
    std::atomic<uint32_t> g_hudLayoutGeneration{1};
    std::atomic<HudLayoutProfile> g_safeFramePublishedOwner{
        HudLayoutProfile::None};
    std::atomic<uint32_t> g_safeFramePublishedGeneration{0};
    std::atomic<bool> g_safeFrameScanInFlight{false};
    SRWLOCK g_hudLayoutWriteLock = SRWLOCK_INIT;

    // Per-title freshness beacons. A resident-but-idle game module cannot
    // borrow another title's camera ownership.
    std::atomic<uint64_t> g_halo3LastCamCopyMs{0};
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    std::atomic<uint64_t> g_odstLastCamCopyMs{0};
#endif
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    // Reach has no camera-copy hook to beat like Halo 3 and ODST. Its armed
    // per-eye core is the equivalent liveness proof, so the Present-thread
    // Reach branch stamps this beacon while that core owns the frame.
    std::atomic<uint64_t> g_reachLastCamCopyMs{0};
#endif
    std::atomic<uintptr_t> g_nativePauseFlag{0};
    std::atomic<bool> g_enginePauseValidated{false};

    const TitleRuntimeHeartbeatPolicy& RuntimeHeartbeatPolicy()
    {
        static const TitleRuntimeHeartbeatPolicy policy = [] {
            TitleRuntimeHeartbeatPolicy value{};
            value.freshForMs[TitleRuntimeSlotIndex(GameTitle::Halo3)] =
                kTitleRuntimeHeartbeatFreshMs;
            value.freshForMs[TitleRuntimeSlotIndex(GameTitle::Halo3ODST)] =
                kOdstCameraHardTimeoutMs + 1;
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
            // Reach heartbeats once per armed Present (Game_AutoVrTick), the
            // fastest cadence of the three titles, so Halo 3's window fits.
            // A zero window here made ResolveTitleRuntime disqualify Reach
            // unconditionally (heartbeatFreshForMs == 0): Reach was never the
            // resolved owner, Game_HasTitleCapability denied every shared
            // capability (rumble stayed dead), and the worker's fallback-mode
            // publication stomped the present path's Gameplay back to Loading
            // every 50 ms - the "Runtime mode: gameplay -> loading" log flap.
            value.freshForMs[TitleRuntimeSlotIndex(GameTitle::HaloReach)] =
                kTitleRuntimeHeartbeatFreshMs;
#endif
            return value;
        }();
        return policy;
    }

    GameTitle RetainedRuntimeTitle()
    {
        const bool halo3 =
            g_halo3RuntimeGeneration.load(std::memory_order_acquire) != 0;
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
        const bool odst =
            g_odstRuntimeGeneration.load(std::memory_order_acquire) != 0;
        if (halo3 == odst)
            return GameTitle::None;
        return halo3 ? GameTitle::Halo3 : GameTitle::Halo3ODST;
#else
        return halo3 ? GameTitle::Halo3 : GameTitle::None;
#endif
    }

    TitleAdapterRuntimeSnapshot RuntimeSnapshot(uint64_t nowMs)
    {
        return TitleAdapter_GetRuntimeSnapshot(
            nowMs, RuntimeHeartbeatPolicy(), RetainedRuntimeTitle());
    }

    bool PublishHalo3Lifecycle(
        bool installed, bool armed, bool teardownRequested)
    {
        const uint32_t generation =
            g_halo3RuntimeGeneration.load(std::memory_order_acquire);
        if (!generation)
            return false;
        TitleRuntimeLifecycle lifecycle{};
        lifecycle.installed = installed;
        lifecycle.armed = armed;
        lifecycle.teardownRequested = teardownRequested;
        lifecycle.enabledCapabilities = installed && !teardownRequested
            ? kHalo3RuntimeCapabilities : TitleCapability_None;
        return TitleAdapter_PublishLifecycle(
            GameTitle::Halo3, generation, lifecycle);
    }

#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    bool PublishOdstLifecycle()
    {
        const uint32_t generation =
            g_odstRuntimeGeneration.load(std::memory_order_acquire);
        if (!generation)
            return false;
        TitleRuntimeLifecycle lifecycle{};
        lifecycle.installed =
            g_odstCamera.installed.load(std::memory_order_acquire);
        lifecycle.armed =
            g_odstCamera.armed.load(std::memory_order_acquire);
        lifecycle.teardownRequested =
            g_odstCamera.teardownRequested.load(std::memory_order_acquire);
        lifecycle.enabledCapabilities =
            lifecycle.installed && !lifecycle.teardownRequested
                ? kOdstRuntimeCapabilities : TitleCapability_None;
        return TitleAdapter_PublishLifecycle(
            GameTitle::Halo3ODST, generation, lifecycle);
    }

    void ClearOdstRuntimePublication()
    {
        const uint32_t generation =
            g_odstRuntimeGeneration.load(std::memory_order_acquire);
        if (!generation)
            return;
        TitleAdapter_PublishLifecycle(
            GameTitle::Halo3ODST, generation, {});
        TitleAdapter_ClearHeartbeat(GameTitle::Halo3ODST, generation);
        g_odstLastCamCopyMs.store(0, std::memory_order_release);
        g_odstRuntimeGeneration.store(0, std::memory_order_release);
    }
#endif

    // Previous addresses and authored baselines are retained in separate
    // per-title caches across generations. The tag allocator often reuses its
    // block across map reloads, so an exact title-anchor/payload re-check can
    // avoid a full scan without ever crossing titles or compounding curvature.
    struct HudLayoutRememberedCache
    {
        std::atomic<uintptr_t> slots[kMaxSafeFrameHits]{};
        std::atomic<uint32_t> baseCurvatureBits[kMaxSafeFrameHits]{};
        std::atomic<int> count{0};
    };
    // Keep each title's authored curvature baseline across teardown and title
    // switches. The adjusted tag can remain resident; discarding the baseline
    // would make the next scan treat an already-adjusted Z as authored and
    // compound curvature. Reuse still requires exact title-anchor verification.
    HudLayoutRememberedCache g_halo3HudRemembered{};
    HudLayoutRememberedCache g_odstHudRemembered{};
    HudLayoutRememberedCache g_reachHudRemembered{};

    static HudLayoutRememberedCache* HudLayoutRememberedFor(
        HudLayoutProfile profile)
    {
        switch (profile)
        {
        case HudLayoutProfile::Halo3:
            return &g_halo3HudRemembered;
        case HudLayoutProfile::Halo3ODST:
            return &g_odstHudRemembered;
        case HudLayoutProfile::HaloReach:
            return &g_reachHudRemembered;
        default:
            return nullptr;
        }
    }

    // These timers used to be function statics shared by every title. Keeping
    // them in the generation-owned runtime prevents a Halo 3 attempt from
    // delaying ODST's first locate by up to fifteen seconds.
    std::atomic<uint64_t> g_hudLayoutLastVerifyMs{0};
    std::atomic<uint64_t> g_hudLayoutLastReacquireMs{0};
    std::atomic<uint64_t> g_hudLayoutLastAttemptMs{0};
    // When the current title took HUD-layout ownership. A level's tag data is
    // not always resident the first time a scan is eligible, so the first
    // attempts have to retry quickly; the long cooldown only makes sense once
    // the title has been settled for a while.
    std::atomic<uint64_t> g_hudLayoutOwnerSinceMs{0};

    static void ClearHudLayoutSlots()
    {
        for (int i = 0; i < kMaxSafeFrameHits; ++i)
        {
            g_safeFrameSlots[i].store(0, std::memory_order_relaxed);
            g_safeFrameBaseCurvatureBits[i].store(0, std::memory_order_relaxed);
        }
        g_safeFramePublishedOwner.store(
            HudLayoutProfile::None, std::memory_order_relaxed);
        g_safeFramePublishedGeneration.store(0, std::memory_order_relaxed);
        g_safeFrameHitCount.store(-1, std::memory_order_release);
        g_hudAppliedBits.store(0, std::memory_order_relaxed);
        g_hudAppliedAspectBits.store(0, std::memory_order_relaxed);
        g_hudAppliedCurvatureBits.store(0, std::memory_order_relaxed);
    }

    static bool HudLayoutContextMatches(
        HudLayoutProfile profile, uint32_t generation)
    {
        return HudLayoutPublicationMatches(
            g_hudLayoutOwner.load(std::memory_order_acquire),
            g_hudLayoutGeneration.load(std::memory_order_acquire),
            profile, generation);
    }

    static bool HudLayoutResultsMatch(
        HudLayoutProfile profile, uint32_t generation)
    {
        return HudLayoutPublicationMatches(
            profile, generation,
            g_safeFramePublishedOwner.load(std::memory_order_acquire),
            g_safeFramePublishedGeneration.load(std::memory_order_acquire));
    }

    static uint32_t EnsureHudLayoutProfile(HudLayoutProfile profile)
    {
        const HudLayoutAdapter* adapter = HudLayoutAdapterFor(profile);
        if (!adapter)
            return 0;
        if (g_hudLayoutOwner.load(std::memory_order_acquire) == profile)
            return g_hudLayoutGeneration.load(std::memory_order_acquire);

        AcquireSRWLockExclusive(&g_hudLayoutWriteLock);
        if (g_hudLayoutOwner.load(std::memory_order_acquire) == profile)
        {
            const uint32_t generation =
                g_hudLayoutGeneration.load(std::memory_order_acquire);
            ReleaseSRWLockExclusive(&g_hudLayoutWriteLock);
            return generation;
        }
        const uint32_t generation =
            g_hudLayoutGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        // Publishing the new owner after the generation bump invalidates any
        // old scan. The exclusive lock also drains the only foreign-memory
        // writer before active slots can change title ownership.
        g_hudLayoutOwner.store(profile, std::memory_order_release);
        ClearHudLayoutSlots();
        g_hudLayoutLastVerifyMs.store(0, std::memory_order_relaxed);
        g_hudLayoutLastReacquireMs.store(0, std::memory_order_relaxed);
        g_hudLayoutLastAttemptMs.store(0, std::memory_order_relaxed);
        g_hudLayoutOwnerSinceMs.store(
            GetTickCount64(), std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&g_hudLayoutWriteLock);
        LOG("SAFEFRAME: layout owner is %s (generation %u); active "
            "publication and timers reset", adapter->name, generation);
        return generation;
    }

    static void InvalidateHudLayoutProfile(HudLayoutProfile profile)
    {
        if (g_hudLayoutOwner.load(std::memory_order_acquire) != profile)
            return;
        AcquireSRWLockExclusive(&g_hudLayoutWriteLock);
        if (g_hudLayoutOwner.load(std::memory_order_acquire) == profile)
        {
            g_hudLayoutGeneration.fetch_add(1, std::memory_order_acq_rel);
            g_hudLayoutOwner.store(
                HudLayoutProfile::None, std::memory_order_release);
            ClearHudLayoutSlots();
            g_hudLayoutLastVerifyMs.store(0, std::memory_order_relaxed);
            g_hudLayoutLastReacquireMs.store(0, std::memory_order_relaxed);
            g_hudLayoutLastAttemptMs.store(0, std::memory_order_relaxed);
        }
        ReleaseSRWLockExclusive(&g_hudLayoutWriteLock);
    }

    // Plain helpers: SEH frames must stay free of C++ unwinding (C2712), and a
    // region can decommit between VirtualQuery and the read, so every touch of
    // foreign memory is guarded.
    // Plain byte compare honouring the adapter's wildcard mask. No SEH here:
    // every caller has already established that the bytes are readable.
    static bool SafeFrameAnchorEquals(
        const unsigned char* candidate, const HudLayoutAnchorView& view)
    {
        for (int i = 8; i < view.anchorLength; ++i)
        {
            const uint8_t mask = view.mask[i];
            if (mask && (candidate[i] & mask) != (view.anchor[i] & mask))
                return false;
        }
        return true;
    }

    // Matches every resolution-class anchor the adapter carries, not only the
    // primary one. Reach authors one curvature record per screen shape and the
    // engine picks by aspect, so the widescreen record alone is not enough.
    static int SafeFrameScanRegion(
        uintptr_t regionBase, size_t len, const HudLayoutAdapter& adapter,
        uintptr_t* out, int maxOut)
    {
        int found = 0;
        const unsigned char* p =
            reinterpret_cast<const unsigned char*>(regionBase);
        const int anchorCount = HudLayoutAnchorCount(adapter);
        __try
        {
            uint64_t prefixes[1 + kHudLayoutMaxAltAnchors]{};
            size_t spans[1 + kHudLayoutMaxAltAnchors]{};
            for (int a = 0; a < anchorCount; ++a)
            {
                const HudLayoutAnchorView view = HudLayoutAnchorAt(adapter, a);
                memcpy(&prefixes[a], view.anchor, sizeof(prefixes[a]));
                spans[a] = static_cast<size_t>(HudLayoutScanSpan(view));
            }
            for (size_t i = 0; i + 8 <= len && found < maxOut; ++i)
            {
                const uint64_t here =
                    *reinterpret_cast<const uint64_t*>(p + i);
                for (int a = 0; a < anchorCount; ++a)
                {
                    if (here != prefixes[a] || i + spans[a] > len)
                        continue;
                    const HudLayoutAnchorView view =
                        HudLayoutAnchorAt(adapter, a);
                    if (!SafeFrameAnchorEquals(p + i, view))
                        continue;
                    out[found++] = regionBase + i +
                        static_cast<size_t>(view.safeFrameOffset);
                    // Skip the matched identity only: two curvature records
                    // are far further apart than one anchor.
                    i += static_cast<size_t>(view.anchorLength) - 1;
                    break;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return found; }
        return found;
    }

    // A title whose record has no depth field reports 0.0f, which every
    // plausibility and equality check below treats as "nothing to move".
    static int SafeFrameReadLayout(
        uintptr_t slot, const HudLayoutAdapter& adapter,
        uint32_t* destinationZ, uint32_t* h, uint32_t* v)
    {
        __try
        {
            *destinationZ = HudLayoutHasDepthField(adapter)
                ? *reinterpret_cast<const volatile uint32_t*>(
                      slot + adapter.depthFromSlot)
                : 0u;
            *h = *reinterpret_cast<const volatile uint32_t*>(slot);
            *v = *reinterpret_cast<const volatile uint32_t*>(slot + 4);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return 1;
    }

    static int SafeFrameStoreLayout(
        uintptr_t slot, const HudLayoutAdapter& adapter, float destinationZ,
        float horizontal, float vertical)
    {
        __try
        {
            if (HudLayoutHasDepthField(adapter))
                *reinterpret_cast<volatile float*>(
                    slot + adapter.depthFromSlot) = destinationZ;
            *reinterpret_cast<volatile float*>(slot) = horizontal;
            *reinterpret_cast<volatile float*>(slot + 4) = vertical;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return 1;
    }

    // A record the engine only reads can live on a read-only page. Make it
    // writable for exactly this store and put the protection back. Mapped
    // views become copy-on-write, so the page privatises to this process and
    // the game's files on disk are never modified.
    static int SafeFrameWriteLayout(
        uintptr_t slot, const HudLayoutAdapter& adapter, float destinationZ,
        float horizontal, float vertical)
    {
        if (SafeFrameStoreLayout(
                slot, adapter, destinationZ, horizontal, vertical))
            return 1;
        if (!adapter.scanMappedRegions)
            return 0;

        const uintptr_t low = HudLayoutHasDepthField(adapter)
            ? slot + adapter.depthFromSlot : slot;
        const size_t span = (slot + 8) - low;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(low), &mbi, sizeof(mbi)) !=
            sizeof(mbi))
            return 0;
        const DWORD want = mbi.Type == MEM_MAPPED
            ? PAGE_WRITECOPY : PAGE_READWRITE;
        DWORD previous = 0;
        if (!VirtualProtect(
                reinterpret_cast<void*>(low), span, want, &previous))
            return 0;
        const int wrote = SafeFrameStoreLayout(
            slot, adapter, destinationZ, horizontal, vertical);
        DWORD restored = 0;
        VirtualProtect(
            reinterpret_cast<void*>(low), span, previous, &restored);
        return wrote;
    }

    // A slot is valid if ANY of the adapter's resolution-class anchors still
    // matches at its own offset behind the slot. Distinct virtual width/height
    // bytes keep the classes from aliasing each other.
    static int SafeFrameVerifySlot(
        uintptr_t slot, const HudLayoutAdapter& adapter,
        uint32_t* destinationZ, uint32_t* h, uint32_t* v)
    {
        const int anchorCount = HudLayoutAnchorCount(adapter);
        __try
        {
            bool matched = false;
            for (int a = 0; a < anchorCount && !matched; ++a)
            {
                const HudLayoutAnchorView view = HudLayoutAnchorAt(adapter, a);
                const uintptr_t anchorAddress =
                    slot - static_cast<uintptr_t>(view.safeFrameOffset);
                if (memcmp(reinterpret_cast<const void*>(anchorAddress),
                           view.anchor, 8) != 0)
                    continue;
                if (!SafeFrameAnchorEquals(
                        reinterpret_cast<const unsigned char*>(anchorAddress),
                        view))
                    continue;
                matched = true;
            }
            if (!matched)
                return 0;
            *destinationZ = HudLayoutHasDepthField(adapter)
                ? *reinterpret_cast<const volatile uint32_t*>(
                      slot + adapter.depthFromSlot)
                : 0u;
            *h = *reinterpret_cast<const volatile uint32_t*>(slot);
            *v = *reinterpret_cast<const volatile uint32_t*>(slot + 4);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    static bool SafeFramePairPlausible(uint32_t h, uint32_t v)
    {
        float hf = 0.0f, vf = 0.0f;
        memcpy(&hf, &h, sizeof(hf));
        memcpy(&vf, &v, sizeof(vf));
        return hf >= 0.15f && hf <= 1.05f &&
               vf >= 0.15f && vf <= 1.05f;
    }

    static bool HudDestinationPlausible(uint32_t bits)
    {
        float value = 0.0f;
        memcpy(&value, &bits, sizeof(value));
        return isfinite(value) && value >= -10.0f && value <= 10.0f;
    }

    static uint32_t RememberedHudCurvatureBaseline(
        HudLayoutProfile profile, uintptr_t slot, uint32_t fallback)
    {
        HudLayoutRememberedCache* remembered =
            HudLayoutRememberedFor(profile);
        if (!remembered)
            return fallback;
        const int count = remembered->count.load(std::memory_order_acquire);
        for (int i = 0; i < count && i < kMaxSafeFrameHits; ++i)
        {
            if (remembered->slots[i].load(
                    std::memory_order_relaxed) == slot)
                return remembered->baseCurvatureBits[i].load(
                    std::memory_order_relaxed);
        }
        return fallback;
    }

    // Halo builds its native HUD for the game render surface's pixel aspect.
    // Match Halo 3's headset-confirmed correction and user-facing aspect trim
    // identically for every title adapter.
    static void ComputeHudSafeFramePair(
        float size, float aspect, float& horizontal, float& vertical)
    {
        horizontal = vertical = size;
        float gameAspect = 0.0f;
        float eyeFov[4]{};
        if (VR_GetGameRenderAspect(gameAspect) && VR_GetEyeFov(0, eyeFov))
        {
            const float halfX = fmaxf(-eyeFov[0], eyeFov[1]);
            const float halfY = fmaxf(eyeFov[2], -eyeFov[3]);
            const float tanX = tanf(halfX), tanY = tanf(halfY);
            if (isfinite(tanX) && isfinite(tanY) &&
                tanX > 0.01f && tanY > 0.01f)
            {
                const float correction = gameAspect / (tanX / tanY);
                if (isfinite(correction) &&
                    correction >= 0.25f && correction <= 4.0f)
                {
                    if (correction > 1.0f)
                        vertical = size / correction;
                    else
                        horizontal = size * correction;
                }
            }
        }
        horizontal *= aspect;
        horizontal = fmaxf(0.15f, fminf(horizontal, 1.0f));
        vertical = fmaxf(0.15f, fminf(vertical, 1.0f));
    }

    static uintptr_t EncodeHudLayoutScanToken(
        HudLayoutProfile profile, uint32_t generation)
    {
        static_assert(sizeof(uintptr_t) >= sizeof(uint64_t));
        return (static_cast<uintptr_t>(generation) << 32) |
            static_cast<uint32_t>(profile);
    }

    DWORD WINAPI SafeFrameScanThread(LPVOID parameter)
    {
        const uintptr_t token = reinterpret_cast<uintptr_t>(parameter);
        const auto profile =
            static_cast<HudLayoutProfile>(static_cast<uint32_t>(token));
        const uint32_t generation = static_cast<uint32_t>(token >> 32);
        const HudLayoutAdapter* adapter = HudLayoutAdapterFor(profile);
        if (!adapter)
        {
            g_safeFrameScanInFlight.store(false, std::memory_order_release);
            return 0;
        }

        const uint64_t t0 = GetTickCount64();
        uintptr_t selfBase = 0;
        size_t selfSize = 0;
        sig::ModuleRange(L"halo3xr.dll", selfBase, selfSize);

        uintptr_t acceptedSlots[kMaxSafeFrameHits]{};
        uint32_t acceptedBaselines[kMaxSafeFrameHits]{};
        int accepted = 0;
        int rawHits = 0;
        bool cancelled = false;
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        uintptr_t addr =
            reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t addrMax =
            reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        MEMORY_BASIC_INFORMATION mbi{};
        while (addr < addrMax && accepted < kMaxSafeFrameHits &&
               VirtualQuery(
                   reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) ==
                   sizeof(mbi))
        {
            if (!HudLayoutContextMatches(profile, generation))
            {
                cancelled = true;
                break;
            }
            const uintptr_t regionBase =
                reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t next = regionBase + mbi.RegionSize;
            // Halo 3's tag data is private read-write, and that exact filter is
            // headset-proven for Halo 3 and ODST. Reach's map data need not be:
            // its adapter also inspects mapped and copy-on-write regions, so a
            // record the engine actually reads cannot be missed just because it
            // was not allocated the way Halo 3's was. Writing a copy-on-write
            // page privatises it in this process only; no file is modified.
            // Halo 3's tag data is private read-write and that exact filter is
            // headset-proven for Halo 3 and ODST. Reach's is not: the curvature
            // record is const to the engine, so the copy it actually renders
            // from can be a read-only mapping. Reach therefore accepts any
            // committed readable private or mapped region, and the writer makes
            // the page writable copy-on-write for the store, which privatises
            // it in this process only and never modifies a file on disk.
            const bool readableProtect =
                mbi.Protect == PAGE_READWRITE ||
                (adapter->scanMappedRegions &&
                 (mbi.Protect == PAGE_READONLY ||
                  mbi.Protect == PAGE_WRITECOPY ||
                  mbi.Protect == PAGE_EXECUTE_READ ||
                  mbi.Protect == PAGE_EXECUTE_READWRITE ||
                  mbi.Protect == PAGE_EXECUTE_WRITECOPY));
            const bool writableProtect = readableProtect;
            const bool allowedType =
                mbi.Type == MEM_PRIVATE ||
                (adapter->scanMappedRegions && mbi.Type == MEM_MAPPED);
            const bool candidate =
                mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                writableProtect && allowedType;
            const bool self = selfBase && regionBase >= selfBase &&
                regionBase < selfBase + selfSize;
            if (candidate && !self)
            {
                uintptr_t hits[kMaxSafeFrameHits]{};
                const int n = SafeFrameScanRegion(
                    regionBase, mbi.RegionSize, *adapter,
                    hits, kMaxSafeFrameHits);
                for (int k = 0; k < n; ++k)
                {
                    ++rawHits;
                    uint32_t destinationZ = 0, h = 0, v = 0;
                    if (!SafeFrameReadLayout(
                            hits[k], *adapter, &destinationZ, &h, &v))
                        continue;
                    const bool payloadOk =
                        SafeFramePairPlausible(h, v) &&
                        HudDestinationPlausible(destinationZ);
                    LOG("SAFEFRAME [%s]: anchor at %p (type 0x%X protect "
                        "0x%X) destination-Z %08X, safe frame %08X/%08X -> %s",
                        adapter->name, reinterpret_cast<void*>(hits[k]),
                        static_cast<unsigned>(mbi.Type),
                        static_cast<unsigned>(mbi.Protect),
                        destinationZ, h, v,
                        payloadOk ? "VERIFIED" :
                            "payload implausible, REJECTED");
                    if (payloadOk && accepted < kMaxSafeFrameHits)
                    {
                        acceptedSlots[accepted] = hits[k];
                        acceptedBaselines[accepted] =
                            RememberedHudCurvatureBaseline(profile, hits[k], destinationZ);
                        ++accepted;
                    }
                }
            }
            addr = next;
        }

        if (cancelled)
        {
            LOG("SAFEFRAME [%s]: title generation changed during scan; "
                "cancelling before the next memory region", adapter->name);
            g_safeFrameScanInFlight.store(false, std::memory_order_release);
            return 0;
        }

        const int observedAccepted = accepted;
        if (!HudLayoutAcceptedCountOk(*adapter, accepted))
        {
            LOG("SAFEFRAME [%s]: expected %d to %d title-proven layout "
                "block(s), observed %d; all candidates rejected",
                adapter->name, adapter->expectedBlocks, adapter->maxBlocks,
                accepted);
            accepted = 0;
        }
        LOG("SAFEFRAME [%s]: scan done in %llu ms (%s) - "
            "%d raw anchor hit(s), %d plausible pair(s), %d accepted",
            adapter->name,
            static_cast<unsigned long long>(GetTickCount64() - t0),
            adapter->scanMappedRegions
                ? "private + mapped, any readable protection"
                : "private-RW only",
            rawHits, observedAccepted, accepted);

        if (HudLayoutContextMatches(profile, generation))
        {
            HudLayoutRememberedCache* remembered =
                HudLayoutRememberedFor(profile);
            for (int i = 0; i < kMaxSafeFrameHits; ++i)
            {
                const uintptr_t slot =
                    i < accepted ? acceptedSlots[i] : 0;
                const uint32_t baseline =
                    i < accepted ? acceptedBaselines[i] : 0;
                g_safeFrameSlots[i].store(
                    slot, std::memory_order_relaxed);
                g_safeFrameBaseCurvatureBits[i].store(
                    baseline, std::memory_order_relaxed);
            }
            // A failed or ambiguous rescan must not erase the only retained
            // authored baseline: the resident tag may still contain our prior
            // curvature adjustment. Replace remembered data only with a full,
            // exact-cardinality title proof.
            if (HudLayoutAcceptedCountOk(*adapter, accepted))
            {
                for (int i = 0; i < kMaxSafeFrameHits; ++i)
                {
                    remembered->slots[i].store(
                        i < accepted ? acceptedSlots[i] : 0,
                        std::memory_order_relaxed);
                    remembered->baseCurvatureBits[i].store(
                        i < accepted ? acceptedBaselines[i] : 0,
                        std::memory_order_relaxed);
                }
                remembered->count.store(
                    accepted, std::memory_order_release);
            }
            g_safeFramePublishedOwner.store(
                profile, std::memory_order_relaxed);
            g_safeFramePublishedGeneration.store(
                generation, std::memory_order_release);
            g_safeFrameHitCount.store(
                accepted, std::memory_order_release);
            if (accepted)
                LOG("SAFEFRAME [%s]: title-owned layout ready; hud_size and "
                    "hud_aspect apply from the next frame%s", adapter->name,
                    HudLayoutHasDepthField(*adapter) ? " (hud_curvature too)"
                        : " - hud_curvature has NO live control in this "
                          "engine: its curvature is folded into a derived "
                          "basis when the tag block loads, so the slider is "
                          "deliberately not written");
        }
        else
        {
            LOG("SAFEFRAME [%s]: title generation changed during scan; "
                "discarding every result", adapter->name);
        }
        g_safeFrameScanInFlight.store(false, std::memory_order_release);
        return 0;
    }

    void LaunchSafeFrameScan(HudLayoutProfile profile, const char* why)
    {
        const HudLayoutAdapter* adapter = HudLayoutAdapterFor(profile);
        const uint32_t generation = EnsureHudLayoutProfile(profile);
        if (!adapter || !generation)
            return;
        if (g_safeFrameScanInFlight.exchange(
                true, std::memory_order_acq_rel))
            return;

        AcquireSRWLockExclusive(&g_hudLayoutWriteLock);
        if (!HudLayoutContextMatches(profile, generation))
        {
            ReleaseSRWLockExclusive(&g_hudLayoutWriteLock);
            g_safeFrameScanInFlight.store(false, std::memory_order_release);
            return;
        }
        ClearHudLayoutSlots();
        g_safeFrameHitCount.store(-2, std::memory_order_release);
        ReleaseSRWLockExclusive(&g_hudLayoutWriteLock);
        const uintptr_t token =
            EncodeHudLayoutScanToken(profile, generation);
        HANDLE thread = CreateThread(
            nullptr, 0, SafeFrameScanThread,
            reinterpret_cast<LPVOID>(token), 0, nullptr);
        if (thread)
        {
            CloseHandle(thread);
            LOG("SAFEFRAME [%s]: scan started (%s)",
                adapter->name, why);
        }
        else
        {
            if (HudLayoutContextMatches(profile, generation))
                g_safeFrameHitCount.store(-1, std::memory_order_release);
            g_safeFrameScanInFlight.store(
                false, std::memory_order_release);
            LOG("SAFEFRAME [%s]: scan thread create FAILED",
                adapter->name);
        }
    }

    // Present-thread, change/validation only. Slider changes apply on the next
    // Present and verified slots are rechecked once per second for map reloads.
    void ApplyHudLayoutOnce(HudLayoutProfile profile)
    {
        const HudLayoutAdapter* adapter = HudLayoutAdapterFor(profile);
        const uint32_t generation =
            g_hudLayoutGeneration.load(std::memory_order_acquire);
        if (!adapter || !HudLayoutContextMatches(profile, generation) ||
            !HudLayoutResultsMatch(profile, generation))
            return;
        const int n =
            g_safeFrameHitCount.load(std::memory_order_acquire);
        if (n <= 0)
            return;

        const float wantSize = g_config.hud_size;
        const float wantAspect = g_config.hud_aspect;
        const float wantCurvature = g_config.hud_curvature;
        if (!(wantSize >= 0.30f && wantSize <= 1.00f) ||
            !(wantAspect >= kHudAspectMin &&
              wantAspect <= kHudAspectMax) ||
            !(wantCurvature >= kHudCurvatureMin &&
              wantCurvature <= kHudCurvatureMax))
            return;

        float wantH = wantSize, wantV = wantSize;
        ComputeHudSafeFramePair(
            wantSize, wantAspect, wantH, wantV);
        uint32_t wantBits = 0, wantAspectBits = 0;
        uint32_t wantCurvatureBits = 0;
        uint32_t wantHBits = 0, wantVBits = 0;
        memcpy(&wantBits, &wantSize, sizeof(wantBits));
        memcpy(&wantAspectBits, &wantAspect, sizeof(wantAspectBits));
        memcpy(
            &wantCurvatureBits, &wantCurvature,
            sizeof(wantCurvatureBits));
        memcpy(&wantHBits, &wantH, sizeof(wantHBits));
        memcpy(&wantVBits, &wantV, sizeof(wantVBits));

        // Read-back reporting. The value observed here is whatever survived
        // since the previous pass, so it distinguishes "our write never stuck"
        // from "our write stuck and the title ignores this copy" without
        // another test session. Present thread, at most once every five
        // seconds, and never from a render or palette hot hook.
        static uint64_t lastReadbackLogMs = 0;
        const uint64_t readbackNow = GetTickCount64();
        const bool reportReadback =
            readbackNow - lastReadbackLogMs >= 5000;
        if (reportReadback)
            lastReadbackLogMs = readbackNow;

        int live = 0;
        for (int i = 0; i < n && i < kMaxSafeFrameHits; ++i)
        {
            if (!HudLayoutContextMatches(profile, generation))
                return;
            const uintptr_t slot =
                g_safeFrameSlots[i].load(std::memory_order_relaxed);
            if (!slot)
                continue;
            uint32_t destinationZ = 0, h = 0, v = 0;
            if (!SafeFrameVerifySlot(
                    slot, *adapter, &destinationZ, &h, &v))
                continue;
            if (!SafeFramePairPlausible(h, v) ||
                !HudDestinationPlausible(destinationZ))
                continue;
            if (reportReadback)
            {
                float readH = 0.0f, readV = 0.0f;
                memcpy(&readH, &h, sizeof(readH));
                memcpy(&readV, &v, sizeof(readV));
                LOG("SAFEFRAME [%s]: slot %d at %p reads %.4f/%.4f, "
                    "writing %.4f/%.4f", adapter->name, i,
                    reinterpret_cast<void*>(slot), readH, readV,
                    wantH, wantV);
            }
            const uint32_t baseBits =
                g_safeFrameBaseCurvatureBits[i].load(
                    std::memory_order_relaxed);
            if (!HudDestinationPlausible(baseBits))
                continue;
            float authoredDestinationZ = 0.0f;
            memcpy(
                &authoredDestinationZ, &baseBits,
                sizeof(authoredDestinationZ));
            // Identical Halo 3 user semantics: 0 is flat, 1 is fully curved,
            // and 0.5 restores this title's retained authored baseline.
            const bool hasDepth = HudLayoutHasDepthField(*adapter);
            const float curvatureDelta =
                0.30f - 0.60f * wantCurvature;
            const float targetDestinationZ = hasDepth
                ? authoredDestinationZ + curvatureDelta
                : 0.0f;
            uint32_t targetDestinationBits = 0;
            memcpy(
                &targetDestinationBits, &targetDestinationZ,
                sizeof(targetDestinationBits));
            if (!HudDestinationPlausible(targetDestinationBits))
                continue;
            if (!adapter->forceWriteEveryPass &&
                (!hasDepth || destinationZ == targetDestinationBits) &&
                h == wantHBits && v == wantVBits)
            {
                ++live;
                continue;
            }
            // Serialize the only foreign writes against title invalidation.
            // A writer already inside this shared section finishes before ODST
            // teardown can release ownership; a writer arriving later observes
            // the bumped generation and performs no store.
            AcquireSRWLockShared(&g_hudLayoutWriteLock);
            const bool stillOwned =
                HudLayoutContextMatches(profile, generation) &&
                HudLayoutResultsMatch(profile, generation);
            const bool wrote = stillOwned &&
                SafeFrameWriteLayout(
                    slot, *adapter, targetDestinationZ, wantH, wantV);
            ReleaseSRWLockShared(&g_hudLayoutWriteLock);
            if (!stillOwned)
                return;
            if (wrote)
                ++live;
        }

        if (live == n)
        {
            g_hudAppliedBits.store(
                wantBits, std::memory_order_relaxed);
            g_hudAppliedAspectBits.store(
                wantAspectBits, std::memory_order_relaxed);
            g_hudAppliedCurvatureBits.store(
                wantCurvatureBits, std::memory_order_relaxed);
        }
        else
        {
            g_hudAppliedBits.store(0, std::memory_order_relaxed);
            g_hudAppliedAspectBits.store(0, std::memory_order_relaxed);
            g_hudAppliedCurvatureBits.store(
                0, std::memory_order_relaxed);
            g_safeFramePublishedOwner.store(
                HudLayoutProfile::None, std::memory_order_relaxed);
            g_safeFramePublishedGeneration.store(
                0, std::memory_order_relaxed);
            g_safeFrameHitCount.store(-1, std::memory_order_release);
        }
    }

    bool TryReacquireSafeFrames(HudLayoutProfile profile)
    {
        const HudLayoutAdapter* adapter = HudLayoutAdapterFor(profile);
        HudLayoutRememberedCache* remembered =
            HudLayoutRememberedFor(profile);
        const uint32_t generation =
            g_hudLayoutGeneration.load(std::memory_order_acquire);
        if (!adapter || !remembered ||
            !HudLayoutContextMatches(profile, generation))
            return false;

        const int count = remembered->count.load(std::memory_order_acquire);
        if (!HudLayoutCanReacquireFromRemembered(*adapter, count))
            return false;
        uintptr_t slots[kMaxSafeFrameHits]{};
        uint32_t baselines[kMaxSafeFrameHits]{};
        int accepted = 0;
        for (int i = 0; i < count && i < kMaxSafeFrameHits; ++i)
        {
            const uintptr_t slot =
                remembered->slots[i].load(std::memory_order_relaxed);
            const uint32_t baseline =
                remembered->baseCurvatureBits[i].load(
                    std::memory_order_relaxed);
            uint32_t destinationZ = 0, h = 0, v = 0;
            if (slot &&
                SafeFrameVerifySlot(
                    slot, *adapter, &destinationZ, &h, &v) &&
                SafeFramePairPlausible(h, v) &&
                HudDestinationPlausible(destinationZ) &&
                HudDestinationPlausible(baseline))
            {
                slots[accepted] = slot;
                baselines[accepted] = baseline;
                ++accepted;
            }
        }
        if (accepted != count ||
            !HudLayoutAcceptedCountOk(*adapter, accepted) ||
            !HudLayoutContextMatches(profile, generation))
            return false;

        for (int i = 0; i < kMaxSafeFrameHits; ++i)
        {
            g_safeFrameSlots[i].store(
                i < accepted ? slots[i] : 0,
                std::memory_order_relaxed);
            g_safeFrameBaseCurvatureBits[i].store(
                i < accepted ? baselines[i] : 0,
                std::memory_order_relaxed);
        }
        g_hudAppliedBits.store(0, std::memory_order_relaxed);
        g_hudAppliedAspectBits.store(0, std::memory_order_relaxed);
        g_hudAppliedCurvatureBits.store(
            0, std::memory_order_relaxed);
        g_safeFramePublishedOwner.store(
            profile, std::memory_order_relaxed);
        g_safeFramePublishedGeneration.store(
            generation, std::memory_order_release);
        g_safeFrameHitCount.store(
            accepted, std::memory_order_release);
        LOG("SAFEFRAME [%s]: reacquired %d title-owned layout block(s); "
            "scan skipped", adapter->name, accepted);
        return true;
    }

    void HudLayoutAutoTick(HudLayoutProfile profile)
    {
        const HudLayoutAdapter* adapter = HudLayoutAdapterFor(profile);
        const uint32_t generation = EnsureHudLayoutProfile(profile);
        if (!adapter || !generation)
            return;

        const float wantSize = g_config.hud_size;
        const float wantAspect = g_config.hud_aspect;
        const float wantCurvature = g_config.hud_curvature;
        uint32_t wantBits = 0, wantAspectBits = 0;
        uint32_t wantCurvatureBits = 0;
        memcpy(&wantBits, &wantSize, sizeof(wantBits));
        memcpy(&wantAspectBits, &wantAspect, sizeof(wantAspectBits));
        memcpy(
            &wantCurvatureBits, &wantCurvature,
            sizeof(wantCurvatureBits));

        int count =
            g_safeFrameHitCount.load(std::memory_order_acquire);
        if (count > 0 &&
            !HudLayoutResultsMatch(profile, generation))
        {
            g_safeFrameHitCount.store(-1, std::memory_order_release);
            count = -1;
        }

        const uint64_t now = GetTickCount64();
        if (count > 0)
        {
            const uint64_t lastVerify =
                g_hudLayoutLastVerifyMs.load(
                    std::memory_order_relaxed);
            // Reach recomputes and overwrites this record itself (read-back
            // proved a fresh authored value can appear with no config change
            // and no title reload): a once-per-second reassert loses that race
            // for most of every second. Reasserting every Present call is the
            // only way ours is reliably the last write before the next draw.
            if (adapter->forceWriteEveryPass ||
                g_hudAppliedBits.load(
                    std::memory_order_relaxed) != wantBits ||
                g_hudAppliedAspectBits.load(
                    std::memory_order_relaxed) != wantAspectBits ||
                g_hudAppliedCurvatureBits.load(
                    std::memory_order_relaxed) !=
                    wantCurvatureBits ||
                now - lastVerify >= 1000)
            {
                g_hudLayoutLastVerifyMs.store(
                    now, std::memory_order_relaxed);
                ApplyHudLayoutOnce(profile);
            }
            return;
        }
        if (count == -2 ||
            g_safeFrameScanInFlight.load(std::memory_order_acquire))
            return;

        uint64_t lastCam = 0;
        if (profile == HudLayoutProfile::Halo3)
        {
            lastCam =
                g_halo3LastCamCopyMs.load(std::memory_order_relaxed);
        }
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
        else if (profile == HudLayoutProfile::Halo3ODST)
        {
            lastCam =
                g_odstLastCamCopyMs.load(std::memory_order_relaxed);
        }
#endif
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
        else if (profile == HudLayoutProfile::HaloReach)
        {
            lastCam =
                g_reachLastCamCopyMs.load(std::memory_order_relaxed);
        }
#endif
        if (!lastCam || now < lastCam || now - lastCam > 1000)
            return;

        const bool stockSize =
            wantSize >= 0.8695f && wantSize <= 0.8705f;
        const bool stockAspect =
            wantAspect >= 0.9995f && wantAspect <= 1.0005f;
        const bool stockCurvature =
            wantCurvature >= 0.4995f &&
            wantCurvature <= 0.5005f;
        const bool settingsAreStock =
            stockSize && stockAspect && stockCurvature;
        if (settingsAreStock)
        {
            HudLayoutRememberedCache* remembered =
                HudLayoutRememberedFor(profile);
            const int rememberedCount = remembered
                ? remembered->count.load(std::memory_order_acquire)
                : 0;
            if (!HudLayoutCanReacquireFromRemembered(
                    *adapter, rememberedCount))
                return;

            const uint64_t lastReacquire =
                g_hudLayoutLastReacquireMs.load(
                    std::memory_order_relaxed);
            if (now - lastReacquire >= 1000)
            {
                g_hudLayoutLastReacquireMs.store(
                    now, std::memory_order_relaxed);
                if (TryReacquireSafeFrames(profile))
                    ApplyHudLayoutOnce(profile);
            }
            // A stock config never triggers a process-wide scan. Remembered
            // title-owned slots are enough to restore a resident adjusted tag.
            return;
        }

        const uint64_t lastReacquire =
            g_hudLayoutLastReacquireMs.load(
                std::memory_order_relaxed);
        if (now - lastReacquire >= 1000)
        {
            g_hudLayoutLastReacquireMs.store(
                now, std::memory_order_relaxed);
            if (TryReacquireSafeFrames(profile))
                return;
        }

        // A level's tag data is often not resident yet the first time a scan
        // is eligible, so an early attempt finds nothing. A flat 15s cooldown
        // then meant waiting 15s plus a full scan before the HUD moved. Retry
        // quickly while the title is still settling, and only fall back to the
        // long cooldown once it has been owned long enough that a miss means
        // the record genuinely is not there.
        const uint64_t ownerSince =
            g_hudLayoutOwnerSinceMs.load(std::memory_order_relaxed);
        const bool settling =
            ownerSince && now >= ownerSince && now - ownerSince < 30000;
        const uint64_t attemptCooldownMs = settling ? 2000 : 15000;
        const uint64_t lastAttempt =
            g_hudLayoutLastAttemptMs.load(
                std::memory_order_relaxed);
        if (now - lastAttempt < attemptCooldownMs)
            return;
        g_hudLayoutLastAttemptMs.store(
            now, std::memory_order_relaxed);
        LaunchSafeFrameScan(
            profile,
            "auto: HUD layout is customized and no title-owned slots are located");
    }
    void ResolveBodyVars(uintptr_t base, size_t size)
    {
        struct { const char* name; int32_t on; } wanted[3] = {
            {"director_disable_first_person", 1}, // stop treating view as FP
            {"render_first_person", 0},           // hide the viewmodel layer
            {"debug_first_person_models", 1},     // has a live value slot on disk
        };
        int n=0;
        for (auto& w : wanted)
        {
            int32_t* slot=reinterpret_cast<int32_t*>(FindDebugVarFloat(base,size,w.name));
            if (slot) { g_bodyVars[n++]={slot,0,w.on}; }
            LOG("VRIK: body switch '%s' -> %p%s", w.name, slot,
                slot?"":" (null at install; may init at runtime)");
        }
        g_bodyVarCount.store(n,std::memory_order_release);
        LOG("VRIK: %d/3 body switches resolved; Show body toggle %s",
            n, n?"available":"disabled");
    }

    void ApplyBodySetting()
    {
        const int n=g_bodyVarCount.load(std::memory_order_acquire);
        if (!n) return;
        if (g_config.body_wip)
        {
            if (!g_bodyApplied.exchange(true))
            {
                for (int i=0;i<n;++i) g_bodyVars[i].original=*g_bodyVars[i].slot;
                LOG("VRIK: body mode ON (%d switches forced)", n);
            }
            for (int i=0;i<n;++i) *g_bodyVars[i].slot=g_bodyVars[i].onValue;
        }
        else if (g_bodyApplied.exchange(false))
        {
            for (int i=0;i<n;++i) *g_bodyVars[i].slot=g_bodyVars[i].original;
            LOG("VRIK: body mode OFF (engine values restored)");
        }
    }

    // (Removed 2026-07-19: the old ResolveChudScale/ApplyChudScale patched the
    // 1.0f immediates in 0x278EE0 — the headset proved those are the CHUD ALPHA,
    // not size. That function turned out to drive game BRIGHTNESS; it's now the
    // brightness hook, HudXformHook. HUD layout is instead controlled through
    // the verified chud_globals curvature fields above.)

    // Bullet-origin measurement: on each right-trigger pull, log where Halo
    // spawns the bullet (the camera) vs the gun muzzle world position, so the
    // "bullets from thin air" gap is quantified. The true fix moves the spawn
    // to the muzzle via a fire hook (runtime hunt); this proves + measures it.
    bool DesiredWristWorld(bool left, BoneMatrix& out, float& meshScale); // defined below
    void ProbeBulletOrigin()
    {
        if (!g_config.bullet_probe) return;
        VrPadState pad; VR_GetPadState(pad);
        static bool prev=false;
        const bool trig = pad.valid && pad.trigR>0.5f;
        if (trig && !prev)
        {
            const float cam[3]={g_camX.load(),g_camY.load(),g_camZ.load()};
            BoneMatrix w{}; float ms=1.0f;
            if (DesiredWristWorld(false,w,ms))
            {
                const float dx=cam[0]-w.translation[0],dy=cam[1]-w.translation[1],
                            dz=cam[2]-w.translation[2];
                LOG("BULLET-PROBE shot: spawn(camera)=(%.2f,%.2f,%.2f) "
                    "gun=(%.2f,%.2f,%.2f) offset=%.2f wu (%.2f m)",
                    cam[0],cam[1],cam[2],
                    w.translation[0],w.translation[1],w.translation[2],
                    sqrtf(dx*dx+dy*dy+dz*dz), sqrtf(dx*dx+dy*dy+dz*dz)*3.048f);
            }
        }
        prev=trig;
    }

    // Called every frame from CamCopyHook. Zero wins over the engine's tag
    // reload while the toggle is off; originals are restored on re-enable.
    // Reads/writes are SEH-guarded: on a Halo3 title reload these pointers are
    // re-resolved into a fresh module instance, but a stray call from a stale
    // detour (or a race with that re-resolve) must never take MCC down for a
    // comfort setting.
    void ApplyMotionBlurSetting()
    {
        if (g_motionBlurVarCount.load(std::memory_order_acquire) != 4) return;
        if (!g_config.motion_blur)
        {
            for (auto& var : g_motionBlurVars)
            {
                float current = 0.0f;
                if (!SafeReadFloat(var.slot, &current)) return;
                if (current != 0.0f) var.original = current;
                if (!SafeWriteFloat(var.slot, 0.0f)) return;
            }
            if (!g_motionBlurZeroed.exchange(true))
                LOG("M3: motion blur forced OFF (blur scale/max zeroed; artifact probe active)");
        }
        else if (g_motionBlurZeroed.exchange(false))
        {
            for (auto& var : g_motionBlurVars)
                if (!SafeWriteFloat(var.slot, var.original)) break;
            LOG("M3: motion blur restored to engine values");
        }
    }

    // Locate the first-person weapon slot that owns a composed bone array.
    // Pointer compares only, so it is safe to call before composition too.
    bool FindFirstPersonWeapon(BoneMatrix* bones, int& outSlot, unsigned char*& outWeapon)
    {
        if (!bones || !g_engineTlsIndex) return false;
        auto** slots=reinterpret_cast<void**>(__readgsqword(0x58));
        if (!slots) return false;
        auto* tls=reinterpret_cast<unsigned char*>(slots[*g_engineTlsIndex]);
        if (!tls) return false;
        auto* weapons=*reinterpret_cast<unsigned char**>(tls+0x568);
        if (!weapons) return false;
        for(int candidate=0;candidate<2;++candidate)
        {
            auto* w=weapons+candidate*0x11BC;
            if (reinterpret_cast<BoneMatrix*>(w+0x4A4)==bones)
            { outSlot=candidate; outWeapon=w; return true; }
        }
        return false;
    }

    void RotateByQuat(const float q[4], const float in[3], float out[3])
    {
        const float x=q[0], y=q[1], z=q[2], w=q[3];
        const float tx=2*(y*in[2]-z*in[1]), ty=2*(z*in[0]-x*in[2]), tz=2*(x*in[1]-y*in[0]);
        out[0]=in[0]+w*tx+(y*tz-z*ty);
        out[1]=in[1]+w*ty+(z*tx-x*tz);
        out[2]=in[2]+w*tz+(x*ty-y*tx);
    }

    float Clamp(float v, float lo, float hi);
    float WrapPi(float a);
    bool ControllerWorldPose(float basis[9],float pos[3],float& scale);
    bool ControllerWorldPoseEx(bool left,float basis[9],float pos[3],float& scale);
    bool DesiredWristWorld(bool left, BoneMatrix& out, float& meshScale);

    void BuildTrackedGameBasis(const float q[4], bool head, float basis[9])
    {
        const float xrForward[3]={0,0,-1}, xrUp[3]={0,1,0};
        float f[3],u[3];
        RotateByQuat(q,xrForward,f);
        RotateByQuat(q,xrUp,u);
        const float yaw=atan2f(f[0],-f[2]);
        const float pitch=asinf(Clamp(f[1],-1.0f,1.0f));

        // Roll is measured around the tracked forward axis exactly as it is in
        // ApplyHeadLook, so head and controller pass through the same mapping.
        float rx=-f[2], rz=f[0];
        float rl=sqrtf(rx*rx+rz*rz);
        if (rl<1e-4f) rl=1e-4f;
        rx/=rl; rz/=rl;
        const float nux=-f[1]*rz, nuy=rl, nuz=f[1]*rx;
        const float roll=atan2f(u[0]*rx+u[2]*rz,u[0]*nux+u[1]*nuy+u[2]*nuz);
        const float gy=g_gameYawRef+g_yawSign.load()*WrapPi(yaw-g_headYawRef);
        const float gp=Clamp(g_pitchSign.load()*pitch+(head?g_pitchTrim.load():0.0f),-1.5f,1.5f);
        const float cp=cosf(gp),sp=sinf(gp),cy=cosf(gy),sy=sinf(gy);
        const float cr=cosf(roll),sr=sinf(roll);
        const float forward[3]={cp*cy,cp*sy,sp};
        const float up[3]={(-sp*cy)*cr+sy*sr,(-sp*sy)*cr-cy*sr,cp*cr};
        const float left[3]={up[1]*forward[2]-up[2]*forward[1],
                             up[2]*forward[0]-up[0]*forward[2],
                             up[0]*forward[1]-up[1]*forward[0]};
        memcpy(basis,forward,sizeof(forward));
        memcpy(basis+3,left,sizeof(left));
        memcpy(basis+6,up,sizeof(up));
    }

    // Basis columns (forward,left,up) from game-frame yaw/pitch/roll.
    void BasisFromAngles(float yaw, float pitch, float roll, float basis[9])
    {
        const float cp=cosf(pitch),sp=sinf(pitch),cy=cosf(yaw),sy=sinf(yaw);
        const float cr=cosf(roll),sr=sinf(roll);
        const float forward[3]={cp*cy,cp*sy,sp};
        const float up[3]={(-sp*cy)*cr+sy*sr,(-sp*sy)*cr-cy*sr,cp*cr};
        const float left[3]={up[1]*forward[2]-up[2]*forward[1],
                             up[2]*forward[0]-up[0]*forward[2],
                             up[0]*forward[1]-up[1]*forward[0]};
        memcpy(basis,forward,sizeof(forward));
        memcpy(basis+3,left,sizeof(left));
        memcpy(basis+6,up,sizeof(up));
    }

    // Column-basis product out = left * right. Element (row,column) is stored
    // at [column*3+row]. Keeping this in one helper makes the otherwise subtle
    // H^-1*C order explicit at the weapon-bank call site.
    void MultiplyBases(const float left[9], const float right[9], float out[9])
    {
        for (int c=0;c<3;++c)
            for (int r=0;r<3;++r)
            {
                float v=0.0f;
                for (int k=0;k<3;++k)
                    v+=left[k*3+r]*right[c*3+k];
                out[c*3+r]=v;
            }
    }

    // Shortest-arc rotation (column-major 3x3) taking unit vector `from` onto
    // unit vector `to`. Used to swing a bone's rest orientation onto its new
    // IK-solved child direction while carrying the authored twist.
    void ShortestArcRotation(const float from[3], const float to[3], float out[9])
    {
        const float c=from[0]*to[0]+from[1]*to[1]+from[2]*to[2];
        float v[3]={from[1]*to[2]-from[2]*to[1],
                    from[2]*to[0]-from[0]*to[2],
                    from[0]*to[1]-from[1]*to[0]};
        if (c>0.99999f)
        { out[0]=out[4]=out[8]=1; out[1]=out[2]=out[3]=out[5]=out[6]=out[7]=0; return; }
        if (c<-0.99999f)
        {
            // 180 degrees: rotate about any axis perpendicular to `from`.
            float ax[3]={1,0,0};
            if (fabsf(from[0])>0.9f) { ax[0]=0; ax[1]=1; }
            float p[3]={from[1]*ax[2]-from[2]*ax[1],
                        from[2]*ax[0]-from[0]*ax[2],
                        from[0]*ax[1]-from[1]*ax[0]};
            const float pl=sqrtf(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
            if (pl<1e-5f){ out[0]=out[4]=out[8]=1; out[1]=out[2]=out[3]=out[5]=out[6]=out[7]=0; return; }
            p[0]/=pl; p[1]/=pl; p[2]/=pl;
            // R = 2*p*p^T - I (180 deg about p), stored column-major.
            for (int col=0;col<3;++col)
                for (int row=0;row<3;++row)
                    out[col*3+row]=2.0f*p[row]*p[col]-(row==col?1.0f:0.0f);
            return;
        }
        // Rodrigues: R = I + [v]x + [v]x^2 / (1+c). Column-major m[col*3+row].
        const float k=1.0f/(1.0f+c);
        const float vx=v[0],vy=v[1],vz=v[2];
        // rows of R:
        const float r00=1-(vy*vy+vz*vz)*k, r01=-vz+vx*vy*k, r02=vy+vx*vz*k;
        const float r10=vz+vx*vy*k,        r11=1-(vx*vx+vz*vz)*k, r12=-vx+vy*vz*k;
        const float r20=-vy+vx*vz*k,       r21=vx+vy*vz*k,    r22=1-(vx*vx+vy*vy)*k;
        out[0]=r00; out[1]=r10; out[2]=r20; // column 0 (rows 0,1,2)
        out[3]=r01; out[4]=r11; out[5]=r21; // column 1
        out[6]=r02; out[7]=r12; out[8]=r22; // column 2
    }

    // Column-basis product out = orthonormalLeft^-1 * right = left^T * right.
    void MultiplyInverseBasis(const float orthonormalLeft[9], const float right[9], float out[9])
    {
        for (int c=0;c<3;++c)
            for (int r=0;r<3;++r)
            {
                float v=0.0f;
                for (int k=0;k<3;++k)
                    v+=orthonormalLeft[r*3+k]*right[c*3+k];
                out[c*3+r]=v;
        }
    }

    bool NormalizedBasis(const BoneMatrix& matrix, float out[9])
    {
        for (int c=0;c<3;++c)
        {
            float lengthSquared=0.0f;
            for (int r=0;r<3;++r)
            {
                out[c*3+r]=matrix.rotation[c*3+r];
                lengthSquared+=out[c*3+r]*out[c*3+r];
            }
            const float length=sqrtf(lengthSquared);
            if (!isfinite(length) || length<0.001f) return false;
            for (int r=0;r<3;++r) out[c*3+r]/=length;
        }
        // A corrupt/non-orthogonal pose is much more dangerous than dropping
        // one weapon palette. Normal animation matrices are orthonormal to
        // floating-point noise.
        for (int a=0;a<3;++a)
            for (int b=a+1;b<3;++b)
            {
                float dot=0.0f;
                for (int r=0;r<3;++r) dot+=out[a*3+r]*out[b*3+r];
                if (!isfinite(dot) || fabsf(dot)>0.02f) return false;
            }
        return true;
    }

    bool ComposeBoneMatrices(const BoneMatrix& left, const BoneMatrix& right,
                             BoneMatrix& output)
    {
        if (!isfinite(left.scale) || !isfinite(right.scale) ||
            fabsf(left.scale)<0.001f || fabsf(right.scale)<0.001f) return false;
        float leftBasis[9],rightBasis[9];
        if (!NormalizedBasis(left,leftBasis) || !NormalizedBasis(right,rightBasis)) return false;
        BoneMatrix result{};
        result.scale=left.scale*right.scale;
        MultiplyBases(leftBasis,rightBasis,result.rotation);
        for (int r=0;r<3;++r)
        {
            float rotated=0.0f;
            for (int c=0;c<3;++c)
                rotated+=leftBasis[c*3+r]*right.translation[c];
            result.translation[r]=left.translation[r]+left.scale*rotated;
            if (!isfinite(result.translation[r])) return false;
        }
        output=result;
        return true;
    }

    bool InvertBoneMatrix(const BoneMatrix& input, BoneMatrix& output)
    {
        if (!isfinite(input.scale) || fabsf(input.scale)<0.001f) return false;
        float basis[9];
        if (!NormalizedBasis(input,basis)) return false;
        BoneMatrix result{};
        result.scale=1.0f/input.scale;
        for (int c=0;c<3;++c)
            for (int r=0;r<3;++r)
                result.rotation[c*3+r]=basis[r*3+c];
        for (int r=0;r<3;++r)
        {
            float v=0.0f;
            for (int c=0;c<3;++c) v+=result.rotation[c*3+r]*input.translation[c];
            result.translation[r]=-v/input.scale;
            if (!isfinite(result.translation[r])) return false;
        }
        output=result;
        return true;
    }

    void StoreAtomicBoneMatrix(AtomicBoneMatrix& destination, const BoneMatrix& value)
    {
        destination.sequence.fetch_add(1,std::memory_order_acq_rel); // writer active (odd)
        const float* source=reinterpret_cast<const float*>(&value);
        for (int i=0;i<13;++i)
            destination.value[i].store(source[i],std::memory_order_relaxed);
        destination.sequence.fetch_add(1,std::memory_order_release); // complete (even)
    }

    bool LoadAtomicBoneMatrix(const AtomicBoneMatrix& source, BoneMatrix& value)
    {
        for (int attempt=0;attempt<4;++attempt)
        {
            const uint32_t before=source.sequence.load(std::memory_order_acquire);
            if (!before || (before&1)) continue;
            float* destination=reinterpret_cast<float*>(&value);
            for (int i=0;i<13;++i)
                destination[i]=source.value[i].load(std::memory_order_relaxed);
            const uint32_t after=source.sequence.load(std::memory_order_acquire);
            if (before==after && !(after&1)) return true;
        }
        return false;
    }

    bool LoadCameraBasis(float basis[9])
    {
        if (!g_camValid.load()) return false;
        for(int j=0;j<3;++j)
        {
            basis[j]=g_camFwd[j].load();
            basis[6+j]=g_camUp[j].load();
        }
        basis[3]=basis[7]*basis[2]-basis[8]*basis[1];
        basis[4]=basis[8]*basis[0]-basis[6]*basis[2];
        basis[5]=basis[6]*basis[1]-basis[7]*basis[0];
        return true;
    }

    bool GetControllerFirstPersonTransform(int slot, float target[3], float desired[9])
    {
        // Slot 0 is Halo's right-hand first-person weapon. Do not move a
        // second/left-hand slot as part of the right-controller override.
        if (slot!=0 || !g_vrAim.load() || !g_enabled.load()) return false;
        if (!g_aimSeen.load()) return false;
        float hq[4],hp[3],cq[4],cp[3];
        if (!VR_GetHeadPose(hq,hp) || !VR_GetAimPose(cq,cp)) return false;

        // The visible renderer supplies the head camera as the skeleton root:
        //     World = Head * record
        // Therefore the replacement record (not a delta composed onto the
        // engine's already head-baked record) must be:
        //     record = Head^-1 * Controller
        // so World = Head * Head^-1 * Controller = Controller.
        const float ih[4]={-hq[0],-hq[1],-hq[2],hq[3]};
        const float dp[3]={cp[0]-hp[0],cp[1]-hp[1],cp[2]-hp[2]};
        float rp[3]; RotateByQuat(ih,dp,rp);
        const float s=g_worldScale.load();
        // Head basis = the EXACT vectors the engine's camera consumed this
        // tick (captured in CamCopyHook), not a re-derivation. left = up x fwd.
        float headBasis[9],controllerBasis[9];
        if (!LoadCameraBasis(headBasis)) return false;
        BuildTrackedGameBasis(cq,false,controllerBasis);
        // The renderer draws World = Root * record, Root = (Head, camPos).
        // We want World = Controller, so record must be Head^-1 * Controller,
        // and the offset must be expressed IN the head frame (Head^-1 * world
        // offset == the head-frame components themselves).
        target[0]=-rp[2]*s; target[1]=-rp[0]*s; target[2]=rp[1]*s; // (fwd,left,up)
        // Head-relative with the engine's EXACT camera floats. The two
        // bracketing experiments (2026-07-15 ~05:1x): zero head terms -> the
        // gun FOLLOWS the head (proves the render root carries the camera
        // orientation); head-cancellation -> counter-wobble DURING head
        // motion only (the renderer samples the camera on its own interpolated
        // 120Hz clock; our 60Hz sim-side write is stale by up to a tick).
        // This form is exactly right whenever the head is not mid-motion; the
        // residual is a velocity-proportional wobble + a one-tick flick on
        // snap turns. Fixing THAT requires cancelling on the renderer's clock
        // = intercepting the render-side FP root build (not yet located; see
        // CONTINUATION). Do not retry frame-algebra variants: both directions
        // are already falsified.
        // rel = Head^-1 * Controller. ORDER IS THE WHOLE BUG (2026-07-15 05:2x):
        //   rel = Controller * Head^T  -> World = H*C*H^T = CONJUGATION: the
        //     head rotation is applied TWICE -> "gun tracks inverted".
        //   rel = 0 head terms         -> World = H*C -> "gun follows head".
        //   rel = Head^T * Controller  -> World = H*H^T*C = C. Correct.
        // Storage is column-major (m[c*3+j] = component j of column c), so
        // (H^T C)(r,c) = sum_k H(k,r)C(k,c) => rel[c*3+r] = sum_k H[r*3+k]*C[c*3+k].
        float rel[9];
        MultiplyInverseBasis(headBasis,controllerBasis,rel);

        // Verify the storage/order invariant in the live build. If an engine
        // update ever changes the basis convention, do not write a plausible
        // but wrong quaternion into the animation bank.
        float reconstructedController[9];
        MultiplyBases(headBasis,rel,reconstructedController);
        float maxError=0.0f;
        for(int i=0;i<9;++i)
            maxError=fmaxf(maxError,fabsf(reconstructedController[i]-controllerBasis[i]));
        if (!isfinite(maxError) || maxError>0.002f) return false;
        memcpy(desired,rel,sizeof(rel));

        return true;
    }

    bool GetControllerTransformForRoot(const BoneMatrix& root, float target[3],
                                       float desiredCameraControl[9], float& meshScale)
    {
        if (!isfinite(root.scale) || fabsf(root.scale)<0.001f) return false;
        float rootBasis[9];
        if (!NormalizedBasis(root,rootBasis)) return false;

        float controllerBasis[9],controllerPosition[3];
        if (!ControllerWorldPose(controllerBasis,controllerPosition,meshScale)) return false;
        // This is the actual root pointer passed to the visible-palette
        // consumer, not a TLS re-read or a separately sampled head matrix.
        MultiplyInverseBasis(rootBasis,controllerBasis,desiredCameraControl);

        const float delta[3]={controllerPosition[0]-root.translation[0],
                              controllerPosition[1]-root.translation[1],
                              controllerPosition[2]-root.translation[2]};
        for (int c=0;c<3;++c)
        {
            target[c]=0.0f;
            for (int r=0;r<3;++r) target[c]+=rootBasis[c*3+r]*delta[r];
            target[c]/=root.scale;
            if (!isfinite(target[c])) return false;
        }
        for (int i=0;i<9;++i)
            if (!isfinite(desiredCameraControl[i])) return false;
        return isfinite(meshScale) && meshScale>0.0f;
    }

    // THE LEVER, HaloCEVR's pattern (root fed INTO composition) applied to the
    // input the mesh actually reads. Proven by elimination across headset
    // tests + the composer disassembly (0x23200C):
    //   output[0] = defaultsRoot * sourceRecord[0]            (0x23203B)
    //   output[i] = output[parent[i]] * sourceRecord[i]       (0x232099)
    // - Writing `defaults` (03:27 build) moved ONLY the muzzle flash: the
    //   composed output feeds markers/effects, and nothing else.
    // - The visible MESH recomposes from the 0x20-byte orientation bank — the
    //   composers' `source` argument — with its own camera-derived root
    //   (that is how it stays head-glued in vanilla). Editing the bank root
    //   is the only lever that has ever moved the actual gun in a headset.
    // So replace the wrist ancestor's bank record (quaternion + translation)
    // with the controller pose expressed in the head-camera frame; the mesh's
    // own camera root then cancels the head and lands the gun on the hand.
    // The composed output inherits the same pose through that child, so the
    // muzzle flash stays correct WITHOUT touching `defaults` (writing both
    // would double-apply the transform).
    //
    // No new hooks: 0x20 bytes into data the engine hands us and immediately
    // consumes. Detouring additional engine functions is what crashed the game.
    // Write the controller pose into the first bank record on the wrist's
    // ancestry chain BELOW the root (found by walking the tag node table's
    // parent words, never guessed). Rationale, from tonight's falsifications:
    // the renderer rebuilds the mesh from the bank's CHILD records under its
    // own camera-derived root — record 0 is replaced by that root (why the
    // record-0 test moved only the camera feedback, never the mesh), children
    // are kept. The game-thread composition consumes the same record, so the
    // markers/flash inherit the pose with no separate camera_control edit.
    bool ApplyControllerToBankChild(void* model, BoneMatrix* output, float* bank)
    {
        if (!model || !bank || !output || !g_animationTagData || !*g_animationTagData) return false;
        int slot=-1; unsigned char* weapon=nullptr;
        if (!FindFirstPersonWeapon(output,slot,weapon)) return false;
        const int count=*reinterpret_cast<int*>(weapon+0x49C);
        if (count<=0 || count>64 ||
            *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(model)+0x14)!=count) return false;
        const int recordOffset=*reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(model)+0x18);
        if (!recordOffset) return false;
        auto* records=*g_animationTagData+static_cast<ptrdiff_t>(recordOffset)*4;
        constexpr uint32_t kRightHandStringIndex=0xA6;
        int wristIndex=-1;
        for(int i=0;i<count;++i)
            if (*reinterpret_cast<const uint32_t*>(records+i*0x20)==kRightHandStringIndex)
            { wristIndex=i; break; }
        if (wristIndex<0) return false;
        // Walk wrist -> root; stop at the child whose parent IS the root (0).
        int child=wristIndex;
        for(int guard=0; guard<16; ++guard)
        {
            const int parent=*reinterpret_cast<const int16_t*>(records+child*0x20+8);
            if (parent<0 || parent>=count) return false;
            if (parent==0) break;
            child=parent;
        }
        // Cache the authored alignment nodes for the render-thread
        // interpolator. Publish the count last so an acquiring reader never
        // observes indices from a half-updated skeleton.
        const int selectedIndex=*reinterpret_cast<int*>(weapon+0x11A4);
        const int orientationIndex=
            selectedIndex>=0&&selectedIndex<count?selectedIndex:wristIndex;
        // Arm-IK chain: elbow = parent(wrist), shoulder = parent(elbow).
        auto parentOf=[&](int node)->int{
            if (node<0||node>=count) return -1;
            const int p=*reinterpret_cast<const int16_t*>(records+node*0x20+8);
            return (p>=0&&p<count)?p:-1;
        };
        const int elbowIndex=parentOf(wristIndex);
        const int shoulderIndex=parentOf(elbowIndex);
        // Descendant mask: every bone whose parent chain passes through the
        // given wrist (plus the wrist itself). Capped at 64 bones (mask width).
        auto subtreeOf=[&](int wrist)->uint64_t{
            if (wrist<0) return 0;
            uint64_t mask=(wrist<64)?(1ull<<wrist):0;
            for(int i=0;i<count && i<64;++i)
            {
                int n=i;
                for(int g=0;g<16;++g)
                {
                    if (n==wrist) { mask|=(1ull<<i); break; }
                    n=parentOf(n);
                    if (n<0) break;
                }
            }
            return mask;
        };
        const uint64_t descendants=subtreeOf(wristIndex);
        // Left arm: l_hand global string id 0xA2 — LIVE-PROVEN (2026-07-19
        // skeleton dump: index 5 = id 0xA2, parent chain 5<-3<-1, subtree 16;
        // right mirror 6=0xA6<-4<-2, subtree 21 incl. the 5 gun bones 37-41).
        // Both offline derivations (0xA1, then 0x9E from the disk pointer
        // table) were falsified live — trust only the runtime skeleton dump.
        constexpr uint32_t kLeftHandStringIndex=0xA2;
        int lWristIndex=-1;
        for(int i=0;i<count;++i)
            if (*reinterpret_cast<const uint32_t*>(records+i*0x20)==kLeftHandStringIndex)
            { lWristIndex=i; break; }
        // TOPOLOGICAL FALLBACK (2026-07-19): every offline id derivation for
        // the left hand has been falsified live (0xA1, then 0x9E: "L wrist
        // -1"), so stop depending on the id at all. The left wrist is
        // structurally unmistakable: the DEEPEST node with a hand-sized
        // subtree (>=10 bones) that neither contains the right wrist nor
        // lies inside its subtree. Deepest == smallest qualifying subtree
        // (l_hand < l_radius < l_humerus). Log the id found there so the
        // true value becomes a recorded fact.
        if (lWristIndex<0)
        {
            int bestPop=count;
            for(int i=0;i<count && i<64;++i)
            {
                if ((descendants>>i)&1) continue;          // inside right subtree
                const uint64_t sub=subtreeOf(i);
                if ((sub>>wristIndex)&1) continue;         // ancestor of right wrist
                const int pop=(int)__popcnt64(sub);
                if (pop>=10 && pop<=count/2 && pop<bestPop)
                { bestPop=pop; lWristIndex=i; }
            }
            static std::atomic<bool> loggedTopo{false};
            if (lWristIndex>=0 && !loggedTopo.exchange(true))
                LOG("M3 VRIK: left wrist found TOPOLOGICALLY: index %d, subtree %d, "
                    "record string id 0x%08X (record this id!)",
                    lWristIndex,bestPop,
                    *reinterpret_cast<const uint32_t*>(records+lWristIndex*0x20));
        }
        const int lElbowIndex=parentOf(lWristIndex);
        const int lShoulderIndex=parentOf(lElbowIndex);
        const uint64_t lDescendants=subtreeOf(lWristIndex);
        g_fpWristIndex[slot].store(wristIndex,std::memory_order_relaxed);
        g_fpOrientationIndex[slot].store(orientationIndex,std::memory_order_relaxed);
        g_fpElbowIndex[slot].store(elbowIndex,std::memory_order_relaxed);
        g_fpShoulderIndex[slot].store(shoulderIndex,std::memory_order_relaxed);
        g_fpWristDescendants[slot].store(descendants,std::memory_order_relaxed);
        g_fpLWristIndex[slot].store(lWristIndex,std::memory_order_relaxed);
        g_fpLElbowIndex[slot].store(lElbowIndex,std::memory_order_relaxed);
        g_fpLShoulderIndex[slot].store(lShoulderIndex,std::memory_order_relaxed);
        g_fpLWristDescendants[slot].store(lDescendants,std::memory_order_relaxed);
        g_fpBoneCount[slot].store(count,std::memory_order_release);
        // PROBE (2026-07-19, shotgun left-hand-stuck): the analysis above
        // re-runs every frame for whatever weapon is up, but this log was
        // once-per-session, so only the FIRST weapon's skeleton was ever
        // recorded. Key it on the skeleton's identity instead: every weapon
        // SWITCH dumps its chain + skeleton, and a missing left wrist is
        // called out loudly. Rate-limited (dual-wield could alternate
        // skeletons per frame). Log-only — behavior unchanged.
        uint64_t skelKey=(uint64_t)count;
        for(int i=0;i<count && i<64;++i)
            skelKey=skelKey*31+*reinterpret_cast<const uint32_t*>(records+i*0x20);
        g_fpSkeletonKey.store(skelKey,std::memory_order_release);
        static std::atomic<uint64_t> loggedSkelKey{0};
        static std::atomic<DWORD> lastSkelLogMs{0};
        const DWORD skelNowMs=GetTickCount();
        if (loggedSkelKey.load(std::memory_order_relaxed)!=skelKey &&
            skelNowMs-lastSkelLogMs.load(std::memory_order_relaxed)>=2000)
        {
            loggedSkelKey.store(skelKey,std::memory_order_relaxed);
            lastSkelLogMs.store(skelNowMs,std::memory_order_relaxed);
            if (lWristIndex<0)
                LOG("M3 VRIK PROBE: LEFT WRIST NOT FOUND on this skeleton — left "
                    "arm stays game-animated (the 'hand stuck on gun' symptom)");
            LOG("M3 VRIK: arm chains — R wrist %d/elbow %d/shoulder %d (subtree %d) | "
                "L wrist %d/elbow %d/shoulder %d (subtree %d)",
                wristIndex,elbowIndex,shoulderIndex,(int)__popcnt64(descendants),
                lWristIndex,lElbowIndex,lShoulderIndex,(int)__popcnt64(lDescendants));
            // Full skeleton dump, per weapon: index=id/parent for every record.
            // This is the ground truth every offline id derivation failed to
            // reproduce — keep it in every log.
            char line[512]; int pos=0; int from=0;
            for(int i=0;i<count;++i)
            {
                const uint32_t id=*reinterpret_cast<const uint32_t*>(records+i*0x20);
                const int par=*reinterpret_cast<const int16_t*>(records+i*0x20+8);
                const int n=snprintf(line+pos,sizeof(line)-pos,"%d=%X/%d ",i,id,par);
                if (n<0 || pos+n>=(int)sizeof(line)-1)
                {
                    line[pos]=0;
                    LOG("M3 VRIK: skeleton[%d..%d]: %s",from,i-1,line);
                    pos=0; from=i; --i; continue;
                }
                pos+=n;
            }
            line[pos]=0;
            LOG("M3 VRIK: skeleton[%d..%d]: %s",from,count-1,line);
        }

        // The CE probe proved the interpolated render packet is the first safe
        // gun/arms-only boundary. Once that hook is live, this sim-bank path
        // becomes topology discovery only: writing here would apply the hand
        // twice and can leak through camera_control into the gameplay camera.
        if (g_fpInterpolatorHooked.load(std::memory_order_acquire)) return true;

        float target[3],desired[9];
        if (!GetControllerFirstPersonTransform(slot,target,desired)) return false;
        // Never hand the engine a non-finite value: that is how a bad frame
        // becomes a crash deep inside the renderer instead of a visible glitch.
        for(float v : target) if (!isfinite(v)) return false;
        for(float v : desired) if (!isfinite(v)) return false;
        const float meshScale=Clamp(g_config.gun_scale,0.3f,3.0f);
        if (!isfinite(meshScale) || meshScale<=0.0f) return false;

        // Column-basis matrix (desired[c*3+r], columns = forward/left/up) ->
        // quaternion, robust in all four trace branches.
        const float m00=desired[0],m10=desired[1],m20=desired[2];
        const float m01=desired[3],m11=desired[4],m21=desired[5];
        const float m02=desired[6],m12=desired[7],m22=desired[8];
        float qx,qy,qz,qw;
        const float tr=m00+m11+m22;
        if (tr>0.0f)
        { const float s=sqrtf(tr+1.0f)*2.0f; qw=0.25f*s; qx=(m21-m12)/s; qy=(m02-m20)/s; qz=(m10-m01)/s; }
        else if (m00>m11 && m00>m22)
        { const float s=sqrtf(1.0f+m00-m11-m22)*2.0f; qw=(m21-m12)/s; qx=0.25f*s; qy=(m01+m10)/s; qz=(m02+m20)/s; }
        else if (m11>m22)
        { const float s=sqrtf(1.0f+m11-m00-m22)*2.0f; qw=(m02-m20)/s; qx=(m01+m10)/s; qy=0.25f*s; qz=(m12+m21)/s; }
        else
        { const float s=sqrtf(1.0f+m22-m00-m11)*2.0f; qw=(m10-m01)/s; qx=(m02+m20)/s; qy=(m12+m21)/s; qz=0.25f*s; }
        if (!isfinite(qx)||!isfinite(qy)||!isfinite(qz)||!isfinite(qw)) return false;
        const float qLength=sqrtf(qx*qx+qy*qy+qz*qz+qw*qw);
        if (!isfinite(qLength) || qLength<0.001f) return false;
        qx/=qLength; qy/=qLength; qz/=qLength; qw/=qLength;

        // The engine record is not composed with anymore: its content is the
        // per-tick camera bake (rest pose identity), and composing with it
        // re-imports the head at whatever phase the animator sampled — the
        // dual-tracking / snap-turn fling. The record is replaced outright.

        // This target record's scale is deliberately NOT written: camera_control
        // descends from this node, and changing it zooms the game camera —
        // the user's "scale control scales the entire world" report. The
        // engine's animated value is preserved; a mesh-only size lever is an
        // open follow-up (gun_scale currently has no effect on the mesh).
        // HaloCEVR's actual anti-contortion mechanism (WeaponHandler.cpp):
        //     if (bone.Parent == 0 || boneArray[bone.Parent].Parent == 0)
        //         outBoneTransforms[i].scale = 0.0f;   // hide the arms
        // The span from the camera-anchored body to the hand-anchored wrist
        // CANNOT be posed away — the shipped Halo VR mod hides the geometry
        // that spans it. Ours: apply the same parent-or-grandparent criterion
        // only outside the controller and camera root branches, i.e. to the
        // body/other-arm geometry stretching between the two
        // anchors. Our own branch (child) keeps its scale, and the branch
        // camera_control descends from is never touched — a scale there zooms
        // the game camera (the "scale moves the whole world" report).
        const int cc=selectedIndex;
        int camBranch=-1;
        if (cc>=0 && cc<count)
        {
            int n=cc;
            for(int g=0; g<16; ++g)
            {
                const int p=*reinterpret_cast<const int16_t*>(records+n*0x20+8);
                if (p<0||p>=count) break;
                if (p==0) { camBranch=n; break; }
                n=p;
            }
        }
        auto rootBranchOf=[&](int node)
        {
            for(int g=0; g<16 && node>0 && node<count; ++g)
            {
                const int p=*reinterpret_cast<const int16_t*>(records+node*0x20+8);
                if (p==0) return node;
                if (p<0 || p>=count) break;
                node=p;
            }
            return -1;
        };
        auto writeRecord=[&](float* base)
        {
            float* r=base+static_cast<ptrdiff_t>(child)*8;
            r[0]=qx; r[1]=qy; r[2]=qz; r[3]=qw; // i,j,k,w
            r[4]=target[0]; r[5]=target[1]; r[6]=target[2];
            for(int i=1;i<count;++i)
            {
                const int branch=rootBranchOf(i);
                if (branch==child || branch==camBranch) continue;
                const int parent=*reinterpret_cast<const int16_t*>(records+i*0x20+8);
                const int grandparent=(parent>0 && parent<count)
                    ? *reinterpret_cast<const int16_t*>(records+parent*0x20+8) : -1;
                if (parent==0 || grandparent==0)
                    base[static_cast<ptrdiff_t>(i)*8+7]=0.0f;
            }
        };
        writeRecord(bank);
        // The renderer interpolates TWO sim snapshots of these records (the
        // engine's 60Hz-sim -> 120Hz-render path). Writing only the half being
        // composed leaves the other half head-glued and the visible gun lands
        // midway between head and hand — the reported "weird in-between
        // state". Write the sibling half too (banks at TLS+0x560, one 0x1000
        // bank per slot, two 0x800 halves of 64 records each).
        auto** tlsSlots=reinterpret_cast<void**>(__readgsqword(0x58));
        auto* tls2=tlsSlots?reinterpret_cast<unsigned char*>(tlsSlots[*g_engineTlsIndex]):nullptr;
        auto* banks=tls2?*reinterpret_cast<unsigned char**>(tls2+0x560):nullptr;
        if (banks)
        {
            auto* slotBank=banks+static_cast<size_t>(slot)*0x1000;
            auto* half=reinterpret_cast<unsigned char*>(bank);
            const bool match=(half==slotBank || half==slotBank+0x800);
            static std::atomic<bool> loggedHalf{false};
            if (!loggedHalf.exchange(true))
                LOG("M3 DIAG: bank source=%p slotBank=%p sibling-write=%s",
                    half,slotBank,match?"ACTIVE":"MISSED (blend may persist)");
            if (match)
                writeRecord(reinterpret_cast<float*>(slotBank+((half-slotBank)^0x800)));
        }
        static std::atomic<unsigned> logged{0};
        const unsigned bit=1u<<slot;
        if (!(logged.fetch_or(bit)&bit))
            LOG("M3: slot %d bank CHILD node %d (wrist ancestor under root) bound to the %s controller (scale %.2f)",
                slot,child,slot==0?"right":"left",meshScale);
        return true;
    }

    void CacheAuthoredFirstPersonAlignment(BoneMatrix* bones, int first, int composedCount)
    {
        if (!bones || first!=0) return;
        int slot=-1;
        unsigned char* weapon=nullptr;
        if (!FindFirstPersonWeapon(bones,slot,weapon) || slot<0 || slot>1) return;
        const int count=*reinterpret_cast<int*>(weapon+0x49C);
        const int wrist=g_fpWristIndex[slot].load(std::memory_order_acquire);
        const int cameraControl=g_fpOrientationIndex[slot].load(std::memory_order_acquire);
        if (count<=0 || count>64 || composedCount<count ||
            wrist<0 || wrist>=count || cameraControl<0 || cameraControl>=count) return;

        BoneMatrix inverseWrist{},wristToCamera{};
        if (!InvertBoneMatrix(bones[wrist],inverseWrist) ||
            !ComposeBoneMatrices(inverseWrist,bones[cameraControl],wristToCamera)) return;
        StoreAtomicBoneMatrix(g_fpWristToCamera[slot],wristToCamera);
        static std::atomic<unsigned> loggedSlots{0};
        const unsigned bit=1u<<slot;
        if (!(loggedSlots.fetch_or(bit)&bit))
            LOG("M3: cached authored wrist->camera_control relation for FP slot %d "
                "(wrist %d, camera_control %d)",slot,wrist,cameraControl);
    }

    bool ApplyControllerToComposedBones(void* model, BoneMatrix* bones)
    {
        if (g_fpInterpolatorHooked.load(std::memory_order_acquire)) return false;
        if (!model || !bones || !g_engineTlsIndex || !g_animationTagData || !*g_animationTagData)
            return false;
        auto** slots=reinterpret_cast<void**>(__readgsqword(0x58));
        if (!slots) return false;
        auto* tls=reinterpret_cast<unsigned char*>(slots[*g_engineTlsIndex]);
        if (!tls) return false;
        auto* weapons=*reinterpret_cast<unsigned char**>(tls+0x568);
        if (!weapons) return false;
        int slot=-1;
        unsigned char* weapon=nullptr;
        for(int candidate=0;candidate<2;++candidate)
        {
            auto* w=weapons+candidate*0x11BC;
            if (reinterpret_cast<BoneMatrix*>(w+0x4A4)==bones) { slot=candidate; weapon=w; break; }
        }
        if (!weapon) return false;
        const int count=*reinterpret_cast<int*>(weapon+0x49C);
        if (count<=0 || count>64 ||
            *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(model)+0x14)!=count) return false;
        const int recordOffset=*reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(model)+0x18);
        if (!recordOffset) return false;
        auto* records=*g_animationTagData+static_cast<ptrdiff_t>(recordOffset)*4;
        // Runtime animation node records use the raw global string index, not
        // the packed map StringId.  This is also how the engine calls its own
        // node finder at 0x2323AC (for example, edx=0xD9/0x1D9).  r_hand is
        // global string index 0xA6; using 0x0C0000A6 made every pose silently
        // miss the wrist and left the view model attached to the camera.
        constexpr uint32_t kRightHandStringIndex=0xA6;
        int wristIndex=-1;
        for(int i=0;i<count;++i)
            if (*reinterpret_cast<const uint32_t*>(records+i*0x20)==kRightHandStringIndex)
            {
                wristIndex=i;
                break;
            }
        if (wristIndex<0)
        {
            static std::atomic<bool> loggedMissing{false};
            if (!loggedMissing.exchange(true))
            {
                LOG("M3: r_hand node 0x%X absent from first-person skeleton (%d bones; first nodes %X,%X,%X,%X)",
                    kRightHandStringIndex,count,
                    count>0?*reinterpret_cast<const uint32_t*>(records):0,
                    count>1?*reinterpret_cast<const uint32_t*>(records+0x20):0,
                    count>2?*reinterpret_cast<const uint32_t*>(records+0x40):0,
                    count>3?*reinterpret_cast<const uint32_t*>(records+0x60):0);
            }
            return false;
        }
        // Halo resolves the per-weapon camera_control node (string index 0xD9)
        // itself and stores its bone index here. Unlike a guessed `gun` name,
        // this exists in the live 43-node skeleton and follows each weapon's
        // authored barrel orientation.
        const int selectedIndex=*reinterpret_cast<int*>(weapon+0x11A4);
        const int orientationIndex=selectedIndex>=0&&selectedIndex<count?selectedIndex:wristIndex;

        // Proven offline (2026-07-15): the root transform handed to the composer
        // at halo3+0x2C4626 is scale 1, rotation IDENTITY, translation ~0, so
        // these composed bones are CAMERA-space. Log the real values once to
        // confirm the magnitudes match that reading in the live game.
        static std::atomic<bool> loggedBones{false};
        if (!loggedBones.exchange(true))
            LOG("M3 PROBE: composed bones camera-space check: wrist[%d] t=(%.4f,%.4f,%.4f) "
                "scale=%.3f | camera_control[%d] t=(%.4f,%.4f,%.4f) fwd=(%.3f,%.3f,%.3f)",
                wristIndex,bones[wristIndex].translation[0],bones[wristIndex].translation[1],
                bones[wristIndex].translation[2],bones[wristIndex].scale,
                selectedIndex,bones[orientationIndex].translation[0],
                bones[orientationIndex].translation[1],bones[orientationIndex].translation[2],
                bones[orientationIndex].rotation[0],bones[orientationIndex].rotation[1],
                bones[orientationIndex].rotation[2]);

        if (g_config.weapon_probe)
        {
            // DECISIVE PROBE. No controller, no head, no rotation: shove the
            // whole composed assembly a fixed 0.3 world units (~1 m) to the
            // LEFT (camera space: +x forward, +y left, +z up). The visible gun
            // either moves or it does not, and that single bit tells us whether
            // the mesh reads these matrices at all — which the disassembly
            // cannot, because our edits provably reach both the effects anchor
            // and the mesh's own render packet.
            for(int i=0;i<count;++i) bones[i].translation[1]+=0.3f;
            static std::atomic<bool> loggedProbe{false};
            if (!loggedProbe.exchange(true))
                LOG("M3 PROBE ACTIVE: all %d bones of slot %d pushed +0.3 left; "
                    "if the GUN MESH does not move, it does not read weapon+0x4A4",
                    count,slot);
            return true;
        }

        float target[3],desired[9];
        if (!GetControllerFirstPersonTransform(slot,target,desired)) return false;
        const float anchor[3]={bones[wristIndex].translation[0],bones[wristIndex].translation[1],
                               bones[wristIndex].translation[2]};
        float current[9];
        for(int column=0;column<3;++column)
        {
            float len=0;
            for(int j=0;j<3;++j)
            {
                current[column*3+j]=bones[orientationIndex].rotation[column*3+j];
                len+=current[column*3+j]*current[column*3+j];
            }
            len=sqrtf(len);
            if (len<0.001f) return false;
            for(int j=0;j<3;++j) current[column*3+j]/=len;
        }
        auto rotateDelta=[&](const float in[3],float out[3])
        {
            float component[3]{};
            for(int column=0;column<3;++column)
                for(int j=0;j<3;++j)
                    component[column]+=in[j]*current[column*3+j];
            for(int j=0;j<3;++j)
                out[j]=desired[j]*component[0]+desired[3+j]*component[1]+desired[6+j]*component[2];
        };

        // Halo 3's weapon vertices are weighted across r_hand, camera_control,
        // root and weapon-specific nodes. Moving only the wrist descendants
        // leaves some weights head-driven, producing the reported dual
        // head+hand motion. Apply one rigid delta and one uniform mesh scale
        // to the complete composed assembly so every influence agrees. The
        // bones are camera-space world units and the overlay frustum matches
        // the world projection, so 1.0 draws the weapon at authored size.
        const float meshScale=Clamp(g_config.gun_scale,0.3f,3.0f);
        for(int i=0;i<count;++i)
        {
            const float d[3]={bones[i].translation[0]-anchor[0],bones[i].translation[1]-anchor[1],
                              bones[i].translation[2]-anchor[2]};
            float rt[3]; rotateDelta(d,rt);
            for(int j=0;j<3;++j) bones[i].translation[j]=target[j]+rt[j]*meshScale;
            bones[i].scale*=meshScale;
            for(int column=0;column<3;++column)
            {
                float rotated[3]; rotateDelta(&bones[i].rotation[column*3],rotated);
                for(int j=0;j<3;++j) bones[i].rotation[column*3+j]=rotated[j];
            }
        }
        static std::atomic<unsigned> logged{0};
        const unsigned bit=1u<<slot;
        if (!(logged.fetch_or(bit)&bit))
            LOG("M3: complete first-person slot %d bound to %s controller (%d bones, wrist %d, camera_control %d, scale %.2f)",
                slot,slot==0?"right":"left",count,wristIndex,selectedIndex,meshScale);
        return true;
    }

    bool LoadCachedRenderRoot(int player, BoneMatrix& root)
    {
        if (player<0 || player>=4 || !g_engineTlsIndex) return false;
        auto** tlsSlots=reinterpret_cast<void**>(__readgsqword(0x58));
        if (!tlsSlots) return false;
        auto* tls=reinterpret_cast<unsigned char*>(tlsSlots[*g_engineTlsIndex]);
        if (!tls) return false;
        auto* renderPlayers=*reinterpret_cast<unsigned char**>(tls+0x568);
        if (!renderPlayers) return false;
        root=*reinterpret_cast<const BoneMatrix*>(
            renderPlayers+static_cast<uintptr_t>(player)*0x2430+0x23F0);
        return isfinite(root.scale) && fabsf(root.scale)>0.001f;
    }

    // AUTO BARREL ALIGNMENT source data. In the authored first-person pose the
    // barrel lies on the camera-forward axis (Halo aims the viewmodel at the
    // center reticle), and the render root IS the camera, so in record space
    // the authored barrel direction expressed in the wrist frame is
    // invRot(wrist) * (1,0,0). That is a rig constant per weapon (the gun is
    // glued to the hand); a slow EMA rides out idle sway and reload swings.
    // Written by the game thread (FpInterpolateHook), read by DesiredWristWorld.
    // Authored barrel-in-wrist direction per held-weapon slot: the primary
    // (slot 0) aligns to the right controller ray, the dual-wield secondary
    // (slot 1) to the left. Each skeleton's own animated wrist is measured —
    // the secondary's authored pose differs from the primary's.
    std::atomic<float> g_barrelInWrist[2][3];
    std::atomic<bool> g_barrelInWristValid[2]={{false},{false}};

    // Marker/effect packet builders also consume 0x184B08 directly. Preserve
    // their already headset-verified controller registration on the live
    // interpolation buffer. The visible palette never consumes this mutation:
    // it receives the private unmodified copy reconstructed below.
    // Same single same-frame rigid transform as the visible palette (wrist ->
    // controller), applied to the live interpolation buffer that feeds
    // markers/muzzle effects, so the flash and the gun cannot diverge.
    bool ApplyControllerToMarkerBonesWithTarget(
        const BoneMatrix& root, const BoneMatrix& desiredWristWorld,
        float meshScale, BoneMatrix* bones, int count, int transformAnchor)
    {
        if (!bones || count<=0 || count>120 ||
            transformAnchor<0 || transformAnchor>=count ||
            !isfinite(meshScale) || meshScale<=0.0f)
        {
            return false;
        }
        BoneMatrix candidate[120]{};
        memcpy(candidate,bones,static_cast<size_t>(count)*sizeof(BoneMatrix));
        BoneMatrix wristWorld{},inverseWristWorld{},t{},inverseRoot{},tRoot{},m{};
        if (!ComposeBoneMatrices(root,candidate[transformAnchor],wristWorld) ||
            !InvertBoneMatrix(wristWorld,inverseWristWorld) ||
            !ComposeBoneMatrices(desiredWristWorld,inverseWristWorld,t) ||
            !InvertBoneMatrix(root,inverseRoot) ||
            !ComposeBoneMatrices(t,root,tRoot) ||
            !ComposeBoneMatrices(inverseRoot,tRoot,m)) return false;
        for (int i=0;i<count;++i)
        {
            BoneMatrix transformed{};
            if (!ComposeBoneMatrices(m,candidate[i],transformed)) return false;
            candidate[i]=transformed;
        }
        if (meshScale!=1.0f)
        {
            const float anchor[3]={candidate[transformAnchor].translation[0],
                                   candidate[transformAnchor].translation[1],
                                   candidate[transformAnchor].translation[2]};
            for (int i=0;i<count;++i)
            {
                for (int r=0;r<3;++r)
                    candidate[i].translation[r]=anchor[r]+
                        (candidate[i].translation[r]-anchor[r])*meshScale;
                candidate[i].scale*=meshScale;
                if (!isfinite(candidate[i].scale) ||
                    !isfinite(candidate[i].translation[0]) ||
                    !isfinite(candidate[i].translation[1]) ||
                    !isfinite(candidate[i].translation[2]))
                {
                    return false;
                }
            }
        }
        const std::span<const float> packed{
            reinterpret_cast<const float*>(candidate),
            static_cast<size_t>(count)*kReachFpBoneMatrixFloatCount};
        return ReachFpCommitGraphIfFinite(packed,static_cast<size_t>(count),[&]() {
            memcpy(bones,candidate,static_cast<size_t>(count)*sizeof(BoneMatrix));
            return true;
        });
    }

    bool ApplyControllerToMarkerBonesFromRoot(
        BoneMatrix root, BoneMatrix* bones, int count, int wrist,
        int leftWrist, bool dual)
    {
        if (!bones || count<=0 || count>64 || wrist<0 || wrist>=count) return false;
        // Root POSE from the LIVE center camera, not the TLS cache: the cache
        // can hold a stale or per-eye root, and the transform baked against it
        // put an IPD-sized static offset on the flash and made it follow head
        // motion (2026-07-19 report: "flash follows my head and hand, offset
        // from the gun; bullets spawn further out"). The old headset-proven
        // flash lever used these same camera atomics. TLS root keeps only its
        // scale.
        {
            float camBasis[9];
            if (g_camValid.load() && LoadCameraBasis(camBasis))
            {
                memcpy(root.rotation,camBasis,sizeof(camBasis));
                root.translation[0]=g_camX.load();
                root.translation[1]=g_camY.load();
                root.translation[2]=g_camZ.load();
            }
        }
        // IDENTICAL hand target as the visible gun (one shared definition) so
        // muzzle flash and weapon can never diverge. A dual-wield secondary is
        // carried by the slot's LEFT hand; the hand follows the controller and
        // the weapon/marker assembly inherits that same rigid hand delta.
        const int transformAnchor=dual ? leftWrist : wrist;
        if (transformAnchor<0 || transformAnchor>=count) return false;
        float meshScale=1.0f;
        BoneMatrix desiredWristWorld{};
        if (!DesiredWristWorld(dual,desiredWristWorld,meshScale)) return false;
        if (!ApplyControllerToMarkerBonesWithTarget(
                root,desiredWristWorld,meshScale,bones,count,transformAnchor))
            return false;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            LOG("M3: marker/muzzle bones rigid-parented with the same transform as the gun");
        return true;
    }

    bool ApplyControllerToMarkerBones(int player, BoneMatrix* bones, int count,
                                      int wrist, int cameraControl, bool dual)
    {
        (void)cameraControl;
        BoneMatrix root{};
        if (!LoadCachedRenderRoot(player,root)) return false;
        const int leftWrist =
            g_fpLWristIndex[1].load(std::memory_order_acquire);
        return ApplyControllerToMarkerBonesFromRoot(
            root,bones,count,wrist,leftWrist,dual);
    }

    bool __fastcall FpInterpolateHook(int view,int id,int slot,
                                      BoneMatrix** outBones,int* outCount)
    {
        const bool result=g_origFpInterpolate(view,id,slot,outBones,outCount);
        if (slot==0 || slot==1)
        {
        g_fpInterpolationContexts[slot]={};
        if (result && outBones && outCount && *outBones)
        {
            const int count=*outCount;
            const int cached=g_fpBoneCount[slot].load(std::memory_order_acquire);
            const int wrist=g_fpWristIndex[slot].load(std::memory_order_acquire);
            const int cameraControl=g_fpOrientationIndex[slot].load(std::memory_order_acquire);
            const int elbow=g_fpElbowIndex[slot].load(std::memory_order_acquire);
            const int shoulder=g_fpShoulderIndex[slot].load(std::memory_order_acquire);
            if (count>0 && count<=64 && cached==count &&
                wrist>=0 && wrist<count && cameraControl>=0 && cameraControl<count)
            {
                auto& context=g_fpInterpolationContexts[slot];
                context.source=*outBones;
                context.count=count;
                context.player=view;
                context.slot=slot;
                context.wrist=wrist;
                context.cameraControl=cameraControl;
                context.elbow=elbow;
                context.shoulder=shoulder;
                context.wristDescendants=
                    g_fpWristDescendants[slot].load(std::memory_order_acquire);
                context.lWrist=g_fpLWristIndex[slot].load(std::memory_order_acquire);
                context.lElbow=g_fpLElbowIndex[slot].load(std::memory_order_acquire);
                context.lShoulder=g_fpLShoulderIndex[slot].load(std::memory_order_acquire);
                context.lWristDescendants=
                    g_fpLWristDescendants[slot].load(std::memory_order_acquire);
                context.valid=true;
                memcpy(g_fpUnmodifiedInterpolations[slot],*outBones,
                       static_cast<size_t>(count)*sizeof(BoneMatrix));
                if (slot==1)
                {
                    static std::atomic<bool> loggedDual{false};
                    if (!loggedDual.exchange(true))
                        LOG("DUAL: slot 1 FP context captured (%d bones, wrist %d, "
                            "camera_control %d) — left-hand weapon path live",
                            count,wrist,cameraControl);
                }
                // Measure the authored barrel-in-wrist direction (row 0 of the
                // wrist rotation = invRot * camera-forward; storage m[c*3+r]).
                // Per slot: each weapon hand auto-aligns to its own ray.
                {
                    const float* wr=g_fpUnmodifiedInterpolations[slot][wrist].rotation;
                    float b[3]={wr[0],wr[3],wr[6]};
                    const float bl=sqrtf(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]);
                    if (bl>1e-4f && isfinite(bl))
                    {
                        b[0]/=bl; b[1]/=bl; b[2]/=bl;
                        if (g_barrelInWristValid[slot].load(std::memory_order_relaxed))
                        {
                            const float a=0.02f;
                            for(int j=0;j<3;++j)
                                b[j]=g_barrelInWrist[slot][j].load(std::memory_order_relaxed)
                                     *(1.0f-a)+b[j]*a;
                            const float l2=sqrtf(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]);
                            if (l2>1e-4f){ b[0]/=l2; b[1]/=l2; b[2]/=l2; }
                        }
                        for(int j=0;j<3;++j)
                            g_barrelInWrist[slot][j].store(b[j],std::memory_order_relaxed);
                        g_barrelInWristValid[slot].store(true,std::memory_order_release);
                        static std::atomic<bool> loggedBarrel{false};
                        if (slot==0 && !loggedBarrel.exchange(true))
                            LOG("M3 VRIK: authored barrel-in-wrist measured "
                                "(%.3f, %.3f, %.3f) — expect ~(1,0,0)+cant if the "
                                "camera-forward invariant holds",b[0],b[1],b[2]);
                    }
                }
                ApplyControllerToMarkerBones(view,*outBones,count,wrist,
                                             cameraControl,slot==1);
            }
        }
        }
        return result;
    }

    // TRUE RIGID PARENTING (2026-07-19, replacing the cached-relation
    // reconstruction). Everything below is computed from THIS frame only:
    // the untouched interpolated bones, the actual render root passed to the
    // palette consumer, and a live controller read. One rigid transform T
    // snaps the untouched wrist bone's world pose onto the controller
    // (rotation AND position, all axes), and the same T is applied to every
    // bone, so the assembly stays exactly as authored/animated. No cached
    // wrist->camera_control relation, no synthesized bones, no sim-clock
    // state — the layer the user correctly called a "mask" is gone. Barrel
    // mounting is a CONSTANT user trim (gun_pitch/yaw/roll + gun_forward_m),
    // not a per-frame estimate.
    // ONE shared definition of "where a wrist glued to its controller belongs
    // in the world" — used by the visible palette, the arm IK, AND the
    // muzzle/marker path so the gun, flash, and hands can never diverge. The
    // right hand carries the weapon mount trim + forward standoff; the left
    // hand mirrors the yaw/roll trim and has no standoff.
    bool DesiredWristWorld(bool left, BoneMatrix& out, float& meshScale)
    {
        float basisC[9], posC[3];
        if (!ControllerWorldPoseEx(left, basisC, posC, meshScale))
            return false;
        float mounted[9];
        // Begin with the controller basis. Automatic authored-barrel alignment
        // establishes the zero-slider pose first; user trim is applied later.
        memcpy(mounted, basisC, sizeof(mounted));
        // AUTO BARREL ALIGNMENT (weapon hands only): swing the default hand by
        // the minimal world rotation that puts the measured authored barrel
        // axis exactly on the controller ray (basis column 0) — the SAME ray
        // the cursor and bullet steering use. The barrel therefore sits on the
        // cursor line by construction at zero trim. User calibration comes
        // afterward so pitch, yaw, and roll remain independent controls.
        // Weapon hand = the right hand only: the dual-wield secondary is
        // seated by its weapon NODE directly at the palm point, so the
        // wrist-row-0 swing heuristic must not fight it (23:04 headset
        // result: the heuristic mis-seated the left gun).
        const int barrelSlot=left?-1:0;
        if (barrelSlot>=0 &&
            g_barrelInWristValid[barrelSlot].load(std::memory_order_acquire))
        {
            const float b[3]={g_barrelInWrist[barrelSlot][0].load(std::memory_order_relaxed),
                              g_barrelInWrist[barrelSlot][1].load(std::memory_order_relaxed),
                              g_barrelInWrist[barrelSlot][2].load(std::memory_order_relaxed)};
            float worldBarrel[3]={0,0,0};
            for(int c=0;c<3;++c)
                for(int r=0;r<3;++r)
                    worldBarrel[r]+=mounted[c*3+r]*b[c];
            const float ray[3]={basisC[0],basisC[1],basisC[2]};
            float swing[9],aligned[9];
            ShortestArcRotation(worldBarrel,ray,swing);
            MultiplyBases(swing,mounted,aligned);
            bool ok=true;
            for(int j=0;j<9;++j) if (!isfinite(aligned[j])) { ok=false; break; }
            if (ok)
            {
                memcpy(mounted,aligned,sizeof(aligned));
                static std::atomic<bool> loggedSwing{false};
                if (!loggedSwing.exchange(true))
                {
                    const float d=Clamp(worldBarrel[0]*ray[0]+worldBarrel[1]*ray[1]+
                                        worldBarrel[2]*ray[2],-1.0f,1.0f);
                    LOG("M3 VRIK: auto barrel alignment ACTIVE, first swing %.1f deg",
                        acosf(d)*57.2958f);
                }
            }
        }
        // Right-hand calibration is already part of VR_GetAimPose, so applying
        // it here again would rotate only the mesh a second time. Preserve the
        // existing mirrored presentation trim for the independent left hand.
        if (left)
        {
            float mount[9], trimmed[9];
            BasisFromAngles(-g_config.gun_yaw_deg * 0.0174533f,
                             g_config.gun_pitch_deg * 0.0174533f,
                            -g_config.gun_roll_deg * 0.0174533f, mount);
            MultiplyBases(mounted, mount, trimmed);
            memcpy(mounted, trimmed, sizeof(trimmed));
        }
        out = BoneMatrix{};
        out.scale = 1.0f;
        memcpy(out.rotation, mounted, sizeof(mounted));
        memcpy(out.translation, posC, sizeof(posC));
        return true;
    }

    bool ReconstructVisiblePaletteSource(uint16_t tag,
                                         const FpInterpolationContext& context,
                                         const BoneMatrix& root,
                                         const BoneMatrix* source,
                                         const BoneMatrix*& replacement,
                                         const FpExplicitPoseTargets* explicitTargets = nullptr,
                                         const BoneMatrix* unmodifiedOverride = nullptr)
    {
        if (!context.valid || context.slot<0 || context.slot>1 ||
            source!=context.source ||
            context.count<=0 ||
            context.count>static_cast<int>(kReachFpMaxSourceNodeCount) ||
            context.wrist<0 || context.wrist>=context.count) return false;

        // Dual-wield secondary (slot 1): its LEFT hand follows the left
        // controller. The separate weapon subtree then inherits that solved
        // hand's rigid delta; it is never independently seated on a controller.
        // Slot-1 failures never touch the primary arm diagnostics.
        const bool dual=(context.slot==1);
        const int perfEyeBucket = explicitTargets ? -1 : FramePerfEyeBucket();
        if (perfEyeBucket >= 0)
            g_perfFpPaletteRequests[perfEyeBucket].fetch_add(
                1, std::memory_order_relaxed);
        const BoneMatrix* const unmodified=unmodifiedOverride
            ? unmodifiedOverride
            : g_fpUnmodifiedInterpolations[context.slot];
        const size_t paletteBytes = static_cast<size_t>(context.count) *
            sizeof(BoneMatrix);

        auto cacheMatches = [&](const FpStereoPaletteCache& cache) {
            return cache.valid && cache.tag == tag &&
                cache.player == context.player && cache.slot == context.slot &&
                cache.count == context.count && cache.wrist == context.wrist &&
                cache.elbow == context.elbow &&
                cache.shoulder == context.shoulder &&
                cache.wristDescendants == context.wristDescendants &&
                cache.heldObjectStart == context.heldObjectStart &&
                cache.lWrist == context.lWrist &&
                cache.lElbow == context.lElbow &&
                cache.lShoulder == context.lShoulder &&
                cache.lWristDescendants == context.lWristDescendants &&
                cache.armIk == g_config.arm_ik &&
                memcmp(cache.original, unmodified, paletteBytes) == 0;
        };
        if (g_fpStereoSolveScope.armed)
        {
            for (const auto& cache : g_fpStereoSolveScope.palettes)
            {
                if (!cacheMatches(cache))
                    continue;
                bool reused = false;
                if (memcmp(&cache.root, &root, sizeof(BoneMatrix)) == 0)
                {
                    memcpy(g_fpPaletteScratch, cache.solved, paletteBytes);
                    reused = true;
                }
                else
                {
                    BoneMatrix inverseRoot{}, cachedToCurrent{};
                    reused = InvertBoneMatrix(root, inverseRoot) &&
                        ComposeBoneMatrices(
                            inverseRoot, cache.root, cachedToCurrent);
                    for (int i = 0; reused && i < context.count; ++i)
                        reused = ComposeBoneMatrices(
                            cachedToCurrent, cache.solved[i],
                            g_fpPaletteScratch[i]);
                }
                if (reused)
                {
                    if (perfEyeBucket >= 0)
                        g_perfFpPaletteCacheHits[perfEyeBucket].fetch_add(
                            1, std::memory_order_relaxed);
                    replacement = g_fpPaletteScratch;
                    return true;
                }
            }
        }
        auto cacheSolvedPalette = [&](const BoneMatrix* solved) {
            if (!g_fpStereoSolveScope.armed || !solved)
                return;
            for (auto& cache : g_fpStereoSolveScope.palettes)
            {
                if (cache.valid)
                    continue;
                cache.valid = true;
                cache.tag = tag;
                cache.player = context.player;
                cache.slot = context.slot;
                cache.count = context.count;
                cache.wrist = context.wrist;
                cache.elbow = context.elbow;
                cache.shoulder = context.shoulder;
                cache.wristDescendants = context.wristDescendants;
                cache.heldObjectStart = context.heldObjectStart;
                cache.lWrist = context.lWrist;
                cache.lElbow = context.lElbow;
                cache.lShoulder = context.lShoulder;
                cache.lWristDescendants = context.lWristDescendants;
                cache.armIk = g_config.arm_ik;
                cache.root = root;
                memcpy(cache.original, unmodified, paletteBytes);
                memcpy(cache.solved, solved, paletteBytes);
                if (perfEyeBucket >= 0)
                    g_perfFpPaletteCacheStores[perfEyeBucket].fetch_add(
                        1, std::memory_order_relaxed);
                return;
            }
            if (perfEyeBucket >= 0)
                g_perfFpPaletteCacheFull[perfEyeBucket].fetch_add(
                    1, std::memory_order_relaxed);
        };

        float meshScale=1.0f;
        BoneMatrix desiredWristWorld{};
        bool haveDesiredWrist=false;
        if (explicitTargets)
        {
            if (dual && explicitTargets->leftWristValid)
            {
                desiredWristWorld=explicitTargets->leftWrist;
                meshScale=explicitTargets->leftScale;
                haveDesiredWrist=true;
            }
            else if (!dual && explicitTargets->rightWristValid)
            {
                desiredWristWorld=explicitTargets->rightWrist;
                meshScale=explicitTargets->rightScale;
                haveDesiredWrist=true;
            }
        }
        else
        {
            haveDesiredWrist=DesiredWristWorld(
                dual,desiredWristWorld,meshScale);
        }
        if (!haveDesiredWrist)
        {
            if (dual) return false;
            g_armFailurePublished.store("right-controller-pose",std::memory_order_relaxed);
            g_armFailureSide.store(1,std::memory_order_release);
            return false;
        }
        auto loadLeftWristTarget = [&](BoneMatrix& desired, float& scale) {
            if (explicitTargets)
            {
                if (!explicitTargets->leftWristValid)
                    return false;
                desired=explicitTargets->leftWrist;
                scale=explicitTargets->leftScale;
                return true;
            }
            return DesiredWristWorld(true,desired,scale);
        };
        // CENTER-ROOT WORLD SOLVE (2026-07-19): this function runs once per
        // EYE, and the palette consumer's `root` is that eye's camera. Any
        // world position built from it (the planted shoulder, the solved
        // elbow) shifts by the eye offset, so the two eyes solved DIFFERENT
        // arms — the user saw the bare left arm split between eyes. Build all
        // WORLD-space poses from the live CENTER camera instead; only the
        // final record conversion (invRoot) may use the eye root, which makes
        // the rendered world pose eye-independent and the stereo fuse.
        BoneMatrix centerRoot=root;
        if (explicitTargets)
        {
            if (!explicitTargets->centerRootValid)
                return false;
            centerRoot=explicitTargets->centerRoot;
        }
        else
        {
            float camBasis[9];
            if (g_camValid.load() && LoadCameraBasis(camBasis))
            {
                memcpy(centerRoot.rotation,camBasis,sizeof(camBasis));
                centerRoot.translation[0]=g_camX.load();
                centerRoot.translation[1]=g_camY.load();
                centerRoot.translation[2]=g_camZ.load();
            }
        }

        // SHOULDER LEVELING (2026-07-19): the arm rest pose is composed from the
        // camera frame, so its FULL pitch/roll rides the head — look down and the
        // shoulder swings up into your face (user: "the shoulders follow my head,
        // they don't stay in place"). Build a torso frame with the camera's
        // HEADING only (yaw about world-up +Z; Blam is a Z-up engine, and the
        // camera cols are fwd/left/up per below), level in pitch/roll, at the
        // camera position. Anchor the arm to THAT instead of the pitching camera.
        // The hand + gun ride the controller target and are root-invariant (the
        // root cancels in desired*invWristRest*bone), so this levels the visible
        // UPPER ARM without moving the hand/gun the user already likes. Falls
        // back to the camera frame when looking near-vertical or when disabled.
        BoneMatrix torsoRoot=centerRoot;
        if (g_config.shoulder_level)
        {
            float cb[9];
            if (NormalizedBasis(centerRoot,cb))
            {
                // MEASURED world-up (not assumed) — see g_worldUp / CamCopyHook.
                const float U[3]={
                    explicitTargets ? 0.0f : g_worldUp[0].load(),
                    explicitTargets ? 0.0f : g_worldUp[1].load(),
                    explicitTargets ? 1.0f : g_worldUp[2].load()};
                float fwd[3]={cb[0],cb[1],cb[2]};           // camera forward, world
                const float d=fwd[0]*U[0]+fwd[1]*U[1]+fwd[2]*U[2];
                float fH[3]={fwd[0]-U[0]*d,fwd[1]-U[1]*d,fwd[2]-U[2]*d};
                const float fl=sqrtf(fH[0]*fH[0]+fH[1]*fH[1]+fH[2]*fH[2]);
                if (fl>1e-3f)                                // not looking straight up/down
                {
                    fH[0]/=fl; fH[1]/=fl; fH[2]/=fl;
                    float L[3]={U[1]*fH[2]-U[2]*fH[1],       // left = up x fwd
                                U[2]*fH[0]-U[0]*fH[2],
                                U[0]*fH[1]-U[1]*fH[0]};
                    const float ll=sqrtf(L[0]*L[0]+L[1]*L[1]+L[2]*L[2]);
                    if (ll>1e-3f)
                    {
                        L[0]/=ll; L[1]/=ll; L[2]/=ll;
                        torsoRoot.rotation[0]=fH[0]; torsoRoot.rotation[1]=fH[1]; torsoRoot.rotation[2]=fH[2];
                        torsoRoot.rotation[3]=L[0];  torsoRoot.rotation[4]=L[1];  torsoRoot.rotation[5]=L[2];
                        torsoRoot.rotation[6]=U[0];  torsoRoot.rotation[7]=U[1];  torsoRoot.rotation[8]=U[2];
                    }
                }
            }
        }
        // The arm rest pose and elbow pole are built from this frame; when
        // shoulder_level is off it equals centerRoot, so behavior is unchanged.
        const BoneMatrix& armRoot = torsoRoot;

        // VRIK ARM IK: keep the body exactly where the game posed it and bend
        // ONLY the arms so each wrist reaches its controller — shoulders
        // planted, elbows solved analytically (ik.cpp), hand + gun ride the
        // wrist rigidly. Right arm is required; the left arm applies when its
        // chain resolved and the left controller is tracked.
        const bool carrierChainValid=dual
            ? (context.lShoulder>=0 && context.lShoulder<context.count &&
               context.lElbow>=0 && context.lElbow<context.count &&
               context.lWrist>=0 && context.lWrist<context.count)
            : (context.shoulder>=0 && context.shoulder<context.count &&
               context.elbow>=0 && context.elbow<context.count);
        if (g_config.arm_ik && carrierChainValid)
        {
            const BoneMatrix* unmod=unmodified;
            memcpy(g_fpPaletteScratch,unmod,
                   static_cast<size_t>(context.count)*sizeof(BoneMatrix));
            BoneMatrix invRoot{};
            if (InvertBoneMatrix(root,invRoot))
            {
                // Reach supplies an explicit head-centre root.  Convert every
                // authored record through that centre before modifying either
                // arm, so the cached world pose cannot inherit whichever eye
                // happened to render first.  Halo 3/ODST retain their existing
                // per-title path when explicitTargets is null.
                if (explicitTargets)
                {
                    BoneMatrix centerToEye{};
                    if (!ComposeBoneMatrices(invRoot,centerRoot,centerToEye))
                        return false;
                    for (int i=0;i<context.count;++i)
                        if (!ComposeBoneMatrices(centerToEye,unmod[i],
                                                 g_fpPaletteScratch[i]))
                            return false;
                }
                // IK divergence probe (log-only): capture the per-eye solve
                // inputs so a single desktop/headset session names WHICH input
                // differs between the two eye passes (root by design; anything
                // else is the left-arm-splits-between-eyes bug). Filled by
                // applyArm, compared when eye 1 lands against eye 0.
                struct ArmProbe { float S[3]; float E[3]; float T[3];
                                  float upperLen; float lowerLen; };
                ArmProbe probeLeft{};
                bool probeLeftValid=false;
                auto applyArm=[&](int shoulder,int elbow,int wrist,uint64_t mask,
                                  bool carryHeldObjects,
                                  const BoneMatrix& desired,float outSign,
                                  ArmProbe* probeOut,float shoulderDrop)->bool
                {
                    g_armFailWhy="compose-rest";
                    BoneMatrix shW{},elW{},wrW{};
                    if (!ComposeBoneMatrices(armRoot,unmod[shoulder],shW) ||
                        !ComposeBoneMatrices(armRoot,unmod[elbow],elW) ||
                        !ComposeBoneMatrices(armRoot,unmod[wrist],wrW)) return false;
                    // Lower the shoulder anchor along the view-DOWN axis (camera
                    // up column, negated) so Chief's arm sits lower and stops
                    // clipping the face (drop; right arm only, shoulderDrop>0),
                    // and push BOTH shoulders back along the forward axis so a
                    // title that plants them in front of you (ODST) seats them at
                    // the torso. Halo 3 keeps shoulder_back_m=0 -> unchanged.
                    const float shoulderBack=g_config.shoulder_back_m;
                    if (shoulderDrop>0.0f || shoulderBack!=0.0f)
                    {
                        float rb[9];
                        if (NormalizedBasis(armRoot,rb))
                            for (int j=0;j<3;++j)
                                shW.translation[j]-=rb[6+j]*shoulderDrop
                                                   +rb[0+j]*shoulderBack;
                    }
                    const float* S=shW.translation; const float* Er=elW.translation;
                    const float* Wr=wrW.translation;
                    auto dist=[](const float* a,const float* b){
                        const float dx=a[0]-b[0],dy=a[1]-b[1],dz=a[2]-b[2];
                        return sqrtf(dx*dx+dy*dy+dz*dz); };
                    const float upperLen=dist(S,Er), lowerLen=dist(Er,Wr);
                    const float* T=desired.translation;
                    float mid[3]={(S[0]+Wr[0])*0.5f,(S[1]+Wr[1])*0.5f,(S[2]+Wr[2])*0.5f};
                    float pole[3]={Er[0]-mid[0],Er[1]-mid[1],Er[2]-mid[2]};
                    if (pole[0]*pole[0]+pole[1]*pole[1]+pole[2]*pole[2]<1e-6f)
                    { pole[0]=0; pole[1]=0; pole[2]=-1; }
                    // Elbow pole bias: OUT (away from the torso, camera left/
                    // right column) and DOWN, 75/25 over the authored pole
                    // (user: elbows "feel inward — I want them to stick
                    // outward"). Root is the camera: cols fwd/left/up.
                    {
                        float rootB[9];
                        if (NormalizedBasis(armRoot,rootB))
                        {
                            const float outDir[3]={
                                rootB[3]*outSign-rootB[6]*0.6f,
                                rootB[4]*outSign-rootB[7]*0.6f,
                                rootB[5]*outSign-rootB[8]*0.6f};
                            const float ol=sqrtf(outDir[0]*outDir[0]+
                                outDir[1]*outDir[1]+outDir[2]*outDir[2]);
                            const float pl=sqrtf(pole[0]*pole[0]+
                                pole[1]*pole[1]+pole[2]*pole[2]);
                            if (ol>1e-4f && pl>1e-4f)
                                for (int j=0;j<3;++j)
                                    pole[j]=pole[j]/pl*0.25f+outDir[j]/ol*0.75f;
                        }
                    }
                    // FULL EXTENSION: when the controller is beyond the arm's
                    // natural reach, the two-bone solver clamps to a straight
                    // chain that stops SHORT of the hand, so the hand visibly
                    // detaches from the forearm. Stretch both bones so the arm
                    // always reaches the wrist (skinning stretches the mesh with
                    // it) — capped so it never looks rubbery. Right arm only
                    // (the user's left is perfect and untouched here anyway).
                    float solveUpper=upperLen, solveLower=lowerLen;
                    {
                        const float reach=upperLen+lowerLen;
                        const float td=sqrtf((T[0]-S[0])*(T[0]-S[0])+
                                             (T[1]-S[1])*(T[1]-S[1])+
                                             (T[2]-S[2])*(T[2]-S[2]));
                        if (reach>1e-4f && td>reach)
                        {
                            const float k=fminf(td/reach,1.8f);
                            solveUpper*=k; solveLower*=k;
                        }
                    }
                    float E[3];
                    g_armFailWhy="two-bone-solve";
                    if (!IK_SolveTwoBone(S,T,solveUpper,solveLower,pole,E)) return false;
                    if (probeOut)
                    {
                        for(int j=0;j<3;++j){ probeOut->S[j]=S[j]; probeOut->E[j]=E[j];
                                              probeOut->T[j]=T[j]; }
                        probeOut->upperLen=upperLen; probeOut->lowerLen=lowerLen;
                    }
                    auto unit=[](const float* a,const float* b,float* o){
                        o[0]=a[0]-b[0];o[1]=a[1]-b[1];o[2]=a[2]-b[2];
                        const float l=sqrtf(o[0]*o[0]+o[1]*o[1]+o[2]*o[2]);
                        if (l>1e-6f){o[0]/=l;o[1]/=l;o[2]/=l;} };
                    float restU[3],restF[3],newU[3],newF[3];
                    unit(Er,S,restU); unit(Wr,Er,restF);
                    unit(E,S,newU);   unit(T,E,newF);
                    float Ru[9],Rf[9];
                    ShortestArcRotation(restU,newU,Ru);
                    ShortestArcRotation(restF,newF,Rf);
                    BoneMatrix newSh=shW; MultiplyBases(Ru,shW.rotation,newSh.rotation);
                    BoneMatrix newEl=elW; MultiplyBases(Rf,elW.rotation,newEl.rotation);
                    newEl.translation[0]=E[0]; newEl.translation[1]=E[1]; newEl.translation[2]=E[2];
                    BoneMatrix invWrRest{},D{};
                    g_armFailWhy="invert-wrist";
                    if (!InvertBoneMatrix(wrW,invWrRest)) return false;
                    ComposeBoneMatrices(desired,invWrRest,D);
                    ComposeBoneMatrices(invRoot,newSh,g_fpPaletteScratch[shoulder]);
                    ComposeBoneMatrices(invRoot,newEl,g_fpPaletteScratch[elbow]);
                    for (int i=0;i<context.count;++i)
                    {
                        const bool handDescendant=i<64 &&
                            (mask&(uint64_t{1}<<i));
                        const bool heldObject=carryHeldObjects &&
                            context.heldObjectStart>=0 &&
                            i>=context.heldObjectStart;
                        if (!handDescendant && !heldObject) continue;
                        BoneMatrix boneW{},newW{};
                        if (ComposeBoneMatrices(armRoot,unmod[i],boneW) &&
                            ComposeBoneMatrices(D,boneW,newW))
                            ComposeBoneMatrices(invRoot,newW,g_fpPaletteScratch[i]);
                    }
                    g_armFailWhy=nullptr;
                    return true;
                };
                // Primary keeps its right-hand path. A dual weapon is owned by
                // the actual LEFT hand, which follows the left controller.
                const bool handApplied=dual
                    ? applyArm(context.lShoulder,context.lElbow,context.lWrist,
                               context.lWristDescendants,true,desiredWristWorld,
                               1.0f,nullptr,0.0f)
                    : applyArm(context.shoulder,context.elbow,context.wrist,
                               context.wristDescendants,true,desiredWristWorld,
                               -1.0f,nullptr,g_config.right_shoulder_drop);
                if (handApplied)
                {
                    if (dual)
                    {
                        BoneMatrix authoredLeftWorld{},invAuthoredLeft{},handDelta{};
                        if (!ComposeBoneMatrices(armRoot,unmod[context.lWrist],
                                                 authoredLeftWorld) ||
                            !InvertBoneMatrix(authoredLeftWorld,invAuthoredLeft) ||
                            !ComposeBoneMatrices(desiredWristWorld,invAuthoredLeft,
                                                 handDelta)) return false;
                        for (int i=0;i<context.count && i<64;++i)
                        {
                            if (!(context.wristDescendants&(1ull<<i))) continue;
                            BoneMatrix authoredWorld{},lockedWorld{};
                            if (!ComposeBoneMatrices(armRoot,unmod[i],authoredWorld) ||
                                !ComposeBoneMatrices(handDelta,authoredWorld,lockedWorld) ||
                                !ComposeBoneMatrices(invRoot,lockedWorld,
                                                     g_fpPaletteScratch[i])) return false;
                        }
                        static std::atomic<bool> loggedDualIk{false};
                        if (!explicitTargets && !loggedDualIk.exchange(true))
                            LOG("DUAL VRIK: slot 1 arm IK active on the LEFT "
                                "controller (wrist %d, elbow %d, shoulder %d)",
                                context.lWrist,context.lElbow,context.lShoulder);
                    }
                    // Left SUPPORT arm (primary weapon only): same treatment
                    // onto the left controller. During dual wield the left
                    // hand holds slot 1's weapon instead, and slot 1's own
                    // mirror-side chain stays game-animated.
                    auto publishLeftFailure=[](const char* why){
                        g_armFailurePublished.store(why,std::memory_order_relaxed);
                        g_armFailureSide.store(2,std::memory_order_release);
                    };
                    if (dual)
                    {
                        // HEADSET RESULTS: 22:51 build — the visible arm is
                        // skinned to the lWrist/lElbow/lShoulder chain, not the
                        // gun chain. 22:59 build — carrying that arm at its
                        // AUTHORED offset from the gun put the hand ~1 m left:
                        // with the stock weapon-IK branch patched off, the
                        // secondary's animation never poses l_hand on the grip,
                        // so there is no authored grip relation to preserve.
                        // Target the visible hand at the LEFT CONTROLLER itself
                        // — the identical, user-tuned support-hand treatment
                        // (palm correction + mirrored trim, F1 slider applies).
                        if (context.lShoulder>=0 && context.lShoulder<context.count &&
                            context.lElbow>=0 && context.lElbow<context.count &&
                            context.lWrist>=0 && context.lWrist<context.count)
                        {
                            BoneMatrix desiredL{}; float leftScale=1.0f;
                            if (loadLeftWristTarget(desiredL,leftScale))
                            {
                                static std::atomic<bool> loggedDualArm{false};
                                if (applyArm(context.lShoulder,context.lElbow,
                                             context.lWrist,context.lWristDescendants,
                                             false,desiredL,1.0f,nullptr,0.0f) &&
                                    !explicitTargets &&
                                    !loggedDualArm.exchange(true))
                                    LOG("DUAL VRIK: secondary VISIBLE hand bound to "
                                        "the left controller (wrist %d, elbow %d, "
                                        "shoulder %d)",
                                        context.lWrist,context.lElbow,context.lShoulder);
                            }
                        }
                    }
                    else if (context.lShoulder>=0 && context.lShoulder<context.count &&
                        context.lElbow>=0 && context.lElbow<context.count &&
                        context.lWrist>=0 && context.lWrist<context.count)
                    {
                        BoneMatrix desiredLeft{}; float leftScale=1.0f;
                        if (loadLeftWristTarget(desiredLeft,leftScale))
                        {
                            static std::atomic<bool> loggedLeft{false};
                            if (applyArm(context.lShoulder,context.lElbow,context.lWrist,
                                         context.lWristDescendants,false,desiredLeft,1.0f,
                                         &probeLeft,0.0f))
                            {
                                probeLeftValid=true;
                                g_armFailurePublished.store(nullptr,std::memory_order_relaxed);
                                g_armFailureSide.store(0,std::memory_order_release);
                                if (!explicitTargets &&
                                    !loggedLeft.exchange(true))
                                    LOG("M3 VRIK: LEFT arm on the left controller "
                                        "(wrist %d, elbow %d, shoulder %d)",
                                        context.lWrist,context.lElbow,context.lShoulder);
                            }
                            else publishLeftFailure(g_armFailWhy?g_armFailWhy:"apply-arm");
                        }
                        else if (!explicitTargets)
                            publishLeftFailure("left-controller-pose");
                    }
                    else publishLeftFailure("left-chain-indices");
                    // Compare this eye's LEFT-arm solve inputs to the other
                    // eye's. dRoot large is expected (eye offset). Any nonzero
                    // dCenterRoot / dWrist / dLens / dDesired names the leaking
                    // per-eye input behind the left-arm split. Rate-limited.
                    if (probeLeftValid && !explicitTargets)
                    {
                        static ArmProbe eyeProbe[2]{};
                        static BoneMatrix eyeRoot[2]{}, eyeCenterRoot[2]{};
                        static bool eyeHave[2]={false,false};
                        const int eye=g_stereoEye.load();
                        if (eye==0 || eye==1)
                        {
                            eyeProbe[eye]=probeLeft; eyeRoot[eye]=root;
                            eyeCenterRoot[eye]=centerRoot; eyeHave[eye]=true;
                            static std::atomic<uint64_t> lastMs{0};
                            const uint64_t now=GetTickCount64();
                            if (eye==1 && eyeHave[0] &&
                                now-lastMs.load()>2000)
                            {
                                lastMs.store(now);
                                auto d3=[](const float* a,const float* b){
                                    float m=0; for(int j=0;j<3;++j){
                                        const float e=fabsf(a[j]-b[j]); if(e>m)m=e;} return m;};
                                const float dRoot=d3(eyeRoot[0].translation,eyeRoot[1].translation);
                                const float dCenter=d3(eyeCenterRoot[0].translation,
                                                       eyeCenterRoot[1].translation);
                                const float dWrist=d3(eyeProbe[0].T,eyeProbe[1].T);
                                const float dElbow=d3(eyeProbe[0].E,eyeProbe[1].E);
                                const float dLens=fabsf(eyeProbe[0].upperLen-eyeProbe[1].upperLen)+
                                                  fabsf(eyeProbe[0].lowerLen-eyeProbe[1].lowerLen);
                                LOG("IK-PROBE dRoot=%.4f dCenterRoot=%.4f dDesiredWrist=%.4f "
                                    "dElbow=%.4f dLens=%.4f (dRoot big=OK; others should be ~0)",
                                    dRoot,dCenter,dWrist,dElbow,dLens);
                            }
                        }
                    }
                    // Uniform size trim about the gripping hand (grip stays put).
                    // Each side scales its OWN wrist subtree. Before 2026-07-20
                    // the mask here was always context.wristDescendants — the
                    // RIGHT wrist's bones — even when the anchor switched to the
                    // left wrist, so no left-hand size value ever reached a bone
                    // ("the left arm is not scalable").
                    auto trimSubtree=[&](int anchorBone,uint64_t mask,
                                         bool carryHeldObjects,float scale)
                    {
                        if (scale==1.0f || (!mask && !carryHeldObjects) ||
                            anchorBone<0 || anchorBone>=context.count) return;
                        const float anchor[3]={
                            g_fpPaletteScratch[anchorBone].translation[0],
                            g_fpPaletteScratch[anchorBone].translation[1],
                            g_fpPaletteScratch[anchorBone].translation[2]};
                        for (int i=0;i<context.count;++i)
                        {
                            const bool handDescendant=i<64 &&
                                (mask&(uint64_t{1}<<i));
                            const bool heldObject=carryHeldObjects &&
                                context.heldObjectStart>=0 &&
                                i>=context.heldObjectStart;
                            if (!handDescendant && !heldObject) continue;
                            BoneMatrix& bone=g_fpPaletteScratch[i];
                            for (int r=0;r<3;++r)
                                bone.translation[r]=anchor[r]+
                                    (bone.translation[r]-anchor[r])*scale;
                            bone.scale*=scale;
                        }
                    };
                    const float leftScale=explicitTargets
                        ? explicitTargets->leftScale
                        : Clamp(g_config.left_hand_scale,0.3f,3.0f);
                    if (dual)
                    {
                        // The dual-wield carrier IS the left hand; its own gun
                        // rides that subtree, so the left size governs both.
                        trimSubtree(context.lWrist,context.lWristDescendants,true,leftScale);
                    }
                    else
                    {
                        trimSubtree(context.wrist,context.wristDescendants,true,meshScale);
                        // The support hand is game-animated onto the gun rather
                        // than IK-solved, but its bones are still ours to size.
                        // Exclude anything already trimmed above so a skeleton
                        // that nests the two subtrees cannot scale a bone twice.
                        trimSubtree(context.lWrist,
                                    context.lWristDescendants&~context.wristDescendants,
                                    false,leftScale);
                    }
                    // NOTE: a "length squash along the barrel" is NOT possible
                    // here and must not be re-attempted: the weapon mesh is
                    // rigid geometry on essentially one bone, and BoneMatrix
                    // carries a single UNIFORM scale. Squashing bone ORIGINS
                    // only translates the mesh (user-confirmed 2026-07-19:
                    // "the length just moves the gun"). Size trims are
                    // gun_scale (uniform) and gun_forward_m (seat depth).
                    replacement=g_fpPaletteScratch;
                    cacheSolvedPalette(g_fpPaletteScratch);
                    static std::atomic<bool> loggedIk{false};
                    if (!explicitTargets && !loggedIk.exchange(true))
                        LOG("M3 VRIK: arm IK active — shoulder %d planted, elbow %d solved, "
                            "wrist %d + %lld subtree bones to controller",
                            context.shoulder,context.elbow,context.wrist,
                            (long long)__popcnt64(context.wristDescendants));
                    // Full-solve cache misses. Exact duplicate palettes inside
                    // this stereo pair return above without repeating arm IK.
                    if (!explicitTargets)
                    {
                        static std::atomic<uint32_t> solves{0};
                        static std::atomic<DWORD> lastLog{GetTickCount()};
                        g_perfFpPaletteFullSolves[perfEyeBucket].fetch_add(
                            1, std::memory_order_relaxed);
                        solves.fetch_add(1);
                        const DWORD now=GetTickCount(); DWORD last=lastLog.load();
                        if (now-last>=10000 && lastLog.compare_exchange_strong(last,now))
                        {
                            g_perfPaletteRateLogWrites.fetch_add(
                                1, std::memory_order_relaxed);
                            LOG("PERF: FP palette full solves %.0f/sec "
                                "(exact stereo-pair cache misses)",
                                solves.exchange(0)*1000.0/(now-last));
                        }
                    }
                    return true;
                }
                if (!dual)
                {
                    g_armFailurePublished.store(g_armFailWhy?g_armFailWhy:"right-apply-arm",
                                                std::memory_order_relaxed);
                    g_armFailureSide.store(1,std::memory_order_release);
                }
            }
            else if (!dual)
            {
                g_armFailurePublished.store("invert-root",std::memory_order_relaxed);
                g_armFailureSide.store(1,std::memory_order_release);
            }
            // IK could not solve this frame — fall through to rigid parent.
        }

        // M = rootEye^-1 * T * rootCenter applied per record: the WORLD result
        else if (g_config.arm_ik && !dual)
        {
            g_armFailurePublished.store("right-chain-indices",std::memory_order_relaxed);
            g_armFailureSide.store(1,std::memory_order_release);
        }
        // T * rootCenter * record is built from the center camera (identical
        // in both eyes), and only the record conversion uses the eye root.
        BoneMatrix wristWorld{},inverseWristWorld{},t{},inverseRoot{},m{},tRoot{};
        if (!ComposeBoneMatrices(centerRoot,unmodified[context.wrist],wristWorld) ||
            !InvertBoneMatrix(wristWorld,inverseWristWorld) ||
            !ComposeBoneMatrices(desiredWristWorld,inverseWristWorld,t) ||
            !InvertBoneMatrix(root,inverseRoot) ||
            !ComposeBoneMatrices(t,centerRoot,tRoot) ||
            !ComposeBoneMatrices(inverseRoot,tRoot,m)) return false;

        for (int i=0;i<context.count;++i)
            if (!ComposeBoneMatrices(m,unmodified[i],g_fpPaletteScratch[i])) return false;

        // Uniform user size trim, applied about the wrist so the grip stays on
        // the controller.
        if (meshScale!=1.0f)
        {
            const float* wristT=g_fpPaletteScratch[context.wrist].translation;
            const float anchor[3]={wristT[0],wristT[1],wristT[2]};
            for (int i=0;i<context.count;++i)
            {
                BoneMatrix& bone=g_fpPaletteScratch[i];
                for (int r=0;r<3;++r)
                    bone.translation[r]=anchor[r]+(bone.translation[r]-anchor[r])*meshScale;
                bone.scale*=meshScale;
                if (!isfinite(bone.scale) || !isfinite(bone.translation[0])) return false;
            }
        }
        // Left hand size in the rigid path. The loop above already scaled the
        // WHOLE assembly by meshScale, so only the remaining ratio is applied
        // here — the left hand ends up at left_hand_scale either way, and the
        // slider behaves the same with arm IK on or off.
        {
            const float leftScale=explicitTargets
                ? explicitTargets->leftScale
                : Clamp(g_config.left_hand_scale,0.3f,3.0f);
            const float relative=leftScale/meshScale;
            if (context.lWristDescendants && context.lWrist>=0 &&
                context.lWrist<context.count &&
                isfinite(relative) && relative!=1.0f)
            {
                const float* lT=g_fpPaletteScratch[context.lWrist].translation;
                const float anchor[3]={lT[0],lT[1],lT[2]};
                for (int i=0;i<context.count && i<64;++i)
                {
                    if (!(context.lWristDescendants&(1ull<<i))) continue;
                    BoneMatrix& bone=g_fpPaletteScratch[i];
                    for (int r=0;r<3;++r)
                        bone.translation[r]=anchor[r]+
                            (bone.translation[r]-anchor[r])*relative;
                    bone.scale*=relative;
                    if (!isfinite(bone.scale) || !isfinite(bone.translation[0])) return false;
                }
            }
        }

        replacement=g_fpPaletteScratch;
        cacheSolvedPalette(g_fpPaletteScratch);
        static std::atomic<bool> logged{false};
        if (!explicitTargets && !logged.exchange(true))
            LOG("M3: FP palette rigid-parented to the controller "
                "(player %d, %d bones, wrist %d, single same-frame transform)",
                context.player,context.count,context.wrist);
        return true;
    }

    void __fastcall FpVisiblePaletteHook(uint16_t tag, const BoneMatrix* root,
                                         BoneMatrix* destination, uintptr_t unused,
                                         const BoneMatrix* source, const int32_t* boneMap)
    {
        // Match this palette submission to its slot's interpolation context by
        // the source pointer (each slot interpolates into its own bank), then
        // consume that context so a stale frame can never be re-applied.
        FpInterpolationContext context{};
        for (auto& candidate : g_fpInterpolationContexts)
            if (candidate.valid && source && candidate.source==source)
            {
                context=candidate;
                candidate.valid=false;
                break;
            }
        const BoneMatrix* selectedSource=source;
        // Never let the visible palette consume the live buffer after the
        // marker/effect preservation rewrite. If the final reconstruction is
        // temporarily unavailable (for example while a new weapon's authored
        // relation is publishing), use the untouched snapshot for this frame.
        if (context.valid && source==context.source)
            selectedSource=g_fpUnmodifiedInterpolations[context.slot];
        bool reconstructed=false;
        if (root && source)
            reconstructed=ReconstructVisiblePaletteSource(
                tag,context,*root,source,selectedSource);

        // FLOATING HANDS (optional, OFF by default): a pure presentation filter
        // over the already-solved palette. The VRIK solve above still tracks the
        // hands to the controllers exactly as normal; here we only collapse the
        // bones that are NOT part of either hand-or-gun subtree — the shoulders,
        // elbows, and forearms — so their skinned geometry vanishes and only the
        // hands and held guns remain. Scale is a PROVEN render input (the same
        // field the gun_scale trim resizes the visible mesh with), so shrinking a
        // bone's scale toward zero drives its vertices to the joint origin: an
        // invisible speck, no crash risk. Only runs when we own the scratch
        // buffer (reconstruction succeeded); otherwise the arms simply show this
        // frame rather than risk mutating an engine-owned buffer.
        if (g_config.floating_hands && reconstructed && context.valid &&
            selectedSource==g_fpPaletteScratch &&
            context.count>0 && context.count<=64)
        {
            const uint64_t keep=context.wristDescendants|context.lWristDescendants;
            for (int i=0;i<context.count;++i)
                if (!(keep&(1ull<<i)))
                    g_fpPaletteScratch[i].scale=0.0001f; // collapse to the joint
        }

        // The scope is a magnified world view, not a second first-person view.
        // Let Halo finish the normal FP submission path, but feed its final
        // palette a private zero-scale copy so gun, hands, and arms contribute
        // no pixels. The normal stereo palettes remain completely untouched.
        if (g_scopeRenderActive.load(std::memory_order_acquire) &&
            context.valid && selectedSource &&
            context.count>0 && context.count<=64)
        {
            memcpy(g_scopeHiddenPalette,selectedSource,
                   static_cast<size_t>(context.count)*sizeof(BoneMatrix));
            for (int i=0;i<context.count;++i)
                g_scopeHiddenPalette[i].scale=0.0001f;
            selectedSource=g_scopeHiddenPalette;
        }

        g_origFpVisiblePalette(tag,root,destination,unused,selectedSource,boneMap);

        // Collect every UNIQUE final-palette submission, not just the first
        // one for a weapon. A shotgun-only secondary arm palette can otherwise
        // render the authored pump grip over the correctly solved fp_body.
        // Atomic publication only; Present owns all formatting and logging.
        const uint64_t key=g_fpSkeletonKey.load(std::memory_order_acquire);
        if (key)
        {
            uint64_t signature=key;
            signature=signature*31+tag;
            signature=signature*31+(context.valid?1:0);
            signature=signature*31+(reconstructed?1:0);
            static thread_local uint64_t seen[16]{};
            static thread_local int seenCount=0;
            bool known=false;
            for(int i=0;i<seenCount;++i) if(seen[i]==signature){known=true;break;}
            if(!known && seenCount<16)
            {
                seen[seenCount++]=signature;
                const int slot=g_fpBoneMapSnapshotCount.fetch_add(
                    1,std::memory_order_acq_rel);
                if(slot<16)
                {
                    auto& snap=g_fpBoneMapSnapshots[slot];
                    const int n=(context.valid && boneMap)
                        ? (context.count>64?64:context.count) : 0;
                    snap.sequence.fetch_add(1,std::memory_order_acq_rel);
                    snap.skeletonKey.store(key,std::memory_order_relaxed);
                    snap.tag.store(tag,std::memory_order_relaxed);
                    snap.count.store(n,std::memory_order_relaxed);
                    snap.reconstructed.store(reconstructed?1:0,
                                             std::memory_order_relaxed);
                    for(int i=0;i<n;++i)
                        snap.map[i].store(boneMap[i],std::memory_order_relaxed);
                    snap.sequence.fetch_add(1,std::memory_order_release);
                }
            }
        }
    }

    void __fastcall FpCameraRebuildHook(void* view, unsigned char flag)
    {
        // Let the engine do its full rebuild (including publishing
        // render_first_person_fov_scale for whatever else reads it) ...
        g_origFpCameraRebuild(view, flag);
        // 0x279BEC just built a compressed viewmodel camera + projection at
        // {view+0x08, view+0x1E8} (its argument's own offsets). While one of
        // OUR eye renders is active, overwrite BOTH with this eye's WORLD
        // camera and WORLD derived block (position, orientation, FOV AND the
        // depth terms), then re-run the engine's own constant uploader. This
        // is the last writer before the FP draw, so the gun/HUD render in true
        // world perspective instead of the crushed viewmodel depth slab.
        // No +0x6C8 gate: the earlier gate never matched (proven by the
        // diagnostic) and the layer kept its flattened depth. Applying to
        // whatever 0x279BEC rebuilds during our eye pass is exactly the set of
        // FP overlays that need world depth.
        char* eyeView = static_cast<char*>(g_eyeFpView.load(std::memory_order_acquire));
        if (!view || !eyeView) return;
        char* base = static_cast<char*>(view);
        memcpy(base + 0x08, g_eyeCompactCamera, sizeof(g_eyeCompactCamera));
        memcpy(base + 0x1E8, g_eyeDerivedBlock, sizeof(g_eyeDerivedBlock));
        if (g_fpCameraUpload)
            g_fpCameraUpload(base + 0x08, base + 0x1E8);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            LOG("M3: per-eye FP camera active — gun/HUD now render in full world "
                "projection (depth uncrushed)");
    }

    // (EnforceHudElements + ChudStateCopyHook REMOVED 2026-07-19 evening. They
    // force-wrote chud+0x144..0x14A every frame with an offset map the headset
    // DISPROVED (0x146 was a nav dot, not the crosshair; 0x32F97C copies only
    // 0x144..0x147) — the stomping suppressed the whole HUD except the objective
    // text and the F1 element checkboxes did nothing. The CHUD struct is now
    // fully game-managed; the only element control is the class-gated
    // crosshair predicate in 0x2EDF24. Do not write into
    // the chud byte block again without headset-verified offsets.)

    // DIAGNOSTIC (hud_probe): log the bytes in the CHUD struct that CHANGE, to
    // locate (a) the enemy target-lock state that turns the OG reticle red and
    // (b) the per-element visibility flags (health / motion sensor / ammo).
    // Aim at an enemy, then away, then toggle HUD elements — the flipped
    // offsets appear in the log. Log-only; changes nothing. Called from
    // CamCopyHook where the CHUD pointer is already resolved.
    void ChudProbe()
    {
        if (!g_config.hud_probe || !g_engineTlsIndex) return;
        auto** slots=reinterpret_cast<void**>(__readgsqword(0x58));
        if (!slots) return;
        auto* tls=reinterpret_cast<unsigned char*>(slots[*g_engineTlsIndex]);
        if (!tls) return;
        auto* chud=*reinterpret_cast<unsigned char**>(tls+0x220);
        if (!chud) return;
        // A window generous enough to span the known crosshair/show bytes
        // (0x144/0x146) plus nearby state. We force 0x144/0x146 every frame, so
        // ignore those two offsets in the diff.
        constexpr int kStart=0x100, kEnd=0x260;
        static unsigned char prev[kEnd-kStart];
        static bool have=false;
        if (!have) { memcpy(prev,chud+kStart,sizeof(prev)); have=true;
            LOG("HUD-PROBE armed: watching CHUD +0x%03X..+0x%03X for changes "
                "(aim at an enemy to find the red-reticle byte)",kStart,kEnd); return; }
        static std::atomic<uint64_t> lastMs{0};
        const uint64_t now=GetTickCount64();
        char line[512]; int n=0; int changes=0;
        for (int off=kStart; off<kEnd; ++off)
        {
            const int i=off-kStart;
            const unsigned char cur=chud[off];
            if (cur==prev[i]) continue;
            if (off==0x144 || off==0x146) { prev[i]=cur; continue; } // we drive these
            if (changes<24 && n<(int)sizeof(line)-24)
                n+=snprintf(line+n,sizeof(line)-n,"+0x%03X:%02X->%02X ",off,prev[i],cur);
            prev[i]=cur;
            ++changes;
        }
        if (changes && now-lastMs.load()>250)
        {
            lastMs.store(now);
            LOG("HUD-PROBE %d byte(s) changed: %s",changes,line);
        }
    }

    thread_local bool g_insideSpecialCompose=false;

    // Place the assembly by writing the root BEFORE the engine composes.
    // Post-editing the composed output is retired: it is downstream of the
    // engine's own weapon-lag pass (0x2C484B), which rotates every bone except
    // camera_control and so overwrote the mesh while leaving the muzzle flash
    // on our pose — exactly the reported split. `weapon_probe` still drives the
    // old output path, and only that, as a diagnostic.
    // THE MESH FIX — a call-site patch, not a detour (2026-07-15 ~04:00).
    // Proven chain, all read offline: the visible first-person mesh is built by
    // the object-node recomposer at halo3+0x341768 (single caller 0x3424DD),
    // which dequantizes the object's compressed animation and roots the chain
    // with `call 0x3453DC` at +0x341A5B — a generic object-root getter with 56
    // callers that fabricates {MakeTransformFromXZ(fwd@+0x5C, up@+0x68),
    // pos@+0x50} from the object datum. The FP arms/weapon objects sit exactly
    // at the camera every frame; that collocation IS the head-glue, and it is
    // also how the shim identifies them without knowing their handles.
    //
    // We patch the 4 aligned displacement bytes of that ONE call (atomic
    // InterlockedExchange, installed at DLL load before any level runs) to a
    // 12-byte trampoline that reaches FpRootShim. The shim calls the REAL
    // getter, and only if the returned root is camera-collocated replaces it
    // with the controller's world pose — a write into a STACK buffer the
    // renderer consumes immediately. No engine function is detoured, none of
    // the other 55 callers are affected, and no simulation state is touched.
    // Failure mode if MCC updates: signature miss -> log + gun stays glued.
    using FpRootFn = BoneMatrix*(__fastcall*)(uint16_t index, BoneMatrix* out);
    FpRootFn g_realFpRoot = nullptr;


    // THE HEAD-GLUE, finally acted on safely: right after composition, the FP
    // evaluator loops every bone EXCEPT camera_control (cmp r9d,[rdi+0x11A4])
    // and rotates it via `call 0x120DF8` at halo3+0x2C485B with a camera-
    // pitch/turn matrix. That is the exact flash-vs-mesh partition observed in
    // every headset test tonight. Detouring 0x120DF8 crashes on level load
    // (proven, banned); patching THIS ONE CALL SITE affects no other caller —
    // the same aligned-disp32 technique that survived a full session at
    // 0x341A5B. The shim skips the rotation only for bone addresses inside an
    // assembly we re-rooted this frame (thread_local ranges, same thread that
    // composes), and forwards everything else to the real function untouched.
    using SwayApplyFn = void(__fastcall*)(void* sway, BoneMatrix* bone);
    SwayApplyFn g_realSwayApply = nullptr;

    // CRASH LESSON (both fatal errors tonight, same root cause): halo3.dll is
    // LTCG-optimized — the sway loop keeps its counter (r9d), bone pointer
    // (r8) and count (r10d) LIVE IN VOLATILE REGISTERS across `call 0x120DF8`,
    // because the compiler knows that function never touches them. ANY
    // compiled C/C++ interposition (a MinHook detour or a C++ shim) clobbers
    // those registers and corrupts the caller -> wild writes -> fatal error on
    // level load. Interposing engine-internal calls therefore requires a
    // hand-assembled shim restricted to registers the caller provably treats
    // as dead — here, only RAX (verified: reloaded/unused after the call).
    //
    // The emitted shim (see InstallHook) compares rdx (the bone) against
    // these bounds and returns without rotating when it lies inside an
    // assembly ApplyControllerToRoot re-rooted this frame; everything else
    // tail-jumps to the real rotator. Same game thread writes and reads the
    // bounds (compose -> sway loop), so there is no race.
    alignas(8) volatile uintptr_t g_fpSkipBounds[4] = {0, 0, 0, 0}; // lo0,hi0,lo1,hi1

    bool ControllerWorldPoseEx(bool left, float basis[9], float pos[3], float& scale)
    {
        if (!g_vrAim.load() || !g_enabled.load() || !g_camValid.load() ||
            !g_baseCamValid.load()) return false;
        float cq[4], cp[3];
        // Right/weapon hand uses the shared aim pose (two-hand-adjusted); the
        // left hand uses its own controller for the support-arm IK target.
        if (left ? !VR_GetLeftControllerPose(cq, cp)
                 : !VR_GetAimPose(cq, cp)) return false;
        BuildTrackedGameBasis(cq, false, basis); // controller basis, game world axes
        // Full controller displacement from the recentered room origin. The
        // old camera + (controller-currentHead) expression combined a camera
        // position containing an older head-lean sample with a fresh relative
        // hand sample. That subtraction shortened forward reach and leaked
        // head motion. The gameplay base below was captured before head lean.
        const float dx=cp[0]-g_headPosRef[0];
        const float dy=cp[1]-g_headPosRef[1];
        const float dz=cp[2]-g_headPosRef[2];
        const float sh=sinf(g_headYawRef), ch=cosf(g_headYawRef);
        const float roomForward=dx*sh-dz*ch;
        const float roomRight=dx*ch+dz*sh;
        const float cg=cosf(g_gameYawRef), sg=sinf(g_gameYawRef);
        const float s = g_worldScale.load();
        const float off[3] = {
            (cg*roomForward+sg*roomRight)*s,
            (sg*roomForward-cg*roomRight)*s,
            dy*s};
        const float cam[3] = {g_baseCamX.load(),g_baseCamY.load(),g_baseCamZ.load()};
        // Forward standoff along the controller's own aim direction (basis
        // column 0 = forward). EVERY left-hand use — support hand AND the
        // dual-wield gun seat — is the same wrist-to-palm PALM point (23:17
        // headset result: seating the dual gun by the weapon depth put it on
        // the wrist, ~12 cm behind the rendered hand). Right keeps its
        // independent weapon offset.
        const float standoff = (left
            ? Clamp(g_config.left_hand_forward_m, -0.15f, 0.30f)
            : Clamp(g_config.gun_forward_m, -0.3f, 0.5f)) * s;
        for (int j = 0; j < 3; ++j)
            pos[j] = cam[j] + off[j] + basis[j] * standoff;
        scale = Clamp(g_config.gun_scale, 0.3f, 3.0f);
        for (int j = 0; j < 9; ++j) if (!isfinite(basis[j])) return false;
        for (int j = 0; j < 3; ++j) if (!isfinite(pos[j])) return false;
        return true;
    }

    // Legacy name: the right-hand pose (existing call sites).
    bool ControllerWorldPose(float basis[9], float pos[3], float& scale)
    {
        return ControllerWorldPoseEx(false, basis, pos, scale);
    }

    // Repurposed 2026-07-19 as the VRIK Stage A2 probe. This call-site patch
    // sits inside the OBJECT-node recomposer (0x341768) — the pipeline that
    // animates every visible biped, including the player's own natively
    // visible legs. With the Bone-probe checkbox on, every recomposed object's
    // root is pushed 0.3 wu left. Legs/NPCs visibly shifting = this boundary
    // reaches biped pixels = Stage B (arm IK) has a real write path.
    // The old camera-collocated re-anchor is retired (the FP camera anchor in
    // RenderViewHook owns gun placement now).
    // Retired to a clean passthrough (2026-07-19). This object recomposer's
    // root output was proven NOT to drive the visible body (the +0.6 lift test
    // moved nothing on screen), and the FP camera anchor owns gun placement.
    // Kept only so the call-site patch remains a stable, harmless no-op rather
    // than reintroducing an unpatch path. See docs/VRIK-ROADMAP.md.
    BoneMatrix* __fastcall FpRootShim(uint16_t index, BoneMatrix* out)
    {
        return g_realFpRoot(index, out);
    }

    // The 03:27 lever, restored: write the composers' `defaults` root. Proven
    // side-effect-free in the headset and it puts the muzzle flash/markers on
    // the controller. Does NOT move the mesh (that consumer is still unfound).
    bool ApplyControllerToRoot(BoneMatrix* output, BoneMatrix* root)
    {
        if (!root || !output) return false;
        int slot=-1; unsigned char* weapon=nullptr;
        if (!FindFirstPersonWeapon(output,slot,weapon)) return false;
        float target[3],desired[9];
        if (!GetControllerFirstPersonTransform(slot,target,desired)) return false;
        for(float v : target) if (!isfinite(v)) return false;
        for(float v : desired) if (!isfinite(v)) return false;
        const float meshScale=Clamp(g_config.gun_scale,0.3f,3.0f);
        if (!isfinite(meshScale) || meshScale<=0.0f) return false;
        root->scale=meshScale;
        memcpy(root->rotation,desired,sizeof(desired));
        memcpy(root->translation,target,sizeof(target));
        // This assembly now carries the controller pose; exempt exactly these
        // bones from the engine's camera pitch/turn rotation (emitted shim).
        const int count=*reinterpret_cast<int*>(weapon+0x49C);
        if (count>0 && count<=64)
        {
            g_fpSkipBounds[slot*2+0]=reinterpret_cast<uintptr_t>(output);
            g_fpSkipBounds[slot*2+1]=g_fpSkipBounds[slot*2+0]+static_cast<size_t>(count)*sizeof(BoneMatrix);
        }
        static std::atomic<unsigned> logged{0};
        const unsigned bit=1u<<slot;
        if (!(logged.fetch_or(bit)&bit))
            LOG("M3: first-person slot %d rooted to the %s controller (markers/flash lever, scale %.2f)",
                slot,slot==0?"right":"left",meshScale);
        return true;
    }

    // CENSUS RESULT (2026-07-19, retired): the two composer hooks we install
    // process ONLY first-person weapon/arm skeletons (42-45 bones, camera-space
    // at origin) — never world bipeds. The biped skeleton lives in the render
    // pool at RVA ~0x468xxxx (found live via camscan; see docs/VRIK-ROADMAP.md).
    // The census + biped probe that proved this are removed.

    // BANK WRITES ARE BANNED (2026-07-15, 03:4x headset result): writing the
    // controller pose into bank record 0 did NOT move the mesh, but it DID
    // bleed the wrist into the body/camera — record 0 propagates into
    // camera_control, which the game reads back to drive the camera. The
    // ApplyControllerToBankRoot helper is intentionally no longer called;
    // kept only as documentation of the falsified lever.
    // BULLET-SPAWN FIX (2026-07-19): the projectile spawn / effect origins
    // read the COMPOSED output, which stayed AUTHORED (head-glued) once the
    // bank write went dormant — bullets emerged at the authored center-screen
    // muzzle, "slightly left and ahead of the gun". Composed output is WORLD
    // space (output[0] = defaultsRoot * record0), so snap ONLY the right-wrist
    // subtree onto the shared controller wrist target. Everything else —
    // especially record 0 and camera_control — is left untouched: rewriting
    // those is the falsified "wrist moves the world" camera feedback.
    bool ApplyControllerToComposedWristSubtree(BoneMatrix* output)
    {
        int slot=-1; unsigned char* weapon=nullptr;
        if (!FindFirstPersonWeapon(output,slot,weapon)) return false;
        const int count=g_fpBoneCount[slot].load(std::memory_order_acquire);
        const int wrist=g_fpWristIndex[slot].load(std::memory_order_acquire);
        const int lWrist=g_fpLWristIndex[slot].load(std::memory_order_acquire);
        const uint64_t mask=g_fpWristDescendants[slot].load(std::memory_order_acquire);
        if (count<=0 || count>64 || wrist<0 || wrist>=count ||
            *reinterpret_cast<int*>(weapon+0x49C)!=count) return false;
        float meshScale=1.0f;
        BoneMatrix desired{},invWrist{},t{};
        // Slot 1 (dual-wield secondary): effect origins belong on the LEFT
        // controller, matching its visible weapon — anchored on the carrier
        // hand bone exactly like the visible seat.
        const bool dual=(slot==1);
        const int transformAnchor=dual?lWrist:wrist;
        if (transformAnchor<0 || transformAnchor>=count) return false;
        if (!DesiredWristWorld(dual,desired,meshScale)) return false;
        if (!InvertBoneMatrix(output[transformAnchor],invWrist) ||
            !ComposeBoneMatrices(desired,invWrist,t)) return false;
        for (int i=0;i<count;++i)
        {
            if (!(mask&(1ull<<i))) continue;
            BoneMatrix moved{};
            if (!ComposeBoneMatrices(t,output[i],moved)) return false;
            output[i]=moved;
        }
        if (meshScale!=1.0f)
        {
            const float* a=desired.translation;
            for (int i=0;i<count;++i)
            {
                if (!(mask&(1ull<<i))) continue;
                for (int r=0;r<3;++r)
                    output[i].translation[r]=a[r]+
                        (output[i].translation[r]-a[r])*meshScale;
                output[i].scale*=meshScale;
            }
        }
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            LOG("M3: composed wrist subtree snapped to the controller "
                "(bullet spawn / effect origins now on the gun)");
        return true;
    }

    void __fastcall ComposeBonesHook(void* model, int start, int count, BoneMatrix* output,
                                     void* source, void* defaults)
    {
        if (!g_insideSpecialCompose)
        {
            g_fpSkipBounds[0]=g_fpSkipBounds[1]=g_fpSkipBounds[2]=g_fpSkipBounds[3]=0;
            ApplyControllerToBankChild(model,output,reinterpret_cast<float*>(source));
        }
        g_origComposeBones(model,start,count,output,source,defaults);
        // This is the last full authored pose before Halo's caller applies the
        // camera-lag transform (camera_control alone is exempt). Preserve that
        // relation for exact render-side lag removal.
        CacheAuthoredFirstPersonAlignment(output,start,count);
        // NOTE (2026-07-18): wiring ApplyControllerToComposedWristSubtree here
        // (the "bullet_snap" experiment) caused the RIGHT HAND to spin
        // uncontrollably and pushed bullets to stage-left — the composed output
        // is downstream of the engine weapon-lag pass and re-snapping the wrist
        // fights it. Reverted to the known-good M3 gun tracking. Bullet origin
        // will be fixed via a weapon-fire hook (origin+direction swap) instead,
        // not by editing composed bones. The call is intentionally gone.
        // 04:17 falsified the output rewrite as a mesh lever for good (43-bone
        // rewrite confirmed running; mesh unmoved; "wrist moves the world" =
        // camera_control feedback). Probe-only again. The defaults-root write
        // is retired for the same reason: both only fed markers + the camera.
        if (!g_insideSpecialCompose && g_config.weapon_probe)
            ApplyControllerToComposedBones(model,output);
    }
    void __fastcall ComposeSpecialBonesHook(void* model, BoneMatrix* output, void* source,
                                            void* defaults, int firstSpecial, int secondSpecial)
    {
        g_fpSkipBounds[0]=g_fpSkipBounds[1]=g_fpSkipBounds[2]=g_fpSkipBounds[3]=0;
        ApplyControllerToBankChild(model,output,reinterpret_cast<float*>(source));
        g_insideSpecialCompose=true;
        g_origComposeSpecialBones(model,output,source,defaults,firstSpecial,secondSpecial);
        g_insideSpecialCompose=false;
        int slot=-1; unsigned char* weapon=nullptr;
        if (FindFirstPersonWeapon(output,slot,weapon))
            CacheAuthoredFirstPersonAlignment(
                output,0,*reinterpret_cast<int*>(weapon+0x49C));
        // (bullet_snap reverted here too — see ComposeBonesHook note.)
        if (g_config.weapon_probe) ApplyControllerToComposedBones(model,output);
    }

    float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    float WrapPi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

    // Rodrigues rotation of v (in place) about the unit axis by the angle
    // whose cosine/sine are given.
    void RotateAboutAxis(float* v, const float* axis, float cosA, float sinA)
    {
        const float d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
        const float c[3] = {axis[1] * v[2] - axis[2] * v[1],
                            axis[2] * v[0] - axis[0] * v[2],
                            axis[0] * v[1] - axis[1] * v[0]};
        for (int i = 0; i < 3; ++i)
            v[i] = v[i] * cosA + c[i] * sinA + axis[i] * d * (1.0f - cosA);
    }

    bool ReadCinematicShot(int32_t& scene, int32_t& shot)
    {
        if (!g_cinematicTlsIndex)
            return false;
        __try
        {
            auto** slots = reinterpret_cast<void**>(__readgsqword(0x58));
            if (!slots)
                return false;
            auto* tls = reinterpret_cast<unsigned char*>(
                slots[*g_cinematicTlsIndex]);
            if (!tls)
                return false;
            auto* globals = *reinterpret_cast<unsigned char**>(tls + 0xA8);
            if (!globals || globals[5] == 0)
                return false;
            auto* shotState = *reinterpret_cast<unsigned char**>(
                tls + g_cinematicShotStateOffset);
            if (!shotState)
                return false;
            scene = *reinterpret_cast<const int32_t*>(shotState + 4);
            shot = *reinterpret_cast<const int32_t*>(shotState + 8);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void ApplyHeadLook(void* src)
    {
        if (!src)
            return;

        float q[4], hpos[3];
        if (!VR_GetHeadPose(q, hpos))
            return;
        // Head forward (OpenXR: -Z forward, +Y up).
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float hfx = -2.0f * (w * y + x * z);
        const float hfy =  2.0f * (w * x - y * z);
        const float hfz = -(1.0f - 2.0f * (x * x + y * y));
        const float hy = atan2f(hfx, -hfz);
        const float hp = asinf(Clamp(hfy, -1.0f, 1.0f));

        // Head roll around the forward axis. Compare the headset's actual up
        // vector with a horizon-level up vector at the same yaw/pitch. M1 used
        // only the latter, so rolling your head made the world appear to tilt
        // with you instead of remaining fixed in space.
        const float hux = 2.0f * (x * y - w * z);
        const float huy = 1.0f - 2.0f * (x * x + z * z);
        const float huz = 2.0f * (y * z + w * x);
        float hrx = -hfz, hrz = hfx; // horizon-right = cross(head forward, room up)
        float hrLen = sqrtf(hrx * hrx + hrz * hrz);
        if (hrLen < 1e-4f) hrLen = 1e-4f;
        hrx /= hrLen; hrz /= hrLen;
        const float hnux = -hfy * hrz;
        const float hnuy = hrLen;
        const float hnuz = hfy * hrx;
        const float headRoll = atan2f(hux * hrx + huz * hrz,
                                      hux * hnux + huy * hnuy + huz * hnuz);

        float* fwd = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcFwd);
        float* up = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcUp);
        float* pos = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcPos);

        // A cutscene shot changes Halo's authored camera, but the old VR yaw
        // reference otherwise survives the cut. That can spawn the viewer
        // facing backward in the new shot. The engine's exact scene/shot IDs
        // are read from its TLS state; rebase the current physical head-forward
        // to the authored camera only on entry, an ID transition, or exit.
        // Pitch and roll remain entirely HMD-owned, avoiding an artificial
        // camera rotation during continuous cinematic motion.
        static thread_local bool previousCinematic = false;
        static thread_local int32_t previousScene = -1;
        static thread_local int32_t previousShot = -1;
        int32_t cinematicScene = -1;
        int32_t cinematicShot = -1;
        const bool cinematic =
            ReadCinematicShot(cinematicScene, cinematicShot);
        const bool cinematicBoundary =
            (cinematic && (!previousCinematic ||
                cinematicScene != previousScene ||
                cinematicShot != previousShot)) ||
            (!cinematic && previousCinematic);
        previousCinematic = cinematic;
        previousScene = cinematic ? cinematicScene : -1;
        previousShot = cinematic ? cinematicShot : -1;

        const bool manualRecenter = g_needRecenter.exchange(false);
        if (manualRecenter || cinematicBoundary)
        {
            g_gameYawRef = atan2f(fwd[1], fwd[0]); // align current head to current heading
            g_headYawRef = hy;
            if (manualRecenter)
            {
                g_headPosRef[0] = hpos[0]; g_headPosRef[1] = hpos[1];
                g_headPosRef[2] = hpos[2];
                g_needPosRecenter = false;
                LOG("head tracking recentered (game yaw %.1f deg)",
                    g_gameYawRef * 57.2958f);
            }
            if (cinematicBoundary)
            {
                g_cinematicRebaseScene.store(
                    cinematic ? cinematicScene : -1,
                    std::memory_order_relaxed);
                g_cinematicRebaseShot.store(
                    cinematic ? cinematicShot : -1,
                    std::memory_order_relaxed);
                g_cinematicRebaseSerial.fetch_add(
                    1, std::memory_order_release);
            }
        }
        else if (g_needPosRecenter.exchange(false))
        {
            // Enabling leaning: capture the neutral head position only, so the
            // aim/yaw baseline is left untouched (no view snap).
            g_headPosRef[0] = hpos[0]; g_headPosRef[1] = hpos[1]; g_headPosRef[2] = hpos[2];
        }

        // Rotation: yaw relative + recenter, pitch absolute + trim.
        const float gy = g_gameYawRef + g_yawSign.load() * WrapPi(hy - g_headYawRef);
        const float gp = Clamp(g_pitchSign.load() * hp + g_pitchTrim.load(), -1.5f, 1.5f);
        const float cgp = cosf(gp), sgp = sinf(gp), cgy = cosf(gy), sgy = sinf(gy);

        fwd[0] = cgp * cgy; fwd[1] = cgp * sgy; fwd[2] = sgp;
        if (g_writeUp.load())
        {
            const float cr = cosf(headRoll), sr = sinf(headRoll);
            // Horizon-level up plus roll toward the camera's right vector.
            up[0] = (-sgp * cgy) * cr + sgy * sr;
            up[1] = (-sgp * sgy) * cr - cgy * sr;
            up[2] = cgp * cr;
        }

        // Position (leaning): shift the camera by the headset's room-space move,
        // decomposed in the head's horizontal frame and re-applied in the game's
        // frame so it stays correct as you turn. Added to the game's own
        // position each frame (the sim rewrites pos before our hook, so this
        // does not accumulate).
        if (g_positional.load())
        {
            const float dx = hpos[0] - g_headPosRef[0];
            const float dy = hpos[1] - g_headPosRef[1];
            const float dz = hpos[2] - g_headPosRef[2];
            float hlen = sqrtf(hfx * hfx + hfz * hfz);
            if (hlen < 1e-4f) hlen = 1e-4f;
            const float hfhx = hfx / hlen, hfhz = hfz / hlen; // head forward (horizontal)
            const float fwdComp = dx * hfhx + dz * hfhz;       // room move along look dir
            const float rightComp = dx * (-hfhz) + dz * hfhx;  // room move to the right
            const float s = g_worldScale.load();
            float ox = (cgy * fwdComp + sgy * rightComp) * s;  // game forward/right at gy
            float oy = (sgy * fwdComp - cgy * rightComp) * s;
            float oz = dy * s;
            ox = Clamp(ox, -1.5f, 1.5f); oy = Clamp(oy, -1.5f, 1.5f); oz = Clamp(oz, -1.5f, 1.5f);
            pos[0] += ox; pos[1] += oy; pos[2] += oz;
        }

        // M2 alternate-eye proof: offset only the render camera by half the
        // measured PSVR2 IPD. Halo right in the horizontal plane is
        // (sin(yaw), -cos(yaw), 0). This does not accumulate because the game
        // rewrites the source camera before every call.
        // Per-eye separation is applied later by RenderViewHook to the compact
        // render-only camera. Keeping it out of the authoritative source avoids
        // feeding stereo offsets back into simulation or temporal history.
    }

    // M3: snap/smooth turning from the right Sense stick. Rotating the yaw
    // reference turns the head-locked view instantly, and the hand-steered aim
    // follows because its target is expressed relative to the same reference.
    void ApplyVrTurn(const VrPadState& pad)
    {
        if (!g_vrAim.load())
            return;
        if (!pad.valid)
            return;
        // Smooth turn needs a sub-frame timebase. GetTickCount only updates on
        // the ~15.6 ms system tick, but this runs several times per 11 ms (90 Hz)
        // frame from CamCopyHook, so a GetTickCount delta was 0 on most frames
        // and ~15 ms in a lump on others -> a visible ~5 Hz yaw stutter. The
        // performance counter gives the true elapsed time between calls, so the
        // yaw advances evenly regardless of how many calls land in a frame.
        static LARGE_INTEGER freq{}, last{};
        if (freq.QuadPart == 0)
            QueryPerformanceFrequency(&freq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = last.QuadPart == 0 ? 0.0f
                       : (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        const float x = pad.turnX; // stick right = turn right = yaw decreases
        if (g_config.turn_smooth)
        {
            if (fabsf(x) > 0.15f)
                g_gameYawRef = WrapPi(g_gameYawRef -
                    x * (g_config.turn_smooth_deg_s / 57.2958f) * dt);
        }
        else
        {
            static bool latched = false; // one snap per stick flick
            if (!latched && fabsf(x) > 0.6f)
            {
                g_gameYawRef = WrapPi(g_gameYawRef -
                    (x > 0 ? 1.0f : -1.0f) * g_config.turn_snap_deg / 57.2958f);
                latched = true;
            }
            else if (fabsf(x) < 0.3f)
                latched = false;
        }
    }

    void ApplyVrTurn()
    {
        VrPadState pad;
        VR_GetPadState(pad);
        ApplyVrTurn(pad);
    }

    void* __fastcall CamCopyHook(void* dst, void* src)
    {
        VR_NotifyCameraTransform();
        const uint64_t cameraNowMs = GetTickCount64();
        const uint32_t runtimeGeneration =
            g_halo3RuntimeGeneration.load(std::memory_order_acquire);
        if (runtimeGeneration)
        {
            g_halo3LastCamCopyMs.store(
                cameraNowMs, std::memory_order_relaxed);
            TitleAdapter_PublishHeartbeat(
                GameTitle::Halo3, runtimeGeneration, cameraNowMs);
        }
        // Low-frequency timing proof paired with vr.cpp's HMD sample-rate log.
        // The hook normally runs multiple times per presented frame, and every
        // call below reads the latest once-per-OpenXR-frame predicted pose.
        static uint64_t cameraRateStartMs = 0;
        static unsigned cameraTransforms = 0;
        ++cameraTransforms;
        if (!cameraRateStartMs) cameraRateStartMs = cameraNowMs;
        else if (cameraNowMs - cameraRateStartMs >= 10000)
        {
            g_perfCameraRateLogWrites.fetch_add(1, std::memory_order_relaxed);
            LOG("M1 timing: camera transforms %.1f/sec (latest predicted HMD pose consumed each call)",
                cameraTransforms * 1000.0 / (cameraNowMs - cameraRateStartMs));
            cameraTransforms = 0;
            cameraRateStartMs = cameraNowMs;
        }
        ChudProbe();
        ApplyMotionBlurSetting();
        ProbeBulletOrigin();
        ApplyBodySetting();
        // M2 tracing: this function copies src+0x68/+0x6C into the compact
        // render camera at dst+0x28/+0x2C. Record only the first few calls so
        // we can distinguish world, weapon, and other camera passes without
        // producing a frame-sized log forever.
        static std::atomic<unsigned> traceCount{0};
        if (src)
        {
            const float srcTanX=*reinterpret_cast<const float*>(
                reinterpret_cast<const char*>(src) + kSrcProjX);
            g_projectionTanX.store(srcTanX);
            g_projectionTanY.store(*reinterpret_cast<const float*>(
                reinterpret_cast<const char*>(src) + kSrcProjY));
            // ZOOM DETECTION (for the weapon scope): Halo narrows the camera's
            // projection tangent when the player zooms. Track the widest
            // (unzoomed) tangent as the baseline; a meaningfully smaller current
            // tangent = zoomed, with magnification = baseline/current. Logged so
            // the scope render can be built on a confirmed signal.
            if (srcTanX>0.2f && srcTanX<3.0f)
            {
                static float baseTan=0.0f;
                if (srcTanX>baseTan) baseTan=srcTanX; // widest seen = hip FOV
                const float factor = (baseTan>1e-3f)? baseTan/srcTanX : 1.0f;
                const bool zoomed = factor>1.15f;
                g_zoomFactor.store(zoomed?factor:1.0f);
                static bool wasZoomed=false;
                if (zoomed!=wasZoomed)
                {
                    wasZoomed=zoomed;
                    g_perfZoomLogWrites.fetch_add(1, std::memory_order_relaxed);
                    LOG("M3 ZOOM: %s (tan %.3f vs base %.3f => %.2fx)",
                        zoomed?"zoomed IN":"unzoomed",srcTanX,baseTan,factor);
                }
            }
            // Camera buffers are heap-allocated and move on level changes;
            // log every new one so external tools (camscan aimwrite) can
            // always read the current address from the log.
            static void* seenSrc[16]{};
            static unsigned seenSrcCount = 0;
            bool newSrc = true;
            for (unsigned i = 0; i < seenSrcCount; ++i)
                if (seenSrc[i] == src) { newSrc = false; break; }
            if (newSrc && seenSrcCount < 16)
                seenSrc[seenSrcCount++] = src;
            const unsigned trace = traceCount.fetch_add(1);
            if (trace < 24 || newSrc)
            {
                const float* pos = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcPos);
                const float projX = *reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcProjX);
                const float projY = *reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcProjY);
                LOG("M2 camera copy %u: dst=%p src=%p pos=(%.2f,%.2f,%.2f) proj=(%.6f,%.6f)",
                    trace, dst, src, pos[0], pos[1], pos[2], projX, projY);
            }
        }
        if (src)
        {
            // The game recomputes this forward from its aim state every frame,
            // so pre-overwrite it equals the true aim direction (bullets follow
            // it even while head look repaints the view).
            const float* fwd = reinterpret_cast<const float*>(
                reinterpret_cast<const char*>(src) + kSrcFwd);
            g_aimFwdX.store(fwd[0]); g_aimFwdY.store(fwd[1]); g_aimFwdZ.store(fwd[2]);
            g_aimSeen = true;
        }
        if (g_enabled.load())
        {
            ApplyVrTurn();
            if (src)
            {
                // Snapshot the engine-owned locomotion/body origin before
                // ApplyHeadLook adds this frame's headset lean. The visible
                // weapon reads this origin but never writes it.
                const float* p=reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src)+kSrcPos);
                g_baseCamX.store(p[0]); g_baseCamY.store(p[1]); g_baseCamZ.store(p[2]);
                g_baseCamValid.store(true,std::memory_order_release);
            }
            // The head pose LIVES in this authoritative camera (the proven M3
            // regime). A 07-15 experiment saved/restored the original values
            // around the copy so gameplay would keep the aim pose — but the
            // first-person bone frame is head-camera-relative, and splitting
            // the two frames made the hand-anchored weapon visibly pick up
            // both head and aim motion. Do not scope this write again.
            ApplyHeadLook(src);
            if (src)
            {
                // Post-head-look camera position (includes leaning): the
                // reference FpRootShim uses to recognize camera-glued FP
                // objects and to place the controller in world space.
                const float* p = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcPos);
                g_camX.store(p[0]); g_camY.store(p[1]); g_camZ.store(p[2]);
                const float* f = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcFwd);
                const float* u = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcUp);
                for (int j = 0; j < 3; ++j)
                { g_camFwd[j].store(f[j]); g_camUp[j].store(u[j]); }
                g_camValid.store(true);

                // Measure the world-up axis from the engine's camera-up (see the
                // g_worldUp declaration). Bootstrap from the first sample, then EMA
                // slowly toward each tick's up — but ONLY while looking roughly
                // level (camera-fwd within ~25 deg of horizontal), so staring up or
                // down a slope for a while can't drag the estimate off vertical.
                {
                    if (!g_worldUpInit.load())
                    {
                        const float l=sqrtf(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]);
                        if (l>1e-4f)
                            for (int j=0;j<3;++j) g_worldUp[j].store(u[j]/l);
                        g_worldUpInit.store(true);
                    }
                    else
                    {
                        float wu[3]={g_worldUp[0].load(),g_worldUp[1].load(),
                                     g_worldUp[2].load()};
                        const float lookLevel=fabsf(f[0]*wu[0]+f[1]*wu[1]+f[2]*wu[2]);
                        if (lookLevel<0.42f)   // forward within ~25 deg of horizontal
                        {
                            const float a=0.01f;
                            for (int j=0;j<3;++j) wu[j]+=(u[j]-wu[j])*a;
                            const float l=sqrtf(wu[0]*wu[0]+wu[1]*wu[1]+wu[2]*wu[2]);
                            if (l>1e-4f)
                                for (int j=0;j<3;++j) g_worldUp[j].store(wu[j]/l);
                        }
                    }
                    static std::atomic<int> wuFrames{0};
                    const int n=wuFrames.fetch_add(1);
                    if (n==600)
                        LOG("M3: measured world-up = (%.3f, %.3f, %.3f)",
                            g_worldUp[0].load(),g_worldUp[1].load(),g_worldUp[2].load());
                }
            }
        }
        else
        {
            g_camValid.store(false);
            g_baseCamValid.store(false);
        }

        return g_origCamCopy(dst, src);
    }

    void __fastcall ObserverCameraEffectHook(int userIndex)
    {
        // Halo applies weapon recoil, explosions, and other authored camera
        // impulses after it computes the observer's stable camera result. A
        // monitor can move the view independently of the player; an HMD must
        // never do that. Keep the stock effects when VR head tracking is off,
        // but make the tracked headset the sole owner of the VR view pose.
        if (!g_enabled.load(std::memory_order_relaxed))
            g_origObserverCameraEffect(userIndex);
    }

    bool BuildRightHandScopeCamera(char* camera, const unsigned char* saved)
    {
        float controllerBasis[9], weaponSeat[3], meshScale=1.0f;
        if(!ControllerWorldPoseEx(false,controllerBasis,weaponSeat,meshScale))
            return false;
        const float* cameraOrigin=reinterpret_cast<const float*>(saved);
        const float bulletForward[3]={
            g_aimFwdX.load(),g_aimFwdY.load(),g_aimFwdZ.load()};
        ScopeCameraPose pose{};
        if(!ComputeScopeCameraPose(controllerBasis,cameraOrigin,
                                   bulletForward,pose))
            return false;
        memcpy(camera,saved,0x90);
        memcpy(camera+0x00,pose.position,sizeof(pose.position));
        memcpy(camera+0x0C,pose.forward,sizeof(pose.forward));
        memcpy(camera+0x18,pose.up,sizeof(pose.up));
        return true;
    }

    void __fastcall RenderViewHook(void* view)
    {
        if (!VR_IsStereoEnabled() || !g_enabled.load() || !view)
        {
            g_origRenderView(view);
            return;
        }

        // Compact camera produced by CamCopy: position +0x00, forward +0x0C,
        // up +0x18. It begins at view+0x08. Snapshot it so both eye calls start
        // from identical frame state and the engine sees its original afterward.
        char* camera = reinterpret_cast<char*>(view) + 8;
        alignas(16) unsigned char saved[0x90];
        alignas(16) unsigned char savedDerived[0x90];
        alignas(16) unsigned char savedCameraCopy[0x90];
        alignas(16) unsigned char savedDerivedCopy[0x90];
        memcpy(saved, camera, sizeof(saved));
        memcpy(savedDerived, reinterpret_cast<char*>(view) + 0x98, sizeof(savedDerived));
        memcpy(savedCameraCopy, reinterpret_cast<char*>(view) + 0x158, sizeof(savedCameraCopy));
        memcpy(savedDerivedCopy, reinterpret_cast<char*>(view) + 0x1E8, sizeof(savedDerivedCopy));
        const float* fwd = reinterpret_cast<const float*>(saved + 0x0C);
        const float* up = reinterpret_cast<const float*>(saved + 0x18);
        float right[3] = {
            fwd[1] * up[2] - fwd[2] * up[1],
            fwd[2] * up[0] - fwd[0] * up[2],
            fwd[0] * up[1] - fwd[1] * up[0]};
        // STEREO GHOSTING — root cause finally OBSERVED (2026-07-14, the
        // CopyResource probe): between the eye passes, the engine snapshots
        // the full-resolution scene into a sampleable texture
        // (M2 COPY eye=-1, 2912x2100 fmt29 -> fmt29) — its "last frame"
        // source for temporal effects. In stereo that snapshot is made from
        // whichever eye rendered LAST, and BOTH eyes sample it next frame:
        // the last eye reads itself (clean), the first eye reads the other
        // eye (trailing after-images offset by the eye separation). This
        // explains every earlier result: fixed order -> steady first-eye
        // ghost; alternation -> flicker; a discarded warm-up render -> clean
        // (it flushed the foreign snapshot through the effect chain) at the
        // cost of a third render (60 fps).
        //
        // The fix (vr.cpp, VR_Begin/EndRasterEye + the CopyResource hook):
        // keep a per-eye copy of that snapshot — captured after each eye's
        // own render, substituted into the game's snapshot texture right
        // before that eye renders again. Each eye then always samples its own
        // previous frame. Three texture copies per frame instead of a third
        // world render, so this runs at the full two-render rate.
        {
            static std::atomic<unsigned> viewRenders{0};
            static std::atomic<DWORD> lastLog{GetTickCount()};
            g_perfViewRenders.fetch_add(1, std::memory_order_relaxed);
            viewRenders.fetch_add(1);
            const DWORD now = GetTickCount();
            DWORD last = lastLog.load();
            if (now - last >= 10000 && lastLog.compare_exchange_strong(last, now))
            {
                const unsigned n = viewRenders.exchange(0);
                g_perfViewRateLogWrites.fetch_add(1, std::memory_order_relaxed);
                LOG("M2: view renders %.0f/sec (equals fps => one per frame; "
                    "a multiple => extra engine views)", n * 1000.0 / (now - last));
            }
        }
        // Cache visible FP palette solves only within this one stereo pair.
        // Exact input matching in ReconstructVisiblePaletteSource keeps any
        // changed animation pass on the existing full-solve path.
        g_fpStereoSolveScope = {};
        g_fpStereoSolveScope.armed = true;
        const int firstEye = g_config.right_eye_first ? 1 : 0;
        for (int pass = 0; pass < 2; ++pass)
        {
            const int eye = pass == 0 ? firstEye : 1 - firstEye;
            g_stereoEye = eye;
            VR_BeginRasterEye(eye);
            memcpy(camera, saved, sizeof(saved));
            float* pos = reinterpret_cast<float*>(camera);
            const float sign = eye == 0 ? -1.0f : 1.0f;
            // Render from the exact eye offsets returned by xrLocateViews.
            // Quest 2's physical lens spacing is adjustable, and OpenXR may
            // also report non-horizontal offsets. A fixed PSVR2 IPD makes the
            // raster disparity disagree with the poses sent to the compositor,
            // which presents as two images that cannot be fused. Keep the old
            // 67.5 mm baseline only as a safe fallback before views are valid.
            float eyePosition[3]={sign*0.5f*0.0675f,0.0f,0.0f};
            float eyeOrientation[4]{};
            const bool haveEyeView =
                VR_GetEyeViewOffset(eye,eyePosition,eyeOrientation);
            const float eyeScale=g_worldScale.load();
            for(int axis=0;axis<3;++axis)
                pos[axis] += (right[axis]*eyePosition[0] +
                              up[axis]*eyePosition[1] -
                              fwd[axis]*eyePosition[2]) * eyeScale;

            // Cant: PSVR2 mounts each display angled outward a few degrees,
            // and the per-eye FOV OpenXR reports is measured around that
            // canted axis. Turn this eye's raster camera by the same relative
            // rotation (OpenXR view axes +X/+Y/+Z=right/up/-forward mapped
            // onto the camera basis) so the raster covers exactly what the
            // compositor displays; rendering both eyes straight ahead leaves
            // the outward lens edge uncovered = black border per eye. The
            // matching per-eye orientation is submitted in vr.cpp. (Assumes
            // the default yaw/pitch mapping; F4/F5 flips would mirror it.)
            if (haveEyeView)
            {
                const float sinHalf = sqrtf(eyeOrientation[0] * eyeOrientation[0] +
                                            eyeOrientation[1] * eyeOrientation[1] +
                                            eyeOrientation[2] * eyeOrientation[2]);
                if (sinHalf > 1e-5f)
                {
                    float angle = 2.0f * atan2f(sinHalf, eyeOrientation[3]);
                    if (angle > 3.14159265f) angle -= 6.2831853f; // shortest arc
                    const float ax = eyeOrientation[0] / sinHalf;
                    const float ay = eyeOrientation[1] / sinHalf;
                    const float az = eyeOrientation[2] / sinHalf;
                    const float axis[3] = {
                        ax * right[0] + ay * up[0] - az * fwd[0],
                        ax * right[1] + ay * up[1] - az * fwd[1],
                        ax * right[2] + ay * up[2] - az * fwd[2]};
                    const float cosA = cosf(angle), sinA = sinf(angle);
                    RotateAboutAxis(reinterpret_cast<float*>(camera + 0x0C), axis, cosA, sinA);
                    RotateAboutAxis(reinterpret_cast<float*>(camera + 0x18), axis, cosA, sinA);
                }
            }

            // Rebuild exactly the same derived blocks as the engine's camera
            // setup function. Without this, changing the compact camera here
            // is too late and the GPU keeps using the center-eye matrices.
            alignas(16) unsigned char temporary[0x40]{};
            if (g_buildViewport && g_buildMatrices)
            {
                float eyeFov[4];
                float halfX=1.07338f,halfY=0.92502f;
                if(VR_GetEyeFov(eye,eyeFov))
                {
                    halfX=fmaxf(-eyeFov[0],eyeFov[1]);
                    halfY=fmaxf(eyeFov[2],-eyeFov[3]);
                }
                // Native R3 may narrow Halo's compact camera. Keep the headset
                // view and its culling volume at the normal OpenXR FOV; only
                // the separate scope pass consumes the weapon zoom.
                float* cameraTangents=reinterpret_cast<float*>(camera+0x28);
                cameraTangents[0]=tanf(halfX);
                cameraTangents[1]=tanf(halfY);
                g_buildViewport(camera, temporary);
                g_buildMatrices(camera, temporary, reinterpret_cast<char*>(view) + 0x98, 0.0f);
                const float* finalProjection = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(view) + 0x98 + 0x78);
                float* vrProjection = reinterpret_cast<float*>(
                    reinterpret_cast<char*>(view) + 0x98 + 0x78);
                vrProjection[0]=1.0f/tanf(halfX);
                vrProjection[5]=1.0f/tanf(halfY);
                finalProjection = vrProjection;
                if (fabsf(finalProjection[0]) > 0.01f && fabsf(finalProjection[5]) > 0.01f)
                {
                    g_renderHalfFovX = atanf(1.0f / fabsf(finalProjection[0]));
                    g_renderHalfFovY = atanf(1.0f / fabsf(finalProjection[5]));
                }
                // Capture the engine's real matrix layout before introducing
                // off-axis center terms. This is logged once and left
                // untouched so the diagnostic build remains distortion-free.
                static std::atomic<bool> loggedProjection{false};
                if (!loggedProjection.exchange(true))
                {
                    const float* p = reinterpret_cast<const float*>(
                        reinterpret_cast<const char*>(view) + 0x98 + 0x78);
                    LOG("M2 projection rows: [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f]",
                        p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                        p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
                }
                // {view+0x158, view+0x1E8} is a SECOND camera+derived pair on
                // the main view, consumed by the vtable render method 0x28331C
                // via 0x295DC0. It is NOT the first-person pair (that is the
                // sub-view at view+0x6C8: compact +0x6D0, derived +0x8B0 —
                // corrected 2026-07-19). Its exact role is OPEN again; the
                // previous-frame/temporal-reprojection reading from the ghost
                // notes is back on the table. These copies keep it coherent
                // with the current eye camera, as shipped since bd2254e.
                memcpy(reinterpret_cast<char*>(view) + 0x158, camera, 0x90);
                memcpy(reinterpret_cast<char*>(view) + 0x1E8,
                       reinterpret_cast<char*>(view) + 0x98, 0x90);
            }
            // Match the gun/HUD overlay frustum EXACTLY to the widened world
            // raster, otherwise the ~81 deg overlay stretched across the
            // ~123 deg frame magnifies the first-person weapon and HUD ~2x —
            // and any deliberate mismatch would also shift where the
            // hand-anchored weapon projects, breaking controller registration.
            // Written BEFORE the per-view preparation below so the tangents are
            // in place for everything preparation triggers.
            if (const uintptr_t gunCam = g_gunCamera.load())
            {
                float* gunTan = reinterpret_cast<float*>(gunCam + kGunProjX);
                static std::atomic<bool> loggedGunTan{false};
                if (!loggedGunTan.exchange(true))
                    LOG("M2 gun overlay tangents: game (%.4f, %.4f) -> world match (%.4f, %.4f)",
                        gunTan[0], gunTan[1],
                        tanf(g_renderHalfFovX.load()), tanf(g_renderHalfFovY.load()));
                gunTan[0] = tanf(g_renderHalfFovX.load());
                gunTan[1] = tanf(g_renderHalfFovY.load());
                // Experimental HUD sizing: the other three overlay cameras get
                // a scaled frustum (>1 = smaller HUD). Element 0 (the weapon)
                // is never scaled — its projection must match the world for
                // controller registration. Default 1.0 = byte-identical no-op.
                // Elements 1-3 are inactive split-screen player cameras, not
                // independent HUD layers. HUD placement must be solved at the
                // CHUD draw itself; never move this shared camera to the hand.
            }
            // THE DEPTH FIX (2026-07-19). The gun/HUD first-person layer is
            // drawn through the FP camera pair {view+0x158, view+0x1E8}, which
            // the engine builds with a CRUSHED viewmodel depth range (a thin
            // near-far slab so the gun never clips walls on a flat screen). In
            // VR that reads as an orthographic, flattened space: the gun looks
            // squashed, warps when twisted, and cannot move forward — the
            // user's exact report. The fix is to render the FP layer through
            // this eye's FULL WORLD camera + projection (position, orientation,
            // FOV AND depth terms), so the weapon lives in true world
            // perspective. The authoritative override happens during the
            // render in FpCameraRebuildHook (the engine rebuilds this pair
            // mid-render); this pre-render copy keeps it coherent beforehand.
            memcpy(reinterpret_cast<char*>(view) + 0x158, camera, 0x90);
            memcpy(reinterpret_cast<char*>(view) + 0x1E8,
                   reinterpret_cast<char*>(view) + 0x98, 0x90);
            if (g_fpCameraUpload)
                g_fpCameraUpload(reinterpret_cast<char*>(view) + 0x158,
                                 reinterpret_cast<char*>(view) + 0x1E8);
            // Snapshot this eye's finished camera + derived block and ARM the
            // FP hooks BEFORE the per-view preparation: the measured in-eye FP
            // driver runs (~3 per eye per frame, exactly-equal histogram
            // buckets) are triggered BY g_prepareView below — arming after it
            // meant every stamp silently missed (2026-07-19 evening logs).
            memcpy(g_eyeCompactCamera, camera, sizeof(g_eyeCompactCamera));
            memcpy(g_eyeDerivedBlock, reinterpret_cast<char*>(view) + 0x98,
                   sizeof(g_eyeDerivedBlock));
            g_eyeFpView.store(view, std::memory_order_release);
            // The draw routine consumes camera state uploaded to engine globals
            // by this per-view preparation stage, not the view structure
            // directly. Re-run it after each eye matrix rebuild.
            if (g_prepareView)
                g_prepareView(view, 0);
            g_origRenderView(view);
            g_eyeFpView.store(nullptr, std::memory_order_release);
            VR_CaptureRenderedEye(eye);
            VR_EndRasterEye();
        }
        g_fpStereoSolveScope.armed = false;
        g_stereoEye = -1;
        g_eyeFpView.store(nullptr,std::memory_order_release);

        // One mono world-only camera at Halo's collision-safe gameplay origin.
        // Its bullet-aligned direction remains hand-aimed, and the physical 4:3
        // display remains mounted on the gun independently.
        if(g_buildViewport && g_buildMatrices && VR_ScopeShouldRenderThisFrame() &&
           BuildRightHandScopeCamera(camera,saved) && VR_BeginScopeRaster())
        {
            alignas(16) unsigned char scopeTemporary[0x40]{};
            const float zoom=VR_GetScopeZoom();
            float sourceAspect=4.0f/3.0f;
            VR_GetScopeRenderAspect(sourceAspect);
            const ScopeProjectionTangents scopeProjection=
                ComputeScopeProjectionTangents(zoom,sourceAspect);
            const float tanX=scopeProjection.horizontal;
            const float tanY=scopeProjection.vertical;
            if(tanX>1e-4f && tanY>1e-4f)
            {
                float* cameraTangents=reinterpret_cast<float*>(camera+0x28);
                cameraTangents[0]=tanX;
                cameraTangents[1]=tanY;
            }
            g_buildViewport(camera,scopeTemporary);
            g_buildMatrices(camera,scopeTemporary,
                            reinterpret_cast<char*>(view)+0x98,0.0f);
            if(tanX>1e-4f && tanY>1e-4f)
            {
                float* projection=reinterpret_cast<float*>(
                    reinterpret_cast<char*>(view)+0x98+0x78);
                projection[0]=1.0f/tanX;
                projection[5]=1.0f/tanY;
            }
            memcpy(reinterpret_cast<char*>(view)+0x158,camera,0x90);
            memcpy(reinterpret_cast<char*>(view)+0x1E8,
                   reinterpret_cast<char*>(view)+0x98,0x90);
            g_scopeRenderActive.store(true,std::memory_order_release);
            if(g_prepareView) g_prepareView(view,0);
            g_origRenderView(view);
            g_scopeRenderActive.store(false,std::memory_order_release);
            VR_CaptureScope();
            VR_EndScopeRaster();
            static std::atomic<bool> logged{false};
            if(!logged.exchange(true))
                LOG("scope camera active: collision-safe bullet origin, %.2fx 4:3 lens",
                    zoom);
        }
        memcpy(camera, saved, sizeof(saved));
        memcpy(reinterpret_cast<char*>(view) + 0x98, savedDerived, sizeof(savedDerived));
        memcpy(reinterpret_cast<char*>(view) + 0x158, savedCameraCopy, sizeof(savedCameraCopy));
        memcpy(reinterpret_cast<char*>(view) + 0x1E8, savedDerivedCopy, sizeof(savedDerivedCopy));
    }

#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    constexpr float kOdstWorldUnitsPerMeter = 1.0f / 3.048f;

    // A camera gets first-person aim/control ownership when its blend is at
    // least this close to 1. On-foot and vehicle views use that path and the
    // internal scene-color target. The blend-0 death camera remains a distinct
    // third-person mode: it gets headset camera ownership but no weapon aim,
    // and its completed per-eye draws are captured from the direct backbuffer.
    void OdstRequestPresentationDetach()
    {
        g_odstCamera.presentationDetachRequested.fetch_add(
            1, std::memory_order_acq_rel);
    }

    bool OdstPresentationDetachOwned()
    {
        const uint64_t completed =
            g_odstCamera.presentationDetachCompleted.load(
                std::memory_order_acquire);
        const uint64_t requested =
            g_odstCamera.presentationDetachRequested.load(
                std::memory_order_acquire);
        return requested != completed;
    }

    bool OdstCameraOnlyContext()
    {
        const bool runtimeStateOwned =
            g_odstCamera.installed.load(std::memory_order_acquire) ||
            OdstPresentationDetachOwned();
        const TitleDescriptor* title = TitleAdapter_GetActive();
        const bool adapterReportsOdst =
            title && title->title == GameTitle::Halo3ODST;
        const bool privateBuildEnabled =
            TitleRegistry_HookPlan(GameTitle::Halo3ODST) ==
                TitleHookPlan::OdstExperimentalCameraCore;
        return OdstCameraOnlyScopeRequired(
            privateBuildEnabled, adapterReportsOdst, runtimeStateOwned);
    }

    bool ReadOdstEnginePaused(bool& paused)
    {
        const uintptr_t address = g_odstNativePauseFlag.load(
            std::memory_order_acquire);
        if (!address)
            return false;
        __try
        {
            const uint8_t value = *reinterpret_cast<const uint8_t*>(address);
            if (value > 1)
                return false;
            paused = value != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void OdstRequestFallback(OdstFallbackReason reason)
    {
        int expected = static_cast<int>(OdstFallbackReason::None);
        g_odstCamera.fallbackReason.compare_exchange_strong(
            expected, static_cast<int>(reason), std::memory_order_acq_rel);
        // Let an in-flight outer renderer finish both eyes coherently. Teardown
        // disables its entry before disarming the FP dependencies.
        g_odstCamera.cameraArrayReady.store(
            false, std::memory_order_release);
        g_odstCamera.teardownRequested.store(true, std::memory_order_release);
        PublishOdstLifecycle();
        OdstRequestPresentationDetach();
    }

    bool OdstBasisIsActive(const char* bytes, uintptr_t positionOffset,
                           uintptr_t forwardOffset, uintptr_t upOffset)
    {
        if (!bytes)
            return false;
        const float* position = reinterpret_cast<const float*>(
            bytes + positionOffset);
        const float* forward = reinterpret_cast<const float*>(
            bytes + forwardOffset);
        const float* up = reinterpret_cast<const float*>(bytes + upOffset);
        for (int axis = 0; axis < 3; ++axis)
            if (!isfinite(position[axis]) || !isfinite(forward[axis]) ||
                !isfinite(up[axis]))
                return false;
        const float forwardLength = forward[0] * forward[0] +
            forward[1] * forward[1] + forward[2] * forward[2];
        const float upLength = up[0] * up[0] + up[1] * up[1] + up[2] * up[2];
        const float crossX = forward[1] * up[2] - forward[2] * up[1];
        const float crossY = forward[2] * up[0] - forward[0] * up[2];
        const float crossZ = forward[0] * up[1] - forward[1] * up[0];
        const float crossLength = crossX * crossX + crossY * crossY +
            crossZ * crossZ;
        return forwardLength > 0.25f && forwardLength < 4.0f &&
            upLength > 0.25f && upLength < 4.0f && crossLength > 0.0625f;
    }

    bool OdstSourceCameraIsActive(const void* source)
    {
        const auto& layout = kOdstCameraProfile.layout;
        return OdstBasisIsActive(static_cast<const char*>(source),
            layout.sourcePosition, layout.sourceForward, layout.sourceUp);
    }

    bool OdstCompactCameraIsActive(const void* compact)
    {
        const auto& layout = kOdstCameraProfile.layout;
        return OdstBasisIsActive(static_cast<const char*>(compact),
            layout.compactPosition, layout.compactForward, layout.compactUp);
    }

    // Any active plain-perspective slot-0 camera has a proven layout for the
    // per-eye camera rewrite. First-person and vehicle renders use the internal
    // scene-color target; the blend-0 death camera uses a direct backbuffer
    // capture after the same camera rewrite.
    bool OdstCompactCameraIsStereoRedirectable(const void* compact)
    {
        if (!OdstCompactCameraIsActive(compact))
            return false;
        const auto& layout = kOdstCameraProfile.layout;
        const char* bytes = static_cast<const char*>(compact);
        const uint32_t modeFlags = *reinterpret_cast<const uint32_t*>(
            bytes + layout.compactModeFlags);
        const float verticalFov = *reinterpret_cast<const float*>(
            bytes + layout.verticalFov);
        const float referenceFov = *reinterpret_cast<const float*>(
            bytes + layout.referenceFov);
        const float verticalOffset = *reinterpret_cast<const float*>(
            bytes + layout.verticalOffset);
        const float nearClip = *reinterpret_cast<const float*>(
            bytes + layout.nearClip);
        const float farClip = *reinterpret_cast<const float*>(
            bytes + layout.farClip);
        const float* oblique = reinterpret_cast<const float*>(
            bytes + layout.obliquePlane);
        const uint32_t customProjection = *reinterpret_cast<const uint32_t*>(
            bytes + layout.customProjection);
        const float* customData = reinterpret_cast<const float*>(
            bytes + layout.customProjectionData);
        const auto boundsAreOrdered = [bytes](uintptr_t offset) {
            const int16_t* bounds = reinterpret_cast<const int16_t*>(
                bytes + offset);
            return bounds[0] < bounds[2] && bounds[1] < bounds[3];
        };
        return modeFlags == 0 && isfinite(verticalFov) &&
            verticalFov > 0.0001f && verticalFov < 3.1415f &&
            isfinite(referenceFov) && referenceFov > 0.0001f &&
            referenceFov < 3.1415f && verticalOffset == 0.0f &&
            boundsAreOrdered(layout.windowBounds) &&
            boundsAreOrdered(layout.renderBounds) &&
            boundsAreOrdered(layout.activeBounds) && isfinite(nearClip) &&
            isfinite(farClip) && nearClip > 0.0f && farClip > nearClip &&
            oblique[0] == 0.0f && oblique[1] == 0.0f &&
            oblique[2] == 0.0f && oblique[3] == 0.0f &&
            customProjection == 0 && customData[0] == 0.0f &&
            customData[1] == 0.0f && customData[2] == 0.0f &&
            customData[3] == 0.0f;
    }

    bool OdstCompactCameraUsesProvenMode(const void* compact)
    {
        if (!OdstCompactCameraIsStereoRedirectable(compact))
            return false;
        const auto& layout = kOdstCameraProfile.layout;
        const float fpBlend = *reinterpret_cast<const float*>(
            static_cast<const char*>(compact) + layout.compactFpBlend);
        return OdstFirstPersonControlBlend(fpBlend);
    }

    bool OdstSingleUserTailIsValid(const void* view)
    {
        if (!view)
            return false;
        const auto& layout = kOdstCameraProfile.layout;
        const char* bytes = static_cast<const char*>(view);
        return *reinterpret_cast<const int32_t*>(
                   bytes + layout.constructedViewSlot) == 0 &&
            *reinterpret_cast<const int32_t*>(bytes + layout.viewCount) == 1 &&
            *reinterpret_cast<const int32_t*>(
                bytes + layout.additionalContext) == 0 &&
            *reinterpret_cast<const int32_t*>(
                bytes + layout.renderUserIndex) == 0 &&
            *reinterpret_cast<const int32_t*>(
                bytes + layout.constructorResult) == 0 &&
            *reinterpret_cast<const int32_t*>(
                bytes + layout.tableDerivedField) == 0 &&
            *reinterpret_cast<const int32_t*>(
                bytes + layout.initializedZero) == 0 &&
            *reinterpret_cast<const uint8_t*>(
                bytes + layout.finalTailBoolean) == 0;
    }

    bool OdstCameraArraySupportsMode(
        uintptr_t cameraArray, bool requireFirstPerson)
    {
        if (!cameraArray)
            return false;
        const auto& layout = kOdstCameraProfile.layout;
        const char* view = reinterpret_cast<const char*>(cameraArray);
        const void* compact = view + layout.rootCurrentCompact;
        const bool modeValid = requireFirstPerson
            ? OdstCompactCameraUsesProvenMode(compact)
            : OdstCompactCameraIsStereoRedirectable(compact);
        if (!OdstSingleUserTailIsValid(view) || !modeValid)
            return false;

        // The nested FP driver publishes its source pointer after the root
        // camera becomes usable. A null pointer is therefore a valid pre-hook
        // installation state; any non-null pointer must still own slot 0's
        // compact camera exactly.
        const uintptr_t nestedSource = *reinterpret_cast<const uintptr_t*>(
            view + layout.nestedSourceCamera);
        if (!OdstNestedSourceIsCompatible(
                nestedSource, cameraArray + layout.rootCurrentCompact))
            return false;

        // Constructors may leave non-camera bookkeeping in the unused objects.
        // The single-user tail is authoritative; reject only another ACTIVE
        // compact camera rather than demanding byte-for-byte zero storage.
        bool inactive[3]{};
        for (size_t slot = 1; slot < 4; ++slot)
        {
            const char* candidate = view + slot * layout.viewStride;
            inactive[slot - 1] = OdstCompactCameraIsActive(
                candidate + layout.rootCurrentCompact);
        }
        return OdstInactiveCameraSlotsAreSafe(
            inactive[0], inactive[1], inactive[2]);
    }

    bool OdstCameraArraySupportsBringup(uintptr_t cameraArray)
    {
        // Parity fix (cutscene 3D): VR arms on ANY active, plain-perspective,
        // slot-0 single-user camera -- not only a first-person one. Levels that
        // OPEN with a cinematic (ODST's drop-pod intro) present a blend-0
        // cutscene camera before any first-person gameplay camera exists.
        // Gating arm on first person left those whole cutscenes flat 2D with
        // stereo off (confirmed in the headset log: never armed during the
        // intro). The stereo redirect (Build G) and OdstApplyHeadLook already
        // drive any active redirectable camera once armed -- that is why the
        // blend-0 vehicle works -- so arming on the same redirect predicate
        // makes the opening cutscene render stereo + head-tracked like Halo 3.
        // First-person blend still gates CONTROLS (aim/head-look ownership)
        // separately in OdstCamCopyBody; it never gated whether stereo is on.
        return OdstCameraArraySupportsMode(cameraArray, false);
    }

    bool OdstCameraArraySupportsStereoRedirect(uintptr_t cameraArray)
    {
        return OdstCameraArraySupportsMode(cameraArray, false);
    }

    void LogOdstCameraReadiness(uintptr_t cameraArray)
    {
        if (!cameraArray)
            return;
        const auto& layout = kOdstCameraProfile.layout;
        const char* view = reinterpret_cast<const char*>(cameraArray);
        const char* compact = view + layout.rootCurrentCompact;
        const int32_t* tail = reinterpret_cast<const int32_t*>(
            view + layout.constructedViewSlot);
        const uint32_t mode = *reinterpret_cast<const uint32_t*>(
            compact + layout.compactModeFlags);
        const float blend = *reinterpret_cast<const float*>(
            compact + layout.compactFpBlend);
        const float fov = *reinterpret_cast<const float*>(
            compact + layout.verticalFov);
        const float reference = *reinterpret_cast<const float*>(
            compact + layout.referenceFov);
        const float offset = *reinterpret_cast<const float*>(
            compact + layout.verticalOffset);
        const float nearClip = *reinterpret_cast<const float*>(
            compact + layout.nearClip);
        const float farClip = *reinterpret_cast<const float*>(
            compact + layout.farClip);
        const uintptr_t nestedSource = *reinterpret_cast<const uintptr_t*>(
            view + layout.nestedSourceCamera);
        bool inactive[3]{};
        for (size_t slot = 1; slot < 4; ++slot)
            inactive[slot - 1] = OdstCompactCameraIsActive(
                view + slot * layout.viewStride + layout.rootCurrentCompact);
        LOG("ODST camera readiness: tail=[%d,%d,%d,%d,%d,%d,%d,%u] "
            "mode=0x%08X blend=%.6f fov=%.6f ref=%.6f offset=%.6f "
            "near=%.6f far=%.3f",
            tail[0], tail[1], tail[2], tail[3], tail[4], tail[5], tail[6],
            static_cast<unsigned>(*reinterpret_cast<const uint8_t*>(
                view + layout.finalTailBoolean)),
            mode, blend, fov, reference, offset, nearClip, farClip);
        LOG("ODST camera readiness: nestedSource=%p expected=%p "
            "inactiveSlots=%d/%d/%d",
            reinterpret_cast<void*>(nestedSource),
            reinterpret_cast<void*>(cameraArray + layout.rootCurrentCompact),
            inactive[0] ? 1 : 0, inactive[1] ? 1 : 0,
            inactive[2] ? 1 : 0);
    }

    // Diagnostic (worker-only): while the core is installed but NOT yet armed,
    // log the live camera-array readiness whenever it materially changes
    // (rate-limited). If a cutscene keeps the view flat, this shows the actual
    // cutscene-camera state -- active, and whether it satisfies the stereo
    // redirect gate that now also drives arming -- so we can tell an arm that
    // reached the camera from a genuinely different (custom-projection) camera.
    void LogOdstWaitingReadinessIfChanged(uintptr_t cameraArray)
    {
        static uint32_t lastSig = 0xFFFFFFFFu;
        static uint64_t lastLogMs = 0;
        if (!cameraArray)
            return;
        const auto& layout = kOdstCameraProfile.layout;
        const char* compact =
            reinterpret_cast<const char*>(cameraArray) + layout.rootCurrentCompact;
        const uint32_t mode =
            *reinterpret_cast<const uint32_t*>(compact + layout.compactModeFlags);
        const bool active = OdstCompactCameraIsActive(compact);
        const bool redirectable =
            OdstCompactCameraIsStereoRedirectable(compact);
        const bool tailValid = OdstSingleUserTailIsValid(
            reinterpret_cast<const void*>(cameraArray));
        const uint32_t sig = (mode & 0x3FFFu) |
            (static_cast<uint32_t>(active) << 16) |
            (static_cast<uint32_t>(redirectable) << 17) |
            (static_cast<uint32_t>(tailValid) << 18);
        const uint64_t now = GetTickCount64();
        if (sig == lastSig && now - lastLogMs < 2000)
            return;
        lastSig = sig;
        lastLogMs = now;
        LOG("ODST camera WAIT: active=%d tailValid=%d redirectable=%d "
            "(arm-eligible=%s) -- core installed, not yet armed",
            active ? 1 : 0, tailValid ? 1 : 0, redirectable ? 1 : 0,
            (active && tailValid && redirectable) ? "YES" : "NO");
        LogOdstCameraReadiness(cameraArray);
    }

    void ApplyOdstMotionBlurSetting()
    {
        if (!g_odstCamera.motionBlurResolved)
            return;
        if (!g_config.motion_blur)
        {
            for (OdstMotionBlurVar& var : g_odstCamera.motionBlurVars)
            {
                if (*var.slot != 0.0f)
                    var.original = *var.slot;
                *var.slot = 0.0f;
            }
            if (!g_odstCamera.motionBlurZeroed)
            {
                g_odstCamera.motionBlurZeroed = true;
                LOG("ODST comfort: motion blur forced OFF through title-native scale/max vars");
            }
        }
        else if (g_odstCamera.motionBlurZeroed)
        {
            for (OdstMotionBlurVar& var : g_odstCamera.motionBlurVars)
                *var.slot = var.original;
            g_odstCamera.motionBlurZeroed = false;
            LOG("ODST comfort: motion blur restored to engine values");
        }
    }

    void RestoreOdstMotionBlurVars()
    {
        if (!g_odstCamera.motionBlurResolved ||
            !g_odstCamera.motionBlurZeroed)
            return;
        for (OdstMotionBlurVar& var : g_odstCamera.motionBlurVars)
            *var.slot = var.original;
        g_odstCamera.motionBlurZeroed = false;
        LOG("ODST comfort: stock motion-blur values restored during teardown");
    }

    void OdstApplyHeadLook(void* source)
    {
        if (!source)
            return;
        float quaternion[4], headPosition[3];
        if (!VR_GetHeadPose(quaternion, headPosition))
            return;

        const float x = quaternion[0], y = quaternion[1];
        const float z = quaternion[2], w = quaternion[3];
        const float headForwardX = -2.0f * (w * y + x * z);
        const float headForwardY = 2.0f * (w * x - y * z);
        const float headForwardZ = -(1.0f - 2.0f * (x * x + y * y));
        const float headYaw = atan2f(headForwardX, -headForwardZ);
        const float headPitch = asinf(Clamp(headForwardY, -1.0f, 1.0f));

        const float headUpX = 2.0f * (x * y - w * z);
        const float headUpY = 1.0f - 2.0f * (x * x + z * z);
        const float headUpZ = 2.0f * (y * z + w * x);
        float horizonRightX = -headForwardZ;
        float horizonRightZ = headForwardX;
        float horizonLength = sqrtf(horizonRightX * horizonRightX +
                                    horizonRightZ * horizonRightZ);
        if (horizonLength < 1e-4f)
            horizonLength = 1e-4f;
        horizonRightX /= horizonLength;
        horizonRightZ /= horizonLength;
        const float neutralUpX = -headForwardY * horizonRightZ;
        const float neutralUpY = horizonLength;
        const float neutralUpZ = headForwardY * horizonRightX;
        const float headRoll = atan2f(
            headUpX * horizonRightX + headUpZ * horizonRightZ,
            headUpX * neutralUpX + headUpY * neutralUpY + headUpZ * neutralUpZ);

        const auto& layout = kOdstCameraProfile.layout;
        char* bytes = static_cast<char*>(source);
        float* forward = reinterpret_cast<float*>(bytes + layout.sourceForward);
        float* up = reinterpret_cast<float*>(bytes + layout.sourceUp);
        float* position = reinterpret_cast<float*>(bytes + layout.sourcePosition);

        const float stockForwardLength = sqrtf(
            forward[0] * forward[0] + forward[1] * forward[1] +
            forward[2] * forward[2]);
        if (stockForwardLength < 1e-4f)
            return;
        const float stockYaw = atan2f(forward[1], forward[0]);

        // Halo 3 cutscene-facing parity (dd1abc5): at each authored cinematic
        // cut, rebase the VR yaw reference so "forward" aligns with the new
        // shot's camera -- otherwise a cut can leave the viewer facing away from
        // the action. Between cuts the head looks around freely; pitch and roll
        // stay HMD-owned to avoid an artificial rotation during continuous
        // camera motion. Uses ODST's own resolved cinematic scene/shot state.
        static thread_local bool prevCinematic = false;
        static thread_local int32_t prevScene = -1;
        static thread_local int32_t prevShot = -1;
        int32_t cinematicScene = -1;
        int32_t cinematicShot = -1;
        const bool cinematic = ReadCinematicShot(cinematicScene, cinematicShot);
        const bool cinematicBoundary =
            (cinematic && (!prevCinematic ||
                cinematicScene != prevScene || cinematicShot != prevShot)) ||
            (!cinematic && prevCinematic);
        prevCinematic = cinematic;
        prevScene = cinematic ? cinematicScene : -1;
        prevShot = cinematic ? cinematicShot : -1;

        const bool manualRecenter =
            g_needRecenter.exchange(false, std::memory_order_acq_rel);
        if (manualRecenter || cinematicBoundary)
        {
            g_gameYawRef = stockYaw;
            g_headYawRef = headYaw;
            if (manualRecenter)
            {
                memcpy(g_headPosRef, headPosition, sizeof(g_headPosRef));
                g_needPosRecenter.store(false, std::memory_order_release);
            }
            if (cinematicBoundary)
            {
                g_cinematicRebaseScene.store(
                    cinematic ? cinematicScene : -1,
                    std::memory_order_relaxed);
                g_cinematicRebaseShot.store(
                    cinematic ? cinematicShot : -1,
                    std::memory_order_relaxed);
                g_cinematicRebaseSerial.fetch_add(
                    1, std::memory_order_release);
            }
        }
        else if (g_needPosRecenter.exchange(false, std::memory_order_acq_rel))
        {
            memcpy(g_headPosRef, headPosition, sizeof(g_headPosRef));
        }

        // Halo 3 parity: recentered yaw plus HMD-relative yaw; absolute HMD
        // pitch and roll. ODST's changing stock pitch/roll must not move the
        // tracked view through right-stick input, recoil, or authored shake.
        OdstHalo3LookAngles look{};
        if (!ComputeOdstHalo3LookAngles(
                g_gameYawRef, g_headYawRef, headYaw, headPitch, headRoll,
                g_yawSign.load(std::memory_order_relaxed),
                g_pitchSign.load(std::memory_order_relaxed),
                g_pitchTrim.load(std::memory_order_relaxed), look))
            return;
        const float gameYaw = look.yaw;
        const float gamePitch = look.pitch;
        const float gameRoll = look.roll;
        const float cosPitch = cosf(gamePitch), sinPitch = sinf(gamePitch);
        const float cosYaw = cosf(gameYaw), sinYaw = sinf(gameYaw);
        forward[0] = cosPitch * cosYaw;
        forward[1] = cosPitch * sinYaw;
        forward[2] = sinPitch;
        const float cosRoll = cosf(gameRoll), sinRoll = sinf(gameRoll);
        up[0] = (-sinPitch * cosYaw) * cosRoll + sinYaw * sinRoll;
        up[1] = (-sinPitch * sinYaw) * cosRoll - cosYaw * sinRoll;
        up[2] = cosPitch * cosRoll;

        if (g_positional.load(std::memory_order_relaxed))
        {
            const float dx = headPosition[0] - g_headPosRef[0];
            const float dy = headPosition[1] - g_headPosRef[1];
            const float dz = headPosition[2] - g_headPosRef[2];
            float horizontalLength = sqrtf(
                headForwardX * headForwardX + headForwardZ * headForwardZ);
            if (horizontalLength < 1e-4f)
                horizontalLength = 1e-4f;
            const float horizontalForwardX = headForwardX / horizontalLength;
            const float horizontalForwardZ = headForwardZ / horizontalLength;
            const float forwardMove = dx * horizontalForwardX +
                dz * horizontalForwardZ;
            const float rightMove = dx * (-horizontalForwardZ) +
                dz * horizontalForwardX;
            const float scale = kOdstWorldUnitsPerMeter;
            position[0] += Clamp(
                (cosYaw * forwardMove + sinYaw * rightMove) * scale,
                -1.5f, 1.5f);
            position[1] += Clamp(
                (sinYaw * forwardMove - cosYaw * rightMove) * scale,
                -1.5f, 1.5f);
            position[2] += Clamp(dy * scale, -1.5f, 1.5f);
        }
    }

    __declspec(noinline) void* __fastcall OdstCamCopyBody(
        void* destination, void* source)
    {
        CamCopyFn original = g_odstCamera.originalCamCopy;
        if (!original)
            return destination;
        const auto& layout = kOdstCameraProfile.layout;
        const uintptr_t primaryDestination = g_odstCamera.gunCameraArray
            ? g_odstCamera.gunCameraArray + layout.rootCurrentCompact
            : 0;
        const bool ownsPrimaryCamera = primaryDestination &&
            reinterpret_cast<uintptr_t>(destination) == primaryDestination;
        const bool singleUserPath = ownsPrimaryCamera &&
            OdstSingleUserTailIsValid(
                reinterpret_cast<const void*>(g_odstCamera.gunCameraArray));
        // Any active camera in our proven slot-0 view keeps the core alive and
        // receives Halo 3's camera ownership. Halo 3 publishes the pre-head-look
        // aim forward on EVERY live camera copy; it does not gate vehicle aim on
        // a first-person blend. ODST's settled vehicle camera is blend 0, so the
        // old blend gate disabled the right-controller path for the entire ride.
        const bool ownsActiveCamera =
            g_odstCamera.installed.load(std::memory_order_acquire) &&
            singleUserPath &&
            OdstSourceCameraIsActive(source);
        // Halo 3 applies headset ownership to every active observer camera,
        // including vehicles, death, and cinematics. Do the same for ODST: a
        // blend-0 active camera still owns both head look and continuous aim.
        const bool transform = ownsActiveCamera &&
            g_odstCamera.armed.load(std::memory_order_acquire) &&
            !g_odstCamera.teardownRequested.load(std::memory_order_acquire) &&
            g_enabled.load(std::memory_order_relaxed);
        float savedPosition[3]{}, savedForward[3]{}, savedUp[3]{};
        if (ownsActiveCamera)
        {
            ApplyOdstMotionBlurSetting();
            // Exact Halo 3 control ownership: publish the source camera's
            // pre-head-look forward for every active camera copy. Vehicles
            // then consume the same continuous closed-loop right-stick aim
            // that guides Halo 3 vehicles. Reads only the proven source
            // layout; no new offset or vehicle-specific patch.
            const char* srcBytes = static_cast<const char*>(source);
            float aimForward[3];
            memcpy(aimForward, srcBytes + layout.sourceForward,
                   sizeof(aimForward));
            if (isfinite(aimForward[0]) && isfinite(aimForward[1]) &&
                isfinite(aimForward[2]))
            {
                g_aimFwdX.store(aimForward[0]);
                g_aimFwdY.store(aimForward[1]);
                g_aimFwdZ.store(aimForward[2]);
                g_aimSeen = true;
            }
        }
        if (transform)
        {
            ApplyVrTurn();
            const char* bytes = static_cast<const char*>(source);
            memcpy(savedPosition, bytes + layout.sourcePosition,
                   sizeof(savedPosition));
            memcpy(savedForward, bytes + layout.sourceForward,
                   sizeof(savedForward));
            memcpy(savedUp, bytes + layout.sourceUp, sizeof(savedUp));
            g_baseCamX.store(savedPosition[0]);
            g_baseCamY.store(savedPosition[1]);
            g_baseCamZ.store(savedPosition[2]);
            g_baseCamValid.store(true, std::memory_order_release);
            OdstApplyHeadLook(source);
            const char* transformedBytes = static_cast<const char*>(source);
            const float* transformedPosition =
                reinterpret_cast<const float*>(
                    transformedBytes + layout.sourcePosition);
            const float* transformedForward =
                reinterpret_cast<const float*>(
                    transformedBytes + layout.sourceForward);
            const float* transformedUp =
                reinterpret_cast<const float*>(
                    transformedBytes + layout.sourceUp);
            g_camX.store(transformedPosition[0]);
            g_camY.store(transformedPosition[1]);
            g_camZ.store(transformedPosition[2]);
            for (int axis = 0; axis < 3; ++axis)
            {
                g_camFwd[axis].store(transformedForward[axis]);
                g_camUp[axis].store(transformedUp[axis]);
            }
            g_camValid.store(true, std::memory_order_release);
            if (!g_worldUpInit.load(std::memory_order_acquire))
            {
                const float length = sqrtf(
                    transformedUp[0] * transformedUp[0] +
                    transformedUp[1] * transformedUp[1] +
                    transformedUp[2] * transformedUp[2]);
                if (length > 1e-4f)
                {
                    for (int axis = 0; axis < 3; ++axis)
                        g_worldUp[axis].store(
                            transformedUp[axis] / length,
                            std::memory_order_relaxed);
                    g_worldUpInit.store(true, std::memory_order_release);
                }
            }
            else
            {
                float worldUp[3] = {
                    g_worldUp[0].load(std::memory_order_relaxed),
                    g_worldUp[1].load(std::memory_order_relaxed),
                    g_worldUp[2].load(std::memory_order_relaxed)};
                const float lookLevel = fabsf(
                    transformedForward[0] * worldUp[0] +
                    transformedForward[1] * worldUp[1] +
                    transformedForward[2] * worldUp[2]);
                if (lookLevel < 0.42f)
                {
                    constexpr float kBlend = 0.01f;
                    for (int axis = 0; axis < 3; ++axis)
                        worldUp[axis] +=
                            (transformedUp[axis] - worldUp[axis]) * kBlend;
                    const float length = sqrtf(
                        worldUp[0] * worldUp[0] +
                        worldUp[1] * worldUp[1] +
                        worldUp[2] * worldUp[2]);
                    if (length > 1e-4f)
                        for (int axis = 0; axis < 3; ++axis)
                            g_worldUp[axis].store(
                                worldUp[axis] / length,
                                std::memory_order_relaxed);
                }
            }
        }
        else if (ownsActiveCamera)
        {
            g_camValid.store(false, std::memory_order_release);
            g_baseCamValid.store(false, std::memory_order_release);
        }
        else if (OdstCamCopyRequestsTeardown(
                     g_odstCamera.armed.load(std::memory_order_acquire),
                     ownsPrimaryCamera, singleUserPath))
        {
            // Our slot-0 view object no longer matches the single-user layout:
            // a genuine level unload/transition, not a mere non-FP camera. An
            // active third-person camera (singleUserPath still valid) is NOT a
            // teardown -- it renders stock and keeps the core armed.
            OdstRequestFallback(OdstSourceCameraIsActive(source)
                ? OdstFallbackReason::UnsupportedCameraMode
                : OdstFallbackReason::LevelUnloaded);
        }
        void* result = original(destination, source);
        if (transform)
        {
            char* bytes = static_cast<char*>(source);
            memcpy(bytes + layout.sourcePosition, savedPosition,
                   sizeof(savedPosition));
            memcpy(bytes + layout.sourceForward, savedForward,
                   sizeof(savedForward));
            memcpy(bytes + layout.sourceUp, savedUp, sizeof(savedUp));
        }
        if (ownsActiveCamera)
        {
            // Match Halo 3: every active camera copy is an aim/reticle timing
            // signal as well as a heartbeat, including the blend-0 vehicle.
            if (g_odstCamera.armed.load(std::memory_order_acquire))
                VR_NotifyCameraTransform();
            const uint64_t cameraNowMs = GetTickCount64();
            g_odstLastCamCopyMs.store(
                cameraNowMs, std::memory_order_release);
            const uint32_t runtimeGeneration =
                g_odstRuntimeGeneration.load(std::memory_order_acquire);
            if (runtimeGeneration)
            {
                TitleAdapter_PublishHeartbeat(
                    GameTitle::Halo3ODST, runtimeGeneration, cameraNowMs);
            }
            g_odstCamera.sawValidCamera.store(true, std::memory_order_release);
        }
        return result;
    }

    __declspec(noinline) void __fastcall OdstObserverCameraEffectHook(
        int userIndex)
    {
        g_odstCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        ObserverCameraEffectFn original =
            g_odstCamera.originalObserverCameraEffect;
        const bool suppress =
            g_odstCamera.armed.load(std::memory_order_acquire) &&
            !g_odstCamera.teardownRequested.load(std::memory_order_acquire) &&
            g_enabled.load(std::memory_order_relaxed);
        if (!suppress && original)
            original(userIndex);
        g_odstCamera.activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    __declspec(noinline) void* __fastcall OdstCamCopyHook(
        void* destination, void* source)
    {
        // The complete wrapper is unwind-backed and scanned during teardown,
        // including its prologue before this increment and its epilogue after
        // the decrement. If a C++ exception ever escapes the body, the
        // decrement is deliberately skipped so cleanup stays fail-closed.
        g_odstCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        void* result = OdstCamCopyBody(destination, source);
        g_odstCamera.activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return result;
    }

    __declspec(noinline) void __fastcall OdstFpCameraRebuildBody(
        void* view, unsigned char flag)
    {
        FpCameraRebuildFn original = g_odstCamera.originalFpCameraRebuild;
        if (!original)
            return;
        original(view, flag);
        char* eyeView = static_cast<char*>(
            g_odstCamera.eyeView.load(std::memory_order_acquire));
        if (!view || !g_odstCamera.armed.load(std::memory_order_acquire) ||
            !eyeView)
            return;
        const auto& layout = kOdstCameraProfile.layout;
        if (view != eyeView + layout.nestedFpBase)
        {
            OdstRequestFallback(OdstFallbackReason::UnsupportedCameraMode);
            return;
        }
        char* bytes = static_cast<char*>(view);
        memcpy(bytes + layout.rootCurrentCompact,
               g_odstCamera.eyeCompactCamera, layout.compactSize);
        memcpy(bytes + layout.rootSecondaryDerived,
               g_odstCamera.eyeDerivedBlock, layout.derivedSize);
        if (g_odstCamera.fpCameraUpload)
            g_odstCamera.fpCameraUpload(
                bytes + layout.rootCurrentCompact,
                bytes + layout.rootSecondaryDerived);
    }

    __declspec(noinline) void __fastcall OdstFpCameraRebuildHook(
        void* view, unsigned char flag)
    {
        g_odstCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        OdstFpCameraRebuildBody(view, flag);
        g_odstCamera.activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    __declspec(noinline) void __fastcall OdstFpDriverBody(
        void* view, unsigned char flag)
    {
        FpDriverFn original = g_odstCamera.originalFpDriver;
        if (!original)
            return;
        void* eyeView = g_odstCamera.eyeView.load(std::memory_order_acquire);
        if (view && g_odstCamera.armed.load(std::memory_order_acquire) && eyeView)
        {
            const auto& layout = kOdstCameraProfile.layout;
            if (view != eyeView || !OdstSingleUserTailIsValid(view))
            {
                OdstRequestFallback(OdstFallbackReason::UnsupportedCameraMode);
                original(view, flag);
                return;
            }
            char* bytes = static_cast<char*>(view);
            memcpy(bytes + layout.rootSecondaryCompact,
                   g_odstCamera.eyeCompactCamera, layout.compactSize);
            memcpy(bytes + layout.rootSecondaryDerived,
                   g_odstCamera.eyeDerivedBlock, layout.derivedSize);
            memcpy(bytes + layout.nestedCurrentCompact,
                   g_odstCamera.eyeCompactCamera, layout.compactSize);
            memcpy(bytes + layout.nestedSecondaryDerived,
                   g_odstCamera.eyeDerivedBlock, layout.derivedSize);
            if (g_odstCamera.fpCameraUpload)
                g_odstCamera.fpCameraUpload(
                    bytes + layout.nestedCurrentCompact,
                    bytes + layout.nestedSecondaryDerived);
        }
        original(view, flag);
    }

    __declspec(noinline) void __fastcall OdstFpDriverHook(
        void* view, unsigned char flag)
    {
        g_odstCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        OdstFpDriverBody(view, flag);
        g_odstCamera.activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    // ODST weapon/arm adapter. The shared Halo 3 solver owns all player-facing
    // behavior and configuration; only this title-proven skeleton topology and
    // these ODST-resolved hook originals live in the adapter.
    thread_local float g_odstFpRootScale[2] = {1.0f, 1.0f};

    bool BuildOdstCenterFpRoot(int slot, BoneMatrix& root)
    {
        if (slot < 0 || slot > 1 || !g_camValid.load())
            return false;
        float basis[9];
        if (!LoadCameraBasis(basis))
            return false;
        root = {};
        const float scale = g_odstFpRootScale[slot];
        root.scale = isfinite(scale) && fabsf(scale) > 0.001f ? scale : 1.0f;
        memcpy(root.rotation, basis, sizeof(basis));
        root.translation[0] = g_camX.load();
        root.translation[1] = g_camY.load();
        root.translation[2] = g_camZ.load();
        return true;
    }

    void MeasureOdstAuthoredBarrel(
        int slot, const BoneMatrix* bones, int wrist)
    {
        const float* wr = bones[wrist].rotation;
        float barrel[3] = {wr[0], wr[3], wr[6]};
        float length = sqrtf(barrel[0] * barrel[0] +
                             barrel[1] * barrel[1] +
                             barrel[2] * barrel[2]);
        if (length <= 1e-4f || !isfinite(length))
            return;
        for (float& component : barrel)
            component /= length;
        if (g_barrelInWristValid[slot].load(std::memory_order_relaxed))
        {
            constexpr float kBlend = 0.02f;
            for (int axis = 0; axis < 3; ++axis)
                barrel[axis] =
                    g_barrelInWrist[slot][axis].load(std::memory_order_relaxed) *
                        (1.0f - kBlend) +
                    barrel[axis] * kBlend;
            length = sqrtf(barrel[0] * barrel[0] +
                           barrel[1] * barrel[1] +
                           barrel[2] * barrel[2]);
            if (length > 1e-4f)
                for (float& component : barrel)
                    component /= length;
        }
        for (int axis = 0; axis < 3; ++axis)
            g_barrelInWrist[slot][axis].store(
                barrel[axis], std::memory_order_relaxed);
        g_barrelInWristValid[slot].store(true, std::memory_order_release);
    }

    // ---- One-shot FP weapon-layout self-check (headset diagnostics) ---------
    // The two remaining fail-closed risks for the ODST weapon/arm path are (1)
    // the FP interpolation hook never firing on a weapon slot and (2) the live
    // node count falling outside ComputeOdstFpSkeletonLayout's accepted 39..64
    // range (H3ODSTEK-derived indices vs. the retail skeleton). Neither can be
    // logged from this hot hook, so it publishes atomically and the 50 ms worker
    // emits one line -- exactly the pattern the non-FP camera capture uses below.
    struct OdstFpLayoutSelfCheck
    {
        std::atomic<uint32_t> key{0};     // 0 = nothing observed yet
        std::atomic<int> slot{-1};
        std::atomic<int> nodeCount{-1};
        std::atomic<int> accepted{0};     // 1 = layout accepted the node count
        std::atomic<int> wrist{-1};
        std::atomic<int> cameraControl{-1};
    };
    OdstFpLayoutSelfCheck g_odstFpLayoutSelfCheck;
    std::atomic<uint32_t> g_odstFpLayoutLoggedKey{0};

    // Atomic-only publish from the hot FP interpolation hook. Deduped by a key
    // built from slot/count/accepted so it republishes only when the observation
    // changes; the worker then emits each distinct observation at most once.
    void PublishOdstFpLayoutSelfCheck(int slot, int nodeCount, bool accepted,
                                      const OdstFpSkeletonLayout& layout)
    {
        const uint32_t key =
            (static_cast<uint32_t>(slot & 0x3) << 30) |
            (static_cast<uint32_t>(accepted ? 1u : 0u) << 29) |
            (static_cast<uint32_t>(nodeCount & 0x1FF) << 20) |
            0x1u;  // low bit set so a real observation is never key 0
        OdstFpLayoutSelfCheck& sc = g_odstFpLayoutSelfCheck;
        if (sc.key.load(std::memory_order_relaxed) == key)
            return;
        sc.slot.store(slot, std::memory_order_relaxed);
        sc.nodeCount.store(nodeCount, std::memory_order_relaxed);
        sc.accepted.store(accepted ? 1 : 0, std::memory_order_relaxed);
        sc.wrist.store(accepted ? layout.rightWrist : -1,
                       std::memory_order_relaxed);
        sc.cameraControl.store(accepted ? layout.cameraControl : -1,
                               std::memory_order_relaxed);
        sc.key.store(key, std::memory_order_release);
    }

    __declspec(noinline) bool __fastcall OdstFpInterpolateWeaponHook(
        int view, int id, int slot, BoneMatrix** outBones, int* outCount)
    {
        g_odstCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        bool result = false;
        FpInterpolateFn original =
            reinterpret_cast<FpInterpolateFn>(g_odstCamera.originalFpInterpolate);
        if (original)
            result = original(view, id, slot, outBones, outCount);
        const bool ownsWeapons =
            g_odstCamera.armed.load(std::memory_order_acquire) &&
            !g_odstCamera.teardownRequested.load(std::memory_order_acquire) &&
            g_enabled.load(std::memory_order_relaxed);
        if ((slot == 0 || slot == 1) && ownsWeapons)
        {
            g_fpInterpolationContexts[slot] = {};
            OdstFpSkeletonLayout layout{};
            const bool haveBones = result && outBones && outCount && *outBones;
            const bool layoutOk =
                haveBones && ComputeOdstFpSkeletonLayout(*outCount, layout);
            PublishOdstFpLayoutSelfCheck(
                slot, haveBones ? *outCount : -1, layoutOk, layout);
            if (layoutOk)
            {
                const int count = *outCount;
                FpInterpolationContext& context =
                    g_fpInterpolationContexts[slot];
                context.source = *outBones;
                context.count = count;
                context.player = view;
                context.slot = slot;
                context.wrist = layout.rightWrist;
                context.cameraControl = layout.cameraControl;
                context.elbow = layout.rightElbow;
                context.shoulder = layout.rightShoulder;
                context.wristDescendants =
                    layout.rightHandAndWeaponDescendants;
                context.lWrist = layout.leftWrist;
                context.lElbow = layout.leftElbow;
                context.lShoulder = layout.leftShoulder;
                context.lWristDescendants = layout.leftHandDescendants;
                context.valid = true;
                memcpy(g_fpUnmodifiedInterpolations[slot], *outBones,
                       static_cast<size_t>(count) * sizeof(BoneMatrix));
                MeasureOdstAuthoredBarrel(
                    slot, g_fpUnmodifiedInterpolations[slot],
                    layout.rightWrist);
                BoneMatrix root{};
                if (BuildOdstCenterFpRoot(slot, root))
                    ApplyControllerToMarkerBonesFromRoot(
                        root, *outBones, count, layout.rightWrist,
                        layout.leftWrist, slot == 1);
            }
        }
        else if (slot == 0 || slot == 1)
            g_fpInterpolationContexts[slot] = {};
        g_odstCamera.activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return result;
    }

    __declspec(noinline) void __fastcall OdstFpVisiblePaletteWeaponHook(
        uint16_t tag, const BoneMatrix* root, BoneMatrix* destination,
        uintptr_t unused, const BoneMatrix* source, const int32_t* boneMap)
    {
        g_odstCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        FpInterpolationContext context{};
        for (FpInterpolationContext& candidate : g_fpInterpolationContexts)
            if (candidate.valid && source && candidate.source == source)
            {
                context = candidate;
                candidate.valid = false;
                break;
            }

        const BoneMatrix* selectedSource = source;
        if (context.valid && source == context.source)
            selectedSource = g_fpUnmodifiedInterpolations[context.slot];
        bool reconstructed = false;
        if (root && source)
        {
            if (context.valid && isfinite(root->scale) &&
                fabsf(root->scale) > 0.001f)
                g_odstFpRootScale[context.slot] = root->scale;
            reconstructed = ReconstructVisiblePaletteSource(
                tag, context, *root, source, selectedSource);
        }
        if (g_config.floating_hands && reconstructed && context.valid &&
            selectedSource == g_fpPaletteScratch &&
            context.count > 0 && context.count <= 64)
        {
            const uint64_t keep =
                context.wristDescendants | context.lWristDescendants;
            for (int i = 0; i < context.count; ++i)
                if (!(keep & (uint64_t{1} << i)))
                    g_fpPaletteScratch[i].scale = 0.0001f;
        }
        if (g_scopeRenderActive.load(std::memory_order_acquire) &&
            context.valid && selectedSource &&
            context.count > 0 && context.count <= 64)
        {
            memcpy(g_scopeHiddenPalette, selectedSource,
                   static_cast<size_t>(context.count) * sizeof(BoneMatrix));
            for (int i = 0; i < context.count; ++i)
                g_scopeHiddenPalette[i].scale = 0.0001f;
            selectedSource = g_scopeHiddenPalette;
        }
        FpVisiblePaletteFn original = reinterpret_cast<FpVisiblePaletteFn>(
            g_odstCamera.originalFpVisiblePalette);
        if (original)
            original(
                tag, root, destination, unused, selectedSource, boneMap);
        g_odstCamera.activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    // ---- Full-parity diagnostics: non-first-person camera capture ----------
    // When slot 0 renders a live camera that is not the proven first-person
    // mode (the third-person death-cam, vehicles, turrets, cutscenes), the core
    // now renders it stock and stays armed instead of tearing down. This
    // log-only capture records that camera's field layout so the next build can
    // enable a stereo redirect for it with ODST evidence rather than a guess.
    // Publish is atomic-only from the hot render hook; the 50 ms worker emits.
    struct OdstNonFpCameraCapture
    {
        std::atomic<uint32_t> seq{0};        // even = stable, odd = mid-write
        std::atomic<uint32_t> modeFlags{0};
        std::atomic<uint32_t> arrayOffset{0};
        std::atomic<uint32_t> flags{0};      // see the OdstNonFpFlag bits below
        std::atomic<float> fpBlend{0.0f};
        std::atomic<float> verticalOffset{0.0f};
        std::atomic<float> verticalFov{0.0f};
        std::atomic<float> referenceFov{0.0f};
        std::atomic<float> nearClip{0.0f};
        std::atomic<float> farClip{0.0f};
    };
    enum OdstNonFpFlag : uint32_t
    {
        OdstNonFpTailValid = 1u << 0,
        OdstNonFpNestedMatch = 1u << 1,
        OdstNonFpActive = 1u << 2,
        OdstNonFpPlainPerspective = 1u << 3,  // no oblique/custom projection,
                                              // ordered bounds, valid clips
        // Granular breakdown of the exact OdstCompactCameraIsStereoRedirectable
        // sub-checks, so a flat cutscene camera tells us precisely which field
        // disqualifies it rather than a single YES/NO.
        OdstNonFpModeZero = 1u << 4,          // +0x24 mode flags == 0
        OdstNonFpVoffZero = 1u << 5,          // +0x34 vertical offset == 0
        OdstNonFpFovInRange = 1u << 6,        // vertical/reference FOV in range
        OdstNonFpObliqueZero = 1u << 7,       // +0x6C..+0x7B oblique plane zero
        OdstNonFpCustomProjZero = 1u << 8,    // +0x7C enable + data zero
        OdstNonFpBoundsOrdered = 1u << 9,     // window/render/active bounds
        OdstNonFpClipsValid = 1u << 10,       // near>0, far>near, finite
    };
    OdstNonFpCameraCapture g_odstNonFpCameraCapture;
    std::atomic<uint32_t> g_odstNonFpCameraLoggedSeq{0};
    // Sentinel modeFlags so a fresh non-FP camera re-captures after any FP frame.
    constexpr uint32_t kOdstNonFpNoCapture = 0xFFFFFFFFu;
    std::atomic<uint32_t> g_odstNonFpCameraLastMode{kOdstNonFpNoCapture};

    // ---- Cutscene diagnostic: why a slot-0 render did NOT stereo-redirect -----
    // The non-FP capture above only fires for an active slot-0 camera whose
    // redirect gate failed. It never sees the cases most likely to make the
    // opening ODST cutscene flat: the private core still disarmed (arming
    // timing), OpenXR withholding presentation, an unresolved camera array, or a
    // cutscene rendered on a foreign (non-slot-0) camera. This lightweight
    // read-only publisher records that reason. Published atomically from the hot
    // render hook; the 50 ms worker emits one line per distinct reason.
    enum class OdstRenderSkipReason : uint32_t
    {
        None = 0,             // last frame was stereo-redirected
        NotArmed = 1,         // core installed but not yet armed (debounce)
        TeardownRequested = 2,
        Unfocused = 3,        // VR_ShouldRenderPreparedFrame() == false
        ArrayUnavailable = 4, // gunCameraArray unresolved / view below array
        NotStrideAligned = 5, // view offset is not a clean slot boundary
        ForeignSlot = 6,      // active render on a slot other than 0
    };
    struct OdstRenderSkipDiag
    {
        std::atomic<uint32_t> seq{0};       // even = stable, odd = mid-write
        std::atomic<uint32_t> reason{0};
        std::atomic<uint32_t> arrayOffset{0};
    };
    OdstRenderSkipDiag g_odstRenderSkipDiag;
    std::atomic<uint32_t> g_odstRenderSkipLoggedSeq{0};
    constexpr uint32_t kOdstRenderSkipNone =
        static_cast<uint32_t>(OdstRenderSkipReason::None);
    std::atomic<uint32_t> g_odstRenderSkipLastReason{kOdstRenderSkipNone};

    void PublishOdstRenderSkip(OdstRenderSkipReason reason, uint32_t arrayOffset)
    {
        const uint32_t r = static_cast<uint32_t>(reason);
        if (g_odstRenderSkipLastReason.load(std::memory_order_relaxed) == r)
            return;  // steady state does not burst; a new reason re-publishes
        OdstRenderSkipDiag& d = g_odstRenderSkipDiag;
        d.seq.fetch_add(1, std::memory_order_acq_rel);  // -> odd (writing)
        d.reason.store(r, std::memory_order_relaxed);
        d.arrayOffset.store(arrayOffset, std::memory_order_relaxed);
        d.seq.fetch_add(1, std::memory_order_release);  // -> even (stable)
        g_odstRenderSkipLastReason.store(r, std::memory_order_relaxed);
    }

    // Called from the hot render hook on a non-FP slot-0 frame. Reads only the
    // already-bounds-checked view object (slot < 4, offsets < viewStride) and
    // never dereferences the nested-source pointer -- it only compares its
    // value. Deduped by modeFlags so full work happens at most once per distinct
    // non-FP camera kind between first-person frames.
    void OdstCaptureNonFpCamera(void* view, uintptr_t arrayOffset)
    {
        const auto& layout = kOdstCameraProfile.layout;
        const char* bytes = static_cast<const char*>(view);
        const char* compact = bytes + layout.rootCurrentCompact;
        const uint32_t modeFlags = *reinterpret_cast<const uint32_t*>(
            compact + layout.compactModeFlags);
        if (g_odstNonFpCameraLastMode.load(std::memory_order_relaxed) ==
            modeFlags)
            return;

        const auto readFloat = [compact](uintptr_t offset) {
            return *reinterpret_cast<const float*>(compact + offset);
        };
        const float* oblique = reinterpret_cast<const float*>(
            compact + layout.obliquePlane);
        const uint32_t customProjection = *reinterpret_cast<const uint32_t*>(
            compact + layout.customProjection);
        const float* customData = reinterpret_cast<const float*>(
            compact + layout.customProjectionData);
        const auto boundsOrdered = [compact](uintptr_t offset) {
            const int16_t* b = reinterpret_cast<const int16_t*>(compact + offset);
            return b[0] < b[2] && b[1] < b[3];
        };
        const float nearClip = readFloat(layout.nearClip);
        const float farClip = readFloat(layout.farClip);
        // Break the redirect gate into its individual sub-checks so a flat
        // cutscene camera reveals the exact disqualifying field, not a lone NO.
        const float verticalFov = readFloat(layout.verticalFov);
        const float referenceFov = readFloat(layout.referenceFov);
        const float verticalOffset = readFloat(layout.verticalOffset);
        const bool modeZero = modeFlags == 0;
        const bool voffZero = verticalOffset == 0.0f;
        const bool fovInRange = isfinite(verticalFov) && verticalFov > 0.0001f &&
            verticalFov < 3.1415f && isfinite(referenceFov) &&
            referenceFov > 0.0001f && referenceFov < 3.1415f;
        const bool obliqueZero = oblique[0] == 0.0f && oblique[1] == 0.0f &&
            oblique[2] == 0.0f && oblique[3] == 0.0f;
        const bool customProjZero = customProjection == 0 &&
            customData[0] == 0.0f && customData[1] == 0.0f &&
            customData[2] == 0.0f && customData[3] == 0.0f;
        const bool boundsOrderedAll = boundsOrdered(layout.windowBounds) &&
            boundsOrdered(layout.renderBounds) &&
            boundsOrdered(layout.activeBounds);
        const bool clipsValid = isfinite(nearClip) && isfinite(farClip) &&
            nearClip > 0.0f && farClip > nearClip;
        const bool plainPerspective = obliqueZero && customProjZero &&
            boundsOrderedAll && clipsValid;

        const uintptr_t nestedSource = *reinterpret_cast<const uintptr_t*>(
            bytes + layout.nestedSourceCamera);
        uint32_t flags = 0;
        if (OdstSingleUserTailIsValid(view)) flags |= OdstNonFpTailValid;
        if (nestedSource ==
            reinterpret_cast<uintptr_t>(view) + layout.rootCurrentCompact)
            flags |= OdstNonFpNestedMatch;
        if (OdstCompactCameraIsActive(compact)) flags |= OdstNonFpActive;
        if (plainPerspective) flags |= OdstNonFpPlainPerspective;
        if (modeZero) flags |= OdstNonFpModeZero;
        if (voffZero) flags |= OdstNonFpVoffZero;
        if (fovInRange) flags |= OdstNonFpFovInRange;
        if (obliqueZero) flags |= OdstNonFpObliqueZero;
        if (customProjZero) flags |= OdstNonFpCustomProjZero;
        if (boundsOrderedAll) flags |= OdstNonFpBoundsOrdered;
        if (clipsValid) flags |= OdstNonFpClipsValid;

        OdstNonFpCameraCapture& cap = g_odstNonFpCameraCapture;
        cap.seq.fetch_add(1, std::memory_order_acq_rel);  // -> odd (writing)
        cap.modeFlags.store(modeFlags, std::memory_order_relaxed);
        cap.arrayOffset.store(static_cast<uint32_t>(arrayOffset),
                              std::memory_order_relaxed);
        cap.flags.store(flags, std::memory_order_relaxed);
        cap.fpBlend.store(readFloat(layout.compactFpBlend),
                          std::memory_order_relaxed);
        cap.verticalOffset.store(readFloat(layout.verticalOffset),
                                 std::memory_order_relaxed);
        cap.verticalFov.store(readFloat(layout.verticalFov),
                              std::memory_order_relaxed);
        cap.referenceFov.store(readFloat(layout.referenceFov),
                               std::memory_order_relaxed);
        cap.nearClip.store(nearClip, std::memory_order_relaxed);
        cap.farClip.store(farClip, std::memory_order_relaxed);
        cap.seq.fetch_add(1, std::memory_order_release);  // -> even (stable)
        g_odstNonFpCameraLastMode.store(modeFlags, std::memory_order_relaxed);
    }

    // Called from the 50 ms worker (NOT a hot hook), so logging is safe here.
    void LogOdstNonFpCameraIfNew()
    {
        OdstNonFpCameraCapture& cap = g_odstNonFpCameraCapture;
        const uint32_t seq = cap.seq.load(std::memory_order_acquire);
        if ((seq & 1u) || seq == 0)
            return; // mid-write or nothing captured yet
        if (seq == g_odstNonFpCameraLoggedSeq.load(std::memory_order_relaxed))
            return; // already logged this capture
        const uint32_t modeFlags = cap.modeFlags.load(std::memory_order_relaxed);
        const uint32_t arrayOffset =
            cap.arrayOffset.load(std::memory_order_relaxed);
        const uint32_t flags = cap.flags.load(std::memory_order_relaxed);
        const float fpBlend = cap.fpBlend.load(std::memory_order_relaxed);
        const float voff = cap.verticalOffset.load(std::memory_order_relaxed);
        const float vfov = cap.verticalFov.load(std::memory_order_relaxed);
        const float rfov = cap.referenceFov.load(std::memory_order_relaxed);
        const float nearClip = cap.nearClip.load(std::memory_order_relaxed);
        const float farClip = cap.farClip.load(std::memory_order_relaxed);
        if (cap.seq.load(std::memory_order_acquire) != seq)
            return; // a newer capture started; re-read next tick
        const bool tail = (flags & OdstNonFpTailValid) != 0;
        const bool nested = (flags & OdstNonFpNestedMatch) != 0;
        const bool active = (flags & OdstNonFpActive) != 0;
        const bool plain = (flags & OdstNonFpPlainPerspective) != 0;
        // Decode the exact gate sub-checks. The redirect actually requires ALL
        // of: tail, nested, active, mode==0, FOV in range, vertical offset 0,
        // oblique zero, custom projection zero, bounds ordered, clips valid.
        // List only the ones that FAILED so a flat cutscene names its cause.
        char why[192];
        why[0] = '\0';
        const auto appendIfFailed = [&](bool passed, const char* label) {
            if (passed)
                return;
            const size_t used = strlen(why);
            _snprintf_s(why + used, sizeof(why) - used, _TRUNCATE, "%s%s",
                        used ? "," : "", label);
        };
        appendIfFailed(tail, "tail");
        appendIfFailed(nested, "nested");
        appendIfFailed(active, "active");
        appendIfFailed((flags & OdstNonFpModeZero) != 0, "mode");
        appendIfFailed((flags & OdstNonFpFovInRange) != 0, "fov");
        appendIfFailed((flags & OdstNonFpVoffZero) != 0, "voff");
        appendIfFailed((flags & OdstNonFpObliqueZero) != 0, "oblique");
        appendIfFailed((flags & OdstNonFpCustomProjZero) != 0, "customProj");
        appendIfFailed((flags & OdstNonFpBoundsOrdered) != 0, "bounds");
        appendIfFailed((flags & OdstNonFpClipsValid) != 0, "clips");
        const bool redirectable = why[0] == '\0';
        char verdict[224];
        if (redirectable)
            _snprintf_s(verdict, sizeof(verdict), _TRUNCATE,
                        "YES (plain slot-0 -- redirect candidate)");
        else
            _snprintf_s(verdict, sizeof(verdict), _TRUNCATE,
                        "NO (render stock; fails: %s)", why);
        LOG("ODST NON-FP CAMERA: mode=0x%08X slot=%u tail=%d nested=%d "
            "active=%d plainPersp=%d blend=%.4f voff=%.4f vfov=%.4f rfov=%.4f "
            "near=%.4f far=%.1f -- stereo-redirectable = %s",
            modeFlags, arrayOffset, tail, nested, active, plain, fpBlend, voff,
            vfov, rfov, nearClip, farClip, verdict);
        g_odstNonFpCameraLoggedSeq.store(seq, std::memory_order_relaxed);
    }

    // Called from the 50 ms worker (NOT a hot hook), so logging is safe here.
    // Emits why a slot-0 render frame was not stereo-redirected in the cases the
    // non-FP capture above cannot see: the core not yet armed, teardown pending,
    // OpenXR unfocused, an unresolved array, or a foreign-slot camera. This is
    // the primary signal for whether the opening ODST cutscene is flat because
    // of arming timing versus a genuinely different camera object.
    void LogOdstRenderSkipIfNew()
    {
        OdstRenderSkipDiag& d = g_odstRenderSkipDiag;
        const uint32_t seq = d.seq.load(std::memory_order_acquire);
        if ((seq & 1u) || seq == 0)
            return; // mid-write or nothing captured yet
        if (seq == g_odstRenderSkipLoggedSeq.load(std::memory_order_relaxed))
            return; // already logged this reason
        const uint32_t reason = d.reason.load(std::memory_order_relaxed);
        const uint32_t arrayOffset =
            d.arrayOffset.load(std::memory_order_relaxed);
        if (d.seq.load(std::memory_order_acquire) != seq)
            return; // a newer publish started; re-read next tick
        const char* name = "unknown";
        switch (static_cast<OdstRenderSkipReason>(reason))
        {
        case OdstRenderSkipReason::None: name = "none(redirected)"; break;
        case OdstRenderSkipReason::NotArmed:
            name = "core not armed (fresh-camera debounce)"; break;
        case OdstRenderSkipReason::TeardownRequested:
            name = "teardown requested"; break;
        case OdstRenderSkipReason::Unfocused:
            name = "OpenXR unfocused (no prepared frame)"; break;
        case OdstRenderSkipReason::ArrayUnavailable:
            name = "camera array unresolved / view below array"; break;
        case OdstRenderSkipReason::NotStrideAligned:
            name = "view offset not slot-aligned"; break;
        case OdstRenderSkipReason::ForeignSlot:
            name = "active camera on a foreign slot (not slot 0)"; break;
        }
        LOG("ODST RENDER SKIP: reason=%s arrayOffset=0x%X -- this frame renders "
            "stock 2D, not stereo", name, arrayOffset);
        g_odstRenderSkipLoggedSeq.store(seq, std::memory_order_relaxed);
    }

    // Called from the 50 ms worker (NOT a hot hook), so logging is safe here.
    // Emits the one-shot FP weapon-layout self-check published by the hot FP
    // interpolation hook: if the hands/gun stay on the head in the headset, this
    // line tells us whether the hook fired and whether the skeleton was accepted.
    void LogOdstFpLayoutSelfCheckIfNew()
    {
        OdstFpLayoutSelfCheck& sc = g_odstFpLayoutSelfCheck;
        const uint32_t key = sc.key.load(std::memory_order_acquire);
        if (key == 0)
            return; // the FP interpolation hook has not seen a weapon slot yet
        if (key == g_odstFpLayoutLoggedKey.load(std::memory_order_relaxed))
            return; // already logged this observation
        const int slot = sc.slot.load(std::memory_order_relaxed);
        const int nodeCount = sc.nodeCount.load(std::memory_order_relaxed);
        const int accepted = sc.accepted.load(std::memory_order_relaxed);
        const int wrist = sc.wrist.load(std::memory_order_relaxed);
        const int cameraControl =
            sc.cameraControl.load(std::memory_order_relaxed);
        if (sc.key.load(std::memory_order_acquire) != key)
            return; // a newer observation started; re-read next tick
        LOG("ODST FP WEAPON SELF-CHECK: interpolation hook fired slot=%d "
            "nodeCount=%d layoutAccepted=%s (need 39..64) wrist=%d "
            "cameraControl=%d -- hands/gun driven by controller = %s",
            slot, nodeCount, accepted ? "YES" : "NO", wrist, cameraControl,
            accepted ? "YES" : "NO (layout rejected -> stock pose on head)");
        g_odstFpLayoutLoggedKey.store(key, std::memory_order_relaxed);
    }

    // Cold-worker report for the bounded phase-level route check. Hot hooks only
    // increment atomics; no logging, COM discovery, allocation, or file I/O.
    void LogOdstNativeHudRouteOnce()
    {
        static bool logged = false;
        if (logged)
            return;
        unsigned completedPhaseScopes = 0, provenOmMatches = 0;
        unsigned exactCopyScopes = 0, copySubstitutions = 0;
        VR_GetNativeHudRouteStats(completedPhaseScopes, provenOmMatches,
                                  exactCopyScopes, copySubstitutions);
        if (completedPhaseScopes < 120)
            return;
        logged = true;
        LOG("ODST NATIVE HUD ROUTE: completedScopes=%u provenOmMatches=%u "
            "exactCopyScopes=%u copySubstitutions=%u -- %s",
            completedPhaseScopes, provenOmMatches, exactCopyScopes,
            copySubstitutions,
            copySubstitutions
                ? "target-1 snapshot sourced from the active eye cache"
                : exactCopyScopes
                    ? "target-1 source identity did not match; stock copy retained"
                    : "exact target-1 copy was not observed; stock path retained");
    }
    __declspec(noinline) void __fastcall OdstRenderViewBody(void* view)
    {
        RenderViewFn original = g_odstCamera.originalRenderView;
        if (!original)
            return;
        // VR not engaged for this title at all (view null, mod disabled, or the
        // user toggled stereo off): pass through silently, no cutscene evidence.
        if (!view || !g_enabled.load(std::memory_order_relaxed) ||
            !VR_IsStereoEnabled())
        {
            original(view);
            return;
        }
        // VR IS engaged but the private core is not presenting. This is the
        // arming-timing signal: if the opening cutscene is flat and one of these
        // fires, the intro renders before the fresh-camera debounce arms the core
        // (NotArmed) or during a teardown.
        const bool teardownRequested =
            g_odstCamera.teardownRequested.load(std::memory_order_acquire);
        if (teardownRequested ||
            !g_odstCamera.armed.load(std::memory_order_acquire))
        {
            PublishOdstRenderSkip(teardownRequested
                    ? OdstRenderSkipReason::TeardownRequested
                    : OdstRenderSkipReason::NotArmed, 0);
            original(view);
            return;
        }

        if (EvaluateOdstStereoFrame(VR_ShouldRenderPreparedFrame()) ==
            OdstStereoFrameAction::RenderStockWithoutCapture)
        {
            // OpenXR deliberately suppresses presentation while the headset is
            // unfocused. Preserve the installed hooks, render the desktop's
            // ordinary view once, and collect no eye-redirect failure evidence.
            PublishOdstRenderSkip(OdstRenderSkipReason::Unfocused, 0);
            original(view);
            return;
        }
        const auto& layout = kOdstCameraProfile.layout;
        const uintptr_t viewAddress = reinterpret_cast<uintptr_t>(view);
        const uintptr_t arrayAddress = g_odstCamera.gunCameraArray;
        if (!arrayAddress || viewAddress < arrayAddress)
        {
            PublishOdstRenderSkip(OdstRenderSkipReason::ArrayUnavailable, 0);
            original(view);
            return;
        }
        const uintptr_t arrayOffset = viewAddress - arrayAddress;
        if (arrayOffset % layout.viewStride != 0 ||
            arrayOffset / layout.viewStride >= 4)
        {
            PublishOdstRenderSkip(OdstRenderSkipReason::NotStrideAligned,
                                  static_cast<uint32_t>(arrayOffset));
            original(view);
            return;
        }
        char* bytes = static_cast<char*>(view);
        char* camera = bytes + layout.rootCurrentCompact;
        // Slot 0's active plain-perspective cameras share the proven camera
        // layout. First-person/vehicles redirect the internal scene color;
        // blend-0 death renders are copied from ODST's direct backbuffer path.
        // Foreign slots and custom projections remain stock. A live render
        // frame is never itself a teardown trigger.
        const bool ownsPrimarySlot = arrayOffset == 0;
        const bool tailValid =
            ownsPrimarySlot && OdstSingleUserTailIsValid(view);
        const bool nestedMatch = ownsPrimarySlot &&
            *reinterpret_cast<const uintptr_t*>(
                bytes + layout.nestedSourceCamera) ==
                viewAddress + layout.rootCurrentCompact;
        const bool redirectable = ownsPrimarySlot &&
            OdstCompactCameraIsStereoRedirectable(camera);
        if (!OdstShouldStereoRedirect(ownsPrimarySlot, tailValid, nestedMatch,
                                      redirectable))
        {
            // Record the non-first-person camera's field layout (log-only) so a
            // future build can extend the redirect to it with ODST evidence.
            // A foreign (non-slot-0) active camera cannot be captured by that
            // slot-0 dump, so note it distinctly -- a cutscene rendered there is
            // why it would be flat.
            if (ownsPrimarySlot)
                OdstCaptureNonFpCamera(view, arrayOffset);
            else
                PublishOdstRenderSkip(OdstRenderSkipReason::ForeignSlot,
                                      static_cast<uint32_t>(arrayOffset));
            original(view);
            return;
        }
        // A redirected frame: allow a later unsupported/custom camera to
        // re-capture fresh diagnostics the next time, and let a later skip
        // reason re-log after a stretch of successful stereo frames.
        g_odstNonFpCameraLastMode.store(kOdstNonFpNoCapture,
                                        std::memory_order_relaxed);
        g_odstRenderSkipLastReason.store(kOdstRenderSkipNone,
                                         std::memory_order_relaxed);
        struct EyeRenderInput
        {
            float position[3]{};
            float orientation[4]{};
            float halfX = 0.0f;
            float halfY = 0.0f;
            OdstHalo3FovMatch fovMatch{};
        } eyeInputs[2];
        bool eyeInputsValid = true;
        for (int eye = 0; eye < 2; ++eye)
        {
            float fov[4]{};
            eyeInputsValid = VR_GetEyeViewOffset(
                eye, eyeInputs[eye].position, eyeInputs[eye].orientation) &&
                VR_GetEyeFov(eye, fov) && eyeInputsValid;
            for (float component : eyeInputs[eye].position)
                eyeInputsValid = isfinite(component) && eyeInputsValid;
            float orientationLength = 0.0f;
            for (float component : eyeInputs[eye].orientation)
            {
                eyeInputsValid = isfinite(component) && eyeInputsValid;
                orientationLength += component * component;
            }
            for (float angle : fov)
                eyeInputsValid = isfinite(angle) && eyeInputsValid;
            eyeInputs[eye].halfX = fmaxf(-fov[0], fov[1]);
            eyeInputs[eye].halfY = fmaxf(fov[2], -fov[3]);
            eyeInputsValid = orientationLength > 0.5f &&
                orientationLength < 1.5f && eyeInputs[eye].halfX > 0.01f &&
                eyeInputs[eye].halfX < 1.55f &&
                eyeInputs[eye].halfY > 0.01f &&
                eyeInputs[eye].halfY < 1.55f &&
                ComputeOdstHalo3FovMatch(
                    eyeInputs[eye].halfX, eyeInputs[eye].halfY,
                    eyeInputs[eye].fovMatch) && eyeInputsValid;
        }
        if (!eyeInputsValid)
        {
            OdstRequestFallback(OdstFallbackReason::EyeRedirectUnavailable);
            original(view);
            return;
        }
        if (!OdstCameraArraySupportsStereoRedirect(arrayAddress))
        {
            // Eye-location calls above are outside the game camera owner. Check
            // the complete four-slot/single-user invariant again, accepting
            // either proven render path, before any camera byte is mutated.
            original(view);
            return;
        }
        alignas(16) unsigned char savedCompact[0x90];
        alignas(16) unsigned char savedDerived[0xC0];
        alignas(16) unsigned char savedSecondaryCompact[0x90];
        alignas(16) unsigned char savedSecondaryDerived[0xC0];
        alignas(16) unsigned char savedNestedCompact[0x90];
        alignas(16) unsigned char savedNestedDerived[0xC0];
        alignas(16) unsigned char savedNestedSecondaryCompact[0x90];
        alignas(16) unsigned char savedNestedSecondaryDerived[0xC0];
        memcpy(savedCompact, camera, layout.compactSize);
        memcpy(savedDerived, bytes + layout.rootCurrentDerived, layout.derivedSize);
        memcpy(savedSecondaryCompact, bytes + layout.rootSecondaryCompact,
               layout.compactSize);
        memcpy(savedSecondaryDerived, bytes + layout.rootSecondaryDerived,
               layout.derivedSize);
        memcpy(savedNestedCompact, bytes + layout.nestedCurrentCompact,
               layout.compactSize);
        memcpy(savedNestedDerived, bytes + layout.nestedCurrentDerived,
               layout.derivedSize);
        memcpy(savedNestedSecondaryCompact,
               bytes + layout.nestedSecondaryCompact, layout.compactSize);
        memcpy(savedNestedSecondaryDerived,
               bytes + layout.nestedSecondaryDerived, layout.derivedSize);

        const float* savedForward = reinterpret_cast<const float*>(
            savedCompact + layout.compactForward);
        const float* savedUp = reinterpret_cast<const float*>(
            savedCompact + layout.compactUp);
        const float savedRight[3] = {
            savedForward[1] * savedUp[2] - savedForward[2] * savedUp[1],
            savedForward[2] * savedUp[0] - savedForward[0] * savedUp[2],
            savedForward[0] * savedUp[1] - savedForward[1] * savedUp[0],
        };

        bool capturesOk = true;
        // Exactly one articulated pose per stereo pair, matching Halo 3. The
        // second eye reprojects the cached center-root solve into its own root.
        g_fpStereoSolveScope = {};
        g_fpStereoSolveScope.armed = true;
        const int firstEye = g_config.right_eye_first ? 1 : 0;
        for (int pass = 0; pass < 2; ++pass)
        {
            const int eye = pass == 0 ? firstEye : 1 - firstEye;
            g_stereoEye.store(eye, std::memory_order_release);
            VR_BeginRasterEye(eye);
            memcpy(camera, savedCompact, layout.compactSize);
            memcpy(bytes + layout.rootCurrentDerived, savedDerived,
                   layout.derivedSize);
            memcpy(bytes + layout.rootSecondaryCompact, savedSecondaryCompact,
                   layout.compactSize);
            memcpy(bytes + layout.rootSecondaryDerived, savedSecondaryDerived,
                   layout.derivedSize);
            memcpy(bytes + layout.nestedCurrentCompact, savedNestedCompact,
                   layout.compactSize);
            memcpy(bytes + layout.nestedCurrentDerived, savedNestedDerived,
                   layout.derivedSize);
            memcpy(bytes + layout.nestedSecondaryCompact,
                   savedNestedSecondaryCompact, layout.compactSize);
            memcpy(bytes + layout.nestedSecondaryDerived,
                   savedNestedSecondaryDerived, layout.derivedSize);

            float* position = reinterpret_cast<float*>(
                camera + layout.compactPosition);
            const float* eyePosition = eyeInputs[eye].position;
            const float* eyeOrientation = eyeInputs[eye].orientation;
            for (int axis = 0; axis < 3; ++axis)
                position[axis] +=
                    (savedRight[axis] * eyePosition[0] +
                     savedUp[axis] * eyePosition[1] -
                     savedForward[axis] * eyePosition[2]) *
                    kOdstWorldUnitsPerMeter;

            const float sinHalf = sqrtf(
                eyeOrientation[0] * eyeOrientation[0] +
                eyeOrientation[1] * eyeOrientation[1] +
                eyeOrientation[2] * eyeOrientation[2]);
            if (sinHalf > 1e-5f)
            {
                float angle = 2.0f * atan2f(sinHalf, eyeOrientation[3]);
                if (angle > 3.14159265f)
                    angle -= 6.2831853f;
                const float axis[3] = {
                    (eyeOrientation[0] / sinHalf) * savedRight[0] +
                        (eyeOrientation[1] / sinHalf) * savedUp[0] -
                        (eyeOrientation[2] / sinHalf) * savedForward[0],
                    (eyeOrientation[0] / sinHalf) * savedRight[1] +
                        (eyeOrientation[1] / sinHalf) * savedUp[1] -
                        (eyeOrientation[2] / sinHalf) * savedForward[1],
                    (eyeOrientation[0] / sinHalf) * savedRight[2] +
                        (eyeOrientation[1] / sinHalf) * savedUp[2] -
                        (eyeOrientation[2] / sinHalf) * savedForward[2],
                };
                const float cosAngle = cosf(angle), sinAngle = sinf(angle);
                RotateAboutAxis(reinterpret_cast<float*>(
                                    camera + layout.compactForward),
                                axis, cosAngle, sinAngle);
                RotateAboutAxis(reinterpret_cast<float*>(
                                    camera + layout.compactUp),
                                axis, cosAngle, sinAngle);
            }

            const float halfX = eyeInputs[eye].halfX;
            const float halfY = eyeInputs[eye].halfY;
            const OdstHalo3FovMatch& fovMatch = eyeInputs[eye].fovMatch;
            *reinterpret_cast<float*>(camera + layout.verticalFov) =
                fovMatch.compactVerticalInput;
            *reinterpret_cast<float*>(camera + layout.referenceFov) =
                fovMatch.compactReferenceInput;

            alignas(16) unsigned char temporary[0x40]{};
            g_odstCamera.buildViewport(camera, temporary);
            g_odstCamera.buildMatrices(
                camera, temporary, bytes + layout.rootCurrentDerived, 0.0f);
            float* projection = reinterpret_cast<float*>(
                bytes + layout.rootCurrentDerived + layout.projectionMatrix);
            projection[0] = fovMatch.projectionX;
            projection[5] = fovMatch.projectionY;
            static std::atomic<unsigned> loggedFovEyes{0};
            const unsigned eyeBit = 1u << eye;
            if (!(loggedFovEyes.fetch_or(
                      eyeBit, std::memory_order_relaxed) & eyeBit))
            {
                LOG("ODST FOV match eye %d: compact pair %.4f/%.4f -> "
                    "final projection %.5f/%.5f (Halo 3 numeric path)",
                    eye, fovMatch.compactVerticalInput,
                    fovMatch.compactReferenceInput,
                    projection[0], projection[5]);
            }
            g_odstRenderHalfFovX[eye].store(halfX, std::memory_order_relaxed);
            g_odstRenderHalfFovY[eye].store(halfY, std::memory_order_relaxed);

            memcpy(bytes + layout.rootSecondaryCompact, camera,
                   layout.compactSize);
            memcpy(bytes + layout.rootSecondaryDerived,
                   bytes + layout.rootCurrentDerived, layout.derivedSize);
            if (g_odstCamera.fpCameraUpload)
                g_odstCamera.fpCameraUpload(
                    bytes + layout.rootSecondaryCompact,
                    bytes + layout.rootSecondaryDerived);
            memcpy(g_odstCamera.eyeCompactCamera, camera, layout.compactSize);
            memcpy(g_odstCamera.eyeDerivedBlock,
                   bytes + layout.rootCurrentDerived, layout.derivedSize);
            g_odstCamera.eyeView.store(view, std::memory_order_release);
            g_odstPreparingEyeHud = true;
            g_odstCamera.prepareView(view, 0);
            g_odstPreparingEyeHud = false;
            original(view);
            g_odstCamera.eyeView.store(nullptr, std::memory_order_release);
            bool captured = VR_CaptureRenderedEye(eye);
            if (!captured)
                captured = VR_CaptureBackbufferEye(eye);
            capturesOk = captured && capturesOk;
            VR_EndRasterEye();
        }

        g_fpStereoSolveScope.armed = false;
        g_stereoEye.store(-1, std::memory_order_release);
        g_odstCamera.eyeView.store(nullptr, std::memory_order_release);
        memcpy(camera, savedCompact, layout.compactSize);
        memcpy(bytes + layout.rootCurrentDerived, savedDerived, layout.derivedSize);
        memcpy(bytes + layout.rootSecondaryCompact, savedSecondaryCompact,
               layout.compactSize);
        memcpy(bytes + layout.rootSecondaryDerived, savedSecondaryDerived,
               layout.derivedSize);
        memcpy(bytes + layout.nestedCurrentCompact, savedNestedCompact,
               layout.compactSize);
        memcpy(bytes + layout.nestedCurrentDerived, savedNestedDerived,
               layout.derivedSize);
        memcpy(bytes + layout.nestedSecondaryCompact,
               savedNestedSecondaryCompact, layout.compactSize);
        memcpy(bytes + layout.nestedSecondaryDerived,
               savedNestedSecondaryDerived, layout.derivedSize);

        if (capturesOk)
            g_odstCamera.captureFailures.store(0, std::memory_order_release);
        else
        {
            // Match Halo 3: a live render capture miss never dismantles the
            // title hooks. ODST can switch to its death renderer while fpBlend
            // still reports first person, so mode-based failure thresholds are
            // not reliable. Preserve the last valid eye pair until either the
            // direct backbuffer capture or normal scene-color capture returns.
            g_odstCamera.captureFailures.fetch_add(
                1, std::memory_order_acq_rel);
        }
    }

    __declspec(noinline) void __fastcall OdstRenderViewHook(void* view)
    {
        g_odstCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        g_odstCamera.activeRenderCallbacks.fetch_add(
            1, std::memory_order_acq_rel);
        OdstRenderViewBody(view);
        g_odstCamera.activeRenderCallbacks.fetch_sub(
            1, std::memory_order_acq_rel);
        g_odstCamera.activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    // Both titles schedule native CHUD during prepareView. Keep ODST's calls in
    // that exact engine-owned scope and order, but bind their active eye cache
    // while the title submits the phase. Flat/shell and unrelated callers remain
    // stock because only this render thread sets g_odstPreparingEyeHud.
    void OdstRenderNativeHudPhase(OdstHudPhaseFn original, int userIndex)
    {
        if (!original)
            return;
        const int eye = g_stereoEye.load(std::memory_order_acquire);
        const bool route = g_odstPreparingEyeHud && eye >= 0 && eye <= 1 &&
            g_odstCamera.eyeView.load(std::memory_order_acquire) != nullptr &&
            g_enabled.load(std::memory_order_relaxed) && VR_IsStereoEnabled() &&
            g_odstCamera.armed.load(std::memory_order_acquire) &&
            !g_odstCamera.teardownRequested.load(std::memory_order_acquire) &&
            VR_ShouldRenderPreparedFrame();
        if (!route || !VR_BeginNativeHudEyeDraw(eye))
        {
            original(userIndex);
            return;
        }
        original(userIndex);
        VR_EndNativeHudEyeDraw();
    }
    __declspec(noinline) void __fastcall OdstHudPhasePrimaryHook(int userIndex)
    {
        g_odstCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        OdstRenderNativeHudPhase(g_odstCamera.originalHudPhasePrimary, userIndex);
        g_odstCamera.activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    __declspec(noinline) void __fastcall OdstHudPhaseSecondaryHook(int userIndex)
    {
        g_odstCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        OdstRenderNativeHudPhase(g_odstCamera.originalHudPhaseSecondary, userIndex);
        g_odstCamera.activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
    }

    __declspec(noinline) uint32_t __fastcall OdstHudTargetCopyHook(
        int sourceTargetId, int destinationTargetId)
    {
        g_odstCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        OdstHudTargetCopyFn original = g_odstCamera.originalHudTargetCopy;
        uint32_t result = 0;
        if (original)
        {
            // Static ODST evidence: secondary CHUD snapshots engine target 1
            // into its title-specific target 0x35 before later target-1 work.
            const bool expectedCopy =
                sourceTargetId == 1 && destinationTargetId == 0x35;
            if (expectedCopy)
                VR_BeginNativeHudTargetCopy();
            result = original(sourceTargetId, destinationTargetId);
            if (expectedCopy)
                VR_EndNativeHudTargetCopy();
        }
        g_odstCamera.activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
        return result;
    }
#endif

    // Byte pattern of the camera-copy function's prologue, with the RIP
    // displacement and the short-jump offset wildcarded. Found by signature so
    // the mod survives MCC updates that shift addresses (per the project rules).
    //   mov [rsp+8],rbx; push rdi; sub rsp,0x30; movaps [rsp+0x20],xmm6;
    //   mov rdi,rdx; mov rbx,rcx; test rdx,rdx; je short ??; movss xmm3,[rip+??]
    const char* kCamCopySig =
        "48 89 5C 24 08 57 48 83 EC 30 0F 29 74 24 20 48 8B FA 48 8B D9 48 85 D2 74 ?? F3 0F 10 1D ?? ?? ?? ??";

    // observer_apply_camera_effect (halo3.dll+0x17DF44 in build 1.3528).
    // This is the dedicated post-observer camera-impulse stage: it reads the
    // computed position/forward/up, composes the active effect transform, and
    // writes those three fields back. Bypassing this one function leaves real
    // observer locomotion and headset leaning untouched while preventing the
    // game from moving the HMD view for recoil or other screen shake.
    const char* kObserverCameraEffectSig =
        "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20 "
        "55 41 56 41 57 48 8D 68 A1 48 81 EC D0 00 00 00 "
        "0F 28 05 ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 0F 28 0D ?? ?? ?? ?? "
        "44 0F 29 40 D8";

    // halo3.dll+0x286A14 in build 1.3528: inner per-view renderer, called by
    // the engine's native view loop with rcx = the prepared view structure.
    const char* kRenderViewSig =
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 8B 3D ?? ?? ?? ?? 48 8B F1 85 FF 0F 84 ?? ?? ?? ??";
    const char* kPrepareViewSig =
        "48 89 5C 24 08 57 48 83 EC 20 83 3D ?? ?? ?? ?? 03 8B FA 48 8B D9 48 89 0D ?? ?? ?? ??";
    const char* kBuildViewportSig =
        "40 53 48 83 EC 30 44 0F BF 49 62 4C 8B D9 4C 8B 41 38 48 8B DA 0F BF 51 50";
    const char* kBuildMatricesSig =
        "48 8B C4 48 89 58 08 48 89 78 10 55 48 8D 68 E8 48 81 EC 10 01 00 00 80 3D ?? ?? ?? ?? 00";
    // Start of the engine function that constructs the 4-slot camera-object
    // array: mov [rsp+8],rbx; push rdi; sub rsp,0x20; lea rbx,[rip+array];
    // mov edi,4; mov rcx,rbx; call ctor; add rbx,0x2820; sub rdi,1; jnz.
    // The lea's RIP displacement is at match+13 and the instruction ends at
    // match+17, so the array (= gun/overlay camera) is match+17+disp32. The
    // 0x2820 stride distinguishes it from an identical builder of another
    // camera array.
    const char* kGunCamRefSig =
        "48 89 5C 24 08 57 48 83 EC 20 48 8D 1D ?? ?? ?? ?? BF 04 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 81 C3 20 28 00 00 48 83 EF 01 75 EB";
    // Bone composition boundaries called by the first-person animator at
    // 0x2C4663/0x2C4633. The render packet copy happens immediately afterward.
    const char* kComposeBonesSig =
        "45 85 C0 0F 8E ?? ?? ?? ?? 48 89 5C 24 08 57 48 83 EC 20 45 8B D0 49 8B F9 4C 8B C9";
    const char* kComposeSpecialBonesSig =
        "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20 55 41 55 41 56 48 8D 68 B8";
    const char* kFpInterpolateSig =
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 33 DB 49 63 E8";
    // Final visible first-person skin palette consumer. Unlike 0x2C13B8
    // (marker/effect packets), this function maps interpolated bones into the
    // actual render palette and receives the exact root as argument 2.
    const char* kFpVisiblePaletteSig =
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 49 8B F0 0F B7 C9 4C 8B F2";
    // First-person camera rebuild (0x279BEC in build 1.3528). Re-copies the FP
    // compact camera from [view+0x2A8] (center pose), forces the viewmodel FOV,
    // derives view+0x1E8, then tail-jumps into the constant uploader. Hooked so
    // each eye render substitutes its own world camera afterwards.
    const char* kFpCameraRebuildSig =
        "48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 50 48 8D 79 08 0F 29 78 E8 F3 0F 10 3D ?? ?? ?? ?? 48 8B F1";
    // The uploader that rebuild tail-jumps into (0x2770F0):
    // fastcall(compactCamera, derivedBlock). The hook calls it again after the
    // substitution so the constants the original already pushed are redone.
    const char* kFpCameraUploadSig =
        "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 48 8D 68 A1 48 81 EC C0 00 00 00 0F 29 70 E8 48 8B FA F3 0F 10 35 ?? ?? ?? ?? 48 8D 55 B7";

#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    const char* kOdstFpInterpolateSig =
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 "
        "41 57 48 83 EC 30 33 DB 49 63 E8 38 1D ?? ?? ?? ?? 4D 8B E1 44 8B FA";
    const char* kOdstFpVisiblePaletteSig =
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 "
        "EC 20 48 8B 05 ?? ?? ?? ?? 49 8B F0 0F B7 C9 4C 8B F2";
    const char* kOdstNativeWeaponIkDecisionSig =
        "40 84 ED 74 05 45 84 FF 75 04 84 DB 74 0F BA 03 00 00 00 "
        "41 0F 28 D9 44 8D 42 FF EB 11";
    // Unique ODST-native wrappers that directly invoke chud_draw_widget.
    const char* kOdstHudPhasePrimarySig =
        "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 "
        "41 54 41 56 41 57 48 83 EC 40 48 63 F9";
    const char* kOdstHudPhaseSecondarySig =
        "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 "
        "48 63 D9 8B CB 0F 29 74 24 40";
    // ODST's engine target copy. The secondary phase calls it only after
    // selecting temporary target ID 0x35, with source ID 1.
    const char* kOdstHudTargetCopySig =
        "48 63 C1 4C 8D 1D ?? ?? ?? ?? 33 C9 4C 63 D2 4C 8D 04 40 "
        "8B C1 49 C1 E0 05 43 F7 04 18 00 04 00 00 74 05 43 8B 44 18 58";

    const CameraRuntimeProfile kOdstCameraProfile = {
        L"halo3odst.dll",
        "Halo 3: ODST private camera core",
        0x68A0F232,
        0x4797000,
        {
            0x90, 0xC0,
            0x00, 0x28, 0x34, 0x5C, 0x60, 0x68, 0x6C,
            0x00, 0x0C, 0x18, 0x24, 0x30,
            0x008, 0x098, 0x158, 0x1E8,
            0x6C8, 0x6D0, 0x760, 0x820, 0x8B0, 0x970,
            0x38, 0x4C, 0x5C, 0x28, 0x2C, 0x34, 0x64, 0x68,
            0x6C, 0x7C, 0x80, 0x78,
            0x27E0, 0x27F0, 0x27F4, 0x27F8, 0x27FC,
            0x2800, 0x2804, 0x2808, 0x280C, 0x2810,
        },
        "48 89 5C 24 08 57 48 83 EC 30 0F 29 74 24 20 48 8B FA 48 8B D9 48 85 D2 0F 84 ?? ?? ?? ?? F3 0F 10 15 ?? ?? ?? ?? B1 01 F3 0F 59 15 ?? ?? ?? ?? F3 0F 5E 15 ?? ?? ?? ??",
        "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 54 41 56 41 57 48 83 EC 40 8B 3D ?? ?? ?? ?? 48 8B F1 85 FF 0F 84 ?? ?? ?? ?? 8B 05 ?? ?? ?? ?? BA 00 00 00 00 0F BA E0 0A",
        "48 89 5C 24 08 57 48 83 EC 20 83 3D ?? ?? ?? ?? 03 8B FA 48 8B D9 48 89 0D ?? ?? ?? ??",
        "40 53 48 83 EC 30 44 0F BF 49 62 4C 8B D9 4C 8B 41 38 48 8B DA 0F BF 51 50",
        "48 8B C4 48 89 58 08 48 89 78 10 55 48 8D 68 E8 48 81 EC 10 01 00 00 80 3D ?? ?? ?? ?? 00",
        "48 8B C4 48 89 58 10 48 89 70 18 57 48 83 EC 60 48 8D 79 08 0F 29 70 E8 F3 0F 10 35 ?? ?? ?? ?? 48 8B D9 0F 29 78 D8 40 8A F2 48 8B 81 A8 02 00 00 B9 80 00 00 00 41 BA 58 01 00 00",
        "48 8B C4 48 89 58 08 55 48 8D 68 A1 48 81 EC C0 00 00 00 0F 29 70 E8 4C 8D 45 F7 0F 29 78 D8 48 8B D9 48 8B C2 48 83 C2 78 48 8B C8 E8 ?? ?? ?? ?? 48 8D 55 F7",
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B D9 40 8A F2 8B 89 FC 27 00 00 E8 ?? ?? ?? ?? 66 83 F8 FF 0F 85 ?? ?? ?? ?? B9 03 00 00 00",
        "39 35 ?? ?? ?? ?? 75 ?? 33 D2 48 8B CF E8 ?? ?? ?? ?? 40 38 35 ?? ?? ?? ?? 75 ?? 40 38 35 ?? ?? ?? ??",
        "48 89 5C 24 08 57 48 83 EC 20 48 8D 1D ?? ?? ?? ?? BF 04 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 81 C3 10 28 00 00 48 83 EF 01 75 ?? 48 8B 5C 24 30",
        "E8 ?? ?? ?? ?? 84 C0 74 ?? B9 03 00 00 00 E8 ?? ?? ?? ?? 84 C0 75 ?? 8B D1 B1 01 E8 ?? ?? ?? ?? C6 05 ?? ?? ?? ?? 01",
    };

    static_assert(sizeof(g_odstCamera.eyeCompactCamera) == 0x90);
    static_assert(sizeof(g_odstCamera.eyeDerivedBlock) == 0xC0);
#endif

    // Native pause state owner. The HaloScript external global named
    // game_paused is only a developer override and does not change when MCC's
    // real pause menu opens. Four alternating live snapshots plus a 2 ms
    // transition trace identified the engine flag written by this unique code
    // path: it changes before MCC's generic game-thread suspension flags on
    // both entry and exit. The final mov's RIP target is the pause byte.
    const char* kNativePauseOwnerSig =
        "E8 ?? ?? ?? ?? 84 C0 74 18 B9 03 00 00 00 "
        "E8 ?? ?? ?? ?? 84 C0 75 0A 8B D1 40 8A CE "
        "E8 ?? ?? ?? ?? 40 88 35 ?? ?? ?? ?? E9 ?? ?? ?? ??";

    // The leaf cinematic_in_progress getter reads TLS+0xA8, then byte +5. The
    // cinematic_set_shot evaluator reads the same TLS index and writes the
    // current scene/shot pair through a TLS shot-state pointer at +4/+8. Halo 3
    // holds that pointer at TLS+0x90; ODST recompiled the identical function
    // with TLS+0xA0. The `mov edx, imm32` that carries that offset is wildcarded
    // here so the one signature matches both titles (verified unique in each),
    // and LocateCinematicState reads the exact per-title offset from it. The
    // getter is byte-identical between Halo 3 and ODST. Requiring both unique
    // signatures to resolve the same TLS-index global proves every offset used
    // by ReadCinematicShot; otherwise automatic shot-facing stays disabled.
    const char* kCinematicInProgressSig =
        "8B 0D ?? ?? ?? ?? 33 D2 65 48 8B 04 25 58 00 00 00 "
        "41 B8 A8 00 00 00 48 8B 04 C8 49 8B 0C 00 48 85 C9 "
        "74 03 8A 51 05 8A C2 C3";
    const char* kCinematicSetShotSig =
        "40 53 48 83 EC 20 8B DA E8 ?? ?? ?? ?? 48 85 C0 74 33 "
        "44 8B 48 04 8B 00 44 8B 05 ?? ?? ?? ?? "
        "65 48 8B 0C 25 58 00 00 00 BA ?? 00 00 00 "
        "4A 8B 0C C1 48 8B 14 0A 8B CB 89 42 04 44 89 4A 08";

    void LocateCinematicState(uintptr_t base, size_t size)
    {
        const uintptr_t getter = sig::Find(base, size, kCinematicInProgressSig);
        const uintptr_t setter = sig::Find(base, size, kCinematicSetShotSig);
        const bool uniqueGetter = getter &&
            !sig::Find(getter + 1, base + size - getter - 1,
                       kCinematicInProgressSig);
        const bool uniqueSetter = setter &&
            !sig::Find(setter + 1, base + size - setter - 1,
                       kCinematicSetShotSig);
        if (!uniqueGetter || !uniqueSetter)
        {
            LOG("cutscene facing: cinematic state signatures missing/ambiguous; "
                "automatic shot yaw disabled");
            return;
        }

        const int32_t getterDisp =
            *reinterpret_cast<const int32_t*>(getter + 2);
        const int32_t setterDisp =
            *reinterpret_cast<const int32_t*>(setter + 0x1B);
        auto* getterIndex =
            reinterpret_cast<uint32_t*>(getter + 6 + getterDisp);
        auto* setterIndex =
            reinterpret_cast<uint32_t*>(setter + 0x1F + setterDisp);
        const uintptr_t getterAddress =
            reinterpret_cast<uintptr_t>(getterIndex);
        const uintptr_t setterAddress =
            reinterpret_cast<uintptr_t>(setterIndex);
        if (getterIndex != setterIndex ||
            getterAddress < base ||
            getterAddress + sizeof(uint32_t) > base + size ||
            setterAddress < base ||
            setterAddress + sizeof(uint32_t) > base + size ||
            *getterIndex >= 256)
        {
            LOG("cutscene facing: cinematic TLS proof failed; "
                "automatic shot yaw disabled");
            return;
        }

        // Read the per-title shot-state TLS offset directly from the setter's
        // `mov edx, imm32` (0x90 Halo 3 / 0xA0 ODST). Reject an out-of-range
        // value rather than trusting a mismatched build.
        const uint32_t shotStateOffset =
            *reinterpret_cast<const uint32_t*>(setter + 0x29);
        if (shotStateOffset < 0x40 || shotStateOffset > 0x400)
        {
            LOG("cutscene facing: shot-state offset 0x%X out of range; "
                "automatic shot yaw disabled", shotStateOffset);
            return;
        }

        g_cinematicShotStateOffset = shotStateOffset;
        g_cinematicTlsIndex = getterIndex;
        LOG("cutscene facing: exact scene/shot state resolved (shot-state "
            "TLS offset 0x%X); yaw will rebase at cinematic cuts",
            shotStateOffset);
    }

    void LocateNativePauseFlag(uintptr_t base, size_t size)
    {
        const uintptr_t hit = sig::Find(base, size, kNativePauseOwnerSig);
        if (!hit || sig::Find(hit + 1, base + size - hit - 1,
                              kNativePauseOwnerSig))
        {
            g_nativePauseFlag = 0;
            g_enginePauseValidated = false;
            LOG("pause state: native pause signature missing or ambiguous; "
                "using transition fallback");
            return;
        }

        // Signature starts at halo3.dll+0xB682 in build 1.3528. The target
        // instruction begins at +33, its disp32 is +36, and RIP after it is
        // +40. Resolve rather than retaining the observed +0xA3CA9A offset.
        const int32_t displacement =
            *reinterpret_cast<const int32_t*>(hit + 36);
        const uintptr_t flag = hit + 40 + displacement;
        if (flag < base || flag >= base + size)
        {
            g_nativePauseFlag = 0;
            g_enginePauseValidated = false;
            LOG("pause state: native pause signature resolved outside halo3.dll; "
                "using transition fallback");
            return;
        }

        const uint8_t initial = *reinterpret_cast<const uint8_t*>(flag);
        if (initial > 1)
        {
            g_nativePauseFlag = 0;
            g_enginePauseValidated = false;
            LOG("pause state: native pause flag failed boolean validation (%u); "
                "using transition fallback", static_cast<unsigned>(initial));
            return;
        }

        g_nativePauseFlag = flag;
        g_enginePauseValidated = true;
        LOG("pause state: authoritative native flag at halo3.dll+0x%llX "
            "(initial=%u, owner signature +0x%llX)",
            (unsigned long long)(flag - base), static_cast<unsigned>(initial),
            (unsigned long long)(hit - base));
    }

    bool ReadEnginePaused(bool& paused)
    {
        const uintptr_t flag = g_nativePauseFlag.load(std::memory_order_acquire);
        if (!flag)
            return false;
        __try
        {
            const uint8_t value = *reinterpret_cast<const uint8_t*>(flag);
            if (value > 1)
                return false;
            paused = value != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool InstallHook(uintptr_t base, size_t size, uint32_t runtimeGeneration)
    {
        if (!runtimeGeneration)
            return false;
        LocateNativePauseFlag(base, size);
        LocateCinematicState(base, size);
        uintptr_t hit = sig::Find(base, size, kCamCopySig);
        if (!hit)
        {
            LOG("M1: camera signature NOT FOUND — MCC may have updated. Head tracking is");
            LOG("M1: disabled; the game and the VR screen still work normally.");
            return false;
        }
        // Uniqueness check — if the pattern matched twice we can't trust it.
        const uintptr_t after = hit + 1;
        if (sig::Find(after, base + size - after, kCamCopySig))
        {
            LOG("M1: camera signature is not unique; leaving stock Halo 3 untouched");
            return false;
        }
        LOG("M1: camera-copy found by signature at halo3.dll+0x%llX (expected 0x%llX for build 1.3528)",
            (unsigned long long)(hit - base), (unsigned long long)kCamCopyRva);

        void* target = reinterpret_cast<void*>(hit);
        g_halo3RuntimeGeneration.store(
            runtimeGeneration, std::memory_order_release);
        g_halo3LastCamCopyMs.store(0, std::memory_order_release);
        // A new module generation must begin disarmed even if the prior title
        // instance or an early manual toggle left the shared flag set.
        g_enabled.store(false, std::memory_order_release);
        g_autoVrOwned.store(false, std::memory_order_release);
        g_autoVrUserVeto.store(false, std::memory_order_release);
        TitleAdapter_ClearHeartbeat(GameTitle::Halo3, runtimeGeneration);
        const MH_STATUS createStatus = MH_CreateHook(
            target, reinterpret_cast<void*>(&CamCopyHook),
            reinterpret_cast<void**>(&g_origCamCopy));
        if (createStatus != MH_OK)
        {
            LOG("M1: FAILED to create camera-copy hook at %p (%d); "
                "head tracking unavailable",
                target, static_cast<int>(createStatus));
            TitleAdapter_ClearHeartbeat(GameTitle::Halo3, runtimeGeneration);
            g_halo3RuntimeGeneration.store(0, std::memory_order_release);
            return false;
        }
        const MH_STATUS enableStatus = MH_EnableHook(target);
        if (enableStatus != MH_OK)
        {
            const MH_STATUS disableStatus = MH_DisableHook(target);
            const MH_STATUS removeStatus = MH_RemoveHook(target);
            LOG("M1: FAILED to enable camera-copy hook at %p (%d); "
                "rollback disable=%d remove=%d",
                target, static_cast<int>(enableStatus),
                static_cast<int>(disableStatus),
                static_cast<int>(removeStatus));
            if (removeStatus == MH_OK || removeStatus == MH_ERROR_NOT_CREATED)
                g_origCamCopy = nullptr;
            TitleAdapter_ClearHeartbeat(GameTitle::Halo3, runtimeGeneration);
            g_halo3RuntimeGeneration.store(0, std::memory_order_release);
            return false;
        }
        if (!PublishHalo3Lifecycle(true, false, false))
        {
            LOG("M1: title generation changed during camera install; rolling back");
            MH_DisableHook(target);
            MH_RemoveHook(target);
            TitleAdapter_ClearHeartbeat(GameTitle::Halo3, runtimeGeneration);
            g_halo3RuntimeGeneration.store(0, std::memory_order_release);
            g_origCamCopy = nullptr;
            return false;
        }
        TitleAdapter_PublishMode(
            GameTitle::Halo3, runtimeGeneration, RuntimeMode::Loading);
        LOG("M1: camera hooked. F2 head tracking, F3 recenter, F6 leaning, F8/F9 pitch trim, F10 screen-follow (yaw/pitch/up flips: F1 menu)");

        RememberInstalledGameHook(target);
        // Comfort invariant: the OpenXR pose, not Halo's authored screen-shake
        // transform, owns the view while head tracking is active. This hook is
        // independent from stereo and fails open to Halo's stock behavior.
        {
            uintptr_t effectHit = sig::Find(base, size, kObserverCameraEffectSig);
            const bool uniqueEffect = effectHit &&
                !sig::Find(effectHit + 1, base + size - effectHit - 1,
                           kObserverCameraEffectSig);
            bool effectHooked = false;
            if (uniqueEffect)
            {
                const MH_STATUS createStatus = MH_CreateHook(
                    reinterpret_cast<void*>(effectHit),
                    reinterpret_cast<void*>(&ObserverCameraEffectHook),
                    reinterpret_cast<void**>(&g_origObserverCameraEffect));
                if (createStatus == MH_OK)
                    effectHooked = MH_EnableHook(
                        reinterpret_cast<void*>(effectHit)) == MH_OK;
                if (effectHooked)
                    RememberInstalledGameHook(reinterpret_cast<void*>(effectHit));
                if (!effectHooked && createStatus == MH_OK)
                {
                    MH_RemoveHook(reinterpret_cast<void*>(effectHit));
                    g_origObserverCameraEffect = nullptr;
                }
            }
            if (effectHooked)
                LOG("VR comfort: native camera recoil/shake suppressed at "
                    "halo3.dll+0x%llX while head tracking is active",
                    (unsigned long long)(effectHit - base));
            else
                LOG("VR comfort: camera-effect signature missing/ambiguous or "
                    "hook failed; native camera recoil/shake remains active");
        }

        uintptr_t composeHit=sig::Find(base,size,kComposeBonesSig);
        uintptr_t specialHit=sig::Find(base,size,kComposeSpecialBonesSig);
        if (composeHit && specialHit &&
            !sig::Find(composeHit+1,base+size-composeHit-1,kComposeBonesSig) &&
            !sig::Find(specialHit+1,base+size-specialHit-1,kComposeSpecialBonesSig))
        {
            // Both compositor callers and the allocator use the engine TLS
            // index global at halo3.dll+0xA39F9C. Resolve it through the proven
            // first-person update signature instead of baking that RVA.
            uintptr_t fp=sig::Find(base,size,
                "48 8B C4 44 88 40 18 89 50 10 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57");
            if (fp)
            {
                const int32_t tlsDisp=*reinterpret_cast<const int32_t*>(fp+0x62);
                g_engineTlsIndex=reinterpret_cast<uint32_t*>(fp+0x66+tlsDisp);
            }
            // composeHit+0x50 is mov rax,[rip+tag-data-base]; its displacement
            // resolves the node-record storage used for names and parents.
            const int32_t tagDataDisp=*reinterpret_cast<const int32_t*>(composeHit+0x53);
            g_animationTagData=reinterpret_cast<unsigned char**>(composeHit+0x57+tagDataDisp);
            if (g_engineTlsIndex && g_animationTagData &&
                MH_CreateHook(reinterpret_cast<void*>(composeHit),reinterpret_cast<void*>(&ComposeBonesHook),
                              reinterpret_cast<void**>(&g_origComposeBones)) == MH_OK &&
                MH_CreateHook(reinterpret_cast<void*>(specialHit),reinterpret_cast<void*>(&ComposeSpecialBonesHook),
                              reinterpret_cast<void**>(&g_origComposeSpecialBones)) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(composeHit)) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(specialHit)) == MH_OK)
                LOG("M3: first-person marker/muzzle matrix hooks installed at halo3.dll+0x%llX/+0x%llX",
                    (unsigned long long)(composeHit-base),(unsigned long long)(specialHit-base));
            else
                LOG("M3: FAILED to hook first-person hand/weapon matrix composers");
        }
        else
            LOG("M3: first-person hand/weapon matrix signatures missing or ambiguous");

        RememberInstalledGameHook(reinterpret_cast<void*>(composeHit));
        RememberInstalledGameHook(reinterpret_cast<void*>(specialHit));
        uintptr_t interpolateHit=sig::Find(base,size,kFpInterpolateSig);
        uintptr_t visiblePaletteHit=sig::Find(base,size,kFpVisiblePaletteSig);
        const bool uniqueInterpolate=interpolateHit &&
            !sig::Find(interpolateHit+1,base+size-interpolateHit-1,kFpInterpolateSig);
        const bool uniqueVisiblePalette=visiblePaletteHit &&
            !sig::Find(visiblePaletteHit+1,base+size-visiblePaletteHit-1,
                       kFpVisiblePaletteSig);
        bool visiblePathOk=false;
        if (uniqueInterpolate && uniqueVisiblePalette)
        {
            const MH_STATUS createInterpolate=MH_CreateHook(
                reinterpret_cast<void*>(interpolateHit),
                reinterpret_cast<void*>(&FpInterpolateHook),
                reinterpret_cast<void**>(&g_origFpInterpolate));
            const MH_STATUS createPalette=MH_CreateHook(
                reinterpret_cast<void*>(visiblePaletteHit),
                reinterpret_cast<void*>(&FpVisiblePaletteHook),
                reinterpret_cast<void**>(&g_origFpVisiblePalette));
            if (createInterpolate==MH_OK && createPalette==MH_OK)
                visiblePathOk=
                    MH_EnableHook(reinterpret_cast<void*>(interpolateHit))==MH_OK &&
                    MH_EnableHook(reinterpret_cast<void*>(visiblePaletteHit))==MH_OK;
            if (!visiblePathOk)
            {
                MH_DisableHook(reinterpret_cast<void*>(interpolateHit));
                MH_DisableHook(reinterpret_cast<void*>(visiblePaletteHit));
                MH_RemoveHook(reinterpret_cast<void*>(interpolateHit));
                MH_RemoveHook(reinterpret_cast<void*>(visiblePaletteHit));
                g_origFpInterpolate=nullptr;
                g_origFpVisiblePalette=nullptr;
            }
        }
        RememberInstalledGameHook(reinterpret_cast<void*>(interpolateHit));
        RememberInstalledGameHook(reinterpret_cast<void*>(visiblePaletteHit));
        if (visiblePathOk)
        {
            g_fpInterpolatorHooked.store(true,std::memory_order_release);
            LOG("M3: visible FP reconstruction hooked at halo3.dll+0x%llX -> +0x%llX "
                "(interpolation identity + final palette root; gun/arms only)",
                (unsigned long long)(interpolateHit-base),
                (unsigned long long)(visiblePaletteHit-base));
        }
        else
            LOG("M3: complete visible FP path missing/ambiguous or hook failed; using sim fallback");

        // Native first-person weapon IK is a flat-screen support-hand system:
        // the animation graph can attach the arm's marker to a marker on the
        // weapon. H3EK proves the shotgun enables exactly
        // left_hand -> left_hand, with the target marker parented to pump node
        // 4; the AR's corresponding weapon-IK block is empty. Our controller
        // solver must own that arm instead. Redirect the one unique decision
        // to Halo's EXISTING no-weapon-IK state (mode 1/3), before any level is
        // evaluated. This is a two-byte startup patch, not a per-frame detour,
        // and preserves all authored fire/reload/melee animation channels.
        {
            const char* kFpNativeWeaponIkDecisionSig =
                "40 84 ED 74 05 45 84 FF 75 04 84 DB 74 0F BA 03 00 00 00 "
                "41 0F 28 D8 44 8D 42 FF EB 11";
            uintptr_t decision=sig::Find(base,size,kFpNativeWeaponIkDecisionSig);
            const bool uniqueDecision=decision &&
                !sig::Find(decision+1,base+size-decision-1,
                           kFpNativeWeaponIkDecisionSig);
            if (uniqueDecision)
            {
                unsigned char* branch=reinterpret_cast<unsigned char*>(decision+3);
                if (branch[0]==0x74 && branch[1]==0x05)
                {
                    DWORD oldProtect=0;
                    if (VirtualProtect(branch,2,PAGE_EXECUTE_READWRITE,&oldProtect))
                    {
                        // JE +5 (conditional native-IK decision) -> JMP +0x18
                        // (the stock no-weapon-IK branch at halo3+0x2C3959).
                        branch[0]=0xEB;
                        branch[1]=0x18;
                        FlushInstructionCache(GetCurrentProcess(),branch,2);
                        DWORD ignored=0;
                        VirtualProtect(branch,2,oldProtect,&ignored);
                        LOG("M3 VRIK: native first-person weapon IK disabled at "
                            "halo3.dll+0x%llX; controller solver owns support arms",
                            (unsigned long long)(decision-base));
                    }
                    else
                        LOG("M3 VRIK: native weapon-IK branch protection failed; "
                            "shotgun support hand may stay attached to pump");
                }
                else
                    LOG("M3 VRIK: native weapon-IK branch bytes changed; patch skipped");
            }
            else
                LOG("M3 VRIK: native weapon-IK decision missing/ambiguous; patch skipped");
        }

        // The CHUD steal-and-requad machinery is GONE (2026-07-18): its three
        // CHUD hooks + draw-call classifier removed the native HUD from both
        // eyes, never displayed the hand quad, and its retry loop cost ~30 fps.
        // The native HUD renders untouched again; only the stock crosshair is
        // suppressed (our floating reticle replaces it).
        uintptr_t fpCamHit=sig::Find(base,size,kFpCameraRebuildSig);
        uintptr_t fpUploadHit=sig::Find(base,size,kFpCameraUploadSig);
        const bool uniqueFpCam=fpCamHit &&
            !sig::Find(fpCamHit+1,base+size-fpCamHit-1,kFpCameraRebuildSig);
        const bool uniqueFpUpload=fpUploadHit &&
            !sig::Find(fpUploadHit+1,base+size-fpUploadHit-1,kFpCameraUploadSig);
        bool fpCameraOk=false;
        if (uniqueFpCam && uniqueFpUpload)
        {
            if (MH_CreateHook(reinterpret_cast<void*>(fpCamHit),
                              reinterpret_cast<void*>(&FpCameraRebuildHook),
                              reinterpret_cast<void**>(&g_origFpCameraRebuild))==MH_OK)
            {
                fpCameraOk=MH_EnableHook(reinterpret_cast<void*>(fpCamHit))==MH_OK;
                if (!fpCameraOk)
                {
                    MH_RemoveHook(reinterpret_cast<void*>(fpCamHit));
                    g_origFpCameraRebuild=nullptr;
                }
            }
        }
        RememberInstalledGameHook(reinterpret_cast<void*>(fpCamHit));
        if (fpCameraOk)
        {
            g_fpCameraUpload=reinterpret_cast<FpCameraUploadFn>(fpUploadHit);
            LOG("M3: FP camera rebuild hooked at halo3.dll+0x%llX (uploader +0x%llX); "
                "gun/HUD layer renders per-eye",
                (unsigned long long)(fpCamHit-base),
                (unsigned long long)(fpUploadHit-base));
        }
        else
            LOG("M3: FP camera rebuild signature missing/ambiguous; gun/HUD stay a mono flat layer");

        // GAME BRIGHTNESS: hook 0x278EE0 (once thought to size the HUD; the
        // headset proved it drives brightness). See HudXformHook — it scales the
        // two screen color/gamma floats by game_brightness. MinHook on the
        // function installs reliably. Prologue verified unique on disk (push rbp;
        // mov rbp,rsp; sub rsp,0x50; save xmm6/xmm7; the movaps arg shuffle).
        {
            const char* kHudXformSig =
                "40 55 48 8B EC 48 83 EC 50 0F 29 74 24 40 0F 28 F1 "
                "0F 29 7C 24 30 0F 28 CA 0F 28 F8";
            uintptr_t hudXform = sig::Find(base, size, kHudXformSig);
            RememberInstalledGameHook(reinterpret_cast<void*>(hudXform));
            if (hudXform && !sig::Find(hudXform+1, base+size-hudXform-1, kHudXformSig))
            {
                if (MH_CreateHook(reinterpret_cast<void*>(hudXform),
                                  reinterpret_cast<void*>(&HudXformHook),
                                  reinterpret_cast<void**>(&g_realHudXform)) == MH_OK &&
                    MH_EnableHook(reinterpret_cast<void*>(hudXform)) == MH_OK)
                    LOG("M3: brightness hook installed at halo3.dll+0x%llX "
                        "(game_brightness)",
                        (unsigned long long)(hudXform - base));
                else
                    LOG("M3: brightness MH_CreateHook failed; brightness fixed at 1.0");
            }
            else
                LOG("M3: brightness signature missing/ambiguous; brightness fixed at 1.0");
        }

        // HUD CROSSHAIR CLASS HIDER. H3EK and ManagedDonkey agree that class 2
        // is the authoritative crosshair marker. chud_draw_widget already checks
        // that class, but game_is_playback short-circuits the check during normal
        // play. NOP only that short-circuit and hook the existing class-gated
        // predicate; no tag-table reads are added to the render hook.
        {
            // True HUD height: translate Halo's computed CHUD anchor basis. The
            // prologue is unique in the current halo3.dll and fails open if MCC
            // changes it. The authored crosshair capture is excluded in the hook.
            const char* kHudAnchorBasisSig =
                "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 20 55 "
                "41 54 41 55 41 56 41 57 48 8D 68 88 48 81 EC 50 01 00 00";
            uintptr_t anchorBasis = sig::Find(base, size, kHudAnchorBasisSig);
            const bool uniqueAnchorBasis = anchorBasis &&
                !sig::Find(anchorBasis + 1, base + size - anchorBasis - 1,
                           kHudAnchorBasisSig);
            bool anchorHookReady = false;
            if (uniqueAnchorBasis)
            {
                const MH_STATUS createStatus = MH_CreateHook(
                    reinterpret_cast<void*>(anchorBasis),
                    reinterpret_cast<void*>(&HudAnchorBasisHook),
                    reinterpret_cast<void**>(&g_realHudAnchorBasis));
                anchorHookReady = createStatus == MH_OK;
                if (anchorHookReady)
                    anchorHookReady = MH_EnableHook(
                        reinterpret_cast<void*>(anchorBasis)) == MH_OK;
                if (anchorHookReady)
                    RememberInstalledGameHook(reinterpret_cast<void*>(anchorBasis));
                else if (createStatus == MH_OK)
                    MH_RemoveHook(reinterpret_cast<void*>(anchorBasis));
            }
            if (anchorHookReady)
                LOG("M3: HUD height anchor hook installed at halo3.dll+0x%llX",
                    (unsigned long long)(anchorBasis - base));
            else
                LOG("M3: HUD height anchor signature missing/ambiguous or hook failed; "
                    "height remains stock");

            const char* kHudElemSig =
                "48 89 5C 24 10 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 41 8A F9 45 0F B7 C0";
            uintptr_t hudElem = sig::Find(base, size, kHudElemSig);
            bool uniqueHudElem = hudElem != 0;
            if (uniqueHudElem)
                uniqueHudElem = sig::Find(hudElem + 1, base + size - hudElem - 1,
                                          kHudElemSig) == 0;
            if (uniqueHudElem)
            {
                const unsigned char expectedClassGate[] = {
                    0x74, 0x17, 0xB8, 0x02, 0x00, 0x00, 0x00,
                    0x66, 0x41, 0x3B, 0x42, 0x04, 0x75, 0x0B,
                    0x8B, 0xCB, 0xE8
                };
                unsigned char* classGate =
                    reinterpret_cast<unsigned char*>(hudElem + 0x84);
                bool layoutMatches =
                    memcmp(classGate, expectedClassGate,
                           sizeof(expectedClassGate)) == 0;
                if (reinterpret_cast<unsigned char*>(hudElem)[0x7D] != 0xE8)
                    layoutMatches = false;
                if (layoutMatches)
                {
                    const uintptr_t playbackTarget =
                        sig::RipTarget(hudElem + 0x7E, hudElem + 0x82);
                    const uintptr_t visibleTarget =
                        sig::RipTarget(hudElem + 0x95, hudElem + 0x99);
                    bool targetsInModule = playbackTarget >= base;
                    if (playbackTarget >= base + size)
                        targetsInModule = false;
                    if (visibleTarget < base)
                        targetsInModule = false;
                    if (visibleTarget >= base + size)
                        targetsInModule = false;
                    if (targetsInModule)
                    {
                        RememberInstalledGameHook(
                            reinterpret_cast<void*>(visibleTarget));
                        RememberInstalledGameHook(
                            reinterpret_cast<void*>(hudElem));
                        g_gameIsPlayback =
                            reinterpret_cast<GameIsPlaybackFn>(playbackTarget);
                        const MH_STATUS createStatus = MH_CreateHook(
                            reinterpret_cast<void*>(visibleTarget),
                            reinterpret_cast<void*>(&HudCrosshairVisibleHook),
                            reinterpret_cast<void**>(&g_realHudCrosshairVisible));
                        bool hookReady = createStatus == MH_OK;
                        if (hookReady)
                            hookReady = MH_EnableHook(
                                reinterpret_cast<void*>(visibleTarget)) == MH_OK;
                        bool drawHookReady = false;
                        if (hookReady)
                        {
                            const MH_STATUS drawCreate = MH_CreateHook(
                                reinterpret_cast<void*>(hudElem),
                                reinterpret_cast<void*>(&HudDrawWidgetHook),
                                reinterpret_cast<void**>(&g_realHudDrawWidget));
                            drawHookReady = drawCreate == MH_OK;
                            if (drawHookReady)
                                drawHookReady = MH_EnableHook(
                                    reinterpret_cast<void*>(hudElem)) == MH_OK;
                            if (!drawHookReady && drawCreate == MH_OK)
                                MH_RemoveHook(reinterpret_cast<void*>(hudElem));
                        }
                        if (hookReady)
                        {
                            DWORD oldProtect = 0;
                            if (VirtualProtect(classGate, 2, PAGE_EXECUTE_READWRITE,
                                               &oldProtect))
                            {
                                classGate[0] = 0x90;
                                classGate[1] = 0x90;
                                FlushInstructionCache(GetCurrentProcess(),
                                                      classGate, 2);
                                DWORD ignored = 0;
                                VirtualProtect(classGate, 2, oldProtect, &ignored);
                                if (drawHookReady)
                                    LOG("M3: authored CHUD crosshair redirect active "
                                        "at halo3.dll+0x%llX",
                                        (unsigned long long)(hudElem - base));
                                else
                                    LOG("M3: CHUD class hider active but authored "
                                        "draw wrapper unavailable; using VR fallback");
                            }
                            else
                            {
                                if (drawHookReady)
                                {
                                    MH_DisableHook(reinterpret_cast<void*>(hudElem));
                                    MH_RemoveHook(reinterpret_cast<void*>(hudElem));
                                }
                                MH_DisableHook(reinterpret_cast<void*>(visibleTarget));
                                MH_RemoveHook(reinterpret_cast<void*>(visibleTarget));
                                LOG("M3: CHUD class gate protection failed; "
                                    "game reticle stays visible");
                            }
                        }
                        else
                        {
                            if (createStatus == MH_OK)
                                MH_RemoveHook(reinterpret_cast<void*>(visibleTarget));
                            LOG("M3: CHUD crosshair predicate hook failed; "
                                "game reticle stays visible");
                        }
                    }
                    else
                        LOG("M3: CHUD class targets outside halo3.dll; "
                            "game reticle stays visible");
                }
                else
                    LOG("M3: CHUD class-gate layout mismatch; "
                        "game reticle stays visible");
            }
            else
                LOG("M3: HUD element signature missing/ambiguous; "
                    "game reticle stays visible");
        }

        // (0x2EEFC8 placement hook removed — measured: no coordinates there.)

        // DO NOT HOOK halo3+0x120DF8. Tried 2026-07-15: it crashes the game on
        // level load, on contact, even as a pure pass-through (proven — the
        // skip range was never armed, the unconditional probe log never
        // printed, and it still died). Surviving the menus proves nothing:
        // halo3.dll's model pipeline does not run there, so the first real call
        // IS the level load. The weapon-lag diagnosis in RE-notes stands; the
        // mechanism for acting on it must not be a detour on this function.

        // FP mesh re-anchor: patch the single root-fetch call inside the object
        // node recomposer (see FpRootShim). lea rdx,[rsp+20]; mov ecx,ebx;
        // call <root>; then the 0x1205AC multiply — unique on disk, verified.
        const char* kFpRootCallSig =
            "48 8D 54 24 20 8B CB E8 ?? ?? ?? ?? 4D 8B C4 48 8D 4C 24 20 49 8B D7 E8";
        uintptr_t callSite = sig::Find(base, size, kFpRootCallSig);
        if (callSite &&
            !sig::Find(callSite+1, base+size-callSite-1, kFpRootCallSig))
        {
            const uintptr_t callInstr = callSite + 7;    // the E8
            const uintptr_t relAt = callInstr + 1;       // 4-byte aligned disp32
            const int32_t origRel = *reinterpret_cast<const int32_t*>(relAt);
            g_realFpRoot = reinterpret_cast<FpRootFn>(callInstr + 5 + origRel);
            // 12-byte trampoline (mov rax, imm64; jmp rax) within rel32 range
            // of the call site, since our DLL may sit >2GB away.
            unsigned char* tramp = nullptr;
            for (uintptr_t probe = callInstr & ~0xFFFFull;
                 probe > callInstr - 0x40000000ull && !tramp; probe -= 0x100000)
                tramp = static_cast<unsigned char*>(VirtualAlloc(
                    reinterpret_cast<void*>(probe), 0x1000,
                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            const intptr_t newRel = tramp
                ? reinterpret_cast<intptr_t>(tramp) - static_cast<intptr_t>(callInstr + 5) : INT64_MAX;
            if (tramp && newRel >= INT32_MIN && newRel <= INT32_MAX && (relAt & 3) == 0)
            {
                tramp[0]=0x48; tramp[1]=0xB8;                       // mov rax, imm64
                *reinterpret_cast<void**>(tramp+2) =
                    reinterpret_cast<void*>(&FpRootShim);
                tramp[10]=0xFF; tramp[11]=0xE0;                     // jmp rax
                DWORD old;
                if (VirtualProtect(reinterpret_cast<void*>(relAt), 4,
                                   PAGE_EXECUTE_READWRITE, &old))
                {
                    InterlockedExchange(reinterpret_cast<volatile LONG*>(relAt),
                                        static_cast<LONG>(newRel));
                    VirtualProtect(reinterpret_cast<void*>(relAt), 4, old, &old);
                    FlushInstructionCache(GetCurrentProcess(),
                                          reinterpret_cast<void*>(callInstr), 5);
                    LOG("M3: FP mesh root call-site patched at halo3.dll+0x%llX "
                        "(real getter halo3.dll+0x%llX; atomic disp32 swap)",
                        (unsigned long long)(callInstr-base),
                        (unsigned long long)(reinterpret_cast<uintptr_t>(g_realFpRoot)-base));
                }
                else
                    LOG("M3: FP mesh call-site VirtualProtect failed; gun stays camera-glued");
            }
            else
                LOG("M3: FP mesh trampoline allocation failed (rel %lld); gun stays camera-glued",
                    (long long)newRel);
        }
        else
            LOG("VRIK: object-root call-site signature missing/ambiguous; A2 probe unavailable");

        // Second call-site patch: the camera pitch/turn rotation applied to
        // every FP bone but camera_control (the head-glue; see the emitted
        // rax-only shim below and the LTCG note at g_fpSkipBounds).
        const char* kSwayCallSig = "44 3B 8F A4 11 00 00 74 0C 49 8B D0 48 8D 4D C8 E8";
        uintptr_t swaySite = sig::Find(base, size, kSwayCallSig);
        if (!g_fpInterpolatorHooked.load() && swaySite &&
            !sig::Find(swaySite+1, base+size-swaySite-1, kSwayCallSig))
        {
            const uintptr_t callInstr = swaySite + 16;   // the E8
            const uintptr_t relAt = callInstr + 1;       // 4-byte aligned disp32
            const int32_t origRel = *reinterpret_cast<const int32_t*>(relAt);
            g_realSwayApply = reinterpret_cast<SwayApplyFn>(callInstr + 5 + origRel);
            unsigned char* tramp = nullptr;
            for (uintptr_t probe = callInstr & ~0xFFFFull;
                 probe > callInstr - 0x40000000ull && !tramp; probe -= 0x100000)
                tramp = static_cast<unsigned char*>(VirtualAlloc(
                    reinterpret_cast<void*>(probe), 0x1000,
                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            const intptr_t newRel = tramp
                ? reinterpret_cast<intptr_t>(tramp) - static_cast<intptr_t>(callInstr + 5) : INT64_MAX;
            if (tramp && newRel >= INT32_MIN && newRel <= INT32_MAX && (relAt & 3) == 0)
            {
                // Hand-assembled shim, clobbers ONLY rax (the caller keeps its
                // loop state in r8/r9/r10 across this call — LTCG contract; a
                // compiled C++ shim here IS the fatal-error bug). Layout:
                //   [0x00] mov rax,[lo0]; cmp rdx,rax; jb +0x0F (-> lo1 test)
                //   [0x0F] mov rax,[hi0]; cmp rdx,rax; jb +0x2A (-> ret)
                //   [0x1E] mov rax,[lo1]; cmp rdx,rax; jb +0x0F (-> tail)
                //   [0x2D] mov rax,[hi1]; cmp rdx,rax; jb +0x0C (-> ret)
                //   [0x3C] mov rax, real; jmp rax
                //   [0x48] ret
                unsigned char shim[0x49];
                int o = 0;
                auto movRaxAbs = [&](volatile uintptr_t* a) {
                    shim[o++]=0x48; shim[o++]=0xA1;         // mov rax, moffs64
                    const void* p = const_cast<uintptr_t*>(a);
                    memcpy(shim+o, &p, 8); o += 8;
                };
                auto cmpJb = [&](unsigned char disp) {
                    shim[o++]=0x48; shim[o++]=0x39; shim[o++]=0xC2; // cmp rdx, rax
                    shim[o++]=0x72; shim[o++]=disp;                 // jb rel8
                };
                movRaxAbs(&g_fpSkipBounds[0]); cmpJb(0x0F);
                movRaxAbs(&g_fpSkipBounds[1]); cmpJb(0x2A);
                movRaxAbs(&g_fpSkipBounds[2]); cmpJb(0x0F);
                movRaxAbs(&g_fpSkipBounds[3]); cmpJb(0x0C);
                shim[o++]=0x48; shim[o++]=0xB8;             // mov rax, imm64
                const void* real = reinterpret_cast<const void*>(g_realSwayApply);
                memcpy(shim+o, &real, 8); o += 8;
                shim[o++]=0xFF; shim[o++]=0xE0;             // jmp rax
                shim[o++]=0xC3;                             // ret (skip path)
                memcpy(tramp, shim, o);
                DWORD old;
                if (VirtualProtect(reinterpret_cast<void*>(relAt), 4,
                                   PAGE_EXECUTE_READWRITE, &old))
                {
                    InterlockedExchange(reinterpret_cast<volatile LONG*>(relAt),
                                        static_cast<LONG>(newRel));
                    VirtualProtect(reinterpret_cast<void*>(relAt), 4, old, &old);
                    FlushInstructionCache(GetCurrentProcess(),
                                          reinterpret_cast<void*>(callInstr), 5);
                    LOG("M3: camera pitch/turn call-site patched at halo3.dll+0x%llX "
                        "(rotator halo3.dll+0x%llX)",
                        (unsigned long long)(callInstr-base),
                        (unsigned long long)(reinterpret_cast<uintptr_t>(g_realSwayApply)-base));
                }
                else
                    LOG("M3: pitch/turn call-site VirtualProtect failed; gun stays head-rotated");
            }
            else
                LOG("M3: pitch/turn trampoline allocation failed; gun stays head-rotated");
        }
        else if (!g_fpInterpolatorHooked.load())
            LOG("M3: pitch/turn call-site signature missing/ambiguous; gun stays head-rotated");

        ResolveMotionBlurVars(base, size);
        ResolveCinematicFovVar(base, size);
        ResolveBodyVars(base, size);
        DumpHudDebugVars(base, size);

        // (CHUD visibility-snapshot hook removed 2026-07-19 evening — its forced
        // byte writes used a disproven offset map and suppressed the HUD. The
        // reticle kill uses Halo's class-gated path inside 0x2EDF24 only.)

        // RECONSTRUCTION Phase 0: hook the engine's FP render driver and
        // resolve the guard global that gates its first in-window call site.
        // Driver prologue (0x2835D4 in 1.3528); guard cmp site (0x28599D).
        const char* kFpDriverSig =
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 55 41 56 41 57 "
            "48 83 EC 20 48 8B D9 40 8A F2 8B 89 F4 27 00 00";
        const char* kFpDriverGuardSig = "44 39 2D ?? ?? ?? ?? 75 0A 33 D2 48 8B CF E8";
        uintptr_t driverHit = sig::Find(base, size, kFpDriverSig);
        uintptr_t guardHit = sig::Find(base, size, kFpDriverGuardSig);
        if (guardHit && !sig::Find(guardHit+1, base+size-guardHit-1, kFpDriverGuardSig))
        {
            const int32_t disp = *reinterpret_cast<const int32_t*>(guardHit + 3);
            g_fpDriverGuard = reinterpret_cast<int32_t*>(guardHit + 7 + disp);
            LOG("P0: FP driver guard global at halo3.dll+0x%llX (value now %d)",
                (unsigned long long)(reinterpret_cast<uintptr_t>(g_fpDriverGuard) - base),
                *g_fpDriverGuard);
        }
        else
            LOG("P0: FP driver guard site missing/ambiguous");
        if (driverHit && !sig::Find(driverHit+1, base+size-driverHit-1, kFpDriverSig))
        {
            if (MH_CreateHook(reinterpret_cast<void*>(driverHit),
                              reinterpret_cast<void*>(&FpDriverHook),
                              reinterpret_cast<void**>(&g_origFpDriver)) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(driverHit)) == MH_OK)
                LOG("P0: FP driver hooked at halo3.dll+0x%llX",
                    (unsigned long long)(driverHit - base));
            else
                LOG("P0: FP driver hook FAILED");
        }
        else
            LOG("P0: FP driver signature missing/ambiguous");

        RememberInstalledGameHook(reinterpret_cast<void*>(driverHit));
        uintptr_t gunRef = sig::Find(base, size, kGunCamRefSig);
        if (gunRef && !sig::Find(gunRef + 1, base + size - gunRef - 1, kGunCamRefSig))
        {
            const int32_t disp = *reinterpret_cast<const int32_t*>(gunRef + 13);
            g_gunCamera = gunRef + 17 + disp;
            LOG("M2: gun/overlay camera at halo3.dll+0x%llX (expected 0x%llX for build 1.3528)",
                (unsigned long long)(g_gunCamera.load() - base),
                (unsigned long long)kGunCamRva);
        }
        else
        {
            LOG("M2: gun-camera signature missing/ambiguous; weapon/HUD will stay oversized in stereo");
        }

        uintptr_t renderHit = sig::Find(base, size, kRenderViewSig);
        uintptr_t prepareHit = sig::Find(base, size, kPrepareViewSig);
        uintptr_t viewportHit = sig::Find(base, size, kBuildViewportSig);
        uintptr_t matricesHit = sig::Find(base, size, kBuildMatricesSig);
        if (!renderHit || sig::Find(renderHit + 1, base + size - renderHit - 1, kRenderViewSig))
        {
            LOG("M2: render-frame signature missing or ambiguous; raw stereo unavailable");
            return true;
        }
        if (!prepareHit || !viewportHit || !matricesHit)
        {
            LOG("M2: derived camera matrix signatures missing; raw stereo unavailable");
            return true;
        }
        g_prepareView = reinterpret_cast<PrepareViewFn>(prepareHit);
        g_buildViewport = reinterpret_cast<BuildViewportFn>(viewportHit);
        g_buildMatrices = reinterpret_cast<BuildMatricesFn>(matricesHit);
        if (MH_CreateHook(reinterpret_cast<void*>(renderHit), reinterpret_cast<void*>(&RenderViewHook),
                          reinterpret_cast<void**>(&g_origRenderView)) != MH_OK ||
            MH_EnableHook(reinterpret_cast<void*>(renderHit)) != MH_OK)
        {
            LOG("M2: FAILED to hook render-frame entry");
            return true;
        }
        RememberInstalledGameHook(reinterpret_cast<void*>(renderHit));
        g_renderHooked = true;
        LOG("M2: inner per-view double-render hook installed at halo3.dll+0x%llX",
            (unsigned long long)(renderHit - base));
        return true;
    }

#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    enum class OdstInstallResult
    {
        Installed,
        CleanupPending,
        NotReady,
        Failed,
    };

    struct OdstResolvedCameraRuntime
    {
        uintptr_t camCopy = 0;
        uintptr_t observerCameraEffect = 0;
        uintptr_t renderView = 0;
        uintptr_t prepareView = 0;
        uintptr_t buildViewport = 0;
        uintptr_t buildMatrices = 0;
        uintptr_t fpCameraRebuild = 0;
        uintptr_t fpCameraUpload = 0;
        uintptr_t fpDriver = 0;
        uintptr_t fpInterpolate = 0;
        uintptr_t fpVisiblePalette = 0;
        uintptr_t nativeWeaponIkDecision = 0;
        uintptr_t fpDriverGuard = 0;
        uintptr_t gunCameraConstructor = 0;
        uintptr_t nativePauseOwner = 0;
        uintptr_t hudPhasePrimary = 0;
        uintptr_t hudPhaseSecondary = 0;
        uintptr_t hudTargetCopy = 0;
        uint8_t* nativePauseFlag = nullptr;
        float* motionBlurScale = nullptr;
        float* motionBlurMax = nullptr;
        uintptr_t gunCameraArray = 0;
    };

    struct OdstStaticPreflightCache
    {
        bool valid = false;
        uintptr_t moduleBase = 0;
        size_t moduleSize = 0;
        OdstResolvedCameraRuntime resolved{};
    };

    OdstStaticPreflightCache g_odstPreflightCache;

    void ClearOdstStaticPreflightCache()
    {
        g_odstPreflightCache = {};
    }

    bool ApplyOdstNativeWeaponIkBypass(uintptr_t decision)
    {
        uint8_t* branch = reinterpret_cast<uint8_t*>(decision + 3);
        if (!decision || branch[0] != 0x74 || branch[1] != 0x05)
        {
            LOG("ODST VRIK: native weapon-IK branch verification failed");
            return false;
        }
        g_odstCamera.nativeWeaponIkBranch = branch;
        DWORD oldProtect = 0;
        if (!VirtualProtect(branch, 2, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            LOG("ODST VRIK: native weapon-IK branch protection failed");
            return false;
        }
        branch[0] = 0xEB;
        branch[1] = 0x18;
        g_odstCamera.nativeWeaponIkPatched = true;
        FlushInstructionCache(GetCurrentProcess(), branch, 2);
        DWORD ignored = 0;
        if (!VirtualProtect(branch, 2, oldProtect, &ignored))
        {
            LOG("ODST VRIK: could not restore weapon-IK page protection");
            return false;
        }
        LOG("ODST VRIK: native support-hand IK bypassed at "
            "halo3odst.dll+0x%llX; shared controller solver owns both arms",
            static_cast<unsigned long long>(
                decision - g_odstCamera.moduleBase));
        return true;
    }

    bool RestoreOdstNativeWeaponIkBypass()
    {
        uint8_t* branch = g_odstCamera.nativeWeaponIkBranch;
        if (!branch)
            return !g_odstCamera.nativeWeaponIkPatched;
        if (branch[0] == 0x74 && branch[1] == 0x05)
        {
            g_odstCamera.nativeWeaponIkBranch = nullptr;
            g_odstCamera.nativeWeaponIkPatched = false;
            return true;
        }
        if (branch[0] != 0xEB || branch[1] != 0x18)
        {
            LOG("ODST VRIK cleanup: weapon-IK branch has unknown bytes");
            return false;
        }
        DWORD oldProtect = 0;
        if (!VirtualProtect(branch, 2, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            LOG("ODST VRIK cleanup: branch protection failed");
            return false;
        }
        branch[0] = 0x74;
        branch[1] = 0x05;
        FlushInstructionCache(GetCurrentProcess(), branch, 2);
        DWORD ignored = 0;
        if (!VirtualProtect(branch, 2, oldProtect, &ignored))
        {
            LOG("ODST VRIK cleanup: page protection restore failed");
            return false;
        }
        g_odstCamera.nativeWeaponIkBranch = nullptr;
        g_odstCamera.nativeWeaponIkPatched = false;
        return true;
    }

    bool RestoreOdstCrosshairClassGate()
    {
        uint8_t* gate = g_odstCamera.crosshairClassGate;
        if (!gate)
            return !g_odstCamera.crosshairClassGatePatched;
        const bool alreadyStock = gate[0] == 0x74 && gate[1] == 0x17;
        if (!alreadyStock && (gate[0] != 0x90 || gate[1] != 0x90))
        {
            LOG("ODST crosshair cleanup: class-gate has unknown bytes");
            return false;
        }

        DWORD currentProtect = 0;
        if (!VirtualProtect(
                gate, 2, PAGE_EXECUTE_READWRITE, &currentProtect))
        {
            LOG("ODST crosshair cleanup: class-gate protection failed");
            return false;
        }
        if (!alreadyStock)
        {
            gate[0] = 0x74;
            gate[1] = 0x17;
            FlushInstructionCache(GetCurrentProcess(), gate, 2);
        }

        // If install patched the bytes but failed to restore their protection,
        // currentProtect is RWX. Retain the original protection explicitly so
        // cleanup still restores the page to its pre-install state.
        const DWORD targetProtect =
            g_odstCamera.crosshairClassGateOriginalProtect
                ? g_odstCamera.crosshairClassGateOriginalProtect
                : currentProtect;
        DWORD ignored = 0;
        if (!VirtualProtect(gate, 2, targetProtect, &ignored))
        {
            LOG("ODST crosshair cleanup: could not restore class-gate protection");
            return false;
        }
        g_odstCamera.crosshairClassGate = nullptr;
        g_odstCamera.crosshairClassGatePatched = false;
        g_odstCamera.crosshairClassGateOriginalProtect = 0;
        return true;
    }

    enum class OdstOptionalHookResult
    {
        StockFallback,
        Installed,
        CleanupRequired,
    };

    // ODST HUD height. Retail ODST and H3ODSTEK independently prove this as
    // chud_compute_anchor_basis: the ABI matches Halo 3 and its final
    // real_matrix4x3 translation writes Y at basis+0x2C. The long entry AOB
    // captures all four arguments; the unique tail AOB independently guards
    // the exact +0x932 output sequence in the supported retail function.
    OdstOptionalHookResult InstallOdstHudHeight(uintptr_t base, size_t size)
    {
        const char* kOdstHudAnchorBasisSig =
            "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 20 55 "
            "41 54 41 55 41 56 41 57 48 8D 68 98 48 81 EC 40 01 00 00 "
            "4C 8B 2D ?? ?? ?? ?? 49 8B F9 0F 29 70 C8 41 8B D8 "
            "0F 29 78 B8 48 8B F2 44 0F 29 40 A8 44 0F 29 48 98 "
            "48 8B 05 ?? ?? ?? ?? 4C 63 E1";
        const char* kOdstHudAnchorBasisTailSig =
            "F2 0F 10 44 24 38 8B 54 24 54 F2 0F 11 7F 1C "
            "89 4F 24 8B 4C 24 40 F2 0F 11 47 28 "
            "F2 44 0F 11 4F 04 F2 44 0F 11 47 10 "
            "89 4F 30 C7 07 00 00 80 3F";

        const uintptr_t anchorBasis =
            sig::Find(base, size, kOdstHudAnchorBasisSig);
        const bool uniqueEntry = anchorBasis &&
            !sig::Find(
                anchorBasis + 1, base + size - anchorBasis - 1,
                kOdstHudAnchorBasisSig);
        const uintptr_t outputTail =
            sig::Find(base, size, kOdstHudAnchorBasisTailSig);
        const bool uniqueTail = outputTail &&
            !sig::Find(
                outputTail + 1, base + size - outputTail - 1,
                kOdstHudAnchorBasisTailSig);
        const bool exactSupportedLayout =
            uniqueEntry && uniqueTail && outputTail > anchorBasis &&
            outputTail - anchorBasis == 0x932;
        if (!uniqueEntry || !exactSupportedLayout)
        {
            LOG("ODST HUD height: anchor-basis entry/output proof missing or "
                "ambiguous; height remains stock");
            return OdstOptionalHookResult::StockFallback;
        }
        if (g_odstCamera.hookTargetCount >=
            _countof(g_odstCamera.hookTargets))
        {
            LOG("ODST HUD height: lifecycle hook capacity exhausted; "
                "height remains stock");
            return OdstOptionalHookResult::StockFallback;
        }

        const MH_STATUS createStatus = MH_CreateHook(
            reinterpret_cast<void*>(anchorBasis),
            reinterpret_cast<void*>(&OdstHudAnchorBasisHook),
            &g_odstCamera.originalHudAnchorBasis);
        if (createStatus != MH_OK)
        {
            g_odstCamera.originalHudAnchorBasis = nullptr;
            LOG("ODST HUD height: hook create failed; height remains stock");
            return OdstOptionalHookResult::StockFallback;
        }

        const size_t slot = g_odstCamera.hookTargetCount++;
        g_odstCamera.hookTargets[slot] =
            reinterpret_cast<void*>(anchorBasis);
        g_odstCamera.hookTrampolines[slot] =
            g_odstCamera.originalHudAnchorBasis;
        const MH_STATUS enableStatus =
            MH_EnableHook(reinterpret_cast<void*>(anchorBasis));
        if (enableStatus != MH_OK)
        {
            // Retain the registered target, trampoline, and title module.
            // Whole-transaction cleanup provides one verified teardown path
            // even if a future MinHook implementation partially enables here.
            LOG("ODST HUD height: hook enable failed (%d); requesting verified "
                "transaction cleanup", static_cast<int>(enableStatus));
            return OdstOptionalHookResult::CleanupRequired;
        }

        LOG("ODST HUD height: title-native anchor hook active at "
            "halo3odst.dll+0x%llX (basis Y +0x2C)",
            static_cast<unsigned long long>(anchorBasis - base));
        return OdstOptionalHookResult::Installed;
    }

    // ODST class-2 CHUD crosshair hider + authored-widget capture. Full parity
    // with Halo 3's InstallHook crosshair path, but every location is ODST-proven
    // (docs/ODST-SIGNATURE-EVIDENCE.md kHudElemSig candidate; disassembly at
    // halo3odst.dll+0x329488). The class-gate block is byte-identical to Halo 3
    // yet shifted -3 bytes inside the recompiled function: playback call E8 at
    // +0x7A, playback short-circuit je (74 17) at +0x81, class-2 predicate call
    // E8 at +0x91. Missing proof fails open to ODST's stock native crosshair.
    OdstOptionalHookResult InstallOdstCrosshairHider(
        uintptr_t base, size_t size)
    {
        const char* kHudElemSigOdst =
            "44 88 4C 24 20 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 8B D9 45 0F B7 "
            "C0 89 4C 24 20 48 8B 0D ?? ?? ?? ?? 48 89 54 24 28 46 8B 4C C0 04 "
            "45 85 C9 75 ?? 33 C0 EB ??";
        uintptr_t hudElem = sig::Find(base, size, kHudElemSigOdst);
        if (!hudElem ||
            sig::Find(hudElem + 1, base + size - hudElem - 1, kHudElemSigOdst))
        {
            LOG("ODST crosshair: chud_draw_widget signature missing/ambiguous; "
                "native crosshair stays visible");
            return OdstOptionalHookResult::StockFallback;
        }

        const unsigned char expectedClassGate[] = {
            0x74, 0x17, 0xB8, 0x02, 0x00, 0x00, 0x00,
            0x66, 0x41, 0x3B, 0x42, 0x04, 0x75, 0x0B,
            0x8B, 0xCB, 0xE8
        };
        unsigned char* fn = reinterpret_cast<unsigned char*>(hudElem);
        unsigned char* classGate = fn + 0x81;
        if (fn[0x7A] != 0xE8 ||
            memcmp(classGate, expectedClassGate, sizeof(expectedClassGate)) != 0)
        {
            LOG("ODST crosshair: class-gate layout mismatch; native crosshair "
                "stays visible");
            return OdstOptionalHookResult::StockFallback;
        }

        const uintptr_t playbackTarget =
            sig::RipTarget(hudElem + 0x7B, hudElem + 0x7F);
        const uintptr_t visibleTarget =
            sig::RipTarget(hudElem + 0x92, hudElem + 0x96);
        if (playbackTarget < base || playbackTarget >= base + size ||
            visibleTarget < base || visibleTarget >= base + size)
        {
            LOG("ODST crosshair: class targets outside halo3odst.dll; native "
                "crosshair stays visible");
            return OdstOptionalHookResult::StockFallback;
        }
        if (g_odstCamera.hookTargetCount + 2 >
            _countof(g_odstCamera.hookTargets))
        {
            LOG("ODST crosshair: lifecycle hook capacity exhausted; native "
                "crosshair stays visible");
            return OdstOptionalHookResult::StockFallback;
        }

        const size_t firstHookSlot = g_odstCamera.hookTargetCount;
        const MH_STATUS visibleCreate = MH_CreateHook(
            reinterpret_cast<void*>(visibleTarget),
            reinterpret_cast<void*>(&OdstHudCrosshairVisibleHook),
            reinterpret_cast<void**>(&g_realHudCrosshairVisible));
        if (visibleCreate != MH_OK)
        {
            g_realHudCrosshairVisible = nullptr;
            LOG("ODST crosshair: visibility predicate hook create failed; "
                "native crosshair stays visible");
            return OdstOptionalHookResult::StockFallback;
        }
        size_t hookSlot = g_odstCamera.hookTargetCount++;
        g_odstCamera.hookTargets[hookSlot] =
            reinterpret_cast<void*>(visibleTarget);
        g_odstCamera.hookTrampolines[hookSlot] =
            reinterpret_cast<void*>(g_realHudCrosshairVisible);

        const MH_STATUS drawCreate = MH_CreateHook(
            reinterpret_cast<void*>(hudElem),
            reinterpret_cast<void*>(&OdstHudDrawWidgetHook),
            reinterpret_cast<void**>(&g_realHudDrawWidget));
        if (drawCreate != MH_OK)
        {
            const MH_STATUS removeStatus =
                MH_RemoveHook(reinterpret_cast<void*>(visibleTarget));
            if (removeStatus == MH_OK ||
                removeStatus == MH_ERROR_NOT_CREATED)
            {
                g_odstCamera.hookTargetCount = firstHookSlot;
                g_odstCamera.hookTargets[firstHookSlot] = nullptr;
                g_odstCamera.hookTrampolines[firstHookSlot] = nullptr;
                g_realHudCrosshairVisible = nullptr;
                g_realHudDrawWidget = nullptr;
                LOG("ODST crosshair: widget hook create failed; native "
                    "crosshair stays visible");
                return OdstOptionalHookResult::StockFallback;
            }
            LOG("ODST crosshair: widget hook create failed and visibility "
                "cleanup failed (%d); requesting verified transaction cleanup",
                static_cast<int>(removeStatus));
            return OdstOptionalHookResult::CleanupRequired;
        }
        hookSlot = g_odstCamera.hookTargetCount++;
        g_odstCamera.hookTargets[hookSlot] =
            reinterpret_cast<void*>(hudElem);
        g_odstCamera.hookTrampolines[hookSlot] =
            reinterpret_cast<void*>(g_realHudDrawWidget);

        DWORD oldProtect = 0;
        if (!VirtualProtect(
                classGate, 2, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            const MH_STATUS drawRemove =
                MH_RemoveHook(reinterpret_cast<void*>(hudElem));
            const MH_STATUS visibleRemove =
                MH_RemoveHook(reinterpret_cast<void*>(visibleTarget));
            const bool removed =
                (drawRemove == MH_OK ||
                 drawRemove == MH_ERROR_NOT_CREATED) &&
                (visibleRemove == MH_OK ||
                 visibleRemove == MH_ERROR_NOT_CREATED);
            if (removed)
            {
                g_odstCamera.hookTargetCount = firstHookSlot;
                g_odstCamera.hookTargets[firstHookSlot] = nullptr;
                g_odstCamera.hookTargets[firstHookSlot + 1] = nullptr;
                g_odstCamera.hookTrampolines[firstHookSlot] = nullptr;
                g_odstCamera.hookTrampolines[firstHookSlot + 1] = nullptr;
                g_realHudDrawWidget = nullptr;
                g_realHudCrosshairVisible = nullptr;
                LOG("ODST crosshair: class-gate protection failed; native "
                    "crosshair stays visible");
                return OdstOptionalHookResult::StockFallback;
            }
            LOG("ODST crosshair: class-gate protection and hook cleanup failed "
                "(%d/%d); requesting verified transaction cleanup",
                static_cast<int>(drawRemove),
                static_cast<int>(visibleRemove));
            return OdstOptionalHookResult::CleanupRequired;
        }

        classGate[0] = 0x90;
        classGate[1] = 0x90;
        FlushInstructionCache(GetCurrentProcess(), classGate, 2);
        g_odstCamera.crosshairClassGate = classGate;
        g_odstCamera.crosshairClassGatePatched = true;
        g_odstCamera.crosshairClassGateOriginalProtect = oldProtect;
        DWORD ignored = 0;
        if (!VirtualProtect(classGate, 2, oldProtect, &ignored))
        {
            LOG("ODST crosshair: class-gate page protection restore failed; "
                "requesting verified transaction cleanup");
            return OdstOptionalHookResult::CleanupRequired;
        }

        g_gameIsPlayback =
            reinterpret_cast<GameIsPlaybackFn>(playbackTarget);
        const MH_STATUS visibleQueue =
            MH_QueueEnableHook(
                reinterpret_cast<void*>(visibleTarget));
        const MH_STATUS drawQueue =
            visibleQueue == MH_OK
                ? MH_QueueEnableHook(reinterpret_cast<void*>(hudElem))
                : MH_UNKNOWN;
        const MH_STATUS applyStatus =
            visibleQueue == MH_OK && drawQueue == MH_OK
                ? MH_ApplyQueued()
                : MH_UNKNOWN;
        if (visibleQueue != MH_OK || drawQueue != MH_OK ||
            applyStatus != MH_OK)
        {
            // MH_ApplyQueued can enable its first target and then fail its
            // second. Preserve every target/trampoline/patch and use the same
            // verified quiesce path as normal title teardown.
            LOG("ODST crosshair: atomic hook enable failed (%d/%d/%d); "
                "requesting verified transaction cleanup",
                static_cast<int>(visibleQueue),
                static_cast<int>(drawQueue),
                static_cast<int>(applyStatus));
            return OdstOptionalHookResult::CleanupRequired;
        }

        LOG("ODST crosshair: authored CHUD crosshair redirect active at "
            "halo3odst.dll+0x%llX (class-2 predicate +0x%llX)",
            static_cast<unsigned long long>(hudElem - base),
            static_cast<unsigned long long>(visibleTarget - base));
        return OdstOptionalHookResult::Installed;
    }

    void ClearOdstCameraPointers()
    {
        g_odstCamera.moduleBase = 0;
        g_odstCamera.moduleSize = 0;
        g_odstCamera.moduleReference = nullptr;
        g_odstCamera.originalCamCopy = nullptr;
        g_odstCamera.originalObserverCameraEffect = nullptr;
        g_odstCamera.originalRenderView = nullptr;
        g_odstCamera.prepareView = nullptr;
        g_odstCamera.buildViewport = nullptr;
        g_odstCamera.buildMatrices = nullptr;
        g_odstCamera.originalFpCameraRebuild = nullptr;
        g_odstCamera.fpCameraUpload = nullptr;
        g_odstCamera.originalFpDriver = nullptr;
        g_odstCamera.originalHudPhasePrimary = nullptr;
        g_odstCamera.originalHudPhaseSecondary = nullptr;
        g_odstCamera.originalHudTargetCopy = nullptr;
        g_odstCamera.originalHudAnchorBasis = nullptr;
        g_odstCamera.originalFpInterpolate = nullptr;
        g_odstCamera.originalFpVisiblePalette = nullptr;
        g_odstCamera.nativeWeaponIkBranch = nullptr;
        g_odstCamera.nativeWeaponIkPatched = false;
        g_odstCamera.crosshairClassGate = nullptr;
        g_odstCamera.crosshairClassGatePatched = false;
        g_odstCamera.crosshairClassGateOriginalProtect = 0;
        // Drop the shared CHUD trampoline pointers so a later title change can
        // never call into an unloaded halo3odst.dll (stale-pointer crash class).
        g_realHudCrosshairVisible = nullptr;
        g_realHudDrawWidget = nullptr;
        g_gameIsPlayback = nullptr;
        g_odstCamera.gunCameraArray = 0;
        g_odstCamera.eyeView.store(nullptr, std::memory_order_release);
        memset(g_odstCamera.eyeCompactCamera, 0,
               sizeof(g_odstCamera.eyeCompactCamera));
        memset(g_odstCamera.eyeDerivedBlock, 0,
               sizeof(g_odstCamera.eyeDerivedBlock));
        for (OdstMotionBlurVar& var : g_odstCamera.motionBlurVars)
            var = {};
        g_odstCamera.motionBlurResolved = false;
        g_odstCamera.motionBlurZeroed = false;
        // Drop the cinematic-FOV debug-var pointer for the same stale-pointer
        // reason as the trampolines above: a later title change must never let
        // UpdateCinematicFovPolicy write into an unloaded halo3odst.dll. Halo 3
        // re-resolves its own pointer in InstallHook when it next owns the game.
        g_reduceCinematicFov.store(nullptr, std::memory_order_release);
        g_reduceCinematicFovApplied.store(false, std::memory_order_release);
        for (void*& target : g_odstCamera.hookTargets)
            target = nullptr;
        for (void*& trampoline : g_odstCamera.hookTrampolines)
            trampoline = nullptr;
        g_odstCamera.hookTargetCount = 0;
        g_odstCamera.renderHookTarget = nullptr;
        g_odstCamera.installedAtMs.store(0, std::memory_order_release);
        g_odstCamera.cameraArrayReady.store(
            false, std::memory_order_release);
    }

    bool ValidateOdstCameraLayout()
    {
        const auto& layout = kOdstCameraProfile.layout;
        const bool frontTiled =
            layout.rootCurrentCompact + layout.compactSize ==
                layout.rootCurrentDerived &&
            layout.rootCurrentDerived + layout.derivedSize ==
                layout.rootSecondaryCompact &&
            layout.rootSecondaryCompact + layout.compactSize ==
                layout.rootSecondaryDerived &&
            layout.rootSecondaryDerived + layout.derivedSize == 0x2A8;
        const bool nestedMatches =
            layout.nestedFpBase + layout.rootCurrentCompact ==
                layout.nestedCurrentCompact &&
            layout.nestedFpBase + layout.rootCurrentDerived ==
                layout.nestedCurrentDerived &&
            layout.nestedFpBase + layout.rootSecondaryCompact ==
                layout.nestedSecondaryCompact &&
            layout.nestedFpBase + layout.rootSecondaryDerived ==
                layout.nestedSecondaryDerived &&
            layout.nestedFpBase + 0x2A8 == layout.nestedSourceCamera;
        const bool boundsFit =
            layout.sourceReferenceFov + sizeof(float) <= 0x90 &&
            layout.compactUp + sizeof(float) * 3 <= layout.compactSize &&
            layout.compactModeFlags + sizeof(uint32_t) <= layout.compactSize &&
            layout.compactFpBlend + sizeof(float) <= layout.compactSize &&
            layout.windowBounds + sizeof(int16_t) * 4 <= layout.compactSize &&
            layout.renderBounds + sizeof(int16_t) * 4 <= layout.compactSize &&
            layout.activeBounds + sizeof(int16_t) * 4 <= layout.compactSize &&
            layout.verticalFov + sizeof(float) <= layout.compactSize &&
            layout.referenceFov + sizeof(float) <= layout.compactSize &&
            layout.verticalOffset + sizeof(float) <= layout.compactSize &&
            layout.nearClip + sizeof(float) <= layout.compactSize &&
            layout.farClip + sizeof(float) <= layout.compactSize &&
            layout.obliquePlane + sizeof(float) * 4 <= layout.compactSize &&
            layout.customProjection + sizeof(uint32_t) <= layout.compactSize &&
            layout.customProjectionData + sizeof(float) * 4 <=
                layout.compactSize &&
            layout.projectionMatrix + sizeof(float) * 16 <= layout.derivedSize &&
            layout.normalizedViewport + sizeof(float) * 4 <= layout.viewStride &&
            layout.constructedViewSlot + sizeof(int32_t) <= layout.viewStride &&
            layout.viewCount + sizeof(int32_t) <= layout.viewStride &&
            layout.additionalContext + sizeof(int32_t) <= layout.viewStride &&
            layout.renderUserIndex + sizeof(int32_t) <= layout.viewStride &&
            layout.initializedZero + sizeof(int32_t) <= layout.viewStride &&
            layout.finalTailBoolean + sizeof(uint8_t) <= layout.viewStride;
        if (layout.compactSize != sizeof(g_odstCamera.eyeCompactCamera) ||
            layout.derivedSize != sizeof(g_odstCamera.eyeDerivedBlock) ||
            layout.viewStride != 0x2810 || !frontTiled || !nestedMatches ||
            !boundsFit)
        {
            LOG("ODST camera preflight: internal layout profile invariant failed");
            return false;
        }
        return true;
    }

    bool ResolveOdstTextRange(uintptr_t base, size_t size,
                              uintptr_t& textBegin, uintptr_t& textEnd,
                              uint32_t& timestamp, size_t& imageSize)
    {
        if (!base || size < sizeof(IMAGE_DOS_HEADER) || size > UINTPTR_MAX - base)
            return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
            static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > size)
            return false;
        const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + ntOffset);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
            return false;
        timestamp = nt->FileHeader.TimeDateStamp;
        imageSize = nt->OptionalHeader.SizeOfImage;
        const size_t sectionTableOffset = ntOffset +
            offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
            nt->FileHeader.SizeOfOptionalHeader;
        if (sectionTableOffset > size || nt->FileHeader.NumberOfSections >
                (size - sectionTableOffset) / sizeof(IMAGE_SECTION_HEADER))
            return false;
        const IMAGE_SECTION_HEADER* sections =
            reinterpret_cast<const IMAGE_SECTION_HEADER*>(base + sectionTableOffset);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            const IMAGE_SECTION_HEADER& section = sections[i];
            if (memcmp(section.Name, ".text", 5) != 0 ||
                !(section.Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;
            const size_t sectionSize =
                section.Misc.VirtualSize > section.SizeOfRawData
                    ? section.Misc.VirtualSize
                    : section.SizeOfRawData;
            const size_t sectionOffset = section.VirtualAddress;
            if (!sectionSize || sectionOffset >= size ||
                sectionSize > size - sectionOffset)
                return false;
            textBegin = base + sectionOffset;
            textEnd = textBegin + sectionSize;
            return true;
        }
        return false;
    }

    bool FindUniqueOdstRole(uintptr_t base, size_t size,
                            uintptr_t textBegin, uintptr_t textEnd,
                            const char* role, const char* pattern,
                            uintptr_t& result)
    {
        result = sig::Find(base, size, pattern);
        const uintptr_t moduleEnd = base + size;
        if (!result || result < textBegin || result >= textEnd)
        {
            LOG("ODST camera preflight: %s signature missing or outside .text", role);
            result = 0;
            return false;
        }
        const uintptr_t next = result + 1;
        if (next < moduleEnd && sig::Find(next, moduleEnd - next, pattern))
        {
            LOG("ODST camera preflight: %s signature is ambiguous", role);
            result = 0;
            return false;
        }
        return true;
    }

    OdstInstallResult PreflightOdstCameraRuntime(
        uintptr_t base, size_t size, OdstResolvedCameraRuntime& resolved)
    {
        if (!ValidateOdstCameraLayout())
            return OdstInstallResult::Failed;

        uintptr_t textBegin = 0, textEnd = 0;
        uint32_t timestamp = 0;
        size_t imageSize = 0;
        if (!ResolveOdstTextRange(
                base, size, textBegin, textEnd, timestamp, imageSize))
        {
            LOG("ODST camera preflight: invalid loaded PE image");
            return OdstInstallResult::Failed;
        }
        if (timestamp != kOdstCameraProfile.expectedTimestamp ||
            imageSize != kOdstCameraProfile.expectedImageSize || imageSize != size)
        {
            LOG("ODST camera preflight: module identity mismatch "
                "(timestamp 0x%08X, image 0x%zX; expected 0x%08X/0x%zX)",
                timestamp, imageSize, kOdstCameraProfile.expectedTimestamp,
                kOdstCameraProfile.expectedImageSize);
            return OdstInstallResult::Failed;
        }

        const uintptr_t moduleEnd = base + size;
        const size_t arraySize = 4 * kOdstCameraProfile.layout.viewStride;
        const bool cachedStatic = g_odstPreflightCache.valid &&
            g_odstPreflightCache.moduleBase == base &&
            g_odstPreflightCache.moduleSize == size;
        if (cachedStatic)
            resolved = g_odstPreflightCache.resolved;
        else
        {
            ClearOdstStaticPreflightCache();
            bool signaturesOk = true;
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd, "camera copy",
                kOdstCameraProfile.camCopyPattern, resolved.camCopy);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd,
                "post-observer camera effect",
                kObserverCameraEffectSig, resolved.observerCameraEffect);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd, "inner view renderer",
                kOdstCameraProfile.renderViewPattern, resolved.renderView);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd, "prepare view",
                kOdstCameraProfile.prepareViewPattern, resolved.prepareView);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd, "viewport builder",
                kOdstCameraProfile.buildViewportPattern, resolved.buildViewport);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd, "matrix builder",
                kOdstCameraProfile.buildMatricesPattern, resolved.buildMatrices);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd, "FP camera rebuild",
                kOdstCameraProfile.fpCameraRebuildPattern,
                resolved.fpCameraRebuild);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd, "FP camera uploader",
                kOdstCameraProfile.fpCameraUploadPattern,
                resolved.fpCameraUpload);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd, "FP driver",
                kOdstCameraProfile.fpDriverPattern, resolved.fpDriver);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd, "FP interpolation",
                kOdstFpInterpolateSig, resolved.fpInterpolate);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd, "FP visible palette",
                kOdstFpVisiblePaletteSig, resolved.fpVisiblePalette);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd,
                "native FP weapon-IK decision",
                kOdstNativeWeaponIkDecisionSig,
                resolved.nativeWeaponIkDecision);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd, "FP driver guard",
                kOdstCameraProfile.fpDriverGuardPattern,
                resolved.fpDriverGuard);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd,
                "four-slot camera constructor",
                kOdstCameraProfile.gunCameraConstructorPattern,
                resolved.gunCameraConstructor);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd,
                "native pause owner",
                kOdstCameraProfile.nativePauseOwnerPattern,
                resolved.nativePauseOwner);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd, "native CHUD phase primary",
                kOdstHudPhasePrimarySig, resolved.hudPhasePrimary);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd, "native CHUD phase secondary",
                kOdstHudPhaseSecondarySig, resolved.hudPhaseSecondary);
            signaturesOk &= FindUniqueOdstRole(
                base, size, textBegin, textEnd,
                "native CHUD target copy", kOdstHudTargetCopySig,
                resolved.hudTargetCopy);
            resolved.motionBlurScale = FindDebugVarFloat(
                base, size, "motion_blur_scale");
            resolved.motionBlurMax = FindDebugVarFloat(
                base, size, "motion_blur_max");
            const bool motionBlurOk = resolved.motionBlurScale &&
                resolved.motionBlurMax &&
                resolved.motionBlurScale != resolved.motionBlurMax &&
                reinterpret_cast<uintptr_t>(resolved.motionBlurScale) >= base &&
                reinterpret_cast<uintptr_t>(resolved.motionBlurScale) +
                    sizeof(float) <= moduleEnd &&
                reinterpret_cast<uintptr_t>(resolved.motionBlurMax) >= base &&
                reinterpret_cast<uintptr_t>(resolved.motionBlurMax) +
                    sizeof(float) <= moduleEnd;
            if (!motionBlurOk)
                LOG("ODST camera preflight: title-native motion-blur scale/max vars unavailable");

            bool nativePauseOk = false;
            if (resolved.nativePauseOwner)
            {
                // The unique ODST owner signature starts at +0xBCA5. Its final
                // instruction is C6 05 disp32 01 at +0x20 and writes the native
                // pause byte used throughout halo3odst.dll.
                const uintptr_t write = resolved.nativePauseOwner + 0x20;
                if (write + 7 <= moduleEnd &&
                    *reinterpret_cast<const uint8_t*>(write) == 0xC6 &&
                    *reinterpret_cast<const uint8_t*>(write + 1) == 0x05 &&
                    *reinterpret_cast<const uint8_t*>(write + 6) == 0x01)
                {
                    const int32_t displacement =
                        *reinterpret_cast<const int32_t*>(write + 2);
                    const uintptr_t flag = write + 7 + displacement;
                    if (flag >= base && flag < moduleEnd)
                    {
                        const uint8_t initial =
                            *reinterpret_cast<const uint8_t*>(flag);
                        if (initial <= 1)
                        {
                            resolved.nativePauseFlag =
                                reinterpret_cast<uint8_t*>(flag);
                            nativePauseOk = true;
                        }
                    }
                }
            }
            if (!nativePauseOk)
                LOG("ODST camera preflight: native pause owner/flag proof failed");
            if (!signaturesOk || !motionBlurOk || !nativePauseOk)
                return OdstInstallResult::Failed;

            const uintptr_t guardCall = resolved.fpDriverGuard + 13;
            if (*reinterpret_cast<const uint8_t*>(guardCall) != 0xE8)
            {
                LOG("ODST camera preflight: FP guard call opcode changed");
                return OdstInstallResult::Failed;
            }
            const int32_t guardDisplacement =
                *reinterpret_cast<const int32_t*>(guardCall + 1);
            const uintptr_t guardTarget = guardCall + 5 + guardDisplacement;
            if (guardTarget != resolved.fpDriver)
            {
                LOG("ODST camera preflight: FP guard does not call the resolved driver");
                return OdstInstallResult::Failed;
            }

            const uintptr_t constructor = resolved.gunCameraConstructor;
            const int32_t arrayDisplacement =
                *reinterpret_cast<const int32_t*>(constructor + 13);
            resolved.gunCameraArray = constructor + 17 + arrayDisplacement;
            if (resolved.gunCameraArray < base ||
                resolved.gunCameraArray > moduleEnd ||
                arraySize > moduleEnd - resolved.gunCameraArray)
            {
                LOG("ODST camera preflight: four-slot camera array is outside "
                    "halo3odst.dll");
                return OdstInstallResult::Failed;
            }
            g_odstPreflightCache.valid = true;
            g_odstPreflightCache.moduleBase = base;
            g_odstPreflightCache.moduleSize = size;
            g_odstPreflightCache.resolved = resolved;
        }

        bool allConstructed = true;
        for (size_t slot = 0; slot < 4; ++slot)
        {
            const uintptr_t slotAddress = resolved.gunCameraArray +
                slot * kOdstCameraProfile.layout.viewStride;
            const uintptr_t vtable =
                *reinterpret_cast<const uintptr_t*>(slotAddress);
            if (!vtable)
                allConstructed = false;
            else if (vtable < base || vtable >= moduleEnd)
            {
                LOG("ODST camera preflight: camera slot %zu vtable is outside module",
                    slot);
                return OdstInstallResult::Failed;
            }
        }
        if (!allConstructed)
        {
            if (!g_odstCamera.waitingLogged.exchange(true))
                LOG("ODST camera preflight passed; waiting for the four-slot camera array to construct");
            return OdstInstallResult::NotReady;
        }

        const auto& layout = kOdstCameraProfile.layout;
        if (!OdstCameraArraySupportsBringup(resolved.gunCameraArray))
        {
            if (!g_odstCamera.waitingLogged.exchange(true))
            {
                LOG("ODST camera preflight passed; waiting for the proven "
                    "slot-0/user-0 ordinary camera mode");
                LogOdstCameraReadiness(resolved.gunCameraArray);
            }
            return OdstInstallResult::NotReady;
        }

        g_odstCamera.waitingLogged.store(false, std::memory_order_release);
        LOG("ODST camera preflight: profile '%s', PE timestamp 0x%08X, "
            "image 0x%zX, compact 0x%zX, derived 0x%zX, stride 0x%zX",
            kOdstCameraProfile.displayName, timestamp, imageSize,
            layout.compactSize, layout.derivedSize, layout.viewStride);
        LOG("ODST camera RVAs: copy=%llX render=%llX prepare=%llX "
            "viewport=%llX matrices=%llX",
            static_cast<unsigned long long>(resolved.camCopy - base),
            static_cast<unsigned long long>(resolved.renderView - base),
            static_cast<unsigned long long>(resolved.prepareView - base),
            static_cast<unsigned long long>(resolved.buildViewport - base),
            static_cast<unsigned long long>(resolved.buildMatrices - base));
        LOG("ODST camera RVAs: fpRebuild=%llX fpUpload=%llX fpDriver=%llX "
            "fpGuard=%llX constructor=%llX array=%llX",
            static_cast<unsigned long long>(resolved.fpCameraRebuild - base),
            static_cast<unsigned long long>(resolved.fpCameraUpload - base),
            static_cast<unsigned long long>(resolved.fpDriver - base),
            static_cast<unsigned long long>(resolved.fpDriverGuard - base),
            static_cast<unsigned long long>(resolved.gunCameraConstructor - base),
            static_cast<unsigned long long>(resolved.gunCameraArray - base));
        LOG("ODST pause evidence: owner=%llX flag=%llX (initial=%u)",
            static_cast<unsigned long long>(resolved.nativePauseOwner - base),
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(resolved.nativePauseFlag) - base),
            static_cast<unsigned>(*resolved.nativePauseFlag));
        LOG("ODST comfort evidence: observerEffect=%llX blurScale=%llX "
            "blurMax=%llX (stock %.4f/%.4f)",
            static_cast<unsigned long long>(
                resolved.observerCameraEffect - base),
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(resolved.motionBlurScale) - base),
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(resolved.motionBlurMax) - base),
            *resolved.motionBlurScale, *resolved.motionBlurMax);
        return OdstInstallResult::Installed;
    }

    bool OdstDisableStatusIsSafe(MH_STATUS status)
    {
        return status == MH_OK || status == MH_ERROR_DISABLED ||
            status == MH_ERROR_NOT_CREATED;
    }

    struct OdstCodeRange
    {
        DWORD64 begin = 0;
        DWORD64 end = 0;
    };

    struct OdstFrozenThread
    {
        HANDLE handle = nullptr;
        DWORD threadId = 0;
        bool suspended = false;
    };

    class OdstThreadFreeze
    {
    public:
        ~OdstThreadFreeze() { Release(); }

        bool Capture()
        {
            if (!m_threads.empty())
                return false;

            const DWORD processId = GetCurrentProcessId();
            const DWORD currentThreadId = GetCurrentThreadId();
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return false;

            bool enumerateOk = true;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (!Thread32First(snapshot, &entry))
                enumerateOk = false;
            while (enumerateOk)
            {
                if (entry.th32OwnerProcessID == processId &&
                    entry.th32ThreadID != currentThreadId)
                {
                    HANDLE thread = OpenThread(
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                            THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                        FALSE, entry.th32ThreadID);
                    if (!thread)
                    {
                        if (GetLastError() != ERROR_INVALID_PARAMETER)
                            enumerateOk = false;
                    }
                    else
                    {
                        m_threads.push_back(
                            {thread, entry.th32ThreadID, false});
                    }
                }
                if (!Thread32Next(snapshot, &entry))
                {
                    if (GetLastError() != ERROR_NO_MORE_FILES)
                        enumerateOk = false;
                    break;
                }
            }
            CloseHandle(snapshot);
            if (!enumerateOk)
            {
                Release();
                return false;
            }

            // No heap work occurs after the first suspension. Holding every
            // captured thread at once makes the subsequent RIP/counter check a
            // single process-wide quiescence snapshot instead of a sequence of
            // observations that can race one another.
            for (OdstFrozenThread& thread : m_threads)
            {
                const DWORD previousCount = SuspendThread(thread.handle);
                if (previousCount == static_cast<DWORD>(-1))
                {
                    if (WaitForSingleObject(thread.handle, 0) != WAIT_OBJECT_0)
                    {
                        Release();
                        return false;
                    }
                    continue;
                }
                thread.suspended = true;
            }

            // A captured thread can create another thread before it is
            // suspended. Re-enumerate only after the captured set is frozen;
            // an unseen live thread aborts this attempt and is captured on the
            // next retry. Thread IDs cannot be reused while these handles live.
            if (!FrozenSnapshotIsComplete(processId, currentThreadId))
            {
                Release();
                return false;
            }
            return true;
        }

        bool Release()
        {
            bool ok = true;
            for (size_t i = m_threads.size(); i > 0; --i)
            {
                OdstFrozenThread& thread = m_threads[i - 1];
                if (thread.suspended &&
                    ResumeThread(thread.handle) == static_cast<DWORD>(-1) &&
                    WaitForSingleObject(thread.handle, 0) != WAIT_OBJECT_0)
                {
                    ok = false;
                }
                CloseHandle(thread.handle);
            }
            m_threads.clear();
            return ok;
        }

        const std::vector<OdstFrozenThread>& Threads() const
        {
            return m_threads;
        }

    private:
        bool FrozenSnapshotIsComplete(
            DWORD processId, DWORD currentThreadId) const
        {
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return false;

            bool complete = true;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (!Thread32First(snapshot, &entry))
                complete = false;
            while (complete)
            {
                if (entry.th32OwnerProcessID == processId &&
                    entry.th32ThreadID != currentThreadId)
                {
                    bool foundFrozen = false;
                    for (const OdstFrozenThread& thread : m_threads)
                    {
                        if (thread.threadId == entry.th32ThreadID)
                        {
                            foundFrozen = thread.suspended ||
                                WaitForSingleObject(thread.handle, 0) ==
                                    WAIT_OBJECT_0;
                            break;
                        }
                    }
                    if (!foundFrozen)
                        complete = false;
                }
                if (!complete || !Thread32Next(snapshot, &entry))
                {
                    if (complete && GetLastError() != ERROR_NO_MORE_FILES)
                        complete = false;
                    break;
                }
            }
            CloseHandle(snapshot);
            return complete;
        }

        std::vector<OdstFrozenThread> m_threads;
    };

    bool ResolveOdstCodeRange(const void* function, OdstCodeRange& range)
    {
        DWORD64 imageBase = 0;
        const PRUNTIME_FUNCTION runtimeFunction = RtlLookupFunctionEntry(
            reinterpret_cast<DWORD64>(function), &imageBase, nullptr);
        if (!runtimeFunction || runtimeFunction->EndAddress <=
                runtimeFunction->BeginAddress)
            return false;
        range.begin = imageBase + runtimeFunction->BeginAddress;
        range.end = imageBase + runtimeFunction->EndAddress;
        return true;
    }

    void* OdstTrampolineForHook(size_t index)
    {
        if (index >= g_odstCamera.hookTargetCount ||
            index >= _countof(g_odstCamera.hookTrampolines))
            return nullptr;
        return g_odstCamera.hookTrampolines[index];
    }

    bool ScanForOdstDetourIngress(std::atomic<int>& counter,
                                  bool renderOnly, bool& busy)
    {
        static bool rangesResolved = false;
        static OdstCodeRange ranges[13]{};
        if (!rangesResolved)
        {
            const void* functions[] = {
                reinterpret_cast<const void*>(&OdstCamCopyHook),
                reinterpret_cast<const void*>(&OdstRenderViewHook),
                reinterpret_cast<const void*>(&OdstFpCameraRebuildHook),
                reinterpret_cast<const void*>(&OdstFpDriverHook),
                reinterpret_cast<const void*>(&OdstObserverCameraEffectHook),
                reinterpret_cast<const void*>(&OdstFpInterpolateWeaponHook),
                reinterpret_cast<const void*>(&OdstFpVisiblePaletteWeaponHook),
                reinterpret_cast<const void*>(&OdstHudPhasePrimaryHook),
                reinterpret_cast<const void*>(&OdstHudPhaseSecondaryHook),
                reinterpret_cast<const void*>(&OdstHudTargetCopyHook),
                reinterpret_cast<const void*>(&OdstHudAnchorBasisHook),
                reinterpret_cast<const void*>(&OdstHudCrosshairVisibleHook),
                reinterpret_cast<const void*>(&OdstHudDrawWidgetHook),
            };
            static_assert(_countof(functions) == _countof(ranges));
            bool resolved = true;
            for (size_t i = 0; i < _countof(functions); ++i)
                resolved &= ResolveOdstCodeRange(functions[i], ranges[i]);
            if (!resolved)
            {
                LOG("ODST camera cleanup: could not resolve the complete ODST detour unwind ranges");
                return false;
            }
            rangesResolved = true;
        }

        OdstThreadFreeze frozenThreads;
        if (!frozenThreads.Capture())
            return false;

        busy = false;
        bool scanOk = true;
        for (const OdstFrozenThread& thread : frozenThreads.Threads())
        {
            if (!thread.suspended)
                continue;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(thread.handle, &context))
            {
                if (WaitForSingleObject(thread.handle, 0) != WAIT_OBJECT_0)
                    scanOk = false;
                continue;
            }

            const DWORD64 instruction = context.Rip;
            const size_t first = renderOnly ? 1 : 0;
            const size_t last = renderOnly ? 2 : _countof(ranges);
            for (size_t i = first; i < last; ++i)
            {
                if (instruction >= ranges[i].begin &&
                    instruction < ranges[i].end)
                {
                    busy = true;
                    break;
                }
            }

            // MinHook v1.3.4 x64 stores a hook's trampoline and relay in the
            // same 64-byte memory slot, whose base is the ppOriginal value.
            // A thread can be preempted in that relay before it reaches the
            // detour's counter, so the complete slot is part of ingress.
            for (size_t i = first;
                 !busy && i < last && i < g_odstCamera.hookTargetCount; ++i)
            {
                if (!g_odstCamera.hookTargets[i])
                    continue;
                const DWORD64 trampoline = reinterpret_cast<DWORD64>(
                    OdstTrampolineForHook(i));
                if (!trampoline)
                {
                    scanOk = false;
                    continue;
                }
                if (instruction >= trampoline && instruction < trampoline + 64)
                    busy = true;
            }
        }
        if (counter.load(std::memory_order_acquire) != 0)
            busy = true;

        // All observed threads remain suspended through the RIP and counter
        // checks. Once hooks are disabled, an all-clear snapshot is stable:
        // after resume there is no patched target that can enter a relay.
        const bool resumed = frozenThreads.Release();
        return scanOk && resumed;
    }

    bool WaitForOdstCallbacks(std::atomic<int>& counter, const char* role,
                              bool renderOnly)
    {
        for (unsigned waited = 0; waited < 2000; ++waited)
        {
            if (counter.load(std::memory_order_acquire) == 0)
            {
                bool ingressBusy = false;
                if (!ScanForOdstDetourIngress(counter, renderOnly, ingressBusy))
                {
                    LOG("ODST camera cleanup: %s ingress scan failed", role);
                    return false;
                }
                if (!ingressBusy &&
                    counter.load(std::memory_order_acquire) == 0)
                    return true;
            }
            Sleep(1);
        }
        LOG("ODST camera cleanup: %s callbacks did not reach verified quiescence",
            role);
        return false;
    }

    bool DisableAndRemoveOdstHooks()
    {
        // Stop new outer stereo transactions first. Existing ones retain all FP
        // dependencies until their complete two-eye callback has returned.
        if (g_odstCamera.renderHookTarget)
        {
            const MH_STATUS status =
                MH_DisableHook(g_odstCamera.renderHookTarget);
            if (!OdstDisableStatusIsSafe(status))
            {
                LOG("ODST camera cleanup: render disable failed for %p (%d)",
                    g_odstCamera.renderHookTarget, static_cast<int>(status));
                return false;
            }
        }
        if (!WaitForOdstCallbacks(
                g_odstCamera.activeRenderCallbacks, "outer-render", true))
            return false;

        g_odstCamera.armed.store(false, std::memory_order_release);
        PublishOdstLifecycle();
        g_odstCamera.eyeView.store(nullptr, std::memory_order_release);
        g_stereoEye.store(-1, std::memory_order_release);

        bool disabledAll = true;
        for (size_t i = 0; i < g_odstCamera.hookTargetCount; ++i)
        {
            void* target = g_odstCamera.hookTargets[i];
            if (!target || target == g_odstCamera.renderHookTarget)
                continue;
            const MH_STATUS status = MH_DisableHook(target);
            if (!OdstDisableStatusIsSafe(status))
            {
                disabledAll = false;
                LOG("ODST camera cleanup: disable failed for %p (%d)",
                    target, static_cast<int>(status));
            }
        }
        if (!disabledAll ||
            !WaitForOdstCallbacks(
                g_odstCamera.activeCallbacks, "all-detour", false))
            return false;

        bool removedAll = true;
        for (size_t i = g_odstCamera.hookTargetCount; i > 0; --i)
        {
            void* target = g_odstCamera.hookTargets[i - 1];
            if (!target)
                continue;
            const MH_STATUS status = MH_RemoveHook(target);
            if (status != MH_OK && status != MH_ERROR_NOT_CREATED)
            {
                removedAll = false;
                LOG("ODST camera cleanup: remove failed for %p (%d)",
                    target, static_cast<int>(status));
            }
        }
        return removedAll;
    }

    void ReleaseOdstModuleReferenceAndClearPointers()
    {
        HMODULE moduleReference = g_odstCamera.moduleReference;
        ClearOdstCameraPointers();
        if (moduleReference)
            FreeLibrary(moduleReference);
    }

    bool DiscardCreatedOdstHooks()
    {
        OdstRequestFallback(OdstFallbackReason::InstallFailure);
        if (!DisableAndRemoveOdstHooks())
        {
            // Retain every original, target, and the module reference. A
            // pass-through detour is safer than clearing a trampoline that may
            // still be reachable; the worker retries verified cleanup.
            g_odstCamera.installed.store(true, std::memory_order_release);
            PublishOdstLifecycle();
            return false;
        }
        if (!RestoreOdstNativeWeaponIkBypass())
        {
            g_odstCamera.installed.store(true, std::memory_order_release);
            PublishOdstLifecycle();
            LOG("ODST camera rollback: retaining title module until the "
                "native weapon-IK branch is restored");
            return false;
        }
        if (!RestoreOdstCrosshairClassGate())
        {
            g_odstCamera.installed.store(true, std::memory_order_release);
            PublishOdstLifecycle();
            LOG("ODST camera rollback: retaining title module until the "
                "crosshair class-gate is restored");
            return false;
        }
        RestoreOdstMotionBlurVars();
        g_odstCamera.installed.store(false, std::memory_order_release);
        ReleaseOdstModuleReferenceAndClearPointers();
        g_odstCamera.teardownRequested.store(false, std::memory_order_release);
        g_odstCamera.fallbackReason.store(
            static_cast<int>(OdstFallbackReason::None), std::memory_order_release);
        ClearOdstRuntimePublication();
        return true;
    }

    OdstInstallResult InstallOdstCameraCore(
        uintptr_t base, size_t size, uint32_t runtimeGeneration)
    {
        if (!runtimeGeneration)
            return OdstInstallResult::Failed;
        HMODULE moduleReference = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(base),
                                &moduleReference) ||
            reinterpret_cast<uintptr_t>(moduleReference) != base)
        {
            if (moduleReference)
                FreeLibrary(moduleReference);
            LOG("ODST camera preflight: could not retain the exact title module");
            return OdstInstallResult::Failed;
        }

        OdstResolvedCameraRuntime resolved{};
        const OdstInstallResult preflight =
            PreflightOdstCameraRuntime(base, size, resolved);
        if (preflight != OdstInstallResult::Installed)
        {
            FreeLibrary(moduleReference);
            return preflight;
        }
        if (g_odstCamera.moduleReference || g_odstCamera.hookTargetCount != 0)
        {
            FreeLibrary(moduleReference);
            LOG("ODST camera install: prior hook ownership was not cleared");
            return OdstInstallResult::Failed;
        }

        g_odstCamera.moduleBase = base;
        g_odstCamera.moduleSize = size;
        g_odstCamera.moduleReference = moduleReference;
        g_odstCamera.prepareView =
            reinterpret_cast<PrepareViewFn>(resolved.prepareView);
        g_odstCamera.buildViewport =
            reinterpret_cast<BuildViewportFn>(resolved.buildViewport);
        g_odstCamera.buildMatrices =
            reinterpret_cast<BuildMatricesFn>(resolved.buildMatrices);
        g_odstCamera.fpCameraUpload =
            reinterpret_cast<FpCameraUploadFn>(resolved.fpCameraUpload);
        g_odstCamera.gunCameraArray = resolved.gunCameraArray;
        g_odstNativePauseFlag.store(
            reinterpret_cast<uintptr_t>(resolved.nativePauseFlag),
            std::memory_order_release);
        g_odstCamera.motionBlurVars[0] = {
            resolved.motionBlurScale, *resolved.motionBlurScale};
        g_odstCamera.motionBlurVars[1] = {
            resolved.motionBlurMax, *resolved.motionBlurMax};
        g_odstCamera.motionBlurResolved = true;
        g_odstCamera.motionBlurZeroed = false;

        struct HookSpec
        {
            const char* name;
            void* target;
            void* detour;
            void** original;
        };
        HookSpec hooks[] = {
            {"camera copy", reinterpret_cast<void*>(resolved.camCopy),
             reinterpret_cast<void*>(&OdstCamCopyHook),
             reinterpret_cast<void**>(&g_odstCamera.originalCamCopy)},
            {"inner view renderer", reinterpret_cast<void*>(resolved.renderView),
             reinterpret_cast<void*>(&OdstRenderViewHook),
             reinterpret_cast<void**>(&g_odstCamera.originalRenderView)},
            {"FP camera rebuild", reinterpret_cast<void*>(resolved.fpCameraRebuild),
             reinterpret_cast<void*>(&OdstFpCameraRebuildHook),
             reinterpret_cast<void**>(&g_odstCamera.originalFpCameraRebuild)},
            {"FP driver", reinterpret_cast<void*>(resolved.fpDriver),
             reinterpret_cast<void*>(&OdstFpDriverHook),
             reinterpret_cast<void**>(&g_odstCamera.originalFpDriver)},
            {"camera recoil/shake",
             reinterpret_cast<void*>(resolved.observerCameraEffect),
             reinterpret_cast<void*>(&OdstObserverCameraEffectHook),
             reinterpret_cast<void**>(
                 &g_odstCamera.originalObserverCameraEffect)},
            {"FP interpolation", reinterpret_cast<void*>(resolved.fpInterpolate),
             reinterpret_cast<void*>(&OdstFpInterpolateWeaponHook),
             &g_odstCamera.originalFpInterpolate},
            {"FP visible palette",
             reinterpret_cast<void*>(resolved.fpVisiblePalette),
             reinterpret_cast<void*>(&OdstFpVisiblePaletteWeaponHook),
             &g_odstCamera.originalFpVisiblePalette},
            {"native CHUD phase primary",
             reinterpret_cast<void*>(resolved.hudPhasePrimary),
             reinterpret_cast<void*>(&OdstHudPhasePrimaryHook),
             reinterpret_cast<void**>(&g_odstCamera.originalHudPhasePrimary)},
            {"native CHUD phase secondary",
             reinterpret_cast<void*>(resolved.hudPhaseSecondary),
             reinterpret_cast<void*>(&OdstHudPhaseSecondaryHook),
             reinterpret_cast<void**>(&g_odstCamera.originalHudPhaseSecondary)},
            {"native CHUD target copy",
             reinterpret_cast<void*>(resolved.hudTargetCopy),
             reinterpret_cast<void*>(&OdstHudTargetCopyHook),
             reinterpret_cast<void**>(&g_odstCamera.originalHudTargetCopy)},
        };
        static_assert(_countof(hooks) == 10);

        for (const HookSpec& hook : hooks)
        {
            const MH_STATUS status = MH_CreateHook(
                hook.target, hook.detour, hook.original);
            if (status != MH_OK)
            {
                LOG("ODST camera install: MH_CreateHook failed for %s (%d); "
                    "rolling back", hook.name, static_cast<int>(status));
                return DiscardCreatedOdstHooks()
                    ? OdstInstallResult::Failed
                    : OdstInstallResult::CleanupPending;
            }
            const size_t slot = g_odstCamera.hookTargetCount++;
            g_odstCamera.hookTargets[slot] = hook.target;
            g_odstCamera.hookTrampolines[slot] = *hook.original;
            if (hook.target == reinterpret_cast<void*>(resolved.renderView))
                g_odstCamera.renderHookTarget = hook.target;
        }

        bool queueOk = true;
        for (void* target : g_odstCamera.hookTargets)
        {
            if (!target)
                continue;
            const MH_STATUS status = MH_QueueEnableHook(target);
            if (status != MH_OK)
            {
                LOG("ODST camera install: queue-enable failed at %p (%d)",
                    target, static_cast<int>(status));
                queueOk = false;
                break;
            }
        }
        g_odstRuntimeGeneration.store(
            runtimeGeneration, std::memory_order_release);
        g_odstLastCamCopyMs.store(0, std::memory_order_release);
        TitleAdapter_ClearHeartbeat(
            GameTitle::Halo3ODST, runtimeGeneration);
        const MH_STATUS applyStatus = queueOk ? MH_ApplyQueued() : MH_UNKNOWN;
        if (!queueOk || applyStatus != MH_OK)
        {
            LOG("ODST camera install: atomic enable failed (%d); rolling back all hooks",
                static_cast<int>(applyStatus));
            return DiscardCreatedOdstHooks()
                ? OdstInstallResult::Failed
                : OdstInstallResult::CleanupPending;
        }
        if (!ApplyOdstNativeWeaponIkBypass(
                resolved.nativeWeaponIkDecision))
        {
            LOG("ODST camera install: weapon-IK bypass failed; rolling back "
                "the complete ten-hook parity transaction");
            return DiscardCreatedOdstHooks()
                ? OdstInstallResult::Failed
                : OdstInstallResult::CleanupPending;
        }

        // Best-effort HUD parity: hook ODST's proven anchor-basis output for the
        // shared vertical slider, hide its native class-2 crosshair, and paint
        // the weapon's authored widget as the floating VR reticle, exactly like
        // Halo 3. Missing or ambiguous title proof leaves native behavior stock;
        // a hook-manager failure after ownership begins rolls back everything.
        const OdstOptionalHookResult heightHook =
            InstallOdstHudHeight(base, size);
        if (heightHook == OdstOptionalHookResult::CleanupRequired)
        {
            return DiscardCreatedOdstHooks()
                ? OdstInstallResult::Failed
                : OdstInstallResult::CleanupPending;
        }
        const OdstOptionalHookResult crosshairHooks =
            InstallOdstCrosshairHider(base, size);
        if (crosshairHooks == OdstOptionalHookResult::CleanupRequired)
        {
            return DiscardCreatedOdstHooks()
                ? OdstInstallResult::Failed
                : OdstInstallResult::CleanupPending;
        }

        g_odstCamera.captureFailures.store(0, std::memory_order_release);
        g_odstCamera.sawValidCamera.store(false, std::memory_order_release);
        g_odstCamera.fallbackReason.store(
            static_cast<int>(OdstFallbackReason::None), std::memory_order_release);
        g_odstCamera.teardownRequested.store(false, std::memory_order_release);
        g_odstCamera.cameraArrayReady.store(true, std::memory_order_release);
        g_odstCamera.installed.store(true, std::memory_order_release);
        g_odstCamera.armed.store(false, std::memory_order_release);
        g_odstCamera.installedAtMs.store(
            GetTickCount64(), std::memory_order_release);
        g_odstLastCamCopyMs.store(0, std::memory_order_release);
        g_stereoEye.store(-1, std::memory_order_release);
        g_aimSeen.store(false, std::memory_order_release);
        for (auto& valid : g_barrelInWristValid)
            valid.store(false, std::memory_order_release);
        g_camValid.store(false, std::memory_order_release);
        g_baseCamValid.store(false, std::memory_order_release);
        g_zoomFactor.store(1.0f, std::memory_order_release);
        g_enabled.store(false, std::memory_order_release);
        g_positional.store(true, std::memory_order_release);
        g_needRecenter.store(true, std::memory_order_release);
        if (!PublishOdstLifecycle())
        {
            LOG("ODST camera install: title generation changed; requesting verified cleanup");
            OdstRequestFallback(OdstFallbackReason::TitleLeft);
            return OdstInstallResult::CleanupPending;
        }
        TitleAdapter_PublishMode(
            GameTitle::Halo3ODST, runtimeGeneration, RuntimeMode::Loading);
        VR_SetScopeActive(false);
        // Resolve ODST's cinematic scene/shot state so OdstApplyHeadLook can
        // rebase the VR yaw at each authored cut, matching Halo 3. The signature
        // is title-agnostic; the shot-state TLS offset is read from ODST's own
        // instruction (0xA0). Failure logs and leaves shot-facing disabled.
        LocateCinematicState(base, size);
        // ODST cinematic FOV parity (issue #18): Halo applies a 25% widescreen
        // FOV reduction while a cinematic is in progress, which also narrows the
        // visibility projection and pulls the view in during ODST cutscenes.
        // Halo 3 name-resolves this engine debug var in InstallHook and holds it
        // at 0 while stereo is active; ODST installs through this separate core,
        // so resolve it here against halo3odst.dll's own table using the same
        // proven FindDebugVarFloat path already used for the motion-blur vars.
        // UpdateCinematicFovPolicy() enforces it from the ODST present tick, and
        // the pointer is cleared in the hook-state teardown below.
        ResolveCinematicFovVar(base, size);
        LOG("ODST camera install: ten-hook camera/weapon/CHUD core plus %zu "
            "verified optional HUD hook(s) retained and disarmed; "
            "waiting for presentation detach and a fresh camera heartbeat; "
            "Halo 3 weapon, arm IK, two-hand, and dual-wield config path ready",
            g_odstCamera.hookTargetCount - _countof(hooks));
        return OdstInstallResult::Installed;
    }

    bool RemoveOdstCameraCore()
    {
        g_odstCamera.cameraArrayReady.store(
            false, std::memory_order_release);
        InvalidateHudLayoutProfile(HudLayoutProfile::Halo3ODST);
        OdstRequestPresentationDetach();
        if (!DisableAndRemoveOdstHooks())
        {
            LOG("ODST camera teardown: verified cleanup incomplete; retaining "
                "module, targets, and trampolines for retry");
            return false;
        }
        if (!RestoreOdstNativeWeaponIkBypass())
        {
            LOG("ODST camera teardown: native weapon-IK restore incomplete; "
                "retaining the exact title module for retry");
            return false;
        }
        if (!RestoreOdstCrosshairClassGate())
        {
            LOG("ODST camera teardown: crosshair class-gate restore incomplete; "
                "retaining the exact title module for retry");
            return false;
        }
        RestoreOdstMotionBlurVars();

        const auto reason = static_cast<OdstFallbackReason>(
            g_odstCamera.fallbackReason.load(std::memory_order_acquire));
        const char* reasonName = "requested cleanup";
        if (reason == OdstFallbackReason::LevelUnloaded)
            reasonName = "level unload";
        else if (reason == OdstFallbackReason::EyeRedirectUnavailable)
            reasonName = "eye redirect unavailable";
        else if (reason == OdstFallbackReason::UnsupportedCameraMode)
            reasonName = "camera mode outside the proven single-user path";
        else if (reason == OdstFallbackReason::NoCameraHeartbeat)
            reasonName = "camera heartbeat timeout";
        else if (reason == OdstFallbackReason::InstallFailure)
            reasonName = "install rollback";
        else if (reason == OdstFallbackReason::TitleLeft)
            reasonName = "title exit";
        else if (reason == OdstFallbackReason::NativePause)
            reasonName = "native pause boundary";

        g_odstCamera.installed.store(false, std::memory_order_release);
        g_odstLastCamCopyMs.store(0, std::memory_order_release);
        g_stereoEye.store(-1, std::memory_order_release);
        g_aimSeen.store(false, std::memory_order_release);
        for (auto& valid : g_barrelInWristValid)
            valid.store(false, std::memory_order_release);
        g_camValid.store(false, std::memory_order_release);
        g_baseCamValid.store(false, std::memory_order_release);
        g_zoomFactor.store(1.0f, std::memory_order_release);
        g_odstCamera.sawValidCamera.store(false, std::memory_order_release);
        g_odstCamera.captureFailures.store(0, std::memory_order_release);
        ReleaseOdstModuleReferenceAndClearPointers();
        g_odstCamera.teardownRequested.store(false, std::memory_order_release);
        g_odstCamera.fallbackReason.store(
            static_cast<int>(OdstFallbackReason::None), std::memory_order_release);
        ClearOdstRuntimePublication();
        LOG("ODST camera teardown complete (%s); stock renderer owns the title",
            reasonName);
        return true;
    }

    bool ProbeOdstCameraReadiness(const wchar_t* moduleName,
                                  uintptr_t cameraArrayRva)
    {
        if (!moduleName || !cameraArrayRva)
            return false;
        uintptr_t base = 0;
        size_t size = 0;
        if (!sig::ModuleRange(moduleName, base, size))
            return false;
        HMODULE moduleReference = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(base),
                                &moduleReference) ||
            reinterpret_cast<uintptr_t>(moduleReference) != base)
        {
            if (moduleReference)
                FreeLibrary(moduleReference);
            return false;
        }

        uintptr_t textBegin = 0, textEnd = 0;
        uint32_t timestamp = 0;
        size_t imageSize = 0;
        const size_t arraySize = 4 * kOdstCameraProfile.layout.viewStride;
        const bool identityMatches = ResolveOdstTextRange(
            base, size, textBegin, textEnd, timestamp, imageSize) &&
            timestamp == kOdstCameraProfile.expectedTimestamp &&
            imageSize == kOdstCameraProfile.expectedImageSize &&
            imageSize == size;
        const bool arrayFits = cameraArrayRva < size &&
            arraySize <= size - cameraArrayRva;
        const bool ready = identityMatches && arrayFits &&
            OdstCameraArraySupportsBringup(base + cameraArrayRva);
        FreeLibrary(moduleReference);
        return ready;
    }
#endif

#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    // ------------------------------------------------------------------
    // Halo: Reach per-eye stereo camera core.
    //
    // Reach is a permanent title. Its render path was proven statically and
    // corroborated by the read-only observer (docs/REACH-SIGNATURE-EVIDENCE.md):
    // the exact inner renderer player_view_render (RVA 0x26C6DC), the outer
    // main_render_view (RVA 0x0C31F4) with its normal vs screenshot caller
    // returns, the 0x2B0 rasterizer-camera workspace whose primary compact
    // camera lives at +0x000 (Reach layout: position +0x00, forward +0x0C, up
    // +0x18, vertical FOV +0x28 -- exactly what ValidateReachCompactCamera
    // decodes), and the player-view rollback regions.
    //
    // The worker installs the five proven camera/FP hooks only after the
    // loaded-image preflight passes and the VR eye-capture resources are
    // published, then arms after a one-second fresh-camera safety interval,
    // exactly like the accepted ODST core. A failed owned eye is invalidated and
    // never published as Reach stereo. The one-original-render safety call stays
    // outside that completed pair and cannot become a partial VR mode.
    // Camera-injection correctness remains an exact-hash headset result.
    using ReachPlayerViewRenderFn = void(__fastcall*)(uintptr_t);
    using ReachMainRenderViewFn =
        uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uint32_t);

    // The four stock camera-rebuild helpers, with the exact ABIs proven in
    // REACH-SIGNATURE-EVIDENCE.md steps 2-6. Running them per eye before the
    // inner render rebuilds player_view+0x490 from the VR camera, exactly as
    // Halo 3's RenderViewHook rebuilds view+0x98 with the engine's own helpers.
    using ReachFrustumHelperFn = bool(__fastcall*)(void*, float*);
    using ReachProjectionBuilderFn = void(__fastcall*)(void*, float*, void*, float);
    using ReachCameraStateUpdaterFn = void(__fastcall*)(void*, void*);
    using ReachMatrixBuilderFn =
        void(__fastcall*)(void*, void*, void*, void*, void*);
    using ReachFpCameraRebuildFn = void(__fastcall*)(void*, bool);
    using ReachFpCameraUploadFn = void(__fastcall*)(void*, void*);

    struct ReachRenderHelpers
    {
        ReachFrustumHelperFn frustum = nullptr;
        ReachProjectionBuilderFn projection = nullptr;
        ReachCameraStateUpdaterFn cameraState = nullptr;
        ReachMatrixBuilderFn matrix = nullptr;
        bool Ready() const
        {
            return frustum && projection && cameraState && matrix;
        }
    };
    ReachRenderHelpers g_reachHelpers;

    // A direct `call rel32` is `E8 <int32>`; its target is site + 5 + rel32.
    // Verifying the two proven setup call sites resolve to the documented helper
    // RVAs anchors those addresses to real, in-image call edges so a changed
    // module fails open instead of arming on a stale offset.
    bool ReachVerifyRel32Call(
        uintptr_t base, uintptr_t siteRva, uintptr_t expectedTargetRva)
    {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(base + siteRva);
        if (p[0] != 0xE8)
            return false;
        int32_t rel = 0;
        memcpy(&rel, p + 1, sizeof(rel));
        const uintptr_t target =
            base + siteRva + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
        return target == base + expectedTargetRva;
    }

    bool ReachVerifyRipRelativeLea(
        uintptr_t base, uintptr_t siteRva,
        uint8_t opcode0, uint8_t opcode1, uint8_t opcode2,
        uintptr_t expectedTargetRva)
    {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(base + siteRva);
        if (p[0] != opcode0 || p[1] != opcode1 || p[2] != opcode2)
            return false;
        int32_t displacement = 0;
        memcpy(&displacement, p + 3, sizeof(displacement));
        const uintptr_t target = static_cast<uintptr_t>(
            static_cast<intptr_t>(base + siteRva + 7) + displacement);
        return target == base + expectedTargetRva;
    }

    // The complete main_render_view body is already hash-pinned by the cold
    // preflight. Recheck its live visibility edges and the callee's exact
    // secondary-camera load before authorizing pre-visibility mutation, so an
    // in-memory mismatch remains stock instead of silently culling from an
    // unknown camera object.
    bool ReachVerifyVisibilityConsumer(uintptr_t base, size_t size)
    {
        constexpr uintptr_t kWorkspaceLeaRva = 0x000C3319;
        constexpr uint8_t kSecondaryCompactLea[7]{
            0x48, 0x8D, 0x8A, 0x54, 0x01, 0x00, 0x00};
        const auto fits = [size](uintptr_t rva, size_t bytes)
        {
            return rva < size && bytes <= size - rva;
        };
        if (!fits(kWorkspaceLeaRva, 7) ||
            !fits(kReachVisibilityClusterLookupCallRva, 5) ||
            !fits(kReachVisibilitySecondaryCompactLeaRva,
                  sizeof(kSecondaryCompactLea)) ||
            !fits(kReachVisibilitySecondaryDerivedLeaRva, 7) ||
            !fits(kReachVisibilityBuildCallRva, 5))
        {
            return false;
        }
        return ReachVerifyRipRelativeLea(
                   base, kWorkspaceLeaRva, 0x48, 0x8D, 0x15,
                   kReachDefaultWorkspaceRva) &&
            ReachVerifyRel32Call(
                base, kReachVisibilityClusterLookupCallRva,
                kReachVisibilityClusterLookupTargetRva) &&
            memcmp(reinterpret_cast<const void*>(
                       base + kReachVisibilitySecondaryCompactLeaRva),
                   kSecondaryCompactLea,
                   sizeof(kSecondaryCompactLea)) == 0 &&
            ReachVerifyRipRelativeLea(
                base, kReachVisibilitySecondaryDerivedLeaRva,
                0x4C, 0x8D, 0x05,
                kReachVisibilitySecondaryDerivedAddressRva) &&
            ReachVerifyRel32Call(
                base, kReachVisibilityBuildCallRva,
                kReachVisibilityBuildTargetRva);
    }

    // Decode the exact internal patchy-fog skip gate from the already
    // hash-pinned player_view_render body. This cold install-time check binds
    // the writable byte to the retail RIP-relative test and the proven helper
    // call edge. A changed opcode, branch, target, or mapping leaves Reach
    // completely stock.
    bool ReachVerifyPatchyFogGate(
        uintptr_t base, size_t size, uint8_t*& flags)
    {
        flags = nullptr;
        constexpr size_t kGateBytes = 17;
        const auto fits = [size](uintptr_t rva, size_t bytes)
        {
            return rva < size && bytes <= size - rva;
        };
        if (!fits(kReachPatchyFogGateTestRva, kGateBytes) ||
            !fits(kReachPatchyFogFlagsRva, sizeof(uint8_t)) ||
            !fits(kReachPatchyFogCallRva, 5))
        {
            return false;
        }

        const uint8_t* gate = reinterpret_cast<const uint8_t*>(
            base + kReachPatchyFogGateTestRva);
        constexpr uint8_t kTail[] = {
            kReachPatchyFogSkipMask, 0x75, 0x08, 0x48, 0x8B, 0xCE, 0xE8};
        if (gate[0] != 0xF6 || gate[1] != 0x05 ||
            memcmp(gate + 6, kTail, sizeof(kTail)) != 0 ||
            kReachPatchyFogSkipJumpRva != kReachPatchyFogGateTestRva + 7 ||
            kReachPatchyFogCallRva != kReachPatchyFogGateTestRva + 12)
        {
            return false;
        }

        int32_t displacement = 0;
        memcpy(&displacement, gate + 2, sizeof(displacement));
        const uintptr_t next = base + kReachPatchyFogGateTestRva + 7;
        const uintptr_t target = static_cast<uintptr_t>(
            static_cast<intptr_t>(next) + displacement);
        if (target != base + kReachPatchyFogFlagsRva ||
            !ReachVerifyRel32Call(
                base, kReachPatchyFogCallRva,
                kReachPatchyFogTargetRva))
        {
            return false;
        }

        auto* resolved = reinterpret_cast<uint8_t*>(target);
        uint8_t current = 0;
        if (!SafeReadByte(resolved, &current))
            return false;
        flags = resolved;
        return true;
    }

    constexpr size_t kReachCompactCameraBytes = 0x90;

    struct ReachEyeRenderInput
    {
        float position[3]{};
        ReachEyeCullFrustum frustum{};
        ReachSymmetricFovCover rasterCover{};
    };

    struct ReachOwnerScope
    {
        bool active = false;
        uintptr_t workspace = 0;
        uintptr_t playerView = 0;
        int32_t cameraStackDepthBefore = -1;
        uint64_t preparedSerial = 0;
        ReachVrRenderAccess* renderAccess = nullptr;
        float gameplayBasePosition[3]{};
        FpExplicitPoseTargets fpTargets{};
        alignas(16) unsigned char headCenter[kReachCompactCameraBytes]{};
        ReachEyeRenderInput eyes[2]{};
    };

    // Outer and inner hooks run on the same render thread in one synchronous
    // call stack, so a thread-local owner scope needs no cross-thread sync.
    thread_local ReachOwnerScope g_reachOwnerScope;
    // A nested main_render_view must be entirely stock. Clearing the owner
    // scope alone is insufficient if that nested call recursively enters the
    // outer hook again, so carry an explicit suppression bit through the whole
    // nested stock call tree.
    thread_local bool g_reachNestedOuterSuppressed = false;

    struct ReachFpCameraEyeScope
    {
        bool active = false;
        bool chudParityFailed = false;
        bool chudClass2Seen = false;
        bool authoredCrosshairCaptured = false;
        // Identity of the art captured this eye: which weapon definition,
        // which collection, which widgets. Static art produces the same key
        // every frame, which is what lets the per-frame upload be skipped.
        uint64_t captureKey = 0;
        uint32_t generation = 0;
        uint32_t eye = 0;
        uint64_t preparedSerial = 0;
        uintptr_t workspace = 0;
        uintptr_t playerView = 0;
        alignas(16) unsigned char compact[kReachCompactCameraSize]{};
        alignas(16) unsigned char derived[kReachDerivedBlockSize]{};
    };
    thread_local ReachFpCameraEyeScope g_reachFpCameraEyeScope;

    struct ReachCameraCore
    {
        std::atomic<bool> installed{false};
        std::atomic<bool> armed{false};
        std::atomic<bool> teardownRequested{false};
        // Counts every Reach detour from wrapper entry until every
        // trampoline/original call has returned. Teardown disables all hooks,
        // then proves that neither a callback nor a MinHook relay ingress
        // remains before freeing either trampoline or the retained title DLL.
        std::atomic<int> activeCallbacks{0};
        uintptr_t base = 0;
        size_t size = 0;
        std::atomic<uint32_t> generation{0};
        HMODULE moduleReference = nullptr;
        void* innerTarget = nullptr;
        void* outerTarget = nullptr;
        uint64_t installedAtMs = 0;
        MotionBlurVar motionBlurVars[2]{};
        bool motionBlurResolved = false;
        bool motionBlurSuppressed = false;
        uint8_t* patchyFogFlags = nullptr;
        // Live `render_lightmap_shadows` boolean. The owned-eye scope saves,
        // clears, and restores it around player_view_render to isolate Reach's
        // proven object-caster -> static-lightmap receiver pass.
        uint8_t* lightmapShadowsEnabled = nullptr;
        uint8_t* nativeWeaponIkDisable = nullptr;
        uint8_t nativeWeaponIkDisableOriginal = 0;
        bool nativeWeaponIkBypassActive = false;
        void* fpInterpolateTarget = nullptr;
        void* fpPaletteTarget = nullptr;
        void* fpCameraTarget = nullptr;
        void* ssaoTarget = nullptr;
        std::atomic<bool> ssaoIsolationActive{false};
        void* hudDrawWidgetTarget = nullptr;
        void* observerCameraTarget = nullptr;
        void* effectLocationTarget = nullptr;
        void* rainRenderTarget = nullptr;
    } g_reachCamera;
    std::atomic<uint32_t> g_reachLightmapShadowSuppressedEyes{0};
    std::atomic<uint32_t> g_reachLightmapShadowAlreadyDisabledEyes{0};
    std::atomic<uint32_t> g_reachLightmapShadowWriteFailures{0};
    std::atomic<uint32_t> g_reachLightmapShadowRestoreFailures{0};
    std::atomic<uint8_t> g_reachLightmapShadowOutstandingValue{0};
    std::atomic<bool> g_reachLightmapShadowRestoreOutstanding{false};
    std::atomic<uint32_t> g_reachSsaoSuppressedEyeCalls[2]{};
    std::atomic<uint32_t> g_reachSsaoPassthroughCalls{0};
    // Headset candidate 839aed7 proved the exact pass can be suppressed and
    // restored per eye without changing the black static-world defect. Keep
    // the resolver and scoped implementation as evidence, but do not arm it.
    constexpr bool kReachLightmapShadowIsolationEnabled = false;
    // Headset candidate 8d7af6e suppressed the exact native SSAO callee for
    // 3,602 calls in each eye; the black static-world defect was unchanged.
    // Retain the proven hook and lifecycle code as evidence, but do not arm it.
    constexpr bool kReachSsaoIsolationEnabled = false;
    bool RestoreReachNativeWeaponIkBypass();
    bool RestoreReachLightmapShadowsControl();
    bool RestoreReachThirdPersonEffectSuppression();
    void ReachMuzzleRetargetRestore();
    static_assert(std::atomic<int>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<uint8_t>::is_always_lock_free);
    static_assert(std::atomic<uint32_t>::is_always_lock_free);

    // Published only after the matching eye was rendered and copied. The
    // release/acquire serial binds each pair of projection scales to the exact
    // OpenXR prepared frame that will submit those Reach eye textures.
    std::atomic<float> g_reachRenderHalfFovX[2]{};
    std::atomic<float> g_reachRenderHalfFovY[2]{};
    std::atomic<uint64_t> g_reachRenderFovSerial[2]{};

    ReachPlayerViewRenderFn g_reachOrigPlayerViewRender = nullptr;
    ReachMainRenderViewFn g_reachOrigMainRenderView = nullptr;
    using ReachFpInterpolateFn = bool(__fastcall*)(
        int, int, int, BoneMatrix**, int*);
    ReachFpInterpolateFn g_reachOrigFpInterpolate = nullptr;
    // Production first-person palette trampoline. ABI verified against the
    // pinned Reach image at 0x2B4EB0.
    using ReachFpPaletteFn = void(__fastcall*)(
        uint16_t, const BoneMatrix*, BoneMatrix*, uintptr_t,
        const BoneMatrix*, const int32_t*);
    ReachFpPaletteFn g_reachOrigFpPalette = nullptr;
    ReachFpCameraRebuildFn g_reachOrigFpCameraRebuild = nullptr;
    ReachFpCameraUploadFn g_reachFpCameraUpload = nullptr;
    using ReachRenderSsaoFn = void(__fastcall*)(void*);
    ReachRenderSsaoFn g_reachOrigRenderSsao = nullptr;
    // Arguments 3 and 4 are FULL 32-bit values, not short/byte. Reach's own
    // code proves it: at 0x2DA39D it does "mov r12d, r8d" and at 0x2DA41D
    // "cmp r12d, dword ptr [rdi+4]" - a full 32-bit compare of argument 3 -
    // and at 0x2DA39A "mov eax, r9d" reads argument 4 the same way. Declaring
    // them narrower truncated the upper bits on the way back into the engine,
    // so it took the wrong branch, decoded an invalid Blam pool handle, and
    // faulted at haloreach.dll+0x2ED80C ("mov eax,[rcx+0x98]"). That was the
    // crash, four times over, not the address and not a skipped call.
    using ReachHudDrawWidgetFn = void(__fastcall*)(
        int, void*, unsigned int, unsigned int, void*);
    ReachHudDrawWidgetFn g_reachOrigHudDrawWidget = nullptr;
    // A runtime authored-target loss rejects parity for this exact title-module
    // generation. Do not churn through remove/reinstall loops or silently arm a
    // lesser mode; a fresh module generation is required for another proof.
    std::atomic<uint32_t> g_reachChudParityFailedGeneration{0};

    static int SafeReadU32(const void* slot, uint32_t* value)
    {
        __try
        {
            *value = *reinterpret_cast<const volatile uint32_t*>(slot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return 1;
    }

    static int SafeReadPtr(const void* slot, uintptr_t* value)
    {
        __try
        {
            *value = *reinterpret_cast<const volatile uintptr_t*>(slot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return 1;
    }

    // Reach's own resolution, decoded from 0x2DA68A-0x2DA6EC. See
    // reach_render_logic.h for the derivation. Returns false if any link in
    // the chain cannot be read; the caller must treat that as a real failure,
    // never as "not a crosshair".
    // This runs for every CHUD widget draw - roughly ten thousand times a
    // second - so the five-level pointer walk below is cached. The class of a
    // given (definition, collection) pair is fixed tag data; it can only change
    // when the loaded tags change, which the camera generation tracks. Direct
    // mapped, fixed size, no allocation, thread_local so no synchronisation.
    struct ReachChudClassCacheEntry
    {
        uint32_t key = 0xFFFFFFFFu;
        uint32_t generation = 0;
        int8_t scriptingClass = -1;
        bool resolved = false;
    };
    constexpr size_t kReachChudClassCacheSize = 64;
    thread_local ReachChudClassCacheEntry
        g_reachChudClassCache[kReachChudClassCacheSize];

    static bool ResolveReachChudCollectionClassUncached(
        uintptr_t base, const void* descriptor, unsigned int definitionIndex,
        uint8_t collectionIndex, int8_t& scriptingClass);

    static bool ResolveReachChudCollectionClass(
        uintptr_t base, const void* descriptor, unsigned int definitionIndex,
        int8_t& scriptingClass)
    {
        if (!base || !descriptor)
            return false;
        uint8_t collectionIndex = 0;
        if (!SafeReadByte(
                reinterpret_cast<const uint8_t*>(descriptor) +
                    kReachChudDescriptorCollectionByte,
                &collectionIndex))
            return false;

        const uint32_t generation =
            g_reachCamera.generation.load(std::memory_order_relaxed);
        const uint32_t key =
            ((definitionIndex & 0xFFFFu) << 8) | collectionIndex;
        ReachChudClassCacheEntry& entry =
            g_reachChudClassCache[key % kReachChudClassCacheSize];
        if (entry.key == key && entry.generation == generation)
        {
            scriptingClass = entry.scriptingClass;
            return entry.resolved;
        }

        int8_t resolvedClass = -1;
        const bool ok = ResolveReachChudCollectionClassUncached(
            base, descriptor, definitionIndex, collectionIndex, resolvedClass);
        entry.key = key;
        entry.generation = generation;
        entry.scriptingClass = resolvedClass;
        entry.resolved = ok;
        scriptingClass = resolvedClass;
        return ok;
    }

    static bool ResolveReachChudCollectionClassUncached(
        uintptr_t base, const void* descriptor, unsigned int definitionIndex,
        uint8_t collectionIndex, int8_t& scriptingClass)
    {
        (void)descriptor;
        const auto pool = [base](uint32_t handle, uintptr_t& out) -> bool {
            const uintptr_t slot = base + kReachChudPoolTableRva +
                static_cast<uintptr_t>(handle >> 28) * 8;
            return SafeReadPtr(reinterpret_cast<const void*>(slot), &out) != 0;
        };

        uintptr_t globalPtr = 0;
        if (!SafeReadPtr(
                reinterpret_cast<const void*>(
                    base + kReachChudDefinitionTableRva),
                &globalPtr) ||
            !globalPtr)
            return false;

        uint32_t definitionHandle = 0;
        if (!SafeReadU32(
                reinterpret_cast<const void*>(
                    globalPtr +
                    static_cast<uintptr_t>(definitionIndex & 0xFFFFu) * 8 + 4),
                &definitionHandle))
            return false;

        uintptr_t definitionPool = 0;
        if (!pool(definitionHandle, definitionPool) || !definitionPool)
            return false;
        const uintptr_t chudDefinition =
            definitionPool + static_cast<uintptr_t>(definitionHandle) * 4;

        uint32_t collectionHandle = 0;
        if (!SafeReadU32(
                reinterpret_cast<const void*>(chudDefinition + 4),
                &collectionHandle))
            return false;

        uintptr_t collectionPool = 0;
        if (!pool(collectionHandle, collectionPool) || !collectionPool)
            return false;
        const uintptr_t collection = collectionPool +
            (static_cast<uintptr_t>(collectionHandle) +
             static_cast<uintptr_t>(collectionIndex) *
                 kReachChudCollectionStride) * 4;

        uint8_t rawClass = 0;
        if (!SafeReadByte(
                reinterpret_cast<const uint8_t*>(
                    collection + kReachChudCollectionClassOffset),
                &rawClass))
            return false;
        scriptingClass = static_cast<int8_t>(rawClass);
        return true;
    }

    // Last art identity captured for Reach. Read by the compositor to decide
    // whether the authored reticle actually needs re-uploading this frame.
    std::atomic<uint64_t> g_reachAuthoredCrosshairKey{0};

    bool ReachOwnsHudStereoTransaction()
    {
        if (TitleAdapter_GetActiveTitle() != GameTitle::HaloReach)
            return false;
        const uint32_t cameraGeneration =
            g_reachCamera.generation.load(std::memory_order_acquire);
        return cameraGeneration != 0 &&
            TitleAdapter_GetGeneration(GameTitle::HaloReach) ==
                cameraGeneration &&
            g_reachCamera.installed.load(std::memory_order_acquire) &&
            g_reachCamera.armed.load(std::memory_order_acquire) &&
            !g_reachCamera.teardownRequested.load(std::memory_order_acquire) &&
            g_enabled.load(std::memory_order_relaxed) &&
            VR_IsStereoEnabled() && g_reachFpCameraEyeScope.active &&
            g_reachFpCameraEyeScope.generation == cameraGeneration;
    }

    // REACHHUD diagnostic counters. The CHUD hook is a hot hook, so it only
    // bumps atomics here; every log line is emitted by the 50 ms worker.
    // Purpose: the 2026-07-27 session lost the VR crosshair mid-level, and the
    // user's observation ties it to on-screen objective text. These separate
    // the three candidate mechanisms - the engine stops drawing class-2
    // widgets, the collection descriptor stops being readable (which rejects
    // the transaction), or the art key churns and republishes different art -
    // instead of guessing between them.
    std::atomic<uint64_t> g_reachChudLastClass2Ms{0};
    std::atomic<uint32_t> g_reachChudClass2Draws{0};
    std::atomic<uint32_t> g_reachChudUnreadable{0};
    std::atomic<uint32_t> g_reachChudRejects{0};
    std::atomic<uint32_t> g_reachChudRedirectUnavailable{0};
    std::atomic<uint8_t> g_reachChudLastUnreadablePath{0};
    // Readable draws whose collection class was not 2, plus a histogram of
    // the classes actually seen. Closes the blind spot that made the
    // 2026-07-27 objective drought ambiguous.
    constexpr size_t kReachChudClassBuckets = 10;
    std::atomic<uint32_t> g_reachChudOtherDraws{0};
    std::array<std::atomic<uint32_t>, kReachChudClassBuckets>
        g_reachChudClassCounts{};

    void RejectReachChudParityForCurrentEye() noexcept
    {
        g_reachChudRejects.fetch_add(1, std::memory_order_relaxed);
        g_reachFpCameraEyeScope.chudParityFailed = true;
        g_reachChudParityFailedGeneration.store(
            g_reachCamera.generation, std::memory_order_release);
        g_reachCamera.armed.store(false, std::memory_order_release);
        g_reachCamera.teardownRequested.store(
            true, std::memory_order_release);
    }

    // A momentarily unavailable render-target redirect is NOT a fatal
    // transaction failure. It used to call the reject path above, which
    // disarms the core and tears down all six hooks - so a transient
    // resource state, or simply turning the crosshair off, killed VR for the
    // whole session. Both defects were observed in headset testing on
    // 2026-07-27, and it is the same shape as the two silent teardown paths
    // removed on 2026-07-26 (AGENTS.md: a feature failing must never take
    // down a working VR path).
    //
    // Consequence of the redirect being unavailable: this widget draws to the
    // eye instead of the offscreen target. With kill_reticle=1 its alpha is
    // already cleared, so nothing appears. The eye is marked so a partially
    // captured pair is never published - the compositor skips that frame and
    // retries, exactly like the accepted projection-view skip - and the lazy
    // capture entry creates whatever was missing on the next draw.
    void ReportReachRedirectUnavailable() noexcept
    {
        g_reachChudRedirectUnavailable.fetch_add(
            1, std::memory_order_relaxed);
        g_reachFpCameraEyeScope.chudParityFailed = true;
    }

    // Official HREK exposes the scripting-class byte directly to Reach's
    // five-argument chud_draw_widget transaction. Select only class 2 and
    // reuse Halo 3/ODST's authored-widget capture. No procedural reticle,
    // widget-name fallback, or mixed flat-crosshair mode exists.
    // ---- Reach rain: stop the volume swinging with the view ----------------
    // See kReachRainParticleRenderRva for the derivation. The renderer centres
    // the whole rain volume at position + forward * (size * 0.45), and in VR
    // that forward carries both head-look and hand aim, so the entire rain
    // field rotates with the player. Zeroing the forward for the duration of
    // this one call centres the volume on the camera instead, which keeps the
    // rain following the player's POSITION while decoupling it from rotation.
    //
    // Scoped save/substitute/restore around exactly one engine call on the
    // render thread, with __finally so an exception cannot leave the workspace
    // modified. This is not the banned "scoped camera write": that rule is
    // about splitting head-look between frames, and this touches a value the
    // rain renderer is the only documented consumer of during its own call.
    const char* kReachRainRenderSig =
        "48 63 05 ?? ?? ?? ?? 44 0F 28 D2 85 C0 78 0D 48 8D 1D ?? ?? ?? ?? "
        "48 8B 1C C3 EB 02 33 DB";
    using ReachRainRenderFn = void(__fastcall*)(uintptr_t, uintptr_t, float);
    ReachRainRenderFn g_reachOrigRainRender = nullptr;
    std::atomic<uint32_t> g_reachRainDecoupled{0};
    std::atomic<uint32_t> g_reachRainSkipped{0};

    // Resolve the workspace the rain renderer itself will use, the same way it
    // does: camera-stack depth indexes the camera-stack pointer array.
    uintptr_t ReachResolveTopCameraWorkspace()
    {
        const uintptr_t base = g_reachCamera.base;
        if (!base)
            return 0;
        __try
        {
            const int32_t depth = *reinterpret_cast<const int32_t*>(
                base + kReachCameraStackDepthRva);
            if (depth < 0 || depth > 64)
                return 0;
            const uintptr_t workspace = *reinterpret_cast<const uintptr_t*>(
                base + kReachCameraStackPointersRva +
                static_cast<uintptr_t>(depth) * sizeof(uintptr_t));
            if (workspace < base || workspace >= base + g_reachCamera.size)
                return 0;
            return workspace;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    // Guarded save-and-zero of the workspace forward. Separate function so no
    // __try/__except is nested inside the detour's __try/__finally - that
    // nesting crashed the MSVC front end outright (CL.exe 0xC0000005).
    __declspec(noinline) bool ReachRainZeroForward(
        float* forward, float* saved)
    {
        __try
        {
            saved[0] = forward[0];
            saved[1] = forward[1];
            saved[2] = forward[2];
            if (!isfinite(saved[0]) || !isfinite(saved[1]) ||
                !isfinite(saved[2]))
            {
                return false;
            }
            forward[0] = 0.0f;
            forward[1] = 0.0f;
            forward[2] = 0.0f;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __declspec(noinline) void ReachRainRestoreForward(
        float* forward, const float* saved)
    {
        __try
        {
            forward[0] = saved[0];
            forward[1] = saved[1];
            forward[2] = saved[2];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    __declspec(noinline) void __fastcall ReachRainRenderDetour(
        uintptr_t passthroughA, uintptr_t passthroughB, float intensity)
    {
        g_reachCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        float* forward = nullptr;
        float savedForward[3] = {0.0f, 0.0f, 0.0f};
        __try
        {
            ReachRainRenderFn original = g_reachOrigRainRender;
            if (!original)
                return;
            if (g_reachCamera.armed.load(std::memory_order_acquire) &&
                g_enabled.load(std::memory_order_acquire))
            {
                const uintptr_t workspace = ReachResolveTopCameraWorkspace();
                if (workspace)
                {
                    float* candidate = reinterpret_cast<float*>(
                        workspace + kReachSecondaryCompactOffset +
                        kReachCompactCameraForwardOffset);
                    if (ReachRainZeroForward(candidate, savedForward))
                    {
                        forward = candidate;
                        g_reachRainDecoupled.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
                if (!forward)
                    g_reachRainSkipped.fetch_add(1, std::memory_order_relaxed);
            }
            original(passthroughA, passthroughB, intensity);
        }
        __finally
        {
            if (forward)
                ReachRainRestoreForward(forward, savedForward);
            g_reachCamera.activeCallbacks.fetch_sub(
                1, std::memory_order_acq_rel);
        }
    }

    // ---- Reach second muzzle flash -----------------------------------------
    // Puts the face-stuck muzzle element onto the controller-held gun, beside
    // the one that already tracks it. See kReachEffectLocationResolverRva for
    // the full disassembly and why effect[0x50] is a sound discriminator.
    //
    // This deliberately does NOT hook the object marker query 0x00471C30 or
    // 0x0047044C. Those are genuinely shared with the projectile chain, and a
    // previous candidate that moved a shared marker consumer produced the
    // rejected "projectile origin too far right and rearward" result. This
    // redirects only the resolver's own first-person branch, for effects that
    // already declare a first-person weapon user.
    using ReachEffectLocationFn =
        void(__fastcall*)(void*, uintptr_t, void*);
    using ReachEffectFpMarkerFn =
        void(__fastcall*)(int, int, unsigned int, void*);
    ReachEffectLocationFn g_reachOrigEffectLocation = nullptr;
    ReachEffectFpMarkerFn g_reachEffectFpMarkerQuery = nullptr;
    std::atomic<uint32_t> g_reachMuzzleRedirects{0};
    std::atomic<uint32_t> g_reachMuzzleReadFailures{0};
    // Diagnostic, worker-logged only. The 2026-07-27 headset test proved the
    // hook installs and the first-person query decodes, yet the flash stayed on
    // the player's face - and nothing in the log could say which branch the
    // muzzle effect actually takes. These counters separate the three
    // possibilities that produce identical silence:
    //   noFpUser  - effect[0x50] low nibble is 0, so the engine itself treats
    //               this effect as world-only and the redirect can never fire
    //   fpNone    - a first-person user exists but the output index is 'none'
    //   alreadyFp - the location was already a first-person location
    // A bitmask of every distinct effect[0x50] seen is recorded too, because
    // "which values actually occur" is the fact that decides the real fix.
    // Identity-fallback repair. The stuck muzzle element is a location whose
    // marker lookup FAILED, so the resolver handed it the global identity
    // matrix and it renders at the origin of the view. Its sibling particle
    // systems on the SAME effect resolve their markers fine and land on the
    // controller-held gun. So: remember the last transform that resolved for
    // this effect, and when a sibling comes back as the identity fallback,
    // give it that transform instead. The systems line up.
    const float* g_reachEffectIdentity = nullptr;   // into the retained module
    thread_local const void* g_reachLastGoodEffect = nullptr;
    thread_local float g_reachLastGoodMatrix[kReachEffectMatrixFloats]{};
    thread_local bool g_reachLastGoodValid = false;
    std::atomic<uint32_t> g_reachMuzzleRepaired{0};
    std::atomic<uint32_t> g_reachMuzzleIdentityNoSibling{0};

    // True when the resolver returned the untouched identity fallback, i.e. the
    // marker was not found. Exact 13-float comparison against the module's own
    // global, so nothing that resolved a real marker can be mistaken for it.
    __declspec(noinline) bool ReachMatrixIsIdentityFallback(const float* m)
    {
        const float* identity = g_reachEffectIdentity;
        if (!identity || !m)
            return false;
        __try
        {
            for (size_t i = 0; i < kReachEffectMatrixFloats; ++i)
                if (m[i] != identity[i])
                    return false;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __declspec(noinline) bool ReachCopyMatrix(float* dst, const float* src)
    {
        __try
        {
            for (size_t i = 0; i < kReachEffectMatrixFloats; ++i)
            {
                if (!isfinite(src[i]))
                    return false;
            }
            for (size_t i = 0; i < kReachEffectMatrixFloats; ++i)
                dst[i] = src[i];
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::atomic<uint32_t> g_reachMuzzleBodyCopiesHidden{0};
    // Definition index of the player's own weapon effect, captured by the hot
    // hook for the retarget worker. 0xFFFFFFFF = nothing captured.
    std::atomic<uint32_t> g_reachMuzzleCapturedDefIndex{0xFFFFFFFFu};

    // ---- the weapon anchor the muzzle flash needs --------------------------
    // Why the flash sits on the player's face: our palette work never moves the
    // game's skeleton. ReconstructVisiblePaletteSource builds the
    // controller-aligned weapon into g_fpPaletteScratch and hands THAT to the
    // renderer, and ReachRestoreFpLiveGraph puts the live graph back. So the
    // gun you see is composed from a scratch copy while the engine's real
    // weapon stays where it always was - at the player's body. Anything that
    // samples the real skeleton afterwards, like the effect system asking for
    // the muzzle marker, resolves against the un-moved weapon.
    //
    // That is also why four candidates aimed at effect LOCATIONS did nothing:
    // the location was resolving correctly the whole time, against a weapon we
    // had already put back.
    //
    // The palette already computes both transforms. Publishing them lets the
    // effect hook re-parent a marker from the stock weapon onto the controller
    // one exactly - full rigid transform, so there is no offset.
    struct ReachWeaponAnchor
    {
        std::atomic<uint32_t> seq{0};   // even = stable (single writer)
        std::atomic<uint32_t> generation{0};
        BoneMatrix stock{};
        BoneMatrix moved{};
    };
    ReachWeaponAnchor g_reachWeaponAnchor;
    // Highest collection index seen per CHUD definition. HREK shows the HUD
    // muzzle-flash collection (`warning_flashes`) is always the LAST one, and
    // retail strips the name, so the index is the only handle. Learned rather
    // than hardcoded because the index differs per weapon (3 on the assault
    // rifle, 4 on magnum/dmr/sniper).
    constexpr size_t kReachChudMaxCollectionSlots = 256;
    std::atomic<uint8_t> g_reachChudMaxCollection[kReachChudMaxCollectionSlots]{};
    std::atomic<uint32_t> g_reachHudFlashHidden{0};
    // Historical 511eb0b candidate. It copied the absolute-world
    // controller wrist directly into one local-space live-skeleton record
    // after palette composition. That write did not fix the muzzle flash;
    // b942078 later fixed it in loaded tag data. Keep the old write dormant
    // for evidence and count every exact invocation that would have reached it.
    constexpr bool kReachPostPaletteWorldWristWriteEnabled = false;
    thread_local bool g_reachWeaponAnchorPending = false;
    thread_local BoneMatrix g_reachWeaponAnchorMoved{};
    std::atomic<uint32_t> g_reachLiveGraphWeaponWrites{0};
    std::atomic<uint32_t> g_reachWorldWristWritesPrevented{0};

    void ReachPublishWeaponAnchor(
        const BoneMatrix& stock, const BoneMatrix& moved, uint32_t generation)
    {
        const uint32_t start =
            g_reachWeaponAnchor.seq.load(std::memory_order_relaxed);
        g_reachWeaponAnchor.seq.store(start + 1, std::memory_order_release);
        g_reachWeaponAnchor.stock = stock;
        g_reachWeaponAnchor.moved = moved;
        g_reachWeaponAnchor.generation.store(
            generation, std::memory_order_relaxed);
        g_reachWeaponAnchor.seq.store(start + 2, std::memory_order_release);
    }

    bool ReachReadWeaponAnchor(BoneMatrix& stock, BoneMatrix& moved)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const uint32_t before =
                g_reachWeaponAnchor.seq.load(std::memory_order_acquire);
            if (before == 0 || (before & 1u))
                continue;
            if (g_reachWeaponAnchor.generation.load(
                    std::memory_order_relaxed) != g_reachCamera.generation)
            {
                return false;
            }
            stock = g_reachWeaponAnchor.stock;
            moved = g_reachWeaponAnchor.moved;
            if (g_reachWeaponAnchor.seq.load(std::memory_order_acquire) ==
                before)
            {
                return true;
            }
        }
        return false;
    }

    // How near the stock weapon a marker must sit to count as belonging to it.
    // The AR's own muzzle_flash marker is 0.23 world units from the weapon
    // origin, and world units are ~3 m, so 0.5 covers any weapon's markers
    // while staying far inside the distance to another character.
    constexpr float kReachWeaponMarkerRadius = 0.5f;
    // Candidate b510b5e required the engine's first-person weapon-owner bit
    // before re-parenting. The exact headset run proved that gate executed and
    // left 215,356 no-owner world effects untouched, but the black world
    // geometry was unchanged. Keep the rejected behavior dormant rather than
    // stacking the next candidate onto it.
    constexpr bool kReachEffectOwnerGateEnabled = false;
    std::atomic<uint32_t> g_reachMuzzleReparented{0};
    std::atomic<uint32_t> g_reachMuzzleOutOfRange{0};
    std::atomic<uint32_t> g_reachMuzzleNearestMilli{0xFFFFFFFFu};

    // R * v and R^T * v for the row-major 3x3 in a BoneMatrix.
    inline void ReachRotate(const float* r, const float* v, float* out)
    {
        out[0] = r[0] * v[0] + r[1] * v[1] + r[2] * v[2];
        out[1] = r[3] * v[0] + r[4] * v[1] + r[5] * v[2];
        out[2] = r[6] * v[0] + r[7] * v[1] + r[8] * v[2];
    }
    inline void ReachRotateTranspose(const float* r, const float* v, float* out)
    {
        out[0] = r[0] * v[0] + r[3] * v[1] + r[6] * v[2];
        out[1] = r[1] * v[0] + r[4] * v[1] + r[7] * v[2];
        out[2] = r[2] * v[0] + r[5] * v[1] + r[8] * v[2];
    }

    // Rebuild an effect matrix that sits on the stock weapon so it sits on the
    // controller-aligned weapon instead, preserving its exact local offset and
    // orientation. Guarded; leaves the matrix untouched on any fault or if it
    // is not on the stock weapon.
    __declspec(noinline) bool ReachReparentEffectMatrix(
        float* matrix, const BoneMatrix& stock, const BoneMatrix& moved)
    {
        __try
        {
            float* position = matrix + (kReachEffectMatrixPositionOffset / 4);
            float* rotation = matrix + 1;   // BoneMatrix: scale, rotation[9]
            const float delta[3] = {
                position[0] - stock.translation[0],
                position[1] - stock.translation[1],
                position[2] - stock.translation[2]};
            const float distanceSquared = delta[0] * delta[0] +
                delta[1] * delta[1] + delta[2] * delta[2];
            if (!isfinite(distanceSquared))
                return false;
            // Cheapest possible visibility into what the hook actually sees:
            // the closest any effect matrix came to the stock weapon. If the
            // repair never fires, this number says whether it was a near miss
            // or whether nothing is resolving near the weapon at all.
            const uint32_t milli = static_cast<uint32_t>(
                sqrtf(distanceSquared) * 1000.0f);
            uint32_t nearest = g_reachMuzzleNearestMilli.load(
                std::memory_order_relaxed);
            while (milli < nearest &&
                   !g_reachMuzzleNearestMilli.compare_exchange_weak(
                       nearest, milli, std::memory_order_relaxed))
            {
            }
            if (distanceSquared >
                kReachWeaponMarkerRadius * kReachWeaponMarkerRadius)
            {
                g_reachMuzzleOutOfRange.fetch_add(
                    1, std::memory_order_relaxed);
                return false;
            }
            // Translation only, and deliberately so. The anchor is now
            // head -> gun, not weapon -> weapon, so rotating by
            // R_moved * R_head^T would spin the flash by the difference between
            // where the player looks and where they point - visible garbage.
            // The flash's own orientation already comes from the engine's
            // weapon, which controller aim steers down the controller ray, so
            // it is kept exactly as resolved. Only the position moves, off the
            // head and onto the gun.
            (void)rotation;
            float newPosition[3] = {
                position[0] + (moved.translation[0] - stock.translation[0]),
                position[1] + (moved.translation[1] - stock.translation[1]),
                position[2] + (moved.translation[2] - stock.translation[2])};
            // Headset calibration (muzzle_height_m): the translation-only
            // transfer above preserves the authored marker offset exactly, so
            // the origin lands on the barrel line but at the authored height,
            // which reads as a few inches low in VR. Lift it along the GUN's
            // own up axis, not world up, so it rolls with the weapon.
            //
            // This is the effect origin only. It runs after the projectile's
            // origin and direction are resolved, so the impact point the user
            // confirmed as already correct is not touched. Reach-only in
            // practice: this hook is on Reach's effect-location resolver.
            const float lift = g_config.muzzle_height_m;
            if (lift != 0.0f)
            {
                // moved is a BoneMatrix, whose 3x3 is row-major with world =
                // R * local (see ReachRotate above). Halo authors weapon
                // frames +X forward / +Z up, which the HREK primary_trigger
                // record corroborates: direction +X along the barrel.
                const float localUp[3] = {0.0f, 0.0f, 1.0f};
                float worldUp[3];
                ReachRotate(moved.rotation, localUp, worldUp);
                const float length = sqrtf(worldUp[0] * worldUp[0] +
                    worldUp[1] * worldUp[1] + worldUp[2] * worldUp[2]);
                if (isfinite(length) && length > 1.0e-4f)
                {
                    const float scale =
                        lift * kReachWorldUnitsPerMeter / length;
                    for (int i = 0; i < 3; ++i)
                        newPosition[i] += worldUp[i] * scale;
                }
            }
            for (int i = 0; i < 3; ++i)
                if (!isfinite(newPosition[i]))
                    return false;
            position[0] = newPosition[0];
            position[1] = newPosition[1];
            position[2] = newPosition[2];
            g_reachMuzzleReparented.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Push a resolved effect location far below the world so nothing it spawns
    // is visible. Guarded; leaves the matrix untouched on any read/write fault.
    __declspec(noinline) bool ReachDisplaceEffectLocation(void* matrix)
    {
        __try
        {
            float* position = reinterpret_cast<float*>(
                static_cast<unsigned char*>(matrix) +
                kReachEffectMatrixPositionOffset);
            position[0] = 0.0f;
            position[1] = 0.0f;
            position[2] = kReachEffectHiddenPositionZ;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::atomic<uint32_t> g_reachMuzzleWorldNoFpUser{0};
    std::atomic<uint32_t> g_reachMuzzleWorldFpNone{0};
    std::atomic<uint32_t> g_reachMuzzleAlreadyFp{0};
    std::atomic<uint32_t> g_reachMuzzleFpByteLowMask{0};
    std::atomic<uint32_t> g_reachMuzzleFpByteHighMask{0};

    // Guarded read of the two effect fields the decision needs.
    bool ReachReadEffectFpFields(
        const void* effect, unsigned char& fpUserByte, int& objectIndex,
        uint32_t& definitionIndex)
    {
        __try
        {
            const unsigned char* bytes =
                static_cast<const unsigned char*>(effect);
            fpUserByte = bytes[kReachEffectFpUserByteOffset];
            objectIndex = *reinterpret_cast<const int*>(
                bytes + kReachEffectObjectIndexOffset);
            definitionIndex = *reinterpret_cast<const uint32_t*>(bytes + 0x08);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    __declspec(noinline) void __fastcall ReachEffectLocationDetour(
        void* effect, uintptr_t nodeDesignator, void* outMatrix)
    {
        g_reachCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        __try
        {
            ReachEffectLocationFn original = g_reachOrigEffectLocation;
            if (!original)
                return;
            ReachEffectFpMarkerFn fpQuery = g_reachEffectFpMarkerQuery;
            if (!fpQuery || !effect || !outMatrix ||
                !g_reachCamera.armed.load(std::memory_order_acquire) ||
                !g_enabled.load(std::memory_order_acquire))
            {
                original(effect, nodeDesignator, outMatrix);
                return;
            }
            unsigned char fpUserByte = 0;
            int objectIndex = 0;
            uint32_t effectDefinitionIndex = 0xFFFFFFFFu;
            if (!ReachReadEffectFpFields(effect, fpUserByte, objectIndex,
                                         effectDefinitionIndex))
            {
                g_reachMuzzleReadFailures.fetch_add(
                    1, std::memory_order_relaxed);
                original(effect, nodeDesignator, outMatrix);
                return;
            }
            // ---- take the player's own weapon effects off their body -------
            // The first-person marker query has exactly ONE caller: this
            // resolver's own tail jump at 0x00120F0A. Runtime counters over
            // ~275,000 resolutions show that branch executes ZERO times, so
            // Reach never places an effect location in first-person space -
            // every one resolves through the object marker query onto the WORLD
            // weapon. In flat play that is invisible because the world weapon
            // is not drawn in first person. In VR it sits at the player's body,
            // which is the muzzle flash welded to their face.
            //
            // effect[0x50] low nibble is first_person_weapon_user_mask. It is
            // non-zero ONLY for the local player's own first-person weapon
            // effects, and zero for every other character's weapon, every
            // impact, and every explosion - measured: 13,998 with a mask
            // against 262,665 without, in 30 s of firing. That is the exact
            // discriminator, and it is why this cannot touch anyone else's
            // muzzle flash.
            //
            // The flash on the controller-held gun is drawn by the first-person
            // weapon's own render path, not by this resolver, so it is
            // untouched. Displacing the world copy is what stock first-person
            // play effectively does by never showing it.
            // DISABLED after a headset test. This displaced ~14,000 effect
            // locations per 30 s - the counters prove it fired, because
            // world/fp-output-none fell from 13,998 to 0 - and the flash on the
            // player's face did not move. So the element is NOT positioned
            // through this resolver, which is now the THIRD independent
            // measurement saying so (zero first-person designators, zero
            // identity fallbacks, and now this). Nothing unproven stays active.
            constexpr bool kReachHideBodyWeaponEffects = false;
            if (kReachHideBodyWeaponEffects &&
                (fpUserByte & 0x0Fu) != 0u &&
                static_cast<short>(nodeDesignator & 0xFFFFu) >= 0)
            {
                if (ReachDisplaceEffectLocation(outMatrix))
                {
                    g_reachMuzzleBodyCopiesHidden.fetch_add(
                        1, std::memory_order_relaxed);
                    return;
                }
            }

            ReachEffectFpDecision decision =
                ReachDecideEffectLocation(
                    fpUserByte, static_cast<unsigned int>(nodeDesignator));
            // DISABLED 2026-07-27 after a headset failure, kept for evidence.
            //
            // This redirect fired 26,576 times in 30 s of shooting and the
            // face-stuck muzzle flash did not move at all (REACHFX line,
            // candidate 6c4e796). That is conclusive: the flash the player sees
            // is not placed through this resolver's world path, so the redirect
            // buys nothing while relocating tens of thousands of unrelated
            // first-person weapon effect placements per half minute - exactly
            // the kind of unproven side effect that produced the rejected
            // projectile-origin result earlier in this project.
            //
            // HREK tag data says why the model was wrong: the muzzle flash is a
            // particle_system_definition_block_new gated by the static
            // effect_camera_modes enum (1 = only in first person, 2 = only in
            // third person) at particle-system element +0x1E, not an
            // effect_part placed at a marker location. The face-stuck element
            // is almost certainly the THIRD-PERSON set, which never renders
            // during real first-person play.
            //
            // The counters above stay live so the next candidate can still see
            // what this path does. Re-enabling this requires new evidence that
            // the flash actually resolves here.
            constexpr bool kReachMuzzleRedirectEnabled = false;
            if (!kReachMuzzleRedirectEnabled)
                decision.redirect = false;
            if (!decision.redirect)
            {
                // Record WHY, so one run distinguishes the three ways this can
                // decline. Atomics only - no logging on this path.
                const short designator =
                    static_cast<short>(nodeDesignator & 0xFFFFu);
                if (designator < 0)
                {
                    g_reachMuzzleAlreadyFp.fetch_add(
                        1, std::memory_order_relaxed);
                }
                else if ((fpUserByte & 0x0Fu) == 0u)
                {
                    g_reachMuzzleWorldNoFpUser.fetch_add(
                        1, std::memory_order_relaxed);
                    g_reachMuzzleFpByteLowMask.fetch_or(
                        1u << (fpUserByte & 0x0Fu), std::memory_order_relaxed);
                    g_reachMuzzleFpByteHighMask.fetch_or(
                        1u << ((fpUserByte >> 4) & 0x0Fu),
                        std::memory_order_relaxed);
                }
                else
                {
                    g_reachMuzzleWorldFpNone.fetch_add(
                        1, std::memory_order_relaxed);
                    g_reachMuzzleFpByteHighMask.fetch_or(
                        1u << ((fpUserByte >> 4) & 0x0Fu),
                        std::memory_order_relaxed);
                    // The player's own weapon effect: hand its definition to
                    // the retarget worker. Relaxed store, latest wins.
                    g_reachMuzzleCapturedDefIndex.store(
                        effectDefinitionIndex, std::memory_order_relaxed);
                }
                // The engine resolves the matrix HERE. Only after this call
                // does outMatrix hold a real marker transform; the previous
                // candidate re-parented beforehand, against uninitialised
                // memory, and the engine then overwrote it - which is exactly
                // why the probe came back with the nearest-approach sentinel
                // untouched and zero re-parents.
                original(effect, nodeDesignator, outMatrix);
            // ---- re-parent the face-stuck muzzle flash onto the gun --------
            // The resolver hands back a marker matrix built against the
            // engine's weapon, which our palette work never moved. If that
            // matrix sits on the stock weapon, rebuild it against the
            // controller-aligned weapon: express the marker in the stock
            // weapon's local frame, then re-apply that exact local offset and
            // orientation on the moved weapon. Rigid and exact - the marker
            // keeps its real position on the barrel, so there is no offset.
            //
            // Candidate b510b5e required both ownership and position after a
            // preserved log disproved the claim that proximity implied weapon
            // ownership. The gate worked exactly as intended, but its headset
            // result left the black world geometry unchanged. It is disabled
            // above and retained only as evidence; the next candidate starts
            // from the accepted E490 behavior.
            {
                BoneMatrix stockWeapon{};
                BoneMatrix movedWeapon{};
                float* resolved = static_cast<float*>(outMatrix);
                if ((!kReachEffectOwnerGateEnabled ||
                     (fpUserByte & 0x0Fu) != 0u) &&
                    g_reachCamera.armed.load(std::memory_order_acquire) &&
                    g_enabled.load(std::memory_order_acquire) &&
                    ReachReadWeaponAnchor(stockWeapon, movedWeapon))
                {
                    ReachReparentEffectMatrix(resolved, stockWeapon,
                                              movedWeapon);
                }
            }

                return;
            }
            // Exactly the call the engine's own first-person branch makes,
            // with the arguments it builds, so the world copy of this weapon
            // effect lands on the first-person weapon beside the one that
            // already tracks the controller.
            fpQuery(decision.userIndex, objectIndex, decision.markerIndex,
                    outMatrix);
            g_reachMuzzleRedirects.fetch_add(1, std::memory_order_relaxed);
        }
        __finally
        {
            g_reachCamera.activeCallbacks.fetch_sub(
                1, std::memory_order_acq_rel);
        }
    }

    // ---- Reach observer-camera re-parenting --------------------------------
    // Takes the rain, objective markers and character name tags OFF the hand.
    // They are not a HUD "layer" - they are every consumer of Reach's observer
    // camera, which our aim steering points down the controller ray while the
    // world renders from a private head copy. See
    // kReachRenderCameraFromObserverRva for the full derivation and the proven
    // destination layout.
    //
    // This does NOT apply head-look a second time. ReachApplyHeadLook consumes
    // one-shot state (g_needRecenter and g_reachCineCutRealign, both exchange())
    // so calling it again per frame would silently eat the manual recenter and
    // the accepted cutscene yaw realign. Instead this copies the camera the
    // world was ALREADY rendered from for this exact eye, which also guarantees
    // the markers can never disagree with the world they are drawn over.
    using ReachObserverCameraFn = void(__fastcall*)(void*, const void*);
    ReachObserverCameraFn g_reachOrigObserverCamera = nullptr;
    std::atomic<uint32_t> g_reachObserverCameraSiteHits[6]{};
    std::atomic<uint32_t> g_reachObserverCameraCorrected[6]{};
    std::atomic<uint32_t> g_reachObserverCameraUnknownSite{0};

    __declspec(noinline) void __fastcall ReachObserverCameraDetour(
        void* dst, const void* src)
    {
        const uintptr_t returnAddress =
            reinterpret_cast<uintptr_t>(_ReturnAddress());
        g_reachCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        __try
        {
            ReachObserverCameraFn original = g_reachOrigObserverCamera;
            if (!original)
                return;
            // Always run the engine's own build first, unconditionally, on
            // every path. A skipped call leaves the destination camera
            // uninitialised; correcting it afterwards is purely additive.
            original(dst, src);

            const uintptr_t base = g_reachCamera.base;
            if (!dst || !base || returnAddress <= base)
                return;
            const int site =
                ReachClassifyObserverCameraReturn(returnAddress - base);
            if (site < 0)
            {
                g_reachObserverCameraUnknownSite.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            g_reachObserverCameraSiteHits[site].fetch_add(
                1, std::memory_order_relaxed);
            // The world render camera is the headset-accepted path and already
            // carries head-look. Touching it would double-apply and put the
            // accepted Reach 3D at risk, so it is never corrected.
            if (site == kReachObserverCameraWorldSite)
                return;
            if (!ReachOwnsHudStereoTransaction())
                return;
            const ReachFpCameraEyeScope& scope = g_reachFpCameraEyeScope;
            if (!scope.active ||
                scope.generation != g_reachCamera.generation)
            {
                return;
            }
            // Destination and the cached head camera share the compact layout
            // exactly (proven by this function's own copy block), so this is a
            // straight three-field copy: position, forward, up. Field of view
            // and everything else the engine derived is left alone.
            const float* headPos =
                reinterpret_cast<const float*>(scope.compact + 0x00);
            const float* headFwd =
                reinterpret_cast<const float*>(scope.compact + 0x0C);
            const float* headUp =
                reinterpret_cast<const float*>(scope.compact + 0x18);
            for (int i = 0; i < 3; ++i)
            {
                if (!isfinite(headPos[i]) || !isfinite(headFwd[i]) ||
                    !isfinite(headUp[i]))
                {
                    return;
                }
            }
            unsigned char* out = static_cast<unsigned char*>(dst);
            memcpy(out + 0x00, headPos, sizeof(float) * 3);
            memcpy(out + 0x0C, headFwd, sizeof(float) * 3);
            memcpy(out + 0x18, headUp, sizeof(float) * 3);
            g_reachObserverCameraCorrected[site].fetch_add(
                1, std::memory_order_relaxed);
        }
        __finally
        {
            g_reachCamera.activeCallbacks.fetch_sub(
                1, std::memory_order_acq_rel);
        }
    }

    __declspec(noinline) void __fastcall ReachHudDrawWidgetDetour(
        int userIndex, void* descriptor, unsigned int widgetIndex,
        unsigned int useAlternatePath, void* drawState)
    {
        bool captureStarted = false;
        g_reachCamera.activeCallbacks.fetch_add(
            1, std::memory_order_acq_rel);
        __try
        {
            ReachHudDrawWidgetFn original = g_reachOrigHudDrawWidget;
            if (!original)
                return;

            const bool ownsStereo = ReachOwnsHudStereoTransaction();
            const bool matchingEyeScope =
                g_reachFpCameraEyeScope.active &&
                g_reachFpCameraEyeScope.generation ==
                    g_reachCamera.generation;
            if (matchingEyeScope && !ownsStereo)
            {
                // Ownership can be revoked by teardown or an explicit VR
                // disable while this stock draw call is already in flight.
                // Mark the eye so it cannot be copied, but still run the
                // engine's draw: skipping it is what corrupts state (see the
                // +0x2ED80C crash note below).
                g_reachFpCameraEyeScope.chudParityFailed = true;
                original(userIndex, descriptor, widgetIndex,
                         useAlternatePath, drawState);
                return;
            }
            if (ownsStereo &&
                g_scopeRenderActive.load(std::memory_order_acquire))
            {
                // The magnified world-only scope picture must not receive a
                // native CHUD widget, but the call still has to happen.
                // Redirect entry: hiding must not depend on crosshair=1.
                if (VR_BeginAuthoredReticleRedirect())
                    captureStarted = true;
                original(userIndex, descriptor, widgetIndex,
                         useAlternatePath, drawState);
                return;
            }

            // descriptor+4 is a WIDGET INDEX, not the scripting class. The
            // class belongs to the owning collection, reached through
            // descriptor+3. Resolving it the way Reach does is the only way to
            // catch every piece of a crosshair: almost all drawn widgets
            // author their class as "undefined/use parent".
            uint8_t collectionIndexForFlash = 0;
            bool collectionIsFlash = false;
            int8_t resolvedClass = -1;
            const bool descriptorReadable = descriptor &&
                ResolveReachChudCollectionClass(
                    g_reachCamera.base, descriptor, useAlternatePath,
                    resolvedClass);
            const uint8_t rawClass = static_cast<uint8_t>(resolvedClass);
            const bool isCrosshairClass = descriptorReadable &&
                resolvedClass == kReachChudCrosshairScriptingClass;
            // Learn this definition's highest collection index, then treat that
            // collection as the HUD muzzle flash. Never the crosshair: the
            // crosshair collection is index 0 in every weapon HREK shows, so a
            // crosshair-class widget is excluded outright as a second guard.
            if (descriptorReadable && !isCrosshairClass &&
                SafeReadByte(
                    reinterpret_cast<const uint8_t*>(descriptor) +
                        kReachChudDescriptorCollectionByte,
                    &collectionIndexForFlash))
            {
                std::atomic<uint8_t>& slot =
                    g_reachChudMaxCollection[
                        (useAlternatePath & 0xFFu) %
                            kReachChudMaxCollectionSlots];
                uint8_t known = slot.load(std::memory_order_relaxed);
                while (collectionIndexForFlash > known &&
                       !slot.compare_exchange_weak(
                           known, collectionIndexForFlash,
                           std::memory_order_relaxed))
                {
                }
                // Require a settled maximum: only hide once this definition has
                // shown a collection ABOVE the ammo collections, so the first
                // frames cannot hide the bullet count while the max is still
                // being learned.
                const uint8_t settled = slot.load(std::memory_order_relaxed);
                collectionIsFlash = settled >= 3 &&
                    collectionIndexForFlash == settled;
            }
            if (ownsStereo && isCrosshairClass)
                g_reachFpCameraEyeScope.chudClass2Seen = true;
            // REACHHUD: atomics only, no logging on this hot hook.
            if (ownsStereo)
            {
                if (isCrosshairClass)
                {
                    g_reachChudClass2Draws.fetch_add(
                        1, std::memory_order_relaxed);
                    g_reachChudLastClass2Ms.store(
                        GetTickCount64(), std::memory_order_relaxed);
                }
                else if (!descriptorReadable)
                {
                    g_reachChudUnreadable.fetch_add(
                        1, std::memory_order_relaxed);
                    g_reachChudLastUnreadablePath.store(
                        static_cast<uint8_t>(useAlternatePath & 0xFFu),
                        std::memory_order_relaxed);
                }
                else
                {
                    // Readable, but its owning collection did not resolve to
                    // class 2. Previously counted by nothing, which left
                    // "0 unreadable, 0 rejects, no class-2" ambiguous between
                    // "Reach stopped drawing the crosshair" and "Reach is
                    // still drawing it and our class resolution stopped
                    // returning 2". The class comes from the owning
                    // COLLECTION via descriptor+3, and objective widgets can
                    // shift collection indices, so that second case is real.
                    // Record a class histogram (classes are the 9-value CHUD
                    // scripting enum; anything else buckets at the end).
                    g_reachChudOtherDraws.fetch_add(
                        1, std::memory_order_relaxed);
                    const size_t bucket = (resolvedClass >= 0 &&
                        resolvedClass < static_cast<int8_t>(
                            kReachChudClassBuckets - 1))
                        ? static_cast<size_t>(resolvedClass)
                        : kReachChudClassBuckets - 1;
                    g_reachChudClassCounts[bucket].fetch_add(
                        1, std::memory_order_relaxed);
                }
            }

            const int stereoEye =
                g_stereoEye.load(std::memory_order_relaxed);
            if (matchingEyeScope &&
                g_reachFpCameraEyeScope.chudParityFailed &&
                isCrosshairClass)
            {
                // Already-failed eye: hide the widget, but never skip the
                // call. Redirect entry for the same crosshair=0 reason.
                if (VR_BeginAuthoredReticleRedirect())
                    captureStarted = true;
                original(userIndex, descriptor, widgetIndex,
                         useAlternatePath, drawState);
                return;
            }
            const ReachChudCrosshairAction action =
                ReachDecideChudCrosshairAction(
                    ownsStereo, descriptorReadable,
                    static_cast<int8_t>(rawClass), g_config.crosshair,
                    g_config.kill_reticle,
                    stereoEye,
                    g_config.right_eye_first);

            // NEVER skip the engine's own draw call. Halo 3 and ODST can hide
            // their crosshair by NOPing a class-2 visibility predicate, so the
            // widget function still runs in full and merely declines to draw.
            // Reach has no such predicate (proven: docs/REACH-HANDOFF), so the
            // previous approach returned early instead - skipping the call
            // outright. That corrupts engine state: a headset test crashed
            // haloreach.dll at +0x2ED80C, the first instruction of a function
            // whose very first act is "mov eax,[rcx+0x98]", i.e. it was handed
            // a null pointer that the skipped call was supposed to establish.
            // The identical fault address appeared in an earlier attempt at
            // this same hook, so this is the mechanism, not a coincidence.
            //
            // Instead, always run the original and redirect its pixels into
            // the offscreen authored-reticle target when they must not reach
            // the eye. Begin/End already save and restore render targets,
            // viewports and scissors, so the engine draws normally and simply
            // lands somewhere the player cannot see.
            // The HUD muzzle flash. It is a flat CHUD widget, not a world
            // particle: a `blob` bitmap mirrored on both axes and scaled ~5x4,
            // driven by "weapon ammo loaded", so it pulses at the centre of the
            // screen on every shot and never moves with aim. That is exactly
            // what the player reports - an animated flipbook stuck to their
            // face, present on some weapons and not others.
            //
            // It lives in the `warning_flashes` collection, which HREK shows is
            // ALWAYS THE LAST collection: assault_rifle [3/4], magnum [4/5],
            // dmr [4/5], sniper_rifle [4/5]. `ammo_area` and
            // `backpack_ammo_area` - the bullet count and the weapon logo - are
            // always earlier, and the crosshair is always [0]. Hiding the
            // highest collection index therefore cannot take the ammo, the
            // logo, or the crosshair with it.
            //
            // The collection name is stripped from retail, so the index is the
            // only handle available; the max index per definition is learned
            // from what actually draws.
            // REVERTED 2026-07-27. This hid the GRENADE indicator: the
            // "always the last collection" rule held for the four weapon HUDs
            // HREK was checked against, but the last collection at runtime is
            // not warning_flashes - it was grenades. The player also states
            // plainly that the element is not a CHUD widget at all: not blue,
            // not vector art, a flipbook texture. CHUD is therefore ruled out
            // as the subsystem, and this rule was wrong on both counts.
            constexpr bool kReachHideHudMuzzleFlash = false;
            const bool hideHudMuzzleFlash = kReachHideHudMuzzleFlash &&
                ownsStereo && descriptorReadable && collectionIsFlash;
            if (hideHudMuzzleFlash)
                g_reachHudFlashHidden.fetch_add(1, std::memory_order_relaxed);

            const bool hideFromEye = hideHudMuzzleFlash ||
                action == ReachChudCrosshairAction::Suppress ||
                action == ReachChudCrosshairAction::CaptureAuthored ||
                action == ReachChudCrosshairAction::RejectTransaction;
            if (action == ReachChudCrosshairAction::RejectTransaction)
                RejectReachChudParityForCurrentEye();
            if (isCrosshairClass && hideFromEye)
            {
                // Fold this widget's identity in. Same weapon, same collection,
                // same widgets -> same key -> the art is unchanged and the
                // compositor can skip re-uploading it. This must cover every
                // path that actually redirects a crosshair widget, not just
                // the CaptureAuthored action: the art is captured through the
                // Suppress path as well, which is why the key stayed 0 and the
                // skip never engaged.
            }
            if (hideFromEye)
            {
                // Lazy-creating redirect that also ignores crosshair=0.
                // Two headset regressions came from this one call. The
                // PREPARED entry refuses unless every resource already
                // exists, and that refusal disarmed the core (armed
                // 01:29:20.548 -> stereo OFF .602). The plain capture entry
                // refuses when crosshair=0, and since Reach has no
                // visibility predicate and its alpha write is inert, that
                // refusal put the flat stock crosshair back on the eye for
                // players who asked for none.
                if (VR_BeginAuthoredReticleRedirect())
                {
                    captureStarted = true;
                    // Fold this widget's identity in AFTER the redirect
                    // begins, never before. Beginning a redirect on a new
                    // displayed frame is what CLEARS the capture surface and
                    // resets the per-frame key (see the serial check in
                    // BeginAuthoredReticleCaptureInternal). Folding first
                    // published a key describing content, and the clear then
                    // wiped the surface underneath it - so the key no longer
                    // described what the surface held, and the compositor
                    // could publish a blank or partial image over good art.
                    // That is the crosshair vanishing across a
                    // cinematic -> gameplay -> objective transition, where
                    // frames legitimately begin capturing in a different
                    // order. Folding here means the key is only ever built
                    // from widgets that actually reached the surface, on the
                    // surface that survived the clear.
                    if (isCrosshairClass && !hideHudMuzzleFlash)
                    {
                        ReachFpCameraEyeScope& scope =
                            g_reachFpCameraEyeScope;
                        scope.captureKey = FoldAuthoredCrosshairKey(
                            scope.captureKey, widgetIndex, useAlternatePath);
                        g_authoredCrosshairKeyAccum = scope.captureKey;
                        g_authoredCrosshairKey.store(
                            scope.captureKey, std::memory_order_release);
                    }
                }
                else
                    ReportReachRedirectUnavailable();
            }
            original(userIndex, descriptor, widgetIndex,
                     useAlternatePath, drawState);
        }
        __finally
        {
            if (captureStarted)
            {
                if (VR_EndPreparedAuthoredReticleCapture())
                {
                    g_reachFpCameraEyeScope.authoredCrosshairCaptured = true;
                    g_reachAuthoredCrosshairKey.store(
                        g_reachFpCameraEyeScope.captureKey,
                        std::memory_order_release);
                }
                else
                    // Same rule as the Begin side: restoring the render state
                    // failed for this widget, so this eye's art is incomplete
                    // and must not publish - but the camera core keeps running.
                    ReportReachRedirectUnavailable();
            }
            g_reachCamera.activeCallbacks.fetch_sub(
                1, std::memory_order_acq_rel);
        }
    }

    constexpr size_t kReachFpLayoutCacheCapacity = 4;
    // Retail 0x2AF648 emits at most four interpolation -> palette transactions
    // per invocation (two palette banks across two held-weapon slots). Keep one
    // bounded context for each transaction, exactly as Halo 3/ODST keep one per
    // weapon slot, then pair the final palette by its source pointer.
    constexpr size_t kReachFpTransactionCapacity = 4;
    struct ReachFpLayoutCacheEntry
    {
        bool valid = false;
        bool invalidateNextPair = false;
        uint32_t generation = 0;
        int liveSourceCount = 0;
        int interpolationView = 0;
        int interpolationId = 0;
        int interpolationSlot = 0;
        uint16_t bodyTag = 0;
        uint64_t learnedPreparedSerial = 0;
        ReachFpBodyLayout layout{};
    };
    struct ReachFpPairScope
    {
        bool armed = false;
        uint32_t generation = 0;
        uint64_t preparedSerial = 0;
        FpExplicitPoseTargets targets{};
        ReachFpLayoutCacheEntry layouts[kReachFpLayoutCacheCapacity]{};
    };
    struct ReachFpInterpolationContext
    {
        bool valid = false;
        bool transformed = false;
        uint32_t generation = 0;
        uint64_t preparedSerial = 0;
        uint64_t captureSerial = 0;
        BoneMatrix* source = nullptr;
        int liveSourceCount = 0;
        int interpolationView = 0;
        int interpolationId = 0;
        int interpolationSlot = 0;
        uint16_t bodyTag = 0;
        ReachFpBodyLayout layout{};
        FpExplicitPoseTargets targets{};
        BoneMatrix untouchedLive[kReachFpMaxSourceNodeCount]{};
    };
    thread_local ReachFpLayoutCacheEntry
        g_reachFpLayoutCache[kReachFpLayoutCacheCapacity];
    thread_local ReachFpPairScope g_reachFpPairScope;
    thread_local ReachFpInterpolationContext
        g_reachFpInterpolations[kReachFpTransactionCapacity];
    thread_local uint64_t g_reachFpCaptureSerial = 0;

    struct ReachFpStatus
    {
        std::atomic<uint64_t> key{0};
        std::atomic<uint32_t> generation{0};
        std::atomic<int> code{0};
        std::atomic<int> bodyCount{0};
        std::atomic<int> liveCount{0};
    } g_reachFpStatus;
    std::atomic<uint64_t> g_reachFpLoggedStatusKey{0};
    struct ReachFpCameraUploadStatus
    {
        std::atomic<uint64_t> preparedSerial{0};
        std::atomic<uint32_t> generation{0};
        std::atomic<uint32_t> eyeMask{0};
    } g_reachFpCameraUploadStatus;
    std::atomic<uint32_t> g_reachFpCameraLoggedGeneration{0};

    void ReachBeginFpPairScope(uint32_t generation, uint64_t preparedSerial,
                               const FpExplicitPoseTargets& targets);
    void ReachEndFpPairScope();

    // Reach's apply_distortions pass divides motion_blur_max by
    // motion_blur_scale. Zeroing both controls creates 0/0 NaNs in the
    // translucent screen-space pass. Match accepted Halo 3/ODST comfort
    // behavior without invalid constants: retain/reassert a positive authored
    // scale and zero only the maximum at each exact normal stereo boundary.
    // Restore both authored values only after both detours are quiescent. These
    // render-hook helpers do no logging, allocation, locking, or scanning.
    bool ReachRestoreMotionBlurValues()
    {
        if (!g_reachCamera.motionBlurResolved ||
            !g_reachCamera.motionBlurSuppressed)
        {
            return true;
        }
        for (MotionBlurVar& var : g_reachCamera.motionBlurVars)
        {
            if (!SafeWriteFloat(var.slot, var.original))
                return false;
        }
        g_reachCamera.motionBlurSuppressed = false;
        return true;
    }

    void ReachApplyMotionBlurSetting()
    {
        if (!g_reachCamera.motionBlurResolved)
            return;
        if (g_config.motion_blur)
        {
            ReachRestoreMotionBlurValues();
            return;
        }

        MotionBlurVar& scale = g_reachCamera.motionBlurVars[0];
        MotionBlurVar& maximum = g_reachCamera.motionBlurVars[1];
        float currentScale = 0.0f;
        float currentMaximum = 0.0f;
        if (!SafeReadFloat(scale.slot, &currentScale) ||
            !SafeReadFloat(maximum.slot, &currentMaximum) ||
            !std::isfinite(currentMaximum) || currentMaximum < 0.0f)
        {
            return;
        }

        // Tags may reload either control. Preserve a newly authored usable
        // scale/max, but repair an unexpected zero/near-zero scale by
        // reasserting the positive value captured at install.
        if (ReachMotionBlurScaleUsable(currentScale))
            scale.original = currentScale;
        if (!ReachMotionBlurScaleUsable(scale.original))
            return;
        if (currentMaximum != 0.0f)
            maximum.original = currentMaximum;

        // Mark restoration pending before the first possible write. If either
        // protected write fails after a partial mutation, config re-enable or
        // quiescent teardown must still restore both authored values.
        g_reachCamera.motionBlurSuppressed = true;
        if (!SafeWriteFloat(scale.slot, scale.original))
            return;
        if (!SafeWriteFloat(maximum.slot, 0.0f))
            return;
    }

    // Reach draws patchy fog as screen-aligned translucent noise sheets. In
    // sequential stereo those sheets move opposite the tracked head even while
    // world fog and geometry are correct. The same exact owned-eye boundary is
    // also where the title's dynamic lightmap-shadow pass is suppressed. That
    // proven object-caster -> static-lightmap receiver boundary matches the
    // reported headset symptom and is isolated here as one candidate. Both
    // title controls are restored in __finally. Unclaimed, nested, screenshot,
    // and flat renders never enter this scope.
    bool ReachCallPlayerViewWithEyeScopedSuppressions(uintptr_t playerView)
    {
        uint8_t original = 0;
        bool suppressionWritten = false;
        bool callReturned = false;
        bool restoreSucceeded = true;
        uint8_t lightmapOriginal = 0;
        bool lightmapChanged = false;
        bool lightmapAlreadyDisabled = false;
        bool lightmapRestoreSucceeded = true;

        // A prior exceptional eye is never allowed to redefine zero as its
        // entry state. Recover our outstanding write before rendering again;
        // failure drops only this frame and the next frame will retry.
        if (g_reachLightmapShadowRestoreOutstanding.load(
                std::memory_order_acquire) &&
            !RestoreReachLightmapShadowsControl())
        {
            g_reachLightmapShadowRestoreFailures.fetch_add(
                1, std::memory_order_relaxed);
            return false;
        }
        __try
        {
            uint8_t* const lightmap =
                g_reachCamera.lightmapShadowsEnabled;
            if (!lightmap)
            {
                // Exact proof was unavailable; only this candidate stays stock.
            }
            else if (SafeReadByte(lightmap, &lightmapOriginal) &&
                     lightmapOriginal == 0)
            {
                lightmapAlreadyDisabled = true;
            }
            else if (lightmapOriginal == 1 && SafeWriteByte(lightmap, 0))
            {
                lightmapChanged = true;
                g_reachLightmapShadowOutstandingValue.store(
                    lightmapOriginal, std::memory_order_relaxed);
                g_reachLightmapShadowRestoreOutstanding.store(
                    true, std::memory_order_release);
            }
            else
            {
                // This feature alone stays stock for the eye. The worker-side
                // report makes the failure loud without logging in this hook.
                g_reachLightmapShadowWriteFailures.fetch_add(
                    1, std::memory_order_relaxed);
            }
            uint8_t* const flags = g_reachCamera.patchyFogFlags;
            if (flags && SafeReadByte(flags, &original) &&
                SafeWriteByte(
                    flags, ReachPatchyFogSuppressedFlags(original)))
            {
                suppressionWritten = true;
                g_reachOrigPlayerViewRender(playerView);
                callReturned = true;
            }
        }
        __finally
        {
            if (suppressionWritten)
            {
                uint8_t current = 0;
                uint8_t* const flags = g_reachCamera.patchyFogFlags;
                restoreSucceeded = flags && SafeReadByte(flags, &current) &&
                    SafeWriteByte(
                        flags,
                        ReachPatchyFogRestoredFlags(current, original));
            }
            if (lightmapChanged)
            {
                uint8_t* const lightmap =
                    g_reachCamera.lightmapShadowsEnabled;
                uint8_t restored = 0;
                lightmapRestoreSucceeded = lightmap &&
                    SafeWriteByte(lightmap, lightmapOriginal) &&
                    SafeReadByte(lightmap, &restored) &&
                    restored == lightmapOriginal;
                if (lightmapRestoreSucceeded)
                {
                    g_reachLightmapShadowOutstandingValue.store(
                        0, std::memory_order_relaxed);
                    g_reachLightmapShadowRestoreOutstanding.store(
                        false, std::memory_order_release);
                }
                else
                {
                    g_reachLightmapShadowRestoreFailures.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
        }
        if (callReturned && restoreSucceeded && lightmapRestoreSucceeded)
        {
            if (lightmapChanged)
            {
                g_reachLightmapShadowSuppressedEyes.fetch_add(
                    1, std::memory_order_relaxed);
            }
            else if (lightmapAlreadyDisabled)
            {
                g_reachLightmapShadowAlreadyDisabledEyes.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        return callReturned && restoreSucceeded && lightmapRestoreSucceeded;
    }

    uintptr_t ReachFpCameraWorkspaceIfLive(const void* view) noexcept
    {
        const ReachFpCameraEyeScope& scope = g_reachFpCameraEyeScope;
        const ReachOwnerScope& owner = g_reachOwnerScope;
        const uintptr_t base = g_reachCamera.base;
        if (!scope.active || g_reachNestedOuterSuppressed || !base ||
            !g_reachCamera.armed.load(std::memory_order_acquire) ||
            scope.generation != g_reachCamera.generation ||
            scope.eye > 1 ||
            g_stereoEye.load(std::memory_order_acquire) !=
                static_cast<int>(scope.eye) ||
            !owner.active || owner.workspace != scope.workspace ||
            owner.playerView != scope.playerView ||
            owner.preparedSerial != scope.preparedSerial ||
            !owner.renderAccess || !owner.renderAccess->active ||
            owner.renderAccess->preparedSerial != scope.preparedSerial)
        {
            return 0;
        }

        const int32_t depth = *reinterpret_cast<const int32_t*>(
            base + kReachCameraStackDepthRva);
        if (depth < 0 || depth > 3)
            return 0;
        const uintptr_t topWorkspace = *reinterpret_cast<const uintptr_t*>(
            base + kReachCameraStackPointersRva +
            static_cast<uintptr_t>(depth) * sizeof(uintptr_t));
        const uintptr_t workspaceCallback =
            *reinterpret_cast<const uintptr_t*>(
                base + kReachFpCameraWorkspaceRva +
                kReachFpCameraWorkspaceCallbackOffset);
        const uintptr_t nestedWorkspace =
            SelectReachFpCameraNestedWorkspace(
                base, g_reachCamera.size, topWorkspace, workspaceCallback,
                reinterpret_cast<uintptr_t>(view));
        if (!nestedWorkspace ||
            *reinterpret_cast<const uintptr_t*>(
                base + kReachActiveViewRva) != scope.playerView ||
            *reinterpret_cast<const uint32_t*>(
                base + kReachSelectedSpecializationRva) != 0)
        {
            return 0;
        }
        return nestedWorkspace;
    }

    void PublishReachFpCameraUpload(
        const ReachFpCameraEyeScope& scope) noexcept
    {
        const uint64_t serial = scope.preparedSerial;
        if (g_reachFpCameraUploadStatus.preparedSerial.load(
                std::memory_order_relaxed) != serial)
        {
            g_reachFpCameraUploadStatus.eyeMask.store(
                0, std::memory_order_relaxed);
            g_reachFpCameraUploadStatus.generation.store(
                scope.generation, std::memory_order_relaxed);
            g_reachFpCameraUploadStatus.preparedSerial.store(
                serial, std::memory_order_release);
        }
        g_reachFpCameraUploadStatus.eyeMask.fetch_or(
            uint32_t{1} << scope.eye, std::memory_order_release);
    }

    void ReachFpCameraRebuildBody(void* view, bool firstPersonEnabled)
    {
        ReachFpCameraRebuildFn original = g_reachOrigFpCameraRebuild;
        if (!original)
            return;

        // Preserve the title's complete first-person rebuild and FOV side
        // effects. During one exact admitted eye render, replace its final
        // compact camera and derived/projection pair with that eye's already
        // rebuilt world pair, then rerun Reach's own constant uploader. This is
        // the same last-writer transaction used by Halo 3 and ODST.
        original(view, firstPersonEnabled);
        if (!g_reachFpCameraUpload)
            return;

        ReachFpCameraEyeScope& scope = g_reachFpCameraEyeScope;
        const uintptr_t nestedWorkspace =
            ReachFpCameraWorkspaceIfLive(view);
        if (!nestedWorkspace)
            return;
        void* const compact = reinterpret_cast<void*>(
            nestedWorkspace);
        void* const derived = reinterpret_cast<void*>(
            nestedWorkspace + kReachSecondaryDerivedOffset);
        memcpy(compact, scope.compact, sizeof(scope.compact));
        memcpy(derived, scope.derived, sizeof(scope.derived));
        g_reachFpCameraUpload(compact, derived);
        PublishReachFpCameraUpload(scope);
    }

    __declspec(noinline) void __fastcall ReachFpCameraRebuildDetour(
        void* view, bool firstPersonEnabled)
    {
        g_reachCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        __try
        {
            ReachFpCameraRebuildBody(view, firstPersonEnabled);
        }
        __finally
        {
            g_reachCamera.activeCallbacks.fetch_sub(
                1, std::memory_order_acq_rel);
        }
    }

    bool ReachOwnsSsaoEyeTransaction(uintptr_t returnAddress) noexcept
    {
        if (!ReachOwnsHudStereoTransaction() ||
            !g_reachCamera.ssaoIsolationActive.load(
                std::memory_order_acquire) ||
            g_reachNestedOuterSuppressed)
        {
            return false;
        }

        const uintptr_t base = g_reachCamera.base;
        const ReachFpCameraEyeScope& eyeScope = g_reachFpCameraEyeScope;
        const ReachOwnerScope& owner = g_reachOwnerScope;
        if (!base ||
            returnAddress != base + kReachSsaoCallRva + 5 ||
            eyeScope.eye > 1 ||
            g_stereoEye.load(std::memory_order_acquire) !=
                static_cast<int>(eyeScope.eye) ||
            !owner.active || owner.workspace != eyeScope.workspace ||
            owner.playerView != eyeScope.playerView ||
            owner.preparedSerial != eyeScope.preparedSerial ||
            !owner.renderAccess || !owner.renderAccess->active ||
            owner.renderAccess->preparedSerial != eyeScope.preparedSerial ||
            owner.cameraStackDepthBefore < -1 ||
            owner.cameraStackDepthBefore >= 3)
        {
            return false;
        }

        uint32_t depthRaw = 0;
        uintptr_t topWorkspace = 0;
        uintptr_t activeView = 0;
        uintptr_t workspaceCallback = 0;
        uint32_t specialization = 0;
        if (!SafeReadU32(
                reinterpret_cast<const void*>(
                    base + kReachCameraStackDepthRva),
                &depthRaw) ||
            !SafeReadPtr(
                reinterpret_cast<const void*>(
                    base + kReachCameraStackPointersRva +
                    static_cast<uintptr_t>(
                        owner.cameraStackDepthBefore + 1) *
                        sizeof(uintptr_t)),
                &topWorkspace) ||
            !SafeReadPtr(
                reinterpret_cast<const void*>(base + kReachActiveViewRva),
                &activeView) ||
            !SafeReadPtr(
                reinterpret_cast<const void*>(
                    owner.workspace + kReachRenderScopeSnapshotSize -
                    sizeof(uintptr_t)),
                &workspaceCallback) ||
            !SafeReadU32(
                reinterpret_cast<const void*>(
                    base + kReachSelectedSpecializationRva),
                &specialization))
        {
            return false;
        }

        const int32_t depth = static_cast<int32_t>(depthRaw);
        return depth == owner.cameraStackDepthBefore + 1 &&
            depth >= 0 && depth <= 3 &&
            topWorkspace == owner.workspace &&
            activeView == eyeScope.playerView &&
            workspaceCallback == base + kReachCameraStackCallbackRva &&
            specialization == 0;
    }

    __declspec(noinline) void __fastcall ReachRenderSsaoDetour(
        void* ssaoDefinition)
    {
        const uintptr_t returnAddress =
            reinterpret_cast<uintptr_t>(_ReturnAddress());
        g_reachCamera.activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
        __try
        {
            if (ReachOwnsSsaoEyeTransaction(returnAddress))
            {
                const uint32_t eye = g_reachFpCameraEyeScope.eye;
                g_reachSsaoSuppressedEyeCalls[eye].fetch_add(
                    1, std::memory_order_relaxed);
            }
            else
            {
                ReachRenderSsaoFn original = g_reachOrigRenderSsao;
                if (original)
                {
                    g_reachSsaoPassthroughCalls.fetch_add(
                        1, std::memory_order_relaxed);
                    original(ssaoDefinition);
                }
            }
        }
        __finally
        {
            g_reachCamera.activeCallbacks.fetch_sub(
                1, std::memory_order_acq_rel);
        }
    }

    // kReachSecondaryCompactOffset and the derived/render-bounds sub-block
    // offsets now live in reach_render_logic.h alongside the rest of the proven
    // workspace layout.
    // Player-view rollback: the camera-state block (+0x3B0) through the
    // last-window flag (+0xA30) inclusive.
    constexpr uintptr_t kReachPvSnapshotBegin = kReachPlayerViewCameraStateOffset;
    constexpr size_t kReachPvSnapshotBytes =
        kReachLastWindowFlagOffset + 1 - kReachPlayerViewCameraStateOffset;

    // Replicates Halo 3's proven ApplyHeadLook math (same engine family, same
    // world-axis convention: forward = (cos p cos y, cos p sin y, sin p), +Z up)
    // onto Reach's compact-camera offsets (pos +0x00, fwd +0x0C, up +0x18). It
    // shares the universal recenter/turn references so F-key recenter and stick
    // turn behave for Reach exactly as for Halo 3 and ODST.
    // Raised on the render thread when the probed cinematic state shows an
    // authored camera cut (ReachBuildHeadCullCamera); consumed exactly once by
    // ReachApplyHeadLook's realign branch in the same frame. The count feeds
    // the worker's confirmation log line.
    std::atomic<bool> g_reachCineCutRealign{false};
    std::atomic<uint32_t> g_reachCineCutCount{0};
    // Of those cuts, the ones the cinematic-globals stamp missed and only the
    // authored-camera discontinuity test caught. Reported alongside the total so
    // a headset log shows which detector is carrying the cutscene.
    std::atomic<uint32_t> g_reachCineCutPoseCount{0};

    // A Reach shot cut is a discontinuous jump of the authored cinematic camera.
    // These bounds separate that jump from every legitimate authored move: the
    // fastest sustained cinematic whip pan is well under 200 deg/sec, which is
    // ~1.7 deg on a 120 Hz frame and ~7 deg even across a 33 ms hitch, and the
    // fastest camera-follow moves (vehicle chase shots) travel under 1 world
    // unit per frame under the same hitch. Anything past these is a cut, not
    // motion. Both are deliberately loose: a missed cut leaves the previous
    // shot's facing (today's bug), while a false one snaps the view mid-shot.
    constexpr float kReachCineCutYawRadians = 0.35f;        // ~20 degrees
    constexpr float kReachCineCutJumpUnitsSq = 4.0f;        // 2 world units


    bool ReachApplyHeadLook(
        unsigned char* cam, const ReachVrRenderSnapshot& tracking)
    {
        float q[4] = {
            tracking.headOrientation[0], tracking.headOrientation[1],
            tracking.headOrientation[2], tracking.headOrientation[3]};
        const float hpos[3] = {
            tracking.headPosition[0], tracking.headPosition[1],
            tracking.headPosition[2]};
        float qLengthSquared = 0.0f;
        for (float component : q)
        {
            if (!isfinite(component))
                return false;
            qLengthSquared += component * component;
        }
        for (float component : hpos)
            if (!isfinite(component))
                return false;
        if (!isfinite(qLengthSquared) || qLengthSquared <= 1e-8f)
            return false;
        const float qLength = sqrtf(qLengthSquared);
        for (float& component : q)
            component /= qLength;
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float hfx = -2.0f * (w * y + x * z);
        const float hfy =  2.0f * (w * x - y * z);
        const float hfz = -(1.0f - 2.0f * (x * x + y * y));
        const float hy = atan2f(hfx, -hfz);
        const float hp = asinf(Clamp(hfy, -1.0f, 1.0f));

        const float hux = 2.0f * (x * y - w * z);
        const float huy = 1.0f - 2.0f * (x * x + z * z);
        const float huz = 2.0f * (y * z + w * x);
        float hrx = -hfz, hrz = hfx;
        float hrLen = sqrtf(hrx * hrx + hrz * hrz);
        if (hrLen < 1e-4f) hrLen = 1e-4f;
        hrx /= hrLen; hrz /= hrLen;
        const float hnux = -hfy * hrz;
        const float hnuy = hrLen;
        const float hnuz = hfy * hrx;
        const float headRoll = atan2f(hux * hrx + huz * hrz,
                                      hux * hnux + huy * hnuy + huz * hnuz);

        float* pos = reinterpret_cast<float*>(cam + 0x00);
        float* fwd = reinterpret_cast<float*>(cam + 0x0C);
        float* up = reinterpret_cast<float*>(cam + 0x18);

        // An authored camera cut realigns yaw only, exactly like Halo 3/ODST
        // at their scene/shot IDs: the current physical head-forward maps to
        // the new shot's authored facing. A manual recenter additionally
        // rebaselines the lean/position reference; a cut must not, or leaning
        // would snap mid-cinematic.
        const bool manualRecenter = g_needRecenter.exchange(false);
        const bool cutRealign = g_reachCineCutRealign.exchange(
            false, std::memory_order_acq_rel);
        if (manualRecenter || cutRealign)
        {
            g_gameYawRef = atan2f(fwd[1], fwd[0]);
            g_headYawRef = hy;
            if (manualRecenter)
            {
                g_headPosRef[0] = hpos[0]; g_headPosRef[1] = hpos[1];
                g_headPosRef[2] = hpos[2];
                g_needPosRecenter = false;
            }
        }
        else if (g_needPosRecenter.exchange(false))
        {
            g_headPosRef[0] = hpos[0]; g_headPosRef[1] = hpos[1];
            g_headPosRef[2] = hpos[2];
        }

        const float gy = g_gameYawRef + g_yawSign.load() * WrapPi(hy - g_headYawRef);
        const float gp = Clamp(g_pitchSign.load() * hp + g_pitchTrim.load(),
                               -1.5f, 1.5f);
        const float cgp = cosf(gp), sgp = sinf(gp), cgy = cosf(gy), sgy = sinf(gy);
        fwd[0] = cgp * cgy; fwd[1] = cgp * sgy; fwd[2] = sgp;
        if (g_writeUp.load())
        {
            const float cr = cosf(headRoll), sr = sinf(headRoll);
            up[0] = (-sgp * cgy) * cr + sgy * sr;
            up[1] = (-sgp * sgy) * cr - cgy * sr;
            up[2] = cgp * cr;
        }
        if (g_positional.load())
        {
            const float dx = hpos[0] - g_headPosRef[0];
            const float dy = hpos[1] - g_headPosRef[1];
            const float dz = hpos[2] - g_headPosRef[2];
            float hlen = sqrtf(hfx * hfx + hfz * hfz);
            if (hlen < 1e-4f) hlen = 1e-4f;
            const float hfhx = hfx / hlen, hfhz = hfz / hlen;
            const float fwdComp = dx * hfhx + dz * hfhz;
            const float rightComp = dx * (-hfhz) + dz * hfhx;
            const float s = kReachWorldUnitsPerMeter;
            float ox = (cgy * fwdComp + sgy * rightComp) * s;
            float oy = (sgy * fwdComp - cgy * rightComp) * s;
            float oz = dy * s;
            ox = Clamp(ox, -1.5f, 1.5f); oy = Clamp(oy, -1.5f, 1.5f);
            oz = Clamp(oz, -1.5f, 1.5f);
            pos[0] += ox; pos[1] += oy; pos[2] += oz;
        }
        return true;
    }

    // Snapshot both eyes once at the outer, pre-visibility boundary. Reach
    // rasterizes a symmetric fixed-aspect cover for each asymmetric OpenXR eye,
    // so the binocular cull is built from those actual widened raster corners
    // plus relative eye cant. The inner loop consumes the same snapshot instead
    // of re-reading a potentially different xrLocateViews result.
    bool ReachCollectEyeInputs(
        const ReachObservedRect& renderBounds,
        const ReachVrRenderSnapshot& tracking,
        ReachEyeRenderInput (&inputs)[2],
        ReachSymmetricFovCover& cullCover)
    {
        const int renderWidth =
            static_cast<int>(renderBounds.x1) - renderBounds.x0;
        const int renderHeight =
            static_cast<int>(renderBounds.y1) - renderBounds.y0;
        if (renderWidth <= 0 || renderHeight <= 0)
            return false;

        std::array<ReachEyeCullFrustum, 2> cullFrusta{};
        for (int eye = 0; eye < 2; ++eye)
        {
            const ReachVrEyeSnapshot& eyeSnapshot = tracking.eyes[eye];
            memcpy(inputs[eye].position, eyeSnapshot.position,
                   sizeof(inputs[eye].position));
            for (float component : inputs[eye].position)
                if (!isfinite(component))
                    return false;

            ReachEyeCullFrustum frustum{};
            frustum.angleLeft = eyeSnapshot.fov[0];
            frustum.angleRight = eyeSnapshot.fov[1];
            frustum.angleUp = eyeSnapshot.fov[2];
            frustum.angleDown = eyeSnapshot.fov[3];
            frustum.relativeOrientation = {
                eyeSnapshot.orientation[0], eyeSnapshot.orientation[1],
                eyeSnapshot.orientation[2], eyeSnapshot.orientation[3]};
            inputs[eye].frustum = frustum;
            inputs[eye].rasterCover = SelectReachSymmetricFovCover(
                frustum.angleLeft, frustum.angleRight,
                frustum.angleUp, frustum.angleDown,
                static_cast<uint32_t>(renderWidth),
                static_cast<uint32_t>(renderHeight));
            if (!inputs[eye].rasterCover.valid)
                return false;
            if (!BuildReachSymmetricRasterCullFrustum(
                    inputs[eye].rasterCover,
                    inputs[eye].frustum.relativeOrientation,
                    static_cast<uint32_t>(renderWidth),
                    static_cast<uint32_t>(renderHeight),
                    cullFrusta[eye]))
            {
                return false;
            }
        }

        cullCover = SelectReachStereoCullFovCover(
            cullFrusta, static_cast<uint32_t>(renderWidth),
            static_cast<uint32_t>(renderHeight));
        return cullCover.valid;
    }

    // ---- Reach cinematic-state probe (log-only, fail-open) -----------------
    // Evidence (2026-07-27, pinned haloreach.dll): Reach registers its
    // cutscene game state under the official names "cinematic globals"
    // (0x40 bytes) and "cinematic globals non deterministic" (0x10 bytes) at
    // exactly one registration site, and that site caches a per-engine-thread
    // pointer to each member inside the module's TLS block - the same design
    // HREK's exported symbol __tls_set_g_cinematic_globals_allocator names.
    // The signature below matches the registration's instruction stream; the
    // module TLS-index location, the verifying name string, and both TLS cache
    // slots are DECODED from the matched bytes, never hardcoded. The probe
    // READS and LOGS only - no engine write, no VR behavior change. Which
    // dwords mean in_progress / scene / shot is deliberately NOT assumed; the
    // first headset cutscene run assigns them from the log. A missing or
    // ambiguous match logs once and leaves the probe off.
    constexpr size_t kReachCineMemberADwords = 0x40 / 4;
    constexpr size_t kReachCineMemberBDwords = 0x10 / 4;
    const char* kReachCineRegistrationSig =
        "65 48 8B 0C 25 58 00 00 00 "  // mov rcx, gs:[0x58]
        "4C 8D 35 ?? ?? ?? ?? "        // lea r14, [member-offset table]
        "8B 15 ?? ?? ?? ?? "           // mov edx, [module TLS index] (+18)
        "45 8D 4D 10 "
        "89 05 ?? ?? ?? ?? "
        "45 33 C0 "
        "48 98 "
        "4C 89 6C 24 38 "
        "48 8B 1C D1 "                 // mov rbx, [rcx+rdx*8]  (TLS block)
        "48 8D 15 ?? ?? ?? ?? "        // lea rdx, [verify name] (+49)
        "4C 89 6C 24 30 "
        "41 BF 28 00 00 00 "
        "BD ?? ?? 00 00 "              // mov ebp, state-buffer base offset
        "BF ?? ?? 00 00 "              // mov edi, member A TLS slot (+70)
        "41 8B 0C 1F "
        "89 0D ?? ?? ?? ?? "
        "48 8D 0C 40 "
        "48 03 C9 "
        "41 8B 84 CE ?? ?? 00 00 "     // mov eax, [r14+rcx*8+table]
        "8D 4E C4 "
        "48 03 44 2B 20 "              // add rax, [rbx+rbp+0x20]
        "48 89 04 1F";                 // mov [rdi+rbx], rax  (cache member A)
    // Same function, a few instructions later: mov ecx, imm32 carrying the
    // member B TLS slot, immediately followed by the member B cache store
    // mov [rcx+rbx], rax. Searched only inside a bounded tail window.
    const char* kReachCineSlotBSig = "B9 ?? ?? ?? ?? 48 89 04 19";

    // ---- Reach native pause flag -------------------------------------------
    // See kReachNativePauseFlagRva in reach_render_logic.h for how this was
    // identified (live differential + code-reference filter + a 10 Hz watch of
    // a five-second pause cadence). The signature below is the runtime binding:
    // the flag's own address is DECODED from the matched store's disp32, never
    // hardcoded, exactly like Halo 3's LocateNativePauseFlag and ODST's owner
    // proof. The store alone is ambiguous (39 sites); the TLS-member fetch and
    // the 0x200 state-bit set in front of it are what make it unique.
    const char* kReachNativePauseOwnerSig =
        "8B 15 ?? ?? ?? ?? "             // mov edx, [module TLS index]
        "65 48 8B 04 25 58 00 00 00 "    // mov rax, gs:[0x58]
        "B9 A0 00 00 00 "                // mov ecx, 0xA0  (member slot)
        "48 8B 04 D0 "                   // mov rax, [rax+rdx*8]
        "48 8B 14 08 "                   // mov rdx, [rax+rcx]
        "B8 00 02 00 00 "                // mov eax, 0x200
        "66 42 09 04 32 "                // or word ptr [rdx+r14], ax
        "44 88 2D ?? ?? ?? ??";          // mov byte ptr [rip+d32], r13b

    // 0 = not located, otherwise the live address of Reach's pause byte.
    std::atomic<uintptr_t> g_reachNativePauseFlag{0};
    // Cached last read, so the XInput path does not repeat a guarded engine
    // read: -1 unknown, 0 running, 1 paused. Published by the Present tick.
    std::atomic<int> g_reachEnginePauseCache{-1};
    constexpr size_t kReachCineSigLength = 111;
    constexpr const char* kReachCineVerifyName =
        "cinematic globals non deterministic";

    struct ReachCineProbeState
    {
        std::atomic<bool> armed{false};
        std::atomic<uint32_t> attemptedGeneration{0};
        uint32_t* tlsIndex = nullptr; // inside the retained Reach module
        uint32_t slotA = 0;
        uint32_t slotB = 0;           // 0 = member B unavailable
        // Single-writer seqlock: the engine render thread publishes, the 50 ms
        // worker consumes. Even seq = stable.
        std::atomic<uint32_t> seq{0};
        uint32_t bufA[kReachCineMemberADwords] = {};
        uint32_t bufB[kReachCineMemberBDwords] = {};
        std::atomic<uint32_t> sampleFailures{0};
        // Bumped on every successful (re)arm so the worker-side log state
        // rebaselines instead of diffing against a previous generation.
        std::atomic<uint32_t> logReset{0};
        // Cut-detection history, owned by the render thread inside
        // ReachBuildHeadCullCamera (single writer, no locking needed).
        uint32_t cutPrevStamp = 0;
        bool cutPrevVisible = false;
        bool cutPrevValid = false;
        // Previous frame's authored camera pose, kept only while a cinematic is
        // running, for the shot-cut discontinuity test below.
        float cutPrevPos[3] = {};
        float cutPrevFwd[3] = {};
        bool cutPrevPoseValid = false;
    };
    ReachCineProbeState g_reachCineProbe;

    // Engine render thread, once per owned outer pass. Deterministic and
    // allocation/log-free per the hot-path rules; the SEH guard mirrors the
    // accepted Halo 3 ReadCinematicShot TLS read.
    void ReachCineProbeSample()
    {
        if (!g_reachCineProbe.armed.load(std::memory_order_acquire))
            return;
        uint32_t* tlsIndexPtr = g_reachCineProbe.tlsIndex;
        if (!tlsIndexPtr)
            return;
        __try
        {
            auto** slots = reinterpret_cast<void**>(__readgsqword(0x58));
            if (!slots)
            {
                g_reachCineProbe.sampleFailures.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            const uint32_t tlsIndex = *tlsIndexPtr;
            if (tlsIndex >= 0x200)
            {
                g_reachCineProbe.sampleFailures.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            auto* block = reinterpret_cast<unsigned char*>(slots[tlsIndex]);
            if (!block)
            {
                g_reachCineProbe.sampleFailures.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            auto* memberA = *reinterpret_cast<unsigned char**>(
                block + g_reachCineProbe.slotA);
            if (!memberA)
            {
                g_reachCineProbe.sampleFailures.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            unsigned char* memberB = nullptr;
            if (g_reachCineProbe.slotB)
            {
                memberB = *reinterpret_cast<unsigned char**>(
                    block + g_reachCineProbe.slotB);
            }
            const uint32_t seqBefore =
                g_reachCineProbe.seq.load(std::memory_order_relaxed);
            g_reachCineProbe.seq.store(
                seqBefore + 1, std::memory_order_release);
            memcpy(g_reachCineProbe.bufA, memberA,
                   sizeof(g_reachCineProbe.bufA));
            if (memberB)
            {
                memcpy(g_reachCineProbe.bufB, memberB,
                       sizeof(g_reachCineProbe.bufB));
            }
            g_reachCineProbe.seq.store(
                seqBefore + 2, std::memory_order_release);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_reachCineProbe.sampleFailures.fetch_add(
                1, std::memory_order_relaxed);
        }
    }

    // Adds one eye's exact OpenXR separation/cant and stamps the already
    // validated symmetric raster FOV. Missing or non-finite input fails the
    // whole transaction; there is no fixed-IPD or stale-view fallback.
    bool ReachApplyEyeOffset(
        unsigned char* cam, const ReachEyeRenderInput& input)
    {
        if (!cam || !input.rasterCover.valid)
            return false;

        float eyeOrientation[4] = {
            input.frustum.relativeOrientation[0],
            input.frustum.relativeOrientation[1],
            input.frustum.relativeOrientation[2],
            input.frustum.relativeOrientation[3]};
        float orientationLengthSquared = 0.0f;
        for (float component : eyeOrientation)
        {
            if (!isfinite(component))
                return false;
            orientationLengthSquared += component * component;
        }
        if (!isfinite(orientationLengthSquared) ||
            orientationLengthSquared <= 1e-8f)
        {
            return false;
        }
        const float orientationLength = sqrtf(orientationLengthSquared);
        for (float& component : eyeOrientation)
            component /= orientationLength;

        float* pos = reinterpret_cast<float*>(cam + 0x00);
        float* fwd = reinterpret_cast<float*>(cam + 0x0C);
        float* up = reinterpret_cast<float*>(cam + 0x18);
        const float right[3] = {
            fwd[1] * up[2] - fwd[2] * up[1],
            fwd[2] * up[0] - fwd[0] * up[2],
            fwd[0] * up[1] - fwd[1] * up[0]};
        const float s = kReachWorldUnitsPerMeter;
        for (int axis = 0; axis < 3; ++axis)
            pos[axis] += (right[axis] * input.position[0] +
                          up[axis] * input.position[1] -
                          fwd[axis] * input.position[2]) * s;
        const float sinHalf = sqrtf(eyeOrientation[0] * eyeOrientation[0] +
                                    eyeOrientation[1] * eyeOrientation[1] +
                                    eyeOrientation[2] * eyeOrientation[2]);
        if (sinHalf > 1e-5f)
        {
            float angle = 2.0f * atan2f(sinHalf, eyeOrientation[3]);
            if (angle > 3.14159265f) angle -= 6.2831853f;
            const float ax = eyeOrientation[0] / sinHalf;
            const float ay = eyeOrientation[1] / sinHalf;
            const float az = eyeOrientation[2] / sinHalf;
            const float axisVec[3] = {
                ax * right[0] + ay * up[0] - az * fwd[0],
                ax * right[1] + ay * up[1] - az * fwd[1],
                ax * right[2] + ay * up[2] - az * fwd[2]};
            const float cosA = cosf(angle), sinA = sinf(angle);
            RotateAboutAxis(fwd, axisVec, cosA, sinA);
            RotateAboutAxis(up, axisVec, cosA, sinA);
        }
        *reinterpret_cast<float*>(cam + 0x28) =
            input.rasterCover.verticalFov;
        return true;
    }

    // Build the single head-centre camera consumed by Reach's outer CPU
    // visibility pass. The frustum/projection helpers are the exact stock
    // pre-scope pair. The caller commits this same centre to the bounded
    // player-view state before the inner owner is acquired. The claimed inner
    // transaction replaces it with each exact eye and rolls it back afterward;
    // a claimed failure is suppressed rather than rerendered.
    bool ReachBuildHeadCullCamera(
        const unsigned char* stockCompact,
        const ReachSymmetricFovCover& cullCover,
        const ReachVrRenderSnapshot& tracking,
        unsigned char* headCenter,
        unsigned char* headDerived)
    {
        // Log-only cinematic-state sample on the engine thread that owns the
        // per-frame camera work. If cutscenes stop this path from running, the
        // worker-side stall report is itself the finding.
        ReachCineProbeSample();
        if (!stockCompact || !headCenter || !headDerived ||
            !cullCover.valid || !g_reachHelpers.Ready())
        {
            return false;
        }
        // Publish the game's own facing yaw from the pristine stock camera
        // forward (offset 0x0C, same convention as the recenter reference at
        // g_gameYawRef = atan2f(fwd[1], fwd[0])) BEFORE the head transform
        // overwrites it. Head-relative locomotion reads this to rotate the move
        // stick to gaze. Read-only: this does not modify stockCompact.
        {
            const float* stockFwd =
                reinterpret_cast<const float*>(stockCompact + 0x0C);
            const float sfx = stockFwd[0], sfy = stockFwd[1];
            if (isfinite(sfx) && isfinite(sfy) &&
                (sfx * sfx + sfy * sfy) > 1e-8f)
            {
                g_reachStockHeadingYaw.store(atan2f(sfy, sfx),
                                             std::memory_order_relaxed);
                g_reachMoveHeadingValid.store(true, std::memory_order_release);
                // Publish the pristine game aim forward — the same vector Halo 3
                // exposes at CamCopyHook (g_aimFwd*/g_aimSeen) — so the shared
                // closed-loop aim steering in Game_ComputeAimStick can drive
                // Reach's frozen sim aim onto the controller ray. Read-only:
                // stockCompact is never modified (headCenter is the writable
                // copy). Raw forward, matching Halo 3's publication; the yaw uses
                // the same atan2(y,x) convention as the heading above.
                if (isfinite(stockFwd[2]))
                {
                    g_aimFwdX.store(stockFwd[0]);
                    g_aimFwdY.store(stockFwd[1]);
                    g_aimFwdZ.store(stockFwd[2]);
                    g_aimSeen.store(true, std::memory_order_release);
                }
            }
        }
        // Authored camera-cut detection (probe-proven, headset log
        // 2026-07-27 00:43-00:48): cinematic-globals +0x28 is an authored shot
        // stamp that holds still through ordinary gameplay, and the byte at
        // +0x26 rises exactly when a fade-to-black ends. Either edge is an
        // authored discontinuity, so request the yaw realign ReachApplyHeadLook
        // performs this same frame. The sampler ran at function entry on this
        // thread, so bufA is this frame's coherent copy. Fail-open: an unarmed
        // probe means no realign - stock behavior - never a wrong one.
        //
        // +0x28 alone is NOT sufficient. It was accepted as an every-cut marker
        // on the strength of a session that only watched cutscene starts and
        // ends; the 2026-07-27 08:12-08:14 log then showed it moving just twice
        // across a 65-second cutscene, and the user confirmed in-headset that
        // the first shot orients but every later shot does not. The camera
        // discontinuity test below is what covers the shots it misses.
        if (g_reachCineProbe.armed.load(std::memory_order_acquire))
        {
            const uint32_t stateWord = g_reachCineProbe.bufA[0x24 / 4];
            const uint32_t shotStamp = g_reachCineProbe.bufA[0x28 / 4];
            const bool screenVisible = ((stateWord >> 16) & 0xFFu) != 0;
            // Byte +0x24 is the cinematic-in-progress flag, from the same
            // headset log that proved +0x26 and +0x28: it reads 0 in the menu
            // and during ordinary play, rises to 1 for the whole cutscene, and
            // drops back to 0 on the frame the cutscene hands back to gameplay.
            const bool cinematicRunning = (stateWord & 0xFFu) != 0;
            bool cut = g_reachCineProbe.cutPrevValid && screenVisible &&
                (shotStamp != g_reachCineProbe.cutPrevStamp ||
                 !g_reachCineProbe.cutPrevVisible);

            // The stamp above only moves at some authored boundaries - a
            // headset log (2026-07-27 08:12-08:14) showed it changing twice
            // across a 65-second cutscene that plainly cut far more often, so
            // every shot in between kept the previous shot's facing. Detect the
            // cut from the authored camera instead: within a shot the cinematic
            // camera moves continuously, and at a cut it jumps. stockCompact is
            // the pristine engine camera for this frame (the head transform
            // below writes headCenter, never this), so during a cinematic it is
            // the authored pose and nothing else. Gated on cinematicRunning so
            // this can never fire during play, where the same camera legitimately
            // jumps on snap turn, respawn, and vehicle entry.
            const float* stockPos =
                reinterpret_cast<const float*>(stockCompact + 0x00);
            const float* stockFwd =
                reinterpret_cast<const float*>(stockCompact + 0x0C);
            bool poseUsable = cinematicRunning && screenVisible;
            for (int i = 0; poseUsable && i < 3; ++i)
            {
                if (!isfinite(stockPos[i]) || !isfinite(stockFwd[i]))
                    poseUsable = false;
            }
            if (poseUsable &&
                (stockFwd[0] * stockFwd[0] + stockFwd[1] * stockFwd[1]) <= 1e-8f)
            {
                poseUsable = false;
            }
            if (poseUsable && g_reachCineProbe.cutPrevPoseValid && !cut)
            {
                const float yawDelta = fabsf(WrapPi(
                    atan2f(stockFwd[1], stockFwd[0]) -
                    atan2f(g_reachCineProbe.cutPrevFwd[1],
                           g_reachCineProbe.cutPrevFwd[0])));
                const float dx = stockPos[0] - g_reachCineProbe.cutPrevPos[0];
                const float dy = stockPos[1] - g_reachCineProbe.cutPrevPos[1];
                const float dz = stockPos[2] - g_reachCineProbe.cutPrevPos[2];
                if (yawDelta > kReachCineCutYawRadians ||
                    (dx * dx + dy * dy + dz * dz) > kReachCineCutJumpUnitsSq)
                {
                    cut = true;
                    g_reachCineCutPoseCount.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            if (cut)
            {
                g_reachCineCutRealign.store(true, std::memory_order_release);
                g_reachCineCutCount.fetch_add(1, std::memory_order_relaxed);
            }

            if (poseUsable)
            {
                memcpy(g_reachCineProbe.cutPrevPos, stockPos,
                       sizeof(g_reachCineProbe.cutPrevPos));
                memcpy(g_reachCineProbe.cutPrevFwd, stockFwd,
                       sizeof(g_reachCineProbe.cutPrevFwd));
            }
            g_reachCineProbe.cutPrevPoseValid = poseUsable;
            g_reachCineProbe.cutPrevStamp = shotStamp;
            g_reachCineProbe.cutPrevVisible = screenVisible;
            g_reachCineProbe.cutPrevValid = true;
        }
        memcpy(headCenter, stockCompact, kReachCompactCameraBytes);
        ApplyVrTurn(tracking.pad);
        if (!ReachApplyHeadLook(headCenter, tracking))
            return false;
        *reinterpret_cast<float*>(headCenter + 0x28) = cullCover.verticalFov;

        ReachCompactCameraObservation transformed{};
        if (!ValidateReachCompactCamera(
                headCenter, kReachCompactCameraBytes, transformed))
        {
            return false;
        }
        float frustumBounds[4]{};
        if (!g_reachHelpers.frustum(headCenter, frustumBounds))
            return false;
        for (float component : frustumBounds)
            if (!isfinite(component))
                return false;
        g_reachHelpers.projection(
            headCenter, frustumBounds, headDerived, 0.0f);
        const float* projectionMatrix = reinterpret_cast<const float*>(
            headDerived + kReachDerivedProjectionOffset);
        const ReachProjectionHalfFovs rasterFov =
            DecodeReachProjectionHalfFovs(
                projectionMatrix[0], projectionMatrix[5]);
        return ReachProjectionCoversOpenXr(rasterFov, cullCover);
    }

    // Byte-exact rollback of everything the per-eye transaction can touch,
    // preserving the final eye's actual post-render last-window flag. Runs in a
    // __finally so it executes even if a stock render faults mid-transaction.
    void ReachRestoreScope(
        uintptr_t workspace, uintptr_t playerView,
        const unsigned char* savedWorkspace, const unsigned char* savedPv)
    {
        const uint8_t finalByte =
            *reinterpret_cast<uint8_t*>(playerView + kReachLastWindowFlagOffset);
        memcpy(reinterpret_cast<void*>(workspace), savedWorkspace,
               kReachRenderScopeSnapshotSize);
        memcpy(reinterpret_cast<void*>(playerView + kReachPvSnapshotBegin),
               savedPv, kReachPvSnapshotBytes);
        *reinterpret_cast<uint8_t*>(playerView + kReachLastWindowFlagOffset) =
            finalByte;
    }

    // Returns true only if the already-claimed transaction rendered and
    // captured both eyes. False is terminal for this title generation: the
    // caller must revoke ownership and must not invoke the flat renderer.
    // Reach's own script function chud_show_crosshair (haloreach.dll+0x1B3190)
    // hides the crosshair by writing 0.0f into a per-widget alpha field:
    //
    //   chud_globals = *(void**)(TLS[*(uint32_t*)(base+0xC17B18)] + 0x5B0)
    //   alpha_n      = *(float*)(chud_globals + 0x334 + n*0xC60), n = 0..15
    //
    // chud_fade_crosshair_for_player (+0x1B3528) writes the same +0x334 field
    // (and +0x358/+0x37C for a timed fade), which confirms the meaning. The
    // float it stores for "shown" is 1.0f at haloreach.dll+0xA8AF18.
    //
    // Reproducing that exact native write is how Reach honours kill_reticle.
    // It hooks nothing, allocates nothing, and cannot affect camera ownership:
    // every step is guarded, and failure simply leaves the stock crosshair up.
    constexpr size_t kReachChudGlobalsTlsOffset = 0x5B0;
    // The CHUD widget alpha state is three parallel arrays of nine widgets.
    // Derived by disassembling every chud_fade_*_for_player implementation in
    // haloreach.dll and reading the displacement each one writes:
    //
    //   current alpha  +0x32C + i*4      fade target +0x350 + i*4
    //   fade duration  +0x374 + i*4
    //
    //   i=1 weapon stats   i=2 CROSSHAIR    i=3 shield     i=4 grenades
    //   i=5 messages       i=6 motion sensor i=7 chapter title i=8 cinematics
    //
    // Crosshair therefore owns exactly these three fields, and nothing else.
    // Setting only the current alpha is not enough: the engine drives current
    // toward the fade target every frame, so the target must be cleared too.
    constexpr size_t kReachChudCrosshairAlpha = 0x334;
    constexpr size_t kReachChudCrosshairFadeTarget = 0x358;
    constexpr size_t kReachChudCrosshairFadeDuration = 0x37C;

    // Resolved once on the cold title worker, never guessed. Chain:
    //   "chud_show_crosshair\0"   -> exactly one occurrence in the image
    //   exactly one qword -> it   -> that script-function table entry
    //   entry + 0x18              -> the implementation
    //   implementation + 0x30     -> its own 'mov ecx,[rip+rel32]' TLS index
    // Every step is required to be unique/exact; anything else leaves the stock
    // crosshair alone.
    const uint32_t* g_reachChudTlsIndex = nullptr;

    bool ResolveReachChudCrosshairFields(uintptr_t base, size_t size)
    {
        g_reachChudTlsIndex = nullptr;
        static const char kName[] = "chud_show_crosshair";
        const uint8_t* image = reinterpret_cast<const uint8_t*>(base);
        constexpr size_t kNameBytes = sizeof(kName); // includes the NUL

        uintptr_t nameVa = 0;
        size_t nameHits = 0;
        for (size_t i = 0; i + kNameBytes <= size;)
        {
            const void* hit = memchr(image + i, kName[0], size - i - kNameBytes + 1);
            if (!hit)
                break;
            const size_t at = static_cast<size_t>(
                reinterpret_cast<const uint8_t*>(hit) - image);
            if (memcmp(image + at, kName, kNameBytes) == 0)
            {
                nameVa = base + at;
                if (++nameHits > 1)
                    break;
            }
            i = at + 1;
        }
        if (nameHits != 1)
            return false;

        uintptr_t entry = 0;
        size_t pointerHits = 0;
        for (size_t i = 0; i + sizeof(uintptr_t) <= size; i += sizeof(uintptr_t))
        {
            if (*reinterpret_cast<const uintptr_t*>(image + i) == nameVa)
            {
                entry = base + i;
                if (++pointerHits > 1)
                    break;
            }
        }
        if (pointerHits != 1)
            return false;

        const uintptr_t impl =
            *reinterpret_cast<const uintptr_t*>(entry + 0x18);
        if (impl < base || impl + 0x40 > base + size)
            return false;

        // The implementation's own prologue and its TLS-index instruction.
        static const uint8_t kPrologue[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x0F,
            0xBF, 0xC1, 0x8B, 0xDA, 0x45, 0x8A, 0xC8};
        const uint8_t* fn = reinterpret_cast<const uint8_t*>(impl);
        if (memcmp(fn, kPrologue, sizeof(kPrologue)) != 0)
            return false;
        if (fn[0x30] != 0x8B || fn[0x31] != 0x0D)
            return false;
        int32_t displacement = 0;
        memcpy(&displacement, fn + 0x32, sizeof(displacement));
        const uintptr_t tlsIndexVa = impl + 0x36 +
            static_cast<uintptr_t>(static_cast<intptr_t>(displacement));
        if (tlsIndexVa < base || tlsIndexVa + sizeof(uint32_t) > base + size)
            return false;

        g_reachChudTlsIndex =
            reinterpret_cast<const uint32_t*>(tlsIndexVa);
        LOG("Reach crosshair: chud_show_crosshair resolved at haloreach.dll+"
            "0x%llX through its unique script-table entry; CHUD TLS index at "
            "+0x%llX",
            static_cast<unsigned long long>(impl - base),
            static_cast<unsigned long long>(tlsIndexVa - base));
        return true;
    }

    // DISABLED 2026-07-27 - this was destroying the art it was meant to
    // complement. It zeroes the crosshair's OWN alpha/fade-target/fade-duration
    // record every admitted frame. Reach's CHUD then has a fully faded-out
    // crosshair, and once an objective event makes it re-evaluate that fade
    // state it stops emitting the widget at all: the headset log shows
    // "no class-2 crosshair widget drawn for 2s - the engine stopped emitting
    // it" with unreadable descriptors 0 and rejects 0, and no recovery line
    // afterwards. No class-2 draw means nothing to capture, so the authored
    // quad stops being submitted and the player's crosshair disappears for
    // the rest of the session. That is the reported "every time an objective
    // is given, the crosshair disappears".
    //
    // It is also redundant. The render-target redirect is what keeps Reach's
    // flat crosshair off the eye, proven by `crosshair=0` (headset confirmed
    // 2026-07-27): there the redirect runs with no alpha write at all and no
    // flat crosshair appears. Suppressing the widget through the engine's own
    // fade fields was only ever a second belt on top of that, and it costs the
    // capture source.
    //
    // Kept compiled and callable rather than deleted, per the revert rule: if
    // a future headset test ever shows the flat crosshair leaking through the
    // redirect, re-enabling this is one line - but fix the leak in the
    // redirect first, because this write cannot be made safe.
    constexpr bool kReachSuppressNativeCrosshairAlpha = false;

    void SuppressReachNativeCrosshair()
    {
        if (!kReachSuppressNativeCrosshairAlpha)
            return;
        if (!g_config.crosshair || !g_config.kill_reticle)
            return;
        if (!g_reachChudTlsIndex)
            return;

        uintptr_t chudGlobals = 0;
        __try
        {
            const uint32_t tlsIndex = *g_reachChudTlsIndex;
            if (tlsIndex >= 1088)
                return;
            auto** slots = reinterpret_cast<void**>(__readgsqword(0x58));
            if (!slots)
                return;
            auto* tls = reinterpret_cast<unsigned char*>(slots[tlsIndex]);
            if (!tls)
                return;
            chudGlobals = *reinterpret_cast<const volatile uintptr_t*>(
                tls + kReachChudGlobalsTlsOffset);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (!chudGlobals)
            return;

        // Exactly the crosshair's three fields, in one record. An earlier
        // version also wrote +0x334 at 0xC60 strides for 16 "slots"; fifteen of
        // those landed on unrelated CHUD records and dragged player markers
        // around with the weapon. There is no stride: there is one crosshair.
        float* alpha = reinterpret_cast<float*>(
            chudGlobals + kReachChudCrosshairAlpha);
        float* fadeTarget = reinterpret_cast<float*>(
            chudGlobals + kReachChudCrosshairFadeTarget);
        float* fadeDuration = reinterpret_cast<float*>(
            chudGlobals + kReachChudCrosshairFadeDuration);
        float current = 0.0f;
        float target = 0.0f;
        if (!SafeReadFloat(alpha, &current) ||
            !SafeReadFloat(fadeTarget, &target))
            return;
        if (!(current > 0.0f) && !(target > 0.0f))
            return; // already hidden; never fight the engine needlessly
        (void)SafeWriteFloat(fadeDuration, 0.0f); // snap, no fade
        (void)SafeWriteFloat(fadeTarget, 0.0f);
        (void)SafeWriteFloat(alpha, 0.0f);

        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            LOG("Reach crosshair: native CHUD crosshair alpha cleared through "
                "the title's own crosshair alpha/fade fields (kill_reticle=1)");
    }

    bool ReachStereoTransaction(uintptr_t playerView, ReachVrRenderAccess& access)
    {
        const uintptr_t workspace = g_reachOwnerScope.workspace;
        unsigned char* compact = reinterpret_cast<unsigned char*>(workspace);

        if (!g_reachHelpers.Ready() || !access.active ||
            !g_reachOwnerScope.active ||
            !g_reachFpPairScope.armed ||
            g_reachFpPairScope.generation!=g_reachCamera.generation ||
            g_reachFpPairScope.preparedSerial!=access.preparedSerial ||
            access.preparedSerial != g_reachOwnerScope.preparedSerial)
            return false;

        // Once per admitted frame, before either eye renders. Fail-open: this
        // never reports failure and never influences the transaction result.
        SuppressReachNativeCrosshair();

        ReachCompactCameraObservation observed{};
        ReachCompactCameraObservation secondaryObserved{};
        const unsigned char* secondaryCompact =
            reinterpret_cast<const unsigned char*>(
                workspace + kReachSecondaryCompactOffset);
        if (!ValidateReachCompactCamera(
                compact, kReachCompactCameraBytes, observed) ||
            !ValidateReachCompactCamera(
                secondaryCompact, kReachCompactCameraBytes,
                secondaryObserved) ||
            memcmp(compact, g_reachOwnerScope.headCenter,
                   kReachCompactCameraBytes) != 0 ||
            memcmp(secondaryCompact, g_reachOwnerScope.headCenter,
                   kReachCompactCameraBytes) != 0)
        {
            return false;
        }

        // player_view_render loads the render-camera owner global (0x04E38A90)
        // into rax and dereferences [rax+0xB0], and it CLEARS that global to zero
        // on every call (RVA 0x26CE2F). Left alone, the second eye's call loads
        // null and faults in the first-person camera build -- the observed access
        // violation at haloreach.dll RVA 0x26E02F. Require the engine's admitted
        // owner (player_view + 0x3B0), re-arm the global to it before each eye's
        // call, and never restore it afterward so the final eye's stock zero
        // persists. An unexpected owner rejects the claimed transaction.
        const uintptr_t reachOwnerGlobal =
            g_reachCamera.base + kReachRenderCameraOwnerRva;
        const uintptr_t entryOwner =
            *reinterpret_cast<uintptr_t*>(reachOwnerGlobal);
        if (entryOwner != playerView + kReachPlayerViewCameraStateOffset)
            return false;

        // A prepared serial can encounter more than one qualifying outer call.
        // Invalidate the whole eye and authored-CHUD pair before each admitted
        // attempt so a partial retry cannot combine one newly rendered eye, or
        // a no-widget state, with the prior attempt from the same frame serial.
        g_reachRenderFovSerial[0].store(0, std::memory_order_release);
        g_reachRenderFovSerial[1].store(0, std::memory_order_release);
        VR_InvalidatePreparedReachAuthoredReticleCapture();

        alignas(16) unsigned char savedWorkspace[kReachRenderScopeSnapshotSize];
        alignas(16) unsigned char savedPv[kReachPvSnapshotBytes];
        alignas(16) unsigned char center[kReachCompactCameraBytes];
        memcpy(savedWorkspace, reinterpret_cast<void*>(workspace),
               kReachRenderScopeSnapshotSize);
        memcpy(savedPv,
               reinterpret_cast<void*>(playerView + kReachPvSnapshotBegin),
               kReachPvSnapshotBytes);

        // The outer hook already consumed turn/head pose once before CPU
        // visibility. Both exact eyes must derive from that identical centre;
        // applying either transform again here would double room-scale lean and
        // split culling from the raster camera.
        memcpy(center, g_reachOwnerScope.headCenter,
               kReachCompactCameraBytes);

        const uint8_t originalLastWindow =
            *reinterpret_cast<uint8_t*>(playerView + kReachLastWindowFlagOffset);
        const bool rightFirst = g_config.right_eye_first;
        bool completed = false;
        bool transactionValid = true;
        uint32_t capturedEyes = 0;
        __try
        {
            for (uint32_t pass = 0; pass < 2; ++pass)
            {
                const ReachStereoPassPolicy policy = SelectReachStereoPassPolicy(
                    pass, rightFirst, originalLastWindow);
                if (!policy.valid)
                {
                    transactionValid = false;
                    break;
                }
                g_stereoEye.store(
                    static_cast<int>(policy.eye),std::memory_order_release);
                // Head-tracked centre plus this eye's exact OpenXR separation,
                // cant, and symmetric covering FOV, stamped into the primary
                // compact camera.
                memcpy(compact, center, kReachCompactCameraBytes);
                const ReachEyeRenderInput& eyeInput =
                    g_reachOwnerScope.eyes[policy.eye];
                const ReachSymmetricFovCover& requestedFov =
                    eyeInput.rasterCover;
                if (!ReachApplyEyeOffset(compact, eyeInput))
                {
                    transactionValid = false;
                    break;
                }

                // Rebuild this eye exactly as stock setup does, but from the VR
                // camera, so player_view_render renders from per-eye matrices
                // instead of the pre-built stock centre matrices. Without this
                // rebuild the compact-camera edits are never read -- that is the
                // cone and the missing 6DOF the headset showed. This is the
                // proven pre-scope rebuild (REACH-SIGNATURE-EVIDENCE.md steps
                // 2-6): frustum bounds -> projection -> primary/secondary
                // coherence -> camera-state update -> projection/matrix build
                // into player_view+0x490. It mirrors Halo 3's RenderViewHook,
                // which rebuilds view+0x98 with the engine's own helpers.
                unsigned char* primaryDerived = reinterpret_cast<unsigned char*>(
                    workspace + kReachPrimaryDerivedOffset);
                float frustumBounds[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (!g_reachHelpers.frustum(compact, frustumBounds))
                {
                    transactionValid = false;
                    break;
                }
                bool frustumFinite = true;
                for (float component : frustumBounds)
                    frustumFinite = isfinite(component) && frustumFinite;
                if (!frustumFinite)
                {
                    transactionValid = false;
                    break;
                }
                g_reachHelpers.projection(
                    compact, frustumBounds, primaryDerived, 0.0f);
                const float* projectionMatrix = reinterpret_cast<const float*>(
                    primaryDerived + kReachDerivedProjectionOffset);
                const ReachProjectionHalfFovs rasterFov =
                    DecodeReachProjectionHalfFovs(
                        projectionMatrix[0], projectionMatrix[5]);
                if (!ReachProjectionCoversOpenXr(rasterFov, requestedFov))
                {
                    transactionValid = false;
                    break;
                }
                // Mirror the rebuilt primary compact + derived into the secondary
                // render pair, matching stock normal setup which mirrors both.
                memcpy(reinterpret_cast<void*>(
                           workspace + kReachSecondaryCompactOffset),
                       compact, kReachCompactCameraBytes);
                memcpy(reinterpret_cast<void*>(
                           workspace + kReachSecondaryDerivedOffset),
                       primaryDerived, kReachDerivedBlockSize);
                // Re-arm the render-camera owner before the state updater and the
                // render read it; the previous eye's call zeroed it (see the
                // entry check above).
                *reinterpret_cast<uintptr_t*>(reachOwnerGlobal) = entryOwner;
                g_reachHelpers.cameraState(
                    reinterpret_cast<void*>(
                        playerView + kReachPlayerViewCameraStateOffset),
                    compact);
                g_reachHelpers.matrix(
                    reinterpret_cast<void*>(
                        playerView + kReachPlayerViewCurrentMatricesOffset),
                    primaryDerived,
                    primaryDerived + kReachDerivedProjectionOffset,
                    compact + kReachCompactRenderBoundsOffset,
                    reinterpret_cast<void*>(
                        playerView + kReachPlayerViewProjectionOffsetPairOffset));

                if (policy.writeLastWindow)
                    *reinterpret_cast<uint8_t*>(
                        playerView + kReachLastWindowFlagOffset) =
                        policy.lastWindowInput;
                // Re-arm once more immediately before the render in case the
                // state updater cleared or moved the owner the inner render
                // dereferences.
                *reinterpret_cast<uintptr_t*>(reachOwnerGlobal) = entryOwner;
                ReachFpCameraEyeScope& fpCameraScope =
                    g_reachFpCameraEyeScope;
                fpCameraScope.active = false;
                fpCameraScope.generation = g_reachCamera.generation;
                fpCameraScope.eye = policy.eye;
                fpCameraScope.preparedSerial = access.preparedSerial;
                fpCameraScope.workspace = workspace;
                fpCameraScope.playerView = playerView;
                fpCameraScope.chudParityFailed = false;
                fpCameraScope.chudClass2Seen = false;
                fpCameraScope.authoredCrosshairCaptured = false;
                fpCameraScope.captureKey = 0;
                memcpy(fpCameraScope.compact, compact,
                       sizeof(fpCameraScope.compact));
                memcpy(fpCameraScope.derived, primaryDerived,
                       sizeof(fpCameraScope.derived));
                bool renderReturned = false;
                __try
                {
                    fpCameraScope.active = true;
                    renderReturned =
                        ReachCallPlayerViewWithEyeScopedSuppressions(playerView);
                }
                __finally
                {
                    fpCameraScope.active = false;
                }
                if (!renderReturned)
                {
                    transactionValid = false;
                    break;
                }
                if (!VR_ReachCopyEye(access, policy.eye))
                {
                    transactionValid = false;
                    break;
                }
                // Publish only after this exact raster was rendered and copied.
                // The release serial makes the values visible to the OpenXR
                // submit path only for the matching prepared frame.
                g_reachRenderHalfFovX[policy.eye].store(
                    rasterFov.horizontal, std::memory_order_relaxed);
                g_reachRenderHalfFovY[policy.eye].store(
                    rasterFov.vertical, std::memory_order_relaxed);
                g_reachRenderFovSerial[policy.eye].store(
                    access.preparedSerial, std::memory_order_release);
                ++capturedEyes;
                if (pass == 0)
                {
                    memcpy(reinterpret_cast<void*>(workspace), savedWorkspace,
                           kReachRenderScopeSnapshotSize);
                    memcpy(reinterpret_cast<void*>(
                               playerView + kReachPvSnapshotBegin),
                           savedPv, kReachPvSnapshotBytes);
                }
            }
            completed = transactionValid && capturedEyes == 2;
        }
        __finally
        {
            // The first-person interpolation/palette work occurs earlier in
            // the admitted outer render. Keep that outer-owned pair scope
            // alive across both eye renders, but do not leak an eye selection
            // into the remainder of the stock outer transaction.
            g_stereoEye.store(-1,std::memory_order_release);
            ReachRestoreScope(workspace, playerView, savedWorkspace, savedPv);
        }
        return completed;
    }

    bool ReachInnerScopeMatchesLive(
        uintptr_t playerView, uintptr_t returnAddress)
    {
        const ReachOwnerScope& scope = g_reachOwnerScope;
        const uintptr_t base = g_reachCamera.base;
        if (g_reachNestedOuterSuppressed ||
            !scope.active || !base || scope.playerView != playerView ||
            !scope.renderAccess || !scope.renderAccess->active ||
            scope.cameraStackDepthBefore < -1 ||
            scope.cameraStackDepthBefore >= 3)
        {
            return false;
        }

        const int32_t depth = *reinterpret_cast<const int32_t*>(
            base + kReachCameraStackDepthRva);
        if (depth != scope.cameraStackDepthBefore + 1 ||
            depth < 0 || depth > 3)
        {
            return false;
        }
        const uintptr_t topWorkspace = *reinterpret_cast<const uintptr_t*>(
            base + kReachCameraStackPointersRva +
            static_cast<uintptr_t>(depth) * sizeof(uintptr_t));
        return returnAddress == base + kReachPlayerViewRenderReturnRva &&
            *reinterpret_cast<const uintptr_t*>(
                base + kReachActiveViewRva) == playerView &&
            topWorkspace == scope.workspace &&
            *reinterpret_cast<const uintptr_t*>(
                scope.workspace + kReachRenderScopeSnapshotSize -
                sizeof(uintptr_t)) ==
                    base + kReachCameraStackCallbackRva &&
            *reinterpret_cast<const uint32_t*>(
                base + kReachSelectedSpecializationRva) == 0;
    }

    void ReachPlayerViewRenderBody(
        uintptr_t playerView, uintptr_t returnAddress)
    {
        if (!ReachInnerScopeMatchesLive(playerView, returnAddress))
        {
            // This invocation has not crossed the exact Reach eye-ownership
            // boundary. Match Halo 3/ODST and leave unrelated native passes
            // stock, including authored states that emit no crosshair widget.
            // Only a call that passes ReachInnerScopeMatchesLive below is a
            // claimed VR transaction whose later failure must tear down.
            g_reachOrigPlayerViewRender(playerView);
            return;
        }
        if (!g_reachCamera.armed.load(std::memory_order_acquire))
        {
            // An exact inner scope exists only after the armed outer transaction
            // claimed this call tree. Mid-call revocation is terminal and may
            // never invoke the flat renderer, regardless of teardown-store order.
            Game_RejectReachAuthoredReticle(
                g_reachCamera.generation.load(std::memory_order_acquire),
                "core disarmed mid-call while an eye scope was claimed");
            return;
        }

        const ReachModuleEpoch epoch{g_reachCamera.base, g_reachCamera.generation};
        const ReachPreflightToken preflight =
            ReachRenderCandidate_GetPreflight(epoch);
        const ReachPreparedFrameToken prepared = VR_ReachPreparedFrame(epoch);
        if (!preflight.Complete() ||
            !ReachRenderCandidate_IsPreflightCurrent(preflight) ||
            !prepared.Ready() ||
            prepared.Serial() != g_reachOwnerScope.preparedSerial ||
            !VR_ReachDisplayReady(epoch) ||
            !g_reachOwnerScope.renderAccess ||
            g_reachOwnerScope.renderAccess->preparedSerial !=
                prepared.Serial())
        {
            Game_RejectReachAuthoredReticle(
                epoch.generation,
                "preflight/prepared-frame/display-access proof not current");
            return;
        }

        ReachVrRenderAccess& access = *g_reachOwnerScope.renderAccess;
        bool handled = false;
        __try
        {
            handled = ReachStereoTransaction(playerView, access);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            handled = false;
        }
        if (!handled)
        {
            // Skip this frame, keep the core. ReachStereoTransaction restores
            // the workspace and player view in its own __finally, so an
            // incomplete attempt leaves no partial engine state behind, and the
            // eye serials it never published cannot be composited. A single
            // failure after minutes of correct rendering is not a reason to
            // unhook Reach and drop the player out of VR -- that is exactly the
            // over-reaction that cost 2026-07-26. Halo 3 and ODST skip and
            // recover.
            static std::atomic<uint64_t> lastSkipLogMs{0};
            const uint64_t nowMs = GetTickCount64();
            uint64_t previousMs = lastSkipLogMs.load(std::memory_order_relaxed);
            if (nowMs - previousMs >= 2000 &&
                lastSkipLogMs.compare_exchange_strong(
                    previousMs, nowMs, std::memory_order_relaxed))
            {
                LOG("Reach frame skipped (stereo eye transaction did not "
                    "complete); the camera core stays armed and the next frame "
                    "retries");
            }
        }
    }

    // Keep the counter wrapper separate from the SEH-heavy body. The wrapper's
    // complete unwind range is also part of worker-side ingress scanning, which
    // closes the pre-counter instruction and MinHook-relay teardown windows.
    __declspec(noinline) void __fastcall ReachPlayerViewRenderDetour(
        uintptr_t playerView)
    {
        // Capture the engine caller before entering the helper body. Calling
        // _ReturnAddress() from that body would see this wrapper's call site
        // and make the exact retail caller gate fail permanently.
        const uintptr_t returnAddress =
            reinterpret_cast<uintptr_t>(_ReturnAddress());
        g_reachCamera.activeCallbacks.fetch_add(
            1, std::memory_order_acq_rel);
        __try
        {
            ReachPlayerViewRenderBody(playerView, returnAddress);
        }
        __finally
        {
            g_reachCamera.activeCallbacks.fetch_sub(
                1, std::memory_order_acq_rel);
        }
    }

    // Reach production first-person path. Every interpolation transaction keeps
    // a bounded untouched graph; the live copy serves marker/attachment users,
    // while every matching final palette reconstructs from private scratch.
    // Hot hooks perform bounded reads and atomic status publication only.
    static int SafeReadBytes(const void* src, void* dst, size_t n)
    {
        __try { memcpy(dst, src, n); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return 1;
    }

    static int SafeWriteBytes(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return 1;
    }

    bool ReachBoneMatrixFinite(const BoneMatrix& matrix)
    {
        return ReachFpPackedGraphFinite(
            std::span<const float>{reinterpret_cast<const float*>(&matrix),
                                   kReachFpBoneMatrixFloatCount},1);
    }

    void PublishReachFpStatus(int code, int bodyCount, int liveCount)
    {
        const uint32_t generation=g_reachCamera.generation;
        const uint64_t key=(static_cast<uint64_t>(generation)<<32) |
            (static_cast<uint64_t>(code&0xFF)<<24) |
            (static_cast<uint64_t>(bodyCount&0xFFF)<<12) |
            static_cast<uint64_t>(liveCount&0xFFF);
        if (g_reachFpStatus.key.load(std::memory_order_relaxed)==key)
            return;
        g_reachFpStatus.generation.store(generation,std::memory_order_relaxed);
        g_reachFpStatus.code.store(code,std::memory_order_relaxed);
        g_reachFpStatus.bodyCount.store(bodyCount,std::memory_order_relaxed);
        g_reachFpStatus.liveCount.store(liveCount,std::memory_order_relaxed);
        g_reachFpStatus.key.store(key,std::memory_order_release);
    }

    bool ReachLayoutsEqual(const ReachFpBodyLayout& a,
                           const ReachFpBodyLayout& b)
    {
        return a.Valid() && b.Valid() && a.kind==b.kind &&
            a.paletteBodyNodeCount==b.paletteBodyNodeCount &&
            a.liveSourceNodeCount==b.liveSourceNodeCount &&
            a.rightShoulderSource==b.rightShoulderSource &&
            a.rightElbowSource==b.rightElbowSource &&
            a.rightWristSource==b.rightWristSource &&
            a.leftShoulderSource==b.leftShoulderSource &&
            a.leftElbowSource==b.leftElbowSource &&
            a.leftWristSource==b.leftWristSource &&
            a.cameraControlSource==b.cameraControlSource &&
            a.rightHandSourceDescendants==b.rightHandSourceDescendants &&
            a.leftHandSourceDescendants==b.leftHandSourceDescendants &&
            a.rightControllerOwnedSourceBranch==
                b.rightControllerOwnedSourceBranch &&
            a.leftControllerOwnedSourceBranch==
                b.leftControllerOwnedSourceBranch;
    }

    bool ReachResolvePaletteNodeCount(uint16_t tag, int& count)
    {
        count=0;
        const uintptr_t base=g_reachCamera.base;
        if (!base) return false;
        const uint8_t* modelTable=nullptr;
        if (!SafeReadBytes(reinterpret_cast<const void*>(
                base+kReachRenderModelTableRva),&modelTable,sizeof(modelTable)) ||
            !modelTable) return false;
        uint32_t modelHandle=0;
        if (!SafeReadBytes(modelTable+static_cast<size_t>(tag)*8u+4u,
                           &modelHandle,sizeof(modelHandle))) return false;
        const uint8_t* blockBase=nullptr;
        if (!SafeReadBytes(reinterpret_cast<const uint8_t*>(
                base+kReachNodeRecordBlockTableRva)+
                static_cast<size_t>(modelHandle>>28)*8u,
                &blockBase,sizeof(blockBase)) || !blockBase) return false;
        uint32_t rawCount=0;
        if (!SafeReadBytes(blockBase+static_cast<size_t>(modelHandle)*4u+0x30,
                           &rawCount,sizeof(rawCount)) ||
            rawCount==0 || rawCount>kReachFpMaxSourceNodeCount) return false;
        count=static_cast<int>(rawCount);
        return true;
    }

    const ReachFpLayoutCacheEntry* ReachFindFrozenLayout(
        int liveCount, int view, int id, int slot)
    {
        for (const ReachFpLayoutCacheEntry& entry : g_reachFpPairScope.layouts)
            if (entry.valid && entry.liveSourceCount==liveCount &&
                entry.interpolationView==view &&
                entry.interpolationId==id &&
                entry.interpolationSlot==slot && entry.layout.Valid())
                return &entry;
        return nullptr;
    }

    void ReachLearnLayout(const ReachFpInterpolationContext& context,
                          uint16_t bodyTag,
                          const ReachFpBodyLayout& layout)
    {
        if (!context.source || !layout.Valid() || !g_reachFpPairScope.armed)
            return;
        ReachFpLayoutCacheEntry* destination=nullptr;
        for (ReachFpLayoutCacheEntry& entry : g_reachFpLayoutCache)
        {
            if (entry.valid && entry.generation==g_reachFpPairScope.generation &&
                entry.liveSourceCount==context.liveSourceCount &&
                entry.interpolationView==context.interpolationView &&
                entry.interpolationId==context.interpolationId &&
                entry.interpolationSlot==context.interpolationSlot)
            {
                if (!entry.invalidateNextPair && entry.bodyTag==bodyTag &&
                    ReachLayoutsEqual(entry.layout,layout))
                    return;
                destination=&entry;
                break;
            }
            if (!entry.valid && !destination)
                destination=&entry;
        }
        if (!destination)
            destination=&g_reachFpLayoutCache[0];
        *destination={};
        destination->valid=true;
        destination->generation=g_reachFpPairScope.generation;
        destination->liveSourceCount=context.liveSourceCount;
        destination->interpolationView=context.interpolationView;
        destination->interpolationId=context.interpolationId;
        destination->interpolationSlot=context.interpolationSlot;
        destination->bodyTag=bodyTag;
        destination->learnedPreparedSerial=g_reachFpPairScope.preparedSerial;
        destination->layout=layout;
        PublishReachFpStatus(1,static_cast<int>(layout.paletteBodyNodeCount),
                             context.liveSourceCount);
    }

    void ReachInvalidateLayoutNextPair(
        const ReachFpInterpolationContext& context)
    {
        for (ReachFpLayoutCacheEntry& entry : g_reachFpLayoutCache)
            if (entry.valid && entry.generation==g_reachFpPairScope.generation &&
                entry.liveSourceCount==context.liveSourceCount &&
                entry.interpolationView==context.interpolationView &&
                entry.interpolationId==context.interpolationId &&
                entry.interpolationSlot==context.interpolationSlot)
                entry.invalidateNextPair=true;
        PublishReachFpStatus(3,0,context.liveSourceCount);
    }
    void ReachBeginFpPairScope(uint32_t generation, uint64_t preparedSerial,
                               const FpExplicitPoseTargets& targets)
    {
        g_reachFpPairScope={};
        for (auto& context : g_reachFpInterpolations)
            context={};
        g_reachFpCaptureSerial=0;
        g_reachFpPairScope.armed=true;
        g_reachFpPairScope.generation=generation;
        g_reachFpPairScope.preparedSerial=preparedSerial;
        g_reachFpPairScope.targets=targets;
        size_t frozen=0;
        for (ReachFpLayoutCacheEntry& entry : g_reachFpLayoutCache)
        {
            if (entry.valid && (entry.generation!=generation ||
                                entry.invalidateNextPair))
                entry={};
            if (DecideReachFpPairLayout(
                    entry.valid,entry.invalidateNextPair,entry.generation,
                    entry.learnedPreparedSerial,generation,preparedSerial)==
                    ReachFpPairLayoutDecision::Active &&
                frozen<kReachFpLayoutCacheCapacity)
                g_reachFpPairScope.layouts[frozen++]=entry;
        }
        g_fpStereoSolveScope={};
        g_fpStereoSolveScope.armed=true;
        if (targets.centerRootValid)
        {
            g_fpStereoSolveScope.centerRootValid=true;
            g_fpStereoSolveScope.centerRoot=targets.centerRoot;
        }
    }

    void ReachEndFpPairScope()
    {
        g_stereoEye.store(-1,std::memory_order_release);
        g_fpStereoSolveScope={};
        for (auto& context : g_reachFpInterpolations)
            context={};
        g_reachFpCaptureSerial=0;
        g_reachFpPairScope={};
    }

    bool ReachBuildCenterFpRoot(const unsigned char* compact, BoneMatrix& out)
    {
        if (!compact) return false;
        const float* pos=reinterpret_cast<const float*>(compact+0x00);
        const float* fwdIn=reinterpret_cast<const float*>(compact+0x0C);
        const float* upIn=reinterpret_cast<const float*>(compact+0x18);
        float fwd[3]={fwdIn[0],fwdIn[1],fwdIn[2]};
        float up[3]={upIn[0],upIn[1],upIn[2]};
        const float fl=sqrtf(fwd[0]*fwd[0]+fwd[1]*fwd[1]+fwd[2]*fwd[2]);
        if (!isfinite(fl) || fl<1e-4f) return false;
        for (float& component : fwd) component/=fl;
        float left[3]={up[1]*fwd[2]-up[2]*fwd[1],
                       up[2]*fwd[0]-up[0]*fwd[2],
                       up[0]*fwd[1]-up[1]*fwd[0]};
        const float ll=sqrtf(left[0]*left[0]+left[1]*left[1]+left[2]*left[2]);
        if (!isfinite(ll) || ll<1e-4f) return false;
        for (float& component : left) component/=ll;
        up[0]=fwd[1]*left[2]-fwd[2]*left[1];
        up[1]=fwd[2]*left[0]-fwd[0]*left[2];
        up[2]=fwd[0]*left[1]-fwd[1]*left[0];
        out={};
        out.scale=1.0f;
        memcpy(out.rotation,fwd,sizeof(fwd));
        memcpy(out.rotation+3,left,sizeof(left));
        memcpy(out.rotation+6,up,sizeof(up));
        memcpy(out.translation,pos,sizeof(out.translation));
        return ReachBoneMatrixFinite(out);
    }

    bool ReachBuildPreparedControllerTarget(
        const ReachVrRenderSnapshot& tracking, bool left,
        const float gameplayBase[3], BoneMatrix& out, float& meshScale)
    {
        const bool valid=left ? tracking.leftControllerValid
                              : tracking.rightAimValid;
        if (!valid || !gameplayBase) return false;
        float q[4],p[3];
        if (left)
        {
            memcpy(q,tracking.leftControllerOrientation,sizeof(q));
            memcpy(p,tracking.leftControllerPosition,sizeof(p));
        }
        else
        {
            memcpy(q,tracking.rightAimOrientation,sizeof(q));
            memcpy(p,tracking.rightAimPosition,sizeof(p));
        }
        float ql=0.0f;
        for (float component : q)
        {
            if (!isfinite(component)) return false;
            ql+=component*component;
        }
        ql=sqrtf(ql);
        if (!isfinite(ql) || ql<1e-5f) return false;
        for (float& component : q) component/=ql;
        float basis[9];
        BuildTrackedGameBasis(q,false,basis);
        if (left)
        {
            float mount[9],trimmed[9];
            BasisFromAngles(-g_config.gun_yaw_deg*0.0174533f,
                             g_config.gun_pitch_deg*0.0174533f,
                            -g_config.gun_roll_deg*0.0174533f,mount);
            MultiplyBases(basis,mount,trimmed);
            memcpy(basis,trimmed,sizeof(basis));
        }
        const float dx=p[0]-g_headPosRef[0];
        const float dy=p[1]-g_headPosRef[1];
        const float dz=p[2]-g_headPosRef[2];
        const float sh=sinf(g_headYawRef),ch=cosf(g_headYawRef);
        const float roomForward=dx*sh-dz*ch;
        const float roomRight=dx*ch+dz*sh;
        const float cg=cosf(g_gameYawRef),sg=sinf(g_gameYawRef);
        const float scale=kReachWorldUnitsPerMeter;
        const float offset[3]={
            (cg*roomForward+sg*roomRight)*scale,
            (sg*roomForward-cg*roomRight)*scale,
            dy*scale};
        const float standoff=(left
            ? Clamp(g_config.left_hand_forward_m,-0.15f,0.30f)
            : Clamp(g_config.gun_forward_m,-0.3f,0.5f))*scale;
        out={};
        out.scale=1.0f;
        memcpy(out.rotation,basis,sizeof(basis));
        for (int axis=0;axis<3;++axis)
            out.translation[axis]=gameplayBase[axis]+offset[axis]+
                basis[axis]*standoff;
        meshScale=left
            ? Clamp(g_config.left_hand_scale,0.3f,3.0f)
            : Clamp(g_config.gun_scale,0.3f,3.0f);
        return ReachBoneMatrixFinite(out) && isfinite(meshScale);
    }

    bool ReachAlignRightTargetToAuthoredBarrel(
        const BoneMatrix& baseTarget, const BoneMatrix& authoredWrist,
        BoneMatrix& alignedTarget)
    {
        float barrelLocal[3]={authoredWrist.rotation[0],
                              authoredWrist.rotation[3],
                              authoredWrist.rotation[6]};
        const float length=sqrtf(barrelLocal[0]*barrelLocal[0]+
                                 barrelLocal[1]*barrelLocal[1]+
                                 barrelLocal[2]*barrelLocal[2]);
        if (!isfinite(length) || length<1e-4f) return false;
        for (float& component : barrelLocal) component/=length;
        float worldBarrel[3]={0.0f,0.0f,0.0f};
        for (int column=0;column<3;++column)
            for (int row=0;row<3;++row)
                worldBarrel[row]+=baseTarget.rotation[column*3+row]*
                    barrelLocal[column];
        const float ray[3]={baseTarget.rotation[0],baseTarget.rotation[1],
                            baseTarget.rotation[2]};
        float swing[9],rotated[9];
        ShortestArcRotation(worldBarrel,ray,swing);
        MultiplyBases(swing,baseTarget.rotation,rotated);
        alignedTarget=baseTarget;
        memcpy(alignedTarget.rotation,rotated,sizeof(rotated));
        return ReachBoneMatrixFinite(alignedTarget);
    }

    // Reach's official Spartan/Elite body maps resolve the left wrist to source
    // node 11 and publish the complete source-space left-hand descendant mask.
    // The shared rigid (arm_ik=0) reconstruction intentionally carries the
    // whole weapon assembly from the right wrist; that is correct for the gun.
    // HREK proves the visible left-hand skin is also weighted across its hidden
    // upper-arm/forearm/humerus/radius branch. Apply the controller delta only
    // to the visible hand. The later Reach-only presentation pass co-locates
    // those hidden influence records at the solved wrist before collapsing
    // them, so their geometry disappears without pulling the glove back toward
    // four separate arm pivots. Live animation/marker graphs remain untouched.
    bool ReachBindFloatingLeftHandToController(
        const BoneMatrix& renderRoot, const FpInterpolationContext& fp,
        uint64_t leftControllerOwnedSourceBranch,
        const FpExplicitPoseTargets& targets)
    {
        if (!targets.leftWristValid)
            return true;
        if (!ReachBoneMatrixFinite(renderRoot) ||
            !ReachBoneMatrixFinite(targets.leftWrist) ||
            fp.count<=0 ||
            fp.count>static_cast<int>(kReachFpMaxSourceNodeCount) ||
            fp.lWrist<0 || fp.lWrist>=fp.count ||
            fp.lWrist>=64 ||
            !(fp.lWristDescendants&(uint64_t{1}<<fp.lWrist)) ||
            (leftControllerOwnedSourceBranch&fp.lWristDescendants)!=
                fp.lWristDescendants ||
            (leftControllerOwnedSourceBranch&fp.wristDescendants)!=0 ||
            (fp.count<64 &&
             (leftControllerOwnedSourceBranch>>fp.count)!=0))
        {
            return false;
        }

        BoneMatrix currentWristWorld{}, inverseCurrentWrist{}, worldDelta{};
        BoneMatrix inverseRoot{}, deltaRoot{}, recordDelta{};
        if (!ComposeBoneMatrices(
                renderRoot,g_fpPaletteScratch[fp.lWrist],currentWristWorld) ||
            !InvertBoneMatrix(currentWristWorld,inverseCurrentWrist) ||
            !ComposeBoneMatrices(
                targets.leftWrist,inverseCurrentWrist,worldDelta) ||
            !InvertBoneMatrix(renderRoot,inverseRoot) ||
            !ComposeBoneMatrices(worldDelta,renderRoot,deltaRoot) ||
            !ComposeBoneMatrices(inverseRoot,deltaRoot,recordDelta))
        {
            return false;
        }

        for (int node=0;node<fp.count && node<64;++node)
        {
            if (!(fp.lWristDescendants&(uint64_t{1}<<node)))
                continue;
            BoneMatrix transformed{};
            if (!ComposeBoneMatrices(
                    recordDelta,g_fpPaletteScratch[node],transformed) ||
                !ReachBoneMatrixFinite(transformed))
            {
                return false;
            }
            g_fpPaletteScratch[node]=transformed;
        }

        BoneMatrix verifiedWrist{};
        if (!ComposeBoneMatrices(
                renderRoot,g_fpPaletteScratch[fp.lWrist],verifiedWrist) ||
            !ReachBoneMatrixFinite(verifiedWrist))
        {
            return false;
        }
        for (int axis=0;axis<3;++axis)
            if (fabsf(verifiedWrist.translation[axis]-
                      targets.leftWrist.translation[axis])>0.01f)
                return false;
        return true;
    }

    void ReachCaptureFpInterpolation(
        int view, int id, int slot, bool result,
        BoneMatrix** outBones, int* outCount)
    {
        if (slot!=0 || g_reachNestedOuterSuppressed || !result ||
            !outBones || !outCount || !*outBones ||
            !g_reachFpPairScope.armed ||
            g_reachFpPairScope.generation!=g_reachCamera.generation ||
            !g_reachCamera.armed.load(std::memory_order_acquire) ||
            !g_enabled.load(std::memory_order_relaxed) ||
            !g_vrAim.load(std::memory_order_relaxed)) return;
        const int count=*outCount;
        if (count<=0 || count>static_cast<int>(kReachFpMaxSourceNodeCount))
            return;

        // Retail pairs each interpolation with its palette immediately. Retain
        // one bounded context per outstanding transaction and let the palette
        // consume the newest exact source-pointer match, as H3/ODST do.
        ReachFpInterpolationContext* destination=nullptr;
        for (auto& candidate : g_reachFpInterpolations)
            if (!candidate.valid)
            {
                destination=&candidate;
                break;
            }
        if (!destination)
            return;
        *destination={};
        ReachFpInterpolationContext& context=*destination;
        context.valid=true;
        context.generation=g_reachFpPairScope.generation;
        context.preparedSerial=g_reachFpPairScope.preparedSerial;
        context.captureSerial=++g_reachFpCaptureSerial;
        context.source=*outBones;
        context.liveSourceCount=count;
        context.interpolationView=view;
        context.interpolationId=id;
        context.interpolationSlot=slot;

        const ReachFpLayoutCacheEntry* frozen=
            ReachFindFrozenLayout(count,view,id,slot);
        if (!frozen) return;
        context.layout=frozen->layout;
        context.bodyTag=frozen->bodyTag;
        if (!context.layout.Valid() ||
            !g_reachFpPairScope.targets.centerRootValid ||
            !g_reachFpPairScope.targets.rightWristValid ||
            !ReachBoneMatrixFinite(g_reachFpPairScope.targets.centerRoot) ||
            !ReachBoneMatrixFinite(g_reachFpPairScope.targets.rightWrist) ||
            !isfinite(g_reachFpPairScope.targets.rightScale) ||
            g_reachFpPairScope.targets.rightScale<=0.0f ||
            (g_reachFpPairScope.targets.leftWristValid &&
             (!ReachBoneMatrixFinite(g_reachFpPairScope.targets.leftWrist) ||
              !isfinite(g_reachFpPairScope.targets.leftScale) ||
              g_reachFpPairScope.targets.leftScale<=0.0f))) return;
        const size_t liveBytes=static_cast<size_t>(count)*sizeof(BoneMatrix);
        if (!SafeReadBytes(context.source,context.untouchedLive,liveBytes))
            return;
        for (int node=0;node<count;++node)
            if (!ReachBoneMatrixFinite(context.untouchedLive[node]))
                return;
        context.targets=g_reachFpPairScope.targets;

        // Match the accepted H3/ODST split exactly: the live interpolation bank
        // is only for marker/muzzle/attachment consumers and receives one rigid
        // controller transform. Every visible palette below is reconstructed
        // independently from untouchedLive into private scratch.
        // Prepared wrists are already absolute world targets (pre-head gameplay
        // base + tracked controller displacement). Match H3/ODST by using the
        // center root only for world/record conversion; adding its head
        // translation again would make the assembly drift on a physical turn.
        FpExplicitPoseTargets markerTargets=context.targets;
        BoneMatrix alignedRight{};
        if (!ReachAlignRightTargetToAuthoredBarrel(
                markerTargets.rightWrist,
                context.untouchedLive[context.layout.rightWristSource],
                alignedRight)) return;
        if (!ApplyControllerToMarkerBonesWithTarget(
                markerTargets.centerRoot,alignedRight,markerTargets.rightScale,
                context.source,count,context.layout.rightWristSource))
        {
            SafeWriteBytes(context.source,context.untouchedLive,liveBytes);
            return;
        }
        for (int node=0;node<count;++node)
        {
            BoneMatrix checked{};
            if (!SafeReadBytes(context.source+node,&checked,sizeof(checked)) ||
                !ReachBoneMatrixFinite(checked))
            {
                SafeWriteBytes(context.source,context.untouchedLive,liveBytes);
                return;
            }
        }
        context.transformed=true;
    }
    __declspec(noinline) bool __fastcall ReachFpInterpolate(
        int view, int id, int slot, BoneMatrix** outBones, int* outCount)
    {
        g_reachCamera.activeCallbacks.fetch_add(1,std::memory_order_acq_rel);
        bool result=false;
        __try
        {
            ReachFpInterpolateFn original=g_reachOrigFpInterpolate;
            if (original)
                result=original(view,id,slot,outBones,outCount);
            ReachCaptureFpInterpolation(view,id,slot,result,outBones,outCount);
        }
        __finally
        {
            g_reachCamera.activeCallbacks.fetch_sub(1,std::memory_order_acq_rel);
        }
        return result;
    }

    bool ReachRestoreFpLiveGraph(ReachFpInterpolationContext& context)
    {
        if (!context.transformed)
            return true;
        if (!context.source || context.liveSourceCount<=0 ||
            context.liveSourceCount>static_cast<int>(kReachFpMaxSourceNodeCount))
            return false;
        const size_t bytes=static_cast<size_t>(context.liveSourceCount)*
            sizeof(BoneMatrix);
        if (!SafeWriteBytes(context.source,context.untouchedLive,bytes))
            return false;
        context.transformed=false;
        return true;
    }

    void ReachProcessFpPalette(
        uint16_t tag, const BoneMatrix* root, BoneMatrix* destination,
        uintptr_t unused, const BoneMatrix* source, const int32_t* boneMap)
    {
        ReachFpPaletteFn original=g_reachOrigFpPalette;
        if (g_reachNestedOuterSuppressed)
        {
            if (original)
                original(tag,root,destination,unused,source,boneMap);
            return;
        }

        // Match and consume the newest outstanding interpolation transaction by
        // exact source pointer. This is the same final-palette ownership model
        // used by Halo 3 and ODST; no render-model palette is admitted by guess.
        ReachFpInterpolationContext* matched=nullptr;
        for (auto& candidate : g_reachFpInterpolations)
        {
            const bool current=candidate.valid && source &&
                candidate.source==source && g_reachFpPairScope.armed &&
                candidate.generation==g_reachCamera.generation &&
                candidate.generation==g_reachFpPairScope.generation &&
                candidate.preparedSerial==g_reachFpPairScope.preparedSerial;
            if (current && (!matched ||
                            candidate.captureSerial>matched->captureSerial))
                matched=&candidate;
        }
        ReachFpInterpolationContext context{};
        const bool contextCurrent=matched!=nullptr;
        if (matched)
        {
            context=*matched;
            matched->valid=false;
        }
        const BoneMatrix* selectedSource=source;

        int paletteCount=0;
        int32_t mapCopy[64]{};
        ReachFpBodyLayout observed{};
        bool bodyLayout=false;
        if (contextCurrent && boneMap &&
            ReachResolvePaletteNodeCount(tag,paletteCount) &&
            (paletteCount==static_cast<int>(kReachSpartanFpBodyNodeCount) ||
             paletteCount==static_cast<int>(kReachEliteFpBodyNodeCount)) &&
            SafeReadBytes(boneMap,mapCopy,
                static_cast<size_t>(paletteCount)*sizeof(int32_t)))
        {
            bodyLayout=ResolveReachFpBodyLayout(
                std::span<const int32_t>{mapCopy,
                    static_cast<size_t>(paletteCount)},
                static_cast<size_t>(context.liveSourceCount),observed);
        }
        const bool frozenLayoutValid=context.layout.Valid();
        const bool exactBodyMatchesFrozen=bodyLayout && frozenLayoutValid &&
            tag==context.bodyTag && ReachLayoutsEqual(context.layout,observed);
        const ReachFpPaletteAction action=DecideReachFpPaletteAction(
            contextCurrent,frozenLayoutValid,context.transformed,bodyLayout,
            exactBodyMatchesFrozen,tag==context.bodyTag);
        auto restoreStockAndInvalidate=[&](bool relearnExactBody) {
            if (context.transformed)
            {
                selectedSource=context.untouchedLive;
                ReachRestoreFpLiveGraph(context);
            }
            ReachInvalidateLayoutNextPair(context);
            if (relearnExactBody)
                ReachLearnLayout(context,tag,observed);
        };

        if (action==ReachFpPaletteAction::LearnStockOnly)
        {
            ReachLearnLayout(context,tag,observed);
        }
        else if (action==ReachFpPaletteAction::RestoreStockAndInvalidate)
        {
            restoreStockAndInvalidate(bodyLayout);
        }
        else if (action==ReachFpPaletteAction::ArticulateKnownTransaction)
        {
            if (!root || !context.targets.rightWristValid ||
                context.liveSourceCount<=0 ||
                context.liveSourceCount>
                    static_cast<int>(kReachFpMaxSourceNodeCount) ||
                !ReachBoneMatrixFinite(*root))
            {
                restoreStockAndInvalidate(false);
            }
            else
            {
                FpInterpolationContext fp{};
                fp.source=source;
                fp.count=context.liveSourceCount;
                fp.player=context.interpolationView;
                fp.slot=context.interpolationSlot;
                fp.wrist=context.layout.rightWristSource;
                fp.cameraControl=context.layout.cameraControlSource;
                fp.elbow=context.layout.rightElbowSource;
                fp.shoulder=context.layout.rightShoulderSource;
                fp.wristDescendants=
                    context.layout.rightHandSourceDescendants;
                fp.heldObjectStart=static_cast<int>(
                    context.layout.paletteBodyNodeCount);
                fp.lWrist=context.layout.leftWristSource;
                fp.lElbow=context.layout.leftElbowSource;
                fp.lShoulder=context.layout.leftShoulderSource;
                fp.lWristDescendants=
                    context.layout.leftHandSourceDescendants;
                fp.valid=true;
                selectedSource=context.untouchedLive;
                const BoneMatrix* replacement=selectedSource;
                FpExplicitPoseTargets targets=context.targets;
                targets.centerRoot.scale=root->scale;
                BoneMatrix alignedRight{};
                if (!ReachAlignRightTargetToAuthoredBarrel(
                        targets.rightWrist,
                        context.untouchedLive[
                            context.layout.rightWristSource],
                        alignedRight))
                {
                    restoreStockAndInvalidate(false);
                    if (original)
                        original(tag,root,destination,unused,
                                 selectedSource,boneMap);
                    return;
                }
                targets.rightWrist=alignedRight;
                // Publish the pair the effect system needs. `stock` is where
                // the engine still believes the weapon is - the player's body -
                // and `alignedRight` is where the player actually sees it. The
                // muzzle flash resolves against the former; re-parenting it
                // between the two puts it on the gun with no offset.
                // MUST be centerRoot, not untouchedLive[rightWristSource].
                // The live-graph bone is in the first-person skeleton's LOCAL
                // space; effect matrices are in world space. Comparing them put
                // the nearest approach at 75,842 mm (7478cb7 probe) - not a
                // near miss, two different coordinate systems. centerRoot and
                // alignedRight are both absolute world poses supplied by the
                // Reach adapter, so distances between them and an effect matrix
                // are meaningful. centerRoot sits at the player's head, which
                // is exactly where the stuck flash renders.
                if (context.targets.centerRootValid)
                {
                    ReachPublishWeaponAnchor(
                        context.targets.centerRoot, alignedRight,
                        g_reachCamera.generation);
                }
                if (ReachBoneMatrixFinite(alignedRight))
                {
                    g_reachWeaponAnchorMoved = alignedRight;
                    g_reachWeaponAnchorPending = true;
                }
                const bool reconstructed=ReconstructVisiblePaletteSource(
                    tag,fp,*root,source,replacement,&targets,
                    context.untouchedLive);
                selectedSource=replacement;
                const bool leftHandBound=reconstructed &&
                    selectedSource==g_fpPaletteScratch &&
                    ReachBindFloatingLeftHandToController(
                        *root,fp,
                        context.layout.leftControllerOwnedSourceBranch,
                        targets);
                bool outputFinite=reconstructed && leftHandBound;
                for (int i=0;outputFinite && i<fp.count;++i)
                    outputFinite=ReachBoneMatrixFinite(g_fpPaletteScratch[i]);
                if (!outputFinite)
                {
                    restoreStockAndInvalidate(false);
                }
                else
                {
                    // Reach temporarily forces the accepted floating-hands
                    // presentation independent of the universal config. The
                    // exact hand masks and held-object boundary come from the
                    // pinned HREK/retail body maps; no arm or body geometry is
                    // admitted into this title's visible FP palette.
                    if (selectedSource==g_fpPaletteScratch)
                    {
                        const uint64_t keep=fp.wristDescendants|
                            fp.lWristDescendants;
                        const uint64_t hiddenLeft=
                            context.layout.leftControllerOwnedSourceBranch&
                            ~fp.lWristDescendants;
                        const uint64_t hiddenRight=
                            context.layout.rightControllerOwnedSourceBranch&
                            ~fp.wristDescendants;
                        BoneMatrix collapsedAtLeftWrist=
                            g_fpPaletteScratch[fp.lWrist];
                        collapsedAtLeftWrist.scale=0.0001f;
                        BoneMatrix collapsedAtRightWrist=
                            g_fpPaletteScratch[fp.wrist];
                        collapsedAtRightWrist.scale=0.0001f;
                        for (int i=0;i<fp.count;++i)
                        {
                            const bool hand=i<64 &&
                                (keep&(uint64_t{1}<<i));
                            const bool held=fp.heldObjectStart>=0 &&
                                i>=fp.heldObjectStart;
                            if (!hand && !held)
                            {
                                if (i<64 &&
                                    (hiddenLeft&(uint64_t{1}<<i)))
                                {
                                    g_fpPaletteScratch[i]=
                                        collapsedAtLeftWrist;
                                }
                                else if (i<64 &&
                                         (hiddenRight&(uint64_t{1}<<i)))
                                {
                                    g_fpPaletteScratch[i]=
                                        collapsedAtRightWrist;
                                }
                                else
                                {
                                    g_fpPaletteScratch[i].scale=0.0001f;
                                }
                            }
                        }
                    }
                    PublishReachFpStatus(
                        2,static_cast<int>(
                            context.layout.paletteBodyNodeCount),
                        context.liveSourceCount);
                }
            }
        }
        if (original)
            original(tag,root,destination,unused,selectedSource,boneMap);

        // Candidate 511eb0b put alignedRight here after the visible palette had
        // already been composed. alignedRight is an absolute world pose; the
        // live interpolation records are local to the first-person root. The
        // same source file had already measured those as different coordinate
        // spaces, but the candidate nevertheless copied the world matrix into
        // one local record. It was ineffective for the muzzle flash and was
        // superseded by b942078's accepted loaded-tag retarget, yet the bad
        // write remained active. Do not let a later render consumer observe
        // that internally inconsistent skeleton. The coherent rigid local
        // graph transform performed before the palette remains unchanged.
        if (g_reachWeaponAnchorPending &&
            context.layout.rightWristSource >= 0 &&
            context.layout.rightWristSource < context.liveSourceCount &&
            context.source)
        {
            if constexpr (kReachPostPaletteWorldWristWriteEnabled)
            {
                SafeWriteBytes(
                    const_cast<BoneMatrix*>(context.source) +
                        context.layout.rightWristSource,
                    &g_reachWeaponAnchorMoved, sizeof(BoneMatrix));
                g_reachLiveGraphWeaponWrites.fetch_add(
                    1, std::memory_order_relaxed);
            }
            else
            {
                g_reachWorldWristWritesPrevented.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }
        g_reachWeaponAnchorPending = false;
    }
    __declspec(noinline) void __fastcall ReachFpPalette(
        uint16_t tag, const BoneMatrix* root, BoneMatrix* destination,
        uintptr_t unused, const BoneMatrix* source, const int32_t* boneMap)
    {
        g_reachCamera.activeCallbacks.fetch_add(1,std::memory_order_acq_rel);
        __try
        {
            ReachProcessFpPalette(
                tag,root,destination,unused,source,boneMap);
        }
        __finally
        {
            g_reachCamera.activeCallbacks.fetch_sub(1,std::memory_order_acq_rel);
        }
    }

    uintptr_t ReachMainRenderViewBody(
        uintptr_t workspace, uintptr_t playerView, uint32_t windowIndex,
        uintptr_t returnAddress)
    {
        const ReachOwnerScope previous = g_reachOwnerScope;
        if (g_reachNestedOuterSuppressed)
            return g_reachOrigMainRenderView(
                workspace, playerView, windowIndex);
        if (previous.active)
        {
            // The camera stack can saturate at depth three. Leaving the parent
            // TLS token visible while a nested stock call runs could let its
            // inner renderer satisfy the parent's pointers/depth by accident.
            // Suppress the complete nested call tree. A nested stock FP
            // interpolation can reuse and overwrite the parent's live graph,
            // so preserve that bounded graph as well as the owner token and
            // restore it even if stock raises an SEH exception.
            uintptr_t nestedResult = 0;
            struct ParentGraphSnapshot
            {
                bool contextValid = false;
                bool graphSaved = false;
                BoneMatrix* graph = nullptr;
                int count = 0;
                BoneMatrix records[kReachFpMaxSourceNodeCount]{};
            } savedParents[kReachFpTransactionCapacity]{};
            for (size_t i=0;i<kReachFpTransactionCapacity;++i)
            {
                const ReachFpInterpolationContext& context=
                    g_reachFpInterpolations[i];
                ParentGraphSnapshot& saved=savedParents[i];
                saved.contextValid=context.valid;
                saved.graph=context.source;
                saved.count=context.liveSourceCount;
                saved.graphSaved=context.valid && saved.graph &&
                    saved.count>0 &&
                    saved.count<=static_cast<int>(kReachFpMaxSourceNodeCount) &&
                    SafeReadBytes(saved.graph,saved.records,
                        static_cast<size_t>(saved.count)*sizeof(BoneMatrix));
            }
            g_reachOwnerScope = {};
            g_reachNestedOuterSuppressed = true;
            __try
            {
                nestedResult = g_reachOrigMainRenderView(
                    workspace, playerView, windowIndex);
            }
            __finally
            {
                for (size_t i=0;i<kReachFpTransactionCapacity;++i)
                {
                    const ParentGraphSnapshot& saved=savedParents[i];
                    if (saved.contextValid &&
                        (!saved.graphSaved || !SafeWriteBytes(
                             saved.graph,saved.records,
                             static_cast<size_t>(saved.count)*
                                 sizeof(BoneMatrix))))
                        g_reachFpInterpolations[i]={};
                }
                g_reachNestedOuterSuppressed = false;
                g_reachOwnerScope = previous;
            }
            return nestedResult;
        }

        const ReachModuleEpoch epoch{
            g_reachCamera.base, g_reachCamera.generation};
        uintptr_t expectedWorkspace = 0;
        uintptr_t expectedPlayerView = 0;
        const ReachPreflightToken preflight =
            ReachRenderCandidate_GetPreflight(epoch);
        const ReachPreparedFrameToken prepared = VR_ReachPreparedFrame(epoch);
        if (!g_reachCamera.armed.load(std::memory_order_acquire) ||
            windowIndex != 0 || !g_reachHelpers.Ready() ||
            ClassifyReachOuterRenderCaller(
                epoch.moduleBase, kReachRetailImageSize, returnAddress) !=
                ReachOuterRenderCaller::NormalPlayer ||
            !ReachAddressFromRva(
                epoch.moduleBase, kReachRetailImageSize,
                kReachDefaultWorkspaceRva, expectedWorkspace) ||
            !ReachAddressFromRva(
                epoch.moduleBase, kReachRetailImageSize,
                kReachPlayerViewArrayRva, expectedPlayerView) ||
            workspace != expectedWorkspace ||
            playerView != expectedPlayerView ||
            !preflight.Complete() ||
            !ReachRenderCandidate_IsPreflightCurrent(preflight) ||
            !prepared.Ready() || !VR_ReachDisplayReady(epoch))
        {
            return g_reachOrigMainRenderView(
                workspace, playerView, windowIndex);
        }

        ReachApplyMotionBlurSetting();

        ReachVrRenderSnapshot tracking{};
        if (!VR_ReachGetRenderSnapshot(prepared, tracking) ||
            tracking.preparedSerial != prepared.Serial())
        {
            return g_reachOrigMainRenderView(
                workspace, playerView, windowIndex);
        }

        const int32_t stackDepthBefore =
            *reinterpret_cast<const int32_t*>(
                epoch.moduleBase + kReachCameraStackDepthRva);
        const uintptr_t expectedOwner =
            playerView + kReachPlayerViewCameraStateOffset;
        unsigned char* primaryCompact =
            reinterpret_cast<unsigned char*>(workspace);
        unsigned char* primaryDerived = reinterpret_cast<unsigned char*>(
            workspace + kReachPrimaryDerivedOffset);
        const unsigned char* secondaryCompact =
            reinterpret_cast<const unsigned char*>(
                workspace + kReachSecondaryCompactOffset);
        const unsigned char* secondaryDerived =
            reinterpret_cast<const unsigned char*>(
                workspace + kReachSecondaryDerivedOffset);
        ReachCompactCameraObservation observedPrimary{};
        ReachCompactCameraObservation observedSecondary{};
        if (stackDepthBefore < -1 || stackDepthBefore >= 3 ||
            *reinterpret_cast<const uintptr_t*>(
                epoch.moduleBase + kReachRenderCameraOwnerRva) !=
                    expectedOwner ||
            *reinterpret_cast<const uint32_t*>(
                epoch.moduleBase + kReachSelectedSpecializationRva) != 0 ||
            !ValidateReachCompactCamera(
                primaryCompact, kReachCompactCameraBytes,
                observedPrimary) ||
            !ValidateReachCompactCamera(
                secondaryCompact, kReachCompactCameraBytes,
                observedSecondary) ||
            memcmp(primaryCompact, secondaryCompact,
                   kReachCompactCameraBytes) != 0 ||
            memcmp(primaryDerived, secondaryDerived,
                   kReachDerivedBlockSize) != 0)
        {
            return g_reachOrigMainRenderView(
                workspace, playerView, windowIndex);
        }

        ReachOwnerScope candidate{};
        candidate.workspace = workspace;
        candidate.playerView = playerView;
        candidate.cameraStackDepthBefore = stackDepthBefore;
        candidate.preparedSerial = prepared.Serial();
        memcpy(candidate.gameplayBasePosition,primaryCompact+0x00,
               sizeof(candidate.gameplayBasePosition));
        ReachSymmetricFovCover cullCover{};
        if (!ReachCollectEyeInputs(
                observedPrimary.renderBounds, tracking,
                candidate.eyes, cullCover))
        {
            return g_reachOrigMainRenderView(
                workspace, playerView, windowIndex);
        }

        ReachVrRenderAccess access{};
        if (!VR_ReachBeginRenderAccess(epoch, prepared, access))
            return g_reachOrigMainRenderView(
                workspace, playerView, windowIndex);

        alignas(16) unsigned char headDerived[kReachDerivedBlockSize]{};
        bool headCullReady = false;
        __try
        {
            headCullReady = ReachBuildHeadCullCamera(
                primaryCompact, cullCover, tracking,
                candidate.headCenter, headDerived);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            headCullReady = false;
        }
        const ReachPreparedFrameToken currentPrepared =
            VR_ReachPreparedFrame(epoch);
        if (!headCullReady || !currentPrepared.Ready() ||
            currentPrepared.Serial() != candidate.preparedSerial ||
            access.preparedSerial != candidate.preparedSerial)
        {
            VR_ReachEndRenderAccess(access);
            return g_reachOrigMainRenderView(
                workspace, playerView, windowIndex);
        }

        candidate.fpTargets={};
        candidate.fpTargets.centerRootValid=ReachBuildCenterFpRoot(
            candidate.headCenter,candidate.fpTargets.centerRoot);
        candidate.fpTargets.rightWristValid=
            ReachBuildPreparedControllerTarget(
                tracking,false,candidate.gameplayBasePosition,
                candidate.fpTargets.rightWrist,
                candidate.fpTargets.rightScale);
        candidate.fpTargets.leftWristValid=
            ReachBuildPreparedControllerTarget(
                tracking,true,candidate.gameplayBasePosition,
                candidate.fpTargets.leftWrist,
                candidate.fpTargets.leftScale);

        alignas(16) unsigned char savedWorkspace[kReachCameraPairDataSize];
        alignas(16) unsigned char savedPv[kReachPvSnapshotBytes];
        memcpy(savedWorkspace, reinterpret_cast<const void*>(workspace),
               kReachCameraPairDataSize);
        memcpy(savedPv,
               reinterpret_cast<const void*>(
                   playerView + kReachPvSnapshotBegin),
               kReachPvSnapshotBytes);

        // Commit coherent head-centre pre-ownership state before the outer
        // function's visibility work. Visibility reads the secondary workspace
        // pair before player_view_render. Updating both with the exact proven
        // setup helpers prevents a claimed transaction from mixing head-owned
        // culling with old gun/aim-owned raster matrices before terminal rejection.
        bool headCommitReady = false;
        __try
        {
            memcpy(primaryCompact, candidate.headCenter,
                   kReachCompactCameraBytes);
            memcpy(primaryDerived, headDerived, kReachDerivedBlockSize);
            memcpy(reinterpret_cast<void*>(
                       workspace + kReachSecondaryCompactOffset),
                   candidate.headCenter, kReachCompactCameraBytes);
            memcpy(reinterpret_cast<void*>(
                       workspace + kReachSecondaryDerivedOffset),
                   headDerived, kReachDerivedBlockSize);
            g_reachHelpers.cameraState(
                reinterpret_cast<void*>(expectedOwner),
                candidate.headCenter);
            g_reachHelpers.matrix(
                reinterpret_cast<void*>(
                    playerView + kReachPlayerViewCurrentMatricesOffset),
                headDerived,
                headDerived + kReachDerivedProjectionOffset,
                candidate.headCenter + kReachCompactRenderBoundsOffset,
                reinterpret_cast<void*>(
                    playerView +
                    kReachPlayerViewProjectionOffsetPairOffset));
            *reinterpret_cast<uintptr_t*>(
                epoch.moduleBase + kReachRenderCameraOwnerRva) = expectedOwner;
            headCommitReady = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            headCommitReady = false;
        }
        if (!headCommitReady)
        {
            memcpy(reinterpret_cast<void*>(workspace), savedWorkspace,
                   kReachCameraPairDataSize);
            memcpy(reinterpret_cast<void*>(
                       playerView + kReachPvSnapshotBegin),
                   savedPv, kReachPvSnapshotBytes);
            *reinterpret_cast<uintptr_t*>(
                epoch.moduleBase + kReachRenderCameraOwnerRva) = expectedOwner;
            VR_ReachEndRenderAccess(access);
            return g_reachOrigMainRenderView(
                workspace, playerView, windowIndex);
        }
        candidate.renderAccess = &access;
        candidate.active = true;
        g_reachOwnerScope = candidate;
        // Reach builds the first-person interpolation graph and visible
        // palettes in main_render_view before it enters player_view_render.
        // Freeze the prepared-frame targets/layouts at this admitted outer
        // boundary so that pre-inner work can learn or apply the exact body
        // layout, and keep the same scope across both stereo eyes.
        ReachBeginFpPairScope(
            epoch.generation,candidate.preparedSerial,candidate.fpTargets);

        uintptr_t result = 0;
        __try
        {
            result = g_reachOrigMainRenderView(
                workspace, playerView, windowIndex);
        }
        __finally
        {
            ReachEndFpPairScope();
            // Restore only the proven camera-pair data. The engine owns the
            // camera-stack callback at +0x2A8 and its push/pop lifetime.
            const uint8_t finalLastWindow =
                *reinterpret_cast<const uint8_t*>(
                    playerView + kReachLastWindowFlagOffset);
            memcpy(reinterpret_cast<void*>(workspace), savedWorkspace,
                   kReachCameraPairDataSize);
            memcpy(reinterpret_cast<void*>(
                       playerView + kReachPvSnapshotBegin),
                   savedPv, kReachPvSnapshotBytes);
            *reinterpret_cast<uint8_t*>(
                playerView + kReachLastWindowFlagOffset) = finalLastWindow;
            g_reachOwnerScope = previous;
            VR_ReachEndRenderAccess(access);
        }
        return result;
    }

    __declspec(noinline) uintptr_t __fastcall ReachMainRenderViewDetour(
        uintptr_t workspace, uintptr_t playerView, uint32_t windowIndex)
    {
        const uintptr_t returnAddress =
            reinterpret_cast<uintptr_t>(_ReturnAddress());
        uintptr_t result = 0;
        g_reachCamera.activeCallbacks.fetch_add(
            1, std::memory_order_acq_rel);
        __try
        {
            result = ReachMainRenderViewBody(
                workspace, playerView, windowIndex, returnAddress);
        }
        __finally
        {
            g_reachCamera.activeCallbacks.fetch_sub(
                1, std::memory_order_acq_rel);
        }
        return result;
    }

    struct ReachDetourCodeRange
    {
        DWORD64 begin = 0;
        DWORD64 end = 0;
    };

    struct ReachFrozenThread
    {
        HANDLE handle = nullptr;
        DWORD threadId = 0;
        bool suspended = false;
    };

    // Worker-only process freeze used after both Reach hooks are disabled. It
    // closes the small ingress window in which a thread is already inside a
    // MinHook relay or the detour prologue but has not incremented the counter.
    class ReachThreadFreeze
    {
    public:
        ~ReachThreadFreeze() { Release(); }

        bool Capture()
        {
            if (!m_threads.empty())
                return false;

            const DWORD processId = GetCurrentProcessId();
            const DWORD currentThreadId = GetCurrentThreadId();
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return false;

            bool enumerateOk = true;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (!Thread32First(snapshot, &entry))
                enumerateOk = false;
            while (enumerateOk)
            {
                if (entry.th32OwnerProcessID == processId &&
                    entry.th32ThreadID != currentThreadId)
                {
                    HANDLE thread = OpenThread(
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                            THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                        FALSE, entry.th32ThreadID);
                    if (!thread)
                    {
                        if (GetLastError() != ERROR_INVALID_PARAMETER)
                            enumerateOk = false;
                    }
                    else
                    {
                        m_threads.push_back(
                            {thread, entry.th32ThreadID, false});
                    }
                }
                if (!Thread32Next(snapshot, &entry))
                {
                    if (GetLastError() != ERROR_NO_MORE_FILES)
                        enumerateOk = false;
                    break;
                }
            }
            CloseHandle(snapshot);
            if (!enumerateOk)
            {
                Release();
                return false;
            }

            // Complete every allocation before the first suspension. All
            // captured threads then remain frozen through the RIP/counter
            // snapshot, so no observed thread can move between safe ranges.
            for (ReachFrozenThread& thread : m_threads)
            {
                const DWORD previousCount = SuspendThread(thread.handle);
                if (previousCount == static_cast<DWORD>(-1))
                {
                    if (WaitForSingleObject(thread.handle, 0) != WAIT_OBJECT_0)
                    {
                        Release();
                        return false;
                    }
                    continue;
                }
                thread.suspended = true;
            }

            if (!FrozenSnapshotIsComplete(processId, currentThreadId))
            {
                Release();
                return false;
            }
            return true;
        }

        bool Release()
        {
            bool ok = true;
            for (size_t i = m_threads.size(); i > 0; --i)
            {
                ReachFrozenThread& thread = m_threads[i - 1];
                if (thread.suspended &&
                    ResumeThread(thread.handle) == static_cast<DWORD>(-1) &&
                    WaitForSingleObject(thread.handle, 0) != WAIT_OBJECT_0)
                {
                    ok = false;
                }
                CloseHandle(thread.handle);
            }
            m_threads.clear();
            return ok;
        }

        const std::vector<ReachFrozenThread>& Threads() const
        {
            return m_threads;
        }

    private:
        bool FrozenSnapshotIsComplete(
            DWORD processId, DWORD currentThreadId) const
        {
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return false;

            bool complete = true;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            if (!Thread32First(snapshot, &entry))
                complete = false;
            while (complete)
            {
                if (entry.th32OwnerProcessID == processId &&
                    entry.th32ThreadID != currentThreadId)
                {
                    bool foundFrozen = false;
                    for (const ReachFrozenThread& thread : m_threads)
                    {
                        if (thread.threadId == entry.th32ThreadID)
                        {
                            foundFrozen = thread.suspended ||
                                WaitForSingleObject(thread.handle, 0) ==
                                    WAIT_OBJECT_0;
                            break;
                        }
                    }
                    if (!foundFrozen)
                        complete = false;
                }
                if (!complete || !Thread32Next(snapshot, &entry))
                {
                    if (complete && GetLastError() != ERROR_NO_MORE_FILES)
                        complete = false;
                    break;
                }
            }
            CloseHandle(snapshot);
            return complete;
        }

        std::vector<ReachFrozenThread> m_threads;
    };

    bool ResolveReachDetourCodeRange(
        const void* function, ReachDetourCodeRange& range)
    {
        DWORD64 imageBase = 0;
        const PRUNTIME_FUNCTION runtimeFunction = RtlLookupFunctionEntry(
            reinterpret_cast<DWORD64>(function), &imageBase, nullptr);
        if (!runtimeFunction ||
            runtimeFunction->EndAddress <= runtimeFunction->BeginAddress)
        {
            return false;
        }
        range.begin = imageBase + runtimeFunction->BeginAddress;
        range.end = imageBase + runtimeFunction->EndAddress;
        return true;
    }

    bool ScanForReachDetourIngress(bool& busy)
    {
        static bool rangesResolved = false;
        static ReachDetourCodeRange ranges[10]{};
        if (!rangesResolved)
        {
            const void* functions[] = {
                reinterpret_cast<const void*>(&ReachMainRenderViewDetour),
                reinterpret_cast<const void*>(&ReachPlayerViewRenderDetour),
                reinterpret_cast<const void*>(&ReachFpInterpolate),
                reinterpret_cast<const void*>(&ReachFpPalette),
                reinterpret_cast<const void*>(&ReachFpCameraRebuildDetour),
                reinterpret_cast<const void*>(&ReachRenderSsaoDetour),
                reinterpret_cast<const void*>(&ReachHudDrawWidgetDetour),
                reinterpret_cast<const void*>(&ReachObserverCameraDetour),
                reinterpret_cast<const void*>(&ReachEffectLocationDetour),
                reinterpret_cast<const void*>(&ReachRainRenderDetour),
            };
            static_assert(_countof(functions) == _countof(ranges));
            bool resolved = true;
            for (size_t i = 0; i < _countof(functions); ++i)
                resolved &= ResolveReachDetourCodeRange(functions[i], ranges[i]);
            if (!resolved)
                return false;
            rangesResolved = true;
        }

        // Snapshot these worker-owned fields before suspending anything. They
        // cannot change until this cleanup call either removes or retains them.
        void* const targets[] = {
            g_reachCamera.outerTarget,
            g_reachCamera.innerTarget,
            g_reachCamera.fpInterpolateTarget,
            g_reachCamera.fpPaletteTarget,
            g_reachCamera.fpCameraTarget,
            g_reachCamera.ssaoTarget,
            g_reachCamera.hudDrawWidgetTarget,
            g_reachCamera.observerCameraTarget,
            g_reachCamera.effectLocationTarget,
            g_reachCamera.rainRenderTarget,
        };
        void* const trampolines[] = {
            reinterpret_cast<void*>(g_reachOrigMainRenderView),
            reinterpret_cast<void*>(g_reachOrigPlayerViewRender),
            reinterpret_cast<void*>(g_reachOrigFpInterpolate),
            reinterpret_cast<void*>(g_reachOrigFpPalette),
            reinterpret_cast<void*>(g_reachOrigFpCameraRebuild),
            reinterpret_cast<void*>(g_reachOrigRenderSsao),
            reinterpret_cast<void*>(g_reachOrigHudDrawWidget),
            reinterpret_cast<void*>(g_reachOrigObserverCamera),
            reinterpret_cast<void*>(g_reachOrigEffectLocation),
            reinterpret_cast<void*>(g_reachOrigRainRender),
        };
        static_assert(_countof(targets) == _countof(ranges));
        static_assert(_countof(trampolines) == _countof(ranges));

        ReachThreadFreeze frozenThreads;
        if (!frozenThreads.Capture())
            return false;

        busy = false;
        bool scanOk = true;
        for (const ReachFrozenThread& thread : frozenThreads.Threads())
        {
            if (!thread.suspended)
                continue;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(thread.handle, &context))
            {
                if (WaitForSingleObject(thread.handle, 0) != WAIT_OBJECT_0)
                    scanOk = false;
                continue;
            }

            const DWORD64 instruction = context.Rip;
            for (size_t i = 0; i < _countof(ranges); ++i)
            {
                if (!targets[i])
                    continue;
                if (instruction >= ranges[i].begin &&
                    instruction < ranges[i].end)
                {
                    busy = true;
                }

                // MinHook v1.3.4 x64 keeps both trampoline and relay in the
                // 64-byte slot rooted at ppOriginal. The relay can be entered
                // before the wrapper increments activeCallbacks.
                const DWORD64 trampoline =
                    reinterpret_cast<DWORD64>(trampolines[i]);
                if (!trampoline)
                {
                    scanOk = false;
                    continue;
                }
                if (instruction >= trampoline &&
                    instruction < trampoline + 64)
                {
                    busy = true;
                }
            }
        }
        if (g_reachCamera.activeCallbacks.load(std::memory_order_acquire) != 0)
            busy = true;

        // Hooks are disabled and every other live thread remains suspended
        // through this RIP/counter snapshot, so an all-clear survives resume.
        const bool resumed = frozenThreads.Release();
        return scanOk && resumed;
    }

    bool WaitForReachDetourQuiescence()
    {
        for (unsigned waited = 0; waited < 2000; ++waited)
        {
            if (g_reachCamera.activeCallbacks.load(
                    std::memory_order_acquire) == 0)
            {
                bool ingressBusy = false;
                if (!ScanForReachDetourIngress(ingressBusy))
                {
                    LOG("Reach camera cleanup: detour ingress scan failed");
                    return false;
                }
                if (!ingressBusy &&
                    g_reachCamera.activeCallbacks.load(
                        std::memory_order_acquire) == 0)
                    return true;
            }
            Sleep(1);
        }
        LOG("Reach camera cleanup: callbacks did not reach verified quiescence");
        return false;
    }

    bool ReachDisableStatusIsSafe(MH_STATUS status)
    {
        return status == MH_OK || status == MH_ERROR_DISABLED ||
            status == MH_ERROR_NOT_CREATED;
    }

    bool DisableAndRemoveReachHooks()
    {
        bool disabledAll = true;
        void* const targets[] = {
            g_reachCamera.rainRenderTarget,
            g_reachCamera.effectLocationTarget,
            g_reachCamera.observerCameraTarget,
            g_reachCamera.hudDrawWidgetTarget,
            g_reachCamera.ssaoTarget,
            g_reachCamera.outerTarget,
            g_reachCamera.innerTarget,
            g_reachCamera.fpInterpolateTarget,
            g_reachCamera.fpPaletteTarget,
            g_reachCamera.fpCameraTarget,
        };
        for (void* target : targets)
        {
            if (!target)
                continue;
            const MH_STATUS status = MH_DisableHook(target);
            if (!ReachDisableStatusIsSafe(status))
            {
                disabledAll = false;
                LOG("Reach camera cleanup: disable failed for %p (%d)",
                    target, static_cast<int>(status));
            }
        }
        if (!disabledAll || !WaitForReachDetourQuiescence())
            return false;

        bool removedAll = true;
        if (g_reachCamera.rainRenderTarget)
        {
            const MH_STATUS status =
                MH_RemoveHook(g_reachCamera.rainRenderTarget);
            if (status == MH_OK || status == MH_ERROR_NOT_CREATED)
            {
                g_reachCamera.rainRenderTarget = nullptr;
                g_reachOrigRainRender = nullptr;
            }
            else
            {
                removedAll = false;
                LOG("Reach rain cleanup: remove failed for %p (%d)",
                    g_reachCamera.rainRenderTarget, static_cast<int>(status));
            }
        }
        if (g_reachCamera.effectLocationTarget)
        {
            const MH_STATUS status =
                MH_RemoveHook(g_reachCamera.effectLocationTarget);
            if (status == MH_OK || status == MH_ERROR_NOT_CREATED)
            {
                g_reachCamera.effectLocationTarget = nullptr;
                g_reachOrigEffectLocation = nullptr;
                g_reachEffectFpMarkerQuery = nullptr;
            }
            else
            {
                removedAll = false;
                LOG("Reach muzzle cleanup: remove failed for %p (%d)",
                    g_reachCamera.effectLocationTarget,
                    static_cast<int>(status));
            }
        }
        if (g_reachCamera.observerCameraTarget)
        {
            const MH_STATUS status =
                MH_RemoveHook(g_reachCamera.observerCameraTarget);
            if (status == MH_OK || status == MH_ERROR_NOT_CREATED)
            {
                g_reachCamera.observerCameraTarget = nullptr;
                g_reachOrigObserverCamera = nullptr;
            }
            else
            {
                removedAll = false;
                LOG("Reach head-lock cleanup: remove failed for %p (%d)",
                    g_reachCamera.observerCameraTarget,
                    static_cast<int>(status));
            }
        }
        if (g_reachCamera.hudDrawWidgetTarget)
        {
            const MH_STATUS status =
                MH_RemoveHook(g_reachCamera.hudDrawWidgetTarget);
            if (status == MH_OK || status == MH_ERROR_NOT_CREATED)
            {
                g_reachCamera.hudDrawWidgetTarget = nullptr;
                g_reachOrigHudDrawWidget = nullptr;
            }
            else
            {
                removedAll = false;
                LOG("Reach crosshair cleanup: remove failed for %p (%d)",
                    g_reachCamera.hudDrawWidgetTarget,
                    static_cast<int>(status));
            }
        }
        if (g_reachCamera.ssaoTarget)
        {
            const MH_STATUS status =
                MH_RemoveHook(g_reachCamera.ssaoTarget);
            if (status == MH_OK || status == MH_ERROR_NOT_CREATED)
            {
                g_reachCamera.ssaoIsolationActive.store(
                    false, std::memory_order_release);
                g_reachCamera.ssaoTarget = nullptr;
                g_reachOrigRenderSsao = nullptr;
            }
            else
            {
                removedAll = false;
                LOG("Reach SSAO cleanup: remove failed for %p (%d)",
                    g_reachCamera.ssaoTarget, static_cast<int>(status));
            }
        }
        if (g_reachCamera.fpCameraTarget)
        {
            const MH_STATUS status =
                MH_RemoveHook(g_reachCamera.fpCameraTarget);
            if (status == MH_OK || status == MH_ERROR_NOT_CREATED)
            {
                g_reachCamera.fpCameraTarget = nullptr;
                g_reachOrigFpCameraRebuild = nullptr;
                g_reachFpCameraUpload = nullptr;
            }
            else
            {
                removedAll = false;
                LOG("Reach FP camera cleanup: remove failed for %p (%d)",
                    g_reachCamera.fpCameraTarget,
                    static_cast<int>(status));
            }
        }
        if (g_reachCamera.fpPaletteTarget)
        {
            const MH_STATUS status =
                MH_RemoveHook(g_reachCamera.fpPaletteTarget);
            if (status == MH_OK || status == MH_ERROR_NOT_CREATED)
            {
                g_reachCamera.fpPaletteTarget = nullptr;
                g_reachOrigFpPalette = nullptr;
            }
            else
            {
                removedAll = false;
                LOG("Reach FP palette cleanup: remove failed for %p (%d)",
                    g_reachCamera.fpPaletteTarget,
                    static_cast<int>(status));
            }
        }
        if (g_reachCamera.fpInterpolateTarget)
        {
            const MH_STATUS status =
                MH_RemoveHook(g_reachCamera.fpInterpolateTarget);
            if (status == MH_OK || status == MH_ERROR_NOT_CREATED)
            {
                g_reachCamera.fpInterpolateTarget = nullptr;
                g_reachOrigFpInterpolate = nullptr;
            }
            else
            {
                removedAll = false;
                LOG("Reach FP interpolation cleanup: remove failed for %p (%d)",
                    g_reachCamera.fpInterpolateTarget,
                    static_cast<int>(status));
            }
        }
        if (g_reachCamera.innerTarget)
        {
            const MH_STATUS status =
                MH_RemoveHook(g_reachCamera.innerTarget);
            if (status == MH_OK || status == MH_ERROR_NOT_CREATED)
            {
                g_reachCamera.innerTarget = nullptr;
                g_reachOrigPlayerViewRender = nullptr;
            }
            else
            {
                removedAll = false;
                LOG("Reach camera cleanup: inner remove failed for %p (%d)",
                    g_reachCamera.innerTarget, static_cast<int>(status));
            }
        }
        if (g_reachCamera.outerTarget)
        {
            const MH_STATUS status =
                MH_RemoveHook(g_reachCamera.outerTarget);
            if (status == MH_OK || status == MH_ERROR_NOT_CREATED)
            {
                g_reachCamera.outerTarget = nullptr;
                g_reachOrigMainRenderView = nullptr;
            }
            else
            {
                removedAll = false;
                LOG("Reach camera cleanup: outer remove failed for %p (%d)",
                    g_reachCamera.outerTarget, static_cast<int>(status));
            }
        }
        return removedAll;
    }

    bool RemoveReachCameraCore()
    {
        g_reachCamera.teardownRequested.store(
            true, std::memory_order_release);
        g_reachCamera.armed.store(false, std::memory_order_release);
        for (int eye = 0; eye < 2; ++eye)
        {
            g_reachRenderFovSerial[eye].store(0, std::memory_order_release);
            g_reachRenderHalfFovX[eye].store(0.0f, std::memory_order_relaxed);
            g_reachRenderHalfFovY[eye].store(0.0f, std::memory_order_relaxed);
        }
        g_reachFpCameraUploadStatus.preparedSerial.store(
            0, std::memory_order_release);
        g_reachFpCameraUploadStatus.generation.store(
            0, std::memory_order_relaxed);
        g_reachFpCameraUploadStatus.eyeMask.store(
            0, std::memory_order_relaxed);
        g_reachFpCameraLoggedGeneration.store(
            0, std::memory_order_release);
        g_aimSeen.store(false, std::memory_order_release);
        g_camValid.store(false, std::memory_order_release);
        g_baseCamValid.store(false, std::memory_order_release);
        if (!DisableAndRemoveReachHooks())
            return false;

        // Put the engine's third-person effect branch back before anything
        // else can unload the module. Optional, so a failure is logged rather
        // than blocking teardown.
        if (!RestoreReachThirdPersonEffectSuppression())
            LOG("Reach muzzle: third-person effect branch could not be restored");
        ReachMuzzleRetargetRestore();

        if (!RestoreReachNativeWeaponIkBypass())
        {
            LOG("Reach camera cleanup: native weapon-IK disable could not be "
                "restored; retaining the exact title module for retry");
            return false;
        }

        // The eye hook normally restores this byte in its __finally. Repeat the
        // restoration after hook quiescence so even an exceptional last render
        // cannot leave the title-wide development control changed.
        if (!RestoreReachLightmapShadowsControl())
        {
            LOG("Reach world-shadow cleanup: render_lightmap_shadows could not "
                "be restored after hook quiescence; retaining the exact title "
                "module and ownership record for retry");
            return false;
        }

        const bool restoreReachBlur = g_reachCamera.motionBlurSuppressed;
        if (!ReachRestoreMotionBlurValues())
        {
            LOG("Reach camera cleanup: could not restore title-native "
                "motion-blur values; retained state will retry");
            return false;
        }
        if (restoreReachBlur)
            LOG("Reach comfort: stock motion-blur values restored during teardown");

        // A callback admitted before armed was cleared could have published
        // its final copied eye while teardown waited. Quiescence makes this
        // second invalidation definitive for the retired title generation.
        for (int eye = 0; eye < 2; ++eye)
        {
            g_reachRenderFovSerial[eye].store(0, std::memory_order_release);
            g_reachRenderHalfFovX[eye].store(0.0f, std::memory_order_relaxed);
            g_reachRenderHalfFovY[eye].store(0.0f, std::memory_order_relaxed);
        }
        g_aimSeen.store(false, std::memory_order_release);
        g_camValid.store(false, std::memory_order_release);
        g_baseCamValid.store(false, std::memory_order_release);

        HMODULE moduleReference = g_reachCamera.moduleReference;
        g_reachCamera.innerTarget = nullptr;
        g_reachCamera.outerTarget = nullptr;
        g_reachCamera.fpInterpolateTarget = nullptr;
        g_reachCamera.fpPaletteTarget = nullptr;
        g_reachCamera.fpCameraTarget = nullptr;
        g_reachCamera.ssaoTarget = nullptr;
        g_reachCamera.ssaoIsolationActive.store(
            false, std::memory_order_release);
        g_reachCamera.hudDrawWidgetTarget = nullptr;
        g_reachCamera.observerCameraTarget = nullptr;
        g_reachCamera.effectLocationTarget = nullptr;
        g_reachCamera.rainRenderTarget = nullptr;
        g_reachCamera.moduleReference = nullptr;
        g_reachCamera.base = 0;
        g_reachCamera.size = 0;
        g_reachCamera.generation = 0;
        g_reachCamera.installedAtMs = 0;
        g_reachCamera.motionBlurVars[0] = {};
        g_reachCamera.motionBlurVars[1] = {};
        g_reachCamera.motionBlurResolved = false;
        g_reachCamera.motionBlurSuppressed = false;
        g_reachCamera.patchyFogFlags = nullptr;
        g_reachCamera.lightmapShadowsEnabled = nullptr;
        g_reachLightmapShadowOutstandingValue.store(
            0, std::memory_order_relaxed);
        g_reachLightmapShadowRestoreOutstanding.store(
            false, std::memory_order_release);
        g_reachCamera.nativeWeaponIkDisable = nullptr;
        g_reachCamera.nativeWeaponIkDisableOriginal = 0;
        g_reachCamera.nativeWeaponIkBypassActive = false;
        g_reachOrigPlayerViewRender = nullptr;
        g_reachOrigMainRenderView = nullptr;
        g_reachOrigFpInterpolate = nullptr;
        g_reachOrigFpPalette = nullptr;
        g_reachOrigFpCameraRebuild = nullptr;
        g_reachFpCameraUpload = nullptr;
        g_reachOrigRenderSsao = nullptr;
        g_reachOrigHudDrawWidget = nullptr;
        g_reachFpStatus.key.store(0,std::memory_order_release);
        g_reachFpLoggedStatusKey.store(0,std::memory_order_release);
        g_reachFpCameraUploadStatus.preparedSerial.store(
            0, std::memory_order_release);
        g_reachFpCameraUploadStatus.generation.store(
            0, std::memory_order_relaxed);
        g_reachFpCameraUploadStatus.eyeMask.store(
            0, std::memory_order_relaxed);
        g_reachFpCameraLoggedGeneration.store(
            0, std::memory_order_release);
        g_reachHelpers = {};
        if (moduleReference)
            FreeLibrary(moduleReference);
        g_reachCamera.teardownRequested.store(
            false, std::memory_order_release);
        g_reachCamera.installed.store(false, std::memory_order_release);
        LOG("Reach camera core removed; stock Reach owns the title");
        return true;
    }

    bool ReachColdExecutableAddress(uintptr_t address)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!address || !VirtualQuery(reinterpret_cast<const void*>(address),
                                      &info,sizeof(info)) ||
            info.State!=MEM_COMMIT || (info.Protect&PAGE_GUARD)) return false;
        const DWORD protection=info.Protect&0xFFu;
        return protection==PAGE_EXECUTE || protection==PAGE_EXECUTE_READ ||
            protection==PAGE_EXECUTE_READWRITE ||
            protection==PAGE_EXECUTE_WRITECOPY;
    }

    bool ReachColdExactSignatureAt(uintptr_t base, size_t size,
                                   uintptr_t expectedRva,
                                   const char* pattern)
    {
        if (!base || !pattern || expectedRva>=size) return false;
        const uintptr_t first=sig::Find(base,size,pattern);
        if (first!=base+expectedRva || !ReachColdExecutableAddress(first))
            return false;
        const uintptr_t next=first+1;
        const uintptr_t end=base+size;
        return next>=end || sig::Find(next,static_cast<size_t>(end-next),pattern)==0;
    }

    bool ResolveReachLightmapShadowsControl(
        uintptr_t base, size_t size, uint8_t*& slot, uint8_t& original)
    {
        static constexpr char kExpectedName[] =
            "render_lightmap_shadows";
        slot = nullptr;
        original = 0;
        const auto fits = [size](uintptr_t rva, size_t bytes)
        {
            return rva < size && bytes <= size - rva;
        };
        if (!fits(kReachLightmapShadowsNameRva, sizeof(kExpectedName)) ||
            !fits(kReachLightmapShadowsEntryRva, 24) ||
            !fits(kReachLightmapShadowsValueRva, 1) ||
            !fits(kReachLightmapShadowsPlayerCallRva, 5) ||
            !fits(kReachLightmapShadowsCompareRva, 7) ||
            !fits(kReachLightmapShadowsRenderCallRva, 5))
        {
            return false;
        }

        const auto* entry = reinterpret_cast<const uint8_t*>(
            base + kReachLightmapShadowsEntryRva);
        uintptr_t entryName = 0;
        uint64_t entryType = 0;
        uintptr_t entryValue = 0;
        memcpy(&entryName, entry, sizeof(entryName));
        memcpy(&entryType, entry + 8, sizeof(entryType));
        memcpy(&entryValue, entry + 16, sizeof(entryValue));
        if (memcmp(
                reinterpret_cast<const void*>(
                    base + kReachLightmapShadowsNameRva),
                kExpectedName, sizeof(kExpectedName)) != 0 ||
            entryName != base + kReachLightmapShadowsNameRva ||
            entryType != kReachDebugBooleanType ||
            entryValue != base + kReachLightmapShadowsValueRva)
        {
            return false;
        }

        const auto* compare = reinterpret_cast<const uint8_t*>(
            base + kReachLightmapShadowsCompareRva);
        if (compare[0] != 0x80 || compare[1] != 0x3D ||
            compare[6] != 0x00)
        {
            return false;
        }
        int32_t displacement = 0;
        memcpy(&displacement, compare + 2, sizeof(displacement));
        const uintptr_t compareTarget = static_cast<uintptr_t>(
            static_cast<intptr_t>(base + kReachLightmapShadowsCompareRva + 7) +
            displacement);
        if (compareTarget != base + kReachLightmapShadowsValueRva ||
            !ReachVerifyRel32Call(
                base, kReachLightmapShadowsPlayerCallRva,
                kReachLightmapShadowsWrapperRva) ||
            !ReachVerifyRel32Call(
                base, kReachLightmapShadowsRenderCallRva,
                kReachLightmapShadowsRenderRva) ||
            !ReachColdExecutableAddress(
                base + kReachLightmapShadowsRenderRva))
        {
            return false;
        }

        auto* const resolved = reinterpret_cast<uint8_t*>(entryValue);
        if (!SafeReadByte(resolved, &original) || original > 1)
            return false;
        slot = resolved;
        return true;
    }

    bool ResolveReachNativeWeaponIkControl(
        uintptr_t base, size_t size, uint8_t*& slot, uint8_t& original)
    {
        slot = nullptr;
        original = 0;
        static constexpr char kDecisionAob[] =
            "41 0F B7 86 2C 53 00 00 66 85 C0 0F 8E 52 02 00 00 "
            "38 1D DC 3A B8 04 0F 85 46 02 00 00 48 8B 15 C6 88 "
            "99 00 0F BF C8";
        if (!ReachColdExactSignatureAt(
                base, size, kReachFpWeaponIkDecisionPreludeRva,
                kDecisionAob) ||
            kReachFpWeaponIkDisableEntryRva >= size ||
            24 > size - kReachFpWeaponIkDisableEntryRva)
        {
            return false;
        }

        const auto* entry = reinterpret_cast<const uint8_t*>(
            base + kReachFpWeaponIkDisableEntryRva);
        uintptr_t entryName = 0;
        uint64_t entryType = 0;
        memcpy(&entryName, entry, sizeof(entryName));
        memcpy(&entryType, entry + 8, sizeof(entryType));
        if (entryName != base + kReachFpWeaponIkDisableNameRva ||
            entryType != kReachDebugBooleanType)
        {
            return false;
        }

        const auto* compare = reinterpret_cast<const uint8_t*>(
            base + kReachFpWeaponIkDisableCompareRva);
        const auto* branch = reinterpret_cast<const uint8_t*>(
            base + kReachFpWeaponIkDisableBranchRva);
        if (compare[0] != 0x38 || compare[1] != 0x1D ||
            branch[0] != 0x0F || branch[1] != 0x85)
        {
            return false;
        }
        int32_t flagDisplacement = 0;
        int32_t branchDisplacement = 0;
        memcpy(&flagDisplacement, compare + 2, sizeof(flagDisplacement));
        memcpy(&branchDisplacement, branch + 2, sizeof(branchDisplacement));
        const uintptr_t flagTarget = static_cast<uintptr_t>(
            static_cast<intptr_t>(base + kReachFpWeaponIkDisableCompareRva + 6) +
            flagDisplacement);
        const uintptr_t disabledTarget = static_cast<uintptr_t>(
            static_cast<intptr_t>(base + kReachFpWeaponIkDisableBranchRva + 6) +
            branchDisplacement);
        // HREK publishes a value pointer in its development debug descriptor;
        // retail leaves that descriptor field unpublished. The shipping
        // consumer instruction is the authoritative binding: decode its exact
        // RIP-relative byte and require the pinned Reach-specific target.
        uint8_t* const resolved = reinterpret_cast<uint8_t*>(flagTarget);
        if (flagTarget != base + kReachFpWeaponIkDisableValueRva ||
            disabledTarget != base + kReachFpWeaponIkDisabledEpilogueRva ||
            !ReachColdExecutableAddress(disabledTarget) ||
            !SafeReadByte(resolved, &original) || original > 1)
        {
            return false;
        }
        slot = resolved;
        return true;
    }

    // ---- selective muzzle retarget: put the odd FP particle on the gun -----
    // See kReachEffectHandleTableRva in reach_render_logic.h. The hot hook
    // captures the definition index of the player's own weapon effects; this
    // worker decodes the loaded tag through the engine's own handle/pool
    // arithmetic, finds the single first-person system whose location differs
    // from its siblings, and writes the siblings' location index over it. The
    // siblings provably track the controller-held gun.
    const char* kReachEffectDecodeSig =
        "4C 8B 2D ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 41 BC 48 00 00 00";
    uintptr_t g_reachMuzzleHandleTable = 0;   // decoded from the signature
    uintptr_t g_reachMuzzlePoolTable = 0;
    bool g_reachMuzzleDecodeResolved = false;
    struct ReachMuzzlePatch { uintptr_t address; uint16_t original; };
    ReachMuzzlePatch g_reachMuzzlePatches[32]{};
    size_t g_reachMuzzlePatchCount = 0;
    uint32_t g_reachMuzzleAttemptedDefs[16]{};
    size_t g_reachMuzzleAttemptedCount = 0;
    std::atomic<uint32_t> g_reachMuzzleRetargetDone{0};

    bool ReachReadU32(uintptr_t address, uint32_t* out)
    {
        __try
        {
            *out = *reinterpret_cast<const uint32_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    bool ReachReadU16(uintptr_t address, uint16_t* out)
    {
        __try
        {
            *out = *reinterpret_cast<const uint16_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
    bool ReachWriteU16(uintptr_t address, uint16_t value)
    {
        __try
        {
            *reinterpret_cast<uint16_t*>(address) = value;
            return *reinterpret_cast<const uint16_t*>(address) == value;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Decode pool[handle >> 28] + handle*4, the engine's own arithmetic.
    bool ReachDecodePoolAddress(uint32_t handle, uintptr_t* out)
    {
        uintptr_t poolBase = 0;
        __try
        {
            poolBase = *reinterpret_cast<const uintptr_t*>(
                g_reachMuzzlePoolTable +
                static_cast<uintptr_t>(handle >> 28) * 8);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (!poolBase)
            return false;
        *out = poolBase + static_cast<uintptr_t>(handle) * 4;
        return true;
    }

    void ReachMuzzleRetargetTick(uintptr_t base, size_t size,
                                 uint32_t generation, bool soleReachTitle)
    {
        static uint32_t attemptedGeneration = 0;
        if (!soleReachTitle || !base || !generation)
        {
            g_reachMuzzlePatchCount = 0;
            g_reachMuzzleAttemptedCount = 0;
            g_reachMuzzleDecodeResolved = false;
            g_reachMuzzleRetargetDone.store(0, std::memory_order_relaxed);
            g_reachMuzzleCapturedDefIndex.store(
                0xFFFFFFFFu, std::memory_order_relaxed);
            attemptedGeneration = 0;
            return;
        }
        if (attemptedGeneration != generation)
        {
            attemptedGeneration = generation;
            g_reachMuzzlePatchCount = 0;
            g_reachMuzzleAttemptedCount = 0;
            g_reachMuzzleDecodeResolved = false;
            g_reachMuzzleRetargetDone.store(0, std::memory_order_relaxed);
        }
        if (!g_reachMuzzleDecodeResolved)
        {
            const uintptr_t hit =
                sig::Find(base, size, kReachEffectDecodeSig);
            if (!hit || sig::Find(hit + 1, base + size - hit - 1,
                                  kReachEffectDecodeSig))
            {
                return;   // silent; retried next tick, logged via REACHFX
            }
            const uintptr_t tableSlot = sig::RipTarget(hit + 3, hit + 7);
            const uintptr_t poolTable = sig::RipTarget(hit + 10, hit + 14);
            if (tableSlot < base || tableSlot >= base + size ||
                poolTable < base || poolTable >= base + size)
            {
                return;
            }
            __try
            {
                g_reachMuzzleHandleTable =
                    *reinterpret_cast<const uintptr_t*>(tableSlot);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return;
            }
            if (!g_reachMuzzleHandleTable)
                return;
            g_reachMuzzlePoolTable = poolTable;
            g_reachMuzzleDecodeResolved = true;
            LOG("Reach muzzle: tag decode resolved (handle table slot "
                "haloreach.dll+0x%llX, pool haloreach.dll+0x%llX)",
                (unsigned long long)(tableSlot - base),
                (unsigned long long)(poolTable - base));
        }
        const uint32_t defIndex = g_reachMuzzleCapturedDefIndex.load(
            std::memory_order_relaxed);
        if (defIndex == 0xFFFFFFFFu)
            return;
        // One attempt per definition; the player swaps weapons, so several
        // definitions accumulate over a session (six weapons qualify).
        for (size_t i = 0; i < g_reachMuzzleAttemptedCount; ++i)
            if (g_reachMuzzleAttemptedDefs[i] == defIndex)
                return;
        if (g_reachMuzzleAttemptedCount >=
            _countof(g_reachMuzzleAttemptedDefs))
        {
            return;
        }
        g_reachMuzzleAttemptedDefs[g_reachMuzzleAttemptedCount++] = defIndex;

        uint32_t handle = 0;
        if (!ReachReadU32(g_reachMuzzleHandleTable +
                              static_cast<uintptr_t>(defIndex & 0xFFFFu) * 8 +
                              4,
                          &handle) ||
            handle == 0 || handle == 0xFFFFFFFFu)
        {
            return;
        }
        uintptr_t defBase = 0;
        if (!ReachDecodePoolAddress(handle, &defBase))
            return;
        uint32_t eventsHandle = 0;
        if (!ReachReadU32(defBase + kReachEffectDefEventsBlockOffset,
                          &eventsHandle) ||
            !eventsHandle || eventsHandle == 0xFFFFFFFFu)
        {
            return;
        }
        uintptr_t eventsBase = 0;
        if (!ReachDecodePoolAddress(eventsHandle, &eventsBase))
            return;

        for (size_t eventIndex = 0; eventIndex < 8; ++eventIndex)
        {
            const uintptr_t eventElement =
                eventsBase + eventIndex * kReachEffectEventStride;
            uint32_t psysHandle = 0;
            if (!ReachReadU32(eventElement + kReachEffectEventPsysBlockOffset,
                              &psysHandle) ||
                !psysHandle || psysHandle == 0xFFFFFFFFu)
            {
                continue;
            }
            uintptr_t psysBase = 0;
            if (!ReachDecodePoolAddress(psysHandle, &psysBase))
                continue;
            unsigned short modes[kReachEffectMaxWalk]{};
            unsigned short locations[kReachEffectMaxWalk]{};
            size_t count = 0;
            for (; count < kReachEffectMaxWalk; ++count)
            {
                uint16_t mode = 0;
                uint16_t location = 0;
                if (!ReachReadU16(psysBase +
                                      count * kReachEffectPsysStride +
                                      kReachEffectPsysCameraModeOffset,
                                  &mode) ||
                    !ReachReadU16(psysBase +
                                      count * kReachEffectPsysStride +
                                      kReachEffectPsysLocationOffset,
                                  &location) ||
                    mode > 3 || location > 15)
                {
                    break;
                }
                modes[count] = mode;
                locations[count] = location;
            }
            const ReachMuzzleRetargetPlan plan =
                ReachDecideMuzzleRetarget(modes, locations, count);
            if (!plan.count)
                continue;
            unsigned applied = 0;
            for (int e = 0; e < plan.count; ++e)
            {
                if (g_reachMuzzlePatchCount >=
                    _countof(g_reachMuzzlePatches))
                {
                    break;
                }
                const int element = plan.elements[e];
                const uintptr_t target = psysBase +
                    static_cast<size_t>(element) * kReachEffectPsysStride +
                    kReachEffectPsysLocationOffset;
                const uint16_t original = locations[element];
                if (!ReachWriteU16(target, plan.newLocation))
                    continue;
                g_reachMuzzlePatches[g_reachMuzzlePatchCount++] =
                    {target, original};
                ++applied;
            }
            if (applied)
            {
                g_reachMuzzleRetargetDone.fetch_add(
                    applied, std::memory_order_relaxed);
                LOG("Reach muzzle: RETARGETED %u first-person particle "
                    "system(s) of definition %u event %zu onto location %u "
                    "(the marker their siblings track the gun with)",
                    applied, defIndex & 0xFFFFu, eventIndex,
                    plan.newLocation);
            }
            // keep scanning further events of this definition
        }
    }

    void ReachMuzzleRetargetRestore()
    {
        for (size_t i = 0; i < g_reachMuzzlePatchCount; ++i)
            ReachWriteU16(g_reachMuzzlePatches[i].address,
                          g_reachMuzzlePatches[i].original);
        g_reachMuzzlePatchCount = 0;
        g_reachMuzzleAttemptedCount = 0;
        g_reachMuzzleRetargetDone.store(0, std::memory_order_relaxed);
        g_reachMuzzleCapturedDefIndex.store(
            0xFFFFFFFFu, std::memory_order_relaxed);
    }

    // ---- suppress the third-person muzzle flash on the player's own body ----
    // See kReachThirdPersonEffectDenyRva for the derivation. Reach's effect
    // render pass allows a "third person only" particle system whenever the
    // effect is not flagged as the player's own first-person weapon - and the
    // player's own WORLD weapon is not flagged, because flat Reach never shows
    // it. VR does, so it renders at the body.
    //
    // This takes the engine's own deny path for camera-mode-2 systems. Applied
    // when the Reach core arms and restored on teardown, exactly like the
    // native weapon-IK bypass above.
    const char* kReachThirdPersonEffectSig =
        "F9 41 B2 01 85 C9 74 1E 83 E9 01 74 14 83 F9 01 75 17 "
        "45 85 F6 75 05 45 85 C0 74 0D 45 32 D2";
    uintptr_t g_reachThirdPersonEffectPatchSite = 0;
    bool g_reachThirdPersonEffectPatched = false;

    bool ReachWriteCode(uintptr_t address, const unsigned char* bytes,
                        size_t count)
    {
        DWORD previous = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(address), count,
                            PAGE_EXECUTE_READWRITE, &previous))
        {
            return false;
        }
        bool ok = true;
        __try
        {
            memcpy(reinterpret_cast<void*>(address), bytes, count);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        DWORD restored = 0;
        VirtualProtect(reinterpret_cast<void*>(address), count, previous,
                       &restored);
        if (ok)
            FlushInstructionCache(GetCurrentProcess(),
                                  reinterpret_cast<void*>(address), count);
        return ok;
    }

    bool ApplyReachThirdPersonEffectSuppression(uintptr_t base, size_t size)
    {
        g_reachThirdPersonEffectPatchSite = 0;
        g_reachThirdPersonEffectPatched = false;
        const uintptr_t hit = sig::Find(base, size, kReachThirdPersonEffectSig);
        if (!hit || sig::Find(hit + 1, base + size - hit - 1,
                              kReachThirdPersonEffectSig))
        {
            LOG("Reach muzzle: third-person effect signature %s; the "
                "body-anchored muzzle flash stays, nothing else affected",
                hit ? "ambiguous" : "missing");
            return false;
        }
        const uintptr_t site = hit + kReachFirstPersonEffectPatchOffset;
        if (memcmp(reinterpret_cast<const void*>(site),
                   kReachFirstPersonEffectOriginal,
                   kReachThirdPersonEffectPatchBytes) != 0)
        {
            LOG("Reach muzzle: third-person deny site does not hold the "
                "expected bytes; leaving it alone");
            return false;
        }
        if (!ReachWriteCode(site, kReachFirstPersonEffectPatch,
                            kReachThirdPersonEffectPatchBytes))
        {
            LOG("Reach muzzle: could not write the third-person suppression");
            return false;
        }
        g_reachThirdPersonEffectPatchSite = site;
        g_reachThirdPersonEffectPatched = true;
        LOG("Reach muzzle: FIRST-person-only particle systems suppressed at "
            "haloreach.dll+0x%llX; the camera-space muzzle flash stops "
            "emitting. Mode 0 and mode 2 systems are untouched",
            static_cast<unsigned long long>(site - base));
        return true;
    }

    bool RestoreReachThirdPersonEffectSuppression()
    {
        if (!g_reachThirdPersonEffectPatched ||
            !g_reachThirdPersonEffectPatchSite)
        {
            return true;
        }
        const bool ok = ReachWriteCode(
            g_reachThirdPersonEffectPatchSite,
            kReachFirstPersonEffectOriginal,
            kReachThirdPersonEffectPatchBytes);
        g_reachThirdPersonEffectPatched = false;
        g_reachThirdPersonEffectPatchSite = 0;
        return ok;
    }

    bool ApplyReachNativeWeaponIkBypass()
    {
        uint8_t* const slot = g_reachCamera.nativeWeaponIkDisable;
        uint8_t current = 0;
        if (!slot || !SafeReadByte(slot, &current) ||
            current != g_reachCamera.nativeWeaponIkDisableOriginal ||
            !SafeWriteByte(slot, 1))
        {
            return false;
        }
        g_reachCamera.nativeWeaponIkBypassActive = true;
        if (!SafeReadByte(slot, &current) || current != 1)
            return false;
        LOG("Reach VRIK: native support-hand weapon IK disabled through the "
            "title's exact debug_animation_fp_weapon_ik_disable control; "
            "shared controller solver owns both arms");
        return true;
    }

    bool RestoreReachNativeWeaponIkBypass()
    {
        if (!g_reachCamera.nativeWeaponIkBypassActive)
            return true;
        uint8_t* const slot = g_reachCamera.nativeWeaponIkDisable;
        uint8_t current = 0;
        if (!slot || !SafeReadByte(slot, &current) || current > 1 ||
            !SafeWriteByte(
                slot, g_reachCamera.nativeWeaponIkDisableOriginal) ||
            !SafeReadByte(slot, &current) ||
            current != g_reachCamera.nativeWeaponIkDisableOriginal)
        {
            return false;
        }
        g_reachCamera.nativeWeaponIkBypassActive = false;
        return true;
    }

    bool RestoreReachLightmapShadowsControl()
    {
        if (!g_reachLightmapShadowRestoreOutstanding.load(
                std::memory_order_acquire))
        {
            return true;
        }
        uint8_t* const slot = g_reachCamera.lightmapShadowsEnabled;
        const uint8_t original =
            g_reachLightmapShadowOutstandingValue.load(
                std::memory_order_relaxed);
        uint8_t current = 0;
        if (!slot || original > 1 || !SafeReadByte(slot, &current) ||
            current > 1 ||
            !SafeWriteByte(slot, original) ||
            !SafeReadByte(slot, &current) || current != original)
        {
            return false;
        }
        g_reachLightmapShadowOutstandingValue.store(
            0, std::memory_order_relaxed);
        g_reachLightmapShadowRestoreOutstanding.store(
            false, std::memory_order_release);
        return true;
    }

    struct ReachHrekChudMatchSet
    {
        uintptr_t candidate = 0;
        size_t candidateCount = 0;
        bool malformed = false;
    };

    size_t CountReachHrekChudBytes(
        std::span<const uint8_t> body, std::span<const uint8_t> bytes,
        size_t limit) noexcept
    {
        size_t count = 0;
        const size_t endOffset = body.size() < limit ? body.size() : limit;
        for (size_t offset = 0; offset < endOffset; ++offset)
        {
            if (bytes.size() <= body.size() - offset &&
                std::memcmp(
                    body.data() + offset, bytes.data(), bytes.size()) == 0)
            {
                ++count;
            }
        }
        return count;
    }

    bool ReachHrekChudHasDirectSelfEntry(
        std::span<const uint8_t> body, uintptr_t functionBegin) noexcept
    {
        for (size_t offset = 0; offset + 5 <= body.size(); ++offset)
        {
            if (body[offset] != 0xE8 && body[offset] != 0xE9)
                continue;
            int32_t displacement = 0;
            std::memcpy(
                &displacement, body.data() + offset + 1,
                sizeof(displacement));
            const uintptr_t callTarget = static_cast<uintptr_t>(
                static_cast<intptr_t>(functionBegin + offset + 5) +
                static_cast<intptr_t>(displacement));
            if (callTarget == functionBegin)
                return true;
        }
        return false;
    }

    void InspectReachHrekChudAbiVariant(
        uintptr_t base, size_t size, const char* classReadAob,
        std::span<const uint8_t> descriptorMove,
        std::span<const uint8_t> classRead,
        size_t expectedClassReadCount,
        std::span<const uint8_t> fifthArgumentLoad,
        ReachHrekChudMatchSet& matches)
    {
        const uintptr_t end = base + size;
        uintptr_t search = base;
        while (!matches.malformed && search < end)
        {
            const uintptr_t hit = sig::Find(
                search, static_cast<size_t>(end - search), classReadAob);
            if (!hit)
                break;
            search = hit + 1;
            if (!ReachColdExecutableAddress(hit))
            {
                matches.malformed = true;
                break;
            }

            DWORD64 imageBase = 0;
            const PRUNTIME_FUNCTION runtimeFunction = RtlLookupFunctionEntry(
                static_cast<DWORD64>(hit), &imageBase, nullptr);
            if (!runtimeFunction || imageBase != base ||
                runtimeFunction->EndAddress <= runtimeFunction->BeginAddress)
            {
                matches.malformed = true;
                break;
            }
            const uintptr_t functionBegin =
                static_cast<uintptr_t>(imageBase) +
                runtimeFunction->BeginAddress;
            const uintptr_t functionEnd =
                static_cast<uintptr_t>(imageBase) +
                runtimeFunction->EndAddress;
            if (functionBegin < base || functionBegin > hit ||
                functionEnd > end || functionEnd <= hit ||
                functionEnd - functionBegin < 0x200 ||
                functionEnd - functionBegin > 0x800)
            {
                matches.malformed = true;
                break;
            }

            const std::span<const uint8_t> body(
                reinterpret_cast<const uint8_t*>(functionBegin),
                static_cast<size_t>(functionEnd - functionBegin));
            const size_t classReadOffset =
                static_cast<size_t>(hit - functionBegin);
            const bool exactClassFlow =
                classReadOffset + classRead.size() <= body.size() &&
                std::memcmp(
                    body.data() + classReadOffset,
                    classRead.data(), classRead.size()) == 0;
            if (!exactClassFlow ||
                CountReachHrekChudBytes(body, descriptorMove, 0x100) != 1 ||
                CountReachHrekChudBytes(body, fifthArgumentLoad, 0x100) != 1 ||
                CountReachHrekChudBytes(body, classRead, body.size()) !=
                    expectedClassReadCount ||
                ReachHrekChudHasDirectSelfEntry(body, functionBegin))
            {
                continue;
            }

            if (!matches.candidate)
            {
                matches.candidate = functionBegin;
                matches.candidateCount = 1;
            }
            else if (matches.candidate != functionBegin)
            {
                ++matches.candidateCount;
            }
        }
    }

    bool ResolveReachHrekChudDrawWidget(
        uintptr_t base, size_t size, void*& target)
    {
        // Reach uses the same authored-widget capture transaction as Halo 3 and
        // ODST. HREK establishes the native five-argument widget ABI; this
        // adapter locates exactly one MCC implementation by that ABI rather
        // than requiring the separately linked MCC binary to duplicate an HREK
        // tool executable's complete prologue and frame size.
        static constexpr char kTagTestClassReadAob[] =
            "41 0F BE 55 04";
        static constexpr char kTagPlayClassReadAob[] =
            "41 0F BE 56 04";
        static constexpr char kSapienPlayClassReadAob[] =
            "41 0F BE 57 04";
        static constexpr std::array<uint8_t, 3> kTagTestDescriptorMove{
            0x4C, 0x8B, 0xEA};
        static constexpr std::array<uint8_t, 3> kTagDescriptorMove{
            0x4C, 0x8B, 0xF2};
        static constexpr std::array<uint8_t, 3> kSapienDescriptorMove{
            0x4C, 0x8B, 0xFA};
        static constexpr std::array<uint8_t, 4> kTagTestFifthArgumentLoad{
            0x48, 0x8B, 0x7D, 0x7F};
        static constexpr std::array<uint8_t, 4> kFifthArgumentLoad{
            0x4C, 0x8B, 0x4D, 0x7F};
        static constexpr std::array<uint8_t, 5> kTagTestClassRead{
            0x41, 0x0F, 0xBE, 0x55, 0x04};
        static constexpr std::array<uint8_t, 5> kTagClassRead{
            0x41, 0x0F, 0xBE, 0x56, 0x04};
        static constexpr std::array<uint8_t, 5> kSapienClassRead{
            0x41, 0x0F, 0xBE, 0x57, 0x04};

        target = nullptr;
        ReachHrekChudMatchSet matches{};
        InspectReachHrekChudAbiVariant(
            base, size, kTagTestClassReadAob, kTagTestDescriptorMove,
            kTagTestClassRead, 4, kTagTestFifthArgumentLoad, matches);
        InspectReachHrekChudAbiVariant(
            base, size, kTagPlayClassReadAob, kTagDescriptorMove,
            kTagClassRead, 1, kFifthArgumentLoad, matches);
        InspectReachHrekChudAbiVariant(
            base, size, kSapienPlayClassReadAob, kSapienDescriptorMove,
            kSapienClassRead, 1, kFifthArgumentLoad, matches);
        if (matches.malformed || matches.candidateCount != 1 ||
            !matches.candidate)
        {
            LOG("Reach crosshair: HREK class-2 widget ABI match count=%zu "
                "malformed=%d; mandatory parity transaction rejected",
                matches.candidateCount, static_cast<int>(matches.malformed));
            return false;
        }

        target = reinterpret_cast<void*>(matches.candidate);
        return true;
    }

    bool InstallReachCameraCore(uintptr_t base, size_t size, uint32_t generation)
    {
        if (g_reachCamera.installed.load(std::memory_order_acquire))
            return true;
        if (!base || !generation || size != kReachRetailImageSize)
            return false;
        if (g_reachChudParityFailedGeneration.load(
                std::memory_order_acquire) == generation)
        {
            return false;
        }
        if (kReachPlayerViewRenderRva >= size || kReachMainRenderViewRva >= size)
            return false;

        const ReachModuleEpoch epoch{base, generation};
        const ReachPreflightToken preflight =
            ReachRenderCandidate_GetPreflight(epoch);
        if (!preflight.Complete() ||
            !ReachRenderCandidate_IsPreflightCurrent(preflight))
            return false;

        static constexpr char kFpInterpolateAob[] =
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 33 DB 49 63 F8 38 1D ?? ?? ?? ?? 4D 8B E1 8B EA 4C 63 D9";
        static constexpr char kFpPaletteAob[] =
            "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 49 8B F0 0F B7 C9 4C 8B F2";
        static constexpr char kFpCameraAob[] =
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 4C 8B 41 08 48 8D 05 ?? ?? ?? ?? 48 8B D9 0F 29 74 24 30";
        if (!ReachColdExactSignatureAt(
                base,size,kReachFpInterpolateRva,kFpInterpolateAob) ||
            !ReachColdExactSignatureAt(
                base,size,kReachFpVisiblePaletteRva,kFpPaletteAob) ||
            !ReachColdExactSignatureAt(
                base,size,kReachFpCameraRebuildRva,kFpCameraAob) ||
            !ReachColdExecutableAddress(base + kReachFpCameraUploadRva))
        {
            LOG("Reach FP install: exact interpolation/palette/camera "
                "signatures failed; stock Reach remains active");
            return false;
        }

        uint8_t* reachNativeWeaponIkDisable = nullptr;
        uint8_t reachNativeWeaponIkDisableOriginal = 0;
        if (!ResolveReachNativeWeaponIkControl(
                base, size, reachNativeWeaponIkDisable,
                reachNativeWeaponIkDisableOriginal))
        {
            LOG("Reach FP install: exact native weapon-IK disable proof failed; "
                "stock Reach remains active");
            return false;
        }

        uint8_t* reachLightmapShadowsEnabled = nullptr;
        uint8_t reachLightmapShadowsOriginal = 0;
        const bool reachLightmapShadowsResolved =
            kReachLightmapShadowIsolationEnabled &&
            ResolveReachLightmapShadowsControl(
                base, size, reachLightmapShadowsEnabled,
                reachLightmapShadowsOriginal);
        if (kReachLightmapShadowIsolationEnabled &&
            !reachLightmapShadowsResolved)
        {
            LOG("Reach world-shadow candidate UNAVAILABLE and not armed: "
                "exact render_lightmap_shadows proof failed; only this feature "
                "stays stock and the Reach camera core continues");
        }

        // Independent of the rejected screen-space-shadow and lightmap-shadow
        // branches: HREK proves this is Reach's native SSAO/HDAO renderer, and
        // the pinned retail image has one exact player-view call to it. The
        // module-wide preflight already pins the complete image hash; these
        // local checks additionally pin the entry ABI and call edge before an
        // optional hook is created.
        static constexpr char kReachSsaoEntryAob[] =
            "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC A0 00 00 00";
        static constexpr char kReachSsaoCallContextAob[] =
            "48 8B 04 C1 48 8D 0C 90 E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 74";
        static constexpr char kReachShadowMaskAcquireAob[] =
            "48 83 EC 38 BA 02 00 00 00 33 C9 E8 ?? ?? ?? ?? B9 0A 00 00 00";
        const bool reachSsaoProven =
            kReachSsaoIsolationEnabled &&
            kReachSsaoBodySize == 0x567 &&
            kReachSsaoCallRva >= 8 &&
            kReachSsaoEndRva <= size &&
            kReachShadowMaskAcquireRva < size &&
            kReachSsaoShadowMaskCallRvas[0] >= kReachSsaoRva &&
            kReachSsaoShadowMaskCallRvas[1] < kReachSsaoEndRva &&
            ReachColdExactSignatureAt(
                base, size, kReachSsaoRva, kReachSsaoEntryAob) &&
            ReachColdExactSignatureAt(
                base, size, kReachSsaoCallRva - 8,
                kReachSsaoCallContextAob) &&
            ReachVerifyRel32Call(
                base, kReachSsaoCallRva, kReachSsaoRva) &&
            ReachVerifyRel32Call(
                base, kReachSsaoShadowMaskCallRvas[0],
                kReachShadowMaskAcquireRva) &&
            ReachVerifyRel32Call(
                base, kReachSsaoShadowMaskCallRvas[1],
                kReachShadowMaskAcquireRva) &&
            ReachColdExactSignatureAt(
                base, size, kReachShadowMaskAcquireRva,
                kReachShadowMaskAcquireAob) &&
            ReachColdExecutableAddress(base + kReachSsaoRva);
        if (!kReachSsaoIsolationEnabled)
        {
            LOG("Reach SSAO candidate intentionally disabled after exact "
                "owned-eye suppression was headset-rejected as the black-world "
                "cause");
        }
        else if (!reachSsaoProven)
        {
            LOG("Reach SSAO candidate UNAVAILABLE and not armed: exact native "
                "SSAO entry/call proof failed; only this feature stays stock "
                "and the Reach camera core continues");
        }

        // Resolve the four stock camera-rebuild helpers. The preflight already
        // pinned the exact module SHA-256, so base+RVA is exact; additionally
        // cross-check the two setup call sites so a mismatched image fails open.
        uint8_t* reachPatchyFogFlags = nullptr;
        if (kReachFrustumHelperRva >= size ||
            kReachProjectionBuilderRva >= size ||
            kReachCameraStateUpdaterRva >= size ||
            kReachProjectionMatrixBuilderRva >= size ||
            !ReachVerifyRel32Call(
                base, kReachSetupFrustumCallRva, kReachFrustumHelperRva) ||
            !ReachVerifyRel32Call(
                base, kReachSetupProjectionCallRva,
                kReachProjectionBuilderRva) ||
            !ReachVerifyVisibilityConsumer(base, size) ||
            !ReachVerifyPatchyFogGate(
                base, size, reachPatchyFogFlags))
        {
            LOG("Reach camera install: camera-rebuild helper verification failed; "
                "stock Reach remains active");
            return false;
        }

        float* const reachMotionBlurScale =
            FindDebugVarFloat(base, size, "motion_blur_scale");
        float* const reachMotionBlurMax =
            FindDebugVarFloat(base, size, "motion_blur_max");
        float reachMotionBlurScaleOriginal = 0.0f;
        float reachMotionBlurMaxOriginal = 0.0f;
        if (!reachMotionBlurScale || !reachMotionBlurMax ||
            !ReachMotionBlurSlotsMatchPinnedImage(
                base, size,
                reinterpret_cast<uintptr_t>(reachMotionBlurScale),
                reinterpret_cast<uintptr_t>(reachMotionBlurMax)) ||
            !SafeReadFloat(
                reachMotionBlurScale, &reachMotionBlurScaleOriginal) ||
            !SafeReadFloat(reachMotionBlurMax, &reachMotionBlurMaxOriginal) ||
            !ReachMotionBlurSuppressionValuesValid(
                reachMotionBlurScaleOriginal, reachMotionBlurMaxOriginal))
        {
            LOG("Reach camera install: exact title-native motion-blur "
                "scale/max proof failed; stock Reach remains active");
            return false;
        }

        HMODULE moduleReference = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(base),
                                &moduleReference) ||
            reinterpret_cast<uintptr_t>(moduleReference) != base)
        {
            if (moduleReference)
                FreeLibrary(moduleReference);
            LOG("Reach camera install: could not retain the exact title module");
            return false;
        }
        // Optional, not part of core VR ownership: see kReachHudDrawWidgetRva
        // for how this address was found and verified. A failed create/enable
        // here must never affect the five mandatory hooks below.
        void* hudDrawWidget =
            reinterpret_cast<void*>(base + kReachHudDrawWidgetRva);

        void* inner = reinterpret_cast<void*>(base + kReachPlayerViewRenderRva);
        void* outer = reinterpret_cast<void*>(base + kReachMainRenderViewRva);
        void* fpInterpolate =
            reinterpret_cast<void*>(base + kReachFpInterpolateRva);
        void* fpPalette =
            reinterpret_cast<void*>(base + kReachFpVisiblePaletteRva);
        void* fpCamera =
            reinterpret_cast<void*>(base + kReachFpCameraRebuildRva);
        const bool innerCreated = MH_CreateHook(inner,
                reinterpret_cast<void*>(&ReachPlayerViewRenderDetour),
                reinterpret_cast<void**>(&g_reachOrigPlayerViewRender)) == MH_OK;
        const bool outerCreated = innerCreated && MH_CreateHook(outer,
                reinterpret_cast<void*>(&ReachMainRenderViewDetour),
                reinterpret_cast<void**>(&g_reachOrigMainRenderView)) == MH_OK;
        const bool fpInterpolateCreated = outerCreated && MH_CreateHook(
                fpInterpolate,reinterpret_cast<void*>(&ReachFpInterpolate),
                reinterpret_cast<void**>(&g_reachOrigFpInterpolate)) == MH_OK;
        const bool fpPaletteCreated = fpInterpolateCreated && MH_CreateHook(
                fpPalette,reinterpret_cast<void*>(&ReachFpPalette),
                reinterpret_cast<void**>(&g_reachOrigFpPalette)) == MH_OK;
        const bool fpCameraCreated = fpPaletteCreated && MH_CreateHook(
                fpCamera,
                reinterpret_cast<void*>(&ReachFpCameraRebuildDetour),
                reinterpret_cast<void**>(&g_reachOrigFpCameraRebuild)) == MH_OK;
        void* ssao = reachSsaoProven
            ? reinterpret_cast<void*>(base + kReachSsaoRva)
            : nullptr;
        const bool ssaoCreated = fpCameraCreated && ssao && MH_CreateHook(
                ssao,
                reinterpret_cast<void*>(&ReachRenderSsaoDetour),
                reinterpret_cast<void**>(&g_reachOrigRenderSsao)) == MH_OK;
        if (fpCameraCreated && reachSsaoProven && !ssaoCreated)
        {
            LOG("Reach SSAO candidate hook create FAILED at "
                "haloreach.dll+0x%llX; only native SSAO remains active",
                static_cast<unsigned long long>(kReachSsaoRva));
        }
        // Re-enabled with the corrected argument widths. Four prior crashes,
        // all 0xC0000005 at haloreach.dll+0x2ED80C, were caused by this
        // detour declaring arguments 3 and 4 as unsigned short / unsigned
        // char. Reach uses both as full 32-bit values (0x2DA39D
        // "mov r12d, r8d", 0x2DA41D "cmp r12d, dword ptr [rdi+4]", 0x2DA39A
        // "mov eax, r9d"), so the truncated values sent back into the engine
        // selected the wrong branch and decoded an invalid pool handle.
        // The address itself was never wrong: an AOB scan for the class-byte
        // read and forward call-graph tracing from the proven
        // kReachPlayerViewRenderRva independently agree on 0x2DA364.
        const bool hudDrawWidgetCreated = fpCameraCreated && MH_CreateHook(
                hudDrawWidget,
                reinterpret_cast<void*>(&ReachHudDrawWidgetDetour),
                reinterpret_cast<void**>(&g_reachOrigHudDrawWidget)) == MH_OK;
        if (fpCameraCreated && !hudDrawWidgetCreated)
            LOG("Reach crosshair: chud_draw_widget hook create FAILED at "
                "haloreach.dll+0x%llX",
                static_cast<unsigned long long>(kReachHudDrawWidgetRva));
        // Optional and independent of the crosshair: takes rain, objective
        // markers and character tags off the hand. Deliberately NOT chained off
        // hudDrawWidgetCreated - one optional feature must never gate another.
        void* observerCamera = reinterpret_cast<void*>(
            base + kReachRenderCameraFromObserverRva);
        const bool observerCameraCreated = fpCameraCreated && MH_CreateHook(
                observerCamera,
                reinterpret_cast<void*>(&ReachObserverCameraDetour),
                reinterpret_cast<void**>(&g_reachOrigObserverCamera)) == MH_OK;
        if (fpCameraCreated && !observerCameraCreated)
            LOG("Reach head-lock: observer-camera hook create FAILED at "
                "haloreach.dll+0x%llX; rain/markers/tags stay on the hand, "
                "nothing else affected",
                static_cast<unsigned long long>(
                    kReachRenderCameraFromObserverRva));
        // Optional and independent: the rain volume. Located by a UNIQUE
        // interior signature because this function's plain prologue matches
        // three times in the module; the entry is the match minus 0x3C, and it
        // is cross-checked against the expected RVA before use.
        void* rainRender = nullptr;
        bool rainRenderCreated = false;
        // DISABLED 2026-07-27 by the player's decision after a headset test:
        // zeroing the rain volume's forward offset changed nothing they could
        // see ("it's still behaving the exact same ... just remove that hook").
        // Frame rate was explicitly confirmed unaffected, so this is removed
        // because it does not work, not because it costs anything.
        //
        // The evidence stays recorded in reach_render_logic.h because it is
        // solid and measured: 0x00288D60 really does compute
        // centre = position + forward * (size * 0.45) from the top-of-stack
        // workspace's secondary compact camera. The negative result is that
        // this is NOT what the player perceives as the rain following them, so
        // the visible rain must be driven by something else - most likely the
        // second, still-unidentified weather draw dispatched from
        // player_view_render at 0x0026CC8D (target 0x00168110, single caller),
        // which is the obvious next place to look. Reach rain is a known open
        // bug, deferred by the player.
        constexpr bool kReachRainDecoupleEnabled = false;
        if (kReachRainDecoupleEnabled)
        {
            const uintptr_t rainHit = sig::Find(base, size, kReachRainRenderSig);
            const bool rainUnique = rainHit &&
                !sig::Find(rainHit + 1, base + size - rainHit - 1,
                           kReachRainRenderSig);
            if (!rainUnique)
            {
                LOG("Reach rain: renderer signature %s; the rain keeps "
                    "swinging with the view, nothing else affected",
                    rainHit ? "ambiguous" : "missing");
            }
            else
            {
                const uintptr_t entry =
                    rainHit - kReachRainRenderSigEntryOffset;
                if (entry - base != kReachRainParticleRenderRva)
                    LOG("Reach rain: renderer resolved to haloreach.dll+0x%llX "
                        "(expected 0x%llX); using the resolved address",
                        static_cast<unsigned long long>(entry - base),
                        static_cast<unsigned long long>(
                            kReachRainParticleRenderRva));
                rainRender = reinterpret_cast<void*>(entry);
                rainRenderCreated = fpCameraCreated && MH_CreateHook(
                        rainRender,
                        reinterpret_cast<void*>(&ReachRainRenderDetour),
                        reinterpret_cast<void**>(
                            &g_reachOrigRainRender)) == MH_OK;
                if (fpCameraCreated && !rainRenderCreated)
                    LOG("Reach rain: renderer hook create FAILED at "
                        "haloreach.dll+0x%llX",
                        static_cast<unsigned long long>(entry - base));
            }
        }
        // Optional and independent again: the second muzzle flash. The
        // first-person marker query is DECODED from the resolver's own tail
        // jump rather than hardcoded, which also verifies we matched the right
        // function - a wrong match cannot produce a valid E9 there.
        void* effectLocation = reinterpret_cast<void*>(
            base + kReachEffectLocationResolverRva);
        bool effectLocationCreated = false;
        {
            const uintptr_t jumpSite =
                base + kReachEffectLocationResolverRva +
                kReachEffectLocationFpJumpOffset;
            uintptr_t fpQuery = 0;
            if (*reinterpret_cast<const uint8_t*>(jumpSite) == 0xE9)
                fpQuery = sig::RipTarget(jumpSite + 1, jumpSite + 5);
            if (fpQuery < base || fpQuery >= base + size)
            {
                LOG("Reach muzzle: first-person marker query did not decode "
                    "from the resolver tail jump; the second muzzle flash "
                    "stays at the face, nothing else affected");
            }
            else if (fpCameraCreated)
            {
                g_reachEffectFpMarkerQuery =
                    reinterpret_cast<ReachEffectFpMarkerFn>(fpQuery);
                // The marker-not-found fallback matrix this module hands out.
                // Comparing against the engine's own copy is what makes the
                // repair exact rather than a guess about which particle is bad.
                g_reachEffectIdentity = reinterpret_cast<const float*>(
                    base + kReachEffectIdentityMatrixRva);
                g_reachLastGoodValid = false;
                g_reachLastGoodEffect = nullptr;
                effectLocationCreated = MH_CreateHook(
                    effectLocation,
                    reinterpret_cast<void*>(&ReachEffectLocationDetour),
                    reinterpret_cast<void**>(
                        &g_reachOrigEffectLocation)) == MH_OK;
                if (!effectLocationCreated)
                {
                    g_reachEffectFpMarkerQuery = nullptr;
                    LOG("Reach muzzle: effect-location hook create FAILED at "
                        "haloreach.dll+0x%llX",
                        static_cast<unsigned long long>(
                            kReachEffectLocationResolverRva));
                }
                else
                {
                    LOG("Reach muzzle: first-person marker query decoded to "
                        "haloreach.dll+0x%llX (expected 0x%llX)",
                        static_cast<unsigned long long>(fpQuery - base),
                        static_cast<unsigned long long>(
                            kReachEffectFpMarkerQueryRva));
                }
            }
        }
        if (!innerCreated || !outerCreated ||
            !fpInterpolateCreated || !fpPaletteCreated || !fpCameraCreated)
        {
            bool cleanupOk=true;
            bool innerRetained=innerCreated;
            bool outerRetained=outerCreated;
            bool fpInterpolateRetained=fpInterpolateCreated;
            bool fpPaletteRetained=fpPaletteCreated;
            bool fpCameraRetained=fpCameraCreated;
            bool ssaoRetained=ssaoCreated;
            bool hudDrawWidgetRetained=hudDrawWidgetCreated;
            bool observerCameraRetained=observerCameraCreated;
            bool effectLocationRetained=effectLocationCreated;
            bool rainRenderRetained=rainRenderCreated;
            auto removeCreated=[&](bool created,void* target,bool& retained) {
                if (!created) return;
                const MH_STATUS status=MH_RemoveHook(target);
                if (status==MH_OK || status==MH_ERROR_NOT_CREATED)
                    retained=false;
                else
                    cleanupOk=false;
            };
            removeCreated(rainRenderCreated,rainRender,
                          rainRenderRetained);
            removeCreated(effectLocationCreated,effectLocation,
                          effectLocationRetained);
            removeCreated(observerCameraCreated,observerCamera,
                          observerCameraRetained);
            removeCreated(hudDrawWidgetCreated,hudDrawWidget,
                          hudDrawWidgetRetained);
            removeCreated(ssaoCreated,ssao,ssaoRetained);
            removeCreated(fpCameraCreated,fpCamera,fpCameraRetained);
            removeCreated(fpPaletteCreated,fpPalette,fpPaletteRetained);
            removeCreated(fpInterpolateCreated,fpInterpolate,
                          fpInterpolateRetained);
            removeCreated(outerCreated,outer,outerRetained);
            removeCreated(innerCreated,inner,innerRetained);

            if (!innerRetained) g_reachOrigPlayerViewRender=nullptr;
            if (!outerRetained) g_reachOrigMainRenderView=nullptr;
            if (!fpInterpolateRetained) g_reachOrigFpInterpolate=nullptr;
            if (!fpPaletteRetained) g_reachOrigFpPalette=nullptr;
            if (!fpCameraRetained) g_reachOrigFpCameraRebuild=nullptr;
            if (!ssaoRetained) g_reachOrigRenderSsao=nullptr;
            if (!hudDrawWidgetRetained) g_reachOrigHudDrawWidget=nullptr;
            if (!observerCameraRetained) g_reachOrigObserverCamera=nullptr;
            if (!effectLocationRetained)
            {
                g_reachOrigEffectLocation=nullptr;
                g_reachEffectFpMarkerQuery=nullptr;
            }
            if (!rainRenderRetained) g_reachOrigRainRender=nullptr;
            if (cleanupOk)
            {
                FreeLibrary(moduleReference);
            }
            else
            {
                g_reachCamera.base=base;
                g_reachCamera.size=size;
                g_reachCamera.generation=generation;
                g_reachCamera.moduleReference=moduleReference;
                g_reachCamera.innerTarget=innerRetained?inner:nullptr;
                g_reachCamera.outerTarget=outerRetained?outer:nullptr;
                g_reachCamera.fpInterpolateTarget=
                    fpInterpolateRetained?fpInterpolate:nullptr;
                g_reachCamera.fpPaletteTarget=
                    fpPaletteRetained?fpPalette:nullptr;
                g_reachCamera.fpCameraTarget=
                    fpCameraRetained?fpCamera:nullptr;
                g_reachCamera.ssaoTarget=ssaoRetained?ssao:nullptr;
                g_reachCamera.ssaoIsolationActive.store(
                    false,std::memory_order_release);
                g_reachCamera.hudDrawWidgetTarget=
                    hudDrawWidgetRetained?hudDrawWidget:nullptr;
                g_reachCamera.observerCameraTarget=
                    observerCameraRetained?observerCamera:nullptr;
                g_reachCamera.effectLocationTarget=
                    effectLocationRetained?effectLocation:nullptr;
                g_reachCamera.rainRenderTarget=
                    rainRenderRetained?rainRender:nullptr;
                g_reachCamera.armed.store(false,std::memory_order_release);
                g_reachCamera.teardownRequested.store(
                    true,std::memory_order_release);
                g_reachCamera.installed.store(true,std::memory_order_release);
            }
            LOG("Reach camera install: mandatory parity hook creation failed; "
                "the complete Reach VR transaction was rejected");
            return false;
        }

        g_reachHelpers.frustum = reinterpret_cast<ReachFrustumHelperFn>(
            base + kReachFrustumHelperRva);
        g_reachHelpers.projection = reinterpret_cast<ReachProjectionBuilderFn>(
            base + kReachProjectionBuilderRva);
        g_reachHelpers.cameraState = reinterpret_cast<ReachCameraStateUpdaterFn>(
            base + kReachCameraStateUpdaterRva);
        g_reachHelpers.matrix = reinterpret_cast<ReachMatrixBuilderFn>(
            base + kReachProjectionMatrixBuilderRva);
        g_reachCamera.base = base;
        g_reachCamera.size = size;
        g_reachCamera.generation = generation;
        g_reachCamera.moduleReference = moduleReference;
        g_reachCamera.innerTarget = inner;
        g_reachCamera.outerTarget = outer;
        g_reachCamera.fpInterpolateTarget = fpInterpolate;
        g_reachCamera.fpPaletteTarget = fpPalette;
        g_reachCamera.fpCameraTarget = fpCamera;
        g_reachCamera.ssaoTarget = ssaoCreated ? ssao : nullptr;
        g_reachCamera.ssaoIsolationActive.store(
            false, std::memory_order_release);
        g_reachCamera.hudDrawWidgetTarget =
            hudDrawWidgetCreated ? hudDrawWidget : nullptr;
        g_reachCamera.observerCameraTarget =
            observerCameraCreated ? observerCamera : nullptr;
        g_reachCamera.effectLocationTarget =
            effectLocationCreated ? effectLocation : nullptr;
        g_reachCamera.rainRenderTarget =
            rainRenderCreated ? rainRender : nullptr;
        g_reachRainDecoupled.store(0, std::memory_order_relaxed);
        g_reachRainSkipped.store(0, std::memory_order_relaxed);
        g_reachMuzzleRedirects.store(0, std::memory_order_relaxed);
        g_reachMuzzleRepaired.store(0, std::memory_order_relaxed);
        g_reachMuzzleReparented.store(0, std::memory_order_relaxed);
        g_reachLiveGraphWeaponWrites.store(0, std::memory_order_relaxed);
        g_reachWorldWristWritesPrevented.store(0, std::memory_order_relaxed);
        g_reachHudFlashHidden.store(0, std::memory_order_relaxed);
        for (auto& slot : g_reachChudMaxCollection)
            slot.store(0, std::memory_order_relaxed);
        g_reachMuzzleOutOfRange.store(0, std::memory_order_relaxed);
        g_reachMuzzleNearestMilli.store(0xFFFFFFFFu, std::memory_order_relaxed);
        g_reachMuzzleIdentityNoSibling.store(0, std::memory_order_relaxed);
        g_reachMuzzleReadFailures.store(0, std::memory_order_relaxed);
        g_reachMuzzleWorldNoFpUser.store(0, std::memory_order_relaxed);
        g_reachMuzzleWorldFpNone.store(0, std::memory_order_relaxed);
        g_reachMuzzleAlreadyFp.store(0, std::memory_order_relaxed);
        g_reachMuzzleFpByteLowMask.store(0, std::memory_order_relaxed);
        g_reachMuzzleFpByteHighMask.store(0, std::memory_order_relaxed);
        for (int site = 0; site < 6; ++site)
        {
            g_reachObserverCameraSiteHits[site].store(
                0, std::memory_order_relaxed);
            g_reachObserverCameraCorrected[site].store(
                0, std::memory_order_relaxed);
        }
        g_reachObserverCameraUnknownSite.store(0, std::memory_order_relaxed);
        g_reachFpCameraUpload = reinterpret_cast<ReachFpCameraUploadFn>(
            base + kReachFpCameraUploadRva);
        g_reachCamera.installedAtMs = GetTickCount64();
        g_reachCamera.motionBlurVars[0] = {
            reachMotionBlurScale, reachMotionBlurScaleOriginal};
        g_reachCamera.motionBlurVars[1] = {
            reachMotionBlurMax, reachMotionBlurMaxOriginal};
        g_reachCamera.motionBlurResolved = true;
        g_reachCamera.motionBlurSuppressed = false;
        g_reachCamera.patchyFogFlags = reachPatchyFogFlags;
        g_reachCamera.lightmapShadowsEnabled =
            reachLightmapShadowsEnabled;
        g_reachLightmapShadowSuppressedEyes.store(
            0, std::memory_order_relaxed);
        g_reachLightmapShadowAlreadyDisabledEyes.store(
            0, std::memory_order_relaxed);
        g_reachLightmapShadowWriteFailures.store(
            0, std::memory_order_relaxed);
        g_reachLightmapShadowRestoreFailures.store(
            0, std::memory_order_relaxed);
        g_reachLightmapShadowOutstandingValue.store(
            0, std::memory_order_relaxed);
        g_reachLightmapShadowRestoreOutstanding.store(
            false, std::memory_order_release);
        for (auto& count : g_reachSsaoSuppressedEyeCalls)
            count.store(0, std::memory_order_relaxed);
        g_reachSsaoPassthroughCalls.store(0, std::memory_order_relaxed);
        g_reachCamera.nativeWeaponIkDisable = reachNativeWeaponIkDisable;
        g_reachCamera.nativeWeaponIkDisableOriginal =
            reachNativeWeaponIkDisableOriginal;
        g_reachCamera.nativeWeaponIkBypassActive = false;
        for (int eye = 0; eye < 2; ++eye)
        {
            g_reachRenderFovSerial[eye].store(0, std::memory_order_release);
            g_reachRenderHalfFovX[eye].store(0.0f, std::memory_order_relaxed);
            g_reachRenderHalfFovY[eye].store(0.0f, std::memory_order_relaxed);
        }
        g_reachFpCameraUploadStatus.preparedSerial.store(
            0, std::memory_order_release);
        g_reachFpCameraUploadStatus.generation.store(
            0, std::memory_order_relaxed);
        g_reachFpCameraUploadStatus.eyeMask.store(
            0, std::memory_order_relaxed);
        g_reachFpCameraLoggedGeneration.store(
            0, std::memory_order_release);
        g_aimSeen.store(false, std::memory_order_release);
        g_camValid.store(false, std::memory_order_release);
        g_baseCamValid.store(false, std::memory_order_release);
        g_reachCamera.armed.store(false, std::memory_order_release);
        g_reachCamera.teardownRequested.store(
            false, std::memory_order_release);
        g_reachCamera.installed.store(true, std::memory_order_release);
        g_needRecenter.store(true, std::memory_order_release);
        if (MH_EnableHook(inner) != MH_OK ||
            MH_EnableHook(outer) != MH_OK ||
            MH_EnableHook(fpInterpolate) != MH_OK ||
            MH_EnableHook(fpPalette) != MH_OK ||
            MH_EnableHook(fpCamera) != MH_OK)
        {
            // State was published before the first enable, so even a partial
            // MinHook failure can use the same disable/quiesce/remove proof as
            // a title transition. A failed proof retains every dependency and
            // teardownRequested makes the worker retry instead of re-arming.
            LOG("Reach camera install: MinHook enable failed; requesting "
                "verified teardown");
            RemoveReachCameraCore();
            return false;
        }
        // DISABLED 2026-07-27 after a headset test. The patch verifiably
        // applied (log: "suppressed at haloreach.dll+0x1D4F18") and denied
        // camera-mode-2 particle systems engine-wide, and the face-stuck flash
        // kept rendering. So the element is NOT a third-person-only particle
        // system. Combined with the earlier eliminations this rules out the
        // entire per-mode emission gate as the source. Kept for evidence.
        // DISABLED 2026-07-27 after the decisive headset test: denying mode-1
        // removed BOTH muzzle flashes - the face one AND the gun one. That is
        // the proof that both are first-person-only particle systems; the
        // difference between them is only WHERE their spawn transform comes
        // from. A blanket denial is therefore too broad, per the player:
        // "restore the muzzle flashes". The selective fix must retarget the
        // stuck system's spawn transform instead of gating emission.
        // ApplyReachThirdPersonEffectSuppression(base, size);
        if (!ApplyReachNativeWeaponIkBypass())
        {
            LOG("Reach camera install: native weapon-IK bypass failed; "
                "requesting verified teardown of the complete parity transaction");
            RemoveReachCameraCore();
            return false;
        }
        LOG("Reach camera core installed: outer/inner stereo + FP "
            "interpolation/palette + per-eye world-projection camera "
            "transactions hooked; waiting one-second fresh-camera interval "
            "before arming");
        // Optional, feature-local candidate. Its exact return-address and
        // thread-local/live ownership gates keep flat, nested, screenshot, and
        // unrelated native calls on the original SSAO path. A create/enable
        // failure never tears down the working Reach camera core.
        if (ssaoCreated)
        {
            const MH_STATUS enableStatus = MH_EnableHook(ssao);
            if (enableStatus == MH_OK)
            {
                g_reachCamera.ssaoIsolationActive.store(
                    true, std::memory_order_release);
                LOG("Reach SSAO candidate BOUND with zero samples: exact native "
                    "SSAO/HDAO +0x%llX will be skipped only for the exact owned "
                    "VR-eye call +0x%llX; all other calls remain stock",
                    static_cast<unsigned long long>(kReachSsaoRva),
                    static_cast<unsigned long long>(kReachSsaoCallRva));
            }
            else
            {
                g_reachCamera.ssaoIsolationActive.store(
                    false, std::memory_order_release);
                const MH_STATUS disableStatus = MH_DisableHook(ssao);
                const MH_STATUS removeStatus = MH_RemoveHook(ssao);
                if (removeStatus == MH_OK ||
                    removeStatus == MH_ERROR_NOT_CREATED)
                {
                    g_reachCamera.ssaoTarget = nullptr;
                    g_reachOrigRenderSsao = nullptr;
                }
                else
                {
                    LOG("Reach SSAO candidate enable failed (%d), and its "
                        "hook disable/remove failed (%d/%d); retained for "
                        "verified title teardown",
                        static_cast<int>(enableStatus),
                        static_cast<int>(disableStatus),
                        static_cast<int>(removeStatus));
                }
                LOG("Reach SSAO candidate not armed; native SSAO remains active "
                    "and the Reach camera core continues");
            }
        }
        // Optional and fail-open, same as the mandatory five are not: a failed
        // enable here removes only this one hook and continues, exactly like a
        // failed create above. Never RemoveReachCameraCore() for this hook.
        if (hudDrawWidgetCreated &&
            MH_EnableHook(hudDrawWidget) != MH_OK)
        {
            LOG("Reach crosshair: chud_draw_widget hook enable failed; "
                "native crosshair stays visible, nothing else affected");
            MH_RemoveHook(hudDrawWidget);
            g_reachCamera.hudDrawWidgetTarget = nullptr;
            g_reachOrigHudDrawWidget = nullptr;
        }
        else if (hudDrawWidgetCreated)
        {
            LOG("Reach crosshair: chud_draw_widget hook active at "
                "haloreach.dll+0x%llX; authored-widget capture pending "
                "per-eye execution",
                static_cast<unsigned long long>(kReachHudDrawWidgetRva));
        }
        // Same optional, fail-open contract: a failure here removes only this
        // hook. The world render camera is never corrected by it either way, so
        // the accepted Reach 3D cannot be affected by this feature failing.
        if (observerCameraCreated &&
            MH_EnableHook(observerCamera) != MH_OK)
        {
            LOG("Reach head-lock: observer-camera hook enable failed; rain, "
                "objective markers and character tags stay on the hand, "
                "nothing else affected");
            MH_RemoveHook(observerCamera);
            g_reachCamera.observerCameraTarget = nullptr;
            g_reachOrigObserverCamera = nullptr;
        }
        else if (observerCameraCreated)
        {
            LOG("Reach head-lock: observer-camera hook active at "
                "haloreach.dll+0x%llX; rain/objective markers/character tags "
                "will follow the head only",
                static_cast<unsigned long long>(
                    kReachRenderCameraFromObserverRva));
        }
        if (effectLocationCreated &&
            MH_EnableHook(effectLocation) != MH_OK)
        {
            LOG("Reach muzzle: effect-location hook enable failed; the second "
                "muzzle flash stays at the face, nothing else affected");
            MH_RemoveHook(effectLocation);
            g_reachCamera.effectLocationTarget = nullptr;
            g_reachOrigEffectLocation = nullptr;
            g_reachEffectFpMarkerQuery = nullptr;
        }
        else if (effectLocationCreated)
        {
            LOG("Reach muzzle: effect-location hook active at "
                "haloreach.dll+0x%llX; world locations of first-person weapon "
                "effects will resolve on the gun",
                static_cast<unsigned long long>(
                    kReachEffectLocationResolverRva));
        }
        if (rainRenderCreated && MH_EnableHook(rainRender) != MH_OK)
        {
            LOG("Reach rain: renderer hook enable failed; the rain keeps "
                "swinging with the view, nothing else affected");
            MH_RemoveHook(rainRender);
            g_reachCamera.rainRenderTarget = nullptr;
            g_reachOrigRainRender = nullptr;
        }
        else if (rainRenderCreated)
        {
            LOG("Reach rain: renderer hook active; the rain volume will centre "
                "on the camera instead of swinging along its forward vector");
        }
        // Optional and fail-open: kill_reticle only. If this cannot be resolved
        // exactly, Reach keeps its stock crosshair and nothing else changes.
        if (!ResolveReachChudCrosshairFields(base, size))
            LOG("Reach crosshair: chud_show_crosshair script-table chain not "
                "exact; the native crosshair stays visible");
        LOG("Reach FP camera hook installed for the exact HREK-homologous "
            "nested workspace; per-eye execution pending");
        LOG("Reach comfort evidence: blurScale=%llX blurMax=%llX "
            "(authored %.4f/%.4f); VR default keeps scale finite and zeros max",
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(reachMotionBlurScale) - base),
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(reachMotionBlurMax) - base),
            reachMotionBlurScaleOriginal, reachMotionBlurMaxOriginal);
        LOG("Reach stereo fog evidence: screen-aligned patchy helper %llX "
            "is suppressed per eye through exact flag %llX bit 0x%02X",
            static_cast<unsigned long long>(kReachPatchyFogTargetRva),
            static_cast<unsigned long long>(kReachPatchyFogFlagsRva),
            static_cast<unsigned>(kReachPatchyFogSkipMask));
        if (reachLightmapShadowsResolved)
        {
            LOG("Reach world-shadow candidate BOUND with zero samples: exact "
                "render_lightmap_shadows control +0x%llX (authored %u) will be "
                "disabled only inside each owned VR eye render",
                static_cast<unsigned long long>(kReachLightmapShadowsValueRva),
                static_cast<unsigned>(reachLightmapShadowsOriginal));
        }

        return true;
    }

    void LogReachFpCameraUploadIfReady()
    {
        const uint64_t serialBefore =
            g_reachFpCameraUploadStatus.preparedSerial.load(
                std::memory_order_acquire);
        if (!serialBefore)
            return;
        const uint32_t generation =
            g_reachFpCameraUploadStatus.generation.load(
                std::memory_order_relaxed);
        const uint32_t eyeMask =
            g_reachFpCameraUploadStatus.eyeMask.load(
                std::memory_order_acquire);
        const uint64_t serialAfter =
            g_reachFpCameraUploadStatus.preparedSerial.load(
                std::memory_order_acquire);
        if (serialBefore != serialAfter ||
            generation != g_reachCamera.generation ||
            (eyeMask & 0x3u) != 0x3u ||
            generation == g_reachFpCameraLoggedGeneration.load(
                std::memory_order_relaxed))
        {
            return;
        }
        g_reachFpCameraLoggedGeneration.store(
            generation, std::memory_order_release);
        LOG("Reach per-eye FP camera ACTIVE: both eyes uploaded world compact "
            "+ derived projection through the dedicated nested workspace "
            "(viewmodel depth uncrushed)");
    }

    void LogReachFpStatusIfNew()
    {
        const uint64_t key=g_reachFpStatus.key.load(std::memory_order_acquire);
        if (!key || key==g_reachFpLoggedStatusKey.load(
                std::memory_order_relaxed)) return;
        const uint32_t generation=
            g_reachFpStatus.generation.load(std::memory_order_relaxed);
        const int code=g_reachFpStatus.code.load(std::memory_order_relaxed);
        const int bodyCount=
            g_reachFpStatus.bodyCount.load(std::memory_order_relaxed);
        const int liveCount=
            g_reachFpStatus.liveCount.load(std::memory_order_relaxed);
        if (generation!=g_reachCamera.generation) return;
        g_reachFpLoggedStatusKey.store(key,std::memory_order_release);
        if (code==1)
            LOG("Reach FP layout learned stock-only: body=%d live=%d; eligible next prepared pair",
                bodyCount,liveCount);
        else if (code==2)
            LOG("Reach FP forced floating-hands active: body=%d live=%d "
                "arm_ik=%d (Reach ignores floating_hands config; left palette "
                "is independently controller-bound)",
                bodyCount,liveCount,static_cast<int>(g_config.arm_ik));
        else if (code==3)
            LOG("Reach FP layout changed or failed validation at live=%d; next pair remains stock",
                liveCount);
    }

    // Called from the 50 ms title worker's Reach block. Self-contained: it never
    // touches the Halo 3 or ODST state machines.
    // Parity with PublishOdstLifecycle above. Without this Reach never reports
    // an armed lifecycle, so TitleRuntimeMaskUnarmedCapabilities strips every
    // arm-gated capability -- including ControllerAim -- and shared features
    // that ask Game_HasTitleCapability (the VR crosshair quad) stay refused
    // even while Reach's camera core is fully armed and rendering stereo.
    bool PublishReachLifecycle()
    {
        const uint32_t generation =
            TitleAdapter_GetGeneration(GameTitle::HaloReach);
        if (!generation)
            return false;
        TitleRuntimeLifecycle lifecycle{};
        lifecycle.installed =
            g_reachCamera.installed.load(std::memory_order_acquire);
        lifecycle.armed =
            g_reachCamera.armed.load(std::memory_order_acquire);
        lifecycle.teardownRequested =
            g_reachCamera.teardownRequested.load(std::memory_order_acquire);
        lifecycle.enabledCapabilities =
            lifecycle.installed && !lifecycle.teardownRequested
                ? kReachRuntimeCapabilities : TitleCapability_None;

        // Publish ONLY on a real state change. Republishing identical state
        // every 50 ms poll re-opens the runtime publication sequence
        // constantly, which keeps the shared snapshot "pending". A pending
        // snapshot reports mode=Loading with NO capabilities
        // (PendingSnapshotFromCandidate), so Game_HasTitleCapability kept
        // denying ControllerAim and the VR reticle chain was never created --
        // visible in the log as "Runtime mode: gameplay -> loading" flapping
        // ~10x/second forever.
        static uint32_t publishedGeneration = 0;
        static uint32_t publishedState = 0xFFFFFFFFu;
        const uint32_t state =
            (lifecycle.installed ? 1u : 0u) |
            (lifecycle.armed ? 2u : 0u) |
            (lifecycle.teardownRequested ? 4u : 0u) |
            (lifecycle.enabledCapabilities << 3);
        if (generation == publishedGeneration && state == publishedState)
            return true;
        const bool published = TitleAdapter_PublishLifecycle(
            GameTitle::HaloReach, generation, lifecycle);
        if (published)
        {
            publishedGeneration = generation;
            publishedState = state;
        }
        return published;
    }
    // Worker-side only. This reports both the unconditional zero-sample armed
    // state and the first real eye suppression immediately. Later movement is
    // rate-limited to 30 seconds. The hot render hook only updates atomics.
    void ReachLightmapShadowLogTick()
    {
        static uint32_t lastGeneration = 0;
        static uint32_t lastTotal = 0;
        static uint64_t lastReportMs = 0;
        if (!g_reachCamera.installed.load(std::memory_order_acquire))
        {
            lastGeneration = 0;
            lastTotal = 0;
            lastReportMs = 0;
            return;
        }
        if (!g_reachCamera.armed.load(std::memory_order_acquire))
            return;
        if (!g_reachCamera.lightmapShadowsEnabled)
            return;

        const uint32_t generation = g_reachCamera.generation;
        const uint32_t suppressed =
            g_reachLightmapShadowSuppressedEyes.load(
                std::memory_order_relaxed);
        const uint32_t alreadyDisabled =
            g_reachLightmapShadowAlreadyDisabledEyes.load(
                std::memory_order_relaxed);
        const uint32_t writeFailures =
            g_reachLightmapShadowWriteFailures.load(
                std::memory_order_relaxed);
        const uint32_t restoreFailures =
            g_reachLightmapShadowRestoreFailures.load(
                std::memory_order_relaxed);
        const uint32_t total = suppressed + alreadyDisabled + writeFailures +
            restoreFailures;
        const uint64_t now = GetTickCount64();
        const bool newGeneration = generation != lastGeneration;
        const bool firstActivity = total != 0 && lastTotal == 0;
        const bool periodic = total != lastTotal && lastReportMs != 0 &&
            now - lastReportMs >= 30000;
        if (!newGeneration && !firstActivity && !periodic)
            return;

        lastGeneration = generation;
        lastTotal = total;
        lastReportMs = now;
        LOG("Reach world-shadow candidate: generation=%u, "
            "suppressed-eye-renders=%u, already-disabled-eye-renders=%u, "
            "write-failures=%u, restore-failures=%u",
            generation, suppressed, alreadyDisabled, writeFailures,
            restoreFailures);
    }

    // Worker-side proof that the behavioral candidate actually executed. The
    // native render detour performs no logging, allocation, or locking; it only
    // increments lock-free counters for the exact owned eye or stock pass-through.
    void ReachSsaoLogTick()
    {
        static uint32_t lastGeneration = 0;
        static uint32_t lastSuppressed = 0;
        static uint32_t lastPassthrough = 0;
        static uint64_t lastReportMs = 0;
        if (!g_reachCamera.installed.load(std::memory_order_acquire) ||
            !g_reachCamera.ssaoIsolationActive.load(
                std::memory_order_acquire))
        {
            lastGeneration = 0;
            lastSuppressed = 0;
            lastPassthrough = 0;
            lastReportMs = 0;
            return;
        }
        if (!g_reachCamera.armed.load(std::memory_order_acquire))
            return;

        const uint32_t generation = g_reachCamera.generation;
        const uint32_t eye0 = g_reachSsaoSuppressedEyeCalls[0].load(
            std::memory_order_relaxed);
        const uint32_t eye1 = g_reachSsaoSuppressedEyeCalls[1].load(
            std::memory_order_relaxed);
        const uint32_t suppressed = eye0 + eye1;
        const uint32_t passthrough = g_reachSsaoPassthroughCalls.load(
            std::memory_order_relaxed);
        const uint64_t now = GetTickCount64();
        const bool newGeneration = generation != lastGeneration;
        const bool firstSuppression = suppressed != 0 && lastSuppressed == 0;
        const bool periodic =
            (suppressed != lastSuppressed || passthrough != lastPassthrough) &&
            lastReportMs != 0 && now - lastReportMs >= 30000;
        if (!newGeneration && !firstSuppression && !periodic)
            return;

        lastGeneration = generation;
        lastSuppressed = suppressed;
        lastPassthrough = passthrough;
        lastReportMs = now;
        LOG("Reach SSAO candidate EXECUTION: generation=%u, suppressed eye0=%u "
            "eye1=%u, stock pass-through calls=%u",
            generation, eye0, eye1, passthrough);
    }

    // Worker-side only. Reports what the effect-location hook actually saw, so
    // one short headset run decides the muzzle mechanism instead of a fourth
    // guess. Silent until something happens, then rate-limited to 30 s.
    void ReachMuzzleLogTick()
    {
        static uint32_t lastTotal = 0;
        static uint64_t lastReportMs = 0;
        const uint32_t redirects =
            g_reachMuzzleRedirects.load(std::memory_order_relaxed);
        const uint32_t noFpUser =
            g_reachMuzzleWorldNoFpUser.load(std::memory_order_relaxed);
        const uint32_t fpNone =
            g_reachMuzzleWorldFpNone.load(std::memory_order_relaxed);
        const uint32_t alreadyFp =
            g_reachMuzzleAlreadyFp.load(std::memory_order_relaxed);
        const uint32_t failures =
            g_reachMuzzleReadFailures.load(std::memory_order_relaxed);
        const uint32_t total =
            redirects + noFpUser + fpNone + alreadyFp + failures +
            g_reachMuzzleReparented.load(std::memory_order_relaxed) +
            g_reachMuzzleOutOfRange.load(std::memory_order_relaxed);
        if (!total || total == lastTotal)
            return;
        const uint64_t now = GetTickCount64();
        if (lastReportMs && now - lastReportMs < 30000)
            return;
        lastReportMs = now;
        lastTotal = total;
        LOG("REACHFX: retargeted=%u; HUD flash widgets hidden %u; live-graph weapon writes %u; "
            "muzzle re-parented %u, "
            "out of range %u, "
            "nearest approach %u mm; lined up %u (no sibling yet %u); "
            "effect locations - redirected %u, world/no-fp-user %u, "
            "world/fp-output-none %u, already-first-person %u, unreadable %u "
            "(effect+0x50 low nibbles seen 0x%04X, high nibbles 0x%04X)",
            g_reachMuzzleRetargetDone.load(std::memory_order_relaxed),
            g_reachHudFlashHidden.load(std::memory_order_relaxed),
            g_reachLiveGraphWeaponWrites.load(std::memory_order_relaxed),
            g_reachMuzzleReparented.load(std::memory_order_relaxed),
            g_reachMuzzleOutOfRange.load(std::memory_order_relaxed),
            g_reachMuzzleNearestMilli.load(std::memory_order_relaxed),
            g_reachMuzzleRepaired.load(std::memory_order_relaxed),
            g_reachMuzzleIdentityNoSibling.load(std::memory_order_relaxed),
            redirects, noFpUser, fpNone, alreadyFp, failures,
            g_reachMuzzleFpByteLowMask.load(std::memory_order_relaxed),
            g_reachMuzzleFpByteHighMask.load(std::memory_order_relaxed));
    }

    // Worker-side proof for the isolated 511eb0b rejection. The palette hook
    // only increments a lock-free counter; logging stays off the render path.
    void ReachWorldWristWriteLogTick()
    {
        static uint32_t lastGeneration = 0;
        static uint32_t lastPrevented = 0;
        static uint64_t lastReportMs = 0;
        if (!g_reachCamera.installed.load(std::memory_order_acquire))
        {
            lastGeneration = 0;
            lastPrevented = 0;
            lastReportMs = 0;
            return;
        }
        const uint32_t generation = g_reachCamera.generation;
        const uint32_t prevented = g_reachWorldWristWritesPrevented.load(
            std::memory_order_relaxed);
        const uint32_t executed = g_reachLiveGraphWeaponWrites.load(
            std::memory_order_relaxed);
        const uint64_t now = GetTickCount64();
        const bool newGeneration = generation != lastGeneration;
        const bool firstPrevention = prevented != 0 && lastPrevented == 0;
        const bool periodic = prevented != lastPrevented &&
            lastReportMs != 0 && now - lastReportMs >= 30000;
        if (!newGeneration && !firstPrevention && !periodic)
            return;
        lastGeneration = generation;
        lastPrevented = prevented;
        lastReportMs = now;
        LOG("Reach FP post-palette world-to-local wrist write DISABLED: "
            "generation=%u, prevented=%u, executed=%u",
            generation, prevented, executed);
    }

    // Worker-side only: report which of the six observer-camera call sites are
    // actually exercised, and which ones the head-lock corrected.
    //
    // This originally logged only when the SET of exercised sites changed, which
    // made it nearly useless: the accepted 6bd17db session printed one snapshot
    // 57 ms after arming, with 0 corrections on every row because the eye scope
    // had not run yet, and never printed again. The headset result proved the
    // correction works while the counters said nothing. Report on a set change
    // OR every 30 s while counts are still moving, so the numbers reflect play.
    void ReachObserverCameraLogTick()
    {
        static uint32_t lastMask = 0xFFFFFFFFu;
        static uint64_t lastReportMs = 0;
        static uint32_t lastTotal = 0;
        uint32_t mask = 0;
        uint32_t hits[6]{};
        uint32_t fixed[6]{};
        uint32_t total = 0;
        for (int site = 0; site < 6; ++site)
        {
            hits[site] = g_reachObserverCameraSiteHits[site].load(
                std::memory_order_relaxed);
            fixed[site] = g_reachObserverCameraCorrected[site].load(
                std::memory_order_relaxed);
            total += hits[site] + fixed[site];
            if (hits[site])
                mask |= (1u << site);
        }
        const uint32_t unknown =
            g_reachObserverCameraUnknownSite.load(std::memory_order_relaxed);
        if (unknown)
            mask |= (1u << 6);
        const uint64_t now = GetTickCount64();
        const bool setChanged = mask != lastMask;
        const bool periodic = total != lastTotal && lastReportMs != 0 &&
            now - lastReportMs >= 30000;
        if (!setChanged && !periodic)
            return;
        lastMask = mask;
        lastReportMs = now;
        lastTotal = total;
        if (!mask)
            return;
        for (int site = 0; site < 6; ++site)
        {
            if (!hits[site])
                continue;
            const char* role =
                site == kReachObserverCameraWorldSite ? "world render (never corrected)"
                : site == kReachObserverCameraChudSite ? "CHUD marker projection"
                : "unidentified";
            LOG("Reach head-lock: site %d return +0x%llX %s - %u calls, "
                "%u head-locked", site,
                (unsigned long long)kReachObserverCameraReturnRvas[site],
                role, hits[site], fixed[site]);
        }
        if (unknown)
            LOG("Reach head-lock: %u calls from an unrecognised return address "
                "(left untouched)", unknown);
    }

    // Guarded read of Reach's native pause byte. Same shape as Halo 3's
    // ReadEnginePaused and ODST's ReadOdstEnginePaused: a value outside {0,1}
    // means the binding is no longer trustworthy, so report "unknown" and let
    // the caller behave exactly as it did before this feature existed.
    bool ReadReachEnginePaused(bool& paused)
    {
        const uintptr_t flag =
            g_reachNativePauseFlag.load(std::memory_order_acquire);
        if (!flag)
            return false;
        __try
        {
            const uint8_t value = *reinterpret_cast<const uint8_t*>(flag);
            if (value > 1)
                return false;
            paused = value != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // 50 ms worker: locate Reach's native pause flag once per module
    // generation. Fail-open in every branch - a missing, ambiguous, or
    // out-of-range result logs once and leaves Reach behaving exactly as it did
    // before (always Gameplay while armed). It never blocks arming, never
    // disarms the camera core, and never touches Halo 3 or ODST.
    void ReachPauseColdPoll(
        uintptr_t base, size_t size, uint32_t generation, bool soleReachTitle)
    {
        static uint32_t attemptedGeneration = 0;
        if (!soleReachTitle || !base || !generation)
        {
            g_reachNativePauseFlag.store(0, std::memory_order_release);
            g_reachEnginePauseCache.store(-1, std::memory_order_release);
            attemptedGeneration = 0;
            return;
        }
        if (attemptedGeneration == generation)
            return;
        attemptedGeneration = generation;
        g_reachNativePauseFlag.store(0, std::memory_order_release);
        g_reachEnginePauseCache.store(-1, std::memory_order_release);

        const uintptr_t hit = sig::Find(base, size, kReachNativePauseOwnerSig);
        if (!hit || sig::Find(hit + 1, base + size - hit - 1,
                              kReachNativePauseOwnerSig))
        {
            LOG("Reach pause state: owner signature %s; Reach keeps reporting "
                "gameplay while armed (no pause presentation)",
                hit ? "ambiguous" : "missing");
            return;
        }
        // The store is the last instruction of the signature:
        //   mov byte ptr [rip+disp32], r13b   (7 bytes, disp32 at +3)
        const uintptr_t store = hit + kReachNativePauseStoreOffset;
        const uintptr_t flag =
            sig::RipTarget(store + 3, hit + kReachNativePauseOwnerSigLength);
        if (flag < base || flag >= base + size)
        {
            LOG("Reach pause state: decoded flag falls outside haloreach.dll; "
                "pause presentation stays off");
            return;
        }
        uint8_t initial = 0xFF;
        __try
        {
            initial = *reinterpret_cast<const uint8_t*>(flag);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            initial = 0xFF;
        }
        if (initial > 1)
        {
            LOG("Reach pause state: flag failed boolean validation (%u); "
                "pause presentation stays off", static_cast<unsigned>(initial));
            return;
        }

        g_reachNativePauseFlag.store(flag, std::memory_order_release);
        LOG("Reach pause state: native flag at haloreach.dll+0x%llX "
            "(initial=%u, owner +0x%llX, expected +0x%llX/+0x%llX)",
            (unsigned long long)(flag - base), static_cast<unsigned>(initial),
            (unsigned long long)(hit - base),
            (unsigned long long)kReachNativePauseOwnerRva,
            (unsigned long long)kReachNativePauseFlagRva);
    }

    // 50 ms worker: locate the cinematic-globals registration once per module
    // generation and decode everything the sampler needs. Fail-open, one log
    // line either way; never touches the camera core.
    void ReachCineProbeColdPoll(
        uintptr_t base, size_t size, uint32_t generation, bool soleReachTitle)
    {
        if (!soleReachTitle || !base || !generation)
        {
            g_reachCineProbe.armed.store(false, std::memory_order_release);
            g_reachCineProbe.cutPrevValid = false;
            g_reachCineProbe.cutPrevPoseValid = false;
            return;
        }
        if (g_reachCineProbe.attemptedGeneration.load(
                std::memory_order_acquire) == generation)
            return;
        g_reachCineProbe.armed.store(false, std::memory_order_release);
        g_reachCineProbe.cutPrevValid = false;
        g_reachCineProbe.cutPrevPoseValid = false;
        g_reachCineProbe.attemptedGeneration.store(
            generation, std::memory_order_release);

        const uintptr_t hit = sig::Find(base, size, kReachCineRegistrationSig);
        if (!hit || sig::Find(hit + 1, base + size - hit - 1,
                              kReachCineRegistrationSig))
        {
            LOG("REACHCINE: registration signature %s; probe off",
                hit ? "ambiguous" : "missing");
            return;
        }
        const uintptr_t tlsIndexAddr = sig::RipTarget(hit + 18, hit + 22);
        const uintptr_t nameAddr = sig::RipTarget(hit + 49, hit + 53);
        const uint32_t slotA = *reinterpret_cast<const uint32_t*>(hit + 70);
        const size_t verifyLength = strlen(kReachCineVerifyName) + 1;
        if (tlsIndexAddr < base || tlsIndexAddr + 4 > base + size ||
            nameAddr < base || nameAddr + verifyLength > base + size ||
            memcmp(reinterpret_cast<const void*>(nameAddr),
                   kReachCineVerifyName, verifyLength) != 0)
        {
            LOG("REACHCINE: decoded operands failed verification; probe off");
            return;
        }
        if (slotA < 8 || slotA > 0x8000 || (slotA & 7) != 0)
        {
            LOG("REACHCINE: member A TLS slot 0x%X out of range; probe off",
                slotA);
            return;
        }
        uint32_t slotB = 0;
        const uintptr_t tailStart = hit + kReachCineSigLength;
        constexpr size_t kTailSpan = 0x80;
        if (tailStart + kTailSpan <= base + size)
        {
            const uintptr_t tail =
                sig::Find(tailStart, kTailSpan, kReachCineSlotBSig);
            if (tail)
            {
                const uint32_t candidate =
                    *reinterpret_cast<const uint32_t*>(tail + 1);
                if (candidate >= 8 && candidate <= 0x8000 &&
                    (candidate & 7) == 0 && candidate != slotA)
                    slotB = candidate;
            }
        }

        g_reachCineProbe.tlsIndex = reinterpret_cast<uint32_t*>(tlsIndexAddr);
        g_reachCineProbe.slotA = slotA;
        g_reachCineProbe.slotB = slotB;
        g_reachCineProbe.sampleFailures.store(0, std::memory_order_relaxed);
        g_reachCineProbe.logReset.fetch_add(1, std::memory_order_release);
        g_reachCineProbe.armed.store(true, std::memory_order_release);
        LOG("REACHCINE: probe armed (tls index rva 0x%llX, member slots "
            "+0x%X/+0x%X%s); log-only",
            static_cast<unsigned long long>(tlsIndexAddr - base), slotA, slotB,
            slotB ? "" : " - member B unavailable");
    }

    // Worker-side diff of one probed member bank against the last values it
    // logged. Dwords that change on eight consecutive bursts are timer-like
    // and get muted so cut transitions stay readable.
    void ReachCineProbeDiffBank(
        char bank, const uint32_t* fresh, uint32_t* lastLogged,
        uint8_t* hotBursts, uint32_t& mutedMask, size_t count,
        unsigned& logged, unsigned& deferred)
    {
        for (size_t index = 0; index < count; ++index)
        {
            if (fresh[index] == lastLogged[index])
            {
                hotBursts[index] = 0;
                continue;
            }
            const uint32_t bit = 1u << index;
            if (mutedMask & bit)
            {
                lastLogged[index] = fresh[index];
                continue;
            }
            if (hotBursts[index] < 0xFF)
                ++hotBursts[index];
            if (hotBursts[index] >= 8)
            {
                mutedMask |= bit;
                lastLogged[index] = fresh[index];
                LOG("REACHCINE: %c+0x%02X changes every burst; muted "
                    "(timer-like)",
                    bank, static_cast<unsigned>(index * 4));
                continue;
            }
            if (logged >= 8)
            {
                ++deferred; // reported next burst
                continue;
            }
            LOG("REACHCINE: %c+0x%02X %08X -> %08X",
                bank, static_cast<unsigned>(index * 4),
                lastLogged[index], fresh[index]);
            lastLogged[index] = fresh[index];
            ++logged;
        }
    }

    // REACHHUD reporter. Runs on the 50 ms worker and states, in plain terms,
    // whether Reach is still drawing crosshair widgets, whether descriptors
    // went unreadable, and what the published art key is doing. One line per
    // state change; silent while healthy.
    void ReachChudDiagnosticTick(uint64_t now)
    {
        if (!g_reachCamera.armed.load(std::memory_order_acquire))
            return;
        static bool class2Present = true;
        static uint32_t reportedUnreadable = 0;
        static uint32_t reportedRejects = 0;
        static uint32_t reportedRedirect = 0;
        static uint64_t lastKeyLogMs = 0;
        static uint64_t loggedKey = 0;

        const uint64_t lastClass2 =
            g_reachChudLastClass2Ms.load(std::memory_order_relaxed);
        // Reach legitimately drops the crosshair briefly (reload, melee), and
        // the published art is deliberately held across those gaps. Two
        // seconds without a single class-2 draw is a different thing: the
        // engine stopped emitting the widget entirely, which is what the
        // player sees as "the crosshair disappeared". Wait for the first
        // sighting before claiming a drought - otherwise this reports one
        // 50 ms after arming, before Reach has drawn anything (observed).
        const bool present = lastClass2 == 0 ||
            (now >= lastClass2 && now - lastClass2 < 2000);
        if (present != class2Present)
        {
            class2Present = present;
            if (present)
                LOG("REACHHUD: crosshair widgets are drawing again "
                    "(%u class-2 draws total)",
                    g_reachChudClass2Draws.load(std::memory_order_relaxed));
            else
            {
                // The decisive line: if otherDraws keeps climbing across the
                // drought, Reach IS still drawing widgets and our class
                // resolution is what changed; if it is flat too, the engine
                // really stopped. The histogram says which class the
                // crosshair's collection now reports.
                LOG("REACHHUD: no class-2 crosshair widget drawn for 2s "
                    "(unreadable %u, rejects %u, readable-non-class-2 %u)",
                    g_reachChudUnreadable.load(std::memory_order_relaxed),
                    g_reachChudRejects.load(std::memory_order_relaxed),
                    g_reachChudOtherDraws.load(std::memory_order_relaxed));
                LOG("REACHHUD: class histogram 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u "
                    "6:%u 7:%u 8:%u other:%u",
                    g_reachChudClassCounts[0].load(std::memory_order_relaxed),
                    g_reachChudClassCounts[1].load(std::memory_order_relaxed),
                    g_reachChudClassCounts[2].load(std::memory_order_relaxed),
                    g_reachChudClassCounts[3].load(std::memory_order_relaxed),
                    g_reachChudClassCounts[4].load(std::memory_order_relaxed),
                    g_reachChudClassCounts[5].load(std::memory_order_relaxed),
                    g_reachChudClassCounts[6].load(std::memory_order_relaxed),
                    g_reachChudClassCounts[7].load(std::memory_order_relaxed),
                    g_reachChudClassCounts[8].load(std::memory_order_relaxed),
                    g_reachChudClassCounts[9].load(std::memory_order_relaxed));
            }
        }

        const uint32_t unreadable =
            g_reachChudUnreadable.load(std::memory_order_relaxed);
        if (unreadable != reportedUnreadable && now - lastKeyLogMs >= 2000)
        {
            reportedUnreadable = unreadable;
            lastKeyLogMs = now;
            LOG("REACHHUD: %u CHUD collection descriptors unreadable "
                "(last alternate-path flag %u) - those draws stayed stock",
                unreadable,
                g_reachChudLastUnreadablePath.load(std::memory_order_relaxed));
        }
        const uint32_t rejects =
            g_reachChudRejects.load(std::memory_order_relaxed);
        if (rejects != reportedRejects)
        {
            reportedRejects = rejects;
            LOG("REACHHUD: %u eye transactions rejected by the CHUD path",
                rejects);
        }
        const uint32_t redirect =
            g_reachChudRedirectUnavailable.load(std::memory_order_relaxed);
        if (redirect != reportedRedirect)
        {
            reportedRedirect = redirect;
            LOG("REACHHUD: crosshair redirect unavailable %u times - those "
                "eyes skipped publishing art; the camera core stayed armed",
                redirect);
        }
        const uint64_t key =
            g_reachAuthoredCrosshairKey.load(std::memory_order_relaxed);
        if (key != loggedKey)
        {
            loggedKey = key;
            LOG("REACHHUD: published crosshair art key -> %016llX",
                static_cast<unsigned long long>(key));
        }
    }

    // 50 ms worker: publish what the engine-thread sampler saw. All logging
    // lives here, off the render path, throttled to 250 ms bursts.
    void ReachCineProbeLogTick()
    {
        if (!g_reachCineProbe.armed.load(std::memory_order_acquire))
            return;
        static uint32_t seenReset = 0;
        static uint32_t lastSeqSeen = 0;
        static uint64_t lastSeqChangeMs = 0;
        static bool stallReported = false;
        static bool haveBaseline = false;
        static uint32_t lastLoggedA[kReachCineMemberADwords] = {};
        static uint32_t lastLoggedB[kReachCineMemberBDwords] = {};
        static uint8_t hotBurstsA[kReachCineMemberADwords] = {};
        static uint8_t hotBurstsB[kReachCineMemberBDwords] = {};
        static uint32_t mutedA = 0;
        static uint32_t mutedB = 0;
        static uint64_t lastBurstMs = 0;
        static uint32_t reportedFailures = 0;
        static uint64_t lastFailureReportMs = 0;

        const uint32_t reset =
            g_reachCineProbe.logReset.load(std::memory_order_acquire);
        if (reset != seenReset)
        {
            seenReset = reset;
            lastSeqSeen = 0;
            lastSeqChangeMs = 0;
            stallReported = false;
            haveBaseline = false;
            memset(hotBurstsA, 0, sizeof(hotBurstsA));
            memset(hotBurstsB, 0, sizeof(hotBurstsB));
            mutedA = 0;
            mutedB = 0;
            reportedFailures = 0;
        }

        const uint64_t now = GetTickCount64();
        const uint32_t failures =
            g_reachCineProbe.sampleFailures.load(std::memory_order_relaxed);
        if (failures != reportedFailures && now - lastFailureReportMs >= 5000)
        {
            reportedFailures = failures;
            lastFailureReportMs = now;
            LOG("REACHCINE: sampler failing (cumulative %u TLS/member read "
                "misses)", failures);
        }

        // Cutscene-facing confirmation, mirroring the Halo 3/ODST worker-side
        // report: one line per authored cut the render thread realigned to.
        static uint32_t lastCutCount = 0;
        const uint32_t cutCount =
            g_reachCineCutCount.load(std::memory_order_relaxed);
        if (cutCount != lastCutCount)
        {
            lastCutCount = cutCount;
            LOG("Reach cutscene facing: realigned to the authored camera "
                "(cut %u, %u of them from the camera-jump test)", cutCount,
                g_reachCineCutPoseCount.load(std::memory_order_relaxed));
        }
        ReachChudDiagnosticTick(now);

        uint32_t copyA[kReachCineMemberADwords];
        uint32_t copyB[kReachCineMemberBDwords];
        uint32_t stableSeq = 0;
        bool stable = false;
        for (int attempt = 0; attempt < 4 && !stable; ++attempt)
        {
            const uint32_t seqBefore =
                g_reachCineProbe.seq.load(std::memory_order_acquire);
            if (seqBefore & 1u)
                continue;
            memcpy(copyA, g_reachCineProbe.bufA, sizeof(copyA));
            memcpy(copyB, g_reachCineProbe.bufB, sizeof(copyB));
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint32_t seqAfter =
                g_reachCineProbe.seq.load(std::memory_order_acquire);
            stable = seqBefore == seqAfter;
            stableSeq = seqBefore;
        }
        if (!stable)
            return;

        if (stableSeq != lastSeqSeen)
        {
            lastSeqSeen = stableSeq;
            lastSeqChangeMs = now;
            stallReported = false;
        }
        else
        {
            if (stableSeq != 0 && !stallReported && lastSeqChangeMs &&
                now - lastSeqChangeMs > 3000)
            {
                stallReported = true;
                LOG("REACHCINE: sampler stalled >3s while armed - the owned "
                    "camera path is not running (cutscene or menu may bypass "
                    "the armed core)");
            }
            return;
        }

        if (!haveBaseline)
        {
            haveBaseline = true;
            memcpy(lastLoggedA, copyA, sizeof(lastLoggedA));
            memcpy(lastLoggedB, copyB, sizeof(lastLoggedB));
            LOG("REACHCINE: baseline A+00 %08X %08X %08X %08X %08X %08X %08X "
                "%08X",
                copyA[0], copyA[1], copyA[2], copyA[3], copyA[4], copyA[5],
                copyA[6], copyA[7]);
            LOG("REACHCINE: baseline A+20 %08X %08X %08X %08X %08X %08X %08X "
                "%08X",
                copyA[8], copyA[9], copyA[10], copyA[11], copyA[12], copyA[13],
                copyA[14], copyA[15]);
            LOG("REACHCINE: baseline B+00 %08X %08X %08X %08X",
                copyB[0], copyB[1], copyB[2], copyB[3]);
            return;
        }

        if (now - lastBurstMs < 250)
            return;
        unsigned logged = 0;
        unsigned deferred = 0;
        ReachCineProbeDiffBank('A', copyA, lastLoggedA, hotBurstsA, mutedA,
                               kReachCineMemberADwords, logged, deferred);
        ReachCineProbeDiffBank('B', copyB, lastLoggedB, hotBurstsB, mutedB,
                               kReachCineMemberBDwords, logged, deferred);
        if (deferred)
            LOG("REACHCINE: %u further changes deferred to next burst",
                deferred);
        if (logged || deferred)
            lastBurstMs = now;
    }

    void ReachCameraCore_Poll(
        uintptr_t base, size_t size, uint32_t generation, bool soleReachTitle)
    {
        ReachCineProbeColdPoll(base, size, generation, soleReachTitle);
        ReachPauseColdPoll(base, size, generation, soleReachTitle);
        ReachMuzzleRetargetTick(base, size, generation, soleReachTitle);
        ReachLightmapShadowLogTick();
        ReachSsaoLogTick();
        ReachObserverCameraLogTick();
        ReachMuzzleLogTick();
        ReachWorldWristWriteLogTick();
        ReachCineProbeLogTick();
        LogReachFpCameraUploadIfReady();
        LogReachFpStatusIfNew();
        const bool installed =
            g_reachCamera.installed.load(std::memory_order_acquire);
        if (installed && g_reachCamera.teardownRequested.load(
                std::memory_order_acquire))
        {
            RemoveReachCameraCore();
            return;
        }
        if (!soleReachTitle || !base || size != kReachRetailImageSize)
        {
            if (installed)
                RemoveReachCameraCore();
            return;
        }
        if (installed &&
            (base != g_reachCamera.base || generation != g_reachCamera.generation))
        {
            RemoveReachCameraCore();
            return;
        }
        if (g_vrRuntimeFailureLatched.load(std::memory_order_acquire))
        {
            g_reachCamera.armed.store(false, std::memory_order_release);
            return;
        }

        const ReachModuleEpoch epoch{base, generation};
        const ReachPreflightToken preflight =
            ReachRenderCandidate_GetPreflight(epoch);
        // Cold-prepare the authored-reticle swapchain/RTVs and the private
        // capture texture on this worker. Reach's hot CHUD capture entry
        // REFUSES to run without them (no lazy allocation in the hot hook),
        // and nothing else creates them for Reach: before the 2026-07-27
        // heartbeat, the shared snapshot spent most of its time unsettled, so
        // the capture entry constantly fell into the Halo 3/ODST lazy branch
        // by title misdetection and created the texture as a side effect.
        // Settling ownership removed that crutch, and Winter Contingency ran
        // its first 3.5 minutes with key-0 captures and no crosshair at all
        // (headset log 2026-07-27 00:43:58-00:47:39). Idempotent and cheap
        // once created. A Failed result never blocks the camera core - the
        // feature degrades alone, loudly, per the failure-isolation rule.
        if (VR_CanPrepareAuthoredReticleResources())
        {
            const AuthoredReticlePreparationResult reticlePreparation =
                VR_PrepareAuthoredReticleResources();
            static uint32_t preparedLogGeneration = 0;
            static uint32_t failedLogGeneration = 0;
            if (reticlePreparation ==
                    AuthoredReticlePreparationResult::Ready &&
                preparedLogGeneration != generation)
            {
                preparedLogGeneration = generation;
                LOG("Reach crosshair: authored capture resources "
                    "cold-prepared for generation %u", generation);
            }
            else if (reticlePreparation ==
                         AuthoredReticlePreparationResult::Failed &&
                     failedLogGeneration != generation)
            {
                failedLogGeneration = generation;
                LOG("Reach crosshair: authored capture resource preparation "
                    "FAILED; authored captures stay off this generation");
            }
        }
        const bool ready = preflight.Complete() &&
            ReachRenderCandidate_IsPreflightCurrent(preflight) &&
            VR_ReachDisplayReady(epoch) &&
            VR_CanPrepareAuthoredReticleResources();

        if (!installed)
        {
            if (ready)
                InstallReachCameraCore(base, size, generation);
            return;
        }
        if (!g_reachCamera.armed.load(std::memory_order_acquire) && ready &&
            GetTickCount64() - g_reachCamera.installedAtMs >=
                kReachRenderSafetyIntervalMs)
        {
            g_reachCamera.armed.store(true, std::memory_order_release);
            LOG("Reach camera core armed: proven five-hook per-eye stereo + "
                "camera/FP transaction is live; failed owned eyes "
                "are revoked and never published");
            if (g_reachCamera.lightmapShadowsEnabled)
            {
                LOG("Reach world-shadow candidate ARMED with zero samples: "
                    "render_lightmap_shadows will be suppressed per owned eye");
            }
            if (g_reachCamera.ssaoIsolationActive.load(
                    std::memory_order_acquire))
            {
                LOG("Reach SSAO candidate ARMED with zero suppressed-eye "
                    "samples; the first exact owned-eye execution will be "
                    "reported by the worker");
            }
        }
        // Parity with ODST: publish the lifecycle every tick so armed state and
        // capabilities stay current for shared features. Without this Reach's
        // arm-gated capabilities are masked off permanently.
        PublishReachLifecycle();
    }
#endif

    DWORD WINAPI WaitThread(LPVOID)
    {
        // The XInput hook is wanted as soon as MCC loads an xinput DLL (so the
        // Sense controllers drive the frontend menus too), the game hooks only
        // once halo3.dll appears (entering a level). MCC loads xinput DLLs
        // lazily and can add MORE of them later (it hooked only xinput1_3 in
        // one session and read the pad through xinput1_4), so this thread
        // keeps polling forever instead of stopping at the first success.
        bool gameHooked = false;
        bool hookRefreshPending = false;
        uintptr_t hookedBase = 0;
        uint32_t haloAttemptedGeneration = 0;
        uint64_t nextInputRefreshMs = 0;
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
        bool odstHooked = false;
        bool odstAttempted = false;
        uintptr_t odstCameraArrayRva = 0;
        uint64_t odstNextAttemptMs = 0;
        OdstCameraRearmGate odstRearmGate;
        OdstPauseRearmGate odstPauseRearmGate;
        bool odstPresentationPrepared = false;
#endif
        for (;;)
        {
            const uint64_t pollNow = GetTickCount64();
            const TitleDescriptor* activeTitle =
                TitleAdapter_PollLoaded(pollNow);
            const TitleAdapterRuntimeSnapshot runtime =
                RuntimeSnapshot(pollNow);
            // Draw-distance trim for whatever title currently owns the frame.
            // Game_ApplyDrawDistance caches by module, so this is a cheap
            // per-tick reassert of the user's scaled render far-clip (survives
            // level/tag loads), not a per-tick whole-module scan.
            if (activeTitle)
            {
                uintptr_t ddBase = 0;
                size_t ddSize = 0;
                if (sig::ModuleRange(activeTitle->moduleName, ddBase, ddSize))
                    Game_ApplyDrawDistance(
                        ddBase, ddSize,
                        TitleAdapter_GetGeneration(activeTitle->title));
            }
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
            {
                const bool soleReachTitle = activeTitle &&
                    activeTitle->title == GameTitle::HaloReach &&
                    TitleAdapter_GetActiveTitle() == GameTitle::HaloReach;
                uintptr_t reachBase = 0;
                size_t reachSize = 0;
                const uint32_t reachGeneration = soleReachTitle
                    ? TitleAdapter_GetGeneration(GameTitle::HaloReach)
                    : 0;
                const bool haveReachRange = soleReachTitle &&
                    sig::ModuleRange(
                        activeTitle->moduleName, reachBase, reachSize);
                ReachRenderCandidate_ColdPoll(
                    reachBase, reachSize, reachGeneration,
                    haveReachRange);
                VR_ReachRenderCandidate_ColdPoll();
                // Install/arm/remove the permanent Reach per-eye camera core.
                // It never installs until loaded-image preflight and VR
                // eye-capture proof pass, and never touches Halo 3/ODST.
                ReachCameraCore_Poll(
                    reachBase, reachSize, reachGeneration, haveReachRange);
            }
#endif
            // Snapshot resolution is deliberately side-effect free so render
            // and input callers cannot log. The worker owns fallback-mode
            // publication when no title render path is publishing a mode.
            TitleAdapter_SetRuntimeMode(runtime.runtime.mode);
            if (pollNow >= nextInputRefreshMs)
            {
                Input_InstallXInputHook();
                Input_ClaimXInputIat(); // re-assert if Steam replaces MCC's import slot
                nextInputRefreshMs = pollNow + 2000;
            }
            const TitleHookPlan hookPlan = TitleRegistry_HookPlan(
                activeTitle ? activeTitle->title : GameTitle::None);
            const bool haloAvailableForInstall =
                hookPlan == TitleHookPlan::Halo3Full;
            const uint32_t haloGeneration =
                g_halo3RuntimeGeneration.load(std::memory_order_acquire);
            const uint32_t observedHaloGeneration =
                TitleAdapter_GetGeneration(GameTitle::Halo3);
            const bool haloGenerationMismatch = gameHooked &&
                (!haloGeneration ||
                 haloGeneration != observedHaloGeneration);
            const bool haloRuntimeRetained = gameHooked && haloGeneration &&
                runtime.runtime.owner == GameTitle::Halo3 &&
                runtime.runtime.generation == haloGeneration &&
                (runtime.runtime.qualifyingOwnerCount == 1 ||
                 runtime.ownershipPending);
            const bool haloActive = !haloGenerationMismatch &&
                (haloAvailableForInstall || haloRuntimeRetained);
            if (gameHooked && !haloActive)
            {
                PublishHalo3Lifecycle(true, false, true);
                TitleAdapter_ClearHeartbeat(
                    GameTitle::Halo3, haloGeneration);
                g_halo3LastCamCopyMs.store(0, std::memory_order_release);
                // Stop new camera transactions immediately. Presentation
                // detaches on the render thread in Game_AutoVrTick.
                g_enabled.store(false, std::memory_order_release);
                g_autoVrOwned.store(false, std::memory_order_release);
                g_autoVrUserVeto.store(false, std::memory_order_release);
                g_halo3RuntimeGeneration.store(0, std::memory_order_release);
                haloAttemptedGeneration = 0;
                // MCC can unload and later map halo3.dll at the same address.
                // MinHook's bookkeeping survives while the new module bytes no
                // longer contain detours, so remember this title boundary.
                gameHooked = false;
                hookRefreshPending = true;
                g_hooked = false;
                // CamCopyHook can start executing again (a lingering detour,
                // or a fresh InstallHook() re-enabling it) before a resolve
                // pass republishes these pointers for the next halo3.dll
                // instance. -1 matches "not yet resolved" so
                // ApplyMotionBlurSetting stays a no-op across the gap instead
                // of dereferencing pointers into this now-inactive instance.
                g_motionBlurVarCount.store(-1, std::memory_order_release);
            }

#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
            const bool odstAvailableForInstall =
                hookPlan == TitleHookPlan::OdstExperimentalCameraCore;
            const uint32_t odstGeneration =
                g_odstRuntimeGeneration.load(std::memory_order_acquire);
            const uint32_t observedOdstGeneration =
                TitleAdapter_GetGeneration(GameTitle::Halo3ODST);
            const bool odstGenerationMismatch = odstHooked &&
                (!odstGeneration ||
                 odstGeneration != observedOdstGeneration);
            const bool odstRuntimeRetained = odstHooked && odstGeneration &&
                runtime.runtime.owner == GameTitle::Halo3ODST &&
                runtime.runtime.generation == odstGeneration &&
                (runtime.runtime.qualifyingOwnerCount == 1 ||
                 runtime.ownershipPending);
            const bool odstActive = !odstGenerationMismatch &&
                (odstAvailableForInstall || odstRuntimeRetained);
            if (odstActive && !odstPresentationPrepared)
            {
                // Prime the render-thread detach while ODST is still loading.
                // The proven Halo 3 one-second camera debounce can then begin
                // immediately when the camera hooks publish their heartbeat.
                OdstRequestPresentationDetach();
                odstPresentationPrepared = true;
                LOG("ODST camera presentation: detach primed during title load");
            }
            else if (!odstActive)
                odstPresentationPrepared = false;
            bool odstPaused = false;
            const bool odstPauseKnown = odstActive &&
                ReadOdstEnginePaused(odstPaused);
            if (odstHooked && odstPauseKnown && odstPaused &&
                !g_odstCamera.teardownRequested.load(
                    std::memory_order_acquire))
            {
                LOG("ODST pause boundary: native pause entered; removing "
                    "private camera hooks before any Save & Quit teardown");
                OdstRequestFallback(OdstFallbackReason::NativePause);
            }
            if (odstHooked && odstActive &&
                !g_odstCamera.teardownRequested.load(std::memory_order_acquire))
            {
                const uint64_t now = GetTickCount64();
                const uint64_t installedAt =
                    g_odstCamera.installedAtMs.load(std::memory_order_acquire);
                const uint64_t last =
                    g_odstLastCamCopyMs.load(std::memory_order_acquire);
                const bool sawCamera =
                    g_odstCamera.sawValidCamera.load(std::memory_order_acquire);
                const bool cameraReady = OdstCameraArraySupportsBringup(
                    g_odstCamera.gunCameraArray);
                g_odstCamera.cameraArrayReady.store(
                    cameraReady, std::memory_order_release);
                if (!g_odstCamera.armed.load(std::memory_order_acquire))
                    LogOdstWaitingReadinessIfChanged(
                        g_odstCamera.gunCameraArray);
                const OdstHeartbeatAction heartbeat = EvaluateOdstHeartbeat(
                    now, installedAt, last, sawCamera, cameraReady);
                if (heartbeat == OdstHeartbeatAction::LevelUnloaded)
                    OdstRequestFallback(OdstFallbackReason::LevelUnloaded);
                else if (heartbeat == OdstHeartbeatAction::NoFirstHeartbeat)
                    OdstRequestFallback(OdstFallbackReason::NoCameraHeartbeat);
            }
            const bool odstTeardown =
                g_odstCamera.teardownRequested.load(std::memory_order_acquire);
            if (odstHooked && (!odstActive || odstTeardown))
            {
                auto reason = static_cast<OdstFallbackReason>(
                    g_odstCamera.fallbackReason.load(std::memory_order_acquire));
                if (!odstActive && reason == OdstFallbackReason::None)
                {
                    OdstRequestFallback(OdstFallbackReason::TitleLeft);
                    reason = OdstFallbackReason::TitleLeft;
                }
                // Cross-title diagnostic (read-only): when ODST is torn down
                // because the title poll went ambiguous/Unknown (another Halo
                // module is merely resident) rather than an explicit pause/unload,
                // record whether ODST's OWN camera was still rendering at that
                // instant. A fresh heartbeat proves the teardown is premature --
                // ODST owns the frame and its hooks should be retained instead.
                if (!odstActive)
                {
                    const uint64_t lastCam =
                        g_odstLastCamCopyMs.load(std::memory_order_acquire);
                    const uint64_t age = (lastCam && pollNow >= lastCam)
                        ? pollNow - lastCam
                        : UINT64_MAX;
                    LOG("ODST cross-title diag: title poll ambiguous/Unknown, "
                        "tearing down (reason=%d) armed=%d; ODST camera heartbeat "
                        "age=%llu ms -- retention candidate = %s",
                        static_cast<int>(reason),
                        g_odstCamera.armed.load(std::memory_order_acquire) ? 1 : 0,
                        static_cast<unsigned long long>(age),
                        (age <= 300)
                            ? "YES (ODST still rendering -- premature teardown)"
                            : "NO (ODST heartbeat stale -- genuine title exit)");
                }
                const bool cameraReadyBeforeRemoval =
                    !odstGenerationMismatch && g_odstCamera.gunCameraArray &&
                    OdstCameraArraySupportsBringup(
                        g_odstCamera.gunCameraArray);
                if (RemoveOdstCameraCore())
                {
                    odstHooked = false;
                    if (reason == OdstFallbackReason::UnsupportedCameraMode)
                    {
                        // A menu is a temporary camera mode, not the end of the
                        // session. Blocking until title exit meant opening
                        // ODST's menu once permanently killed VR: presentation
                        // still flipped back to stereo when the menu closed,
                        // but the camera core stayed torn down behind it, so
                        // the player was left in a hollow 3D that never
                        // recovered without leaving the title.
                        //
                        // The original concern - that a menu can briefly
                        // resemble an unload/reload and must not be reinstalled
                        // behind - is handled by the same gate level
                        // transitions already use. It requires the camera to be
                        // observed NOT ready and then ready again, which only
                        // happens once the menu is gone and gameplay has
                        // resumed, so the core cannot come back while the menu
                        // is still up.
                        odstRearmGate.BlockUntilReload(
                            cameraReadyBeforeRemoval);
                        odstAttempted = true;
                        LOG("ODST camera rearm blocked until gameplay resumes "
                            "after unsupported/menu camera mode");
                    }
                    else if (reason == OdstFallbackReason::NativePause)
                    {
                        odstPauseRearmGate.Block();
                        odstAttempted = true;
                        LOG("ODST camera rearm blocked until native pause exits "
                            "and the live camera is stable");
                    }
                    else if (reason == OdstFallbackReason::LevelUnloaded)
                    {
                        odstRearmGate.BlockUntilReload(
                            cameraReadyBeforeRemoval);
                        odstAttempted = true;
                    }
                    else if (!odstActive)
                        odstAttempted = false;
                    else
                        odstAttempted = true;
                }
            }
            if (!odstActive && !odstHooked)
            {
                odstRearmGate.Observe(false, false);
                odstPauseRearmGate.Observe(
                    pollNow, false, false, false);
                g_odstNativePauseFlag.store(0, std::memory_order_release);
                odstAttempted = false;
                odstCameraArrayRva = 0;
                odstNextAttemptMs = 0;
                g_odstCamera.waitingLogged.store(
                    false, std::memory_order_release);
                g_odstCamera.cameraArrayReady.store(
                    false, std::memory_order_release);
                ClearOdstStaticPreflightCache();
            }
            else if (odstAvailableForInstall && !odstHooked &&
                     (odstRearmGate.IsBlocked() ||
                      odstPauseRearmGate.IsBlocked()))
            {
                const bool ready = ProbeOdstCameraReadiness(
                    activeTitle->moduleName, odstCameraArrayRva);
                odstRearmGate.Observe(true, ready);
                odstPauseRearmGate.Observe(
                    pollNow, true, odstPauseKnown && odstPaused, ready);
                if (odstRearmGate.CanAttemptInstall() &&
                    odstPauseRearmGate.CanAttemptInstall())
                {
                    odstAttempted = false;
                    odstNextAttemptMs = 0;
                    LOG("ODST verified rearm boundary observed; static preflight may retry");
                }
            }
#endif

            if (!gameHooked && haloAvailableForInstall
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
                && !odstHooked
#endif
                )
            {
                const uint32_t generation =
                    TitleAdapter_GetGeneration(GameTitle::Halo3);
                uintptr_t base = 0;
                size_t size = 0;
                if (generation && generation != haloAttemptedGeneration &&
                    sig::ModuleRange(activeTitle->moduleName, base, size))
                {
                    haloAttemptedGeneration = generation;
                    if (hookRefreshPending)
                    {
                        if (hookedBase == base)
                            RemoveInstalledGameHooks();
                        else
                            g_installedGameHookCount = 0;
                    }
                    LOG("%ls loaded at %p, size 0x%zX",
                        activeTitle->moduleName, (void*)base, size);
                    if (InstallHook(base, size, generation))
                    {
                        g_hooked = true;
                        gameHooked = true;
                        hookRefreshPending = false;
                        hookedBase = base;
                    }
                }
            }

#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
            if (!odstHooked && !odstAttempted &&
                odstAvailableForInstall && !gameHooked &&
                odstRearmGate.CanAttemptInstall() &&
                odstPauseRearmGate.CanAttemptInstall() &&
                pollNow >= odstNextAttemptMs)
            {
                uintptr_t base = 0;
                size_t size = 0;
                if (sig::ModuleRange(activeTitle->moduleName, base, size))
                {
                    const uint32_t generation =
                        TitleAdapter_GetGeneration(GameTitle::Halo3ODST);
                    const OdstInstallResult result =
                        InstallOdstCameraCore(base, size, generation);
                    if (result == OdstInstallResult::Installed)
                    {
                        odstHooked = true;
                        odstAttempted = true;
                        odstCameraArrayRva =
                            g_odstCamera.gunCameraArray - base;
                    }
                    else if (result == OdstInstallResult::CleanupPending)
                    {
                        odstHooked = true;
                        odstAttempted = true;
                        if (g_odstCamera.gunCameraArray >= base)
                            odstCameraArrayRva =
                                g_odstCamera.gunCameraArray - base;
                        LOG("ODST camera install rollback is pending verified cleanup");
                    }
                    else if (result == OdstInstallResult::Failed)
                    {
                        odstAttempted = true;
                        LOG("ODST camera bring-up blocked for this title session; "
                            "the stock renderer remains active");
                    }
                    else
                    {
                        odstNextAttemptMs = pollNow + 500;
                    }
                }
            }
            g_hooked.store(gameHooked || odstHooked, std::memory_order_release);
            LogOdstNonFpCameraIfNew();     // emit any non-FP (death/vehicle) cam
            LogOdstRenderSkipIfNew();      // emit why a frame stayed flat 2D
            LogOdstFpLayoutSelfCheckIfNew(); // emit FP weapon-layout self-check
            LogOdstNativeHudRouteOnce();    // bounded in-place CHUD route result
            VR_FramePacingWorkerPoll();
            Sleep(50);
#else
            VR_FramePacingWorkerPoll();
            Sleep(50);
#endif
        }
    }
}

void Game_ReadFramePerfCounters(GameFramePerfCounters& out)
{
    out = {};
    out.viewRenders = g_perfViewRenders.load(std::memory_order_relaxed);
    for (int eye = 0; eye < 3; ++eye)
    {
        out.fpPaletteRequests[eye] =
            g_perfFpPaletteRequests[eye].load(std::memory_order_relaxed);
        out.fpPaletteFullSolves[eye] =
            g_perfFpPaletteFullSolves[eye].load(std::memory_order_relaxed);
        out.fpPaletteCacheHits[eye] =
            g_perfFpPaletteCacheHits[eye].load(std::memory_order_relaxed);
        out.fpPaletteCacheStores[eye] =
            g_perfFpPaletteCacheStores[eye].load(std::memory_order_relaxed);
        out.fpPaletteCacheFull[eye] =
            g_perfFpPaletteCacheFull[eye].load(std::memory_order_relaxed);
    }
    out.zoomLogWrites = g_perfZoomLogWrites.load(std::memory_order_relaxed);
    out.viewRateLogWrites =
        g_perfViewRateLogWrites.load(std::memory_order_relaxed);
    out.paletteRateLogWrites =
        g_perfPaletteRateLogWrites.load(std::memory_order_relaxed);
    out.cameraRateLogWrites =
        g_perfCameraRateLogWrites.load(std::memory_order_relaxed);
    out.fpDriverRateLogWrites =
        g_perfFpDriverRateLogWrites.load(std::memory_order_relaxed);
}

void Game_Init()
{
    // Establish the atomic title policy before globally shared input detours can
    // receive their first call. The worker refreshes it throughout transitions.
    TitleAdapter_PollLoaded(GetTickCount64());
    // Claim MCC's controller path synchronously, before OpenXR startup blocks
    // on SteamVR. The worker keeps re-asserting it if Steam replaces the IAT.
    Input_InstallXInputHook();
    Input_ClaimXInputIat();
    CreateThread(nullptr, 0, WaitThread, nullptr, 0, nullptr);
}

bool Game_IsHooked() { return g_hooked; }
bool Game_IsHeadTracking() { return g_enabled.load(); }
bool Game_IsCameraOnlyBringup()
{
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    return OdstCameraOnlyContext();
#else
    return false;
#endif
}
// Whether the ACTIVE title actually has its authored-crosshair capture hooks
// installed. This is a live fact, not an assumption: if the capture is not
// installed the procedural reticle is the only crosshair and must stay
// visible, and if it IS installed the captured widget is the crosshair and the
// procedural one must stay invisible so it cannot erase the art.
//
// ODST reaches the capture through the shared Halo 3 path -
// OdstHudDrawWidgetHook calls HudDrawWidgetHook and OdstHudCrosshairVisibleHook
// calls HudCrosshairVisibleHook - so once InstallOdstCrosshairHider succeeds it
// captures exactly like Halo 3 does. It was previously excluded by
// Game_IsCameraOnlyBringup, which is why it painted the procedural reticle over
// art it had already captured.
bool Game_TitleCapturesAuthoredCrosshair()
{
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    if (TitleAdapter_GetActiveTitle() == GameTitle::HaloReach)
        return g_reachOrigHudDrawWidget != nullptr;
#endif
    return g_realHudDrawWidget != nullptr;
}


bool Game_OwnsReachAuthoredReticle()
{
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    if (TitleAdapter_GetActiveTitle() != GameTitle::HaloReach)
        return false;
    const uint32_t cameraGeneration =
        g_reachCamera.generation.load(std::memory_order_acquire);
    return cameraGeneration != 0 &&
        TitleAdapter_GetGeneration(GameTitle::HaloReach) ==
            cameraGeneration &&
        g_reachCamera.installed.load(std::memory_order_acquire) &&
        g_reachCamera.armed.load(std::memory_order_acquire) &&
        !g_reachCamera.teardownRequested.load(std::memory_order_acquire) &&
        g_enabled.load(std::memory_order_acquire) && VR_IsStereoEnabled();
#else
    return false;
#endif
}
uint64_t Game_GetReachAuthoredCrosshairKey()
{
    return g_reachAuthoredCrosshairKey.load(std::memory_order_acquire);
}
uint64_t Game_GetAuthoredCrosshairKey()
{
    return g_authoredCrosshairKey.load(std::memory_order_acquire);
}

void Game_ResetAuthoredCrosshairKey()
{
    g_authoredCrosshairKeyAccum = 0;
    g_authoredCrosshairKey.store(0, std::memory_order_release);
}



void Game_RejectReachAuthoredReticle(uint32_t expectedGeneration,
                                     const char* reason)
{
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    if (!expectedGeneration ||
        TitleAdapter_GetActiveTitle() != GameTitle::HaloReach ||
        TitleAdapter_GetGeneration(GameTitle::HaloReach) !=
            expectedGeneration ||
        !g_reachCamera.installed.load(std::memory_order_acquire))
    {
        return;
    }
    // This is the ONLY runtime path that disarms Reach and requests hook
    // removal. It used to do so silently, which made every Reach failure this
    // session unattributable: the log showed one good eye pass, then stereo
    // OFF, with nothing saying who decided that. Always name the caller.
    const bool alreadyRejected =
        g_reachChudParityFailedGeneration.exchange(
            expectedGeneration, std::memory_order_acq_rel) ==
        expectedGeneration;
    if (!alreadyRejected)
    {
        LOG("Reach VR transaction rejected: %s; disarming and requesting hook "
            "teardown for generation %u",
            reason ? reason : "unspecified",
            static_cast<unsigned>(expectedGeneration));
    }
    g_reachCamera.armed.store(false, std::memory_order_release);
    g_reachCamera.teardownRequested.store(true, std::memory_order_release);
#else
    (void)expectedGeneration;
    (void)reason;
#endif
}

bool Game_VrOwnsLookStick()
{
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    if (OdstVrOwnsLookStick(
            OdstCameraOnlyContext(),
            g_enabled.load(std::memory_order_acquire)))
    {
        return true;
    }
#endif
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    // Reach has no separate simulation-camera integration yet. Once its proven
    // stereo camera is armed, ApplyVrTurn owns yaw and the HMD owns pitch; raw
    // stock RX/RY would otherwise create a second, conflicting look transform.
    return TitleAdapter_GetActiveTitle() == GameTitle::HaloReach &&
        g_reachCamera.armed.load(std::memory_order_acquire) &&
        g_enabled.load(std::memory_order_acquire) &&
        g_vrAim.load(std::memory_order_acquire) &&
        VR_IsStereoEnabled();
#else
    return false;
#endif
}

bool Game_ProcessPresentationDetachRequest()
{
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    if (g_odstCamera.presentationDetachInProgress.test_and_set(
            std::memory_order_acq_rel))
        return false;
    const uint64_t completed =
        g_odstCamera.presentationDetachCompleted.load(
            std::memory_order_acquire);
    const uint64_t requested =
        g_odstCamera.presentationDetachRequested.load(
            std::memory_order_acquire);
    if (requested == completed)
    {
        g_odstCamera.presentationDetachInProgress.clear(
            std::memory_order_release);
        return false;
    }
    g_odstCamera.armed.store(false, std::memory_order_release);
    PublishOdstLifecycle();
    g_enabled.store(false, std::memory_order_release);
    g_autoVrOwned.store(false, std::memory_order_release);
    g_autoVrUserVeto.store(false, std::memory_order_release);
    // Keep requested != completed throughout synchronous render-thread
    // cleanup. If another request arrives during the detach, acknowledging
    // only this snapshot leaves the newer generation owned for the next pass.
    VR_DetachGamePresentation();
    g_odstCamera.presentationDetachCompleted.store(
        requested, std::memory_order_release);
    g_odstCamera.presentationDetachInProgress.clear(
        std::memory_order_release);
    return true;
#else
    return false;
#endif
}
bool Game_AllowsSharedGameplayFeatures()
{
    const GameTitle activeTitle = TitleAdapter_GetActiveTitle();
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    if (OdstCameraOnlyContext())
        return false;
#endif
    if (activeTitle == GameTitle::None || activeTitle == GameTitle::Halo3)
        return true;
    if (activeTitle != GameTitle::Unknown)
        return false;
    const TitleAdapterRuntimeSnapshot runtime =
        RuntimeSnapshot(GetTickCount64());
    return runtime.runtime.owner == GameTitle::Halo3 &&
        runtime.runtime.qualifyingOwnerCount == 1 &&
        runtime.runtime.installed && !runtime.runtime.teardownRequested;
}
bool Game_AllowsSharedControllerInput()
{
    const GameTitle activeTitle = TitleAdapter_GetActiveTitle();
    const TitleDescriptor* activeDescriptor = TitleRegistry_Find(activeTitle);
    const bool explicitTitleAllowsControllerInput = activeDescriptor &&
        (activeDescriptor->admissionCapabilities &
            TitleCapability_ControllerInput) != 0;
    const bool resolvedOwnerAllowsControllerInput =
        activeTitle == GameTitle::Unknown &&
        Game_HasTitleCapability(TitleCapability_ControllerInput);
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    const bool cameraOnlyOwned = OdstCameraOnlyContext();
    // Preserve the accepted cumulative frontend transport rule. MCC commonly
    // keeps several title DLLs resident in its shell, so raw availability is
    // Unknown and there is intentionally no camera owner/capability snapshot.
    // Ordinary virtual-pad input must remain usable there. Camera-only teardown
    // still wins unless a unique owner publishes ControllerInput, and explicit
    // unsupported titles remain fail-closed.
    const bool allowAmbiguousFrontend =
        activeTitle == GameTitle::Unknown;
#else
    const bool cameraOnlyOwned = false;
    const bool allowAmbiguousFrontend = false;
#endif
    return TitleRegistry_AllowsSharedControllerInput(
        activeTitle, resolvedOwnerAllowsControllerInput, cameraOnlyOwned,
        allowAmbiguousFrontend, explicitTitleAllowsControllerInput);
}
bool Game_HasTitleCapability(uint32_t requiredCapabilities)
{
    if (!requiredCapabilities ||
        (requiredCapabilities & ~kTitleRuntimeKnownCapabilities))
    {
        return false;
    }
    const TitleAdapterRuntimeSnapshot runtime =
        RuntimeSnapshot(GetTickCount64());
    const uint32_t enabled = TitleRuntimeMaskUnarmedCapabilities(
        runtime.runtime, kRuntimeCapabilitiesRequiringArm);
    return (enabled & requiredCapabilities) == requiredCapabilities;
}
bool Game_CanToggleImmersiveView()
{
    return Game_AllowsSharedGameplayFeatures() || Game_IsCameraOnlyBringup();
}
void Game_DetachForVrRuntimeFailure()
{
    // This is the same render-thread ownership transition used by normal title
    // unload/pause paths, reached only after OpenXR can no longer submit. Stop
    // every title from beginning new camera/stereo transactions before the VR
    // side releases retained presentation resources.
    g_vrRuntimeFailureLatched.store(true, std::memory_order_release);
    const GameTitle activeTitle = TitleAdapter_GetActiveTitle();
    const TitleAdapterRuntimeSnapshot runtime =
        RuntimeSnapshot(GetTickCount64());
    const bool haloOwned = activeTitle == GameTitle::Halo3 ||
        (activeTitle == GameTitle::Unknown &&
         runtime.runtime.owner == GameTitle::Halo3);
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    const bool odstOwned = activeTitle == GameTitle::Halo3ODST ||
        (activeTitle == GameTitle::Unknown &&
         runtime.runtime.owner == GameTitle::Halo3ODST);
    if (odstOwned)
    {
        g_odstCamera.armed.store(false, std::memory_order_release);
        PublishOdstLifecycle();
    }
#endif
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    const bool reachOwned = activeTitle == GameTitle::HaloReach ||
        (activeTitle == GameTitle::Unknown &&
         runtime.runtime.owner == GameTitle::HaloReach);
    if (reachOwned)
        g_reachCamera.armed.store(false, std::memory_order_release);
#endif
    g_enabled.store(false, std::memory_order_release);
    g_autoVrOwned.store(false, std::memory_order_release);
    g_autoVrUserVeto.store(true, std::memory_order_release);
    if (haloOwned)
        PublishHalo3Lifecycle(true, false, false);
    VR_DetachGamePresentation();
}
bool Game_HasAuthoritativePauseState()
{
    if (g_enginePauseValidated.load(std::memory_order_acquire))
        return true;
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    const GameTitle activeTitle = TitleAdapter_GetActiveTitle();
    const TitleAdapterRuntimeSnapshot runtime =
        RuntimeSnapshot(GetTickCount64());
    return (activeTitle == GameTitle::Halo3ODST ||
            (runtime.runtime.owner == GameTitle::Halo3ODST &&
             runtime.runtime.qualifyingOwnerCount == 1)) &&
        g_odstNativePauseFlag.load(std::memory_order_acquire) != 0;
#else
    return false;
#endif
}

// HUD layout (F1 menu): manual rescan + status. The scan normally starts itself
// whenever size, aspect, or curvature is non-stock and no slots are located.
void Game_LocateHudSafeFrames()
{
    HudLayoutProfile profile = HudLayoutProfile::None;
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    if (Game_IsCameraOnlyBringup())
        profile = HudLayoutProfile::Halo3ODST;
#endif
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    // Re-enabled 2026-07-27 with the missing resolution class. The earlier
    // no-effect result was measured while only Reach's "fullscreen wide"
    // record was ever written; the per-eye VR target is 3752x3828 (aspect
    // 0.98), which is not widescreen, so the engine was reading a record this
    // mod never touched. The adapter now carries the fullscreen-standard
    // record too.
    if (profile == HudLayoutProfile::None &&
        TitleAdapter_GetActiveTitle() == GameTitle::HaloReach)
        profile = HudLayoutProfile::HaloReach;
#endif
    if (profile == HudLayoutProfile::None &&
        Game_AllowsSharedGameplayFeatures())
        profile = HudLayoutProfile::Halo3;
    const HudLayoutAdapter* adapter = HudLayoutAdapterFor(profile);
    if (!adapter)
    {
        LOG("SAFEFRAME: manual rescan skipped; no title-owned HUD layout adapter");
        return;
    }
    LaunchSafeFrameScan(profile, "manual rescan from the menu");
}

void Game_GetHudSafeFrameStatus(int& matches, bool& scanning)
{
    const int c = g_safeFrameHitCount.load(std::memory_order_acquire);
    scanning = (c == -2) ||
        g_safeFrameScanInFlight.load(std::memory_order_acquire);
    matches = (c > 0) ? c : 0;
}

void Game_ToggleHeadTracking()
{
    if (!Game_CanToggleImmersiveView())
        return;
    const bool on = !g_enabled.load();
    g_enabled = on;
    if (!Game_IsCameraOnlyBringup())
        PublishHalo3Lifecycle(true, on, false);
    if (on)
        g_needRecenter = true;
    else
        g_autoVrUserVeto = true; // user turned VR off by hand; don't auto re-arm
    LOG("head tracking %s", on ? "ON" : "OFF");
}

// Called every frame from VR_OnPresent. Turns head tracking + stereo ON shortly
// after a level starts driving the camera, and back OFF when you return to the
// menu — so the mod behaves like a normal VR game (no F2/F11). Manual F2 off
// while in a level vetoes auto-arm until the next level load; F2/F11 still work.
void Game_AutoVrTick()
{
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    static OdstFreshCameraDebounce odstFreshDebounce;
    static bool wasOdstCameraContext = false;
    static bool wasOdstNativePause = false;
    static uint64_t lastPresentationDetachCompleted = 0;
    const bool odstCameraContext = Game_IsCameraOnlyBringup();
    if (odstCameraContext)
    {
        if (!wasOdstCameraContext)
        {
            odstFreshDebounce.Reset();
            if (OdstMustClearForeignPause(
                    true, VR_IsPausePresentationTarget(),
                    VR_IsPausePresentation()))
            {
                VR_RequestPausePresentation(false);
                LOG("ODST camera presentation: cleared foreign pause/head-lock "
                    "state at title entry");
            }
        }
        wasOdstCameraContext = true;
        // Issue #18: hold Halo's widescreen cinematic-FOV reduction at 0 while
        // ODST stereo is active, exactly as the Halo 3 present path does. The
        // policy no-ops until the var is resolved and self-restores when stereo
        // is off; it must run here because the shared call site below is behind
        // the ODST-false Game_AllowsSharedGameplayFeatures() gate.
        UpdateCinematicFovPolicy();
        const uint64_t now = GetTickCount64();
        const uint64_t last =
            g_odstLastCamCopyMs.load(std::memory_order_acquire);
        const bool cameraReady = g_odstCamera.cameraArrayReady.load(
            std::memory_order_acquire);
        const bool cameraFresh = cameraReady && last != 0 && now >= last &&
            now - last < kOdstCameraFreshMs;
        const uint64_t installedAt = g_odstCamera.installedAtMs.load(
            std::memory_order_acquire);
        const bool sawCamera = g_odstCamera.sawValidCamera.load(
            std::memory_order_acquire);
        const OdstHeartbeatAction heartbeat = EvaluateOdstHeartbeat(
            now, installedAt, last, sawCamera, cameraReady);
        const bool cameraLost = heartbeat != OdstHeartbeatAction::None;
        const bool inLevelStable =
            odstFreshDebounce.Update(now, cameraFresh);

        const GameTitle availableTitle = TitleAdapter_GetActiveTitle();
        const TitleAdapterRuntimeSnapshot titleRuntime = RuntimeSnapshot(now);
        const bool odstTitleActive =
            (availableTitle == GameTitle::Halo3ODST ||
             (titleRuntime.runtime.owner == GameTitle::Halo3ODST &&
              titleRuntime.runtime.qualifyingOwnerCount == 1)) &&
            !g_odstCamera.teardownRequested.load(std::memory_order_acquire);
        VR_SetScopeActive(false);
        // Cutscene-facing confirmation for ODST: OdstApplyHeadLook bumps the
        // shared rebase serial at each authored cut. Log the transition here (a
        // cold worker-side path), mirroring the Halo 3 report, so the headset
        // test can see each cut re-align the view.
        {
            static uint32_t odstLoggedCineSerial = 0;
            const uint32_t serial =
                g_cinematicRebaseSerial.load(std::memory_order_acquire);
            if (serial != odstLoggedCineSerial)
            {
                odstLoggedCineSerial = serial;
                const int32_t scene =
                    g_cinematicRebaseScene.load(std::memory_order_relaxed);
                const int32_t shot =
                    g_cinematicRebaseShot.load(std::memory_order_relaxed);
                if (scene >= 0 && shot >= 0)
                    LOG("cutscene facing: aligned to scene %d shot %d", scene, shot);
                else
                    LOG("cutscene facing: aligned to gameplay camera on exit");
            }
        }
        // Halo 3 latches aimSeen once its live camera hook publishes. Keep the
        // same ownership here; camera heartbeat teardown and title transitions
        // already clear it. Per-update clearing caused XInput aim to drop out
        // between ODST camera copies, especially throughout vehicle cameras.

        bool nativePaused = false;
        const bool nativePauseKnown = ReadOdstEnginePaused(nativePaused);
        if (nativePauseKnown && nativePaused && !wasOdstNativePause)
        {
            wasOdstNativePause = true;
            VR_RequestPausePresentation(true);
            LOG("ODST pause presentation: native pause entered, switching to 2D");
        }
        else if ((!nativePauseKnown || !nativePaused) && wasOdstNativePause)
        {
            wasOdstNativePause = false;
            VR_RequestPausePresentation(false);
            LOG("ODST pause presentation: native pause exited, restoring stereo target");
        }

        // Report ODST's live play state through the same shared runtime-mode
        // channel Halo 3 uses, so systems gated on the mode behave identically.
        // Controller vibration is delivered during stable gameplay (matching
        // Halo 3's headset-confirmed rumble) and stops during native pause or
        // while the level is still loading. This mirrors the Halo 3 setter
        // (RuntimeMode::Paused / Gameplay / Loading); ApplyControllerHaptics
        // still multiplies by the universal haptic_intensity, so the shared
        // config/F1 slider tunes ODST rumble strength with no per-title profile.
        if (odstTitleActive)
        {
            // Issue #18: once ODST has armed stereo on a stable level, keep
            // reporting Gameplay through brief camera-freshness dips instead of
            // dropping to Loading on the fragile per-frame inLevelStable signal.
            // Movement head-relativity (Game_MoveStickIsLocomotion) and controller
            // rumble (ApplyControllerHaptics) are both gated on this mode, while
            // stereo/head-look/aim ride the persistent armed flag -- so the old
            // gate let movement revert to hand-based and rumble cut out while the
            // view kept working. Tie the mode to the same armed/stereo persistence
            // so all three stay in lockstep. Native pause still maps to Paused;
            // genuine heartbeat loss, pause, or teardown disarm within this same
            // block, so the mode still downgrades on real transitions. Before the
            // first arm, armed is false, so this reports Loading.
            const bool odstStereoActive =
                g_enabled.load(std::memory_order_relaxed) &&
                g_odstCamera.armed.load(std::memory_order_relaxed);
            const RuntimeMode odstMode =
                (nativePauseKnown && nativePaused) ? RuntimeMode::Paused
                : (odstStereoActive ? RuntimeMode::Gameplay
                                    : RuntimeMode::Loading);
            const uint32_t generation =
                g_odstRuntimeGeneration.load(std::memory_order_acquire);
            if (generation)
            {
                TitleAdapter_PublishMode(
                    GameTitle::Halo3ODST, generation, odstMode);
            }
        }

        // Match Halo 3's live HUD-config timing: begin title-owned layout
        // discovery from the first eligible fresh camera heartbeat, without
        // waiting for the one-second stereo arm. The shared writer still
        // verifies ODST's exact adapter anchor before every foreign write.
        if (OdstHudLayoutEligible(
                odstTitleActive, odstCameraContext,
                g_odstCamera.installed.load(std::memory_order_acquire),
                cameraFresh,
                g_odstCamera.teardownRequested.load(
                    std::memory_order_acquire),
                nativePauseKnown && nativePaused))
            HudLayoutAutoTick(HudLayoutProfile::Halo3ODST);

        const bool detachedNow = Game_ProcessPresentationDetachRequest();
        const uint64_t detachCompleted =
            g_odstCamera.presentationDetachCompleted.load(
                std::memory_order_acquire);
        const bool observedCompletedDetach =
            detachCompleted != lastPresentationDetachCompleted;
        lastPresentationDetachCompleted = detachCompleted;
        if (detachedNow || observedCompletedDetach)
        {
            // A completed detach is a hard edge for this Present. Do not let a
            // debounce state retained across a rapid teardown/reinstall re-arm
            // stereo until a full new interval of fresh camera heartbeats.
            odstFreshDebounce.Reset();
            return;
        }

        if (nativePauseKnown && nativePaused)
        {
            // The worker removes every registered ODST hook at this boundary.
            // Disarm on the
            // render thread immediately so no stereo transaction can begin
            // while pause or Save & Quit advances title teardown.
            g_odstCamera.armed.store(false, std::memory_order_release);
            PublishOdstLifecycle();
            g_enabled.store(false, std::memory_order_release);
            g_autoVrOwned.store(false, std::memory_order_release);
            g_autoVrUserVeto.store(false, std::memory_order_release);
            if (VR_IsStereoEnabled())
                VR_DetachGamePresentation();
            odstFreshDebounce.Reset();
            return;
        }

        if (cameraLost &&
            (g_enabled.load(std::memory_order_relaxed) ||
             VR_IsStereoEnabled() || g_autoVrOwned.load(std::memory_order_relaxed)))
        {
            LOG("ODST camera presentation: verified heartbeat loss; "
                "detaching stereo while hook teardown completes");
            g_odstCamera.armed.store(false, std::memory_order_release);
            PublishOdstLifecycle();
            g_enabled.store(false, std::memory_order_release);
            g_autoVrOwned.store(false, std::memory_order_release);
            g_autoVrUserVeto.store(false, std::memory_order_release);
            VR_DetachGamePresentation();
        }

        if (!g_config.auto_vr)
        {
            if (g_autoVrOwned.load(std::memory_order_acquire))
            {
                g_odstCamera.armed.store(false, std::memory_order_release);
                PublishOdstLifecycle();
                g_enabled.store(false, std::memory_order_release);
                g_autoVrOwned.store(false, std::memory_order_release);
                g_autoVrUserVeto.store(false, std::memory_order_release);
                VR_DetachGamePresentation();
            }
            else
            {
                const bool wasArmed =
                    g_odstCamera.armed.load(std::memory_order_acquire);
                const bool eligible = OdstManualArmEligible(
                    inLevelStable,
                    g_enabled.load(std::memory_order_acquire),
                    VR_IsStereoEnabled(),
                    g_odstCamera.teardownRequested.load(
                        std::memory_order_acquire));
                // Pending may retain a transaction that was already armed,
                // but it cannot start a new one before the post-transition
                // camera heartbeat establishes real ownership.
                g_odstCamera.armed.store(
                    eligible &&
                        (wasArmed ||
                         (odstTitleActive &&
                          !titleRuntime.ownershipPending)),
                    std::memory_order_release);
                PublishOdstLifecycle();
            }
            return;
        }
        if (inLevelStable)
        {
            if (odstTitleActive && !titleRuntime.ownershipPending &&
                !g_autoVrUserVeto.load(std::memory_order_relaxed) &&
                !g_odstCamera.teardownRequested.load(std::memory_order_acquire) &&
                (!g_enabled.load(std::memory_order_relaxed) ||
                 !VR_IsStereoEnabled() ||
                 !g_odstCamera.armed.load(std::memory_order_relaxed)))
            {
                g_enabled.store(true, std::memory_order_release);
                g_needRecenter.store(true, std::memory_order_release);
                if (!VR_IsStereoEnabled())
                    VR_ToggleStereo();
                g_autoVrOwned.store(true, std::memory_order_release);
                g_odstCamera.armed.store(true, std::memory_order_release);
                PublishOdstLifecycle();
                LOG("ODST camera bring-up: stable stock camera detected; "
                    "head tracking, stereo, and 6DOF ON");
            }
        }
        else if (cameraLost)
        {
            g_autoVrUserVeto.store(false, std::memory_order_release);
        }
        return;
    }
    if (wasOdstCameraContext)
    {
        wasOdstCameraContext = false;
        if (wasOdstNativePause)
        {
            wasOdstNativePause = false;
            VR_RequestPausePresentation(false);
        }
        odstFreshDebounce.Reset();
        InvalidateHudLayoutProfile(HudLayoutProfile::Halo3ODST);
    }
#endif

#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    static bool wasReachHudContext = false;
    static bool wasReachNativePause = false;
    if (TitleAdapter_GetActiveTitle() == GameTitle::HaloReach)
    {
        wasReachHudContext = true;
        // Reach owns head tracking + stereo whenever its per-eye camera core is
        // armed. Enabling them lets the present path composite the two eye
        // caches the inner detour captured, and publishing Gameplay drives the
        // shared locomotion and haptics paths. Disarm mirrors ODST: drop
        // head tracking and detach so nothing composites stale eyes.
        if (g_reachCamera.armed.load(std::memory_order_acquire))
        {
            if (!g_enabled.load(std::memory_order_relaxed) ||
                !VR_IsStereoEnabled())
            {
                g_enabled.store(true, std::memory_order_release);
                if (!VR_IsStereoEnabled())
                    VR_ToggleStereo();
                LOG("Reach camera bring-up: head tracking, stereo, and 6DOF ON");
            }
            // Native pause, matching Halo 3's behavior rather than ODST's.
            // Halo 3 switches presentation to head-locked 2D and keeps its
            // camera core armed; ODST tears the whole core down for Save & Quit
            // safety, which is exactly what produced its slow-rearm defect. The
            // player asked for "flat head-locked view on pause, stereo back on
            // unpause", so Reach follows Halo 3. If the flag was never located
            // this reports unknown and Reach behaves exactly as it did before.
            bool reachPaused = false;
            const bool reachPauseKnown = ReadReachEnginePaused(reachPaused);
            g_reachEnginePauseCache.store(
                reachPauseKnown ? (reachPaused ? 1 : 0) : -1,
                std::memory_order_release);
            if (reachPauseKnown && reachPaused && !wasReachNativePause)
            {
                wasReachNativePause = true;
                VR_RequestPausePresentation(true);
                LOG("Reach pause presentation: native pause entered, "
                    "switching to head-locked 2D");
            }
            else if ((!reachPauseKnown || !reachPaused) && wasReachNativePause)
            {
                wasReachNativePause = false;
                VR_RequestPausePresentation(false);
                LOG("Reach pause presentation: native pause exited, "
                    "restoring stereo 3D");
            }
            const uint32_t reachGen =
                TitleAdapter_GetGeneration(GameTitle::HaloReach);
            if (reachGen)
                TitleAdapter_PublishMode(
                    GameTitle::HaloReach, reachGen,
                    (reachPauseKnown && reachPaused) ? RuntimeMode::Paused
                                                     : RuntimeMode::Gameplay);
            // Same shared HUD behavior Halo 3 and ODST get, against Reach's own
            // record. An armed per-eye core is Reach's liveness proof; a failed
            // or ambiguous locate leaves Reach's HUD wholly stock and says so.
            if (!g_reachCamera.teardownRequested.load(
                    std::memory_order_acquire))
            {
                const uint64_t reachNowMs = GetTickCount64();
                g_reachLastCamCopyMs.store(
                    reachNowMs, std::memory_order_release);
                // Reach's camera-liveness heartbeat, the homolog of Halo 3's
                // CamCopyHook and ODST's cam-copy publications. Without it the
                // shared resolver never qualifies Reach as owner (its policy
                // window used to be zero as well), so the Haptics capability
                // was permanently denied for Reach - captured rumble requests
                // were discarded - and the 50 ms worker kept re-publishing the
                // fallback Loading mode over the present path's Gameplay.
                // Stopping on teardown/disarm expires ownership within the
                // 500 ms freshness window, which also drops the arm-gated
                // capabilities and stops rumble, matching the other titles.
                if (reachGen)
                    TitleAdapter_PublishHeartbeat(
                        GameTitle::HaloReach, reachGen, reachNowMs);
                HudLayoutAutoTick(HudLayoutProfile::HaloReach);
            }
        }
        else
        {
            // Disarmed: a pause override must not outlive the camera core, or
            // the next arm comes back to a stranded 2D presentation.
            g_reachEnginePauseCache.store(-1, std::memory_order_release);
            if (wasReachNativePause)
            {
                wasReachNativePause = false;
                VR_RequestPausePresentation(false);
                LOG("Reach pause presentation: camera core disarmed, "
                    "clearing 2D pause override");
            }
            if (g_enabled.load(std::memory_order_relaxed) ||
                VR_IsStereoEnabled())
            {
                g_enabled.store(false, std::memory_order_release);
                VR_DetachGamePresentation();
            }
        }
        return;
    }
    if (wasReachHudContext)
    {
        wasReachHudContext = false;
        g_reachLastCamCopyMs.store(0, std::memory_order_release);
        g_reachEnginePauseCache.store(-1, std::memory_order_release);
        // Leaving Reach through its own pause menu must not strand the next
        // title in the 2D override - the same rule Halo 3 applies at its
        // "title left, clearing 2D pause override" transition.
        if (wasReachNativePause)
        {
            wasReachNativePause = false;
            VR_RequestPausePresentation(false);
            LOG("Reach pause presentation: title left, "
                "clearing 2D pause override");
        }
        InvalidateHudLayoutProfile(HudLayoutProfile::HaloReach);
    }
#endif

    if (!Game_AllowsSharedGameplayFeatures())
    {
        const TitleAdapterRuntimeSnapshot transition =
            RuntimeSnapshot(GetTickCount64());
        if (TitleAdapter_GetActiveTitle() == GameTitle::Unknown &&
            transition.ownershipPending &&
            transition.runtime.owner == GameTitle::Halo3 &&
            transition.runtime.installed &&
            !transition.runtime.teardownRequested)
        {
            // The bounded pending state retains an already-running transaction
            // only long enough for its next camera copy. It grants no input,
            // HUD, aim, haptics, or new arming work.
            return;
        }
        InvalidateHudLayoutProfile(HudLayoutProfile::Halo3);
        if (g_enabled.load(std::memory_order_relaxed) ||
            VR_IsStereoEnabled() || g_autoVrOwned.load(std::memory_order_relaxed))
        {
            g_enabled.store(false, std::memory_order_release);
            PublishHalo3Lifecycle(true, false, false);
            g_autoVrOwned.store(false, std::memory_order_release);
            g_autoVrUserVeto.store(false, std::memory_order_release);
            VR_DetachGamePresentation();
        }
        return;
    }

    // A same-title unload/reload can complete between two Present calls. Do
    // not inherit the prior instance's arm or fresh-camera debounce: detach on
    // this render thread and require a full new one-second stable interval.
    static uint32_t haloFreshGeneration = 0;
    static uint64_t freshSince = 0;
    const uint32_t currentHaloGeneration =
        g_halo3RuntimeGeneration.load(std::memory_order_acquire);
    if (currentHaloGeneration != haloFreshGeneration)
    {
        haloFreshGeneration = currentHaloGeneration;
        freshSince = 0;
        const bool presentationWasActive =
            g_enabled.exchange(false, std::memory_order_acq_rel) ||
            VR_IsStereoEnabled() ||
            g_autoVrOwned.load(std::memory_order_acquire);
        g_autoVrOwned.store(false, std::memory_order_release);
        g_autoVrUserVeto.store(false, std::memory_order_release);
        if (currentHaloGeneration)
            PublishHalo3Lifecycle(true, false, false);
        if (presentationWasActive)
            VR_DetachGamePresentation();
    }

    UpdateCinematicFovPolicy();
    HudLayoutAutoTick(HudLayoutProfile::Halo3); // shared behavior, H3 tag adapter
    {
        static uint32_t loggedSerial = 0;
        const uint32_t serial =
            g_cinematicRebaseSerial.load(std::memory_order_acquire);
        if (serial != loggedSerial)
        {
            loggedSerial = serial;
            const int32_t scene =
                g_cinematicRebaseScene.load(std::memory_order_relaxed);
            const int32_t shot =
                g_cinematicRebaseShot.load(std::memory_order_relaxed);
            if (scene >= 0 && shot >= 0)
                LOG("cutscene facing: aligned to scene %d shot %d",
                    scene, shot);
            else
                LOG("cutscene facing: aligned to gameplay camera on exit");
        }
    }
    // Render-thread diagnostics are reported here, on Present. Log only a
    // stable state transition; never log from the palette or HUD hot hooks.
    {
        static int loggedSide=-2;
        static const char* loggedWhy=reinterpret_cast<const char*>(1);
        static uint64_t lastLogMs=0;
        const int side=g_armFailureSide.load(std::memory_order_acquire);
        const char* why=g_armFailurePublished.load(std::memory_order_relaxed);
        const uint64_t diagNow=GetTickCount64();
        if ((side!=loggedSide || why!=loggedWhy) && diagNow-lastLogMs>=500)
        {
            loggedSide=side; loggedWhy=why; lastLogMs=diagNow;
            if (side==0)
                LOG("M3 VRIK SAFE-DIAG: both arms applied to controllers");
            else if (side==1)
                LOG("M3 VRIK SAFE-DIAG: right-arm solve fell back (%s); authored "
                    "support hand remains on weapon",why?why:"pre-solve");
            else if (side==2)
                LOG("M3 VRIK SAFE-DIAG: left arm not applied (%s)",
                    why?why:"pre-solve");
        }
    }
    {
        static int loggedCount=0;
        int available=g_fpBoneMapSnapshotCount.load(std::memory_order_acquire);
        if(available>16) available=16;
        while(loggedCount<available)
        {
            auto& snap=g_fpBoneMapSnapshots[loggedCount];
            const uint32_t begin=snap.sequence.load(std::memory_order_acquire);
            if((begin&1) || begin==0) break;
            const uint64_t key=snap.skeletonKey.load(std::memory_order_relaxed);
            const uint32_t tag=snap.tag.load(std::memory_order_relaxed);
            const int count=snap.count.load(std::memory_order_relaxed);
            const int reconstructed=snap.reconstructed.load(std::memory_order_relaxed);
            int32_t map[64]{};
            for(int i=0;i<count && i<64;++i)
                map[i]=snap.map[i].load(std::memory_order_relaxed);
            const uint32_t end=snap.sequence.load(std::memory_order_acquire);
            if(begin!=end) break;
            int shoulderDest=-1,elbowDest=-1,wristDest=-1;
            for(int i=0;i<count && i<64;++i)
            {
                if(map[i]==1) shoulderDest=i;
                if(map[i]==3) elbowDest=i;
                if(map[i]==5) wristDest=i;
            }
            LOG("M3 VRIK PALETTE #%d: skeleton %016llX tag 0x%04X "
                "reconstructed=%d count=%d; left 1/3/5 -> %d/%d/%d",
                loggedCount,static_cast<unsigned long long>(key),tag,
                reconstructed,count,shoulderDest,elbowDest,wristDest);
            for(int from=0;from<count;from+=16)
            {
                char line[512]; int pos=0;
                const int to=(from+16<count)?from+16:count;
                for(int i=from;i<to && pos<(int)sizeof(line)-24;++i)
                    pos+=snprintf(line+pos,sizeof(line)-pos,"%d=%d ",i,map[i]);
                LOG("M3 VRIK PALETTE #%d MAP[%d..%d]: %s",
                    loggedCount,from,to-1,line);
            }
            ++loggedCount;
        }
    }
    const uint64_t now = GetTickCount64();
    const uint64_t last =
        g_halo3LastCamCopyMs.load(std::memory_order_relaxed);
    const bool cameraFresh = last != 0 && now >= last &&
        (now - last) < 500; // camera driving now
    const bool cameraStale = last == 0 || now < last ||
        (now - last) > 2000; // menu / loading

    // Debounce entry: require the camera to have been fresh continuously for a
    // short spell before arming, so a single stray frame doesn't flip us.
    if (cameraFresh) { if (freshSince == 0) freshSince = now; }
    else freshSince = 0;
    const bool inLevelStable = freshSince != 0 && (now - freshSince) > 1000;

    const GameTitle availableTitle = TitleAdapter_GetActiveTitle();
    const TitleAdapterRuntimeSnapshot titleRuntime = RuntimeSnapshot(now);
    const bool haloTitleActive = availableTitle == GameTitle::Halo3 ||
        titleRuntime.runtime.owner == GameTitle::Halo3;
    const bool pausePresentation = VR_IsPausePresentation();
    bool enginePaused = false;
    static bool previousEnginePaused = false;
    static bool enginePauseLogged = false;
    static uint64_t pauseMismatchSince = 0;
    static bool pauseMismatchValue = false;
    if (ReadEnginePaused(enginePaused))
    {
        if (!enginePauseLogged || enginePaused != previousEnginePaused)
        {
            LOG("pause state: native engine flag=%d, presentation target=%d",
                enginePaused ? 1 : 0,
                VR_IsPausePresentationTarget() ? 1 : 0);
            previousEnginePaused = enginePaused;
            enginePauseLogged = true;
        }
        const bool targetPaused = VR_IsPausePresentationTarget();
        if (g_enginePauseValidated.load() && enginePaused != targetPaused)
        {
            if (pauseMismatchSince == 0 || pauseMismatchValue != enginePaused)
            {
                pauseMismatchSince = now;
                pauseMismatchValue = enginePaused;
            }
            else if (now - pauseMismatchSince >= 50)
            {
                VR_RequestPausePresentation(enginePaused);
                LOG("pause state: authoritative engine value corrected "
                    "presentation to %s",
                    enginePaused ? "head-locked 2D" : "stereo 3D");
                pauseMismatchSince = 0;
            }
        }
        else
            pauseMismatchSince = 0;
    }
    static PauseLevelRecovery pauseLevelRecovery;
    if (!g_enginePauseValidated.load() &&
        pauseLevelRecovery.Update(pausePresentation, cameraStale, inLevelStable))
    {
        // Restart Level leaves Halo's native pause screen without producing a
        // second Start edge. Re-enter stereo only after the replacement
        // level's camera has been stable for the normal debounce interval.
        VR_RequestPausePresentation(false);
        LOG("pause transition: restarted level is stable, restoring stereo 3D");
    }
    static bool pauseExitClearRequested = false;
    if (pausePresentation &&
        !haloTitleActive)
    {
        // Leaving the title through Halo's pause menu must not strand the next
        // level in the pause override. This changes presentation only; it does
        // not inject another Start press into the MCC shell.
        if (!pauseExitClearRequested)
        {
            pauseExitClearRequested = true;
            VR_RequestPausePresentation(false);
            LOG("pause transition: title left, clearing 2D pause override");
        }
    }
    else
        pauseExitClearRequested = false;
    if (haloTitleActive)
    {
        const uint32_t generation =
            g_halo3RuntimeGeneration.load(std::memory_order_acquire);
        if (generation)
        {
            TitleAdapter_PublishMode(
                GameTitle::Halo3, generation,
                pausePresentation ? RuntimeMode::Paused
                    : (inLevelStable ? RuntimeMode::Gameplay
                                     : RuntimeMode::Loading));
        }
    }

    // MCC keeps several game DLLs resident, so module presence cannot identify
    // the active renderer. The camera hook is the proven ownership signal.
    // Once its heartbeat is absent for 500 ms, disarm per-eye rendering and
    // release Halo's retained scene target before another engine takes over.
    // Pause is exempt because its stable 2D presentation is already detached.
    if (!pausePresentation && !cameraFresh &&
        (g_enabled.load() || VR_IsStereoEnabled() || g_autoVrOwned.load()))
    {
        g_enabled = false;
        PublishHalo3Lifecycle(true, false, false);
        g_autoVrOwned = false;
        g_autoVrUserVeto = false;
        VR_DetachGamePresentation();
    }

    if (!g_config.auto_vr || pausePresentation) return;

    if (inLevelStable)
    {
        if (!g_enabled.load() && !g_autoVrUserVeto.load())
        {
            g_enabled = true;
            PublishHalo3Lifecycle(true, true, false);
            g_needRecenter = true;
            if (!VR_IsStereoEnabled()) VR_ToggleStereo();
            g_autoVrOwned = true;
            LOG("auto-VR: level detected — head tracking + stereo ON");
        }
    }
    else if (cameraStale)
    {
        g_autoVrUserVeto = false; // reset veto on leaving the level
        if (g_autoVrOwned.load() && g_enabled.load())
        {
            g_enabled = false;
            PublishHalo3Lifecycle(true, false, false);
            if (VR_IsStereoEnabled()) VR_ToggleStereo();
            g_autoVrOwned = false;
            LOG("auto-VR: left the level — back to the flat menu screen");
        }
    }
}

void Game_Recenter()
{
    // One public recenter action owns both references: Halo's camera/position
    // origin and the OpenXR head-locked screen origin. This keeps keyboard F3,
    // the F1 button, and transition-triggered recentering behavior identical.
    g_needRecenter = true;
    VR_RequestRecenter();
}
void Game_FlipYaw()   { g_yawSign = -g_yawSign.load();   LOG("yaw sign %+.0f", g_yawSign.load()); }
void Game_FlipPitch() { g_pitchSign = -g_pitchSign.load(); LOG("pitch sign %+.0f", g_pitchSign.load()); }
void Game_ToggleUp()  { g_writeUp = !g_writeUp.load();   LOG("write up-vector %s", g_writeUp.load() ? "on" : "off"); }
float Game_GetYawSign()   { return g_yawSign.load(); }
float Game_GetPitchSign() { return g_pitchSign.load(); }
bool Game_GetWriteUp()    { return g_writeUp.load(); }

void Game_TogglePositional()
{
    if (VR_IsStereoEnabled())
    {
        g_positional = true;
        LOG("positional remains ON (required for stereo VR)");
        return;
    }
    const bool on = !g_positional.load();
    g_positional = on;
    if (on)
        g_needPosRecenter = true; // capture neutral head position, no yaw snap
    LOG("positional (leaning) %s", on ? "ON" : "OFF");
}

void Game_ForcePositional()
{
    g_positional = true;
    g_needPosRecenter = true;
    LOG("positional 6DOF forced ON for stereo VR");
}

void Game_PitchTrim(int dir)
{
    const float t = Clamp(g_pitchTrim.load() + dir * 0.035f, -0.8f, 0.8f); // ~2 deg steps
    g_pitchTrim = t;
    LOG("pitch trim %.1f deg", t * 57.2958f);
}

void Game_LeanScale(int dir)
{
    const float s = Clamp(g_worldScale.load() + dir * 0.05f, 0.05f, 2.0f);
    g_worldScale = s;
    LOG("lean scale %.2f (game units per meter)", s);
}

void Game_ToggleVrAim()
{
    const bool on = !g_vrAim.load();
    g_vrAim = on;
    LOG("VR aim (right controller steers weapon) %s", on ? "ON" : "OFF");
}

// Reach reuses the shared closed-loop aim steering, but publishes no runtime
// capability set (no Reach lifecycle), so TitleCapability_ControllerAim is
// never enabled for it. Mirror the Reach-scoped predicate used by
// Game_VrOwnsLookStick / Game_MoveStickIsLocomotion: while Reach's proven
// stereo camera is armed and head tracking + VR aim are on, its aim may be
// steered exactly like Halo 3's. Halo 3 / ODST are unaffected (title-gated).
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
static bool ReachControllerAimActive()
{
    return TitleAdapter_GetActiveTitle() == GameTitle::HaloReach &&
        g_reachCamera.armed.load(std::memory_order_acquire) &&
        g_enabled.load(std::memory_order_acquire) &&
        g_vrAim.load(std::memory_order_acquire) &&
        VR_IsStereoEnabled();
}
#else
static bool ReachControllerAimActive() { return false; }
#endif

bool Game_ComputeAimStick(float& outRx, float& outRy)
{
    if (!Game_HasTitleCapability(TitleCapability_ControllerAim) &&
        !ReachControllerAimActive())
        return false;
    // Closed-loop aim: emit a right-stick deflection proportional to the
    // angular error between the game's aim and the controller ray. The game
    // integrates it through its normal turn-rate path, so bullets, reticle
    // logic, vehicles and turrets all behave as if the player aimed manually.
    // Diagnostic: when aim steering is not running, log WHICH precondition
    // failed (once per distinct reason) so a dead aim is explainable from the
    // log alone.
    static std::atomic<int> lastAimBlock{-1};
    auto blocked = [](int reason, const char* what) {
        int prev = lastAimBlock.exchange(reason);
        if (prev != reason)
            LOG("M3 DIAG: aim steering blocked: %s", what);
        return false;
    };
    if (VR_IsPausePresentation())
        return blocked(6, "Halo pause presentation active");
    if (!g_vrAim.load())
        return blocked(1, "VR aim toggled OFF (press Insert)");
    if (!g_enabled.load())
        return blocked(2, "head tracking OFF (press F2)");
    if (!g_aimSeen.load())
        return blocked(3, "camera hook not running (not in a level?)");
    float q[4], p[3];
    if (!VR_GetAimPose(q, p)) // two-hand-adjusted weapon aim (falls back to right hand)
        return blocked(4, "right controller not tracked");
    float hq[4], hp[3];
    if (!VR_GetHeadPose(hq, hp))
        return blocked(5, "headset not tracked");
    lastAimBlock = 0;

    // Controller forward from the shared, mount-calibrated aim pose. The
    // visible weapon, muzzle, authored reticle and bullets all consume this
    // same corrected controller-local ray.
    const float localDir[3] = {0.0f, 0.0f, -1.0f};
    float f3[3];
    RotateByQuat(q, localDir, f3);
    const float fx = f3[0], fy = f3[1], fz = f3[2];

    // Halo spawns first-person projectiles at the CAMERA — the head — and no
    // steering can move that origin. Aiming the bullet ray PARALLEL to the
    // hand ray therefore leaves a permanent head-to-hand parallax miss (the
    // 07-15 report "bullets shoot from my head"). Instead steer the head-
    // origin ray through the point the hand ray reaches at the crosshair
    // distance: every shot then passes exactly through the floating reticle,
    // and beyond it the two rays are effectively identical.
    const float d = Clamp(g_config.crosshair_distance_m, 2.0f, 50.0f);
    float tx = p[0] + fx * d - hp[0];
    float ty = p[1] + fy * d - hp[1];
    float tz = p[2] + fz * d - hp[2];
    const float tl = sqrtf(tx * tx + ty * ty + tz * tz);
    if (tl > 1e-3f) { tx /= tl; ty /= tl; tz /= tl; }
    const float cy = atan2f(tx, -tz);
    const float cp = asinf(Clamp(ty, -1.0f, 1.0f));
    const float desiredYaw = g_gameYawRef + g_yawSign.load() * WrapPi(cy - g_headYawRef);
    const float desiredPitch = Clamp(g_pitchSign.load() * cp, -1.45f, 1.45f);

    const float ax = g_aimFwdX.load(), ay = g_aimFwdY.load(), az = g_aimFwdZ.load();
    const float aimYaw = atan2f(ay, ax);
    const float aimPitch = asinf(Clamp(az, -1.0f, 1.0f));

    const float errYaw = WrapPi(desiredYaw - aimYaw);
    const float errPitch = desiredPitch - aimPitch;

    // Full deflection at ~4.8 deg of error (was ~10; user: vertical follow too
    // slow). The ceiling is the game's own turn rate — raising in-game look
    // sensitivity raises it further.
    const float k = 12.0f;
    outRx = Clamp(-errYaw * k, -1.0f, 1.0f);
    outRy = Clamp(errPitch * k, -1.0f, 1.0f);
    return true;
}

void Game_MapMoveStick(float& mx, float& my)
{
    if (TitleAdapter_GetActiveTitle() == GameTitle::HaloReach)
    {
        // Reach has no controller aim: its stock movement heading is frozen (the
        // right stick is suppressed and nothing steers the sim camera), while the
        // render view follows the head + VR turn. Rotate the move vector by
        // (gaze - stock heading) so forward walks where you look. This mirrors the
        // Halo 3 rotation below with aimYaw sourced from the published stock
        // camera facing instead of g_aimFwd. Self-neutralizing: once a real Reach
        // body-heading integration lands and the stock heading tracks gaze, the
        // delta goes to zero and this becomes a no-op.
        if (!g_enabled.load() || !g_vrAim.load() ||
            !g_reachMoveHeadingValid.load(std::memory_order_acquire))
            return;
        float q[4], p[3];
        if (!VR_GetHeadPose(q, p))
            return;
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float fx = -2.0f * (w * y + x * z);
        const float fz = -(1.0f - 2.0f * (x * x + y * y));
        const float hy = atan2f(fx, -fz);
        const float gaze = g_gameYawRef + g_yawSign.load() * WrapPi(hy - g_headYawRef);
        const float stockYaw =
            g_reachStockHeadingYaw.load(std::memory_order_relaxed);
        const float delta = WrapPi(gaze - stockYaw);
        const float c = cosf(delta), s = sinf(delta);
        const float nx = mx * c - my * s;
        const float ny = mx * s + my * c;
        mx = nx;
        my = ny;
        return;
    }
    if (!Game_HasTitleCapability(TitleCapability_ControllerAim))
        return;
    // The game moves relative to its aim heading, which VR aim points at the
    // hand. Rotate the move vector by (head - aim) yaw so pushing forward
    // walks where you look instead of where the gun points.
    if (!g_enabled.load() || !g_aimSeen.load() || !g_vrAim.load())
        return;
    float q[4], p[3];
    if (!VR_GetHeadPose(q, p))
        return;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float fx = -2.0f * (w * y + x * z);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float hy = atan2f(fx, -fz);
    const float headYaw = g_gameYawRef + g_yawSign.load() * WrapPi(hy - g_headYawRef);
    const float aimYaw = atan2f(g_aimFwdY.load(), g_aimFwdX.load());
    const float delta = WrapPi(headYaw - aimYaw);
    const float c = cosf(delta), s = sinf(delta);
    const float nx = mx * c - my * s;
    const float ny = mx * s + my * c;
    mx = nx;
    my = ny;
}

bool Game_MoveStickIsLocomotion()
{
    // Only these runtime modes drive the character with the left stick. Every
    // other mode (Paused/settings, Shell, Loading, Cutscene, Dead, Unsupported)
    // means the game is reading the stick for menu navigation, so the input
    // hook must not rotate it head-relative or floor its axes past the deadzone.
    // Halo 3 and ODST both drive RuntimeMode, so this is one shared behavior.
    const RuntimeMode mode = TitleAdapter_GetRuntimeMode();
    if (mode == RuntimeMode::Gameplay ||
        mode == RuntimeMode::Vehicle ||
        mode == RuntimeMode::Turret)
        return true;
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    // Since the Reach camera heartbeat (2026-07-27) the shared RuntimeMode
    // does hold Gameplay for armed Reach, so the mode check above normally
    // answers. This armed-core fallback stays for the bounded windows where
    // ownership has not resolved yet (first heartbeats after arming, the
    // <=500 ms expiry after a transition) so locomotion does not glitch to
    // raw-stick there. Scoped to Reach; Halo 3/ODST keep the pure mode-driven
    // path unchanged.
    //
    // The fallback must honour native pause, or it reinstates the exact defect
    // the pause work removes: it would answer "locomotion" during the pause
    // menu even while the mode above correctly says Paused, and the menu stick
    // would still be treated as walking. The cache is published by the Present
    // tick (-1 = flag not located, in which case this behaves as it always
    // did).
    if (TitleAdapter_GetActiveTitle() == GameTitle::HaloReach &&
        g_reachCamera.armed.load(std::memory_order_acquire) &&
        g_enabled.load(std::memory_order_acquire))
        return g_reachEnginePauseCache.load(std::memory_order_acquire) != 1;
#endif
    return false;
}

void Game_GunScale(int dir)
{
    // Uniform mesh scale of the hand-anchored arms+gun assembly around the
    // wrist. Home = bigger, End = smaller. Persisted next time settings save.
    const float s = Clamp(g_config.gun_scale * (dir > 0 ? 1.05f : 1.0f / 1.05f),
                          0.3f, 3.0f);
    g_config.gun_scale = s;
    LOG("weapon size %.2fx", s);
}

float Game_GetWorldScale() { return g_worldScale.load(); }
float Game_GetZoomFactor() { return g_zoomFactor.load(); }

void Game_GetProjectionTangents(float& tanX, float& tanY)
{
    tanX = g_projectionTanX.load();
    tanY = g_projectionTanY.load();
}

bool Game_GetRenderHalfFovs(
    uint64_t preparedFrameSerial, float halfX[2], float halfY[2])
{
    if (!halfX || !halfY)
        return false;
    const float fallbackX = g_renderHalfFovX.load();
    const float fallbackY = g_renderHalfFovY.load();
    halfX[0] = fallbackX;
    halfX[1] = fallbackX;
    halfY[0] = fallbackY;
    halfY[1] = fallbackY;
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    if (TitleAdapter_GetActiveTitle() == GameTitle::HaloReach)
    {
        if (!g_reachCamera.armed.load(std::memory_order_acquire) ||
            !preparedFrameSerial)
        {
            return false;
        }
        const uint64_t leftSerial =
            g_reachRenderFovSerial[0].load(std::memory_order_acquire);
        const uint64_t rightSerial =
            g_reachRenderFovSerial[1].load(std::memory_order_acquire);
        if (leftSerial != preparedFrameSerial ||
            rightSerial != preparedFrameSerial)
        {
            return false;
        }
        for (int eye = 0; eye < 2; ++eye)
        {
            halfX[eye] =
                g_reachRenderHalfFovX[eye].load(std::memory_order_relaxed);
            halfY[eye] =
                g_reachRenderHalfFovY[eye].load(std::memory_order_relaxed);
            if (!isfinite(halfX[eye]) || !isfinite(halfY[eye]) ||
                halfX[eye] <= 0.0f || halfX[eye] >= 1.5707f ||
                halfY[eye] <= 0.0f || halfY[eye] >= 1.5707f)
            {
                return false;
            }
        }
        return true;
    }
#endif
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    const uint32_t generation =
        g_odstRuntimeGeneration.load(std::memory_order_acquire);
    const TitleAdapterRuntimeSnapshot runtime =
        RuntimeSnapshot(GetTickCount64());
    if (generation &&
        runtime.runtime.owner == GameTitle::Halo3ODST &&
        runtime.runtime.qualifyingOwnerCount == 1 &&
        runtime.runtime.generation == generation &&
        runtime.runtime.installed && runtime.runtime.armed &&
        !runtime.runtime.teardownRequested &&
        (runtime.runtime.enabledCapabilities & TitleCapability_Stereo) != 0)
    {
        halfX[0] = g_odstRenderHalfFovX[0].load(std::memory_order_relaxed);
        halfX[1] = g_odstRenderHalfFovX[1].load(std::memory_order_relaxed);
        halfY[0] = g_odstRenderHalfFovY[0].load(std::memory_order_relaxed);
        halfY[1] = g_odstRenderHalfFovY[1].load(std::memory_order_relaxed);
    }
#endif
    return true;
}


void Game_SetStereoEye(int eye)
{
    g_stereoEye = (eye == 0 || eye == 1) ? eye : -1;
}
