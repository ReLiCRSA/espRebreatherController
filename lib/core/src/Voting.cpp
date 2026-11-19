#include "Voting.h"

#include <math.h>

float ppO2FromMv(float mv, const CellCal& cal) {
    if (cal.slopeMvPerBar <= 0.0f) return 0.0f;
    const float v = (mv - cal.offsetMv) / cal.slopeMvPerBar;
    return v < 0.0f ? 0.0f : v;
}

VoteOutcome evaluateCells(const CellReading readings[CELL_COUNT], const CellCal cal[CELL_COUNT],
                          const VoteConfig& cfg, bool inDeco) {
    VoteOutcome out;
    float usable[CELL_COUNT];
    int nUsable = 0;

    for (int i = 0; i < CELL_COUNT; ++i) {
        CellEval& e = out.cell[i];
        e.mv = readings[i].mv;
        e.present = readings[i].present;

        if (!e.present) {
            e.fault = FaultCode::CELL_DEAD;
            continue;
        }
        if (e.mv < cfg.mvMin || e.mv > cfg.mvMax) {
            // Railed or disconnected input: garbage that can still look plausible.
            e.fault = FaultCode::CELL_BIAS;
            continue;
        }
        if (cal[i].slopeMvPerBar < cfg.minSlopeMvPerBar) e.aged = true;

        e.ppO2 = ppO2FromMv(e.mv, cal[i]);
        usable[nUsable++] = e.ppO2;
    }

    if (nUsable == 0) {
        out.status = VoteStatus::NO_DATA;
        out.medianPpO2 = NAN;
        for (int i = 0; i < CELL_COUNT; ++i)
            if (out.cell[i].fault == FaultCode::NONE) out.cell[i].fault = FaultCode::CELL_DEAD;
        return out;
    }

    // Reference: median of the usable readings.
    for (int a = 0; a < nUsable; ++a)
        for (int b = a + 1; b < nUsable; ++b)
            if (usable[b] < usable[a]) {
                const float t = usable[a];
                usable[a] = usable[b];
                usable[b] = t;
            }
    if (nUsable == 1) {
        out.medianPpO2 = usable[0];
    } else if (nUsable == 2) {
        out.medianPpO2 = 0.5f * (usable[0] + usable[1]);
    } else {
        out.medianPpO2 = usable[1];
    }

    const float tol = cfg.toleranceFloorBar > cfg.toleranceFrac * fabsf(out.medianPpO2)
                          ? cfg.toleranceFloorBar
                          : cfg.toleranceFrac * fabsf(out.medianPpO2);

    float sum = 0.0f;
    float lowest = 1.0e6f;
    int passing = 0;
    float maxDev = 0.0f;
    for (int i = 0; i < CELL_COUNT; ++i) {
        CellEval& e = out.cell[i];
        if (e.fault != FaultCode::NONE) continue;
        const float dev = fabsf(e.ppO2 - out.medianPpO2);
        if (dev > maxDev) maxDev = dev;
        e.passesVote = (dev <= tol);
        if (e.passesVote) {
            sum += e.ppO2;
            ++passing;
            if (e.ppO2 < lowest) lowest = e.ppO2;
        }
    }
    out.passingCount = passing;
    out.maxDeviationBar = maxDev;
    out.maxDeviationFrac =
        fabsf(out.medianPpO2) > 0.001f ? maxDev / fabsf(out.medianPpO2) : maxDev * 1000.0f;

    if (passing == 0) {
        // Nothing agrees: use the lowest reading at all, which is the conservative choice,
        // and flag it loudly. Injection is inhibited by the caller.
        float lowestAll = 1.0e6f;
        for (int i = 0; i < CELL_COUNT; ++i) {
            const CellEval& e = out.cell[i];
            if (e.fault != FaultCode::NONE) continue;
            if (e.ppO2 < lowestAll) lowestAll = e.ppO2;
        }
        out.status = VoteStatus::FAILED;
        out.ppO2Used = (lowestAll > 1.0e5f) ? 0.0f : lowestAll;
        return out;
    }

    if (passing == 1) {
        out.status = VoteStatus::DRIFT_ONE;
        out.ppO2Used = lowest; // the single passing cell
        return out;
    }

    out.status = VoteStatus::OK;
    // In deco the diver is dependent: take the lowest passing cell, not the mean.
    if (inDeco && cfg.conservativeInDeco) out.ppO2Used = lowest;
    else out.ppO2Used = sum / (float)passing;
    return out;
}

float expectedPpO2ForFlush(float fo2, float ambBar, float waterVapourBar) {
    const float dry = ambBar - waterVapourBar;
    if (dry <= 0.0f) return 0.0f;
    return fo2 * dry;
}

bool diluentFlushAgrees(const VoteOutcome& v, float expectedPpO2, float tolBar,
                        float* worstDeviationOut) {
    float worst = 0.0f;
    int usable = 0;
    for (int i = 0; i < CELL_COUNT; ++i) {
        const CellEval& e = v.cell[i];
        if (e.fault != FaultCode::NONE) continue;
        ++usable;
        const float dev = fabsf(e.ppO2 - expectedPpO2);
        if (dev > worst) worst = dev;
    }
    if (worstDeviationOut) *worstDeviationOut = worst;
    // Every cell must independently land on the diluent's expected ppO2. If they are all wrong
    // but agree with each other, voting would be happy and this check would not be.
    return usable == CELL_COUNT && worst <= tolBar;
}

float mvSigma(const float* samples, int n, float* meanOut) {
    if (!samples || n <= 0) {
        if (meanOut) *meanOut = 0.0f;
        return 0.0f;
    }
    float mean = 0.0f;
    for (int i = 0; i < n; ++i) mean += samples[i];
    mean /= (float)n;
    float var = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float d = samples[i] - mean;
        var += d * d;
    }
    var /= (float)n;
    if (meanOut) *meanOut = mean;
    return sqrtf(var);
}
