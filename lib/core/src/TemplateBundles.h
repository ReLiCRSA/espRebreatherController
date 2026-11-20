#ifndef TEMPLATE_BUNDLES_H
#define TEMPLATE_BUNDLES_H

/*
 * TemplateBundles.h — §12 bundled templates as build-in constants. M6 mirrors these to
 * LittleFS /display/templates/ and the web editor; they live in lib/core so the native
 * suite can parse/validate/fit-render every shipped template (an unvalidatable bundled
 * template is a display fault waiting at the water).
 *
 * §12 lists five bundles; wide_256x64 and color_128x128 arrive with their SPI panel
 * decisions (§20 items 6/15, pin budget unresolved) — bundling them now would ship
 * templates that cannot be validated against a real driver, so they are honestly
 * deferred rather than stubbed.
 *
 * Every page carries the SAFETY fields (ppo2_used, depth_m, tts, fault_banner) — the
 * validator would reject them otherwise; that is the invariant proving itself.
 */

#include <cstdint>

namespace disp {
namespace bundles {

// default_128x64 — the dive page (mirrors §12's example shape)
inline const char* default_128x64() {
    return
        "@size 128x64 @font 8 @page 1\n"
        "big  {{ppo2_used:4.2f}}  {{depth_m:5.1f}}m\n"
        "row  NDL {{ndl:d}} TTS {{tts:d}} CEIL {{ceiling_m:2.1f}}\n"
        "row  SP {{setpoint:4.2f}}  GF {{gf_now:3.0f}}  {{gas}}\n"
        "row  {{mode}} MOD {{mod_m:d}} CNS {{cns_pct:d}}%\n"
        "row  VBAT {{v_bat:4.2f}}  {{fault_banner}}\n"
        "@if sim  SIM {{sim_flag}}\n";
}

// compact_128x32 — half-height panel: only the un-droppable essentials fit
inline const char* compact_128x32() {
    return
        "@size 128x32 @font 8 @page 1\n"
        "big  {{ppo2_used:4.2f}}  {{depth_m:5.1f}}m\n"
        "row  TTS {{tts:d}}  NDL {{ndl:d}}\n"
        "row  {{fault_banner}}\n"
        "@if sim SIM {{sim_flag}}\n";
}

// bench_128x64 — per-cell mV/ppO2 + SIM marker + faults (§12 bench template).
// Carries TTS (SAFETY class) even though M4+ is what computes it: the validator's
// SAFETY rule applies to every bundled template too, and the live bench panel renders
// this one — an until-M4 zero shown in a field the diver knows is TTS beats a template
// the validator must reject.
inline const char* bench_128x64() {
    return
        "@size 128x64 @font 8 @page 1\n"
        "big  {{ppo2_used:4.2f}}  {{depth_m:5.1f}}m\n"
        "row  mV {{c1_mv:4d}} {{c2_mv:4d}} {{c3_mv:4d}}\n"
        "row  P {{ppo2_c1:4.2f}} {{ppo2_c2:4.2f}} {{ppo2_c3:4.2f}}\n"
        "row  T {{temp_c:4.1f}}C  V {{v_bat:4.2f}}\n"
        "row  TTS {{tts:d}}  {{fault_banner}}\n"
        "@if sim SIM {{sim_flag}}\n";
}

// The §12 fit-report example, verbatim intent: 9 content lines on a 128x64 (8-line)
// panel — the fit pass must drop the DETAIL tail and report it.
inline const char* fitdemo_9on8() {
    return
        "@size 128x64 @font 8 @page 1\n"
        "big  {{ppo2_used:4.2f}}  {{depth_m:5.1f}}m\n"
        "row  NDL {{ndl:d}}  TTS {{tts:d}}\n"
        "row  CEIL {{ceiling_m:4.1f}}\n"
        "row  SET {{setpoint:4.2f}}  CNS {{cns_pct:d}}%\n"
        "row  MOD {{mod_m:d}}  {{gas}}\n"
        "row  V_BAT {{v_bat:4.2f}}  {{fault_banner}}\n"
        "row  HE {{fhe:4.2f}}  END {{end_m:d}}  DEN {{gas_density:4.1f}}\n"
        "row  OTU {{otu:d}}  DGU {{dgu:d}}\n"
        "row  TEMP {{temp_c:4.1f}}  CAL {{cal_age_h:4.1f}}\n";
}

// Validator demo: omits tts + fault_banner and conditions depth_m — three rule classes.
inline const char* invalid_missing_safety() {
    return
        "@size 128x64 @font 8 @page 1\n"
        "big  {{ppo2_used:4.2f}}\n"
        "@if sim  {{depth_m:5.1f}}m\n";
}

}  // namespace bundles
}  // namespace disp

#endif
