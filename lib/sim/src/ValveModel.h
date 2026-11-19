// Injection valve: pulse -> gas delivered, and pulse -> coil-current signature (AGENTS.md 8).
//
// This one file carries both halves on purpose. The design's primary safety net is **click
// verification from the shunt**, and the three faults it has to distinguish differ only in the
// relationship between the electrical signature and the gas delivered:
//
//   SOLENOID_NO_CLICK    commanded pulse, no current signature (coil open, FET dead)  -> no gas
//   SOLENOID_SLOW_CLICK  damped signature (fouling, low voltage, failing valve)        -> some gas
//   SOLENOID_FAIL        click present but ppO2 unchanged (supply empty, frozen reg)   -> no gas
//
// If gas delivery and the current signature were modelled in different places, the third case — the
// one that *cannot* be detected electrically — would be impossible to simulate, and the taxonomy
// would be untestable. Keeping them in one model is what makes the distinction assertable.
//
// The physics, as designed: the coil is driven **directly from the battery rail, no boost stage**,
// by fixed-width voltage pulses at a variable rate (decision 12 — inductive CCR valves do not meter
// gas by duty cycle, they need full voltage to open repeatably against a seat). So a pulse either
// delivers its calibrated volume or it does not, and whether it does depends on the **volt-seconds**
// applied: width x V_bat. That is why firmware compensates width against battery voltage: a valve
// that opens in 60 ms at 4.1 V may need 120 ms at 3.4 V.
#pragma once

#include <stdint.h>

namespace sim {

enum class ClickState : uint8_t { NO_CLICK = 0, DAMPED, HEALTHY };

// What the coil shunt would show for one commanded pulse. Peak in amps, rise in milliseconds,
// charge in amp-milliseconds. Deliberately plain numbers: M5's click thresholds compare against
// these, and the fault is defined by which of them is missing.
struct ClickSignature {
    bool  present = false;
    float peakA = 0.0f;
    float riseMs = 0.0f;
    float chargeAsMs = 0.0f;
};

struct SolenoidFault {
    bool noClick = false;         // coil open / FET dead: electrically silent
    bool damped = false;          // fouling or a failing valve: slow, low peak
    bool clickButNoGas = false;   // supply empty or frozen regulator: electrically normal, no O2
};

struct ValveParams {
    float nominalVolt = 3.7f;      // 1S2P LiPo mid-rail, the voltage widths are calibrated at
    float coilOhms = 3.5f;         // hot coil, typical for a small pilot valve
    float coilHenry = 0.025f;      // used only for the rise-time signature
    float minOpenVoltSec = 0.185f; // ~50 ms at 3.7 V: below this the valve does not open reliably
    float pulseVolAmbL = 0.15f;    // per-pulse delivered volume, ambient L (bench-calibrated)
    float maxClickPeakA = 1.3f;    // signature ceiling: the current can't exceed V_bat/R_coil
};

class ValveModel {
  public:
    ValveParams params;
    void setParams(const ValveParams& p) { params = p; }
    void setFault(const SolenoidFault& f) { fault_ = f; }
    const SolenoidFault& fault() const { return fault_; }
    void clearFault() { fault_ = SolenoidFault{}; }

    // Deliver one commanded pulse.
    //   widthMs     the voltage-compensated width the drive stage actually applied
    //   batteryVolt V_bat at the moment of the pulse, because that decides whether it opened
    //   ambBar      delivered volume is an ambient-litre figure, so it is pressure-independent
    //               here; the loop converts to moles against its own pressure
    //   sigOut      what the shunt saw, for click verification (may be null)
    // Returns O2 delivered in **ambient litres** (0 when the valve did not open).
    float pulse(float widthMs, float batteryVolt, float ambBar, ClickSignature* sigOut = nullptr);

    // Voltage compensation (AGENTS.md 8), exposed as a free function of the parameters so a test can
    // assert the arithmetic and so M5's drive stage can call exactly this.
    //   width_used = width_table * V_nom / V_bat, clamped to [minMs, maxMs]
    float compensatedWidthMs(float widthTableMs, float batteryVolt, float minMs, float maxMs) const;

    // A valve that has stopped being commanded should not hold a stale signature in the model.
    void reset() { fault_ = SolenoidFault{}; }

  private:
    SolenoidFault fault_;
};

}  // namespace sim
