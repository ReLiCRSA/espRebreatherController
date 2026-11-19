// Gas mixes and the four independent limits a mix must satisfy at depth (AGENTS.md 7).
// Pressure in bar, depth in metres, temperature in Celsius, density in g/L.
#pragma once
#include <stdint.h>

struct GasMix {
    char  name[13];
    float fo2;   // 0.10 .. 1.00
    float fhe;   // 0.00 .. 0.90, fo2 + fhe <= 1.0
};

inline float fn2(const GasMix& g) {
    float f = 1.0f - g.fo2 - g.fhe;
    return f < 0.0f ? 0.0f : f;
}

struct GasLimits {
    float ppO2MaxBar     = 1.60f; // hyperoxic MOD basis
    float ppO2MinBar     = 0.18f; // hypoxic floor
    float heNarcFactor   = 0.60f; // END basis; 0 = helium treated as non-narcotic
    float densityMaxGL   = 6.0f;  // HPNS / work of breathing limit
};

// Metres per bar of hydrostatic head for a given water density (kg/m^3).
float metresPerBar(float waterDensityKgM3);

// Ambient pressure (bar absolute) at a depth, given learned surface pressure.
float ambientAtDepth(float depthM, float surfaceBar, float metresPerBarValue);
// Depth (m) for an ambient pressure, negative allowed (above the learned surface).
float depthForAmbient(float ambBar, float surfaceBar, float metresPerBarValue);

// --- limits, as ambient pressure (bar absolute); convert with depthForAmbient() ---
float modPressure(const GasMix& g, const GasLimits& l);
float hypoxicFloorPressure(const GasMix& g, const GasLimits& l); // deeper than this = hypoxic
float endPressure(const GasMix& g, float ambBar, const GasLimits& l);
float gasDensityGL(const GasMix& g, float ambBar, float tempC);

struct GasAssessment {
    float modM;         // hyperoxic max operating depth
    float hypoxicMinM;  // shallowest safe depth (below this the mix is hypoxic)
    float endM;         // equivalent narcotic depth
    float densityGL;
    bool  modOk;        // current depth <= modM
    bool  hypoxicOk;    // current depth >= hypoxicMinM
    bool  densityOk;    // densityGL <= limit
    bool  selectable;   // all three ok
};

// Assessment of `g` at the given depth. `ambBar` is the ambient at that depth.
GasAssessment assessGas(const GasMix& g, float depthM, float ambBar, float tempC,
                        float surfaceBar, float metresPerBarValue, const GasLimits& l);

// The richest-oxygen mix that is legal at depth: the standard "best mix" / deco-gas choice,
// because a higher ppO2 speeds offgas while MOD, the hypoxic floor, END and density still hold.
// Returns index or -1 when nothing on the list is safe at this depth.
int bestMixIndex(const GasMix* gases, int count, float depthM, float ambBar, float tempC,
                 float surfaceBar, float metresPerBarValue, const GasLimits& l);

bool gasNameValid(const char* name);
bool gasMixValid(const GasMix& g, char* reason, int reasonLen);
