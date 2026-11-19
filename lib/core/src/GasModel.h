// What the diver is actually breathing, expressed as inspired partial pressures.
// In a CCR the inspired gas is a *state*, not a mix (AGENTS.md 8), so this is the single place
// where CCR/OC switching, diluent flushes and bailout take effect on the decompression model.
#pragma once
#include "GasMix.h"

#include <stdint.h>

enum class BreathingState : uint8_t {
    CCR_CLOSED = 0,       // setpoint held by the loop, balance is diluent
    CCR_DILUENT_FLUSH,    // loop filled with diluent
    CCR_O2_FLUSH,         // loop filled with oxygen
    MANUAL_BAILOUT,       // breathing the bailout mix
    OC_OPEN,              // open circuit on the selected mix
};

struct GasModelConfig {
    float pCO2Bar = 0.053f;      // CCR only: alveolar CO2 allowance
    float pH2OBar = 0.0627f;     // saturated water vapour
    float spDeepBar = 0.70f;
    float spShallowBar = 1.30f;
    float spCrossoverDepthM = 21.0f;
};

struct Inspired {
    float po2 = 0.0f;
    float pn2 = 0.0f;
    float phe = 0.0f;
};

inline const char* breathingStateName(BreathingState s) {
    switch (s) {
        case BreathingState::CCR_CLOSED: return "CCR";
        case BreathingState::CCR_DILUENT_FLUSH: return "DILUENT_FLUSH";
        case BreathingState::CCR_O2_FLUSH: return "O2_FLUSH";
        case BreathingState::MANUAL_BAILOUT: return "BAILOUT";
        default: return "OC";
    }
}

inline bool isClosedCircuit(BreathingState s) {
    return s == BreathingState::CCR_CLOSED || s == BreathingState::CCR_DILUENT_FLUSH ||
           s == BreathingState::CCR_O2_FLUSH;
}

// Active setpoint for the current depth: shallow setpoint above the crossover, deep below.
float setpointAt(const GasModelConfig& cfg, float depthM);

// `diluent` is the CCR diluent, `oxygen` the setpoint/inject gas, `bailout` the bailout mix.
// `selected` is the mix chosen for open circuit (or the gas being breathed during a flush).
Inspired resolveInspired(BreathingState state, const GasMix& diluent, const GasMix& oxygen,
                         const GasMix& bailout, const GasMix& selected, float ambBar,
                         float depthM, const GasModelConfig& cfg);
