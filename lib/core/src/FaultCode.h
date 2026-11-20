// Single registry of fault and alarm codes (AGENTS.md 17). These values are stable once
// shipped: they appear in dive logs, fault rings and user-facing docs. Never renumber.
#pragma once
#include <stdint.h>

enum class FaultCode : uint16_t {
    NONE = 0,

    // Sensor / hardware presence
    ADS1115_U1 = 10,
    ADS1115_U2 = 11,
    BARO = 12,
    BARO_RANGE = 13, // fitted baro cannot cover the design depth range (decision 39 tiering):
                     // the BMP085/180 fallback saturates at 1.1 bar abs — under 1 m of water —
                     // and the LPS28DFW tier-2 at 4.06 bar abs — ~31 m at this bench. Neither
                     // figure is recited: the banner quotes the COMPUTED ceiling from
                     // depth::ceilingMeters() against the learned Psurf (decision 38;
                     // test/test_depth pins the hydrostatics, test/test_lps28 the tier-2 case).
    OLED = 14,
    INA219 = 15,
    I2C_BUS_STUCK = 16,
    LITTLEFS = 17,
    NVS = 18,
    CONFIG_CORRUPT = 19,
    TISSUE_STATE_STALE = 20,
    EXTERNAL_WATCHDOG = 21,
    // A presence probe ACKs an ADS1015 exactly like an ADS1115, and the parts differ in sample
    // rate and LSB size. Identity is therefore checked by *measured conversion period* at the
    // design rate (the two parts' RATE tables differ 100x at the same field value), not by the ID
    // register — see src/sensors/Ads1115Bus.h.
    ADC_PART_WRONG = 22,
    // A converter that ACKs its address but does not sample at a rate the control loop and the
    // coil/ADC interlock can be built on: no conversion at all, a conversion far too fast (another
    // part's rate table), or one far too slow. The detail string carries the measured period, and
    // bench command 'd' characterises the whole rate table.
    ADC_CONV_BAD = 23,
    // The ID register does not answer with the ADS1115's documented 0x85. Informational in
    // severity: measured 2026-11-19, the bench module reads 0x00 there while its config register
    // reads back the datasheet power-on default exactly, so a zero ID marks an unmarked/clone die,
    // not a wrong part. It is said out loud (AGENTS.md 17) but it does not ground the unit — the
    // conversion-timing checks are what prove fitness.
    ADC_ID_UNKNOWN = 24,
    // The converter's measured conversion period does not match the datasheet's for the rate field
    // written, i.e. its rate table is not the ADS1115's. Measured on the first bench module
    // 2026-11-19: the design's 32 SPS field yields 16.1 SPS, and the part implements only four rates
    // (8/16/32/64 SPS) instead of eight. It samples, so it is not fatal, but the per-cell sample rate
    // and the coil/ADC interlock window are then not the designed ones and must be re-derived from
    // measurement (bench command 'd'). Held at WARN deliberately: this is a parts-sourcing finding
    // for the bench to resolve, not something to refuse a dive at the water for.
    ADC_RATE_MISMATCH = 25,

    // The barometer ACKs but cannot produce a trustworthy pressure: bad cal ROM (the DS's own
    // 0/0xFFFF rule), a conversion that never completes, or a result outside the part's
    // 300-1100 hPa range. Presence was never identity (risk 21) — the first real conversion on
    // the bench BMP085 is what closes item 17's second half, and this code is what says out loud
    // when it does not. Same logic as ADC_CONV_BAD one register over: a depth input that does not
    // measure must refuse an arm, not silently report an extrapolation (risk 1).
    BARO_CONV_BAD = 26,

    // The only depth source that passed identity is the BMP085/180 — a BENCH sensor since
    // decision 39 (300-1100 hPa = a water column under 1.3 m; MS5837 is primary, LPS28DFW
    // tier 2). Bench mode works fully with it, and it is a fine cross-check, but it must never
    // be what a dive arms on. Takes the previously-unassigned value 27 deliberately: fault
    // values are stable once shipped, and test_boot's walk keeps the gap list honest.
    BARO_BENCH_ONLY = 27,

    // Power
    VBAT_LOW = 30,
    RAIL_3V3_LOW = 31,

    // O2 cell health / voting
    CELL_BIAS = 40,      // inputs railed or common-mode resistors missing
    CELL_DEAD = 41,
    CELL_DRIFT = 42,
    CELL_AGED = 43,      // air slope < 30 mV/bar: current limited, may fail at depth
    VOTING_FAILED = 44,  // no two cells agree

    // Solenoid
    SOLENOID_NO_CLICK = 50,
    SOLENOID_SLOW_CLICK = 51,
    SOLENOID_FAIL = 52,
    // INFO by design (not WARN): a valve that is not fitted is a normal configuration, and a
    // WARN line on every bench boot would train the reader to ignore the fault banner.
    // It must never block arming — the loop still computes and displays everything (AGENTS.md 8).
    SOLENOID_DISABLED = 53,
    SOLENOID_TEST_ACTIVE = 54,

    // Diving alarms
    ALARM_PPO2_HIGH = 60,
    ALARM_PPO2_LOW = 61,
    ALARM_CEILING_VIOLATION = 62,
    ALARM_ASCENT_RATE = 63,
    ALARM_CNS_HIGH = 64,
    ALARM_MOD_EXCEEDED = 65,
    ALARM_HYPOXIC = 66,
    ALARM_GAS_DENSITY = 67,
    ALARM_GAS_SWITCH_REFUSED = 68,

    // Procedure / integrity
    CAL_STALE = 80,
    CAL_REJECTED = 81,
    CAL_UNSTABLE = 82,
    SIM_REJECTED_ARMED = 83,
    DEPTH_STALE = 84,
    DILUENT_FLUSH_MISMATCH = 85,
};

enum class Severity : uint8_t { INFO = 0, WARN = 1, ERROR = 2, FATAL = 3 };

// Default severity when a code is raised. A raising site may downgrade at most to this
// level for presentation, but may always escalate (a mid-dive ADC dropout is FATAL even
// though the same code is ERROR at boot).
inline Severity defaultSeverity(FaultCode c) {
    switch (c) {
        case FaultCode::ADS1115_U1:
        case FaultCode::ADS1115_U2:
        case FaultCode::CELL_BIAS:
        case FaultCode::VOTING_FAILED:
        case FaultCode::SOLENOID_NO_CLICK:
        case FaultCode::SOLENOID_FAIL:
        case FaultCode::RAIL_3V3_LOW:
        case FaultCode::ALARM_PPO2_HIGH:
        case FaultCode::ALARM_PPO2_LOW:
            return Severity::FATAL;

        case FaultCode::BARO:
        case FaultCode::BARO_RANGE:
        // ACKs its address but is not the part this design's timing assumes, or does not convert at
        // all: ppO2 cannot be trusted either way, and a presence probe alone would never say so.
        case FaultCode::ADC_PART_WRONG:
        case FaultCode::ADC_CONV_BAD:
        // The baro equivalent: ACKs, but its cal ROM is corrupt, its conversion did not complete,
        // or its result is outside the part's own range. Depth cannot be trusted either way.
        case FaultCode::BARO_CONV_BAD:
        // The only depth source that passed identity is the bench-only BMP180 (decision 39):
        // ERROR at boot like the range banner, but this one DOES refuse an arm — a dive computer
        // with no depth source that reaches dive depth is not a dive computer.
        case FaultCode::BARO_BENCH_ONLY:
        case FaultCode::OLED:
        case FaultCode::INA219:
        case FaultCode::LITTLEFS:
        case FaultCode::NVS:
        case FaultCode::CONFIG_CORRUPT:
        case FaultCode::TISSUE_STATE_STALE:
        case FaultCode::EXTERNAL_WATCHDOG:
        case FaultCode::CELL_DEAD:
        case FaultCode::SOLENOID_SLOW_CLICK:
        case FaultCode::ALARM_CEILING_VIOLATION:
        case FaultCode::ALARM_MOD_EXCEEDED:
        case FaultCode::ALARM_HYPOXIC:
            return Severity::ERROR;

        case FaultCode::I2C_BUS_STUCK:
        case FaultCode::VBAT_LOW:
        case FaultCode::CELL_DRIFT:
        case FaultCode::CELL_AGED:
        case FaultCode::ALARM_CNS_HIGH:
        case FaultCode::ALARM_ASCENT_RATE:
        case FaultCode::ALARM_GAS_DENSITY:
        case FaultCode::ALARM_GAS_SWITCH_REFUSED:
        case FaultCode::CAL_STALE:
        case FaultCode::CAL_REJECTED:
        case FaultCode::CAL_UNSTABLE:
        case FaultCode::SIM_REJECTED_ARMED:
        case FaultCode::DEPTH_STALE:
        case FaultCode::DILUENT_FLUSH_MISMATCH:
        // Present, converting, but the ID register does not match the ADS1115's: worth saying out
        // loud, not worth grounding the dive unit for. See the enum comment.
        case FaultCode::ADC_ID_UNKNOWN:
        case FaultCode::ADC_RATE_MISMATCH:
            return Severity::WARN;

        default:
            return Severity::INFO;
    }
}

// True when an active fault of this code must refuse an arm transition (AGENTS.md 13).
inline bool blocksArming(FaultCode c) {
    switch (c) {
        case FaultCode::ADS1115_U1:
        case FaultCode::ADS1115_U2:
        case FaultCode::ADC_PART_WRONG:
        case FaultCode::ADC_CONV_BAD:
        case FaultCode::BARO:
        case FaultCode::BARO_CONV_BAD:   // a baro that ACKs but cannot measure is not a depth source
        case FaultCode::BARO_BENCH_ONLY: // decision 39: BMP180-class sole depth source is bench-only
        case FaultCode::OLED:              // AGENTS.md 13: the in-water display is required to arm
        case FaultCode::I2C_BUS_STUCK:    // a bus that failed to init carries ppO2 and depth
        case FaultCode::EXTERNAL_WATCHDOG:
        case FaultCode::CELL_BIAS:
        case FaultCode::CELL_DEAD:
        case FaultCode::CELL_AGED:
        case FaultCode::VOTING_FAILED:
        case FaultCode::CONFIG_CORRUPT:
        case FaultCode::VBAT_LOW:
        case FaultCode::CAL_STALE:
        case FaultCode::SOLENOID_NO_CLICK:
        case FaultCode::SOLENOID_FAIL:
            return true;
        default:
            return false;
    }
}

inline const char* faultName(FaultCode c) {
    switch (c) {
        case FaultCode::NONE: return "NONE";
        case FaultCode::ADS1115_U1: return "ADS1115_U1";
        case FaultCode::ADS1115_U2: return "ADS1115_U2";
        case FaultCode::ADC_PART_WRONG: return "ADC_PART_WRONG";
        case FaultCode::ADC_CONV_BAD: return "ADC_CONV_BAD";
        case FaultCode::ADC_ID_UNKNOWN: return "ADC_ID_UNKNOWN";
        case FaultCode::ADC_RATE_MISMATCH: return "ADC_RATE_MISMATCH";
        case FaultCode::BARO: return "BARO";
        // Renamed from "BARO_RANGE_9M" 2026-11-20 (decision 38): the "9 m" in the name was a
        // 10x error (real fallback ceiling < 1 m at sea level). The enum VALUE (13) is stable —
        // only the display name changed, and any log predating this date showing BARO_RANGE_9M
        // should be read as this same code with the corrected ceiling.
        case FaultCode::BARO_RANGE: return "BARO_RANGE_CLAMP";
        case FaultCode::BARO_CONV_BAD: return "BARO_CONV_BAD";
        case FaultCode::BARO_BENCH_ONLY: return "BARO_BENCH_ONLY";
        case FaultCode::OLED: return "OLED";
        case FaultCode::INA219: return "INA219";
        case FaultCode::I2C_BUS_STUCK: return "I2C_BUS_STUCK";
        case FaultCode::LITTLEFS: return "LITTLEFS";
        case FaultCode::NVS: return "NVS";
        case FaultCode::CONFIG_CORRUPT: return "CONFIG_CORRUPT";
        case FaultCode::TISSUE_STATE_STALE: return "TISSUE_STALE";
        case FaultCode::EXTERNAL_WATCHDOG: return "EXT_WATCHDOG";
        case FaultCode::VBAT_LOW: return "VBAT_LOW";
        case FaultCode::RAIL_3V3_LOW: return "RAIL_3V3_LOW";
        case FaultCode::CELL_BIAS: return "CELL_BIAS";
        case FaultCode::CELL_DEAD: return "CELL_DEAD";
        case FaultCode::CELL_DRIFT: return "CELL_DRIFT";
        case FaultCode::CELL_AGED: return "CELL_AGED";
        case FaultCode::VOTING_FAILED: return "VOTING_FAILED";
        case FaultCode::SOLENOID_NO_CLICK: return "SOLENOID_NO_CLICK";
        case FaultCode::SOLENOID_SLOW_CLICK: return "SOLENOID_SLOW_CLICK";
        case FaultCode::SOLENOID_FAIL: return "SOLENOID_FAIL";
        case FaultCode::SOLENOID_DISABLED: return "SOLENOID_DISABLED";
        case FaultCode::SOLENOID_TEST_ACTIVE: return "SOLENOID_TEST";
        case FaultCode::ALARM_PPO2_HIGH: return "PPO2_HIGH";
        case FaultCode::ALARM_PPO2_LOW: return "PPO2_LOW";
        case FaultCode::ALARM_CEILING_VIOLATION: return "CEILING_VIOLATION";
        case FaultCode::ALARM_ASCENT_RATE: return "ASCENT_RATE";
        case FaultCode::ALARM_CNS_HIGH: return "CNS_HIGH";
        case FaultCode::ALARM_MOD_EXCEEDED: return "MOD_EXCEEDED";
        case FaultCode::ALARM_HYPOXIC: return "HYPOXIC";
        case FaultCode::ALARM_GAS_DENSITY: return "GAS_DENSITY";
        case FaultCode::ALARM_GAS_SWITCH_REFUSED: return "GAS_SWITCH_REFUSED";
        case FaultCode::CAL_STALE: return "CAL_STALE";
        case FaultCode::CAL_REJECTED: return "CAL_REJECTED";
        case FaultCode::CAL_UNSTABLE: return "CAL_UNSTABLE";
        case FaultCode::SIM_REJECTED_ARMED: return "SIM_REJECTED";
        case FaultCode::DEPTH_STALE: return "DEPTH_STALE";
        case FaultCode::DILUENT_FLUSH_MISMATCH: return "DILUENT_FLUSH_MISMATCH";
        default: return "UNKNOWN";
    }
}
