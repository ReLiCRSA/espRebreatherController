// StateJson — the JSON face of the LiveState snapshot (M6, §9/§12).
//
// Why it is built on disp::FieldRegistry instead of a hand-written list: §12 makes the
// registry ONE source of truth for name, type, unit and getter, feeding the templates,
// the WebSocket JSON, GET /api/fields and the dive-log columns. A serializer written by
// hand beside that table would be a second copy of every field, and the next field added
// would appear on the OLED and silently not on the web — the divergence class this project
// keeps hitting (risk 17's "forgotten in a switch"). Iterating kFields means one registry
// row = one JSON key, everywhere.
//
// Numbers are emitted raw (JSON numbers, not display strings): the display fmt in the
// registry is for the OLED ("%.1f"), the web client formats for the web. Strings are
// quoted and escaped.
//
// risk 11: sim_flag is ALWAYS present in this payload. The client must never be able to
// render a SIM frame as live data because a stamp happened to be absent.
//
// Writer discipline (same as ConfigSchema): every append is checked, so a payload that
// does not fit is a hard -1 — never a truncated frame that parses as something smaller.
#pragma once

#include "Fields.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

namespace web {

constexpr int kStateJsonCap = 2048;   // 44 fields x ~35 B + framing; pinned by test
constexpr int kFieldsJsonCap = 3072;  // /api/fields carries name+type+unit+priority+value

namespace detail {

struct Jw {
    char* b;
    int cap, k;
    bool bad;
    void prf(const char* fmt, ...);
    void str(const char* s);
};

inline void Jw::prf(const char* fmt, ...) {
    if (bad) return;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(b + k, (size_t)(cap - k), fmt, ap);
    va_end(ap);
    if (r < 0 || r >= cap - k) bad = true;
    else k += r;
}

// JSON string escaping. Field strings here are firmware-authored (gas names, mode,
// compartment labels), and gas names are already validated (§9's gasNameValid rejects
// markup), so this is defence in depth for a future field, not the trust boundary.
inline void Jw::str(const char* s) {
    prf("\"");
    for (const char* p = s; *p; p++) {
        if (*p == '"' || *p == '\\') prf("\\%c", *p);
        else if ((unsigned char)*p < 0x20) prf("?");   // never emit a raw control char
        else prf("%c", *p);
    }
    prf("\"");
}

// One field's value as JSON text into tmp[]. Returns false for a kind the registry
// declares but this serializer does not handle — an unhandled kind must fail loudly,
// never emit an empty string that the web renders as a plausible blank.
inline bool jsonValue(const disp::LiveState& s, disp::FieldId id, char* tmp, int n) {
    const disp::FieldDef* d = disp::fieldById(id);
    if (!d) return false;
    int r = -1;
    switch (d->kind) {
        case disp::FieldKind::Bool: {
            char b[8];
            if (!disp::renderField(s, id, "%d", b, (int)sizeof b)) return false;
            return snprintf(tmp, (size_t)n, "%s", b[0] == '1' ? "true" : "false") < n;
        }
        case disp::FieldKind::Int:
            if (!disp::renderField(s, id, "%d", tmp, (size_t)n)) return false;
            return true;
        case disp::FieldKind::Float:
            if (!disp::renderField(s, id, "%.4g", tmp, (size_t)n)) return false;
            // A NaN/inf float would render as "nan"/"inf" — not valid JSON. Render 0 and
            // let the sensor-staleness fault codes say what is actually wrong (§17).
            for (char* p = tmp; *p; p++)
                if (*p == 'n' || *p == 'N' || *p == 'i' || *p == 'I') { tmp[0] = '0'; tmp[1] = '\0'; break; }
            return true;
        case disp::FieldKind::Str:
            if (!disp::renderField(s, id, "%s", tmp, (size_t)n)) return false;
            return true;
    }
    (void)r;
    return false;
}

}  // namespace detail

// WS frame / GET /api/state. `tMs` is the device uptime stamp of the snapshot.
inline int liveStateToJson(const disp::LiveState& s, char* buf, int cap, uint32_t tMs) {
    if (!buf || cap < 64) return -1;
    detail::Jw w{ buf, cap, 0, false };
    w.prf("{\"t\":%lu,\"sim\":%d,\"f\":{", (unsigned long)tMs, (int)s.simFlag);
    char v[48];
    bool first = true;
    for (uint8_t i = 0; i < disp::kFieldCount; i++) {
        const disp::FieldDef& d = disp::kFields[i];
        if (!detail::jsonValue(s, d.id, v, (int)sizeof v)) { w.bad = true; break; }
        w.prf("%s\"%s\":", first ? "" : ",", d.name);
        if (d.kind == disp::FieldKind::Str) w.str(v);
        else w.prf("%s", v);
        first = false;
    }
    w.prf("}}");
    return w.bad ? -1 : w.k;
}

// GET /api/fields — the registry itself with a live sample value, so the web display
// editor and the dive-log table can be built from what the firmware actually has (§12).
inline int fieldsToJson(const disp::LiveState& s, char* buf, int cap) {
    if (!buf || cap < 64) return -1;
    detail::Jw w{ buf, cap, 0, false };
    w.prf("{\"t\":%lu,\"sim\":%d,\"fields\":[", 0UL, (int)s.simFlag);
    char v[48];
    for (uint8_t i = 0; i < disp::kFieldCount; i++) {
        const disp::FieldDef& d = disp::kFields[i];
        if (!detail::jsonValue(s, d.id, v, (int)sizeof v)) { w.bad = true; break; }
        w.prf("{\"name\":\"%s\",\"type\":%d,\"unit\":\"%s\",\"priority\":%d,\"value\":",
              d.name, (int)d.kind, d.unit, (int)d.priority);
        if (d.kind == disp::FieldKind::Str) w.str(v);
        else w.prf("%s", v);
        w.prf("}");
        if (i + 1 < disp::kFieldCount) w.prf(",");
    }
    w.prf("]}");
    return w.bad ? -1 : w.k;
}

}  // namespace web
