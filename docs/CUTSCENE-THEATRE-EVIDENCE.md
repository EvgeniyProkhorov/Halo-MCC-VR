# Cutscene Theatre Qualification Evidence

This document records the title-specific facts behind the universal
`CinematicControlState` contract. It does not advance the accepted-build
pointer in `CURRENT-STATE.md`; the theatre candidate still requires the user's
headset acceptance in every advertised title.

## Shared contract

The compositor has no title list and does not infer a cutscene from motion,
letterboxing, fades, vehicles, death cameras, or module names. A generation-
tagged title publication resolves to one of three states:

- `Unknown`: proof is absent, stale, ambiguous, or from another generation.
- `PlayerControlled`: the verified title state says no authored locked
  cinematic owns the camera.
- `AuthoredLocked`: the title's verified authored-cinematic state is active and
  the player camera is unavailable.

Only a fresh `AuthoredLocked` publication plus
`TitleCapability_CutsceneTheater` can request the shared theatre. The
publication freshness window is 500 ms. Capability loss, title transitions,
module unload, a missing detector, or a failed read therefore return only this
feature to immersive presentation.

### Quest flat-theatre result and projection presentation

The exact source `23ed3cef9da9c8bb610ca1c2a540d3cbb98a3f83`, DLL
SHA-256 `828CD701FD4559E24F225FB72ABF0AAAD29918620A0D8AF16E3F2E843F9C88EE`,
entered theatre on the Steam edition through VirtualDesktopXR 1.0.10 on a
Meta Quest 3 at 90 Hz. The runtime supplied a valid 65.9 mm eye separation,
and the saved theatre-depth setting changed from 0.43 to 1.01. The user's
headset result was nevertheless explicit: the theatre image remained flat on
Quest and the depth slider made no visible difference. The same two-eye
theatre had visible stereo depth through the PSVR2/SteamVR path. This is a
cross-runtime presentation failure, not evidence against the headset result.

The failed path submitted two `XrCompositionLayerQuad` layers at the same
room-fixed pose. Each layer selected one physical eye through
`XrEyeVisibility` and selected the corresponding slice of the shared array
swapchain. That is core-valid OpenXR, but it leaves correct stereo dependent on
the runtime honoring both eye-selective layer composition and a nonzero array
slice on a quad layer.

The replacement is headset- and runtime-agnostic. Both title-rendered eyes are
resolved into private ordinary 2D textures. A bounded compositor draw projects
the configured room-fixed screen into each eye's already validated native-FOV
image rectangle, then submits the same single two-view
`XrCompositionLayerProjection` used by immersive gameplay. The screen
projection derives only from the runtime's current eye poses/FOV, the shared
theatre width/distance, and the title-published authored aspect. It contains no
headset name, vendor, runtime, IPD constant, or per-runtime branch. The depth
slider still scales the title's actual per-eye camera offsets before capture;
Flip Depth swaps the two resolved source eyes before projection.

If private projection resources cannot be created, only theatre falls back
loudly to the prior core quad presentation. If a claimed projection frame does
not complete, that frame is dropped and the immersive camera/session remain
armed. The replacement remains headset-pending on Quest and requires a PSVR2
regression before acceptance.

#### Rejected: projection-space theatre compositor

Source `387e5e3595d02608dd8906bcb610236b36ff58f6`, DLL SHA-256
`698311F911FC4A1204F27D35A0F26C21FC7CD15E7F6637687DC05F5478C4BA9F`,
was headset-rejected on the Steam edition through VirtualDesktopXR 1.0.10 on
a Meta Quest 3 at 90 Hz. The user reported major stuttering and warping, broken
3D cinema, and severely malformed box geometry. The exact log proves the new
projection path activated at `01:05:06.017`; it reported 90 FPS, so the
player-visible failure outranks that aggregate counter.

The new path also raised the theatre render-window p95 to 7.03 ms. In the
preserved pre-candidate log from the same Quest/runtime, the older quad path
held 2.56--3.99 ms during extended theatre presentation. The rejected path
added two full-size eye resolves plus two projection draws and cannot be
retained as a performance-neutral presentation change. Evidence is preserved
under
`out/test-runs/387e5e3-quest-theatre-projection-fail-20260730-010527`
(log SHA-256
`C7AA8336B49128720713865C9184BD07AD5EFE60D5B9DA6E76F4A23981F56CE7`).

The projection-space compositor is disabled before any further experiment.
Its code remains inert as negative evidence. Theatre returns to the prior
core eye-selective quad path; this restores the pre-candidate behavior but does
not claim to fix Quest depth.

The compositor also requires a separate fresh, generation-tagged authored
projection aspect. It double-reads the cinematic-control publication around
that projection read, so a title transition or camera-control handoff cannot
combine state and aspect from different samples. Missing or invalid projection
metadata keeps the frame immersive rather than guessing an aspect.

## Halo 3 and ODST common cinematic state

Both engines expose the same uniquely matched
`cinematic_in_progress` leaf getter. It reads the title's TLS cinematic-globals
pointer at `TLS+0xA8`, then its active byte at globals `+5`. The separately
matched `cinematic_set_shot` path resolves the same TLS-index global and the
current scene/shot state:

| Title | Shot-state pointer |
| --- | --- |
| Halo 3 | `TLS+0x90` |
| ODST | `TLS+0xA0` |

This common state is sufficient for Halo 3. Its adapter reports
`AuthoredLocked` only when the globals object is present, its cinematic-active
byte is nonzero, and the title-specific shot-state object is readable. An
inactive cinematic byte reports `PlayerControlled`. A missing or non-unique
signature, missing object, read fault, stale generation, or title unload
reports `Unknown` and masks the capability.

### ODST live look constraints

The first ODST headset pass disproved cinematic-active plus shot-state as a
complete camera-lock test: the first mission's drop-pod sequence entered
theatre even though right-stick look remained available. Candidate `14fc96e`
then tried the HaloScript `player_camera_control` disabled bit. The next
headset run disproved that interpretation too: the bit remained clear in a
legitimate locked cinematic, so it disabled all ODST theatre. The candidate was
reverted in `d00d962`; that global bit is negative evidence and is not used.

H3ODSTEK contains the title-native distinction. Every cinematic shot has a
`user input constraints` block whose records contain `ticks`, `maximum look
angles`, and `frictional force`. The official `c100` opening tags make the
drop-pod exception explicit: `c100_04` shots 3 and 4 set maximum look angles to
`-40,-40,20,40`, while `c100_05` shot 3 sets them to `0,0,0,0` with friction 1.
The earlier opening scenes contain no records granting nonzero look bounds.

The pinned retail ODST module
`5BB20976EFDFD9E1CE59C589339804725FEC239021027C8D65B2733EAB94829A`
retains the `cinematic_scripting_set_user_input_constraints` external. Its
wrapper at `halo3odst+0x22BC4C` resolves the current scene/shot record and calls
the unique implementation at `halo3odst+0x23F25C`. That function uses ODST's
own engine TLS index and the cinematic-camera pointer at `TLS+0x50`, verifies
camera type 5, and interpolates four maximum-look-angle values. The live
current values are at camera `+0xC8/+0xCC/+0xD0/+0xD4`; per-tick rates are at
`+0xD8/+0xDC/+0xE0/+0xE4`; remaining ticks are at `+0xF0`. The unique paired
updater at `halo3odst+0x23EF5C` applies those four rates only while `+0xF0` is
positive and decrements the count each tick, proving that a nonzero rate with
positive remaining ticks is pending look freedom rather than stale storage.

The ODST adapter now retains `AuthoredLocked` only when the common cinematic
proof is active, all four live look limits are zero, and no active interpolation
is opening any limit. Any nonzero current limit or active nonzero rate reports
`PlayerControlled`, keeping the pod immersive from the first opening tick.
Missing or non-unique signatures, a mismatched TLS owner, missing/type-mismatched
camera state, non-finite values, invalid ticks, or a read fault reports
`Unknown` and masks only ODST theatre. It never disarms ODST VR or changes the
camera core.

Halo 3's pristine compact camera stores its authored horizontal/vertical
projection tangents at `+0x28/+0x2C`. ODST's title-evidenced camera layout stores
its vertical/reference FOV inputs at those offsets and the resulting projection
matrix at derived-camera `+0x78`. The theatre branches retain those inputs and
matrix scales instead of substituting OpenXR eye FOV. The engine debug variable
`reduce_widescreen_fov_during_cinematics` is restored to its captured stock
value only while theatre is active; immersive VR keeps the existing widening
policy unchanged.

The first Halo 3 headset pass of the authored-projection candidate reported
that theatre framing remained slightly too wide and that geometry culled at the
camera edge, while Reach was correct. The Halo 3 follow-up is deliberately
title-local. The signature-located compact-to-derived builder at
`halo3+0x2A6980` derives the complete camera block from the compact camera; its
projection matrix begins at the already validated derived offset `+0x78`.
The prepared-view path at `halo3+0x1854C8` then uploads that compact/derived
pair before the inner render at `halo3+0x286A14`. The candidate therefore
builds Halo 3's visibility data from a compact frustum at least as wide as both
the authored camera and the existing OpenXR cover, then changes only the
derived projection's X/Y scales to a 0.95 tangent-space copy of the authored
framing. Applying the same scale to both axes preserves the authored aspect.
When theatre is inactive, projection and visibility both receive the exact
existing OpenXR tangents. ODST and Reach do not call this Halo 3 policy. This
follow-up remains headset-pending; the static boundary and isolation are
evidence, not a claim that the visible culling result has passed.

## Reach

The accepted Winter Contingency probe identified the low byte of Reach's
0x40-byte cinematic-globals member at `+0x24`: it is 1 only for the duration of
the authored cutscene and 0 through the menu and subsequent gameplay. The
registration signature resolves the module TLS index and member slot per
generation; if that proof is not armed, Reach masks the theatre capability and
publishes `Unknown`.

Reach therefore maps a proven nonzero `+0x24` low byte to `AuthoredLocked` and
zero to `PlayerControlled`. The existing authored-camera discontinuity detector
is used only to reorient at shot cuts. Camera motion or a camera jump can never
activate theatre by itself.

The pristine Reach derived camera carries its authored projection at the
evidenced derived-block offset `+0x78`. Theatre publishes that projection's
aspect, retains the pristine compact camera's vertical FOV at `+0x28`, and
requires both rebuilt eye projections to match the published authored aspect.
Immersive Reach continues using its existing OpenXR covering FOV and validation.

## Future titles

A future adapter may advertise `TitleCapability_CutsceneTheater` only after it
has equivalent title-specific evidence that distinguishes an authored locked
camera from player-controllable sequences, plus a validated authored projection
aspect. Once it publishes the shared state and projection,
the existing compositor, configuration, transition, per-eye quad submission,
and failure isolation apply without title-specific presentation code.
