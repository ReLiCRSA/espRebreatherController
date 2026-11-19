#include "SimCells.h"

#include <math.h>

namespace sim {

void SimCells::setStuckMv(int cell, float mv) {
    if (cell < 0 || cell > 2) return;
    fault_[cell].stuck = true;
    fault_[cell].stuckMv = mv;
}

void SimCells::setDead(int cell, bool dead) {
    if (cell < 0 || cell > 2) return;
    fault_[cell].dead = dead;
}

void SimCells::setChannelPresent(int cell, bool present) {
    if (cell < 0 || cell > 2) return;
    fault_[cell].present = present;
}

void SimCells::setDriftMvPerSec(int cell, float mvPerSec) {
    if (cell < 0 || cell > 2) return;
    fault_[cell].driftMvPerSec = mvPerSec;
}

void SimCells::addStepMv(int cell, float mv) {
    if (cell < 0 || cell > 2) return;
    fault_[cell].stepMv += mv;
}

void SimCells::setNoiseMv(int cell, float peakMv) {
    if (cell < 0 || cell > 2) return;
    fault_[cell].noisePeak = peakMv;
}

void SimCells::setT90Sec(int cell, float t90) {
    if (cell < 0 || cell > 2) return;
    fault_[cell].t90Sec = t90 > 0.0f ? t90 : 0.0f;
}

void SimCells::setCurrentLimited(int cell, bool on, float startBar, float fullBar) {
    if (cell < 0 || cell > 2) return;
    fault_[cell].currentLimited = on;
    fault_[cell].clStartBar = startBar;
    fault_[cell].clFullBar = fullBar > startBar ? fullBar : startBar + 0.01f;
}

void SimCells::setInvert(int cell, bool invert) {
    if (cell < 0 || cell > 2) return;
    fault_[cell].invert = invert;
}

void SimCells::resetFaults() {
    for (int i = 0; i < 3; ++i) {
        fault_[i] = Fault{};
        statePpO2_[i] = 0.21f;
        seed_[i] = 0x12345678u + static_cast<uint32_t>(i) * 0x1111u;
    }
}

float SimCells::noiseSample(uint32_t& state, float peak) {
    // xorshift32. Deterministic from the seed; peak-scaled, zero-mean.
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    if (state == 0u) state = 0x9e3779b9u;  // xorshift's one fixed point
    const float unit = static_cast<float>(state & 0xFFFFFFu) / 8388607.5f;  // 0 .. 2
    return (unit - 1.0f) * peak;
}

// Current-limited cell: linear up to clStartBar, then compressing to a fixed ceiling at clFullBar.
// Below the start it is byte-identical to a healthy cell, which is exactly the property that makes
// this failure invisible to an air calibration.
float SimCells::applyCurrentLimit(float ppO2, const Fault& f) const {
    if (!f.currentLimited || ppO2 <= f.clStartBar) return ppO2;
    const float span = f.clFullBar - f.clStartBar;
    const float over = ppO2 - f.clStartBar;
    if (over >= span) return f.clStartBar + span * 0.5f;  // saturated: flat, no more output
    // Saturating approach: steepest just past the knee, flattening towards full bar.
    const float frac = over / span;
    return f.clStartBar + span * 0.5f * (1.0f - (1.0f - frac) * (1.0f - frac));
}

void SimCells::advance(float dtSec, float loopPpO2) {
    if (dtSec <= 0.0f) return;
    for (int i = 0; i < 3; ++i) {
        Fault& f = fault_[i];

        // Cell response lag, in the ppO2 domain: the electrode cannot follow the loop instantly.
        if (f.t90Sec > 0.0f) {
            const float tau = f.t90Sec * kT90OverLn10;
            const float alpha = 1.0f - expf(-dtSec / tau);
            statePpO2_[i] += (loopPpO2 - statePpO2_[i]) * alpha;
        } else {
            statePpO2_[i] = loopPpO2;
        }

        float ppSeen = applyCurrentLimit(statePpO2_[i], f);

        float mv = cfg_[i].gain * ppSeen + cfg_[i].offsetMv;
        if (f.invert) mv = -mv;
        mv += f.stepMv + f.driftMv;
        if (f.noisePeak > 0.0f) mv += noiseSample(seed_[i], f.noisePeak);

        // Overrides last: a stuck or dead cell is not a perturbation of the model, it is the cell
        // doing something else entirely.
        if (f.dead) mv = 0.0f;
        if (f.stuck) mv = f.stuckMv;

        f.driftMv += f.driftMvPerSec * dtSec;

        lastMv_[i] = mv;
        lastPresent_[i] = f.present;
    }
}

void SimCells::readMv(float out[3], bool present[3]) {
    for (int i = 0; i < 3; ++i) {
        if (out) out[i] = lastMv_[i];
        if (present) present[i] = lastPresent_[i];
    }
}

bool SimCells::rawWithinFaultBand(int cell, float mvMin, float mvMax) const {
    if (cell < 0 || cell > 2) return false;
    return lastMv_[cell] >= mvMin && lastMv_[cell] <= mvMax;
}

}  // namespace sim
