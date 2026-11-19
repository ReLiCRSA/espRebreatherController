// Reference CCR setpoint control law (AGENTS.md 8, 11).
//
// Read the name carefully: this is the **reference** law, not the flight controller. M5's
// `src/dive/CcrControl` adds the parts that only exist on the board — the ADS1115 conversion-window
// interlock, the low-voltage lockout, the click-fault handling and the watchdog. What lives here is
// the arithmetic both the firmware and the simulator must agree on, and the reason it is in `lib/`
// is the design requirement that the simulator run closed loop against **the firmware's own
// commanded pulses**. A sim with its own fake controller would prove nothing about a bad control
// law, which is the whole point of AGENTS.md 11.
//
// Drive law is **fixed-width voltage pulses at a variable rate** (decision 12): an inductive CCR
// valve does not meter gas by duty cycle. So the control output is a pulse count for this tick, and
// the pulse width comes from the battery-voltage compensation table, not from the controller's gain.
//
// Injected O2 is tracked as pulses x the per-valve calibration constant (AGENTS.md 8): the loop
// model consumes that number, so the sim's ppO2 is a consequence of what the firmware commanded.
#pragma once

#include "ValveModel.h"

namespace sim {

struct ControlConfig {
    float deadbandBar = 0.03f;      // inside this, do nothing — protects the valve and the log
    float kiBarPerBarSec = 1.20f;   // ppO2 error (bar) -> desired injection (ambient L/s at 1 bar)
    float leakPerSec = 0.50f;       // integrator leak: bounds windup through a descent
    float maxRateAmbLps = 0.20f;    // loop-side ceiling on O2 delivery
    float valveMaxPpm = 120.0f;     // AGENTS.md 2: >=120 pulses/min, 25% duty max
    float widthTableMs = 60.0f;     // width from the table, at the nominal rail
    float widthMinMs = 20.0f;
    float widthMaxMs = 250.0f;
    float overshootCutBar = 0.15f;  // stop injecting this far above setpoint
    bool  enabled = true;           // false = solenoid disabled: compute "would inject", touch no pin
};

struct ControlOutput {
    int   pulses = 0;          // pulses to fire this tick (0 when inside the deadband)
    float widthMs = 0.0f;      // voltage-compensated width the drive stage should apply
    float desiredRateAmbLps = 0.0f;  // what the loop asked for, before pulse-granularity rounding
    float rateAmbLps = 0.0f;   // what the pulse train can actually deliver
    bool  supplyLimited = false;  // asked for more than the valve can give: a real alarm condition
    bool  wouldInject = false;    // true when disabled but the law wanted pulses (bench display)
    float errorBar = 0.0f;
};

class CcrControlLaw {
  public:
    // setpointBar is the *inspired* target for this depth (GasModel::setpointAt handles the
    // deep/shallow crossover); ppO2Used is the voted value, never a single cell.
    ControlOutput compute(float ppO2Used, float setpointBar, float dtSec, float batteryVolt,
                          const ControlConfig& cfg, const ValveModel& valve);

    void reset() {
        integralAmbLps_ = 0.0f;
        pulseResidue_ = 0.0f;
        pulsesPerMin_ = 0.0f;
    }
    float pulsesPerMin() const { return pulsesPerMin_; }
    float integralAmbLps() const { return integralAmbLps_; }

    // Injected O2 accounted as pulses x calibration constant (AGENTS.md 8).
    static float injectedAmbL(int pulses, float pulseVolAmbL) {
        return static_cast<float>(pulses) * pulseVolAmbL;
    }

  private:
    // Loop state. The integrator and the sub-pulse residue are part of the closed loop, so they
    // belong to the controller rather than to the caller — and both have to be reset on a gas
    // switch or a mode change, exactly as M5's flight controller will.
    float integralAmbLps_ = 0.0f;
    float pulseResidue_ = 0.0f;
    float pulsesPerMin_ = 0.0f;
};

}  // namespace sim
