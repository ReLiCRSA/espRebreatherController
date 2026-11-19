#include "LoopModel.h"

#include <math.h>

namespace sim {

// R*T in L*bar per mole, for the loop's gas bookkeeping. The inventory is tracked in moles because
// O2 is consumed and injected at different pressures during a dive; tracking a fraction instead
// would silently gain or lose gas on every descent and still look like a stable setpoint.
static float loopRt(const LoopConfig& c) {
    const float tK = c.tempC + 273.15f;
    return 0.0831446f * tK;
}

void LoopModel::reset() {
    ppO2_ = 0.21f;
    fHe_ = 0.0f;
    injectedAmbL_ = 0.0f;
    pulsesTotal_ = 0;
}

void LoopModel::setPpO2(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;   // dry ppO2 cannot exceed the dry ambient; callers own the ambient
    ppO2_ = v;
}

float LoopModel::loopFo2(float ambBar) const {
    const float dry = ambBar - config.pH2OBar;
    if (dry <= 0.0f) return 0.0f;
    float f = ppO2_ / dry;
    return f > 1.0f ? 1.0f : f;
}

void LoopModel::setFo2(float fo2, float ambBar) {
    const float dry = ambBar - config.pH2OBar;
    if (dry <= 0.0f) return;
    if (fo2 < 0.0f) fo2 = 0.0f;
    if (fo2 > 1.0f) fo2 = 1.0f;
    ppO2_ = fo2 * dry;   // the same expression as expectedPpO2ForFlush(), deliberately
}

LoopStepResult LoopModel::step(float dtSec, float ambBar, float batteryVolt, float ppO2Used,
                               float setpointBar, CcrControlLaw& control,
                               const ControlConfig& controlCfg, ValveModel& valve) {
    LoopStepResult res;
    if (dtSec <= 0.0f) return res;
    res.ppO2Before = ppO2_;

    // 1. The control law decides on the **voted** ppO2 — never on a single cell and never on the
    //    loop's internal truth. That is what turns a cell fault into a control problem, which is
    //    exactly what a simulator has to be able to show (AGENTS.md 5, 11).
    res.control = control.compute(ppO2Used, setpointBar, dtSec, batteryVolt, controlCfg, valve);
    res.pulsesCommanded = res.control.pulses;
    res.supplyLimited = res.control.supplyLimited;
    pulsesTotal_ += static_cast<unsigned long>(res.pulsesCommanded);

    // 2. The valve decides what those pulses actually delivered, and what the shunt saw. Pulse
    //    granularity is real: at 0.15 ambient L each, injection is quantised and ppO2 will show it.
    float deliveredAmbL = 0.0f;
    for (int i = 0; i < res.pulsesCommanded; ++i) {
        ClickSignature sig;
        const float gas = valve.pulse(res.control.widthMs, batteryVolt, ambBar, &sig);
        if (sig.present) {
            res.pulsesClicked++;
            res.lastClick = sig;
        }
        deliveredAmbL += gas;
    }
    res.gasDeliveredAmbL = deliveredAmbL;
    if (res.pulsesCommanded > 0 && res.pulsesClicked == 0) res.noClickWhileCommanded = true;
    // Click present, no gas delivered: the one fault the shunt cannot catch, so it has to come from
    // ppO2 not moving. Flagged here for the alarm layer to raise SOLENOID_FAIL.
    if (res.pulsesClicked > 0 && deliveredAmbL <= 0.0f) res.clickedButNoGas = true;
    injectedAmbL_ += deliveredAmbL;

    // 3. Integrate. Metabolism is a **molar** draw quoted in surface litres, so it does not scale
    //    with ambient pressure; injection is in ambient litres, so it does. Getting these two the
    //    same way round is the trap this file exists to document: multiplying the metabolic term by
    //    ambBar quadruples the diver's O2 consumption at 30 m, the loop still holds setpoint (the
    //    control law just injects 4x too much), and every gas-consumption figure the sim prints is
    //    quietly wrong. Check by hand: 1.2 surf L/min into 8 L sags ppO2 at 0.15 bar/min at any
    //    depth. In open circuit the loop is out of the breathing path, so the inspired value is the
    //    mix's — the caller passes it in `setpointBar`, which for a non-closed state is
    //    resolveInspired().po2.
    const float rt = loopRt(config);
    if (!config.openCircuit) {
        float moles = ppO2_ * config.loopVolumeL / rt;
        const float metabSurfL = config.metabolicRateSurfLpm / 60.0f * dtSec;
        moles -= metabSurfL * config.surfRefBar / rt;
        moles += deliveredAmbL * ambBar / rt;
        if (moles < 0.0f) moles = 0.0f;
        ppO2_ = moles * rt / config.loopVolumeL;
    } else {
        ppO2_ = setpointBar < 0.0f ? 0.0f : setpointBar;
    }
    if (ppO2_ < 0.0f) ppO2_ = 0.0f;

    res.ppO2 = ppO2_;
    return res;
}

void LoopModel::diluentFlush(float diluentFo2, float ambBar) {
    const float eff = config.flushEffectiveness;
    const float blended = diluentFo2 * eff + loopFo2(ambBar) * (1.0f - eff);
    setFo2(blended, ambBar);
}

void LoopModel::o2Flush(float o2Fo2, float ambBar) {
    const float eff = config.flushEffectiveness;
    const float blended = o2Fo2 * eff + loopFo2(ambBar) * (1.0f - eff);
    setFo2(blended, ambBar);
}

void LoopModel::breatheMix(float mixFo2, float ambBar) { setFo2(mixFo2, ambBar); }

}  // namespace sim
