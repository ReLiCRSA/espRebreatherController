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
    BARO_RANGE = 13, // BMP085 fallback: depth saturates ~9 m
    OLED = 14,
    INA219 = 15,
    I2C_BUS_STUCK = 16,
    LITTLEFS = 17,
    NVS = 18,
    CONFIG_CORRUPT = 19,
    TISSUE_STATE_STALE = 20,
    EXTERNAL_WATCHDOG = 21,

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
        case FaultCode::BARO:
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
        case FaultCode::BARO: return "BARO";
        case FaultCode::BARO_RANGE: return "BARO_RANGE_9M";
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
