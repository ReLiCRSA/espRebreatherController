// The breathing loop as a physical system, driven by the firmware's own commanded pulses
// (AGENTS.md 11).
//
// This is the difference between a simulator and a script. ppO2 is not a number the profile supplies;
// it is what falls out of metabolic O2 consumption against the O2 the control law actually injected
// through a valve model that can refuse to open. Get the deadband wrong and you see the oscillation;
// get the pulse width too short for the battery and ppO2 sags and the low-ppO2 alarm fires — which is
// precisely how a bad control law gets caught before water.
//
// Pressure convention, important and easy to get silently wrong: **the loop is tracked in inspired
// (dry) ppO2**, i.e. the quantity an electrogalvanic cell sees at its membrane and the quantity a
// setpoint refers to. That makes loop ppO2, `GasModel`'s closed-circuit `po2` and
// `expectedPpO2ForFlush()` all the same number, so the control law, the cell model and the diluent
// flush check agree by construction instead of by a correction factor nobody checks. Gas fraction in
// the wet loop gas follows as fo2 = ppO2 / (Pamb - pH2O).
//
// Honest limits (AGENTS.md 19 risk 12): `metabolicRateSurfLpm` and `loopVolumeL` are unmeasured
// defaults. The dynamics here tune the control law; they do not validate the loop.
//
// Unit warning, because this is where a loop model goes quietly wrong: a **surface** litre is a
// molar amount (depth-independent) and an **ambient** litre is a volume at the loop's pressure
// (depth-scaled). Metabolism is the first, valve pulse volume is the second. Note that R*T cancels
// in the final arithmetic — ppO2_new = ppO2 - metabSurf*Psurf/V + deliveredAmb*Pamb/V — so the
// loop temperature cannot move a ppO2 figure, but the pressure a litre is quoted at absolutely can.
#pragma once

#include "CcrControlLaw.h"
#include "ValveModel.h"

namespace sim {

struct LoopConfig {
    float loopVolumeL = 8.0f;         // unmeasured default (AGENTS.md 20 open item 5)
    // O2 consumption, in **surface (STPD) litres per minute** — a molar rate, so it does *not*
    // depend on depth. This is the convention metabolic rate is always quoted in, and the trap to
    // avoid: expressed in *ambient* litres the same draw shrinks with depth (1.2 surf L/min is
    // 0.3 amb L/min at 30 m), and a model that multiplies a surface rate by ambient pressure
    // quadruples the diver's metabolism at 30 m while still holding setpoint. Hand-check anchor:
    // 1.2 surf L/min into an 8 L loop sags ppO2 at 1.2/8 = **0.15 bar/min at any depth**.
    float metabolicRateSurfLpm = 1.2f;  // unmeasured default, workload-dependent (open item 5)
    float pH2OBar = 0.0627f;          // wet loop gas: what a fraction refers to
    float surfRefBar = 1.013f;        // the pressure a "surface litre" is quoted at (STPD-ish)
    float tempC = 20.0f;              // loop gas temperature, used for the mole bookkeeping
    float flushEffectiveness = 1.0f;  // 1 = a flush fully replaces the loop gas
    bool  openCircuit = false;        // loop not in the breathing path: ppO2 tracks the mix
};

struct LoopStepResult {
    int   pulsesCommanded = 0;
    int   pulsesClicked = 0;     // pulses the shunt saw current for
    float gasDeliveredAmbL = 0.0f;
    float ppO2Before = 0.0f;
    float ppO2 = 0.0f;
    bool  clickedButNoGas = false;  // a click was seen and ppO2 did not move: SOLENOID_FAIL
    bool  noClickWhileCommanded = false;
    bool  supplyLimited = false;
    ClickSignature lastClick;
    ControlOutput control;
};

class LoopModel {
  public:
    LoopConfig config;
    void setConfig(const LoopConfig& c) { config = c; }
    const LoopConfig& loopConfig() const { return config; }

    // Rigid-ish counterdiffusion assumption: the inert composition follows the diluent, and O2 is
    // the only species the loop integrates. Good enough to exercise the control law; not a model of
    // a diluent-counterdiffused loop (AGENTS.md 11 keeps that honesty explicit).
    void reset();

    // --- loop state ------------------------------------------------------------------
    // Inspired (dry) ppO2, the number the cells and the setpoint both refer to.
    float ppO2() const { return ppO2_; }
    void setPpO2(float v);
    // Wet gas fraction of O2 in the loop at this ambient pressure.
    float loopFo2(float ambBar) const;

    void setFo2(float fo2, float ambBar);   // sets the loop's O2 fraction, wet-gas convention
    float loopVolumeL() const { return config.loopVolumeL; }

    // Total injected O2 since reset, ambient litres: what the pulse counter accounts for.
    float injectedAmbL() const { return injectedAmbL_; }
    unsigned long pulsesTotal() const { return pulsesTotal_; }

    // --- one control tick ------------------------------------------------------------
    // The loop's ppO2 is advanced by metabolism against whatever `valve` delivered for the pulses
    // that `control` asked for. This is the closed loop: control decision -> valve physics -> gas
    // -> cells -> vote -> control decision.
    LoopStepResult step(float dtSec, float ambBar, float batteryVolt, float ppO2Used,
                        float setpointBar, CcrControlLaw& control, const ControlConfig& controlCfg,
                        ValveModel& valve);

    // Flush events. In a real loop these are manual, immediate and large; in the sim they are one
    // call each, and the point is that the *cells* then have to agree with the diluent's expected
    // ppO2 — the independent check voting cannot fake (AGENTS.md 5).
    //
    // They take ambient explicitly because a fraction and a dry ppO2 are different numbers, and the
    // conversion is exactly where a water-vapour bug hides: expected ppO2 for a flush of an `fo2`
    // mix is `fo2 * (Pamb - pH2O)`, which is the same expression `expectedPpO2ForFlush()` uses.
    void diluentFlush(float diluentFo2, float ambBar);
    void o2Flush(float o2Fo2, float ambBar);

    // Open circuit: the loop is out of the breathing path, so the inspired ppO2 is whatever the mix
    // gives at this ambient. The loop still exists physically and its cells still read it, which is
    // why a bailout or OC transition is a state change and not a re-tune (AGENTS.md 8).
    void breatheMix(float mixFo2, float ambBar);

    // Helium share of the gas being breathed in the loop. Only used for bookkeeping/inspection: O2
    // is the species the loop integrates.
    void setBreathedHeliumFraction(float fhe) { fHe_ = fhe < 0.0f ? 0.0f : fhe; }
    float breathedHeliumFraction() const { return fHe_; }

  private:
    float ppO2_ = 0.21f;   // inspired (dry) ppO2 — the cell-domain and setpoint-domain number
    float fHe_ = 0.0f;     // helium fraction of the diluent in the loop
    float injectedAmbL_ = 0.0f;
    unsigned long pulsesTotal_ = 0;
};

}  // namespace sim
