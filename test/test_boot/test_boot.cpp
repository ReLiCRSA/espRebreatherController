// Boot / fault-report contract tests. These are not decoration: the arming gate and
// /api/health both read this object, so a fault that gets lost or a severity that gets
// downgraded is a safety hole (AGENTS.md 4, 13, 17 "no silent fallbacks").
#include <stdio.h>
#include <string.h>

#include "BootReport.h"
#include "unity.h"

void setUp() {}
void tearDown() {}

namespace {
constexpr uint32_t T0 = 1000;
}

void test_missing_device_is_recorded_with_default_severity() {
    boot::Report r;
    r.raise(FaultCode::ADS1115_U1, T0, "no response at 0x48");
    const boot::Entry* e = r.findEntry(FaultCode::ADS1115_U1);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->present);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Severity::FATAL), static_cast<int>(e->severity));
    TEST_ASSERT_EQUAL_STRING("no response at 0x48", e->detail);
    TEST_ASSERT_TRUE(r.active(FaultCode::ADS1115_U1));
    TEST_ASSERT_TRUE(r.blocksArming());
}

void test_a_present_device_raises_nothing() {
    // The probe path must not invent a fault for a device that answered, otherwise every boot
    // shows a wall of noise and real faults stop being readable.
    boot::Report r;
    TEST_ASSERT_FALSE(r.active(FaultCode::BARO));
    TEST_ASSERT_EQUAL_INT(0, r.countActive());
    TEST_ASSERT_FALSE(r.blocksArming());
}

void test_hotplug_closes_the_fault_but_keeps_the_history() {
    boot::Report r;
    r.raise(FaultCode::BARO, T0, "no response at 0x7C");
    TEST_ASSERT_TRUE(r.blocksArming());
    r.clear(FaultCode::BARO, T0 + 5000);
    TEST_ASSERT_FALSE(r.active(FaultCode::BARO));
    TEST_ASSERT_FALSE(r.blocksArming());
    // Still listed for /api/health, marked not present: "it was missing at boot" is information.
    const boot::Entry* e = r.findEntry(FaultCode::BARO);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FALSE(e->present);
    TEST_ASSERT_EQUAL_UINT32(T0 + 5000, e->lastMs);
}

void test_reprobe_bumps_attempts_without_duplicating_the_entry() {
    boot::Report r;
    for (int i = 0; i < 7; ++i) r.raise(FaultCode::INA219, T0 + i * 5000, "no response at 0x40");
    TEST_ASSERT_EQUAL_INT(1, r.count());
    const boot::Entry* e = r.findEntry(FaultCode::INA219);
    TEST_ASSERT_EQUAL_INT(7, e->attempts);
}

void test_severity_only_escalates() {
    // Boot probes at ERROR; the same code mid-dive is FATAL. A later lower-severity raise
    // must never walk an existing FATAL back down.
    boot::Report r;
    r.raise(FaultCode::VOTING_FAILED, Severity::FATAL, T0);
    r.raise(FaultCode::VOTING_FAILED, Severity::WARN, T0 + 1000, "1 of 3 cells");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Severity::FATAL),
                          static_cast<int>(r.findEntry(FaultCode::VOTING_FAILED)->severity));
}

void test_bmp085_fallback_marks_the_depth_ceiling() {
    // Risk 1: the fallback baro saturates near 9 m and that must be visible, never inferred.
    boot::Report r;
    r.raise(FaultCode::BARO_RANGE, Severity::ERROR, T0, "depth clamps 9 m");
    TEST_ASSERT_EQUAL_STRING("BARO_RANGE_9M", faultName(FaultCode::BARO_RANGE));
    TEST_ASSERT_TRUE(r.active(FaultCode::BARO_RANGE));
}

void test_solenoid_absent_does_not_block_arming() {
    // The valve is optional hardware (AGENTS.md 8): absent must be INFO, and INFO must not
    // refuse an arm, or a unit without a fitted valve could never be used as a monitor.
    boot::Report r;
    r.raise(FaultCode::SOLENOID_DISABLED, Severity::INFO, T0, "inert, not fitted");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Severity::INFO),
                          static_cast<int>(defaultSeverity(FaultCode::SOLENOID_DISABLED)));
    TEST_ASSERT_FALSE(r.blocksArming());
}

void test_summary_line_names_faults_and_counts_severities() {
    boot::Report r;
    r.raise(FaultCode::ADS1115_U2, T0); // FATAL
    r.raise(FaultCode::BARO, T0);       // ERROR
    char line[64];
    r.summaryLine(line, sizeof(line));
    printf("  summary: %s\n", line);
    TEST_ASSERT_NOT_NULL(strstr(line, "ADS1115_U2"));
    TEST_ASSERT_NOT_NULL(strstr(line, "BARO"));
    TEST_ASSERT_NOT_NULL(strstr(line, "F:1"));
    TEST_ASSERT_NOT_NULL(strstr(line, "E:1"));

    char clean[16];
    boot::Report ok;
    ok.summaryLine(clean, sizeof(clean));
    TEST_ASSERT_EQUAL_STRING("ALL SENSORS OK", clean);
}

void test_summary_line_clips_to_whole_names_and_marks_the_clip() {
    // OLED lines are short. A clipped line must still say something is wrong, must stay inside
    // the buffer, and must never clip a code into a different-looking code.
    boot::Report r;
    r.raise(FaultCode::ADS1115_U2, T0);
    r.raise(FaultCode::CELL_BIAS, T0);

    char buf[40] = {"PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP"};
    r.summaryLine(buf, 24);
    printf("  clipped(24): %s\n", buf);
    TEST_ASSERT_TRUE(strlen(buf) < 24);
    TEST_ASSERT_EQUAL_CHAR('P', buf[24]); // canary: nothing written past the requested capacity
    TEST_ASSERT_NOT_NULL(strstr(buf, "F:2"));
    TEST_ASSERT_TRUE(strstr(buf, "ADS1115_") == nullptr); // no half-written codes
    TEST_ASSERT_NOT_NULL(strstr(buf, "+..."));
}

void test_capacity_exhaustion_keeps_earlier_faults() {
    // 32 slots, 40 raises: the first faults must survive. Dropping the *earliest* faults
    // would hide the one that matters most (the boot probe failures).
    boot::Report r;
    for (int i = 0; i < 40; ++i)
        r.raise(static_cast<FaultCode>(1000 + i), Severity::WARN, T0);
    TEST_ASSERT_TRUE(r.active(static_cast<FaultCode>(1000)));
    TEST_ASSERT_TRUE(r.active(static_cast<FaultCode>(1031)));
    TEST_ASSERT_EQUAL_INT(boot::kCapacity, r.count());
}

void test_highest_severity_reports_the_worst_active_fault() {
    boot::Report r;
    r.raise(FaultCode::CELL_DRIFT, T0); // WARN
    r.raise(FaultCode::CELL_BIAS, T0);  // FATAL
    r.clear(FaultCode::CELL_BIAS, T0 + 10);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Severity::WARN), static_cast<int>(r.highest()));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_missing_device_is_recorded_with_default_severity);
    RUN_TEST(test_a_present_device_raises_nothing);
    RUN_TEST(test_hotplug_closes_the_fault_but_keeps_the_history);
    RUN_TEST(test_reprobe_bumps_attempts_without_duplicating_the_entry);
    RUN_TEST(test_severity_only_escalates);
    RUN_TEST(test_bmp085_fallback_marks_the_depth_ceiling);
    RUN_TEST(test_solenoid_absent_does_not_block_arming);
    RUN_TEST(test_summary_line_names_faults_and_counts_severities);
    RUN_TEST(test_summary_line_clips_to_whole_names_and_marks_the_clip);
    RUN_TEST(test_capacity_exhaustion_keeps_earlier_faults);
    RUN_TEST(test_highest_severity_reports_the_worst_active_fault);
    return UNITY_END();
}
