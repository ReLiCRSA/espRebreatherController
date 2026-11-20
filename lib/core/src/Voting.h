// Three-cell O2 voting and cell health (AGENTS.md 5). Median-of-three reference, 20% / 0.05 bar
// agreement band, mean of passing cells, lowest-of-passing when in deco, lowest of all when the
// vote fails.
#pragma once
#include "FaultCode.h"

#include <stdint.h>

static const int CELL_COUNT = 3;

struct CellCal {
    float slopeMvPerBar = 47.6f; // 0.21 bar -> ~10 mV implies ~47.6 mV/bar
    float offsetMv = 0.0f;       // mV at ppO2 = 0
};

struct CellReading {
    float mv = 0.0f;
    bool  present = false; // converter channel responded
};

struct VoteConfig {
    float toleranceFrac = 0.20f;
    float toleranceFloorBar = 0.05f;
    float mvMin = -50.0f;
    float mvMax = 1000.0f;
    float minSlopeMvPerBar = 30.0f; // below this the cell is aged / current limited
    bool  conservativeInDeco = true;
};

struct CellEval {
    float mv = 0.0f;
    float ppO2 = 0.0f;
    bool  present = false;
    bool  passesVote = false;
    bool  aged = false;     // slope below minSlopeMvPerBar
    FaultCode fault = FaultCode::NONE;
};

enum class VoteStatus : uint8_t { NO_DATA, OK, DRIFT_ONE, FAILED };

struct VoteOutcome {
    VoteStatus status = VoteStatus::NO_DATA;
    float  ppO2Used = 0.0f;   // what the control loop and deco engine must use
    float  medianPpO2 = 0.0f; // reference (NaN when no usable cell)
    CellEval cell[CELL_COUNT];
    float  maxDeviationBar = 0.0f;
    float  maxDeviationFrac = 0.0f;
    int    passingCount = 0;
};

float ppO2FromMv(float mv, const CellCal& cal);

VoteOutcome evaluateCells(const CellReading readings[CELL_COUNT], const CellCal cal[CELL_COUNT],
                          const VoteConfig& cfg, bool inDeco);

// Diluent flush verification (AGENTS.md 5): an independent check that voting cannot fake.
float expectedPpO2ForFlush(float fo2, float ambBar, float waterVapourBar);
bool diluentFlushAgrees(const VoteOutcome& v, float expectedPpO2, float tolBar,
                        float* worstDeviationOut);

// Stability gate for calibration: sigma of the mV samples over the sampling window.
float mvSigma(const float* samples, int n, float* meanOut);

// Median of the same window (§6: "60 s countdown sampling median mV per cell"). Sorts the
// CALLER's array in place — the sampling window is scratch by contract, and a local copy
// would be a KB of stack in a path that also owns the display scratch (decision 40 rule 1).
// n <= 0 returns 0, which is a legal cell reading: callers gate on n themselves.
float mvMedian(float* samples, int n);
