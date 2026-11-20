// M6 config gate suite. §9 validation, §17 migration, and the JSON codec's
// malformed-input behaviour. The parser is a security boundary: it gates what the
// dive model can be configured to run on, so the adversarial cases are the point.
#include <unity.h>

#include "ConfigSchema.h"

#include <stdio.h>
#include <string.h>

using namespace cfg;

static JsonDoc g_doc;   // KB-scale scratch: file scope, mirroring the target's static (decision 40)

static bool load(const char* json, RuntimeConfig* c) {
    char why[96];
    why[0] = 0;
    return configParseJson(json, (int)strlen(json), &g_doc, why, sizeof why) &&
           configFromDoc(g_doc, c, why, sizeof why);
}

static void expectReject(const char* json) {
    RuntimeConfig c;
    TEST_ASSERT_FALSE_MESSAGE(load(json, &c), json);
}

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- decision 40 rule 1:
// these structs are KB-scale and must never become task-stack locals. Pinned exactly
// so a capacity change re-faces the budget deliberately (test_display precedent).
static void test_struct_footprint_pins() {
    TEST_ASSERT_EQUAL_INT_MESSAGE(80, (int)sizeof(Entry), "Entry grew: JsonDoc budget must be re-faced");
    TEST_ASSERT_EQUAL_INT_MESSAGE(8968, (int)sizeof(JsonDoc), "JsonDoc grew: main.cpp static budget must be re-faced");
    TEST_ASSERT_EQUAL_INT_MESSAGE(384, (int)sizeof(RuntimeConfig), "RuntimeConfig grew: config buffers must be re-faced");
}

static void test_defaults_are_valid() {
    RuntimeConfig c{};
    ConfigErrors e;
    TEST_ASSERT_TRUE_MESSAGE(configValidate(c, &e), "factory defaults must validate");
    TEST_ASSERT_EQUAL_INT(0, e.count);
    TEST_ASSERT_FALSE(e.overflowed);
}

// Round trip must be EXACT, not approximate: what GET shows is what boots.
// Comparison is serialization-equality + targeted field asserts, NOT memcmp: the
// RuntimeConfig member initialisers leave padding bytes indeterminate (measured: an
// uninitialised 0x0e in gases[9] padding on the host), so a byte compare can "differ"
// while every field is equal — a false alarm of the risk-13 class if left in the suite.
static bool sameJson(const RuntimeConfig& a, const RuntimeConfig& b) {
    static char ja[3072], jb[3072];
    int na = configToJson(a, ja, sizeof ja);
    int nb = configToJson(b, jb, sizeof jb);
    return na > 0 && nb > 0 && na == nb && memcmp(ja, jb, (size_t)na) == 0;
}

static void test_round_trip_defaults_exact() {
    char buf[3072];
    RuntimeConfig a{}, b{};
    int n = configToJson(a, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_LESS_THAN_INT_MESSAGE((int)sizeof buf, n, "default config JSON unexpectedly large");
    TEST_ASSERT_TRUE_MESSAGE(load(buf, &b), "round-trip parse failed");
    TEST_ASSERT_TRUE_MESSAGE(sameJson(a, b), "round trip changed the config");
}

static void test_round_trip_edited_and_wallclock_exact() {
    RuntimeConfig a{};
    a.gfLow = 0.33f;
    a.pco2Bar = 0.053f;
    a.sgWater = 1.000f;
    a.heSwitchDepthM = 30.0f;
    a.solenoidEnable = true;
    a.wifiDuringDive = true;
    a.lastWallS = 1763600000u;      // unix-2025-ish: the float-num bug would misdate this by ~100 s
    a.calWallS = 1763000000u;
    a.lastWallUptimeS = 47u;
    a.calSlopeMvPerBar[1] = 41.5f;
    snprintf(a.gases[0].name, sizeof a.gases[0].name, "DIL TX 21/35");
    a.gases[0].fhe = 0.35f;
    snprintf(a.gases[3].name, sizeof a.gases[3].name, "DEC 50/0");
    a.gases[3].fo2 = 0.50f;
    a.selectedGas = 3;

    char buf[3072];
    int n = configToJson(a, buf, sizeof buf);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    RuntimeConfig b{};
    TEST_ASSERT_TRUE(load(buf, &b));
    TEST_ASSERT_TRUE_MESSAGE(sameJson(a, b), "edited round trip not exact");
    // targeted exactness: the wall clock fields are the ones a float Entry::num corrupted
    TEST_ASSERT_EQUAL_UINT32(1763600000u, b.lastWallS);
    TEST_ASSERT_EQUAL_UINT32(1763000000u, b.calWallS);
    TEST_ASSERT_EQUAL_UINT32(47u, b.lastWallUptimeS);
    TEST_ASSERT_EQUAL_STRING("DIL TX 21/35", b.gases[0].name);
    TEST_ASSERT_EQUAL_STRING("DEC 50/0", b.gases[3].name);
    TEST_ASSERT_EQUAL_INT(3, b.selectedGas);
}

static void test_writer_overflow_is_negative_not_truncated() {
    RuntimeConfig a{};
    char tiny[64];
    TEST_ASSERT_EQUAL_INT(-1, configToJson(a, tiny, (int)sizeof tiny));
}

// --- migration (§17): absent fields take defaults; newer schema refused -----------
static void test_migration_partial_document() {
    const char* j = "{\"schema\":1,\"gfLow\":0.35,"
                    "\"gases\":[{\"name\":\"DIL EAN32\",\"fo2\":0.32,\"fhe\":0}]}";
    RuntimeConfig c{};
    TEST_ASSERT_TRUE(load(j, &c));
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.35f, c.gfLow);
    TEST_ASSERT_EQUAL_STRING("DIL EAN32", c.gases[0].name);
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.32f, c.gases[0].fo2);
    // slots the document did not mention keep factory defaults — that IS the migration model
    TEST_ASSERT_EQUAL_STRING("O2", c.gases[1].name);
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.00f, c.gases[1].fo2);
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 8.0f, c.loopVolumeL);
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.025f, c.sgWater);
}

static void test_migration_no_schema_key_accepted() {
    RuntimeConfig c{};
    TEST_ASSERT_TRUE(load("{\"gfLow\":0.4}", &c));   // oldest documents: schema key absent
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.4f, c.gfLow);
}

static void test_migration_newer_schema_refused() {
    RuntimeConfig c{};
    TEST_ASSERT_FALSE(load("{\"schema\":99,\"gfLow\":0.4}", &c));
}

// --- parser: every malformed shape is a hard reject, never a partial config -------
static void test_parser_rejects_syntax_matrix() {
    static const char* bad[] = {
        "",
        "not json",
        "{",
        "[1]",                       // top level must be an object
        "{a:1}",                     // unquoted key
        "{'a':1}",                   // single quotes
        "{\"a\"}",                   // no value
        "{\"a\":}",                  // missing value
        "{\"a\":1,}",                // trailing comma
        "{\"a\":1 } x",              // trailing junk
        "{\"a\":0x10}",              // hex float: token scan stops at 'x', junk follows
        "{\"a\":nan}",               // literal spellings blocked (value dispatch has no nan)
        "{\"a\":\"unterminated}",
        "{\"a\":\"tab\there\"}",     // raw control char inside a string
        "{\"a\":\"\\u1234\"}",       // \u escapes are refused by design
        "{\"\":1}",                  // empty key
        "{\"a\":{\"b\":{\"c\":{\"d\":1}}}}",   // depth cap: gases[i].field is the deepest legal
    };
    for (const char* j : bad) expectReject(j);
}

static void test_parser_rejects_nonfinite_huge_number() {
    expectReject("{\"gfLow\":1e999}");     // inf: rejected at parse, not at validation
}

static void test_parser_duplicate_keys_last_wins() {
    RuntimeConfig c{};
    TEST_ASSERT_TRUE(load("{\"gfLow\":0.3,\"gfLow\":0.5}", &c));
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.5f, c.gfLow);
}

static void test_parser_null_takes_default() {
    RuntimeConfig c{};
    TEST_ASSERT_TRUE(load("{\"gfLow\":null,\"loopVolumeL\":null}", &c));
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.30f, c.gfLow);
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 8.0f, c.loopVolumeL);
}

static void test_parser_bool_and_wrong_type() {
    RuntimeConfig c{};
    TEST_ASSERT_TRUE(load("{\"icdGuard\":false}", &c));
    TEST_ASSERT_FALSE(c.icdGuard);
    expectReject("{\"icdGuard\":\"no\"}");     // string where bool expected
    expectReject("{\"lastWallS\":\"yesterday\"}");
    expectReject("{\"selectedGas\":2.5}");     // fractional into int field
}

static void test_parser_capacity_gates() {
    // entry overflow: kMaxEntries keys still fit exactly; one more must be refused
    char big[4096];
    int k = snprintf(big, sizeof big, "{");
    for (int i = 0; i < kMaxEntries + 1; i++) k += snprintf(big + k, sizeof big - k, "%s\"k%d\":1", i ? "," : "", i);
    snprintf(big + k, sizeof big - k, "}");
    expectReject(big);

    // path overflow: a 45-char key puts the path past kPathCap
    expectReject("{\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\":1}");

    // string value overflow: 25-char gas name exceeds kValStrCap at the parser
    expectReject("{\"gases\":[{\"name\":\"ABCDEFGHIJKLMNOPQRSTUVWXY\",\"fo2\":0.21,\"fhe\":0}]}");
}

static void test_parser_flattens_gas_array_to_paths() {
    const char* j = "{\"gases\":["
        "{\"name\":\"A\",\"fo2\":0.21,\"fhe\":0},{\"name\":\"B\",\"fo2\":1.0,\"fhe\":0},"
        "{\"name\":\"C\",\"fo2\":0.21,\"fhe\":0},{\"name\":\"\",\"fo2\":0,\"fhe\":0},"
        "{\"name\":\"\",\"fo2\":0,\"fhe\":0},{\"name\":\"\",\"fo2\":0,\"fhe\":0},"
        "{\"name\":\"\",\"fo2\":0,\"fhe\":0},{\"name\":\"\",\"fo2\":0,\"fhe\":0},"
        "{\"name\":\"DIL HE\",\"fo2\":0.10,\"fhe\":0.50}]}";
    char why[64];
    TEST_ASSERT_TRUE(configParseJson(j, (int)strlen(j), &g_doc, why, sizeof why));
    TEST_ASSERT_NOT_NULL(docFind(g_doc, "gases[8].name"));
    const Entry* e = docFind(g_doc, "gases[8].fhe");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.5f, (float)e->num);
}

// --- validation: §9's rules and the engine's cross-field sanity, one case each ----
static void expectField(const RuntimeConfig& c, const char* field) {
    ConfigErrors e;
    TEST_ASSERT_FALSE(configValidate(c, &e));
    bool found = false;
    for (int i = 0; i < e.count; i++)
        if (strcmp(e.items[i].field, field) == 0) found = true;
    TEST_ASSERT_TRUE_MESSAGE(found, field);
}

static void test_validation_matrix() {
    RuntimeConfig c;
    c = RuntimeConfig{}; c.gfLow = 0.10f;   expectField(c, "gfLow");          // §9: GF_low >= 20
    c = RuntimeConfig{}; c.gfLow = 0.80f; c.gfHigh = 0.40f; expectField(c, "gfLow");  // GF_low <= GF_high
    c = RuntimeConfig{}; c.lastStopM = 2.0f; expectField(c, "lastStopM");     // §9: >= 3 m
    c = RuntimeConfig{}; c.rateDecoMPerMin = 10.0f; c.rateAscentMPerMin = 8.0f;
        expectField(c, "rateDecoMPerMin");
    c = RuntimeConfig{}; c.spDeepBar = 1.40f; c.spShallowBar = 0.80f;
        expectField(c, "spDeepBar");                                          // deep <= shallow
    c = RuntimeConfig{}; c.ppO2AlarmHighBar = 1.35f;                          // setpoint 1.30: < +0.10
        expectField(c, "ppO2AlarmHighBar");
    c = RuntimeConfig{}; c.ppO2MaxBar = 1.75f; expectField(c, "ppO2MaxBar");  // §9: ppO2_hi <= 1.7
    c = RuntimeConfig{}; c.heSwitchDepthM = 5.0f; expectField(c, "heSwitchDepthM");
    c = RuntimeConfig{}; c.metabolicSurfLpm = 6.0f; expectField(c, "metabolicSurfLpm");
    c = RuntimeConfig{}; c.calSlopeMvPerBar[2] = 29.0f; expectField(c, "calSlope2");
    c = RuntimeConfig{}; c.gases[1].fo2 = 1.05f; expectField(c, "gases[1]");  // §9: FO2 <= 1.00
    c = RuntimeConfig{}; c.gases[3].fo2 = 0.90f; c.gases[3].fhe = 0.90f;
        expectField(c, "gases[3]");                                           // FO2+FHe > 1.0
    c = RuntimeConfig{}; snprintf(c.gases[3].name, 13, "bad;name"); c.gases[3].fo2 = 0.32f;
        expectField(c, "gases[3]");                                           // markup in name
    // NaN must never be "in range": the >=/<= comparison form catches it
    c = RuntimeConfig{}; c.loopVolumeL = 0.0f / 0.0f; expectField(c, "loopVolumeL");
}

static void test_validation_required_gases_and_selection() {
    RuntimeConfig c;
    c = RuntimeConfig{}; c.gases[0] = GasMix{"", 0.0f, 0.0f}; expectField(c, "gases[0]");
    c = RuntimeConfig{}; c.gases[2] = GasMix{"", 0.0f, 0.0f}; expectField(c, "gases[2]");
    c = RuntimeConfig{}; snprintf(c.gases[3].name, 13, "PARTIAL");            // name but no fo2
        expectField(c, "gases[3]");
    c = RuntimeConfig{}; c.selectedGas = 4; expectField(c, "selectedGas");    // empty slot
    c = RuntimeConfig{}; c.selectedGas = 9; expectField(c, "selectedGas");    // out of range
    // a configured staged slot IS selectable and passes on its own
    c = RuntimeConfig{};
    snprintf(c.gases[4].name, 13, "TX 10/80"); c.gases[4].fo2 = 0.10f; c.gases[4].fhe = 0.80f;
    c.selectedGas = 4;
    ConfigErrors e;
    TEST_ASSERT_TRUE_MESSAGE(configValidate(c, &e), "valid hypoxic trimix stage must pass");
}

static void test_validation_reports_overflow_not_dropped_errors() {
    RuntimeConfig c{};
    for (int i = 0; i < kGasCount; i++) c.gases[i] = GasMix{"", 0.0f, 0.0f};   // 3 required errors
    c.gfLow = 0.0f; c.gfHigh = 0.0f; c.lastStopM = 99.0f; c.pco2Bar = 0.0f;
    c.metabolicSurfLpm = 0.0f; c.loopVolumeL = 0.0f;                          // +6 => > kMaxFieldErrors
    ConfigErrors e;
    TEST_ASSERT_FALSE(configValidate(c, &e));
    TEST_ASSERT_EQUAL_INT(kMaxFieldErrors, e.count);
    TEST_ASSERT_TRUE_MESSAGE(e.overflowed, "truncation must be reported, never silent");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_struct_footprint_pins);
    RUN_TEST(test_defaults_are_valid);
    RUN_TEST(test_round_trip_defaults_exact);
    RUN_TEST(test_round_trip_edited_and_wallclock_exact);
    RUN_TEST(test_writer_overflow_is_negative_not_truncated);
    RUN_TEST(test_migration_partial_document);
    RUN_TEST(test_migration_no_schema_key_accepted);
    RUN_TEST(test_migration_newer_schema_refused);
    RUN_TEST(test_parser_rejects_syntax_matrix);
    RUN_TEST(test_parser_rejects_nonfinite_huge_number);
    RUN_TEST(test_parser_duplicate_keys_last_wins);
    RUN_TEST(test_parser_null_takes_default);
    RUN_TEST(test_parser_bool_and_wrong_type);
    RUN_TEST(test_parser_capacity_gates);
    RUN_TEST(test_parser_flattens_gas_array_to_paths);
    RUN_TEST(test_validation_matrix);
    RUN_TEST(test_validation_required_gases_and_selection);
    RUN_TEST(test_validation_reports_overflow_not_dropped_errors);
    return UNITY_END();
}
