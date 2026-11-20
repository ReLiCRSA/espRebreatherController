#include <unity.h>

#include <string.h>

#include "Voting.h"

void setUp() {}
void tearDown() {}

static void makeReadings(const float* ppO2, float slope, CellReading* out) {
    for (int i = 0; i < CELL_COUNT; ++i) {
        out[i].present = true;
        out[i].mv = ppO2[i] * slope;
    }
}

static void uniformSlope(CellCal cal[CELL_COUNT], float slope) {
    for (int i = 0; i < CELL_COUNT; ++i) {
        cal[i].slopeMvPerBar = slope;
        cal[i].offsetMv = 0.0f;
    }
}

void test_three_agree() {
    CellCal cal[CELL_COUNT];
    uniformSlope(cal, 47.6f);
    const float p[CELL_COUNT] = {1.00f, 1.02f, 0.98f};
    CellReading r[CELL_COUNT];
    makeReadings(p, 47.6f, r);
    VoteOutcome v = evaluateCells(r, cal, VoteConfig{}, false);
    TEST_ASSERT_EQUAL_INT((int)VoteStatus::OK, (int)v.status);
    TEST_ASSERT_EQUAL_INT(3, v.passingCount);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.00f, v.ppO2Used);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.00f, v.medianPpO2);
}

void test_one_outlier_ignored() {
    CellCal cal[CELL_COUNT];
    uniformSlope(cal, 47.6f);
    const float p[CELL_COUNT] = {1.10f, 1.11f, 1.48f}; // classic third-cell drift
    CellReading r[CELL_COUNT];
    makeReadings(p, 47.6f, r);
    VoteOutcome v = evaluateCells(r, cal, VoteConfig{}, false);
    TEST_ASSERT_EQUAL_INT((int)VoteStatus::OK, (int)v.status);
    TEST_ASSERT_EQUAL_INT(2, v.passingCount);
    TEST_ASSERT_FALSE(v.cell[2].passesVote);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.105f, v.ppO2Used);
}

void test_dead_cell_leaves_two_good() {
    CellCal cal[CELL_COUNT];
    uniformSlope(cal, 47.6f);
    CellReading r[CELL_COUNT];
    const float p[CELL_COUNT] = {1.00f, 1.01f, 0.0f};
    makeReadings(p, 47.6f, r);
    r[2].present = false;
    VoteOutcome v = evaluateCells(r, cal, VoteConfig{}, false);
    TEST_ASSERT_EQUAL_INT((int)VoteStatus::OK, (int)v.status);
    TEST_ASSERT_EQUAL_INT(2, v.passingCount);
    TEST_ASSERT_EQUAL_INT((int)FaultCode::CELL_DEAD, (int)v.cell[2].fault);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 1.005f, v.ppO2Used);
}

void test_two_disagreeing_fails_to_lowest() {
    CellCal cal[CELL_COUNT];
    uniformSlope(cal, 47.6f);
    CellReading r[CELL_COUNT];
    const float p[CELL_COUNT] = {0.60f, 1.40f, 0.0f};
    makeReadings(p, 47.6f, r);
    r[2].present = false;
    VoteOutcome v = evaluateCells(r, cal, VoteConfig{}, false);
    TEST_ASSERT_EQUAL_INT((int)VoteStatus::FAILED, (int)v.status);
    // Conservative: the lowest reading drives the loop.
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.60f, v.ppO2Used);
}

void test_all_three_disagree_reports_drift_on_median() {
    // Note the geometry: with three usable cells the median is always within its own band, so
    // "nothing agrees" can only happen with two usable cells. Three wildly different readings
    // therefore land in DRIFT_ONE using the median, per AGENTS.md 5, not VOTING_FAILED.
    CellCal cal[CELL_COUNT];
    uniformSlope(cal, 47.6f);
    CellReading r[CELL_COUNT];
    const float p[CELL_COUNT] = {0.50f, 1.00f, 1.60f};
    makeReadings(p, 47.6f, r);
    VoteOutcome v = evaluateCells(r, cal, VoteConfig{}, false);
    TEST_ASSERT_EQUAL_INT((int)VoteStatus::DRIFT_ONE, (int)v.status);
    TEST_ASSERT_EQUAL_INT(1, v.passingCount);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.00f, v.ppO2Used);
    TEST_ASSERT_TRUE(v.maxDeviationFrac > 0.5f);
}

void test_stuck_high_cell_is_outvoted() {
    // The classic galvanic failure at depth: one cell saturates high and reads a constant.
    CellCal cal[CELL_COUNT];
    uniformSlope(cal, 47.6f);
    CellReading r[CELL_COUNT];
    const float p[CELL_COUNT] = {1.30f, 1.31f, 1.30f};
    makeReadings(p, 47.6f, r);
    r[1].mv = 75.0f; // pinned at ~1.57 bar regardless of reality
    VoteOutcome v = evaluateCells(r, cal, VoteConfig{}, false);
    TEST_ASSERT_EQUAL_INT((int)VoteStatus::OK, (int)v.status);
    TEST_ASSERT_FALSE(v.cell[1].passesVote);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.30f, v.ppO2Used);
}

void test_in_deco_uses_lowest_passing() {
    CellCal cal[CELL_COUNT];
    uniformSlope(cal, 47.6f);
    CellReading r[CELL_COUNT];
    const float p[CELL_COUNT] = {0.70f, 0.82f, 0.78f};
    makeReadings(p, 47.6f, r);
    VoteOutcome notDeco = evaluateCells(r, cal, VoteConfig{}, false);
    VoteOutcome inDeco = evaluateCells(r, cal, VoteConfig{}, true);
    TEST_ASSERT_EQUAL_INT((int)VoteStatus::OK, (int)notDeco.status);
    TEST_ASSERT_TRUE(notDeco.ppO2Used > 0.76f);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.70f, inDeco.ppO2Used);
}

void test_railed_input_flagged() {
    CellCal cal[CELL_COUNT];
    uniformSlope(cal, 47.6f);
    CellReading r[CELL_COUNT];
    CellReading base[CELL_COUNT];
    const float p[CELL_COUNT] = {1.0f, 1.0f, 1.0f};
    makeReadings(p, 47.6f, base);
    for (int i = 0; i < CELL_COUNT; ++i) r[i] = base[i];
    r[1].mv = 5000.0f; // railed high: plausible-looking garbage
    VoteOutcome v = evaluateCells(r, cal, VoteConfig{}, false);
    TEST_ASSERT_EQUAL_INT((int)FaultCode::CELL_BIAS, (int)v.cell[1].fault);
}

void test_aged_cell_flagged() {
    CellCal cal[CELL_COUNT];
    uniformSlope(cal, 47.6f);
    cal[0].slopeMvPerBar = 22.0f; // below the 30 mV/bar floor
    CellReading r[CELL_COUNT];
    const float p[CELL_COUNT] = {1.0f, 1.0f, 1.0f};
    makeReadings(p, 47.6f, r);
    r[0].mv = 1.0f * cal[0].slopeMvPerBar;
    VoteOutcome v = evaluateCells(r, cal, VoteConfig{}, false);
    TEST_ASSERT_TRUE(v.cell[0].aged);
    TEST_ASSERT_FALSE(v.cell[1].aged);
}

void test_zero_ppo2_uses_tolerance_floor() {
    CellCal cal[CELL_COUNT];
    uniformSlope(cal, 47.6f);
    CellReading r[CELL_COUNT];
    for (int i = 0; i < CELL_COUNT; ++i) {
        r[i].present = true;
        r[i].mv = 0.0f;
    }
    VoteOutcome v = evaluateCells(r, cal, VoteConfig{}, false);
    TEST_ASSERT_EQUAL_INT(3, v.passingCount);
    TEST_ASSERT_EQUAL_INT((int)VoteStatus::OK, (int)v.status);
}

void test_diluent_flush_catches_correlated_error() {
    // All three cells agree with each other but are wrong together: voting is happy,
    // the flush check is not. This is the whole reason the check exists.
    CellCal cal[CELL_COUNT];
    uniformSlope(cal, 47.6f);
    CellReading r[CELL_COUNT];

    // 40 m on a 10/90 diluent: expected ppO2 ~0.39. The loop reads ~0.70 in all three cells:
    // perfectly consistent with each other, and badly wrong.
    const float expected = expectedPpO2ForFlush(0.10f, 5.0f, 0.0627f);
    const float p[CELL_COUNT] = {0.70f, 0.71f, 0.69f};
    makeReadings(p, 47.6f, r);
    VoteOutcome v = evaluateCells(r, cal, VoteConfig{}, false);
    TEST_ASSERT_EQUAL_INT((int)VoteStatus::OK, (int)v.status); // voting cannot see this
    float worst = 0.0f;
    TEST_ASSERT_FALSE(diluentFlushAgrees(v, expected, 0.10f, &worst));
    TEST_ASSERT_TRUE(worst > 0.10f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.4937f, expected);

    // Correct diluent reading passes.
    const float ok[CELL_COUNT] = {expected, expected + 0.01f, expected - 0.01f};
    makeReadings(ok, 47.6f, r);
    VoteOutcome v2 = evaluateCells(r, cal, VoteConfig{}, false);
    TEST_ASSERT_TRUE(diluentFlushAgrees(v2, expected, 0.10f, &worst));
}

void test_diluent_flush_needs_all_three_cells() {
    // A dead cell must not let the flush check pass silently: an unverified cell is not a
    // verified one (AGENTS.md 19 risk 2 - correlated converter failure).
    CellCal cal[CELL_COUNT];
    uniformSlope(cal, 47.6f);
    CellReading r[CELL_COUNT];
    const float expected = expectedPpO2ForFlush(0.21f, 2.0f, 0.0627f);
    const float ok[CELL_COUNT] = {expected, expected, expected};
    makeReadings(ok, 47.6f, r);
    r[2].present = false;
    VoteOutcome v = evaluateCells(r, cal, VoteConfig{}, false);
    float worst = 0.0f;
    TEST_ASSERT_FALSE(diluentFlushAgrees(v, expected, 0.10f, &worst));
}

void test_sigma_stability_gate() {
    const float s[10] = {10.0f, 10.2f, 9.9f, 10.1f, 10.0f, 9.8f, 10.1f, 10.0f, 9.9f, 10.2f};
    float mean = 0.0f;
    const float sd = mvSigma(s, 10, &mean);
    TEST_ASSERT_TRUE(sd < 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 10.02f, mean);

    const float noisy[10] = {10.0f, 14.0f, 6.0f, 13.0f, 7.0f, 12.0f, 8.0f, 15.0f, 5.0f, 11.0f};
    TEST_ASSERT_TRUE(mvSigma(noisy, 10, &mean) > 1.0f);
}

// §6's other statistic: the median is what the slope is computed FROM, so a step in the
// window (a cell being breathed on mid-cal) must not move it, while the mean does move.
void test_median_robust_to_a_step_in_the_window() {
    float s[9] = {10.0f, 10.1f, 9.9f, 10.0f, 10.2f, 90.0f, 90.1f, 89.9f, 90.0f};
    float scratch[9];
    memcpy(scratch, s, sizeof s);
    // Sorted: 9.9 10.0 10.0 10.1 | 10.2 | 89.9 90.0 90.0 90.1 — four steps below the middle
    // and four above, so the median stays inside the 10 mV cluster while the mean moves to 45.6.
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.2f, mvMedian(scratch, 9));   // sorted in place
    float mean = 0.0f;
    mvSigma(scratch, 9, &mean);
    TEST_ASSERT_TRUE(mean > 40.0f);        // the mean moved; the median did not

    float even[4] = {3.0f, 1.0f, 4.0f, 2.0f};
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.5f, mvMedian(even, 4));       // middle pair averaged

    float one[1] = {-4.0f};
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -4.0f, mvMedian(one, 1));       // sign survives
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, mvMedian(nullptr, 0));    // empty is caller's gate
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_three_agree);
    RUN_TEST(test_one_outlier_ignored);
    RUN_TEST(test_dead_cell_leaves_two_good);
    RUN_TEST(test_two_disagreeing_fails_to_lowest);
    RUN_TEST(test_all_three_disagree_reports_drift_on_median);
    RUN_TEST(test_stuck_high_cell_is_outvoted);
    RUN_TEST(test_in_deco_uses_lowest_passing);
    RUN_TEST(test_railed_input_flagged);
    RUN_TEST(test_aged_cell_flagged);
    RUN_TEST(test_zero_ppo2_uses_tolerance_floor);
    RUN_TEST(test_diluent_flush_catches_correlated_error);
    RUN_TEST(test_sigma_stability_gate);
    RUN_TEST(test_median_robust_to_a_step_in_the_window);
    return UNITY_END();
}
