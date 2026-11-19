// Simulated barometer at the sensor layer (AGENTS.md 11).
//
// It implements the same `Barometer` interface as the MS5837/BMP085 drivers, so a sim run still
// exercises depth smoothing, learned surface pressure, salinity conversion and the range clamp —
// not just the deco maths. There is no pressure chamber on the bench, so this class is also what
// lets real galvanic cells be exercised at depth: real cells + simulated ambient is the MIXED case
// in AGENTS.md 11, and it is the only way to reach setpoint crossover, MOD, the hypoxic floor,
// diluent flush at depth and the ICD guard before the hardware exists.
#pragma once

#include "GasMix.h"   // ambientAtDepth / depthForAmbient / metresPerBar — one conversion, not three
#include "SensorIf.h"

namespace sim {

class SimBaro : public Barometer {
  public:
    SimBaro() { surfaceBar_ = kStandardSurfaceBar; }

    // The profile drives depth directly; the dive loop still converts through the same code as the
    // real sensor, including the learned-surface-pressure offset.
    void setDepthM(float d) { depthM_ = d; }
    float depthM() const { return depthM_; }

    void setSurfaceBar(float bar) { surfaceBar_ = bar; }
    float surfaceBar() const { return surfaceBar_; }

    // Fresh 1.000 / salt 1.025 (AGENTS.md 2). Learned via the same helper the firmware uses.
    void setSalinityKgM3(float kgM3) { mPerBar_ = metresPerBar(kgM3); }
    // Or set the gradient directly — this is what the dive-loop config (zhl::Config::mPerBar)
    // already carries, and round-tripping it through a density makes two numbers that must be
    // equal differ by rounding.
    void setMetresPerBar(float m) { mPerBar_ = m > 0.0f ? m : 10.197f; }
    float metresPerBarValue() const { return mPerBar_; }

    void setTempC(float t) { tempC_ = t; }

    // Model the BMP085 fallback's ceiling (~300-1100 hPa => ~9 m). Anything deeper reads as a
    // constant at the ceiling, exactly as the real part does — which is how the 9 m banner and the
    // clamp get tested without a second barometer on the bench.
    void setRangeCeilingBar(float bar) { ceilingBar_ = bar; }
    float rangeCeilingBar() const override { return ceilingBar_; }

    bool init() override { return true; }

    bool read(float* ambBar, float* tempC) override {
        float p = ambientAtDepth(depthM_, surfaceBar_, mPerBar_);
        // Clamp, never extrapolate (AGENTS.md 19 risk 1). A sensor that saturates must report the
        // saturation, and the firmware is expected to notice and say so out loud.
        if (ceilingBar_ > 0.0f && p > ceilingBar_) p = ceilingBar_;
        if (ambBar) *ambBar = p;
        if (tempC) *tempC = tempC_;
        return true;
    }

    SourceKind source() const override { return SourceKind::SIM; }

    static constexpr float kStandardSurfaceBar = 1.013f;

  private:
    float depthM_ = 0.0f;
    float surfaceBar_ = kStandardSurfaceBar;
    float mPerBar_ = 10.197f;   // fresh by default, matching metresPerBar(1000)
    float tempC_ = 20.0f;
    float ceilingBar_ = 0.0f;   // 0 = no ceiling reported (MS5837-class)
};

}  // namespace sim
