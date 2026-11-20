// RuntimeConfig schema — AGENTS.md §9/§17: every threshold and configuration value lives here,
// the schema is VERSIONED, writes are range-validated SERVER-SIDE, and a rejected write must
// return per-field errors. This header is pure C++ (decision 28's pattern): the struct, its
// JSON text encoding, the range table and the migration defaults are all reachable by
// `pio test -e native`, because a validation gate nobody can test is a gate that rots.
// Filesystem/NVS persistence lives in src/config/RuntimeConfig (it touches hardware).
//
// Why the JSON here is hand-rolled instead of ArduinoJson: the config text is the input to a
// gate that decides what the dive model runs on, so parse + defaults + validate form ONE chain
// that must be pinned by ONE suite, including malformed-input behaviour. A small nested-JSON
// parser with fixed capacity, no allocation and abort-on-any-syntax-error is fully under test;
// the same chain through a third-party parser would only test our configuration of it.
// (ArduinoJson remains available for other web payloads; nothing on the config path uses it.)
//
// Migration model (§17): stored JSON carries `schema`. Fields absent from an older document
// take their defaults (every field has a default); `schema` NEWER than this build is refused
// outright — firmware never silently reinterprets a future format. kSchemaVersion starts at 1;
// adding a field does NOT bump it (absent-field defaults handle it, and the migration tests
// fake older documents by omitting fields); bump only on a semantic change or a rename.
//
// Memory rule (decision 40 rule 1): JsonDoc is KB-scale. It is NEVER a task-stack local —
// callers pass storage (one static scratch in main.cpp, like g_tmpl). sizeof is pinned by
// test/test_config/ so a capacity change re-faces the budget deliberately.
#pragma once
#include "GasMix.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace cfg {

constexpr int kSchemaVersion = 1;
constexpr int kGasCount = 9;   // 0 diluent, 1 oxygen, 2 escape/bailout, 3..8 staged

// Mode of the loop as configured (§4's state machine has the full set; this is the user-
// selectable pair from the web Gases tab).
enum Mode : uint8_t { MODE_CCR = 0, MODE_OC = 1 };

struct RuntimeConfig {
    // --- deco (§7): maps onto zhl::Config at apply time; mPerBar/surfaceBar are DERIVED
    //     (salinity + learned Psurf), never stored here (decision 38: derived, not recited) ---
    float gfLow = 0.30f;              // fraction, NOT percent: §9's "GF_low >= 20" is >= 0.20
    float gfHigh = 0.70f;
    bool  icdGuard = true;            // ours, not Subsurface's (§7): default on, configurable off
    float icdRatio = 0.80f;

    float rateAscentMPerMin = 8.0f;   // §7 optimal-ascent limiter
    float rateDecoMPerMin = 8.0f;     // ascent while in deco (must be <= rateAscent)
    float rateDescentMPerMin = 20.0f;
    float lastStopM = 6.0f;           // §7/decision 22: the diver's choice of unpaid tolerance

    // --- CCR setpoint law (§8) ---
    float spDeepBar = 0.70f;          // setpoint below crossover depth
    float spShallowBar = 1.30f;       // setpoint above crossover depth
    float spCrossoverM = 21.0f;

    // --- gas limits (§7's four limits; feeds GasLimits) ---
    float ppO2MaxBar = 1.60f;         // hyperoxic MOD basis; §9: ppO2_hi <= 1.7
    float ppO2MinBar = 0.18f;         // hypoxic floor
    float heNarcFactor = 0.60f;       // END basis
    float densityMaxGL = 6.0f;        // HPNS / work-of-breathing

    // --- alarms (§9 Deco tab) ---
    float ppO2AlarmHighBar = 1.60f;   // display/alarm bound (distinct from MOD basis)
    float ppO2AlarmLowBar = 0.40f;    // CCR low-ppO2 alarm bound
    float cnsCeilingPct = 100.0f;
    float cnsHalfTimeMin = 125.0f;    // §7 washout, configurable
    float pco2Bar = 0.053f;           // §7 CCR inspired pCO2 term

    // --- depth model inputs (§2) ---
    float sgWater = 1.025f;           // salinity: fresh 1.000, salt 1.025
    float altitudeHintM = 0.0f;       // display/record only: learned Psurf is authoritative

    // --- gases (§9 Gases tab). Slots 0-2 required; 3-8 optional (name empty + fo2 0 = empty) ---
    GasMix gases[kGasCount] = {
        { "DIL AIR",    0.21f, 0.0f },
        { "O2",         1.00f, 0.0f },
        { "ESC AIR",    0.21f, 0.0f },
        { "",           0.0f,  0.0f },
        { "",           0.0f,  0.0f },
        { "",           0.0f,  0.0f },
        { "",           0.0f,  0.0f },
        { "",           0.0f,  0.0f },
        { "",           0.0f,  0.0f },
    };
    int   selectedGas = 0;            // which slot is actually breathed (§9 radio-select)
    float heSwitchDepthM = 0.0f;      // 0 = disabled; He-enabled above this depth (open item 7)

    // --- loop control / solenoid (§8). Values are provisional M5 placeholders until open
    //     items 1-4 (valve choice, IP, pulse volume) are measured; ranges keep them honest. ---
    bool  solenoidEnable = false;     // §8: defaults false; nothing drives the coil until armed+fit
    float valvePulseMsMin = 60.0f;
    float valvePulseMsMax = 120.0f;
    float valvePulsesPerMinMax = 40.0f;
    float valvePulseVolAmbL = 0.15f;  // ambient L per pulse at IP (open item 4 placeholder)
    float vbatInjectMinV = 3.40f;     // §8 low-voltage lockout

    // --- simulator (§11): risk 12 says these are guesses until measured ---
    float metabolicSurfLpm = 1.2f;    // STPD surface litres/min, depth-independent (risk 20)
    float loopVolumeL = 8.0f;

    // --- mode + system ---
    uint8_t mode = MODE_CCR;
    bool wifiDuringDive = false;      // §9/§19: default off

    // --- calibration (§6). slope 0 = uncalibrated; arming requires >= 30 mV/bar (M2 gate) ---
    float calSlopeMvPerBar[3] = { 0.0f, 0.0f, 0.0f };
    float calHighMv[3] = { 0.0f, 0.0f, 0.0f };   // optional 0.98-bar check (§6)
    uint32_t calWallS = 0;            // wall clock at last air cal (cal-age gate needs wall time)

    // --- wall clock: ESP32 classic has no RTC. Phone sets (unixS, uptimeS) via POST /api/time;
    //     wall now = lastWallS + (uptimeS - lastWallUptimeS). Persisted so cal age survives reboot. ---
    uint32_t lastWallS = 0;
    uint32_t lastWallUptimeS = 0;
};

// --- validation (§9): server-side, range-checked, per-field errors back to the caller ---
constexpr int kMaxFieldErrors = 8;

struct FieldError {
    char field[24];
    char msg[40];
};

struct ConfigErrors {
    FieldError items[kMaxFieldErrors];
    int  count = 0;
    bool overflowed = false;   // more errors than fit: caller must know the report is truncated
};

void addError(ConfigErrors* e, const char* field, const char* msg);

// Full validation of a candidate config: every range above, plus the cross-field rules §9
// states and the ones the engine needs to stay sane (alarm above setpoint, deco rate <=
// ascent rate, GF-low <= GF-high, required gas slots valid, selected gas configured).
// Returns true when the config is safe to persist and apply.
bool configValidate(const RuntimeConfig& c, ConfigErrors* out);

// --- JSON codec -------------------------------------------------------------
// Fixed-capacity document: scalars are flattened to (path, value) entries —
// "gfLow", "gases[2].fo2" — which makes the parser, the migration model
// (missing entry = take the default) and the loader all simple, and keeps
// capacity explicit. ANY syntax error aborts the whole parse: config is
// all-or-nothing, there is no such thing as a partially-parsed config.
// Duplicate keys: last occurrence wins (documented, tested).
constexpr int kMaxEntries = 112;   // full config ~68 entries; headroom is the gate
constexpr int kPathCap = 40;       // longest real path "gases[8].name" = 15
constexpr int kValStrCap = 24;     // longest real string: gas name (12 + NUL)

struct Entry {
    char   path[kPathCap];
    // double, NOT float: unix wall seconds (~1.8e9) exceed float's exact-integer range
    // (2^24), and cal-age gating reads these values — a float here silently misdates the
    // calibration by up to ~100 s at the parse layer. Fields narrower than double take the
    // value at the loader's own boundary.
    double num;          // kind NUM / BOOL
    char   str[kValStrCap];
    uint8_t kind;        // 1 number, 2 string, 3 bool, 4 null
};

// ~8.1 KB: static scratch in main.cpp, NEVER a task-stack local (decision 40 rule 1).
struct JsonDoc {
    Entry entries[kMaxEntries];
    int   count;
};

const Entry* docFind(const JsonDoc& d, const char* path);

// Parse nested JSON text (objects, arrays of objects, scalars; no \u escapes —
// a config string never carries unicode, so one is treated as corruption).
// On false, `why` explains and the doc must not be trusted.
bool configParseJson(const char* json, int len, JsonDoc* doc, char* why, int whyLen);

// Build a complete config from a parsed document. Missing fields take defaults
// (that IS the migration model); `schema` newer than this build is refused.
// Does NOT validate — call configValidate() on the result before persisting.
bool configFromDoc(const JsonDoc&, RuntimeConfig* out, char* why, int whyLen);

// Emit a complete config as JSON text (stored form and web GET form).
// Returns the length, or -1 on capacity overflow — never a truncated config.
int configToJson(const RuntimeConfig&, char* buf, int cap);

// Convenience: parse + build in one call (caller supplies the JsonDoc scratch).
bool configFromJson(const char* json, int len, RuntimeConfig* out,
                    JsonDoc* scratch, char* why, int whyLen);

}  // namespace cfg
