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

The compositor also requires a separate fresh, generation-tagged authored
projection aspect. It double-reads the cinematic-control publication around
that projection read, so a title transition or camera-control handoff cannot
combine state and aspect from different samples. Missing or invalid projection
metadata keeps the frame immersive rather than guessing an aspect.

## Halo 3 and ODST

Both engines expose the same uniquely matched
`cinematic_in_progress` leaf getter. It reads the title's TLS cinematic-globals
pointer at `TLS+0xA8`, then its active byte at globals `+5`. The separately
matched `cinematic_set_shot` path resolves the same TLS-index global and the
current scene/shot state:

| Title | Shot-state pointer |
| --- | --- |
| Halo 3 | `TLS+0x90` |
| ODST | `TLS+0xA0` |

The adapter reports `AuthoredLocked` only when the globals object is present,
its cinematic-active byte is nonzero, and the title-specific shot-state object
is readable. An inactive cinematic byte reports `PlayerControlled`. A missing
or non-unique signature, missing object, read fault, stale generation, or title
unload reports `Unknown` and masks the capability.

Halo 3's pristine compact camera stores its authored horizontal/vertical
projection tangents at `+0x28/+0x2C`. ODST's title-evidenced camera layout stores
its vertical/reference FOV inputs at those offsets and the resulting projection
matrix at derived-camera `+0x78`. The theatre branches retain those inputs and
matrix scales instead of substituting OpenXR eye FOV. The engine debug variable
`reduce_widescreen_fov_during_cinematics` is restored to its captured stock
value only while theatre is active; immersive VR keeps the existing widening
policy unchanged.

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
