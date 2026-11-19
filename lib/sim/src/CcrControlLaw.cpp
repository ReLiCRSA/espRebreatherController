#include "CcrControlLaw.h"

namespace sim {

ControlOutput CcrControlLaw::compute(float ppO2Used, float setpointBar, float dtSec,
                                     float batteryVolt, const ControlConfig& cfg,
                                     const ValveModel& valve) {
    ControlOutput out;
    if (dtSec <= 0.0f) return out;

    out.errorBar = setpointBar - ppO2Used;

    // Width comes from the battery-voltage table, never from the control gain: the valve needs full
    // voltage to crack its seat, so the *only* lever on a per-pulse basis is width x V_bat
    // (AGENTS.md 8). Without this the injected volume drifts as the pack empties.
    out.widthMs = valve.compensatedWidthMs(cfg.widthTableMs, batteryVolt, cfg.widthMinMs,
                                           cfg.widthMaxMs);
    // Overshoot guard first: if we are already above target, dumping the integral stops the loop
    // from chasing its own tail on the next descent.
    if (out.errorBar <= -cfg.overshootCutBar) {
        integralAmbLps_ = 0.0f;
        pulseResidue_ = 0.0f;
        return out;  // no pulses, and no stale demand carried forward
    }

    if (cfg.deadbandBar > 0.0f && out.errorBar < cfg.deadbandBar) {
        // Inside the band: no pulses. The leak below still relaxes the integrator, so a long steady
        // stretch does not leave a stale demand parked in the loop.
        integralAmbLps_ *= (dtSec > 0.0f ? 1.0f / (1.0f + cfg.leakPerSec * dtSec) : 1.0f);
        return out;
    }

    // PI in the delivered-rate domain (ambient L/s at 1 bar). The proportional term supplies the
    // steady metabolic demand; the leak is what stops the integrator winding up through a descent,
    // when ppO2 rises on its own and the loop legitimately wants nothing.
    const float p = cfg.kiBarPerBarSec * out.errorBar;
    integralAmbLps_ = (integralAmbLps_ + cfg.kiBarPerBarSec * out.errorBar * dtSec) /
                      (1.0f + cfg.leakPerSec * dtSec);
    out.desiredRateAmbLps = p + integralAmbLps_;
    if (out.desiredRateAmbLps > cfg.maxRateAmbLps) out.desiredRateAmbLps = cfg.maxRateAmbLps;
    if (out.desiredRateAmbLps < 0.0f) out.desiredRateAmbLps = 0.0f;

    // What the pulse train can actually deliver this tick, in whole pulses. Pulse granularity is
    // not noise here: at 0.15 ambient L per pulse a one-pulse-per-tick loop quantises injection,
    // and a control law that pretends it is continuous will report a smooth ppO2 the valve cannot
    // produce.
    const float vol = valve.params.pulseVolAmbL > 0.0f ? valve.params.pulseVolAmbL : 0.001f;
    float pps = out.desiredRateAmbLps / vol;
    const float ppsCap = cfg.valveMaxPpm / 60.0f;
    if (pps > ppsCap) {
        pps = ppsCap;
        out.supplyLimited = true;  // more O2 wanted than the valve can pass: alarm, not a shrug
    }
    out.rateAmbLps = pps * vol;

    int pulses = static_cast<int>(pps * dtSec + 0.5f);
    // A loop that wants O2 but rounds to zero pulses must still get one eventually; otherwise a
    // low-demand steady state silently starves the diver. The ppO2 will sag until it does.
    if (pulses == 0 && out.desiredRateAmbLps > 0.0f) {
        pulseResidue_ += pps * dtSec;
        if (pulseResidue_ >= 1.0f) {
            pulses = static_cast<int>(pulseResidue_);
            pulseResidue_ -= static_cast<float>(pulses);
        }
    }
    if (pulses < 0) pulses = 0;

    out.pulses = cfg.enabled ? pulses : 0;
    out.wouldInject = !cfg.enabled && pulses > 0;  // bench "would inject n%" display (AGENTS.md 8)

    // Exponentially smoothed pulse rate for display/logging (pulses per minute).
    const float ppmNow = dtSec > 0.0f ? static_cast<float>(pulses) * 60.0f / dtSec : 0.0f;
    const float alpha = 1.0f / (1.0f + 10.0f / (60.0f * dtSec));  // ~10 s time constant
    pulsesPerMin_ += (ppmNow - pulsesPerMin_) * alpha;
    return out;
}

}  // namespace sim
