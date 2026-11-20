// M7 display-pipeline tests (lib/core/src/Font6x8.h, Framebuf.h, Fields.h, Template.h,
// TemplateBundles.h, SimDisplay.h). §12 gate: fit report + validator rejections.
//
// Font anchors are bytes copied from the same fetched source the header was extracted
// from (Adafruit GFX src/glcdfont.c, 2026-11-20) — they pin the extraction offset and
// the column-major orientation, never a remembered glyph (decision 37/risk 21).
#include "Font6x8.h"
#include "Framebuf.h"
#include "Fields.h"
#include "Template.h"
#include "TemplateBundles.h"
#include "SimDisplay.h"
#include <stdio.h>
#include <string.h>
#include "unity.h"

using namespace disp;
using namespace disp::tmpl;

void setUp() {}
void tearDown() {}

static LiveState demoState() {
    LiveState s;
    s.ppo2Used = 1.30f; s.ppo2Cell[0] = 1.31f; s.ppo2Cell[1] = 1.30f; s.ppo2Cell[2] = 1.29f;
    s.cellMv[0] = 1050; s.cellMv[1] = 1042; s.cellMv[2] = 1035;
    s.votingPass = true;
    s.depthM = 30.1f; s.ambBar = 3.98f; s.ndlMin = 8; s.ttsMin = 12; s.ceilingM = 6.0f;
    s.lastStopM = 6; s.gfNow = 45.0f; strcpy(s.controllingComp, "7 N2");
    s.cnsPct = 12; s.otu = 180; s.modM = 66; s.hypoxicModM = 8; s.endM = 30;
    s.gasDensityGL = 5.2f;
    strcpy(s.mode, "CCR"); strcpy(s.gas, "AIR");
    s.fo2 = 0.21f; s.fHe = 0.0f; s.ppN2 = 2.62f; s.ppHe = 0.0f; s.setpoint = 1.30f;
    s.pulsesPerMin = 4; s.injectedL = 20.4f; strcpy(s.solenoidState, "idle");
    s.vBat = 3.87f; s.v3v3 = 3.31f; s.batteryPct = 78; s.tempC = 25.2f; s.calAgeH = 1.5f;
    s.diveTimeSec = 720; s.surfaceIntervalMin = 0; s.ascRateMMin = 8.0f;
    strcpy(s.bestMix, "EAN32"); s.dguMin = 9;
    strcpy(s.faultBanner, "OK");
    s.simFlag = 1;
    return s;
}

// ---------------------------------------------------------------- font + framebuf

void test_font_glyph_anchors_from_source() {
    // bytes exactly as in glcdfont.c (fetched 2026-11-20): column-major, bit0 = top
    const uint8_t A[5] = {0x7C, 0x12, 0x11, 0x12, 0x7C};
    const uint8_t at[5] = {0x3E, 0x41, 0x5D, 0x59, 0x4E};
    const uint8_t qm[5] = {0x02, 0x01, 0x59, 0x09, 0x06};
    const uint8_t sp[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(A, font::kGlyphs['A' - 0x20], 5);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(at, font::kGlyphs['@' - 0x20], 5);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(qm, font::kGlyphs['?' - 0x20], 5);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(sp, font::kGlyphs[' ' - 0x20], 5);
    TEST_ASSERT_EQUAL_UINT8(95, sizeof(font::kGlyphs) / sizeof(font::kGlyphs[0]));
}

void test_framebuf_text_and_clip() {
    uint8_t buf[128 * 64 / 8];
    Framebuf fb(buf, 128, 64);
    fb.clear();
    TEST_ASSERT_EQUAL_INT16(6 * 3, fb.textWidth("abc"));
    TEST_ASSERT_TRUE(fb.drawText(0, 0, "A"));
    TEST_ASSERT_FALSE(fb.getPixel(0, 0));                // 'A' col0 0x7C: bit0 clear
    TEST_ASSERT_TRUE(fb.getPixel(0, 2));                 // bits 2..6 set
    TEST_ASSERT_FALSE(fb.getPixel(0, 0 + 7));            // bit7 never used
    TEST_ASSERT_FALSE(fb.drawText(128 - 4, 1, "AB"));    // clipped at right edge
    // out-of-range pixels are dropped, not wrapped
    fb.setPixel(200, 200, true);
    TEST_ASSERT_FALSE(fb.getPixel(5, 6));  // gutter column stays blank, sanity intact
}

void test_framebuf_gauge_and_shift() {
    uint8_t buf[128 * 64 / 8], buf2[128 * 64 / 8];
    Framebuf fb(buf, 128, 64);
    fb.clear();  // storage is caller-provided and uninitialised until a render clears it
    fb.gauge(0, 0, 41, 0.5);  // inner 39, fill ~19-20
    int on = 0;
    for (int16_t x = 0; x < 128; x++) for (int16_t y = 0; y < 8; y++) if (fb.getPixel(x, y)) on++;
    // frame 2*(41+7)-4 = 92 border px, inner fill ~ 19*5 = 95 — assert band, not exact
    TEST_ASSERT_TRUE(on > 150 && on < 220);
    // pixelShift wrap: pixel at y=0 must land at y=h-1 for rows=1
    fb.clear();
    fb.setPixel(10, 0, true);
    Framebuf dst(buf2, 128, 64);
    fb.shiftedInto(dst, 1);
    TEST_ASSERT_TRUE(dst.getPixel(10, 63));
    TEST_ASSERT_FALSE(dst.getPixel(10, 0));
}

// ---------------------------------------------------------------- registry + format

void test_fmt_sanitizer_matrix() {
    TEST_ASSERT_TRUE(fmtSanitize("%d", FieldKind::Int));
    TEST_ASSERT_TRUE(fmtSanitize("%5.2f", FieldKind::Float));
    TEST_ASSERT_TRUE(fmtSanitize("%.0f", FieldKind::Float));
    TEST_ASSERT_TRUE(fmtSanitize("%4s", FieldKind::Str));
    TEST_ASSERT_FALSE(fmtSanitize("%x", FieldKind::Int));     // hex banned
    TEST_ASSERT_FALSE(fmtSanitize("%lu", FieldKind::Int));    // length mods banned
    TEST_ASSERT_FALSE(fmtSanitize("%c", FieldKind::Int));
    TEST_ASSERT_FALSE(fmtSanitize("%%", FieldKind::Str));
    TEST_ASSERT_FALSE(fmtSanitize("%d", FieldKind::Str));     // kind mismatch
    TEST_ASSERT_FALSE(fmtSanitize("%s", FieldKind::Float));
    TEST_ASSERT_FALSE(fmtSanitize("%.", FieldKind::Float));
    TEST_ASSERT_FALSE(fmtSanitize("", FieldKind::Int));
    TEST_ASSERT_FALSE(fmtSanitize("%-2000.19999f", FieldKind::Float));  // >11 chars
}

void test_render_field_values() {
    LiveState s = demoState();
    char out[24];
    TEST_ASSERT_TRUE(renderField(s, kF_ppo2_used, "%4.2f", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("1.30", out);
    TEST_ASSERT_TRUE(renderField(s, kF_depth_m, nullptr, out, sizeof(out)));  // default fmt
    TEST_ASSERT_EQUAL_STRING("30.1", out);
    TEST_ASSERT_FALSE(renderField(s, kF_ndl, "%x", out, sizeof(out)));        // rejected
    TEST_ASSERT_EQUAL_STRING("", out);                                        // no stale value
    TEST_ASSERT_FALSE(renderField(s, kF_gas, "%999s", out, sizeof(out)));     // overflow -> fail
    // registry lookup
    TEST_ASSERT_TRUE(fieldByName("ppo2_used") != kFieldCount);
    TEST_ASSERT_TRUE(fieldIsSafety(kF_depth_m));
    TEST_ASSERT_FALSE(fieldIsSafety(kF_ndl));
    TEST_ASSERT_EQUAL_UINT8(kFieldCount, (uint8_t)fieldByName("nope_xyz"));
}

// ---------------------------------------------------------------- parse

void test_parse_accepts_bundles() {
    Template t; ParseResult r;
    TEST_ASSERT_TRUE_MESSAGE(parse(bundles::default_128x64(), t, r), r.msg);
    TEST_ASSERT_EQUAL_INT(128, t.w); TEST_ASSERT_EQUAL_INT(64, t.h);
    TEST_ASSERT_EQUAL_INT(1, t.pageCount);
    TEST_ASSERT_EQUAL_INT(6, t.page[0].lineCount);  // @if line counts too
    TEST_ASSERT_TRUE(parse(bundles::compact_128x32(), t, r));
    TEST_ASSERT_EQUAL_INT(32, t.h);
    TEST_ASSERT_TRUE(parse(bundles::bench_128x64(), t, r));
    TEST_ASSERT_TRUE(parse(bundles::fitdemo_9on8(), t, r));
    TEST_ASSERT_EQUAL_INT(9, t.page[0].lineCount);
    // multi-page
    TEST_ASSERT_TRUE(parse("@size 128x64 @font 8\nbig {{ppo2_used}} {{depth_m}} {{tts}} {{fault_banner}}\n@page 2\nbig {{ppo2_used}} {{depth_m}} {{tts}} {{fault_banner}} {{ndl}}\n", t, r));
    TEST_ASSERT_EQUAL_INT(2, t.pageCount);
    TEST_ASSERT_EQUAL_INT(1, t.page[0].lineCount);
    TEST_ASSERT_EQUAL_INT(1, t.page[1].lineCount);
}

void test_parse_rejections() {
    Template t; ParseResult r;
    TEST_ASSERT_FALSE(parse("big {{ppo2_used}}\n", t, r));                  // no @size
    TEST_ASSERT_EQUAL_UINT8(kNoSize, r.err);
    TEST_ASSERT_FALSE(parse("@size 128x60 @font 8\nbig {{ndl:d}}\n", t, r)); // h not x8
    TEST_ASSERT_EQUAL_UINT8(kBadSize, r.err);
    TEST_ASSERT_FALSE(parse("@size 128 @font 8\nbig {{ndl:d}}\n", t, r));    // no xH
    TEST_ASSERT_EQUAL_UINT8(kBadSize, r.err);
    TEST_ASSERT_FALSE(parse("@size 512x64 @font 8\nbig {{ndl:d}}\n", t, r)); // too wide
    TEST_ASSERT_EQUAL_UINT8(kBadSize, r.err);
    TEST_ASSERT_FALSE(parse("@size 128x64 @font 16\nbig {{ndl:d}}\n", t, r));
    TEST_ASSERT_EQUAL_UINT8(kBadFont, r.err);
    TEST_ASSERT_FALSE(parse("@size 128x64 @font 8 @size 128x32\nbig {{ndl:d}}\n", t, r));
    TEST_ASSERT_EQUAL_UINT8(kBadDirective, r.err);
    TEST_ASSERT_FALSE(parse("@size 128x64\nbogus {{ndl:d}}\n", t, r));       // unknown primitive
    TEST_ASSERT_EQUAL_UINT8(kUnknownPrim, r.err);
    TEST_ASSERT_FALSE(parse("@size 128x64\nbig {{willpower}}\n", t, r));     // unknown field
    TEST_ASSERT_EQUAL_UINT8(kUnknownField, r.err);
    TEST_ASSERT_EQUAL_UINT16(2, r.lineNo);
    TEST_ASSERT_FALSE(parse("@size 128x64\nbig {{ndl:x}}\n", t, r));         // bad fmt (not %...)
    TEST_ASSERT_EQUAL_UINT8(kBadFormat, r.err);
    TEST_ASSERT_FALSE(parse("@size 128x64\nbig {{ndl:%x}}\n", t, r));        // banned conversion
    TEST_ASSERT_EQUAL_UINT8(kBadFormat, r.err);
    TEST_ASSERT_FALSE(parse("@size 128x64\nbig {{depth_m:5.1f\n", t, r));    // unterminated
    TEST_ASSERT_EQUAL_UINT8(kBadRef, r.err);
    TEST_ASSERT_FALSE(parse("@size 128x64\nbig {{ndl:d}}\n@wat 1\n", t, r)); // unknown directive
    TEST_ASSERT_EQUAL_UINT8(kBadDirective, r.err);
    TEST_ASSERT_FALSE(parse("@size 128x64\nbig {{sim_flag}}\n@if nowhere text\n", t, r));
    TEST_ASSERT_EQUAL_UINT8(kUnknownField, r.err);
    TEST_ASSERT_FALSE(parse("@size 128x64\nbar NDL {{nope}}\n", t, r));
    TEST_ASSERT_EQUAL_UINT8(kUnknownField, r.err);
}

// ---------------------------------------------------------------- validator (§12 gate)

void test_validator_accepts_bundles() {
    Template t; ParseResult pr; ValidationResult vr;
    TEST_ASSERT_TRUE(parse(bundles::default_128x64(), t, pr));
    TEST_ASSERT_TRUE_MESSAGE(validate(t, vr), vr.msg);
    TEST_ASSERT_TRUE(parse(bundles::compact_128x32(), t, pr));
    TEST_ASSERT_TRUE_MESSAGE(validate(t, vr), vr.msg);
    TEST_ASSERT_TRUE(parse(bundles::bench_128x64(), t, pr));
    TEST_ASSERT_TRUE_MESSAGE(validate(t, vr), vr.msg);
    TEST_ASSERT_TRUE(parse(bundles::fitdemo_9on8(), t, pr));
    TEST_ASSERT_TRUE_MESSAGE(validate(t, vr), vr.msg);
}

void test_validator_rejects_hidden_safety_fields() {
    Template t; ParseResult pr; ValidationResult vr;
    // SAFETY field inside @if  — rule 2
    TEST_ASSERT_TRUE(parse(bundles::invalid_missing_safety(), t, pr));
    TEST_ASSERT_FALSE(validate(t, vr));
    TEST_ASSERT_NOT_NULL(strstr(vr.msg, "may not be inside @if"));
    TEST_ASSERT_NOT_NULL(strstr(vr.msg, "depth_m"));
    // SAFETY field omitted entirely — rule 1
    TEST_ASSERT_TRUE(parse("@size 128x64 @font 8\nbig {{ppo2_used}}  {{depth_m}}  {{tts}}\n", t, pr));
    TEST_ASSERT_FALSE(validate(t, vr));
    TEST_ASSERT_NOT_NULL(strstr(vr.msg, "omits required SAFETY field: fault_banner"));
    // a second page that omits SAFETY is rejected even though page 1 is clean
    TEST_ASSERT_TRUE(parse("@size 128x64 @font 8\nbig {{ppo2_used}} {{depth_m}} {{tts}} {{fault_banner}}\n@page 2\nbig {{ndl}} {{gas}}\n", t, pr));
    TEST_ASSERT_FALSE(validate(t, vr));
    TEST_ASSERT_NOT_NULL(strstr(vr.msg, "page 2"));
}

// ---------------------------------------------------------------- fit report (§12 gate)

void test_fit_report_drops_detail_lines() {
    // §12's exact example: one DETAIL tail line makes it 9-on-8.
    Template t; ParseResult pr; Layout lay;
    TEST_ASSERT_TRUE(parse(
        "@size 128x64 @font 8\n"
        "big {{ppo2_used}} {{depth_m}} {{tts}} {{fault_banner}}\n"
        "row {{ndl}} {{setpoint}}\nrow {{mod_m}} {{gas}}\nrow {{ceiling_m}} {{v_bat}}\n"
        "row {{cns_pct}} {{mode}}\nrow {{amb_bar}} {{gf_now}}\nrow {{dive_time}} {{battery_pct}}\n"
        "row {{ppn2}} {{controlling_compartment}}\nrow {{fhe}} {{end_m}} {{gas_density}}\n", t, pr));
    layoutPage(t, 0, 128, 64, lay);
    TEST_ASSERT_TRUE_MESSAGE(lay.fit.fits, lay.fit.message);
    TEST_ASSERT_EQUAL_UINT16(9, lay.fit.need);
    TEST_ASSERT_EQUAL_UINT16(8, lay.fit.cap);
    TEST_ASSERT_EQUAL_STRING("template needs 9 lines, panel has 8 - dropped: fhe, end_m, gas_density",
                             lay.fit.message);
    TEST_ASSERT_FALSE(lay.keep[8]);
    TEST_ASSERT_EQUAL_UINT8(8, lay.keptCount);
}

void test_fit_report_priority_order_and_safety_floor() {
    Template t; ParseResult pr; Layout lay;
    // DETAIL goes before SECONDARY before PRIMARY; within a class, later line first.
    TEST_ASSERT_TRUE(parse(
        "@size 128x64 @font 8\n"
        "big {{ppo2_used}} {{depth_m}} {{tts}} {{fault_banner}}\n"
        "row {{ndl}} {{setpoint}}\nrow {{mod_m}} {{gas}}\nrow {{ceiling_m}} {{v_bat}}\n"
        "row {{cns_pct}} {{mode}}\nrow {{amb_bar}} {{gf_now}}\nrow {{dive_time}} {{battery_pct}}\n"
        "row {{ppn2}} {{c1_mv}}\nrow {{fhe}}\n", t, pr));
    layoutPage(t, 0, 128, 64, lay);
    TEST_ASSERT_TRUE(lay.fit.fits);
    TEST_ASSERT_FALSE(lay.keep[8]);   // DETAIL line (fhe) dropped first
    TEST_ASSERT_TRUE(lay.keep[7]);    // SECONDARY (ppn2/c1_mv) survives one drop
    // width mismatch is a rejection no drop can fix
    layoutPage(t, 0, 256, 64, lay);
    TEST_ASSERT_FALSE(lay.fit.fits);
    TEST_ASSERT_NOT_NULL(strstr(lay.fit.message, "width cannot be dropped"));
    // 9 all-SAFETY lines: nothing droppable
    TEST_ASSERT_TRUE(parse(
        "@size 128x64 @font 8\n"
        "big {{ppo2_used}} {{depth_m}} {{tts}} {{fault_banner}}\n"
        "row {{ppo2_used}}\nrow {{depth_m}}\nrow {{tts}}\nrow {{fault_banner}}\n"
        "row {{ppo2_used}} {{tts}}\nrow {{depth_m}} {{fault_banner}}\nrow {{ppo2_used}} {{depth_m}}\n"
        "row {{tts}} {{fault_banner}}\n", t, pr));
    layoutPage(t, 0, 128, 64, lay);
    TEST_ASSERT_FALSE(lay.fit.fits);
    TEST_ASSERT_NOT_NULL(strstr(lay.fit.message, "cannot drop SAFETY lines"));
}

void test_fitdemo_bundle_reports_drops() {
    Template t; ParseResult pr; Layout lay;
    TEST_ASSERT_TRUE(parse(bundles::fitdemo_9on8(), t, pr));
    layoutPage(t, 0, 128, 64, lay);
    TEST_ASSERT_TRUE_MESSAGE(lay.fit.fits, lay.fit.message);
    TEST_ASSERT_EQUAL_UINT8(2, lay.fit.droppedCount);
    TEST_ASSERT_EQUAL_STRING("temp_c", lay.fit.dropped[0]);  // last DETAIL line goes first
    TEST_ASSERT_EQUAL_STRING("cal_age_h", lay.fit.dropped[1]);
}

// ---------------------------------------------------------------- render end-to-end

// Shared parse scratch, mirroring the target pattern (src/main.cpp g_tmpl):
// renderTemplate takes the CALLER's Template storage — it is ~18 KB, see
// test_struct_footprint_kb_scale_never_task_stack below.
static Template g_scratch;

void test_render_default_template_with_sim_stamp() {
    LiveState s = demoState();
    uint8_t buf[128 * 64 / 8], sbuf[128 * 64 / 8];
    Framebuf fb(buf, 128, 64);
    SimDisplay disp(sbuf, 128, 64);
    ParseResult pr; ValidationResult vr; Layout lay; RenderStats st;

    TEST_ASSERT_TRUE_MESSAGE(renderTemplate(bundles::default_128x64(), g_scratch, 0, 128, 64, s, fb, pr, vr, lay, st), vr.msg);
    TEST_ASSERT_EQUAL_UINT8(6, st.linesDrawn);   // 5 always + the @if sim line (simFlag=1)
    TEST_ASSERT_EQUAL_UINT8(0, st.truncated);
    disp.present(fb);
    char ascii[128 * 64 + 64 + 8];
    disp.ascii(ascii, sizeof(ascii));
    TEST_ASSERT_NOT_NULL(memchr(ascii, '#', sizeof(ascii)));   // something was drawn
    // sim line is the 6th drawn line = row 5 (y 40..47) when simFlag=1
    bool simRowPainted = false;
    for (uint16_t y = 40; y <= 47 && !simRowPainted; y++)
        for (uint16_t x = 0; x < 24; x++)
            if (disp.frame().getPixel(x, y)) { simRowPainted = true; break; }
    TEST_ASSERT_TRUE(simRowPainted);

    s.simFlag = 0;
    TEST_ASSERT_TRUE(renderTemplate(bundles::default_128x64(), g_scratch, 0, 128, 64, s, fb, pr, vr, lay, st));
    TEST_ASSERT_EQUAL_UINT8(5, st.linesDrawn);   // @if line skipped
    disp.present(fb);
    simRowPainted = false;
    for (uint16_t y = 40; y <= 47 && !simRowPainted; y++)
        for (uint16_t x = 0; x < 24; x++)
            if (disp.frame().getPixel(x, y)) { simRowPainted = true; break; }
    TEST_ASSERT_FALSE(simRowPainted);  // stale frame replaced, not left over (no silent fallback)
}

void test_render_long_banner_counts_truncation() {
    LiveState s = demoState();
    std::snprintf(s.faultBanner, sizeof(s.faultBanner), "%s", "F:1 E:2 ADS1115 BARO CONV");
    uint8_t buf[128 * 64 / 8];
    Framebuf fb(buf, 128, 64);
    ParseResult pr; ValidationResult vr; Layout lay; RenderStats st;
    TEST_ASSERT_TRUE(renderTemplate(bundles::default_128x64(), g_scratch, 0, 128, 64, s, fb, pr, vr, lay, st));
    TEST_ASSERT_TRUE(st.truncated > 0);   // clipped honestly, and REPORTED (§12 fit report)
    TEST_ASSERT_EQUAL_UINT8(0, st.fieldErrors);  // 25 chars < tmp[48]: it is the LINE that clips, not the field
    // a full-length (47-char) banner still renders — string fields can only clip the line;
    // the field-error path is exercised by the bar-on-non-gaugeable test instead
    std::snprintf(s.faultBanner, sizeof(s.faultBanner), "%s", "F:1 E:2 W:2 ADS1115_U2 BARO INA219 ADC_ID_UNKNOWN");
    TEST_ASSERT_TRUE(renderTemplate(bundles::default_128x64(), g_scratch, 0, 128, 64, s, fb, pr, vr, lay, st));
    TEST_ASSERT_EQUAL_UINT8(0, st.fieldErrors);
    TEST_ASSERT_TRUE(st.truncated > 0);
}

void test_render_bar_on_non_gaugeable_field_counts_error() {
    // `bar {{gas}}` parses (gas is a real field) but has no gauge range: render must
    // count the failure, not paint a plausible-looking empty gauge silently.
    LiveState s = demoState();
    uint8_t buf[128 * 64 / 8];
    Framebuf fb(buf, 128, 64);
    ParseResult pr; ValidationResult vr;
    static Template t;
    const char* bad =
        "@size 128x64 @font 8\n"
        "big {{ppo2_used}} {{depth_m}} {{tts}} {{fault_banner}}\n"
        "bar {{gas}}\n";
    TEST_ASSERT_TRUE(parse(bad, t, pr));
    TEST_ASSERT_TRUE(validate(t, vr));
    Layout lay; RenderStats st;
    layoutPage(t, 0, 128, 64, lay);
    TEST_ASSERT_TRUE(lay.fit.fits);
    renderPage(t, lay, 0, s, fb, st);
    TEST_ASSERT_EQUAL_UINT8(1, st.fieldErrors);
}

void test_render_bad_page_index_rejected() {
    LiveState s = demoState();
    uint8_t buf[128 * 64 / 8];
    Framebuf fb(buf, 128, 64);
    ParseResult pr; ValidationResult vr; Layout lay; RenderStats st;
    TEST_ASSERT_FALSE(renderTemplate(bundles::default_128x64(), g_scratch, 1, 128, 64, s, fb, pr, vr, lay, st));
    TEST_ASSERT_EQUAL_UINT8(kBadPage, vr.err);
}

void test_struct_footprint_kb_scale_never_task_stack() {
    // The 2026-11-20 post-M7 boot-loop: a `Template` LOCAL in displayService() put ~18 KB
    // on the 8 KB loopTask — TG1WDT_SYS_RESET with setup() complete, same defect class as
    // the M1 SimRunner smash (risk 13's "only shows up when you run it", in a new currency:
    // not a wrong number, a wrong footprint). Host suites never saw it: 8 MB host stack.
    // These exact sizes are the budget — if capacities (kMaxLines/kMaxSegs/kMaxPages/...)
    // change, this test must be updated DELIBERATELY, like the test_pins spare budget:
    // the on-target cost of a capacity change has to be faced at the desk, not at the water.
    // Target rule these numbers enforce: KB-scale structs are static or boot-time, never
    // task-stack locals (see src/main.cpp: all four Template users are static).
    TEST_ASSERT_EQUAL_UINT32(38, sizeof(Seg));
    TEST_ASSERT_EQUAL_UINT32(384, sizeof(Line));
    TEST_ASSERT_EQUAL_UINT32(4609, sizeof(Page));
    TEST_ASSERT_EQUAL_UINT32(18442, sizeof(Template));
    TEST_ASSERT_EQUAL_UINT32(342, sizeof(Layout));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_font_glyph_anchors_from_source);
    RUN_TEST(test_framebuf_text_and_clip);
    RUN_TEST(test_framebuf_gauge_and_shift);
    RUN_TEST(test_fmt_sanitizer_matrix);
    RUN_TEST(test_render_field_values);
    RUN_TEST(test_parse_accepts_bundles);
    RUN_TEST(test_parse_rejections);
    RUN_TEST(test_validator_accepts_bundles);
    RUN_TEST(test_validator_rejects_hidden_safety_fields);
    RUN_TEST(test_fit_report_drops_detail_lines);
    RUN_TEST(test_fit_report_priority_order_and_safety_floor);
    RUN_TEST(test_fitdemo_bundle_reports_drops);
    RUN_TEST(test_render_default_template_with_sim_stamp);
    RUN_TEST(test_render_long_banner_counts_truncation);
    RUN_TEST(test_render_bar_on_non_gaugeable_field_counts_error);
    RUN_TEST(test_render_bad_page_index_rejected);
    RUN_TEST(test_struct_footprint_kb_scale_never_task_stack);
    return UNITY_END();
}
