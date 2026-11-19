// Simulated galvanic cells with fault injection (AGENTS.md 11) — the fault injector for the cell
// chain is part of this class, not a separate module, because a fault has to be injected into the
// same physics that produces the reading. Anything bolted on downstream would skip the calibration
// arithmetic and the bias check, which is where the interesting failures live.
//
// What it produces is **millivolts**, so the firmware's own calibration, scaling and 3-of-3 voting
// sit in the loop (AGENTS.md 11). The cell model is deliberately *not* the inverse of the stored
// CellCal: it has its own per-cell gain and offset, so the calibration division is exercised rather
// than undone.
//
// The most important fault here is `setCurrentLimited()`. A current-limited (aged) cell reads
// **correctly in air**, so the air calibration passes it with flying colours, and then progressively
// under-reads as ppO2 rises — the classic failure that air calibration cannot detect, and the reason
// AGENTS.md 19 risk 3 exists. It is modelled as a top-end roll-off, not a gain error.
#pragma once

#include "SensorIf.h"

namespace sim {

// First-order cell lag. t90 is the time to reach 90% of a step; tau follows as t90/ln(10).
constexpr float kT90OverLn10 = 0.4342945f;

class SimCells : public CellSource {
  public:
    // Per-cell physical response, mV per bar of ppO2 at the sensing electrode. Defaults near
    // 47.6 mV/bar (a galvanic cell in air: 0.21 bar -> ~10 mV) with a small spread, so voting has
    // something ordinary to agree about and the voter's 20% band is exercised by real numbers.
    void setCellGainMvPerBar(int cell, float gain) { cfg_[cell].gain = gain; }
    void setCellOffsetMv(int cell, float mv) { cfg_[cell].offsetMv = mv; }
    float cellGainMvPerBar(int cell) const { return cfg_[cell].gain; }

    // --- fault injectors -------------------------------------------------------------
    // Every injector is per-cell and independent; none of them touch the other cells, because the
    // point of most of these tests is what voting does when exactly one cell misbehaves.

    // Fixed output in mV: the cell stops responding to the loop entirely.
    void setStuckMv(int cell, float mv);
    // Dead cell: 0 mV, as an open lead or an exhausted cell would read.
    void setDead(int cell, bool dead);
    // Channel absent: the converter's read for that cell stops coming back at all, which is a
    // different failure from 0 mV and must be reported differently.
    void setChannelPresent(int cell, bool present);
    // Slow linear drift in mV/s, e.g. a cell going off after a pressure excursion.
    void setDriftMvPerSec(int cell, float mvPerSec);
    // Step offset from now on (a connector corroding, a cell getting knocked).
    void addStepMv(int cell, float mv);
    // Added noise, peak mV. Deterministic generator — identical seeds, identical runs — so a sim
    // regression is reproducible and a verify run is not a coin toss.
    void setNoiseMv(int cell, float peakMv);
    // First-order response lag, seconds to 90% of a step (real galvanic cells are ~1-5 s).
    void setT90Sec(int cell, float t90);
    // Current-limited / aged cell: correct at low ppO2, roll-off above `startBar`, full scale
    // reached at `fullBar` no matter how much more ppO2 there is.
    void setCurrentLimited(int cell, bool on, float startBar = 0.90f, float fullBar = 1.75f);
    // Sign flip: the polarity of a galvanic cell depends on which lead is which, and the bench
    // resolves it with a per-channel `invert` flag (AGENTS.md 2). Injecting it here lets the
    // inverter path be tested without a wiring change.
    void setInvert(int cell, bool invert);

    void resetFaults();

    // --- operation -------------------------------------------------------------------

    // Advance the physics by dtSec with the loop at this ppO2. Called once per engine tick.
    // `loopPpO2` is the ppO2 at the sensing electrode; water-vapour correction belongs to whoever
    // sets the loop state (AGENTS.md 5: a cell sees ppO2, not ppO2 x (Pamb-PH2O)/Pamb).
    void advance(float dtSec, float loopPpO2);

    // CellSource: the millivolts currently being presented. Values are latched by advance(), so a
    // read between ticks returns the last conversion result rather than something time-dependent —
    // which is what a real converter does too.
    void readMv(float out[3], bool present[3]) override;

    SourceKind source() const override { return SourceKind::SIM; }

    // True when the raw signal would be inside the firmware's cell-fault band
    // (VoteConfig.mvMin..mvMax). Useful for the bias-sanity check tests.
    bool rawWithinFaultBand(int cell, float mvMin, float mvMax) const;

  private:
    struct Cfg {
        float gain = 47.6f;
        float offsetMv = 0.0f;
    };
    struct Fault {
        bool  stuck = false;
        float stuckMv = 0.0f;
        bool  dead = false;
        bool  present = true;
        float driftMvPerSec = 0.0f;
        float driftMv = 0.0f;
        float stepMv = 0.0f;
        float noisePeak = 0.0f;
        float t90Sec = 0.0f;
        bool  currentLimited = false;
        float clStartBar = 0.90f;
        float clFullBar = 1.75f;
        bool  invert = false;
    };
    // Deterministic noise: xorshift on a per-cell seed. Not cryptographic, and not std::mt19937 —
    // the point is that the same seed gives the same sequence on every host and every build.
    static float noiseSample(uint32_t& state, float peak);

    float applyCurrentLimit(float ppO2, const Fault& f) const;

    Cfg   cfg_[3];
    Fault fault_[3];
    // The cell's own slow response, tracked between ticks so t90 behaves like a real first-order
    // lag instead of a one-sample smear.
    float statePpO2_[3] = {0.21f, 0.21f, 0.21f};
    uint32_t seed_[3] = {0x12345678u, 0x23456789u, 0x3456789au};
    float lastMv_[3] = {0.0f, 0.0f, 0.0f};
    bool  lastPresent_[3] = {true, true, true};
};

}  // namespace sim
