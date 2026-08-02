# Deferred: all-title save lifecycle investigation

**Status: unconfirmed / deferred on 2026-08-01. Do not roll back any current
candidate because of this note.** Another agent will address the next bug.

## User report

The reported defect is random across Halo 3, ODST, and Reach. It affects
checkpoint creation, loading/resuming a save, and overwriting an existing save.
Co-op exposed one reproducible Halo 3 case, but is not assumed to be the cause.
Do not reduce this to peer configuration, HUD/tag values, refresh rate, vehicle
state, or a Halo-3-only patch without new evidence.

## Preserved evidence

- Reproducer logs:
  - `N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\Halo_MCC_VR\halo3xr.log`
  - `N:\dev\outsidelogs\halo3xr (1).log`
- Both peers reached a Halo 3 section transition at the same point. The game
  session returned to shell; the MCC process itself did not show an application
  crash in these logs.
- At 11:41:30, `autosave_Halo3.bin` completed a normal-size overwrite and the
  session still aborted about 5.75 seconds later.
- At 12:35:52, the same autosave was again overwritten successfully. The later
  boundary froze/finalized the film at 12:36:07 and returned to shell at
  12:36:12, with no subsequent completed overwrite.
- The associated films were cleanly finalized, not truncated.
- The injected DLL has no direct save-file API hook or write path. Its own file
  I/O is its configuration and log file; this is not evidence that the game
  save transaction is safe.

## Engine facts established from H3EK

Halo 3 checkpoint save/load is a game-state transaction, not a comparison of
mod configuration values. A normal checkpoint waits for safe state, then saves
the fixed game-state buffer under the engine game-state lock, signs/checks it,
and persists it. The official code asserts render-thread quiescence while the
locked state is signed/verified. Load validates the stored state before applying
it. Equivalent title-specific evidence is still required for ODST and Reach.

## Plausible but unproven common mechanism

All title render paths temporarily write Halo-owned camera/player-view/render
memory for VR. Halo 3 leaves one source-camera modification in place; ODST and
Reach restore most of their temporary edits after their native render work.
The mod currently has no verified save/load transaction gate.

This is a credible race candidate only. It is **not proven** that any of those
addresses are inside the saved/signed game-state region, or that a mod write is
what caused the observed aborts.

## Required next evidence

Instrument each title's verified save/load transaction boundaries: request and
safety result; game-state lock; snapshot/sign/compress; storage commit/rollback;
and load header/signature/decompress/fixup result. Pair that with file I/O
tracing for MCC. Only after an actual overlap is proven should a candidate gate
VR-owned engine-memory writes during save/load; that gate must fail open to
stock rendering for the affected transaction only.
