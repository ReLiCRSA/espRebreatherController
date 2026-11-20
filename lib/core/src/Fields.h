#ifndef FIELDS_H
#define FIELDS_H

/*
 * Fields.h — the FieldRegistry (§12): ONE source of truth for name, type, unit, printf
 * format, priority class and value-getter for every field available to display
 * templates, the web WebSocket JSON (M6), GET /api/fields (M6) and the dive-log columns
 * (M6). Adding a field = one row here + one member in LiveState.h.
 *
 * Priority classes drive the fit report's drop order (§12): when a template does not
 * fit, DETAIL goes first, then SECONDARY, then PRIMARY — SAFETY is never dropped and a
 * template that omits or conditions a SAFETY field is rejected by Template.h's validator.
 *
 * Format strings are printf-style but SANITIZED: only %[flags][width][.prec] with
 * conversions matched to the field kind (d/i/u for ints, f/g/u for floats, s for strings;
 * no length modifiers, no x/o/c/p). A format that fails the sanitizer is a template
 * validation error — this also bounds what snprintf ever sees from web-edited text.
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include "LiveState.h"

namespace disp {

enum class FieldKind : uint8_t { Float, Int, Bool, Str };
enum class FieldPriority : uint8_t { Safety, Primary, Secondary, Detail };

enum FieldId : uint8_t {
    kF_ppo2_used, kF_ppo2_c1, kF_ppo2_c2, kF_ppo2_c3,
    kF_c1_mv, kF_c2_mv, kF_c3_mv, kF_voting_pass,
    kF_depth_m, kF_amb_bar, kF_ndl, kF_tts, kF_ceiling_m, kF_last_stop_m,
    kF_gf_now, kF_controlling_compartment, kF_cns_pct, kF_otu,
    kF_mod_m, kF_hypoxic_mod_m, kF_end_m, kF_gas_density,
    kF_mode, kF_gas, kF_fo2, kF_fhe, kF_ppn2, kF_pphe, kF_setpoint,
    kF_pulses_per_min, kF_injected_l, kF_solenoid_state,
    kF_v_bat, kF_v_3v3, kF_battery_pct, kF_temp_c, kF_cal_age_h,
    kF_dive_time, kF_surface_interval, kF_asc_rate, kF_best_mix, kF_dgu,
    kF_fault_banner, kF_sim_flag,
    kFieldCount
};

struct FieldDef {
    FieldId id;
    const char* name;
    FieldKind kind;
    FieldPriority priority;
    const char* fmt;          // default printf format
    const char* unit;         // display unit, may be ""
    float gaugeMin, gaugeMax; // for the `bar` primitive (0/0 = not gaugeable)
};

// The table. SAFETY rows are the §12/§13 reserved banner: templates may restyle them
// but never omit or hide them (Template.h enforces).
static const FieldDef kFields[kFieldCount] = {
    { kF_ppo2_used, "ppo2_used", FieldKind::Float, FieldPriority::Safety, "%.2f", "bar", 0.0f, 1.6f },
    { kF_ppo2_c1, "ppo2_c1", FieldKind::Float, FieldPriority::Secondary, "%.2f", "bar", 0.0f, 1.6f },
    { kF_ppo2_c2, "ppo2_c2", FieldKind::Float, FieldPriority::Secondary, "%.2f", "bar", 0.0f, 1.6f },
    { kF_ppo2_c3, "ppo2_c3", FieldKind::Float, FieldPriority::Secondary, "%.2f", "bar", 0.0f, 1.6f },
    { kF_c1_mv, "c1_mv", FieldKind::Int, FieldPriority::Secondary, "%d", "mV", 0, 0 },
    { kF_c2_mv, "c2_mv", FieldKind::Int, FieldPriority::Secondary, "%d", "mV", 0, 0 },
    { kF_c3_mv, "c3_mv", FieldKind::Int, FieldPriority::Secondary, "%d", "mV", 0, 0 },
    { kF_voting_pass, "voting_pass", FieldKind::Bool, FieldPriority::Primary, "%d", "", 0, 0 },
    { kF_depth_m, "depth_m", FieldKind::Float, FieldPriority::Safety, "%.1f", "m", 0, 0 },
    { kF_amb_bar, "amb_bar", FieldKind::Float, FieldPriority::Primary, "%.2f", "bar", 0.0f, 4.0f },
    { kF_ndl, "ndl", FieldKind::Int, FieldPriority::Primary, "%d", "min", 0, 0 },
    { kF_tts, "tts", FieldKind::Int, FieldPriority::Safety, "%d", "min", 0, 0 },
    { kF_ceiling_m, "ceiling_m", FieldKind::Float, FieldPriority::Primary, "%.1f", "m", 0, 0 },
    { kF_last_stop_m, "last_stop_m", FieldKind::Int, FieldPriority::Primary, "%d", "m", 0, 0 },
    { kF_gf_now, "gf_now", FieldKind::Float, FieldPriority::Primary, "%.0f", "%", 0.0f, 100.0f },
    { kF_controlling_compartment, "controlling_compartment", FieldKind::Str, FieldPriority::Secondary, "%s", "", 0, 0 },
    { kF_cns_pct, "cns_pct", FieldKind::Int, FieldPriority::Primary, "%d", "%", 0, 100 },
    { kF_otu, "otu", FieldKind::Int, FieldPriority::Detail, "%d", "OTU", 0, 0 },
    { kF_mod_m, "mod_m", FieldKind::Int, FieldPriority::Primary, "%d", "m", 0, 0 },
    { kF_hypoxic_mod_m, "hypoxic_mod_m", FieldKind::Int, FieldPriority::Secondary, "%d", "m", 0, 0 },
    { kF_end_m, "end_m", FieldKind::Int, FieldPriority::Detail, "%d", "m", 0, 0 },
    { kF_gas_density, "gas_density", FieldKind::Float, FieldPriority::Detail, "%.1f", "g/L", 0.0f, 10.0f },
    { kF_mode, "mode", FieldKind::Str, FieldPriority::Primary, "%s", "", 0, 0 },
    { kF_gas, "gas", FieldKind::Str, FieldPriority::Primary, "%s", "", 0, 0 },
    { kF_fo2, "fo2", FieldKind::Float, FieldPriority::Secondary, "%.2f", "", 0.0f, 1.0f },
    { kF_fhe, "fhe", FieldKind::Float, FieldPriority::Detail, "%.2f", "", 0.0f, 1.0f },
    { kF_ppn2, "ppn2", FieldKind::Float, FieldPriority::Secondary, "%.2f", "bar", 0.0f, 6.0f },
    { kF_pphe, "pphe", FieldKind::Float, FieldPriority::Secondary, "%.2f", "bar", 0.0f, 6.0f },
    { kF_setpoint, "setpoint", FieldKind::Float, FieldPriority::Primary, "%.2f", "bar", 0.0f, 1.6f },
    { kF_pulses_per_min, "pulses_per_min", FieldKind::Int, FieldPriority::Secondary, "%d", "/min", 0, 0 },
    { kF_injected_l, "injected_l", FieldKind::Float, FieldPriority::Detail, "%.2f", "L", 0, 0 },
    { kF_solenoid_state, "solenoid_state", FieldKind::Str, FieldPriority::Secondary, "%s", "", 0, 0 },
    { kF_v_bat, "v_bat", FieldKind::Float, FieldPriority::Primary, "%.2f", "V", 3.0f, 4.2f },
    { kF_v_3v3, "v_3v3", FieldKind::Float, FieldPriority::Secondary, "%.2f", "V", 3.0f, 3.6f },
    { kF_battery_pct, "battery_pct", FieldKind::Int, FieldPriority::Secondary, "%d", "%", 0, 100 },
    { kF_temp_c, "temp_c", FieldKind::Float, FieldPriority::Detail, "%.1f", "C", 0, 0 },
    { kF_cal_age_h, "cal_age_h", FieldKind::Float, FieldPriority::Detail, "%.1f", "h", 0, 0 },
    { kF_dive_time, "dive_time", FieldKind::Int, FieldPriority::Primary, "%d", "s", 0, 0 },
    { kF_surface_interval, "surface_interval", FieldKind::Int, FieldPriority::Secondary, "%d", "min", 0, 0 },
    { kF_asc_rate, "asc_rate", FieldKind::Float, FieldPriority::Secondary, "%.1f", "m/min", 0, 0 },
    { kF_best_mix, "best_mix", FieldKind::Str, FieldPriority::Detail, "%s", "", 0, 0 },
    { kF_dgu, "dgu", FieldKind::Int, FieldPriority::Detail, "%d", "min", 0, 0 },
    { kF_fault_banner, "fault_banner", FieldKind::Str, FieldPriority::Safety, "%s", "", 0, 0 },
    // sim_flag is PRIMARY, not Safety, deliberately: §12's own bundled example conditions
    // it (`@if sim SIM {{sim_flag}}`), and SAFETY fields may never be conditional. The
    // no-SIM-data-on-dive-outputs rule is risk 11's source stamping, not a template rule.
    { kF_sim_flag, "sim_flag", FieldKind::Int, FieldPriority::Primary, "%d", "", 0, 0 },
};

static_assert(sizeof(kFields) / sizeof(kFields[0]) == kFieldCount,
              "Fields.h table must cover every FieldId exactly once");

inline const FieldDef* fieldById(FieldId id) {
    for (uint8_t i = 0; i < kFieldCount; i++)
        if (kFields[i].id == id) return &kFields[i];
    return nullptr;
}

inline FieldId fieldByName(const char* name) {
    // §12's example conditions on bare "sim" (`@if sim SIM {{sim_flag}}`) — alias to
    // sim_flag rather than diverge from the documented grammar.
    if (std::strcmp(name, "sim") == 0) return kF_sim_flag;
    for (uint8_t i = 0; i < kFieldCount; i++)
        if (std::strcmp(kFields[i].name, name) == 0) return kFields[i].id;
    return kFieldCount;  // sentinel: unknown field
}

inline bool fieldIsSafety(FieldId id) {
    const FieldDef* f = fieldById(id);
    return f && f->priority == FieldPriority::Safety;
}

// SAFETY class enumeration the validator uses to require every SAFETY field on every
// page. Kept as data so a new SAFETY field cannot fall through (risk 17's lesson).
constexpr FieldPriority kSafetyMin = FieldPriority::Safety;
constexpr FieldPriority kSafetyMax = FieldPriority::Safety;

// Format sanitizer. §12's template syntax writes the conversion WITHOUT the leading '%'
// ("{{ppo2_used:2.2f}}", "{{ndl:d}}"); the optional '%' is accepted too. Grammar:
// "%"?[flags -+ #0]*[digits][.[digits]][dfu] Int/Bool, [fgu] Float, [s] Str.
// No length modifiers, no x/o/c/p, no %%. Max 11 chars. Returns false on anything else.
inline bool fmtSanitize(const char* f, FieldKind kind) {
    if (!f || !f[0] || std::strlen(f) > 11) return false;
    size_t i = (f[0] == '%') ? 1 : 0;
    while (i < std::strlen(f) && (f[i] == '-' || f[i] == '+' || f[i] == ' ' || f[i] == '#' || f[i] == '0')) i++;
    while (i < std::strlen(f) && f[i] >= '0' && f[i] <= '9') i++;
    if (i < std::strlen(f) && f[i] == '.') {
        i++;
        if (i < std::strlen(f) && f[i] >= '0' && f[i] <= '9') {
            while (i < std::strlen(f) && f[i] >= '0' && f[i] <= '9') i++;
        } else if (kind != FieldKind::Str) {
            return false;  // bare "." is meaningless for numbers
        }
    }
    if (i + 1 != std::strlen(f)) return false;  // exactly one conversion char at the end
    char c = f[i];
    switch (kind) {
        case FieldKind::Int:  return c == 'd' || c == 'i' || c == 'u';
        case FieldKind::Bool: return c == 'd' || c == 'u';
        case FieldKind::Float: return c == 'f' || c == 'g' || c == 'u';
        case FieldKind::Str:  return c == 's';
    }
    return false;
}

// Render one field's value into out[] with a (default or sanitized-checked) format.
// Returns false if the format was rejected or snprintf overflowed — the caller must
// show a visible marker, never a stale value (no silent fallbacks, §17).
inline bool renderField(const LiveState& s, FieldId id, const char* fmt, char* out, size_t n) {
    const FieldDef* d = fieldById(id);
    if (!d || !out || n == 0) return false;
    const char* use = (fmt && fmt[0]) ? fmt : d->fmt;
    if (!fmtSanitize(use, d->kind)) { out[0] = '\0'; return false; }
    char fbuf[13];  // templates write bare conversions ("4.2f"); snprintf needs the '%'
    if (use[0] != '%') {
        fbuf[0] = '%';
        std::strncpy(fbuf + 1, use, sizeof(fbuf) - 2);
        fbuf[sizeof(fbuf) - 1] = '\0';
        use = fbuf;
    }
    int r;
    switch (id) {
        case kF_ppo2_used:   r = std::snprintf(out, n, use, (double)s.ppo2Used); break;
        case kF_ppo2_c1:     r = std::snprintf(out, n, use, (double)s.ppo2Cell[0]); break;
        case kF_ppo2_c2:     r = std::snprintf(out, n, use, (double)s.ppo2Cell[1]); break;
        case kF_ppo2_c3:     r = std::snprintf(out, n, use, (double)s.ppo2Cell[2]); break;
        case kF_c1_mv:       r = std::snprintf(out, n, use, s.cellMv[0]); break;
        case kF_c2_mv:       r = std::snprintf(out, n, use, s.cellMv[1]); break;
        case kF_c3_mv:       r = std::snprintf(out, n, use, s.cellMv[2]); break;
        case kF_voting_pass: r = std::snprintf(out, n, use, (int)(s.votingPass ? 1 : 0)); break;
        case kF_depth_m:     r = std::snprintf(out, n, use, (double)s.depthM); break;
        case kF_amb_bar:     r = std::snprintf(out, n, use, (double)s.ambBar); break;
        case kF_ndl:         r = std::snprintf(out, n, use, s.ndlMin); break;
        case kF_tts:         r = std::snprintf(out, n, use, s.ttsMin); break;
        case kF_ceiling_m:   r = std::snprintf(out, n, use, (double)s.ceilingM); break;
        case kF_last_stop_m: r = std::snprintf(out, n, use, s.lastStopM); break;
        case kF_gf_now:      r = std::snprintf(out, n, use, (double)s.gfNow); break;
        case kF_controlling_compartment: r = std::snprintf(out, n, use, s.controllingComp); break;
        case kF_cns_pct:     r = std::snprintf(out, n, use, s.cnsPct); break;
        case kF_otu:         r = std::snprintf(out, n, use, (int)s.otu); break;
        case kF_mod_m:       r = std::snprintf(out, n, use, s.modM); break;
        case kF_hypoxic_mod_m: r = std::snprintf(out, n, use, s.hypoxicModM); break;
        case kF_end_m:       r = std::snprintf(out, n, use, s.endM); break;
        case kF_gas_density: r = std::snprintf(out, n, use, (double)s.gasDensityGL); break;
        case kF_mode:        r = std::snprintf(out, n, use, s.mode); break;
        case kF_gas:         r = std::snprintf(out, n, use, s.gas); break;
        case kF_fo2:         r = std::snprintf(out, n, use, (double)s.fo2); break;
        case kF_fhe:         r = std::snprintf(out, n, use, (double)s.fHe); break;
        case kF_ppn2:        r = std::snprintf(out, n, use, (double)s.ppN2); break;
        case kF_pphe:        r = std::snprintf(out, n, use, (double)s.ppHe); break;
        case kF_setpoint:    r = std::snprintf(out, n, use, (double)s.setpoint); break;
        case kF_pulses_per_min: r = std::snprintf(out, n, use, s.pulsesPerMin); break;
        case kF_injected_l:  r = std::snprintf(out, n, use, (double)s.injectedL); break;
        case kF_solenoid_state: r = std::snprintf(out, n, use, s.solenoidState); break;
        case kF_v_bat:       r = std::snprintf(out, n, use, (double)s.vBat); break;
        case kF_v_3v3:       r = std::snprintf(out, n, use, (double)s.v3v3); break;
        case kF_battery_pct: r = std::snprintf(out, n, use, s.batteryPct); break;
        case kF_temp_c:      r = std::snprintf(out, n, use, (double)s.tempC); break;
        case kF_cal_age_h:   r = std::snprintf(out, n, use, (double)s.calAgeH); break;
        case kF_dive_time:   r = std::snprintf(out, n, use, s.diveTimeSec); break;
        case kF_surface_interval: r = std::snprintf(out, n, use, s.surfaceIntervalMin); break;
        case kF_asc_rate:    r = std::snprintf(out, n, use, (double)s.ascRateMMin); break;
        case kF_best_mix:    r = std::snprintf(out, n, use, s.bestMix); break;
        case kF_dgu:         r = std::snprintf(out, n, use, s.dguMin); break;
        case kF_fault_banner: r = std::snprintf(out, n, use, s.faultBanner); break;
        case kF_sim_flag:    r = std::snprintf(out, n, use, s.simFlag); break;
        default: return false;
    }
    if (r < 0 || (size_t)r >= n) { out[0] = '\0'; return false; }
    return true;
}

// Fraction 0..1 for the `bar` primitive from a field's declared gauge range.
// Returns false if the field is not gaugeable (no range declared).
inline bool fieldGaugeFrac(const LiveState& s, FieldId id, double* frac) {
    const FieldDef* d = fieldById(id);
    if (!d || (d->gaugeMin == 0.0f && d->gaugeMax == 0.0f)) return false;
    double v = 0.0;
    switch (d->kind) {
        case FieldKind::Float: v = (id == kF_ppo2_used) ? s.ppo2Used :
                                 (id == kF_depth_m) ? s.depthM :
                                 (id == kF_amb_bar) ? s.ambBar :
                                 (id == kF_gf_now) ? s.gfNow :
                                 (id == kF_fo2) ? s.fo2 :
                                 (id == kF_fhe) ? s.fHe :
                                 (id == kF_ppn2) ? s.ppN2 :
                                 (id == kF_pphe) ? s.ppHe :
                                 (id == kF_setpoint) ? s.setpoint :
                                 (id == kF_gas_density) ? s.gasDensityGL :
                                 (id == kF_v_bat) ? s.vBat :
                                 (id == kF_v_3v3) ? s.v3v3 : -1.0; break;
        case FieldKind::Int:   v = (id == kF_cns_pct) ? s.cnsPct :
                                (id == kF_battery_pct) ? s.batteryPct : -1.0; break;
        default: return false;
    }
    if (v < 0.0 && d->gaugeMin >= 0.0f) { /* negative reading: clamp to 0 below */ }
    double span = (double)d->gaugeMax - (double)d->gaugeMin;
    if (span <= 0.0) return false;
    *frac = (v - (double)d->gaugeMin) / span;
    if (*frac < 0.0) *frac = 0.0;
    if (*frac > 1.0) *frac = 1.0;
    return true;
}

}  // namespace disp

#endif
