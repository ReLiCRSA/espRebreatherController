// Oxygen exposure tracking (AGENTS.md 7).
//
// NOTE FOR REVIEWERS: the single-exposure limit table below is the one recorded in AGENTS.md
// (1.6 -> 240 min ... 0.6 -> 1800 min). It is a *documented assumption*, not a validated
// standard. It must be verified against the table the diver and their agency accept before
// this number is trusted underwater. It is exposed as an overridable table for that reason.
#pragma once

struct O2ToxicityState {
    float cnsPercent = 0.0f;
    float otu = 0.0f;
};

class O2Toxicity {
public:
    void reset() { state_ = O2ToxicityState{}; }

    void load(const O2ToxicityState& s) { state_ = s; }
    const O2ToxicityState& state() const { return state_; }

    // Minutes of allowed continuous exposure at ppO2, interpolated over the table.
    // Returns 0 for ppO2 above the top of the table (no allowed exposure).
    static float singleExposureLimitMin(float ppO2);

    // Accumulate `dtSec` of breathing at `ppO2`.
    // CNS accumulates at ppO2 >= `cnsThresholdBar` (default 0.5) and is eliminated
    // continuously with `washoutHalfTimeMin` (default 125 min).
    void step(float ppO2, float dtSec, float washoutHalfTimeMin = 125.0f,
              float cnsThresholdBar = 0.50f, float otuThresholdBar = 0.50f);

private:
    O2ToxicityState state_;
};
