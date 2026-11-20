// M6 serialization layer: the WS/state JSON built on the FieldRegistry, the boot-report
// JSON behind /api/health, and the dive-log CSV/header schema (§10's column list).
// These are the payloads the diver reads — a field silently missing from the WS frame is
// the risk-17 "forgotten in a switch" class with a browser as the victim, so the
// registry-coverage assertion is the point of this suite.
#include <unity.h>

#include "BootReport.h"
#include "DiveSchema.h"
#include "StateJson.h"

#include <string.h>

static boot::Report g_rep;

void setUp() { g_rep.reset(); }
void tearDown() {}

// ---------------------------------------------------------------- /api/health JSON
static void test_health_json_empty_report_arms() {
    char buf[512];
    int n = g_rep.toJson(buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"armOk\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"faults\":[]"));
}

static void test_health_json_lists_active_and_history() {
    g_rep.raise(FaultCode::BARO_BENCH_ONLY, 1000, "BMP180 sole: bench only");
    g_rep.raise(FaultCode::ADS1115_U1, 1200);            // FATAL by default
    g_rep.raise(FaultCode::INA219, 1300);
    g_rep.clear(FaultCode::INA219, 9000);                // hot-plugged: history kept (§4)
    char buf[1024];
    int n = g_rep.toJson(buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"armOk\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"code\":\"BARO_BENCH_ONLY\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"sev\":\"FATAL\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"detail\":\"BMP180 sole: bench only\""));
    // the closed entry is still reported, with active:false — that is the §4 rule
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"code\":\"INA219\""));
    const char* ina = strstr(buf, "\"code\":\"INA219\"");
    TEST_ASSERT_NOT_NULL(strstr(ina, "\"active\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"active\":2"));   // top-level: 2 entries still present
}

static void test_health_json_overflow_is_error_not_truncated() {
    g_rep.raise(FaultCode::BARO_RANGE, 10, "clamps 1.2m@978mbar");
    g_rep.raise(FaultCode::ADC_RATE_MISMATCH, 11, "0x48 62mS DS31mS 98%");
    char tiny[64];
    TEST_ASSERT_EQUAL_INT(-1, g_rep.toJson(tiny, sizeof tiny));
}

// ---------------------------------------------------------------- WS / state JSON
static disp::LiveState sampleState() {
    disp::LiveState s{};
    s.ppo2Used = 1.30f;
    s.ppo2Cell[0] = 1.29f; s.cellMv[0] = 10240;
    s.votingPass = true;
    s.depthM = 30.4f; s.ambBar = 4.02f;
    s.ndlMin = 0; s.ttsMin = 12; s.ceilingM = 6.0f;
    s.gfNow = 62.5f;
    snprintf(s.controllingComp, sizeof s.controllingComp, "7 N2");
    s.cnsPct = 43; s.otu = 68;
    snprintf(s.mode, sizeof s.mode, "CCR");
    snprintf(s.gas, sizeof s.gas, "DIL TX 21/35");   // space and slash: must survive quoting
    s.setpoint = 0.70f; s.simFlag = 1;
    s.vBat = 3.87f;
    snprintf(s.faultBanner, sizeof s.faultBanner, "F:0 E:3 W:2 BARO_BENCH_ONLY +...");
    return s;
}

// The registry is the single source of truth (§12). If a field stops appearing in the
// WS frame — renamed, skipped, a loop bound changed — this fails at the desk.
static void test_state_json_covers_every_registry_field() {
    disp::LiveState s = sampleState();
    static char buf[web::kStateJsonCap];
    int n = web::liveStateToJson(s, buf, sizeof buf, 12345);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_LESS_THAN_INT((int)sizeof buf, n);   // the cap is sized, not assumed
    for (uint8_t i = 0; i < disp::kFieldCount; i++) {
        char key[64];
        snprintf(key, sizeof key, "\"%s\":", disp::kFields[i].name);
        const char* hit = strstr(buf, key);
        TEST_ASSERT_NOT_NULL_MESSAGE(hit, key);
    }
}

static void test_state_json_values_types_and_sim_stamp() {
    disp::LiveState s = sampleState();
    static char buf[web::kStateJsonCap];
    int n = web::liveStateToJson(s, buf, sizeof buf, 12345);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"t\":12345"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"sim\":1"));        // risk 11: stamp always present
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ppo2_used\":1.3"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"depth_m\":30.4"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"voting_pass\":true"));   // bool as JSON bool
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"gas\":\"DIL TX 21/35\""));  // spaces survive, quoted
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"controlling_compartment\":\"7 N2\""));
}

static void test_state_json_nan_becomes_zero_not_invalid_json() {
    disp::LiveState s = sampleState();
    s.ppo2Used = 0.0f / 0.0f;
    static char buf[web::kStateJsonCap];
    int n = web::liveStateToJson(s, buf, sizeof buf, 1);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_NULL(strstr(buf, "nan"));
    TEST_ASSERT_NULL(strstr(buf, "inf"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ppo2_used\":0"));
}

static void test_state_json_overflow_is_negative() {
    disp::LiveState s = sampleState();
    char tiny[64];
    TEST_ASSERT_EQUAL_INT(-1, web::liveStateToJson(s, tiny, sizeof tiny, 1));
}

static void test_fields_json_lists_registry_metadata() {
    disp::LiveState s = sampleState();
    static char buf[web::kFieldsJsonCap];
    int n = web::fieldsToJson(s, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    // one entry per registry row, each with metadata + a live value
    int count = 0;
    for (const char* p = buf; (p = strstr(p, "\"name\":")) != nullptr; p++) count++;
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)disp::kFieldCount, count, "/api/fields must list the whole registry");
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"ppo2_used\",\"type\":0,\"unit\":\"bar\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"fault_banner\",\"type\":3"));  // Str
}

// ---------------------------------------------------------------- dive-log schema
static int countFields(const char* s) {
    int commas = 0;
    for (const char* p = s; *p; p++) if (*p == ',') commas++;
    return commas + 1;
}

static void test_csv_header_is_the_23_column_spec_row() {
    TEST_ASSERT_EQUAL_INT_MESSAGE(23, countFields(dlog::csvHeader()),
        "§10's sample row is 23 columns; a change here changes every exported CSV");
    TEST_ASSERT_NOT_NULL(strstr(dlog::csvHeader(), "controlling_compartment"));
    TEST_ASSERT_NOT_NULL(strstr(dlog::csvHeader(), "coil_shunt_peak"));  // M5's column exists now
    TEST_ASSERT_NOT_NULL(strstr(dlog::csvHeader(), "source"));           // risk 11
}

static void test_csv_row_column_count_matches_header() {
    disp::LiveState st = sampleState();
    dlog::Sample s = dlog::sampleFromLiveState(st, 60);
    s.pulses = 250;
    char row[256];
    int n = dlog::csvRow(s, row, sizeof row);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_INT_MESSAGE(countFields(dlog::csvHeader()), countFields(row),
        "a row with a different column count silently shifts every later field in the file");
    // values landed, in order
    TEST_ASSERT_NOT_NULL(strstr(row, "60,30.4,4.02,1.30,10240,0,0,0,6.0,12,43,68,"));
    // No quoting machinery in this CSV writer — the sanitiser substitutes instead, so a
    // stray quote must never reach the file (a quote would survive most CSV parsers and
    // corrupt the column the graph reads).
    TEST_ASSERT_NULL(strstr(row, "\""));
    // the source stamp rides every row (§10)
    TEST_ASSERT_NOT_NULL(strstr(row, ",1"));   // simFlag 1 -> source 1
}

static void test_csv_sanitiser_keeps_columns_true() {
    disp::LiveState st = sampleState();
    dlog::Sample s = dlog::sampleFromLiveState(st, 120);
    snprintf(s.gas, sizeof s.gas, "bad,name\"x");     // validated elsewhere; guard anyway
    char row[256];
    int n = dlog::csvRow(s, row, sizeof row);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_INT(countFields(dlog::csvHeader()), countFields(row));
    TEST_ASSERT_NOT_NULL(strstr(row, "bad name x"));
}

static void test_csv_row_overflow_is_negative() {
    disp::LiveState st = sampleState();
    dlog::Sample s = dlog::sampleFromLiveState(st, 60);
    char tiny[32];
    TEST_ASSERT_EQUAL_INT(-1, dlog::csvRow(s, tiny, sizeof tiny));
}

static void test_header_json_roundtrips_key_fields() {
    dlog::Header h{};
    h.startWallS = 1763600000u;      // exactness: float would misdate this
    h.durationS = 1560;
    snprintf(h.mode, sizeof h.mode, "CCR");
    h.gases[0] = GasMix{ "DIL TX 21/35", 0.21f, 0.35f };
    h.gases[1] = GasMix{ "O2", 1.0f, 0.0f };
    h.gfLow = 0.30f; h.gfHigh = 0.70f;
    h.maxDepthM = 30.4f;
    h.calSlopeMvPerBar[0] = 41.2f;
    h.minPpO2[0] = 0.68f; h.maxPpO2[0] = 1.32f;
    h.otu = 68; h.cnsPct = 43;
    snprintf(h.faults, sizeof h.faults, "F:0 E:3 W:2 BARO_BENCH_ONLY +...");
    h.sampleCount = 26;
    h.source = 1;                    // SIM run in the same ring, tagged (§10)
    char buf[1536];
    int n = dlog::headerToJson(h, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"startWallS\":1763600000"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"DIL TX 21/35\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"source\":1"));
    // balanced-ish structure sanity: a truncated header must be -1, never partial
    char tiny[64];
    TEST_ASSERT_EQUAL_INT(-1, dlog::headerToJson(h, tiny, sizeof tiny));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_health_json_empty_report_arms);
    RUN_TEST(test_health_json_lists_active_and_history);
    RUN_TEST(test_health_json_overflow_is_error_not_truncated);
    RUN_TEST(test_state_json_covers_every_registry_field);
    RUN_TEST(test_state_json_values_types_and_sim_stamp);
    RUN_TEST(test_state_json_nan_becomes_zero_not_invalid_json);
    RUN_TEST(test_state_json_overflow_is_negative);
    RUN_TEST(test_fields_json_lists_registry_metadata);
    RUN_TEST(test_csv_header_is_the_23_column_spec_row);
    RUN_TEST(test_csv_row_column_count_matches_header);
    RUN_TEST(test_csv_sanitiser_keeps_columns_true);
    RUN_TEST(test_csv_row_overflow_is_negative);
    RUN_TEST(test_header_json_roundtrips_key_fields);
    return UNITY_END();
}
