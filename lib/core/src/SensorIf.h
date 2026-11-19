// Sensor source interfaces and source stamping (AGENTS.md 4, 11, 17).
//
// These live in lib/core, not lib/sim and not src/sensors, for one structural reason: the real
// drivers (src/sensors) and the simulators (lib/sim) must implement the *same* interface, and
// `src/` can never be compiled natively. If the interface lived in either one, the other would have
// to depend on it in a direction the build forbids. Putting it in core is what lets the dive loop be
// written against the interface and tested against the simulator without either knowing about the
// other.
//
// This is also where the `source` stamp lives. AGENTS.md 19 risk 11 treats simulator data reaching
// dive-state output as a fatal design bug, so the stamp is part of the sensor contract rather than a
// field someone remembers to set.
#pragma once

#include <stdint.h>

enum class SourceKind : uint8_t { REAL = 0, SIM = 1 };

// The whole-system stamp for one LiveState snapshot. SIM wins over REAL: a dive computer that
// reports "REAL" while its depth is synthetic is lying about the one input every deco number is
// derived from. MIXED is the honest label for the common bench case of real cells and simulated
// pressure (AGENTS.md 11).
enum class SourceMix : uint8_t { REAL = 0, MIXED = 1, SIM = 2 };

inline SourceMix combineSources(SourceKind a, SourceKind b) {
    if (a == SourceKind::SIM && b == SourceKind::SIM) return SourceMix::SIM;
    if (a == SourceKind::SIM || b == SourceKind::SIM) return SourceMix::MIXED;
    return SourceMix::REAL;
}
inline SourceMix combineSources(SourceMix cur, SourceKind b) {
    if (b == SourceKind::SIM) return cur == SourceMix::REAL ? SourceMix::MIXED : SourceMix::SIM;
    return cur == SourceMix::SIM ? SourceMix::SIM : (cur == SourceMix::MIXED ? SourceMix::MIXED
                                                                            : SourceMix::REAL);
}
inline bool isSimulated(SourceMix s) { return s != SourceMix::REAL; }
inline const char* sourceName(SourceKind s) { return s == SourceKind::SIM ? "SIM" : "REAL"; }
inline const char* sourceName(SourceMix s) {
    return s == SourceMix::SIM ? "SIM" : (s == SourceMix::MIXED ? "MIXED" : "REAL");
}

// Ambient pressure source (MS5837, BMP085, or SimBaro).
//
// Units are bar **absolute**, always: the depth conversion is done by Depth on top of a learned
// surface pressure, so a driver that quietly returns gauge pressure would offset every dive by one
// atmosphere and still look plausible.
class Barometer {
  public:
    virtual ~Barometer() = default;

    // Largest absolute pressure this sensor can actually measure, bar. The firmware clamps
    // depth-dependent logic at this value and never extrapolates past it — AGENTS.md 19 risk 1,
    // because the BMP085 fallback saturates at ~1.1 bar (~9 m) and a computer that extrapolates
    // through that wall reports a depth that does not exist. 0 means "no ceiling to report".
    virtual float rangeCeilingBar() const { return 0.0f; }

    // One-shot initialisation; false with the reason left to the caller's fault report.
    virtual bool init() = 0;

    // Blocking read. Returns false on a failed transaction so the dive loop raises a fault instead
    // of integrating a stale pressure for another minute.
    virtual bool read(float* ambBar, float* tempC) = 0;

    virtual SourceKind source() const { return SourceKind::REAL; }
};

// Raw galvanic cell voltages (Ads1115/CellArray, or SimCells).
//
// Deliberately millivolts and not ppO2: calibration and voting are firmware behaviour, and a
// simulator that injected ppO2 directly would skip the calibration arithmetic, the bias-sanity
// check and the voter — i.e. it would test the deco maths and nothing else (AGENTS.md 11).
class CellSource {
  public:
    virtual ~CellSource() = default;

    // out[i] = millivolts for cell i, present[i] = its converter channel answered.
    virtual void readMv(float out[3], bool present[3]) = 0;

    virtual SourceKind source() const { return SourceKind::REAL; }
};

// Fixed time base (AGENTS.md 4). The engine integrates at a fixed 1 s step, so a simulated clock
// that advances in the same 1 s quanta produces numerically identical tissue state at 1x and 60x —
// time compression changes wall-clock cost, never the arithmetic. AGENTS.md 19 risk 9: anything that
// depends on *real* elapsed time (ascent-rate limiting, warm-up gates, watchdog) must stay on the
// 1x path and must not be certified from a compressed run.
class ClockSource {
  public:
    virtual ~ClockSource() = default;
    virtual uint64_t nowUs() const = 0;
    virtual SourceKind source() const { return SourceKind::REAL; }
};
