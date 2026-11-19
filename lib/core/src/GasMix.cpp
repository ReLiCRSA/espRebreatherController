#include "GasMix.h"

#include <math.h>
#include <string.h>

// Molar masses in g/mol.
static const float M_O2 = 31.998f;
static const float M_N2 = 28.014f;
static const float M_HE = 4.0026f;
static const float R_BAR_L = 0.0831446f; // L*bar/(mol*K)
static const float G_STANDARD = 9.80665f;

float metresPerBar(float waterDensityKgM3) {
    if (waterDensityKgM3 < 900.0f) waterDensityKgM3 = 997.0f; // fresh default guard
    return 100000.0f / (waterDensityKgM3 * G_STANDARD);
}

float ambientAtDepth(float depthM, float surfaceBar, float mPerBar) {
    if (mPerBar <= 0.0f) return surfaceBar;
    return surfaceBar + depthM / mPerBar;
}

float depthForAmbient(float ambBar, float surfaceBar, float mPerBar) {
    if (mPerBar <= 0.0f) return 0.0f;
    return (ambBar - surfaceBar) * mPerBar;
}

float modPressure(const GasMix& g, const GasLimits& l) {
    if (g.fo2 <= 0.0f) return 0.0f;
    return l.ppO2MaxBar / g.fo2;
}

float hypoxicFloorPressure(const GasMix& g, const GasLimits& l) {
    if (g.fo2 <= 0.0f) return 1.0e6f;
    // ppO2 < floor is hypoxic, so the mix may not be breathed shallower than this.
    return l.ppO2MinBar / g.fo2;
}

float endPressure(const GasMix& g, float ambBar, const GasLimits& l) {
    // Narcotic load of the mix at ambBar, expressed as the ambient pressure of air
    // (FN2 = 0.79) carrying the same load. Helium enters with heNarcFactor.
    const float narcotic = fn2(g) + l.heNarcFactor * g.fhe;
    return ambBar * narcotic / 0.79f;
}

float gasDensityGL(const GasMix& g, float ambBar, float tempC) {
    const float mAvg = g.fo2 * M_O2 + fn2(g) * M_N2 + g.fhe * M_HE;
    const float tK = tempC + 273.15f;
    if (tK <= 0.0f) return 0.0f;
    return ambBar * mAvg / (R_BAR_L * tK);
}

GasAssessment assessGas(const GasMix& g, float depthM, float ambBar, float tempC,
                        float surfaceBar, float mPerBar, const GasLimits& l) {
    GasAssessment a{};
    a.modM = depthForAmbient(modPressure(g, l), surfaceBar, mPerBar);
    a.hypoxicMinM = depthForAmbient(hypoxicFloorPressure(g, l), surfaceBar, mPerBar);
    a.endM = depthForAmbient(endPressure(g, ambBar, l), surfaceBar, mPerBar);
    a.densityGL = gasDensityGL(g, ambBar, tempC);

    a.modOk = depthM <= a.modM + 0.05f;
    // A 0.21 mix at the surface has hypoxicMinM < 0, which is correct: it is breathable there.
    a.hypoxicOk = depthM >= a.hypoxicMinM - 0.05f || a.hypoxicMinM < 0.0f;
    a.densityOk = a.densityGL <= l.densityMaxGL;
    a.selectable = a.modOk && a.hypoxicOk && a.densityOk;
    return a;
}

int bestMixIndex(const GasMix* gases, int count, float depthM, float ambBar, float tempC,
                 float surfaceBar, float mPerBar, const GasLimits& l) {
    int best = -1;
    float bestFo2 = -1.0f;
    for (int i = 0; i < count; ++i) {
        GasAssessment a = assessGas(gases[i], depthM, ambBar, tempC, surfaceBar, mPerBar, l);
        if (!a.selectable) continue;
        // Richest oxygen that is still legal: fastest offgas, longest MOD margin is *not* the
        // goal here, decompression speed is.
        if (gases[i].fo2 > bestFo2) {
            bestFo2 = gases[i].fo2;
            best = i;
        }
    }
    return best;
}

bool gasNameValid(const char* name) {
    if (!name || name[0] == '\0') return false;
    size_t n = strlen(name);
    if (n > 12) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        // Alphanumerics plus the punctuation gas names actually use ("TX 21/35", "O2 10/80").
        // Deliberately excludes < > & " ' so a name can never carry markup into the web UI.
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == ' ' || c == '.' ||
                        c == '/' || c == '%';
        if (!ok) return false;
    }
    return true;
}

bool gasMixValid(const GasMix& g, char* reason, int reasonLen) {
    auto fail = [&](const char* m) {
        if (reason && reasonLen > 0) {
            strncpy(reason, m, (size_t)reasonLen - 1);
            reason[reasonLen - 1] = '\0';
        }
        return false;
    };
    if (!(g.fo2 >= 0.10f && g.fo2 <= 1.00f)) return fail("fo2 out of range 0.10..1.00");
    if (!(g.fhe >= 0.0f && g.fhe <= 0.90f)) return fail("fhe out of range 0.00..0.90");
    if (g.fo2 + g.fhe > 1.0001f) return fail("fo2+fhe exceeds 1.0");
    if (!gasNameValid(g.name)) return fail("bad gas name");
    return true;
}
