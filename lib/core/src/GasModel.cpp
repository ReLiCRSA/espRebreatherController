#include "GasModel.h"

float setpointAt(const GasModelConfig& cfg, float depthM) {
    return depthM > cfg.spCrossoverDepthM ? cfg.spDeepBar : cfg.spShallowBar;
}

Inspired resolveInspired(BreathingState state, const GasMix& diluent, const GasMix& oxygen,
                         const GasMix& bailout, const GasMix& selected, float ambBar,
                         float depthM, const GasModelConfig& cfg) {
    Inspired i{};
    if (ambBar <= 0.0f) return i;

    switch (state) {
        case BreathingState::OC_OPEN: {
            // Open circuit: inspired partial pressures follow the mix and ambient pressure.
            const float dry = ambBar - cfg.pH2OBar;
            if (dry <= 0.0f) return i;
            i.po2 = selected.fo2 * dry;
            i.pn2 = fn2(selected) * dry;
            i.phe = selected.fhe * dry;
            return i;
        }
        case BreathingState::MANUAL_BAILOUT: {
            const float dry = ambBar - cfg.pH2OBar;
            if (dry <= 0.0f) return i;
            i.po2 = bailout.fo2 * dry;
            i.pn2 = fn2(bailout) * dry;
            i.phe = bailout.fhe * dry;
            return i;
        }
        case BreathingState::CCR_O2_FLUSH: {
            const float dry = ambBar - cfg.pH2OBar;
            if (dry <= 0.0f) return i;
            i.po2 = oxygen.fo2 * dry;
            i.pn2 = fn2(oxygen) * dry;
            i.phe = oxygen.fhe * dry;
            return i;
        }
        case BreathingState::CCR_DILUENT_FLUSH: {
            const float dry = ambBar - cfg.pH2OBar;
            if (dry <= 0.0f) return i;
            i.po2 = diluent.fo2 * dry;
            i.pn2 = fn2(diluent) * dry;
            i.phe = diluent.fhe * dry;
            return i;
        }
        case BreathingState::CCR_CLOSED:
        default: {
            // Closed circuit at setpoint: O2 is held, the diluent supplies the rest.
            // pN2 = Pamb - pO2 - pCO2 - pH2O, with the diluent's fractions splitting
            // what is left between N2 and He.
            const float sp = setpointAt(cfg, depthM);
            i.po2 = sp;
            float rest = ambBar - sp - cfg.pCO2Bar - cfg.pH2OBar;
            if (rest < 0.0f) rest = 0.0f;
            const float fInert = diluent.fo2 >= 1.0f ? 0.0f : (fn2(diluent) + diluent.fhe);
            if (fInert <= 0.0f) return i; // pure-oxygen diluent: nothing else to load
            i.pn2 = rest * (fn2(diluent) / fInert);
            i.phe = rest * (diluent.fhe / fInert);
            return i;
        }
    }
}
