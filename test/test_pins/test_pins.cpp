// Pin map tests (lib/core/src/PinMap.h).
//
// Why this suite exists: AGENTS.md 3 assigned GPIO 33 to the vibration motor and the Page button at
// the same time, the "avoid 16/17, they are UART0 on most devkits" note was false for this board, and
// the naive spare-pin list people reach for (20, 24, 28..31) does not exist on ESP32-WROOM-32 at all.
// Those are defects in a *table*, and a table nobody can test stays wrong — so it lives in lib/core
// and this file walks it, the same remedy that fixed the ADC register map (decision 28).
//
// The failure this guards against is silent and asymmetric: a haptic alarm on an input-only pad
// (34..39) never fires, and a pin shared with a motor back-biases a button read.
#include "PinMap.h"
#include "unity.h"

#include <cstdio>

using namespace pins;

void setUp() {}
void tearDown() {}

// Unity's _MESSAGE macros take a string only, so a failing row has to name its own pad.
static const char* at(const char* what, int gpio) {
    static char buf[96];
    snprintf(buf, sizeof(buf), "%s (GPIO %d)", what, gpio);
    return buf;
}

// The defect that started this suite: GPIO 33 claimed twice.
void test_no_two_roles_share_a_pin() {
    for (size_t i = 0; i < kAssignmentCount; ++i) {
        for (size_t j = i + 1; j < kAssignmentCount; ++j) {
            if (kAssignments[i].gpio == kAssignments[j].gpio)
                TEST_FAIL_MESSAGE(at("two roles share one GPIO", kAssignments[i].gpio));
        }
    }
    // Named explicitly because these two are the pair that actually collided (open item 12).
    TEST_ASSERT_NOT_EQUAL(BTN_PAGE, VIBRATOR_OUT);
    TEST_ASSERT_TRUE(BTN_PAGE >= 0);  // an unassigned button is a known gap, not a silent one
    TEST_ASSERT_TRUE(VIBRATOR_OUT >= 0);
}

// Every role must sit on a pad this module bonds out, with the direction it needs.
void test_every_role_is_on_a_pad_that_can_do_its_job() {
    for (size_t i = 0; i < kAssignmentCount; ++i) {
        const Assignment& a = kAssignments[i];
        TEST_ASSERT_NOT_NULL_MESSAGE(a.role, "role without a name");
        TEST_ASSERT_TRUE_MESSAGE(bonded(a.gpio), at("not bonded out on WROOM-32", a.gpio));
        TEST_ASSERT_TRUE_MESSAGE(inputCapable(a.gpio), at("pad cannot read", a.gpio));
        if (a.needsOutput)
            TEST_ASSERT_TRUE_MESSAGE(outputCapable(a.gpio),
                                     at("OUTPUT role on an input-only pad: the alarm or valve never "
                                        "actuates", a.gpio));
        TEST_ASSERT_FALSE_MESSAGE(consolePad(a.gpio), at("role on a console pad", a.gpio));
        TEST_ASSERT_FALSE_MESSAGE(flashPad(a.gpio), at("role on an SPI-flash pad", a.gpio));
    }
}

// Nothing in the map may be a boot strapping pin. That is policy, not accident: 0/2/5/12/15 take
// their boot mode from the level on the pin at reset, and an alarm load or a held button changes
// how the unit starts. If a future map needs one, this test is where the argument has to happen.
void test_no_role_sits_on_a_strapping_pin() {
    for (size_t i = 0; i < kAssignmentCount; ++i)
        TEST_ASSERT_FALSE_MESSAGE(strapping(kAssignments[i].gpio),
                                  at("role on a boot strapping pin", kAssignments[i].gpio));
}

// The module-level claims, stated in words in the header and asserted here. Two different reasons a
// pin is unreachable, and the table now separates them because the measurement does: the core
// rejects 24 and 28..31 as not existing on this die, while 20/37/38 exist and are simply not brought
// out by the WROOM-32 module.
void test_the_unbonded_and_reserved_sets_are_what_the_header_claims() {
    static const int kNoSuchPad[] = {24, 28, 29, 30, 31};
    for (int p : kNoSuchPad) {
        TEST_ASSERT_TRUE_MESSAGE(noSuchPad(p), at("expected no such pad", p));
        TEST_ASSERT_NOT_NULL_MESSAGE(unusableReason(p), at("nonexistent pad reported usable", p));
    }
    static const int kDieOnly[] = {20, 37, 38};
    for (int p : kDieOnly) {
        TEST_ASSERT_TRUE_MESSAGE(dieOnlyPad(p), at("expected a die pad this module does not bond out", p));
        TEST_ASSERT_FALSE_MESSAGE(bonded(p), at("die pad unexpectedly reachable", p));
    }
    for (int p = 6; p <= 11; ++p) {
        TEST_ASSERT_TRUE_MESSAGE(flashPad(p), at("SPI flash pad not marked flash", p));
        TEST_ASSERT_FALSE_MESSAGE(bonded(p), at("SPI flash pad reachable as GPIO — re-check module", p));
    }
    // 1/3 are this board's console (measured: bench command 'g' shows them claimed by UART0, and the
    // boot log and bench RX both work there). 16/17 are not the console on this board.
    TEST_ASSERT_TRUE(consolePad(1));
    TEST_ASSERT_TRUE(consolePad(3));
    TEST_ASSERT_FALSE(consolePad(16));
    TEST_ASSERT_FALSE(consolePad(17));
}

// 34..39: input-only, and no internal pull. Both halves matter — a button on 35 with no external
// pull-up reads undefined, and an alarm on 35 never fires.
void test_input_only_pads_are_input_only_and_have_no_pulls() {
    for (int p = 0; p < kGpioCount; ++p) {
        if (!bonded(p)) continue;
        if (p >= 34) {
            TEST_ASSERT_FALSE_MESSAGE(outputCapable(p),
                                      at("34..39 must be input-only on classic ESP32", p));
            TEST_ASSERT_FALSE_MESSAGE(pullCapable(p), at("34..39 have no internal pull-up/down", p));
        } else {
            TEST_ASSERT_TRUE_MESSAGE(outputCapable(p),
                                     at("bonded pad below 34 unexpectedly not output-capable", p));
        }
    }
}

// The pin budget as a number instead of a paragraph. With the map above there is exactly **one**
// spare, non-strapping, output-capable pad left on this module (GPIO 17). If someone frees a pin —
// dropping DRDY interrupts for OS-bit polling, say (open item 14) — this fails and the budget
// paragraph in AGENTS.md 3 gets re-derived deliberately rather than by accident.
void test_the_spare_output_budget_is_exactly_one_pad() {
    int spares[kGpioCount];
    int count = 0;
    for (int p = 0; p < kGpioCount; ++p) {
        if (isSpare(p) && outputCapable(p)) spares[count++] = p;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count, "the spare output-capable pad budget changed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(SPARE_OUT, spares[0], "the declared spare is not the spare");
}

// Button inputs rely on the internal pull-up, and the pads they sit on must have one (34..39 do
// not). If this ever fails, the harness needs an external resistor and the BOM has to say so.
void test_button_inputs_are_on_pads_with_internal_pull_ups() {
    static const int kButtons[] = {BTN_ARM_A, BTN_ARM_B, BTN_PAGE, BTN_MANUAL_INJECT};
    for (int p : kButtons)
        TEST_ASSERT_TRUE_MESSAGE(pullCapable(p),
                                 at("button pad has no internal pull-up — harness needs a resistor", p));
}

// The cell bus must not share a pad with the display/baro bus (AGENTS.md 2: a stuck OLED must not be
// able to delay a cell sample), and the two DRDY lines must be distinct pads.
void test_the_two_i2c_buses_and_the_drdy_lines_do_not_share_pads() {
    TEST_ASSERT_NOT_EQUAL(I2C0_SDA, I2C1_SDA);
    TEST_ASSERT_NOT_EQUAL(I2C0_SCL, I2C1_SCL);
    TEST_ASSERT_NOT_EQUAL(I2C0_SDA, I2C1_SCL);
    TEST_ASSERT_NOT_EQUAL(I2C0_SCL, I2C1_SDA);
    TEST_ASSERT_NOT_EQUAL(ADS_U1_DRDY, ADS_U2_DRDY);
    TEST_ASSERT_NOT_EQUAL(SOLENOID_OUT, BUZZER_OUT);
    TEST_ASSERT_NOT_EQUAL(SOLENOID_OUT, VIBRATOR_OUT);
}

// Every pad number 0..39 must have a decided capability word: no accidental zeros outside the
// documented unbonded set, and no bonded pad with no direction at all. Out-of-range queries are
// refused rather than read out of bounds.
void test_every_pad_number_has_a_capability_word() {
    for (int p = 0; p < kGpioCount; ++p) {
        const uint32_t c = caps(p);
        if (c == 0u) {
            const bool documented = p == 24 || (p >= 28 && p <= 31);
            TEST_ASSERT_TRUE_MESSAGE(documented,
                                     at("pad has no capability word and is not in the documented "
                                        "no-such-pad set", p));
            continue;
        }
        if (!bonded(p)) continue;  // flash pads and die-only pads have no direction by definition
        TEST_ASSERT_TRUE_MESSAGE((c & (kInput | kOutput)) != 0u, at("reachable pad can do nothing", p));
    }
    TEST_ASSERT_EQUAL_UINT32(0u, caps(-1));
    TEST_ASSERT_EQUAL_UINT32(0u, caps(kGpioCount));
}

// The ADC path must match the wiring documented in AGENTS.md 3 — a regression here means the cell
// chain stops sampling because two functions were handed one pad.
void test_the_adc_path_matches_the_wiring_documented_in_agents_md_3() {
    TEST_ASSERT_EQUAL(18, I2C1_SDA);
    TEST_ASSERT_EQUAL(19, I2C1_SCL);
    TEST_ASSERT_EQUAL(23, ADS_U1_DRDY);
    TEST_ASSERT_EQUAL(4, ADS_U2_DRDY);
    TEST_ASSERT_EQUAL(21, I2C0_SDA);
    TEST_ASSERT_EQUAL(22, I2C0_SCL);
    TEST_ASSERT_EQUAL(26, SOLENOID_OUT);
    TEST_ASSERT_EQUAL(14, WDT_HEARTBEAT);
    TEST_ASSERT_EQUAL(16, VIBRATOR_OUT);  // decision 32: moved off 33, which the Page button keeps
    TEST_ASSERT_EQUAL(33, BTN_PAGE);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_no_two_roles_share_a_pin);
    RUN_TEST(test_every_role_is_on_a_pad_that_can_do_its_job);
    RUN_TEST(test_no_role_sits_on_a_strapping_pin);
    RUN_TEST(test_the_unbonded_and_reserved_sets_are_what_the_header_claims);
    RUN_TEST(test_input_only_pads_are_input_only_and_have_no_pulls);
    RUN_TEST(test_the_spare_output_budget_is_exactly_one_pad);
    RUN_TEST(test_button_inputs_are_on_pads_with_internal_pull_ups);
    RUN_TEST(test_the_two_i2c_buses_and_the_drdy_lines_do_not_share_pads);
    RUN_TEST(test_every_pad_number_has_a_capability_word);
    RUN_TEST(test_the_adc_path_matches_the_wiring_documented_in_agents_md_3);
    return UNITY_END();
}
