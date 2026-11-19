#include "O2Toxicity.h"

#include <math.h>

// {ppO2, allowed minutes}, ascending in ppO2.
static const float kTable[][2] = {
    {0.60f, 1800.0f}, {0.70f, 1500.0f}, {0.80f, 1200.0f}, {0.90f, 900.0f},
    {1.00f, 720.0f},  {1.10f, 540.0f},  {1.20f, 450.0f},  {1.30f, 360.0f},
    {1.40f, 300.0f},  {1.50f, 270.0f},  {1.60f, 240.0f},
};
static const int kTableN = (int)(sizeof(kTable) / sizeof(kTable[0]));

float O2Toxicity::singleExposureLimitMin(float ppO2) {
    if (ppO2 < kTable[0][0]) return 1.0e9f;      // below the table: negligible exposure
    if (ppO2 > kTable[kTableN - 1][0]) return 0.0f;
    for (int i = 0; i < kTableN - 1; ++i) {
        const float lo = kTable[i][0], hi = kTable[i + 1][0];
        if (ppO2 >= lo && ppO2 <= hi) {
            const float t = (ppO2 - lo) / (hi - lo);
            return kTable[i][1] + t * (kTable[i + 1][1] - kTable[i][1]);
        }
    }
    return 0.0f;
}

void O2Toxicity::step(float ppO2, float dtSec, float washoutHalfTimeMin, float cnsThresholdBar,
                      float otuThresholdBar) {
    if (dtSec <= 0.0f) return;
    const float dtMin = dtSec / 60.0f;

    if (ppO2 >= cnsThresholdBar) {
        const float limit = singleExposureLimitMin(ppO2);
        if (limit > 0.0f) state_.cnsPercent += 100.0f * dtMin / limit;
    }

    if (washoutHalfTimeMin > 0.0f) {
        const float decay = expf(-0.69314718f * dtMin / washoutHalfTimeMin);
        state_.cnsPercent *= decay;
    }

    if (ppO2 >= otuThresholdBar)
        state_.otu += dtMin * powf(ppO2 / 0.5f, 1.318f);

    if (state_.cnsPercent < 0.0f) state_.cnsPercent = 0.0f;
    if (state_.otu < 0.0f) state_.otu = 0.0f;
}
