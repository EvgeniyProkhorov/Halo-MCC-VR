#pragma once

#include <cstdint>

// Decision core of the level-load gate (PlayerViewLivenessGate in game.cpp).
// The gate's job: no title may install hooks while the engine is loading a
// level, because every capture of the load-bounce bug shows hooks going in
// during a loading screen and the load dying to the main menu 1.3-4 s later
// (docs/ODST-LEVEL-LOAD-LOCKOUT.md, 8 bounced loads across three builds, all
// preceded by an install <= 1.6 s into the load; all 5 gated installs that
// waited for the real frozen-then-ticking transition survived).
//
// Input per 50 ms worker sample: did the engine's player-view fingerprint
// change since the last sample? Output: hold, or open with the reason.
//
// The two ways to open, and why each threshold is what it is:
//
// FROZEN, THEN TICKING - a real level load must produce this sequence: the
// loading screen leaves the player-view memory untouched (or a cold module's
// array is zeroed), then the new level's first frame writes it.
//   - The frozen half requires kFrozenMinSamples CONSECUTIVE still samples
//     (300 ms at the 50 ms poll). One still sample is not enough: the
//     outgoing level's camera keeps "dying ticks" running for 3-4 s after a
//     Save & Quit (measured 06:28:28-06:28:32 in the 50ddf56 capture), and a
//     single-sample hiccup inside that run must not count as the loading
//     screen. Real loading screens freeze for 7-12 s; a pause menu freezes
//     for as long as the user reads it; 300 ms costs nothing real.
//   - The ticking half is the first change after a proven freeze. In every
//     capture nothing ticks the player view mid-load after the freeze
//     begins, so the first tick IS the level running.
//
// ALREADY RUNNING - a title that was mid-level when observation began never
// freezes, so uninterrupted change is accepted separately. The threshold
// must exceed the dying-tick window with margin: 12 samples (0.6 s) was
// shipped in build b70141d and opened on the dying ticks of EVERY bounced
// load (07:14:38, 07:18:32, 07:19:26 in the preserved log). Dying ticks
// measured <= ~4 s; kAlreadyRunningSamples is 120 samples = 6 s of change
// with not one still sample, which no loading screen or menu return has
// ever produced. Cost: after a mid-play adapter flap, VR returns in 6 s
// instead of 0.6 s.
//
// There is deliberately NO time-based unconditional open. Build b70141d
// carried a 12 s "open anyway" timeout; the preserved successful retry load
// froze for 11,766 ms - inside 300 ms of that timeout installing into the
// loading screen. A user idling in a lobby longer than any timeout would
// also convert it into an install-before-load. Holding is the correct state
// for a menu (the flat screen layer still shows in the headset); every real
// level entry produces frozen-then-ticking, and every already-running title
// produces the 6 s run, so no timeout is needed to avoid denying VR.
class LevelLoadGateLogic
{
public:
    enum class Decision : uint8_t
    {
        Hold = 0,
        OpenFrozenThenTicking,
        OpenAlreadyRunning,
    };

    // 300 ms of consecutive stillness at the 50 ms worker poll.
    static constexpr uint32_t kFrozenMinSamples = 6;
    // 6 s of consecutive change at the 50 ms worker poll.
    static constexpr uint32_t kAlreadyRunningSamples = 120;

    Decision Observe(bool changed)
    {
        if (m_open)
            return m_lastOpenDecision;
        if (changed)
        {
            m_stillRun = 0;
            if (m_sawFrozen)
            {
                m_open = true;
                m_lastOpenDecision = Decision::OpenFrozenThenTicking;
                return m_lastOpenDecision;
            }
            if (++m_changeRun >= kAlreadyRunningSamples)
            {
                m_open = true;
                m_lastOpenDecision = Decision::OpenAlreadyRunning;
                return m_lastOpenDecision;
            }
            return Decision::Hold;
        }
        m_changeRun = 0;
        if (++m_stillRun >= kFrozenMinSamples)
            m_sawFrozen = true;
        return Decision::Hold;
    }

    void Reset()
    {
        m_changeRun = 0;
        m_stillRun = 0;
        m_sawFrozen = false;
        m_open = false;
        m_lastOpenDecision = Decision::Hold;
    }

    bool IsOpen() const { return m_open; }
    bool SawFrozen() const { return m_sawFrozen; }
    uint32_t ChangeRun() const { return m_changeRun; }
    uint32_t StillRun() const { return m_stillRun; }

private:
    uint32_t m_changeRun = 0;
    uint32_t m_stillRun = 0;
    bool m_sawFrozen = false;
    bool m_open = false;
    Decision m_lastOpenDecision = Decision::Hold;
};
