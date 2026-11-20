// Dive-log data schema (M6, AGENTS.md 10). Pure C++ and host-testable (decision 28's
// pattern): the COLUMN ORDER and the CSV shape are what the web graphs and every exported
// file depend on, so they live where a test can pin them. The file-writing side lives in
// src/dive/DiveLog (it touches LittleFS).
//
// §10 fixes the 60 s sample row:
//   t, depth, amb_bar, ppO2_used, cell1_mV, cell2_mV, cell3_mV, ndl, ceiling, tts, cns,
//   otu, ppHe, fHe, controlling_compartment, END, mode, gas, pulses, V_BAT,
//   coil_shunt_peak, temp_C, source
// `source` is risk 11's stamp carried into every row, not decoration: a SIM run in the log
// ring must be identifiable from the CSV alone.
//
// coil_shunt_peak has no producer until M5 (the coil shunt walks a floating input today);
// the column exists now so a log recorded before M5 and one recorded after have the same
// shape — an absent column would silently shift every field in the older file.
#pragma once

#include "LiveState.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "GasMix.h"

namespace dlog {

constexpr int kGasSlots = 9;  // cfg::kGasCount; spelled here to keep lib/core independent

struct Sample {
    int32_t tS;                    // seconds since dive start
    float   depthM, ambBar, ppo2Used;
    int32_t cellMv[3];
    int32_t ndlMin, ttsMin;
    float   ceilingM;
    int32_t cnsPct;
    uint32_t otu;
    float   ppHe, fHe;
    char    controllingComp[8];    // "7 N2"
    int32_t endM;
    char    mode[12], gas[16];
    int32_t pulses;                // cumulative O2 pulses this dive
    float   vBat;
    float   coilShuntPeakA;        // M5; 0 until the shunt has a real producer
    float   tempC;
    uint8_t source;                // 0 REAL, 1 SIM, 2 MIXED (§10 'source', risk 11)
};

// Header per §10: start time, mode, all gas mixes, GFs, cal slopes per cell, max depth,
// min/max ppO2 per cell, deco time, CNS, OTU, battery curve, active boot faults.
struct Header {
    uint32_t startWallS = 0;       // 0 = wall clock never set (ESP32 has no RTC)
    int32_t  durationS = 0;
    char     mode[12] = "CCR";
    GasMix   gases[kGasSlots] = {};
    float    gfLow = 0.30f, gfHigh = 0.70f;
    float    calSlopeMvPerBar[3] = { 0.0f, 0.0f, 0.0f };
    float    maxDepthM = 0.0f;
    float    minPpO2[3] = { 0.0f, 0.0f, 0.0f };
    float    maxPpO2[3] = { 0.0f, 0.0f, 0.0f };
    int32_t  decoTimeS = 0;
    int32_t  cnsPct = 0;
    uint32_t otu = 0;
    float    vBatStart = 0.0f, vBatMin = 0.0f;   // battery curve endpoints (§10)
    char     faults[48] = "";                    // active boot-fault summary at start
    int      sampleCount = 0;
    uint8_t  source = 0;                         // SIM runs land in the same ring, tagged
};

// CSV header line, exactly the §10 order. Returned as a static string so the writer and
// the test cannot drift apart.
inline const char* csvHeader() {
    return "t,depth,amb_bar,ppo2_used,cell1_mv,cell2_mv,cell3_mv,ndl,ceiling,tts,cns,otu,"
           "pphe,fhe,controlling_compartment,end,mode,gas,pulses,v_bat,coil_shunt_peak,"
           "temp_c,source";
}

// CSV-safe: commas, quotes and newlines in a text field would shift every later column
// of the row — the same silent-corruption class as a clipped fault name (§4). Gas names
// are validated to exclude them (GasMix.h gasNameValid), so this is a guard for mode /
// compartment labels and any future field: substitute a space, keep the columns true.
inline void csvSafe(char* dst, size_t cap, const char* src) {
    size_t k = 0;
    for (const char* p = src; *p && k + 1 < cap; ++p) {
        char c = *p;
        if (c == ',' || c == '"' || c == '\n' || c == '\r') c = ' ';
        dst[k++] = c;
    }
    dst[k] = '\0';
}

// One CSV row without the trailing newline (the writer adds it). Returns length, or -1 on
// overflow — never a truncated row.
inline int csvRow(const Sample& s, char* buf, int cap) {
    if (!buf || cap < 32) return -1;
    char comp[10], mode[14], gas[18];
    csvSafe(comp, sizeof comp, s.controllingComp);
    csvSafe(mode, sizeof mode, s.mode);
    csvSafe(gas, sizeof gas, s.gas);
    int n = snprintf(buf, (size_t)cap,
                     "%d,%.1f,%.2f,%.2f,%d,%d,%d,%d,%.1f,%d,%d,%u,%.2f,%.2f,%s,%d,%s,%s,%d,"
                     "%.2f,%.2f,%.1f,%u",
                     (int)s.tS, (double)s.depthM, (double)s.ambBar, (double)s.ppo2Used,
                     (int)s.cellMv[0], (int)s.cellMv[1], (int)s.cellMv[2],
                     (int)s.ndlMin, (double)s.ceilingM, (int)s.ttsMin, (int)s.cnsPct,
                     (unsigned)s.otu, (double)s.ppHe, (double)s.fHe, comp, (int)s.endM,
                     mode, gas, (int)s.pulses, (double)s.vBat, (double)s.coilShuntPeakA,
                     (double)s.tempC, (unsigned)s.source);
    if (n < 0 || n >= cap) return -1;
    return n;
}

// Build a sample from the LiveState snapshot (the same immutable snapshot the web sees,
// so what is graphed on the page and what is logged cannot disagree).
inline Sample sampleFromLiveState(const disp::LiveState& st, int32_t tS) {
    Sample s{};
    s.tS = tS;
    s.depthM = st.depthM;
    s.ambBar = st.ambBar;
    s.ppo2Used = st.ppo2Used;
    for (int i = 0; i < 3; i++) s.cellMv[i] = st.cellMv[i];
    s.ndlMin = st.ndlMin;
    s.ttsMin = st.ttsMin;
    s.ceilingM = st.ceilingM;
    s.cnsPct = st.cnsPct;
    s.otu = st.otu;
    s.ppHe = st.ppHe;
    s.fHe = st.fHe;
    snprintf(s.controllingComp, sizeof s.controllingComp, "%s", st.controllingComp);
    s.endM = st.endM;
    snprintf(s.mode, sizeof s.mode, "%s", st.mode);
    snprintf(s.gas, sizeof s.gas, "%s", st.gas);
    s.pulses = 0;              // caller fills: LiveState carries rate, not the dive total
    s.vBat = st.vBat;
    s.coilShuntPeakA = 0.0f;   // M5
    s.tempC = st.tempC;
    s.source = (uint8_t)st.simFlag;
    return s;
}

// JSON writer for the header (dives/NNN.json's header object). Complete or -1.
inline int headerToJson(const Header& h, char* buf, int cap) {
    if (!buf || cap < 64) return -1;
    char faults[52];
    csvSafe(faults, sizeof faults, h.faults);   // reuse the substitution, then escape below
    int n = snprintf(buf, (size_t)cap,
                     "{\"startWallS\":%lu,\"durationS\":%d,\"mode\":\"%s\",\"gfLow\":%.4g,"
                     "\"gfHigh\":%.4g,\"maxDepthM\":%.2f,\"decoTimeS\":%d,\"cnsPct\":%d,"
                     "\"otu\":%u,\"vBatStart\":%.2f,\"vBatMin\":%.2f,\"sampleCount\":%d,"
                     "\"source\":%u,\"faults\":\"",
                     (unsigned long)h.startWallS, (int)h.durationS, h.mode,
                     (double)h.gfLow, (double)h.gfHigh, (double)h.maxDepthM,
                     (int)h.decoTimeS, (int)h.cnsPct, (unsigned)h.otu,
                     (double)h.vBatStart, (double)h.vBatMin, (int)h.sampleCount,
                     (unsigned)h.source);
    if (n < 0 || n >= cap) return -1;
    // faults is a BootReport summary line: firmware text, defensively escaped.
    int k = n;
    for (const char* p = faults; *p; ++p) {
        if (*p == '"' || *p == '\\') { if (k + 2 >= cap) return -1; buf[k++] = '\\'; }
        if (k + 1 >= cap) return -1;
        buf[k++] = *p;
    }
    k += snprintf(buf + k, (size_t)(cap - k), "\",\"calSlopes\":[");
    if (k >= cap) return -1;
    for (int i = 0; i < 3; i++) {
        int r = snprintf(buf + k, (size_t)(cap - k), "%s%.2f", i ? "," : "",
                         (double)h.calSlopeMvPerBar[i]);
        if (r < 0 || r >= cap - k) return -1;
        k += r;
    }
    k += snprintf(buf + k, (size_t)(cap - k), "],\"ppO2Range\":[");
    if (k >= cap) return -1;
    for (int i = 0; i < 3; i++) {
        int r = snprintf(buf + k, (size_t)(cap - k), "%s[%.2f,%.2f]", i ? "," : "",
                         (double)h.minPpO2[i], (double)h.maxPpO2[i]);
        if (r < 0 || r >= cap - k) return -1;
        k += r;
    }
    k += snprintf(buf + k, (size_t)(cap - k), "],\"gases\":[");
    if (k >= cap) return -1;
    for (int i = 0; i < kGasSlots; i++) {
        char nm[16];
        csvSafe(nm, sizeof nm, h.gases[i].name);
        int r = snprintf(buf + k, (size_t)(cap - k), "%s{\"name\":\"%s\",\"fo2\":%.3g,\"fhe\":%.3g}",
                         i ? "," : "", nm, (double)h.gases[i].fo2, (double)h.gases[i].fhe);
        if (r < 0 || r >= cap - k) return -1;
        k += r;
    }
    int r = snprintf(buf + k, (size_t)(cap - k), "]}");
    if (r < 0 || r >= cap - k) return -1;
    return k + r;
}

}  // namespace dlog
