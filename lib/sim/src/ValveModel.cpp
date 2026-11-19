#include "ValveModel.h"

#include <math.h>

namespace sim {

float ValveModel::compensatedWidthMs(float widthTableMs, float batteryVolt, float minMs,
                                     float maxMs) const {
    if (batteryVolt <= 0.0f) return maxMs;  // no rail: the widest pulse is the safest guess
    float w = widthTableMs * params.nominalVolt / batteryVolt;
    if (w < minMs) w = minMs;
    if (w > maxMs) w = maxMs;
    return w;
}

float ValveModel::pulse(float widthMs, float batteryVolt, float ambBar, ClickSignature* sigOut) {
    (void)ambBar;  // ambient litres are pressure-independent by definition
    const float widthSec = widthMs / 1000.0f;

    // An open coil or a dead FET draws nothing, and nothing is delivered. This is the only fault
    // that *is* detectable electrically — which is exactly why it is worth a click check.
    if (fault_.noClick) {
        if (sigOut) *sigOut = ClickSignature{};
        return 0.0f;
    }

    if (batteryVolt <= 0.0f || widthSec <= 0.0f) {
        if (sigOut) *sigOut = ClickSignature{};
        return 0.0f;
    }

    // Inductive coil on a voltage step: i(t) = (V/R)(1 - e^(-Rt/L)). The signature and the opening
    // decision both come from this, from one physical model rather than from three separate fudges.
    const float r = params.coilOhms > 0.0f ? params.coilOhms : 1.0f;
    const float l = params.coilHenry > 0.0f ? params.coilHenry : 0.001f;
    const float tauSec = l / r;

    const float iMaxRail = batteryVolt / r;
    float iPeak = iMaxRail * (1.0f - expf(-widthSec / tauSec));
    float tauEff = tauSec;
    if (fault_.damped) {
        // Fouling / a tired armature: the current still rises, but slower and lower. The diver's
        // response to this fault is different from a no-click, so the signature must differ too.
        iPeak *= 0.70f;
        tauEff *= 2.5f;
    }
    if (iPeak > params.maxClickPeakA) iPeak = params.maxClickPeakA;

    if (sigOut) {
        sigOut->present = true;
        sigOut->peakA = iPeak;
        // Time to 63% of final value, in ms — what the M5 driver measures as "rise".
        sigOut->riseMs = tauEff * 1000.0f;
        // Integral of i(t) over the pulse, in A*ms: charge delivered to the coil.
        const float chargeSec = (batteryVolt / r) *
                                (widthSec - tauSec * (1.0f - expf(-widthSec / tauSec)));
        sigOut->chargeAsMs = (fault_.damped ? chargeSec * 0.70f : chargeSec) * 1000.0f;
    }

    // Does it open? A seat needs a minimum volt-second product; below that it chatter-and-misses,
    // which is the failure the voltage compensation exists to prevent.
    const float voltSec = batteryVolt * widthSec;
    if (voltSec < params.minOpenVoltSec) return 0.0f;

    // A click that delivers no gas: electrically indistinguishable, which is the point. Only the
    // loop's ppO2 can reveal it, and only if something is watching ppO2 after each pulse.
    if (fault_.clickButNoGas) return 0.0f;

    if (fault_.damped) return params.pulseVolAmbL * 0.60f;
    return params.pulseVolAmbL;
}

}  // namespace sim
