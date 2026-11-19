#include "Zhl16c.h"

#include <math.h>

#ifdef CCR_PLAN_TRACE // debug-only: pio run -t test with -DCCR_PLAN_TRACE
#include <stdio.h>
#endif

namespace zhl {

// ZH-L16A coefficient arrays as shipped in Subsurface core/deco.cpp (a in bar, b dimensionless
// in this parameterisation, half-times in minutes). He half-times differ from N2 by design.
static const float kN2A[NCOMP] = {
    1.1696f, 1.0f,    0.8618f, 0.7562f, 0.62f,   0.5043f, 0.441f,  0.4f,
    0.375f,  0.35f,   0.3295f, 0.3065f, 0.2835f, 0.261f,  0.248f,  0.2327f,
};
static const float kN2B[NCOMP] = {
    0.5578f, 0.6514f, 0.7222f, 0.7825f, 0.8126f, 0.8434f, 0.8693f, 0.8910f,
    0.9092f, 0.9222f, 0.9319f, 0.9403f, 0.9477f, 0.9544f, 0.9602f, 0.9653f,
};
static const float kN2HT[NCOMP] = {
    5.0f,   8.0f,   12.5f,  18.5f, 27.0f,  38.3f,  54.3f,  77.0f,
    109.0f, 146.0f, 187.0f, 239.0f, 305.0f, 390.0f, 498.0f, 635.0f,
};
static const float kHeA[NCOMP] = {
    1.6189f, 1.383f,  1.1919f, 1.0458f, 0.922f,  0.8205f, 0.7305f, 0.6502f,
    0.595f,  0.5545f, 0.5333f, 0.5189f, 0.5181f, 0.5176f, 0.5172f, 0.5119f,
};
static const float kHeB[NCOMP] = {
    0.4770f, 0.5747f, 0.6527f, 0.7223f, 0.7582f, 0.7957f, 0.8279f, 0.8553f,
    0.8757f, 0.8903f, 0.8997f, 0.9073f, 0.9122f, 0.9171f, 0.9217f, 0.9267f,
};
static const float kHeHT[NCOMP] = {
    1.88f,   3.02f,   4.72f,   6.99f,   10.21f,  14.48f,  20.53f,  29.11f,
    41.20f,  55.19f,  70.69f,  90.34f,  115.29f, 147.42f, 188.24f, 240.03f,
};

float Engine::halfTimeN2Min(int i) { return (i >= 0 && i < NCOMP) ? kN2HT[i] : 0.0f; }
float Engine::halfTimeHeMin(int i) { return (i >= 0 && i < NCOMP) ? kHeHT[i] : 0.0f; }

void Engine::reset(float surfaceBar) {
    const float inspiredN2 = 0.79f * (surfaceBar - 0.0627f);
    for (int i = 0; i < NCOMP; ++i) {
        pn2_[i] = inspiredN2 > 0.0f ? inspiredN2 : 0.0f;
        phe_[i] = 0.0f;
    }
    gfLowPressure_ = surfaceBar;
    guidingComp_ = -1;
}

void Engine::setTissues(const float* n2, const float* he) {
    if (n2) for (int i = 0; i < NCOMP; ++i) pn2_[i] = n2[i];
    if (he) for (int i = 0; i < NCOMP; ++i) phe_[i] = he[i];
}

void Engine::blend(int ci, const float* n2, const float* he, Blend& out) const {
    out.sat = n2[ci] + he[ci];
    if (out.sat <= 1e-6f) {
        out.a = kN2A[ci];
        out.b = kN2B[ci];
        out.sat = 0.0f;
        return;
    }
    out.a = (kN2A[ci] * n2[ci] + kHeA[ci] * he[ci]) / out.sat;
    out.b = (kN2B[ci] * n2[ci] + kHeB[ci] * he[ci]) / out.sat;
}

// Pressure at which this compartment sits exactly at the given gradient factor.
float Engine::lowestCeilingOf(int ci, const Blend& bl, float gf) const {
    const float den = (1.0f - bl.b) * gf + bl.b;
    if (fabsf(den) < 1e-9f) return 0.0f;
    const float c = (bl.b * bl.sat - gf * bl.a * bl.b) / den;
    return c > 0.0f ? c : 0.0f;
}

// Tolerated ambient pressure for this compartment, using the GF ramp anchored at `anchor`.
// Mirrors Subsurface's tissue_tolerance_calc branch exactly.
float Engine::toleratedOf(int ci, const Blend& bl, float anchor, const Config& cfg) const {
    const float a = bl.a, b = bl.b, p = bl.sat;
    const float gfl = cfg.gfLow, gfh = cfg.gfHigh;
    const float ps = cfg.surfaceBar, pgl = anchor;

    const float lhs = (ps / b + a - ps) * gfh + ps;
    const float rhs = (pgl / b + a - pgl) * gfl + pgl;
    if (!(lhs < rhs)) return 0.0f;

    const float num = -a * b * (gfh * pgl - gfl * ps) -
                      (1.0f - b) * (gfh - gfl) * pgl * ps + b * (pgl - ps) * p;
    const float den = -a * b * (gfh - gfl) + (1.0f - b) * (gfl * pgl - gfh * ps) +
                      b * (pgl - ps);
    if (fabsf(den) < 1e-9f) return 0.0f;
    const float t = num / den;
    return t > 0.0f ? t : 0.0f;
}

float Engine::ceilingCopies(const float* n2, const float* he, float anchor, int* compOut,
                            const Config& cfg) const {
    if (anchor <= cfg.surfaceBar + 0.005f) {
        if (compOut) *compOut = -1;
        return 0.0f;
    }
    float best = 0.0f;
    int bestCi = -1;
    for (int ci = 0; ci < NCOMP; ++ci) {
        Blend bl{};
        blend(ci, n2, he, bl);
        if (bl.sat <= 0.0f) continue;
        const float t = toleratedOf(ci, bl, anchor, cfg);
        if (t > best) {
            best = t;
            bestCi = ci;
        }
    }
    if (best <= cfg.surfaceBar + 0.005f) best = 0.0f;
    if (compOut) *compOut = bestCi;
    return best;
}

void Engine::decayFor(float dtSec, Decay& out) {
    for (int ci = 0; ci < NCOMP; ++ci) {
        // Each gas moves with its own half-time.
        out.n2[ci] = 1.0f - expf(-0.69314718f * dtSec / (kN2HT[ci] * 60.0f));
        out.he[ci] = 1.0f - expf(-0.69314718f * dtSec / (kHeHT[ci] * 60.0f));
    }
}

void Engine::stepCopies(float* n2, float* he, const Inspired& insp, const Decay& dec,
                        const Config& cfg) const {
    for (int ci = 0; ci < NCOMP; ++ci) {
        const float fN2 = dec.n2[ci];
        const float fHe = dec.he[ci];

        const float n2Old = n2[ci];
        const float heOld = he[ci];
        n2[ci] += (insp.pn2 - n2[ci]) * fN2;
        he[ci] += (insp.phe - he[ci]) * fHe;
        if (n2[ci] < 0.0f) n2[ci] = 0.0f;
        if (he[ci] < 0.0f) he[ci] = 0.0f;

        // ICD guard: this rule (fast compartments, 80% ratio) is ours, not Subsurface's.
        // It is a heuristic and is deliberately switchable.
        if (cfg.icdGuard && he[ci] > heOld && n2[ci] < n2Old &&
            kN2HT[ci] <= cfg.fastCompMaxHalfTimeMin) {
            const float allowed = n2Old - cfg.icdRatio * (he[ci] - heOld);
            if (n2[ci] < allowed) n2[ci] = allowed;
        }
    }
}

void Engine::stepCopies(float* n2, float* he, const Inspired& insp, float dtSec,
                        const Config& cfg) const {
    Decay dec;
    decayFor(dtSec, dec);
    stepCopies(n2, he, insp, dec, cfg);
}

void Engine::step(const Inspired& insp, float ambBar, float dtSec, const Config& cfg) {
    stepCopies(pn2_, phe_, insp, dtSec, cfg);

    float worst = 0.0f;
    for (int ci = 0; ci < NCOMP; ++ci) {
        Blend bl{};
        blend(ci, pn2_, phe_, bl);
        if (bl.sat <= 0.0f) continue;
        const float c = lowestCeilingOf(ci, bl, cfg.gfLow);
        if (c > worst) worst = c;
    }
    if (worst > gfLowPressure_) gfLowPressure_ = worst;
}

float Engine::gfAt(float ambBar, const Config& cfg) const {
    if (gfLowPressure_ <= cfg.surfaceBar) return cfg.gfLow;
    const float g = (ambBar - cfg.surfaceBar) / (gfLowPressure_ - cfg.surfaceBar) *
                        (cfg.gfLow - cfg.gfHigh) + cfg.gfHigh;
    return g > cfg.gfLow ? g : cfg.gfLow;
}

float Engine::ceilingBar(const Config& cfg) const {
    int comp = -1;
    const float c = ceilingCopies(pn2_, phe_, gfLowPressure_, &comp, cfg);
    return c;
}

float Engine::ceilingM(const Config& cfg) const {
    const float c = ceilingBar(cfg);
    if (c <= 0.0f) return 0.0f;
    return depthOf(c, cfg);
}

float Engine::depthOf(float ambBar, const Config& cfg) const {
    return ambBar - cfg.surfaceBar > 0.0f ? (ambBar - cfg.surfaceBar) * cfg.mPerBar : 0.0f;
}

float Engine::ambAt(float depthM, const Config& cfg) const {
    return cfg.surfaceBar + depthM / cfg.mPerBar;
}

float Engine::ndlMin(const AscentGas& gas, float ambBar, const Config& cfg, float dtSec,
                     float maxMin) const {
    float n2[NCOMP], he[NCOMP];
    for (int i = 0; i < NCOMP; ++i) { n2[i] = pn2_[i]; he[i] = phe_[i]; }

    float anchor = gfLowPressure_;
    const Inspired hold =
        resolveInspired(gas.state, gas.diluent, gas.oxygen, gas.bailout, gas.selected, ambBar,
                        depthOf(ambBar, cfg), gas.gas);

    // Phased horizon. A flat 5 s step out to 999 min made every "NDL unlimited" tick pay a
    // full 11,880-iteration sweep — measured at 233 ms/tick on the ESP32 even with the decay
    // table hoisted, i.e. minutes for a short bench profile. Real NDLs live in the fine
    // region; past an hour the answer is "unlimited" for display purposes, and there the
    // integration step may be coarse without moving the reported minute materially (linear-
    // step integration error at these dt/tht ratios stays far below the minute grid).
    struct Phase { float horizonMin; float dt; };
    const float dtFine = dtSec > 0.0f ? dtSec : 5.0f;
    const Phase phases[] = {{5.0f, dtFine}, {60.0f, 15.0f}, {1.0e9f, 120.0f}};

    float elapsed = 0.0f;
    for (const Phase& ph : phases) {
        const float limit = ph.horizonMin < maxMin ? ph.horizonMin : maxMin;
        Decay dec;
        decayFor(ph.dt, dec);  // one step per phase: compute the factors once per phase
        while (elapsed < limit) {
            elapsed += ph.dt / 60.0f;
            stepCopies(n2, he, hold, dec, cfg);
            float worst = 0.0f;
            for (int ci = 0; ci < NCOMP; ++ci) {
                Blend bl{};
                blend(ci, n2, he, bl);
                if (bl.sat <= 0.0f) continue;
                const float c = lowestCeilingOf(ci, bl, cfg.gfLow);
                if (c > worst) worst = c;
            }
            if (worst > anchor) anchor = worst;
            if (ceilingCopies(n2, he, anchor, nullptr, cfg) > 0.0f) return elapsed;
        }
        if (elapsed >= maxMin) break;
    }
    return maxMin;
}

DecoPlan Engine::plan(const AscentGas& asc, float ambBar, const Config& cfg) const {
    DecoPlan out{};
    out.gfLowPressureBar = gfLowPressure_;

    float n2[NCOMP], he[NCOMP];
    for (int i = 0; i < NCOMP; ++i) { n2[i] = pn2_[i]; he[i] = phe_[i]; }
    const float anchor = gfLowPressure_;

    int comp = -1;
    float ceil = ceilingCopies(n2, he, anchor, &comp, cfg);
    out.ceilingBar = ceil;
    out.ceilingM = ceil > 0.0f ? depthOf(ceil, cfg) : 0.0f;
    out.controllingComp = comp;
    out.inDeco = ceil > 0.0f;
    lastPlanSteps_ = 0;
    if (!out.inDeco) return out;

    // Stop decisions are made in depth/grid space with one shared tolerance. Comparing
    // pressures with millibar epsilons in three separate places is what stalled the search:
    // a stop depth round-tripped through ambAt()/depthOf() lands a few ten-thousandths off, so
    // "the ceiling cleared the next rung" and "the stop is still this deep" were both true and
    // an iteration made zero progress. 5 cm is far below any resolution this model claims.
    const float kEpsM = 0.05f;
    const float kEpsP = kEpsM / cfg.mPerBar;
    const float stepM = cfg.stopStepM > 0.5f ? cfg.stopStepM : 3.0f;

    float amb = ambBar;
    float totalMin = 0.0f;
    int steps = 0;
    const int kMaxSteps = 20000;
    bool noProgress = false;

    auto roundStopDepth = [&](float depthM) -> float {
        float d = ceilf(depthM / stepM) * stepM;
        if (d < cfg.lastStopM) d = cfg.lastStopM;
        return d;
    };

    // The next shallower rung the diver may rise to, or 0.0f when there is none: the stop grid
    // has no rung between lastStopM and the surface.
    auto nextPermittedDepth = [&](float depthM) -> float {
        const float cand = depthM - stepM;
        if (cand < cfg.lastStopM - kEpsM) return 0.0f;
        return cand;
    };

    auto breatheAt = [&](float p) -> Inspired {
        return resolveInspired(asc.state, asc.diluent, asc.oxygen, asc.bailout, asc.selected, p,
                               depthOf(p, cfg), asc.gas);
    };

    // Fixed timesteps throughout the search: hoist the decay factors out of the loops.
    Decay ascDec, holdDec;
    decayFor(5.0f, ascDec);
    decayFor(10.0f, holdDec);

    auto ascendTo = [&](float targetP) {
        const float dP = cfg.ascentMPerMin / cfg.mPerBar * (5.0f / 60.0f);
        while (amb > targetP + kEpsP && steps++ < kMaxSteps) {
            float next = amb - dP;
            if (next < targetP) next = targetP;
            stepCopies(n2, he, breatheAt(next), ascDec, cfg);
            totalMin += 5.0f / 60.0f;
            amb = next;
        }
    };

    while (amb > cfg.surfaceBar + kEpsP && steps < kMaxSteps) {
        ceil = ceilingCopies(n2, he, anchor, &comp, cfg);
        if (ceil <= cfg.surfaceBar + kEpsP) break;

        // Stop floor: an obligation shallower than lastStopM is not a stop that can be made,
        // so the stop schedule ends here and the run to the surface is the rest of the plan.
        // Holding instead until the model is fully satisfied while breathing at the last stop
        // reloads the intermediate compartments and does not converge: measured 67 min at 6 m
        // for a 50 m / 20 min air dive, against ~10 min on a GF 30/70 computer. The diver's
        // choice of lastStopM *is* the choice of how much residual tolerance to leave unpaid;
        // out.ceilingM keeps the raw value so the display never hides it.
        if (ceil <= ambAt(cfg.lastStopM, cfg) + kEpsP) break;

        if (ceil > amb + kEpsP) out.violation = true;

        const float ambD = depthOf(amb, cfg);
        float targetD = roundStopDepth(depthOf(ceil, cfg));
        if (targetD > ambD) targetD = ambD; // ceiling deeper than we are: hold where we are

        if (targetD < ambD - kEpsM) {
            ascendTo(ambAt(targetD, cfg));
            continue;
        }

        // Standing on the rung the model asks for: hold until the ceiling rises to the next
        // shallower rung. At the last stop there is no shallower rung, so the target is this
        // depth and the stop is dropped when the ceiling reaches the stop floor.
        const float nextD = nextPermittedDepth(ambD);
        const float allowedP = nextD > 0.0f ? ambAt(nextD, cfg) : amb;
        const float holdDT = 10.0f;
        // A hold longer than this is not a deco stop, it is a search that cannot make progress
        // — tissue state pinned at a knife-edge comparison, a breathing state that reloads the
        // compartment it is trying to unload, or a tolerance band narrower than libm's noise
        // between platforms. Measured on-target: the same profile that clears natively drove the
        // ESP32's libm a few hundred microbar the other side of the "ceiling cleared the rung"
        // test at the 6 m stop, every tick then burned the whole step budget (tens of seconds of
        // CPU per tick on 240 MHz — indistinguishable from a hang at the bench), and nothing
        // downstream knew. The budget makes that state *reportable* instead of expensive.
        const float kMaxHoldMin = 240.0f;
        int heldSteps = 0;
        float heldCeil = ceil;
        bool holdCap = false;
        while (steps++ < kMaxSteps) {
            heldCeil = ceilingCopies(n2, he, anchor, &comp, cfg);
            if (heldCeil <= allowedP + kEpsP) break;
            if (heldSteps * holdDT / 60.0f >= kMaxHoldMin) { holdCap = true; break; }
            stepCopies(n2, he, breatheAt(amb), holdDec, cfg);
            totalMin += holdDT / 60.0f;
            heldSteps++;
#ifdef CCR_PLAN_TRACE
            if (heldSteps % 500 == 0)
                fprintf(stderr, "trace hold d=%.1f m amb=%.3f ceil=%.2f m allowed=%.3f comp=%d "
                                "t=%.0f min\n", ambD, amb, depthOf(heldCeil, cfg), allowedP, comp,
                                totalMin);
#endif
        }
        if (heldSteps > 0 && out.nStops < 16) {
            out.stopDepthM[out.nStops] = roundStopDepth(ambD);
            out.stopMin[out.nStops] = heldSteps * holdDT / 60.0f;
            out.nStops++;
        }
        if (holdCap) { noProgress = true; break; }

        // The hold ended because the obligation moved shallower: take that rung now instead of
        // re-deriving it from a freshly rounded ceiling. This makes progress structural.
        if (nextD > 0.0f && heldCeil <= allowedP + kEpsP) {
            ascendTo(ambAt(nextD, cfg));
            continue;
        }

        // Safety net for a future edit: an iteration that neither moved shallower nor consumed
        // time would repeat forever. Report the plan as untrusted rather than spinning.
        if (heldSteps == 0) {
            noProgress = true;
            break;
        }
    }

    // Final leg to the surface. Skipped when the search bailed out: publishing a clean ascent
    // we never validated would be a silent fallback (AGENTS.md 17).
    const bool capped = steps >= kMaxSteps;
    if (!capped && !noProgress) {
        const float dP = cfg.ascentMPerMin / cfg.mPerBar * (5.0f / 60.0f);
        while (amb > cfg.surfaceBar + 0.001f && steps++ < kMaxSteps) {
            float next = amb - dP;
            if (next < cfg.surfaceBar) next = cfg.surfaceBar;
            stepCopies(n2, he, breatheAt(next), ascDec, cfg);
            totalMin += 5.0f / 60.0f;
            amb = next;
        }
    }

    out.bounded = capped || noProgress;
    out.ttsMin = totalMin;
    out.inDeco = out.nStops > 0 || out.violation;
    lastPlanSteps_ = steps;
    return out;
}

} // namespace zhl
