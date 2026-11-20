#ifndef LIVESTATE_H
#define LIVESTATE_H

/*
 * LiveState.h — the one immutable snapshot the display, the web socket and the dive log
 * all read (§4: "Web sees only an immutable LiveState snapshot"). POD, fixed-size,
 * allocation-free; M6 wraps it in the xQueueOverwrite transport.
 *
 * Field set follows §12's field-class list. Names here are the struct members; the single
 * source of truth for template/web/log NAMES, units and formats is Fields.h, which maps a
 * FieldId onto exactly one member of this struct. Adding a field means: member here + row
 * in Fields.h — it then appears on the web Live tab, in the log columns and in templates.
 *
 * Every snapshot carries a source stamp (§11/risk 11): SIM data must never masquerade as
 * real dive data on any output.
 */

#include <cstdint>

namespace disp {

struct LiveState {
    // --- ppO2 / cells (SAFETY + calibration detail)
    float   ppo2Used;          // bar, voted
    float   ppo2Cell[3];       // bar per cell
    int32_t cellMv[3];         // raw mV per cell
    bool    votingPass;

    // --- depth / time (SAFETY)
    float   depthM;
    float   ambBar;
    int32_t ndlMin;            // 0..; INT32_MAX-style "999" convention stays upstream
    int32_t ttsMin;
    float   ceilingM;
    int32_t lastStopM;
    float   gfNow;             // %
    char    controllingComp[8];  // e.g. "7 N2"
    int32_t cnsPct;
    uint32_t otu;
    int32_t modM;
    int32_t hypoxicModM;
    int32_t endM;
    float   gasDensityGL;

    // --- breathing state
    char    mode[12];          // "CCR" / "OC" / "FLUSH" / "BAILOUT"
    char    gas[16];           // active gas name
    float   fo2, fHe;
    float   ppN2, ppHe;
    float   setpoint;

    // --- injection
    int32_t pulsesPerMin;
    float   injectedL;         // ambient L
    char    solenoidState[12]; // "off" "idle" "inject" "FAULT"

    // --- power / environment
    float   vBat, v3v3;
    int32_t batteryPct;
    float   tempC;             // sensor temp (MS5837/BMP logging, §2)
    float   calAgeH;

    // --- dive bookkeeping
    int32_t diveTimeSec;
    int32_t surfaceIntervalMin;
    float   ascRateMMin;
    char    bestMix[16];
    int32_t dguMin;

    // --- banners
    char    faultBanner[48];   // BootReport-style clipped summary

    // --- provenance (risk 11: never decoration)
    int32_t simFlag;           // 0 REAL, 1 SIM, 2 MIXED (SourceKind-ish; int for %d)

    void clear() { *this = LiveState{}; }
};

}  // namespace disp

#endif
