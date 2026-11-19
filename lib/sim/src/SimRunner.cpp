#include "SimRunner.h"

#include <math.h>
#include <stdio.h>  // snprintf in fmtFail

namespace sim {

SimRunner::SimRunner() { resetRun(); }

void SimRunner::setProfile(const Profile& p) {
    profile_ = p;
    if (profile_.count > kMaxSegments) profile_.count = kMaxSegments;
    if (profile_.count < 0) profile_.count = 0;
    totalSec_ = 0;
    for (int i = 0; i < profile_.count; ++i) {
        float d = profile_.seg[i].durationMin;
        if (d < 0.0f) d = 0.0f;
        totalSec_ += static_cast<uint32_t>(d * 60.0f + 0.5f);
    }
}

void SimRunner::setGases(const GasSet& g) {
    gases_ = g;
    if (gases_.stageCount > kMaxGases) gases_.stageCount = kMaxGases;
}

void SimRunner::setConfig(const zhl::Config& engineCfg, const GasModelConfig& gasCfg,
                          const CalSet& cal, const ControlConfig& ctrl) {
    engineCfg_ = engineCfg;
    gasCfg_ = gasCfg;
    cal_ = cal;
    ctrl_ = ctrl;
}

void SimRunner::resetRun() {
    tSec_ = 0;
    prevSeg_ = -1;
    histCount_ = 0;
    histNext_ = 0;
    histDropped_ = 0;
    histKeepTick_ = 0;
    snap_ = Snapshot{};

    baro.setDepthM(0.0f);
    baro.setTempC(profile_.waterTempC);
    baro.setSurfaceBar(engineCfg_.surfaceBar);
    // Same gradient the deco engine uses — one number, not a density round-trip.
    baro.setMetresPerBar(engineCfg_.mPerBar);

    loop.config = LoopConfig{};
    loop.config.metabolicRateSurfLpm = profile_.workloadO2RateSurfLpm;
    loop.config.tempC = profile_.waterTempC;
    loop.config.pH2OBar = gasCfg_.pH2OBar;
    loop.config.openCircuit = false;
    loop.reset();
    loop.setPpO2(airRefPpO2_);

    valve.reset();
    control.reset();
    cells.resetFaults();
    // Seed the cell model's response from the stored calibration, so "three cells in air" means a
    // calibration-quality signal rather than an arbitrary number: 0.21 bar at the stored slope.
    for (int i = 0; i < 3; ++i) {
        cells.setCellGainMvPerBar(i, cal_.cal[i].slopeMvPerBar);
        cells.setCellOffsetMv(i, cal_.cal[i].offsetMv);
    }

    engine.reset(engineCfg_.surfaceBar);
    o2tox.reset();
    // The anchor is engine state, not config (decision 24): it must go through the engine.
    engine.setGfLowPressureBar(engineCfg_.surfaceBar);

    snap_.ambBar = engineCfg_.surfaceBar;
    snap_.depthM = 0.0f;
    snap_.ppO2Used = airRefPpO2_;
    snap_.sources = combineSources(baro.source(), cells.source());
}

const GasMix& SimRunner::gasFor(const Segment& s) const {
    switch (s.gasSel) {
        case GAS_ESCAPE: return gases_.bailout;
        case GAS_STAGE:
            if (s.gasIndex >= 0 && s.gasIndex < gases_.stageCount) return gases_.stage[s.gasIndex];
            return gases_.diluent;
        case GAS_DILUENT:
        default: return gases_.diluent;
    }
}

float SimRunner::inspiredSetpoint(const Segment& s, float ambBar, float depthM) const {
    if (isClosedCircuit(s.mode)) return setpointAt(gasCfg_, depthM);
    const GasMix& g = gasFor(s);
    Inspired i = resolveInspired(s.mode, gases_.diluent, gases_.oxygen, gases_.bailout, g, ambBar,
                                 depthM, gasCfg_);
    return i.po2;
}

void SimRunner::segmentAt(uint32_t t, Segment* outSeg, bool* justEntered) const {
    uint32_t acc = 0;
    int idx = profile_.count - 1;
    for (int i = 0; i < profile_.count; ++i) {
        const uint32_t len = static_cast<uint32_t>(profile_.seg[i].durationMin * 60.0f + 0.5f);
        if (t < acc + len) { idx = i; break; }
        acc += len;
    }
    if (idx < 0) idx = 0;
    if (outSeg) *outSeg = profile_.seg[idx];
    if (justEntered) *justEntered = (idx != prevSeg_);
}

float SimRunner::depthAt(uint32_t t) const {
    // Waypoint semantics: each segment *moves to* its depth at its own rate, then holds for the
    // rest of its duration. Depth is continuous across boundaries — the ramp carries over. The
    // first version restarted each segment from the *previous waypoint* and clamped rampSec to
    // the segment length, so a 3-minute segment aimed at 30 m snapped to 30 m at the boundary
    // (a 225 s ramp does not fit in 180 s): an instant 6 m jump that the deco engine then had
    // to decompress. A profile runner that manufactures depth jumps is worthless for testing
    // ascent-rate behaviour, so the carrying depth, not the waypoint, is what the next segment
    // starts from.
    uint32_t acc = 0;
    float carried = 0.0f;
    for (int i = 0; i < profile_.count; ++i) {
        const Segment& s = profile_.seg[i];
        const uint32_t len = static_cast<uint32_t>(s.durationMin * 60.0f + 0.5f);
        if (t < acc + len) {
            const float rate = s.rateMPerMin > 0.0f ? s.rateMPerMin : profile_.defaultRateMPerMin;
            const float span = s.depthM - carried;
            const float rampSec = span != 0.0f ? fabsf(span) / (rate / 60.0f) : 0.0f;
            const float local = static_cast<float>(t - acc);
            if (rampSec > 0.0f && local < rampSec)
                return carried + span * (local / rampSec);
            return s.depthM;
        }
        const float rate = s.rateMPerMin > 0.0f ? s.rateMPerMin : profile_.defaultRateMPerMin;
        const float span = s.depthM - carried;
        const float rampSec = span != 0.0f ? fabsf(span) / (rate / 60.0f) : 0.0f;
        // What depth the diver actually stands at when this segment's clock runs out.
        carried = rampSec > 0.0f && static_cast<float>(len) < rampSec
                      ? carried + span * (static_cast<float>(len) / rampSec)
                      : s.depthM;
        acc += len;
    }
    return carried;
}

const Snapshot& SimRunner::tick() {
    Segment seg{};
    bool justEntered = false;
    segmentAt(tSec_, &seg, &justEntered);

    const float depthM = depthAt(tSec_);
    baro.setDepthM(depthM);
    baro.setTempC(profile_.waterTempC);
    float ambBar = engineCfg_.surfaceBar, tempC = profile_.waterTempC;
    baro.read(&ambBar, &tempC);

    const bool manualGas = seg.mode == BreathingState::CCR_DILUENT_FLUSH ||
                           seg.mode == BreathingState::CCR_O2_FLUSH;

    const GasMix& breathed = gasFor(seg);
    Inspired ins = resolveInspired(seg.mode, gases_.diluent, gases_.oxygen, gases_.bailout, breathed,
                                   ambBar, depthM, gasCfg_);

    if (justEntered) {
        // A mode or gas change is a state transition, not a re-tune (AGENTS.md 8): flush events
        // replace the loop gas, and the controller's integrator is dropped so it cannot chase the
        // old equilibrium.
        if (seg.mode == BreathingState::CCR_DILUENT_FLUSH) {
            loop.setBreathedHeliumFraction(breathed.fhe);
            loop.diluentFlush(breathed.fo2, ambBar);
        } else if (seg.mode == BreathingState::CCR_O2_FLUSH) {
            loop.o2Flush(gases_.oxygen.fo2, ambBar);
        } else if (seg.mode == BreathingState::MANUAL_BAILOUT ||
                   seg.mode == BreathingState::OC_OPEN) {
            loop.config.openCircuit = true;
            loop.breatheMix(breathed.fo2, ambBar);
        } else {
            loop.config.openCircuit = false;
            loop.setBreathedHeliumFraction(breathed.fhe);
        }
        control.reset();
    }

    // Closed loop: the control law commands pulses and the valve decides what they delivered.
    // During a manual-gas flush the diver, not the injector, owns the loop gas: the law still runs
    // (so the bench can see what it *would* command, AGENTS.md 8) but is disabled, so no pulses are
    // fired on top of the flush. Metabolism keeps consuming, which is honest — a long flush drains
    // O2 in the sim exactly as it does in a loop.
    const float setpoint = inspiredSetpoint(seg, ambBar, depthM);
    ControlConfig tickCfg = ctrl_;
    if (manualGas) tickCfg.enabled = false;
    LoopStepResult lr =
        loop.step(1.0f, ambBar, batteryV_, snap_.ppO2Used, setpoint, control, tickCfg, valve);

    // Cells see the loop, in millivolts. Calibration, scaling and voting all happen downstream in
    // the firmware's own code. Note: CellSource::readMv writes all three channels — it must be
    // called with full arrays, not per-element pointers into a struct (that was a real overrun,
    // caught by ASan the day this suite was written).
    cells.advance(1.0f, loop.ppO2());
    float mv[CELL_COUNT];
    bool present[CELL_COUNT];
    cells.readMv(mv, present);
    CellReading readings[CELL_COUNT];
    for (int i = 0; i < CELL_COUNT; ++i) {
        readings[i].mv = mv[i];
        readings[i].present = present[i];
    }
    VoteOutcome v = evaluateCells(readings, cal_.cal, cal_.vote, false);

    float ppO2Used = v.ppO2Used;
    if (v.status == VoteStatus::NO_DATA) {
        // Fallback of last resort, and deliberately the **lowest** present cell: when the vote
        // cannot be trusted, the control loop must not be led to inject on a phantom reading.
        float lowest = 1e9f;
        for (int i = 0; i < CELL_COUNT; ++i) {
            if (!readings[i].present) continue;
            const float p = ppO2FromMv(readings[i].mv, cal_.cal[i]);
            if (p < lowest) lowest = p;
        }
        ppO2Used = lowest > 1e8f ? 0.0f : lowest;
    }

    engine.step(ins, ambBar, 1.0f, engineCfg_);
    o2tox.step(ppO2Used, 1.0f);

    ascent_.state = seg.mode;
    ascent_.diluent = gases_.diluent;
    ascent_.oxygen = gases_.oxygen;
    ascent_.bailout = gases_.bailout;
    ascent_.selected = breathed;
    ascent_.gas = gasCfg_;

    snap_.tSec = tSec_;
    snap_.segment = 0;
    for (uint32_t acc = 0, i = 0; i < static_cast<uint32_t>(profile_.count); ++i) {
        const uint32_t len = static_cast<uint32_t>(profile_.seg[i].durationMin * 60.0f + 0.5f);
        if (tSec_ < acc + len) { snap_.segment = static_cast<int>(i); break; }
        acc += len;
    }
    snap_.depthM = depthM;
    snap_.ambBar = ambBar;
    snap_.tempC = tempC;
    snap_.ppO2Used = ppO2Used;
    snap_.medianPpO2 = v.medianPpO2;
    for (int i = 0; i < CELL_COUNT; ++i) {
        snap_.cellMv[i] = readings[i].mv;
        snap_.cellPresent[i] = readings[i].present;
    }
    snap_.vote = v.status;
    snap_.passing = v.passingCount;
    snap_.maxDeviationBar = v.maxDeviationBar;
    snap_.ndlMin = engine.ndlMin(ascent_, ambBar, engineCfg_);
    zhl::DecoPlan plan = engine.plan(ascent_, ambBar, engineCfg_);
    snap_.ttsMin = plan.ttsMin;
    snap_.ceilingM = plan.ceilingM;
    snap_.inDeco = plan.inDeco;
    snap_.decoViolation = plan.violation;
    snap_.planBounded = plan.bounded;
    snap_.nStops = plan.nStops < kMaxStops ? plan.nStops : kMaxStops;
    for (int i = 0; i < snap_.nStops; ++i) {
        snap_.stopDepthM[i] = plan.stopDepthM[i];
        snap_.stopMin[i] = plan.stopMin[i];
    }
    snap_.controllingComp = plan.controllingComp;
    snap_.gfAnchorBar = engine.gfLowPressureBar();
    snap_.cnsPct = o2tox.state().cnsPercent;
    snap_.otu = o2tox.state().otu;
    snap_.pulsesCommanded = lr.pulsesCommanded;
    snap_.pulsesClicked = lr.pulsesClicked;
    snap_.pulsesTotal = loop.pulsesTotal();
    snap_.pulsesPerMin = control.pulsesPerMin();
    snap_.injectedAmbL = loop.injectedAmbL();
    snap_.gasDeliveredAmbL = lr.gasDeliveredAmbL;
    snap_.noClick = lr.noClickWhileCommanded;
    snap_.clickNoGas = lr.clickedButNoGas;
    snap_.supplyLimited = lr.supplyLimited;
    snap_.wouldInject = lr.control.wouldInject;
    snap_.clickPeakA = lr.lastClick.peakA;
    snap_.clickRiseMs = lr.lastClick.riseMs;
    snap_.setpointBar = setpoint;
    snap_.po2Inspired = ins.po2;
    snap_.ppN2 = ins.pn2;
    snap_.ppHe = ins.phe;
    snap_.mode = seg.mode;
    snap_.sources = combineSources(baro.source(), cells.source());

    // Ring history, sampled at histStride_. Storing stops being "the first kHist seconds" the
    // moment a run is longer than the ring — the oldest samples are overwritten and counted, so
    // every window query can say what it actually covers.
    if (histKeepTick_ + 1 >= histStride_) {
        histKeepTick_ = 0;
        if (histCount_ < kHist) {
            hist_[histCount_++] = snap_;
        } else {
            hist_[histNext_] = snap_;
            histDropped_++;
        }
        histNext_ = histCount_ < kHist ? histCount_ : (histNext_ + 1) % kHist;
    } else {
        histKeepTick_++;
    }
    prevSeg_ = snap_.segment;
    ++tSec_;
    return snap_;
}

void SimRunner::runSeconds(uint32_t simSeconds) {
    for (uint32_t i = 0; i < simSeconds && !finished(); ++i) tick();
}

void SimRunner::runProfile() {
    while (!finished()) tick();
}

// --- history window queries ---------------------------------------------------------------

const sim::Snapshot* SimRunner::findHist(uint32_t sec) const {
    for (int i = 0; i < histCount_; ++i)
        if (hist_[i].tSec == sec) return &hist_[i];
    return nullptr;
}

// Window queries. Each one resolves `toSec == 0` to "end of run", scans the ring, and says how
// many samples it saw: a min/max over zero samples must never be reported as a number, because a
// plausible-looking 0.0 inside an assertion band is a false pass (risk 13).
float SimRunner::windowMinPpO2(uint32_t fromSec, uint32_t toSec) const {
    const uint32_t end = toSec == 0 ? tSec_ : toSec;
    float best = 1e9f;
    for (int i = 0; i < histCount_; ++i) {
        if (hist_[i].tSec < fromSec || hist_[i].tSec > end) continue;
        if (hist_[i].ppO2Used < best) best = hist_[i].ppO2Used;
    }
    return best > 1e8f ? 0.0f : best;
}

float SimRunner::windowMaxPpO2(uint32_t fromSec, uint32_t toSec) const {
    const uint32_t end = toSec == 0 ? tSec_ : toSec;
    float best = -1e9f;
    for (int i = 0; i < histCount_; ++i) {
        if (hist_[i].tSec < fromSec || hist_[i].tSec > end) continue;
        if (hist_[i].ppO2Used > best) best = hist_[i].ppO2Used;
    }
    return best < -1e8f ? 0.0f : best;
}

float SimRunner::windowEndPpO2(uint32_t fromSec, uint32_t toSec) const {
    const uint32_t end = toSec == 0 ? tSec_ : toSec;
    float val = 0.0f;
    bool any = false;
    for (int i = 0; i < histCount_; ++i) {
        if (hist_[i].tSec < fromSec || hist_[i].tSec > end) continue;
        if (!any || hist_[i].tSec >= end) { val = hist_[i].ppO2Used; any = true; }
    }
    return any ? val : 0.0f;
}

int SimRunner::windowCount(uint32_t fromSec, uint32_t toSec) const {
    const uint32_t end = toSec == 0 ? tSec_ : toSec;
    int n = 0;
    for (int i = 0; i < histCount_; ++i)
        if (hist_[i].tSec >= fromSec && hist_[i].tSec <= end) ++n;
    return n;
}

uint32_t SimRunner::histOldestSec() const {
    if (histCount_ == 0) return 0;
    uint32_t lo = hist_[0].tSec;
    for (int i = 1; i < histCount_; ++i)
        if (hist_[i].tSec < lo) lo = hist_[i].tSec;
    return lo;
}

uint32_t SimRunner::histNewestSec() const {
    if (histCount_ == 0) return 0;
    uint32_t hi = hist_[0].tSec;
    for (int i = 1; i < histCount_; ++i)
        if (hist_[i].tSec > hi) hi = hist_[i].tSec;
    return hi;
}

bool SimRunner::windowAnyBounded(uint32_t fromSec, uint32_t toSec) const {
    const uint32_t end = toSec == 0 ? tSec_ : toSec;
    for (int i = 0; i < histCount_; ++i) {
        if (hist_[i].tSec < fromSec || hist_[i].tSec > end) continue;
        if (hist_[i].planBounded) return true;
    }
    return false;
}

int SimRunner::windowPulses(uint32_t fromSec, uint32_t toSec) const {
    const uint32_t end = toSec == 0 ? tSec_ : toSec;
    int sum = 0;
    for (int i = 0; i < histCount_; ++i) {
        if (hist_[i].tSec < fromSec || hist_[i].tSec > end) continue;
        sum += hist_[i].pulsesCommanded;
    }
    return sum;
}

// --- assertions ---------------------------------------------------------------------------

// One place binds a Metric name to a Snapshot field, so the assertions, the window queries and the
// web graphs cannot disagree about what a metric means.
static float metricOf(const Snapshot& s, Metric m) {
    switch (m) {
        case Metric::PP02_USED: return s.ppO2Used;
        case Metric::DEPTH_M: return s.depthM;
        case Metric::AMBAR: return s.ambBar;
        case Metric::TTS_MIN: return s.ttsMin;
        case Metric::CEILING_M: return s.ceilingM;
        case Metric::NDL_MIN: return s.ndlMin;
        case Metric::CNS_PCT: return s.cnsPct;
        case Metric::OTU: return s.otu;
        case Metric::PULSES_TOTAL: return static_cast<float>(s.pulsesTotal);
        case Metric::INJECTED_AMBL: return s.injectedAmbL;
        case Metric::PLAN_BOUNDED: return s.planBounded ? 1.0f : 0.0f;
        case Metric::VOTE_PASSING: return static_cast<float>(s.passing);
        case Metric::CONTROLLING_COMP: return static_cast<float>(s.controllingComp);
        default: return 0.0f;
    }
}

// The value a windowed assertion observes: the newest retained sample inside the window, not the
// end-of-run snapshot. Otherwise an assertion written over a window silently measures whatever the
// last second of the run happened to be.
float SimRunner::windowEndMetric(uint32_t fromSec, uint32_t toSec, Metric m) const {
    const uint32_t end = toSec == 0 ? tSec_ : toSec;
    const Snapshot* pick = nullptr;
    for (int i = 0; i < histCount_; ++i) {
        if (hist_[i].tSec < fromSec || hist_[i].tSec > end) continue;
        if (!pick || hist_[i].tSec >= pick->tSec) pick = &hist_[i];
    }
    return pick ? metricOf(*pick, m) : 0.0f;
}

static void fmtFail(char* buf, size_t n, const Assertion& a, float observed) {
    snprintf(buf, n, "%s over %us..%us: got %.4f, expected %.4f..%.4f  %s",
             metricName(a.metric), static_cast<unsigned>(a.fromSec),
             static_cast<unsigned>(a.toSec), observed, a.low, a.high, a.note ? a.note : "");
}

VerifyReport SimRunner::verify(const Assertion* a, int n) {
    VerifyReport rep;
    rep.finalSnap = snap_;
    rep.sources = snap_.sources;
    rep.histOldestSec = histOldestSec();
    rep.histNewestSec = histNewestSec();
    rep.histDropped = histDropped_;
    if (n > kMaxAssertions) n = kMaxAssertions;
    for (int i = 0; i < n; ++i) {
        const Assertion& as = a[i];
        AssertionResult r;
        float observed = 0.0f;
        switch (as.metric) {
            case Metric::PP02_MIN:   observed = windowMinPpO2(as.fromSec, as.toSec); break;
            case Metric::PP02_MAX:   observed = windowMaxPpO2(as.fromSec, as.toSec); break;
            case Metric::PP02_DELTA:
                observed = windowEndPpO2(as.fromSec, as.toSec) -
                           windowMaxPpO2(as.fromSec, as.fromSec);
                break;
            case Metric::PLAN_BOUNDED:
                observed = windowAnyBounded(as.fromSec, as.toSec) ? 1.0f : 0.0f;
                break;
            default:
                observed = windowEndMetric(as.fromSec, as.toSec, as.metric);
                break;
        }
        r.observed = observed;
        r.samples = windowCount(as.fromSec, as.toSec);
        // No retained samples in the window: the assertion was not evaluated. Report that as
        // itself, never as a comparison against the 0.0 an empty window returns.
        r.evaluated = r.samples > 0;
        if (!r.evaluated) {
            snprintf(r.message, sizeof(r.message),
                     "NO DATA: %s over %us..%us held no retained samples (history covers "
                     "%us..%us, %lu overwritten, stride %us) %s",
                     metricName(as.metric), static_cast<unsigned>(as.fromSec),
                     static_cast<unsigned>(as.toSec), rep.histOldestSec, rep.histNewestSec,
                     rep.histDropped, histStride_, as.note ? as.note : "");
        } else {
            r.passed = observed >= as.low && observed <= as.high;
            if (!r.passed) fmtFail(r.message, sizeof(r.message), as, observed);
        }
        if (as.metric == Metric::PLAN_BOUNDED && r.evaluated && observed > 0.0f)
            rep.boundedPlanSeen = true;
        rep.item[rep.total++] = r;
        if (r.evaluated) {
            if (r.passed) rep.passed++;
        } else {
            rep.unevaluated++;
        }
    }
    if (snap_.noClick || snap_.clickNoGas) rep.solenoidFaultSeen = true;
    return rep;
}

}  // namespace sim
