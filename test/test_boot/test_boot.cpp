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
    r.raise(FaultCode::BARO, T0, "no response at 0x76/0x77");
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
    // Risk 1: the fallback baro saturates at its 1.1 bar absolute limit — under 1 m of water at
    // sea level, ~1.2 m at the Stockholm bench (decision 38 corrected the "9 m" claim 10x over) —
    // and the clamp must be visible, never inferred. The detail is what the firmware computes at
    // runtime from depth::ceilingMeters(); this test pins one instance of that string shape.
    boot::Report r;
    r.raise(FaultCode::BARO_RANGE, Severity::ERROR, T0, "clamps 1.2m@978mbar");
    TEST_ASSERT_EQUAL_STRING("BARO_RANGE_CLAMP", faultName(FaultCode::BARO_RANGE));
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

// The tables in FaultCode.h are switch statements with a `default`, so a code added to the enum
// and forgotten in a table silently reports as INFO/"UNKNOWN" — a fault that reaches a diver as a
// blank line in the banner. The code values are deliberately grouped with gaps (10..23, 30..31,
// ...), so walking the value range needs the gaps spelled out: a new code that is missing from a
// table lands outside this list and fails. If you add a code, this test tells you to name it.
void test_every_fault_code_has_a_name_and_a_default_severity() {
    static const int kGaps[][2] = {{1, 9}, {29, 29}, {32, 39}, {45, 49}, {55, 59}, {69, 79}};
    auto isGap = [](int v) {
        for (const auto& g : kGaps)
            if (v >= g[0] && v <= g[1]) return true;
        return false;
    };
    int checked = 0;
    for (int v = 1; v <= static_cast<int>(FaultCode::DILUENT_FLUSH_MISMATCH); ++v) {
        if (isGap(v)) continue;
        const FaultCode c = static_cast<FaultCode>(v);
        char msg[56];
        snprintf(msg, sizeof(msg), "code %d has no faultName entry", v);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(faultName(c), "UNKNOWN"), msg);
        // INFO means "configured, not broken". Anything that is not deliberately INFO must be
        // classified, or it defaults to INFO and disappears from the alarm banner.
        const bool intentionalInfo = (c == FaultCode::SOLENOID_DISABLED ||
                                      c == FaultCode::SOLENOID_TEST_ACTIVE);
        snprintf(msg, sizeof(msg), "code %d (%s) falls through defaultSeverity to INFO", v,
                 faultName(c));
        TEST_ASSERT_TRUE_MESSAGE(intentionalInfo || defaultSeverity(c) != Severity::INFO, msg);
        ++checked;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(46, checked, "the walk no longer covers every enumerator");
}

// Decision 39's tiering gate: BARO_RANGE (the computed clamp banner) stays a non-blocking ERROR —
// "arm shallow or not at all" — but BARO_BENCH_ONLY, the BMP085/180 being the only depth source
// that passed identity, DOES refuse an arm. A depth input that cannot reach dive depth is not a
// depth input; the bench tier exists for bring-up, not for diving.
void test_bench_only_baro_blocks_arming_but_range_banner_does_not() {
    boot::Report r;
    r.raise(FaultCode::BARO_RANGE, T0, "clamps 1.2m@978mbar");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Severity::ERROR),
                          static_cast<int>(r.findEntry(FaultCode::BARO_RANGE)->severity));
    TEST_ASSERT_FALSE_MESSAGE(r.blocksArming(),
                              "the computed range banner blocked arming — decision 38's gate is "
                              "\"arm shallow or not at all\", the block belongs to BARO_BENCH_ONLY");
    r.clear(FaultCode::BARO_RANGE, T0 + 1);

    r.raise(FaultCode::BARO_BENCH_ONLY, T0 + 2, "BMP180 sole: bench only");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(faultName(FaultCode::BARO_BENCH_ONLY), "UNKNOWN"),
                                  "BARO_BENCH_ONLY has no faultName entry");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Severity::ERROR),
                          static_cast<int>(r.findEntry(FaultCode::BARO_BENCH_ONLY)->severity));
    TEST_ASSERT_TRUE_MESSAGE(r.blocksArming(),
                             "a bench-only depth source passed the arming gate — decision 39 says no");
    r.clear(FaultCode::BARO_BENCH_ONLY, T0 + 3);
    TEST_ASSERT_FALSE(r.blocksArming());
}

void test_wrong_adc_part_blocks_arming() {
    // A presence probe ACKs an ADS1015 exactly like an ADS1115, so the measured conversion period
    // is the only thing between a wrong part on the bench and cell readings scaled wrong by 16x.
    boot::Report r;
    r.raise(FaultCode::ADC_PART_WRONG, T0, "0x48 conv 312 us");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Severity::ERROR),
                          static_cast<int>(r.findEntry(FaultCode::ADC_PART_WRONG)->severity));
    TEST_ASSERT_TRUE(r.blocksArming());
    r.clear(FaultCode::ADC_PART_WRONG, T0 + 10);
    TEST_ASSERT_FALSE(r.blocksArming());

    // Same for a converter that samples at a rate the control loop cannot be built on — here, the
    // ~62 ms measured on the bench module against the datasheet's 31.25 ms.
    r.raise(FaultCode::ADC_CONV_BAD, T0 + 20, "0x49 conv 62 ms");
    TEST_ASSERT_TRUE(r.blocksArming());

    // But an unexpected ID register alone must NOT ground the unit: the bench module reads 0x00
    // there while converting correctly at the design rate. Over-blocking is its own failure mode —
    // a diver faced with a refusal at the water on a cosmetic code has been trained to override it.
    boot::Report idr;
    idr.raise(FaultCode::ADC_ID_UNKNOWN, T0, "0x48 ID 0x00 not 0x85");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Severity::WARN),
                          static_cast<int>(idr.findEntry(FaultCode::ADC_ID_UNKNOWN)->severity));
    TEST_ASSERT_FALSE_MESSAGE(idr.blocksArming(), "a bare ID warning blocked arming");
}

void test_display_and_bus_faults_block_arming() {
    // AGENTS.md 13: the OLED, the baro and both converters are required to arm. Bench-only is not
    // armable, and a bus that failed to init is worse than a device that is merely absent.
    boot::Report r;
    r.raise(FaultCode::OLED, T0, "0x3C/0x3D err 2");
    TEST_ASSERT_TRUE(r.blocksArming());
    r.clear(FaultCode::OLED, T0 + 1);

    r.raise(FaultCode::I2C_BUS_STUCK, T0 + 2, "I2C1");
    TEST_ASSERT_TRUE(r.blocksArming());
    r.clear(FaultCode::I2C_BUS_STUCK, T0 + 3);

    // A FATAL refuses arming because it is FATAL, even if nobody listed its code in the table.
    r.raise(FaultCode::RAIL_3V3_LOW, T0 + 4, "3.01 V");
    TEST_ASSERT_TRUE_MESSAGE(r.blocksArming(),
                             "an unlisted FATAL became armable — the table is not the gate");
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
    RUN_TEST(test_every_fault_code_has_a_name_and_a_default_severity);
    RUN_TEST(test_wrong_adc_part_blocks_arming);
    RUN_TEST(test_display_and_bus_faults_block_arming);
    RUN_TEST(test_bench_only_baro_blocks_arming_but_range_banner_does_not);
    return UNITY_END();
}
