// Profile runner: a segment queue executed against the *real* firmware stack (AGENTS.md 11).
//
// This is the piece that makes the simulator worth having. Each 1 s tick goes through the same code
// path a dive would:
//
//   profile -> SimBaro (depth/ambient)
//           -> LoopModel + ValveModel (what O2 was actually injected)
//           -> SimCells (millivolts, with faults)
//           -> evaluateCells()          <- the real voter
//           -> resolveInspired()        <- the real breathing-state model
//           -> zhl::Engine::step()      <- the real deco engine
//           -> O2Toxicity, plan(), ndlMin()
//
// Nothing is stubbed and no stage injects a number downstream. That is the requirement in AGENTS.md
// 11 ("simulate at the ADC/sensor layer, not the answer layer"): a simulator that fed ppO2 and depth
// straight into the engine would test the deco maths and nothing else.
//
// Time compression changes wall-clock cost only. The engine always integrates at a fixed 1 s step, so
// a 1x run and a 60x run produce numerically identical tissue state — and AGENTS.md 19 risk 9 still
// applies: anything keyed to *real* elapsed time (ascent-rate limiting, warm-up gates, watchdog)
// cannot be certified from a compressed run.
//
// Every snapshot carries a `sources` stamp. Risk 11 treats simulated data reaching dive-state output
// as a fatal design bug, so the stamp is produced by the runner on every tick rather than being
// something a caller remembers to set.
#pragma once

#include <cstdint>
#include <cstring>

#include "GasMix.h"
#include "GasModel.h"
#include "LoopModel.h"
#include "O2Toxicity.h"
#include "SensorIf.h"
#include "SimBaro.h"
#include "SimCells.h"
#include "ValveModel.h"
#include "CcrControlLaw.h"
#include "Voting.h"
#include "Zhl16c.h"

namespace sim {

constexpr int kMaxSegments = 24;
constexpr int kMaxGases = 8;
constexpr int kMaxAssertions = 24;
constexpr int kMaxStops = 16;

// Which mix the diver is breathing in a segment. The escape/STAGE3 shortcut exists because the
// bailout decision is a safety action and should not depend on getting an index right under stress.
enum GasSel : uint8_t { GAS_DILUENT = 0, GAS_ESCAPE = 1, GAS_STAGE = 2 };

struct GasSet {
    GasMix diluent{"AIR", 0.21f, 0.0f};
    GasMix oxygen{"O2", 1.00f, 0.0f};
    GasMix bailout{"ESCAPE", 0.21f, 0.0f};
    GasMix stage[kMaxGases];
    int stageCount = 0;
};

struct Segment {
    float depthM = 0.0f;
    float durationMin = 0.0f;
    float setpointBar = 0.70f;          // closed-circuit states only
    BreathingState mode = BreathingState::CCR_CLOSED;
    GasSel gasSel = GAS_DILUENT;
    int gasIndex = 0;                   // index into GasSet::stage when gasSel == GAS_STAGE
    float rateMPerMin = 0.0f;           // 0 = use profile default
};

struct Profile {
    Segment seg[kMaxSegments];
    int count = 0;
    float defaultRateMPerMin = 8.0f;
    float waterTempC = 20.0f;
    // Metabolic O2 use, surface litres/min (a molar rate: depth-independent). Unmeasured default,
    // AGENTS.md 20 open item 5 — workload-dependent, and the number the sim's realism hinges on.
    float workloadO2RateSurfLpm = 1.2f;
};

// One tick of everything the display, the log and the web panel would show. Fixed-size on purpose:
// no allocation in the dive path (AGENTS.md 17), and this same shape is what M6's WebSocket push
// will serialise.
struct Snapshot {
    uint32_t tSec = 0;
    int segment = 0;
    float depthM = 0.0f;
    float ambBar = 1.013f;
    float tempC = 20.0f;

    float ppO2Used = 0.0f;
    float medianPpO2 = 0.0f;
    float cellMv[3] = {0.0f, 0.0f, 0.0f};
    bool cellPresent[3] = {true, true, true};
    VoteStatus vote = VoteStatus::NO_DATA;
    int passing = 0;
    float maxDeviationBar = 0.0f;

    float ndlMin = 0.0f;
    float ttsMin = 0.0f;
    float ceilingM = 0.0f;
    bool inDeco = false;
    bool decoViolation = false;
    bool planBounded = false;
    int nStops = 0;
    float stopDepthM[kMaxStops] = {0.0f};
    float stopMin[kMaxStops] = {0.0f};
    int controllingComp = -1;
    float gfAnchorBar = 0.0f;

    float cnsPct = 0.0f;
    float otu = 0.0f;

    int pulsesCommanded = 0;
    int pulsesClicked = 0;
    unsigned long pulsesTotal = 0;
    float pulsesPerMin = 0.0f;
    float injectedAmbL = 0.0f;
    float gasDeliveredAmbL = 0.0f;
    bool noClick = false;
    bool clickNoGas = false;
    bool supplyLimited = false;
    bool wouldInject = false;
    float clickPeakA = 0.0f;
    float clickRiseMs = 0.0f;

    float setpointBar = 0.0f;
    float po2Inspired = 0.0f;
    float ppN2 = 0.0f;
    float ppHe = 0.0f;
    BreathingState mode = BreathingState::CCR_CLOSED;
    SourceMix sources = SourceMix::SIM;
};

enum class Metric : uint8_t {
    PP02_USED = 0, DEPTH_M, AMBAR, TTS_MIN, CEILING_M, NDL_MIN, CNS_PCT, OTU,
    PULSES_TOTAL, INJECTED_AMBL, PP02_DELTA, PLAN_BOUNDED, VOTE_PASSING, CONTROLLING_COMP,
    // A band assertion needs the band, not one number: PP02_USED is the value at the end of the
    // window, and a "held setpoint" check has to say which edge it means. Without these, the only
    // way to assert a hold was to compare the window *maximum* against a low band, which passes
    // for the wrong reason.
    PP02_MIN, PP02_MAX,
};
inline const char* metricName(Metric m) {
    switch (m) {
        case Metric::PP02_USED: return "ppO2";
        case Metric::DEPTH_M: return "depth_m";
        case Metric::AMBAR: return "amb_bar";
        case Metric::TTS_MIN: return "tts_min";
        case Metric::CEILING_M: return "ceiling_m";
        case Metric::NDL_MIN: return "ndl_min";
        case Metric::CNS_PCT: return "cns_pct";
        case Metric::OTU: return "otu";
        case Metric::PULSES_TOTAL: return "pulses";
        case Metric::INJECTED_AMBL: return "injected_L";
        case Metric::PP02_DELTA: return "ppO2_change";
        case Metric::PLAN_BOUNDED: return "plan_bounded";
        case Metric::VOTE_PASSING: return "vote_passing";
        case Metric::CONTROLLING_COMP: return "controlling_comp";
        case Metric::PP02_MIN: return "ppO2_min";
        case Metric::PP02_MAX: return "ppO2_max";
        default: return "metric";
    }
}

struct Assertion {
    Metric metric = Metric::PP02_USED;
    uint32_t fromSec = 0;     // evaluation window, in profile time
    uint32_t toSec = 0;       // 0 = end of run
    float low = 0.0f;
    float high = 0.0f;
    const char* note = "";
};

struct AssertionResult {
    bool passed = false;
    // false = the window held no retained samples, so the assertion was never evaluated. That is
    // not a pass and not a failure of the model: it is the report saying it could not answer.
    bool evaluated = true;
    int samples = 0;
    float observed = 0.0f;
    char message[160] = {0};
};

struct VerifyReport {
    int total = 0;
    int passed = 0;
    int unevaluated = 0;
    AssertionResult item[kMaxAssertions];
    Snapshot finalSnap{};
    bool boundedPlanSeen = false;
    bool solenoidFaultSeen = false;
    SourceMix sources = SourceMix::SIM;
    uint32_t histOldestSec = 0;   // what the history actually covered for this report
    uint32_t histNewestSec = 0;
    unsigned long histDropped = 0;

    // An unevaluated assertion is not a pass. `allPassed` means every assertion was answered and
    // every answer was in its band.
    bool allPassed() const { return total > 0 && passed == total && unevaluated == 0; }
};

// Cell calibration the sim starts from, in the firmware's own CellCal form. The air-cal flow
// (POST /api/cal/air, M2) overwrites these once it exists; until then the sim ships a plausible
// ~47.6 mV/bar slope so voting and the >=30 mV/bar aged check have real inputs.
struct CalSet {
    CellCal cal[3] = {CellCal{}, CellCal{}, CellCal{}};
    VoteConfig vote{};
};

class SimRunner {
  public:
    SimRunner();

    void setProfile(const Profile& p);
    void setGases(const GasSet& g);
    void setConfig(const zhl::Config& engineCfg, const GasModelConfig& gasCfg,
                   const CalSet& cal, const ControlConfig& ctrl);
    void setBattery(float volts) { batteryV_ = volts; }
    float battery() const { return batteryV_; }

    // Start a run: 0 = surface, 0.21 ppO2, tissues at surface. Also refreshes the sim cell model's
    // air signal from the calibration constants so "cells in air" is the calibration-quality
    // reference (AGENTS.md 6: air cal is 0.21 ppO2 at 1 bar).
    void resetRun();

    // One engine tick. Returns the snapshot for that tick.
    const Snapshot& tick();
    // `simSeconds` is wall-time-independent: it runs that many 1 s engine steps.
    void runSeconds(uint32_t simSeconds);
    void runProfile();
    bool finished() const { return tSec_ >= totalSec_; }
    uint32_t totalSec() const { return totalSec_; }
    uint32_t tSec() const { return tSec_; }

    const Snapshot& last() const { return snap_; }
    // Window helpers used by the assertions, and by the web panel's graphs. A window with no
    // retained samples must never answer with a fabricated zero: windowCount() says how many
    // samples back a figure, and verify() reports an assertion it could not evaluate as
    // `evaluated == false` instead of comparing 0.0 against the band. (Reading 0.0 out of an empty
    // window is how an assertion about the tail of a long run could pass without ever looking at
    // the run — the same silent-plausible-number failure as risk 13.)
    float windowMinPpO2(uint32_t fromSec, uint32_t toSec) const;
    float windowMaxPpO2(uint32_t fromSec, uint32_t toSec) const;
    // Value at the end of the window (toSec == 0 means end of run), which is what "ppO2 must have
    // dropped by X over this stretch" needs.
    float windowEndPpO2(uint32_t fromSec, uint32_t toSec) const;
    bool windowAnyBounded(uint32_t fromSec, uint32_t toSec) const;
    int windowPulses(uint32_t fromSec, uint32_t toSec) const;
    int windowCount(uint32_t fromSec, uint32_t toSec) const;
    // Newest retained sample in the window, read as the named metric.
    float windowEndMetric(uint32_t fromSec, uint32_t toSec, Metric m) const;

    // History coverage. The ring keeps the *most recent* samples, so a long run loses its start:
    // histOldestSec()/histNewestSec() say what is actually there and histDropped() counts what was
    // overwritten. Every figure printed from a window should be quotable against that.
    uint32_t histOldestSec() const;
    uint32_t histNewestSec() const;
    unsigned long histDropped() const { return histDropped_; }
    // Keep every Nth tick. Default 1. On an ESP32 the ring is small, so a 26-minute profile needs a
    // stride of ~30 to cover 48 minutes of run rather than its first 96 seconds.
    void setHistoryStride(uint32_t sec) { histStride_ = sec < 1u ? 1u : sec; }
    uint32_t historyStride() const { return histStride_; }

    VerifyReport verify(const Assertion* a, int n);

    // The subsystems, public on purpose: fault injection and bench inspection both need them, and
    // hiding them would push the injectors behind a facade that cannot be asserted.
    SimBaro baro;
    SimCells cells;
    LoopModel loop;
    ValveModel valve;
    CcrControlLaw control;
    zhl::Engine engine;
    O2Toxicity o2tox;

  private:
    void segmentAt(uint32_t t, Segment* outSeg, bool* justEntered) const;
    const Snapshot* findHist(uint32_t sec) const;
    float depthAt(uint32_t t) const;
    const GasMix& gasFor(const Segment& s) const;
    float inspiredSetpoint(const Segment& s, float ambBar, float depthM) const;

    Profile profile_;
    GasSet gases_;
    zhl::Config engineCfg_;
    GasModelConfig gasCfg_;
    CalSet cal_;
    ControlConfig ctrl_;

    uint32_t tSec_ = 0;
    uint32_t totalSec_ = 0;
    int prevSeg_ = -1;
    float batteryV_ = 3.7f;
    float airRefPpO2_ = 0.21f;
    Snapshot snap_{};
    zhl::AscentGas ascent_;

    // Circular history for window queries: enough for assertion windows and the 30-min live graph
    // shape, without an unbounded allocation (AGENTS.md 17). Sized per target: the host wants 512
    // snapshots for long verify profiles; an ESP32 image cannot afford the DRAM (145 KB of BSS with
    // the WiFi stack, and a 52 KB region overflow was the measured result of assuming the host
    // number fits). It is a ring, so it holds the most recent `kHist * histStride_` seconds: 96
    // entries at stride 1 is 96 s, which is why the on-target runner sets a stride (96 x 30 s =
    // 48 min of the fixed bench profile). The full simulator runs on the host, where long runs and
    // stride 1 both fit.
#ifndef SIM_HIST_CAPACITY
#ifdef ARDUINO
    static const int kHist = 96;
#else
    static const int kHist = 512;
#endif
#endif
    Snapshot hist_[kHist];
    int histCount_ = 0;   // valid entries, saturating at kHist
    int histNext_ = 0;    // write cursor once the ring is full
    unsigned long histDropped_ = 0;
    uint32_t histStride_ = 1;
    uint32_t histKeepTick_ = 0;  // ticks since the last stored sample
};

}  // namespace sim
