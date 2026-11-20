// Depth from ambient pressure: learned surface pressure, salinity, and the sensor-range clamp
// (AGENTS.md 2, 7, M3). Pure C++ so test/test_depth pins the arithmetic natively (decision 28's
// pattern — every constant that only lives in src/ is a constant nobody can assert).
//
// Physics, stated so the units are never a rumour again (risk 20 bit the gas path the same way):
//   h = (Pamb - Psurf) / (rho * g),  rho = specificGravity * 1000 kg/m3, g = 9.80665 m/s2.
// Hydrostatics by definition — a learned Psurf means the barometer's *absolute* offset cancels
// and never enters depth (AGENTS.md 2: "only relative accuracy matters", proven on the bench
// BMP180, whose -9...+3 hPa absolute offset against Stockholm stations is invisible to depth).
// The constant-rho assumption is the standard dive-computer one; AGENTS.md 7 makes salinity
// configurable (fresh 1.000, salt 1.025) and nothing here pretends below that.
//
// Psurf is LEARNED, not assumed. A hard-coded 1013.25 mbar is the same error class as the
// wrong-city barometric reference that nearly condemned a good BMP180 (risk 21, item 18): a
// reference is a measurement with a place, an hour and a source. Here the source is the fitted
// barometer itself, sampled at the surface until it is steady.
//
// The clamp is risk 1 in code — and note what the clamp is derived FROM: the fitted part's
// absolute upper limit (Barometer::rangeCeilingBar()), not from the design record's prose.
// Writing this file derived a number that exposed a 10x error in that prose: "BMP085 fallback
// ~9 m" cannot be true, because 1100 mbar absolute minus ~1013 mbar surface is ~87 mbar gauge
// and 1 mbar of water is ~1.02 cm, so the real fallback ceiling is UNDER 1 m at sea level
// (~1.2 m at a 978 mbar bench). test_depth pins the arithmetic; decision 38 corrects the record.
// `clamped` is true whenever the clamp engages, the raw gauge delta is returned alongside, and
// the caller must say so out loud (no silent fallbacks, AGENTS.md 17).
#pragma once

#include <stdint.h>
#include <math.h>

namespace depth {

// Standard gravity. Not 9.81 and not 9.8 — the number is named, so a test can disagree with it
// on purpose instead of by accident.
constexpr float kStdGravity = 9.80665f;

// Metres of water per bar of gauge pressure for a given fluid specific gravity
// (fresh 1.000 -> 10.197 m/bar; salt 1.025 -> 9.948 m/bar — i.e. 10 m of seawater is
// 0.9806 bar, the diver-rule "10 m per bar" to well under half a percent).
inline float metersPerBar(float specificGravity) {
    return 1.0e5f / (specificGravity * 1000.0f * kStdGravity);
}

// Learns the surface ambient pressure from barometer samples and locks it in when the running
// window is steady. N samples and a sigma gate, both explicit — the same shape as the air-cal
// sigma gate in AGENTS.md 6, sized for AGENTS.md 2's "+/-2 mbar ~ 2 cm" statement. A drifting
// or breathed-on module must NOT lock a garbage Psurf; it just stays unlocked and says so.
class SurfaceLearner {
  public:
    static constexpr int kWindow = 16;

    explicit SurfaceLearner(float sigmaMaxBar = 0.002f) : sigmaMaxBar_(sigmaMaxBar) {}

    void reset() {
        n_ = 0;
        locked_ = false;
        psurf_ = 0.0f;
        sigma_ = 0.0f;
    }

    // Feed one absolute-pressure sample (bar). Returns true once the lock criterion is met:
    // at least kWindow samples and population sigma over the window <= sigmaMaxBar.
    bool push(float ambBar) {
        if (locked_) return true;
        ring_[n_ % kWindow] = ambBar;
        ++n_;
        if (n_ >= kWindow) {
            float sum = 0.0f;
            for (int i = 0; i < kWindow; ++i) sum += ring_[i];
            const float mean = sum / static_cast<float>(kWindow);
            float var = 0.0f;
            for (int i = 0; i < kWindow; ++i) {
                const float d = ring_[i] - mean;
                var += d * d;
            }
            sigma_ = sqrtf(var / static_cast<float>(kWindow));
            if (sigma_ <= sigmaMaxBar_) {
                psurf_ = mean;
                locked_ = true;
            }
        }
        return locked_;
    }

    bool locked() const { return locked_; }
    float psurfBar() const { return psurf_; }   // meaningful only when locked()
    float sigmaBar() const { return sigma_; }   // over the last window, 0 before one is full
    uint32_t samples() const { return n_; }

  private:
    float ring_[kWindow];
    uint32_t n_ = 0;
    bool locked_ = false;
    float psurf_ = 0.0f;
    float sigma_ = 0.0f;
    float sigmaMaxBar_;
};

struct Result {
    float depthM;       // what the model and display use: raw, or the ceiling value when clamped
    float gaugeBar;     // Pamb - Psurf as the barometer actually reported it (never clamped)
    bool clamped;       // true: the barometer ran out of range; depthM is the ceiling, not a depth
    bool aboveSurface;  // negative gauge pressure (wave crest, or a Psurf learned too low)
};

// One depth answer. rangeCeilingBar is the fitted barometer's own absolute upper limit
// (Bmp085: 1.1 bar -> under 1 m of usable water column at sea level; Ms5837: 30 bar -> ~288 m).
// A ceiling of 0 means "unknown range" and disables the clamp; only bench code may pass that.
inline Result measure(float ambBar, float psurfBar, float specificGravity, float rangeCeilingBar) {
    Result r;
    r.gaugeBar = ambBar - psurfBar;
    r.aboveSurface = r.gaugeBar < 0.0f;
    const float k = metersPerBar(specificGravity);
    r.depthM = r.gaugeBar * k;
    r.clamped = false;
    if (rangeCeilingBar > 0.0f && ambBar > rangeCeilingBar) {
        r.depthM = (rangeCeilingBar - psurfBar) * k;
        r.clamped = true;
    }
    return r;
}

// The depth ceiling the banner must quote: the water-equivalent of the part's absolute upper
// limit against the *learned* surface pressure — computed, never recited. At a 977.8 mbar bench
// with the BMP180: (1.100 - 0.9778) bar x 9.948 m/bar = ~1.2 m, not the "9 m" the early design
// record asserted (decision 38).
inline float ceilingMeters(float psurfBar, float rangeCeilingBar, float specificGravity) {
    if (rangeCeilingBar <= 0.0f) return -1.0f;  // unknown range: no claim
    return (rangeCeilingBar - psurfBar) * metersPerBar(specificGravity);
}

}  // namespace depth
