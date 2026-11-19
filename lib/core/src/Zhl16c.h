// Buhlmann ZHL-16C/A with gradient factors, N2 + He (AGENTS.md 7).
//
// ALGORITHM PROVENANCE — read this before changing anything.
// My first implementation used gfRequired = p / (a + p/b) with the 1995 ZH-L16 coefficient
// pairing. That is WRONG: it produced no decompression obligation for a 50 m / 20 min air dive.
// The algorithm here is the one shipped in Subsurface (core/deco.cpp, tissue_tolerance_calc /
// get_gf), which is field-deployed in a widely used planner. Two consequences:
//   * The coefficient sets are the ZH-L16A arrays below, and He half-times are NOT the same as
//     N2 half-times (they differ substantially in this parameterisation).
//   * a and b are blended per compartment by inert-gas saturation share, not evaluated as
//     separate per-gas ceilings.
// This is a regression-anchored implementation, NOT a validated one: cross-check every number
// against a trusted dive computer before diving on it.
#pragma once
#include "GasModel.h"

#include <stddef.h>
#include <stdint.h>

namespace zhl {

constexpr int NCOMP = 16;

struct Config {
    float gfLow = 0.30f;
    float gfHigh = 0.70f;
    bool  icdGuard = true;
    float icdRatio = 0.80f;      // fast-tissue N2 offgas limited to this fraction of He uptake
    float fastCompMaxHalfTimeMin = 40.0f; // compartments the ICD guard applies to
    float ascentMPerMin = 8.0f;
    float stopStepM = 3.0f;
    float lastStopM = 6.0f;
    float mPerBar = 10.197f;
    float surfaceBar = 1.013f;
};

struct AscentGas {
    BreathingState state = BreathingState::CCR_CLOSED;
    GasMix diluent{"DIL 21/0", 0.21f, 0.0f};
    GasMix oxygen{"O2", 1.00f, 0.0f};
    GasMix bailout{" Bail 21/0", 0.21f, 0.0f};
    GasMix selected{"DIL 21/0", 0.21f, 0.0f};
    GasModelConfig gas{};
};

struct DecoPlan {
    bool  inDeco = false;
    bool  violation = false;    // mandatory stop deeper than the current ambient
    bool  bounded = false;      // search hit its step cap
    float ceilingBar = 0.0f;    // 0 == no obligation
    float ceilingM = 0.0f;
    float ttsMin = 0.0f;
    int   nStops = 0;
    float stopDepthM[16]{};
    float stopMin[16]{};
    int   controllingComp = -1;
    float gfLowPressureBar = 0.0f; // the dive's GF-low anchor ("first occurrence")
};

class Engine {
public:
    void reset(float surfaceBar);

    const float* n2() const { return pn2_; }
    const float* he() const { return phe_; }
    void setTissues(const float* n2, const float* he);

    void step(const Inspired& insp, float ambBar, float dtSec, const Config& cfg);

    // Mandatory-stop pressure (bar absolute) for the current tissues; 0 when none.
    float ceilingBar(const Config& cfg) const;
    float ceilingM(const Config& cfg) const;
    int   controllingCompartment() const { return guidingComp_; }
    float gfLowPressureBar() const { return gfLowPressure_; }
    // The GF-low anchor is part of the dive state, not a derived value: restoring tissue loading
    // without it re-bases the whole GF ramp and the reported ceiling jumps (AGENTS.md 19 risk 8).
    // TissuePersist must store it alongside the compartments.
    void setGfLowPressureBar(float v) { gfLowPressure_ = v > 0.0f ? v : 0.0f; }
    float gfAt(float ambBar, const Config& cfg) const;

    float depthOf(float ambBar, const Config& cfg) const;
    float ambAt(float depthM, const Config& cfg) const;

    DecoPlan plan(const AscentGas& asc, float ambBar, const Config& cfg) const;
    // Search iterations consumed by the most recent plan() call. A run that saturates the step
    // cap is a plan the model could not finish; "how close to the cap" is how that gets caught
    // on-target without a trace build, where a capped search can cost minutes of CPU per tick.
    int lastPlanSteps() const { return lastPlanSteps_; }

    // Minutes of continued breathing at `ambBar` before a mandatory stop appears at or below it.
    // The gas argument matters: in a CCR the inspired state, not the mix, sets the loading.
    float ndlMin(const AscentGas& gas, float ambBar, const Config& cfg, float dtSec = 5.0f,
                 float maxMin = 999.0f) const;

    static float halfTimeN2Min(int i);
    static float halfTimeHeMin(int i);

private:
    struct Blend { float a, b, sat; };
    void blend(int ci, const float* n2, const float* he, Blend& out) const;
    float lowestCeilingOf(int ci, const Blend& bl, float gf) const;
    float toleratedOf(int ci, const Blend& bl, float anchor, const Config& cfg) const;
    float ceilingCopies(const float* n2, const float* he, float anchor, int* compOut,
                        const Config& cfg) const;

    // Tissue-response factors `1 - exp(-ln2*dt/tht)` per compartment. They depend only on the
    // timestep, and the NDL and plan searches integrate thousands of fixed-step iterations —
    // recomputing 32 expf per iteration made an NDL=999 sweep take 1.15 s per tick on the
    // ESP32 (xtensa's soft-float expf is ~3 us, not x86's ~40 ns). Precomputing per search
    // call is bit-identical: expf on the same input returns the same value.
    struct Decay { float n2[NCOMP]; float he[NCOMP]; };
    static void decayFor(float dtSec, Decay& out);
    void stepCopies(float* n2, float* he, const Inspired& insp, const Decay& dec,
                    const Config& cfg) const;
    // Operates on caller-owned arrays: const on the engine, because planning and NDL search
    // must not disturb the live tissue state.
    void stepCopies(float* n2, float* he, const Inspired& insp, float dtSec, const Config& cfg) const;

    float pn2_[NCOMP]{};
    float phe_[NCOMP]{};
    float gfLowPressure_ = 1.013f;
    int   guidingComp_ = -1;
    mutable int lastPlanSteps_ = 0;
};

} // namespace zhl
