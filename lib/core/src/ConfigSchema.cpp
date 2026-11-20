// ConfigSchema implementation — validation first (the gate §9 requires to be server-side),
// then the JSON codec. Pinned by test/test_config/.
#include "ConfigSchema.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

namespace cfg {

void addError(ConfigErrors* e, const char* field, const char* msg) {
    if (e->count < kMaxFieldErrors) {
        snprintf(e->items[e->count].field, sizeof(e->items[e->count].field), "%s", field);
        snprintf(e->items[e->count].msg, sizeof(e->items[e->count].msg), "%s", msg);
        e->count++;
    } else {
        e->overflowed = true;   // the report says so; errors are never silently dropped
    }
}

namespace {

bool inRange(ConfigErrors* e, const char* name, float v, float lo, float hi) {
    if (!(v >= lo && v <= hi)) {          // !() also catches NaN
        char m[40];
        snprintf(m, sizeof(m), "must be %.3g..%.3g", (double)lo, (double)hi);
        addError(e, name, m);
        return false;
    }
    return true;
}

bool gasSlotEmpty(const GasMix& g) {
    return g.name[0] == '\0' && g.fo2 == 0.0f && g.fhe == 0.0f;
}

}  // namespace

bool configValidate(const RuntimeConfig& c, ConfigErrors* out) {
    ConfigErrors& e = *out;
    e.count = 0;
    e.overflowed = false;

    // deco (§9: GF_low >= 20 -> 0.20; GF_low <= GF_high)
    inRange(&e, "gfLow", c.gfLow, 0.20f, 0.90f);
    inRange(&e, "gfHigh", c.gfHigh, 0.30f, 1.00f);
    if (c.gfLow > c.gfHigh) addError(&e, "gfLow", "must be <= gfHigh");
    inRange(&e, "icdRatio", c.icdRatio, 0.50f, 1.00f);

    inRange(&e, "rateAscentMPerMin", c.rateAscentMPerMin, 5.0f, 20.0f);
    inRange(&e, "rateDecoMPerMin", c.rateDecoMPerMin, 3.0f, 12.0f);
    inRange(&e, "rateDescentMPerMin", c.rateDescentMPerMin, 6.0f, 30.0f);
    if (c.rateDecoMPerMin > c.rateAscentMPerMin)
        addError(&e, "rateDecoMPerMin", "must be <= ascent rate");
    inRange(&e, "lastStopM", c.lastStopM, 3.0f, 12.0f);   // §9: last stop >= 3 m

    // setpoints (§9: 0.4..1.6). The dual law is only meaningful deep <= shallow.
    inRange(&e, "spDeepBar", c.spDeepBar, 0.40f, 1.60f);
    inRange(&e, "spShallowBar", c.spShallowBar, 0.40f, 1.60f);
    if (c.spDeepBar > c.spShallowBar)
        addError(&e, "spDeepBar", "must be <= shallow setpoint");
    inRange(&e, "spCrossoverM", c.spCrossoverM, 2.0f, 60.0f);

    // gas limits (§9: ppO2_hi <= 1.7; hypoxic floor range from §7 0.16-0.18 minimum)
    inRange(&e, "ppO2MaxBar", c.ppO2MaxBar, 1.40f, 1.70f);
    inRange(&e, "ppO2MinBar", c.ppO2MinBar, 0.16f, 0.50f);
    if (c.ppO2MaxBar <= c.ppO2MinBar)
        addError(&e, "ppO2MaxBar", "must exceed hypoxic floor");
    inRange(&e, "heNarcFactor", c.heNarcFactor, 0.0f, 1.0f);
    inRange(&e, "densityMaxGL", c.densityMaxGL, 4.0f, 8.0f);

    // alarms: the high alarm must not fire on the setpoint itself — an alarm that triggers
    // at the loop's own target makes the diver mute alarms (§13's override-training rule).
    inRange(&e, "ppO2AlarmHighBar", c.ppO2AlarmHighBar, 1.00f, 1.70f);
    if (c.ppO2AlarmHighBar < c.spShallowBar + 0.10f)
        addError(&e, "ppO2AlarmHighBar", "must exceed setpoint +0.10");
    inRange(&e, "ppO2AlarmLowBar", c.ppO2AlarmLowBar, 0.16f, 0.60f);
    if (c.ppO2AlarmHighBar <= c.ppO2AlarmLowBar)
        addError(&e, "ppO2AlarmHighBar", "must exceed low alarm");
    inRange(&e, "cnsCeilingPct", c.cnsCeilingPct, 50.0f, 100.0f);
    inRange(&e, "cnsHalfTimeMin", c.cnsHalfTimeMin, 90.0f, 180.0f);
    inRange(&e, "pco2Bar", c.pco2Bar, 0.040f, 0.080f);

    // depth model
    inRange(&e, "sgWater", c.sgWater, 1.000f, 1.030f);
    inRange(&e, "altitudeHintM", c.altitudeHintM, 0.0f, 3500.0f);

    // helium switch depth: 0 = disabled, else a real depth (§9: He only above a switch depth)
    if (!(c.heSwitchDepthM == 0.0f ||
          (c.heSwitchDepthM >= 10.0f && c.heSwitchDepthM <= 60.0f)))
        addError(&e, "heSwitchDepthM", "0 or 10..60 m");

    // mode / select
    if (c.mode > 1) addError(&e, "mode", "0=CCR 1=OC");
    if (c.selectedGas < 0 || c.selectedGas >= kGasCount) {
        addError(&e, "selectedGas", "0..8");
    } else if (gasSlotEmpty(c.gases[c.selectedGas])) {
        addError(&e, "selectedGas", "slot is not configured");
    }

    // gases: slots 0-2 (diluent / oxygen / escape) are REQUIRED (§9's bailout is not optional);
    // staged slots 3-8 are empty or fully valid. gasMixValid enforces §9's FO2 0.10-1.00,
    // FO2+FHe <= 1.0; gasNameValid keeps names log- and template-safe.
    for (int i = 0; i < kGasCount; i++) {
        const GasMix& g = c.gases[i];
        char slot[24];
        snprintf(slot, sizeof(slot), "gases[%d]", i);
        if (gasSlotEmpty(g)) {
            if (i <= 2) addError(&e, slot, "required (dil/O2/escape)");
            continue;
        }
        if (!gasNameValid(g.name)) addError(&e, slot, "bad name");
        char reason[40];
        if (!gasMixValid(g, reason, sizeof(reason))) addError(&e, slot, reason);
    }

    // solenoid (§8 ranges; values provisional until open items 1-4 are measured)
    inRange(&e, "valvePulseMsMin", c.valvePulseMsMin, 20.0f, 250.0f);
    inRange(&e, "valvePulseMsMax", c.valvePulseMsMax, 20.0f, 250.0f);
    if (c.valvePulseMsMin > c.valvePulseMsMax)
        addError(&e, "valvePulseMsMin", "must be <= max width");
    inRange(&e, "valvePulsesPerMinMax", c.valvePulsesPerMinMax, 10.0f, 120.0f);
    inRange(&e, "valvePulseVolAmbL", c.valvePulseVolAmbL, 0.01f, 1.00f);
    inRange(&e, "vbatInjectMinV", c.vbatInjectMinV, 3.00f, 3.80f);

    // simulator (risk 12: guesses until measured — bounds keep them physiological)
    inRange(&e, "metabolicSurfLpm", c.metabolicSurfLpm, 0.3f, 4.0f);
    inRange(&e, "loopVolumeL", c.loopVolumeL, 3.0f, 20.0f);

    // calibration slopes: 0 = uncalibrated, else the §5/§6 age gate band (>=30 mV/bar blocks
    // arming in M2; 120 upper bounds nonsense writes)
    for (int i = 0; i < 3; i++) {
        float s = c.calSlopeMvPerBar[i];
        if (!(s == 0.0f || (s >= 30.0f && s <= 120.0f))) {
            char nm[24];
            snprintf(nm, sizeof(nm), "calSlope%d", i);
            addError(&e, nm, "0 or 30..120 mV/bar");
        }
    }

    return e.count == 0;
}

// --- JSON parser --------------------------------------------------------------
// Recursive descent over fixed buffers. Every failure mode is a hard abort
// (config is all-or-nothing); test/test_config/ attacks it with malformed input.
namespace {

struct Parser {
    const char* s;
    size_t n, i;
    JsonDoc* d;
    char* why;
    int whyLen;
    bool err;

    void bad(const char* m) {
        if (!err) {
            err = true;
            if (whyLen > 0) snprintf(why, (size_t)whyLen, "%s", m);
        }
    }
    void skipWs() {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    }
    bool eof() const { return i >= n; }
    char peek() const { return i < n ? s[i] : '\0'; }

    bool emit(const char* path, uint8_t kind, double num, const char* str) {
        if (strlen(path) >= kPathCap) { bad("path too long"); return false; }
        for (int k = 0; k < d->count; k++) {           // duplicate key: last wins
            if (strcmp(d->entries[k].path, path) == 0) {
                d->entries[k].kind = kind;
                d->entries[k].num = num;
                if (str) snprintf(d->entries[k].str, kValStrCap, "%s", str);
                else d->entries[k].str[0] = '\0';
                return true;
            }
        }
        if (d->count >= kMaxEntries) { bad("too many entries"); return false; }
        Entry& en = d->entries[d->count++];
        snprintf(en.path, kPathCap, "%s", path);
        en.kind = kind;
        en.num = num;
        if (str) snprintf(en.str, kValStrCap, "%s", str);
        else en.str[0] = '\0';
        return true;
    }

    bool parseString(char* out, int cap) {
        if (peek() != '"') { bad("expected string"); return false; }
        i++;
        int k = 0;
        while (true) {
            if (eof()) { bad("unterminated string"); return false; }
            char c = s[i++];
            if (c == '"') { out[k] = '\0'; return true; }
            if ((unsigned char)c < 0x20) { bad("control char in string"); return false; }
            if (c == '\\') {
                if (eof()) { bad("unterminated escape"); return false; }
                char u = s[i++];
                switch (u) {
                    case '"':  c = '"';  break;
                    case '\\': c = '\\'; break;
                    case '/':  c = '/';  break;
                    case 'b':  c = '\b'; break;
                    case 'f':  c = '\f'; break;
                    case 'n':  c = '\n'; break;
                    case 'r':  c = '\r'; break;
                    case 't':  c = '\t'; break;
                    default: bad("bad escape ('\\u' is not accepted)"); return false;
                }
            }
            if (k + 1 >= cap) { bad("string too long"); return false; }
            out[k++] = c;
        }
    }

    bool parseValue(const char* path, int depth) {
        if (depth > 3) { bad("nesting too deep"); return false; }   // gases[i].field = depth 3
        skipWs();
        char c = peek();
        if (c == '{') return parseObject(path, depth);
        if (c == '[') return parseArray(path, depth);
        if (c == '"') {
            char tmp[kValStrCap];
            if (!parseString(tmp, (int)sizeof tmp)) return false;
            return emit(path, 2, 0.0f, tmp);
        }
        if (c == 't') {
            if (i + 4 <= n && memcmp(s + i, "true", 4) == 0) { i += 4; return emit(path, 3, 1.0f, nullptr); }
            bad("bad literal"); return false;
        }
        if (c == 'f') {
            if (i + 5 <= n && memcmp(s + i, "false", 5) == 0) { i += 5; return emit(path, 3, 0.0f, nullptr); }
            bad("bad literal"); return false;
        }
        if (c == 'n') {
            if (i + 4 <= n && memcmp(s + i, "null", 4) == 0) { i += 4; return emit(path, 4, 0.0f, nullptr); }
            bad("bad literal"); return false;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = i;
            // Restricted charset: blocks strtod's hex-float and inf/nan spellings outright.
            while (i < n && (strchr("0123456789.eE+-", s[i]) != nullptr)) i++;
            char tmp[40];
            size_t len = i - start;
            if (len == 0 || len >= sizeof tmp) { bad("bad number"); return false; }
            memcpy(tmp, s + start, len);
            tmp[len] = '\0';
            char* end = nullptr;
            double v = strtod(tmp, &end);
            if (end == nullptr || *end != '\0' || !isfinite(v)) { bad("bad number"); return false; }
            return emit(path, 1, v, nullptr);
        }
        bad("unexpected character in value");
        return false;
    }

    bool childPath(char* out, int cap, const char* path, const char* suffix) {
        int r = snprintf(out, (size_t)cap, "%s%s", path, suffix);
        if (r < 0 || r >= cap) { bad("path too long"); return false; }
        return true;
    }

    bool parseObject(const char* path, int depth) {
        i++;  // '{'
        skipWs();
        if (peek() == '}') { i++; return true; }     // empty object contributes no entries
        while (true) {
            skipWs();
            char key[48];
            if (!parseString(key, (int)sizeof key)) return false;
            if (key[0] == '\0') { bad("empty key"); return false; }
            skipWs();
            if (peek() != ':') { bad("expected ':' after key"); return false; }
            i++;
            char suffix[64];
            snprintf(suffix, sizeof suffix, "%s%s", path[0] ? "." : "", key);
            char child[kPathCap];
            if (!childPath(child, (int)sizeof child, path, suffix)) return false;
            if (!parseValue(child, depth + 1)) return false;
            skipWs();
            char c = peek();
            if (c == ',') { i++; continue; }
            if (c == '}') { i++; return true; }
            bad("expected ',' or '}' in object");
            return false;
        }
    }

    bool parseArray(const char* path, int depth) {
        i++;  // '['
        skipWs();
        if (peek() == ']') { i++; return true; }
        int idx = 0;
        while (true) {
            char suffix[16];
            snprintf(suffix, sizeof suffix, "[%d]", idx++);
            char child[kPathCap];
            if (!childPath(child, (int)sizeof child, path, suffix)) return false;
            if (!parseValue(child, depth + 1)) return false;
            skipWs();
            char c = peek();
            if (c == ',') { i++; continue; }
            if (c == ']') { i++; return true; }
            bad("expected ',' or ']' in array");
            return false;
        }
    }
};

}  // namespace

const Entry* docFind(const JsonDoc& d, const char* path) {
    for (int k = 0; k < d.count; k++)
        if (strcmp(d.entries[k].path, path) == 0) return &d.entries[k];
    return nullptr;
}

bool configParseJson(const char* json, int len, JsonDoc* doc, char* why, int whyLen) {
    doc->count = 0;
    if (whyLen > 0) why[0] = '\0';
    Parser p{ json, (size_t)len, 0, doc, why, whyLen, false };
    p.skipWs();
    if (p.peek() != '{') { p.bad("config must be a JSON object"); return false; }
    if (!p.parseValue("", 0)) return false;
    p.skipWs();
    if (!p.eof()) { p.bad("trailing text after JSON"); return false; }
    return !p.err;
}

// --- document -> RuntimeConfig (the migration model: absent field = default) ---------
namespace {

void whyAt(char* why, int whyLen, const char* path, const char* m) {
    if (whyLen > 0 && why[0] == '\0') snprintf(why, (size_t)whyLen, "%s: %s", path, m);
}

bool wantNum(const JsonDoc& d, const char* path, float* dst, char* why, int whyLen) {
    const Entry* e = docFind(d, path);
    if (!e || e->kind == 4) return true;                       // absent / null -> default
    if (e->kind != 1 && e->kind != 3) { whyAt(why, whyLen, path, "must be a number"); return false; }
    *dst = (float)e->num;
    return true;
}

bool wantBool(const JsonDoc& d, const char* path, bool* dst, char* why, int whyLen) {
    const Entry* e = docFind(d, path);
    if (!e || e->kind == 4) return true;
    if (e->kind != 1 && e->kind != 3) { whyAt(why, whyLen, path, "must be true/false"); return false; }
    *dst = e->num != 0.0;
    return true;
}

bool wantInt(const JsonDoc& d, const char* path, int* dst, int lo, int hi, char* why, int whyLen) {
    const Entry* e = docFind(d, path);
    if (!e || e->kind == 4) return true;
    if (e->kind != 1 && e->kind != 3) { whyAt(why, whyLen, path, "must be a whole number"); return false; }
    double v = e->num;
    if (v != floor(v) || v < (double)lo || v > (double)hi) { whyAt(why, whyLen, path, "out of range"); return false; }
    *dst = (int)v;
    return true;
}

bool wantU32(const JsonDoc& d, const char* path, uint32_t* dst, char* why, int whyLen) {
    const Entry* e = docFind(d, path);
    if (!e || e->kind == 4) return true;
    if (e->kind != 1) { whyAt(why, whyLen, path, "must be a whole number"); return false; }
    double v = e->num;
    if (v < 0.0 || v > 4294967295.0 || v != floor(v)) { whyAt(why, whyLen, path, "out of range"); return false; }
    *dst = (uint32_t)v;
    return true;
}

bool wantStr(const JsonDoc& d, const char* path, char* dst, int cap, char* why, int whyLen) {
    const Entry* e = docFind(d, path);
    if (!e || e->kind == 4) return true;
    if (e->kind != 2) { whyAt(why, whyLen, path, "must be a string"); return false; }
    if ((int)strlen(e->str) + 1 > cap) { whyAt(why, whyLen, path, "too long"); return false; }
    snprintf(dst, (size_t)cap, "%s", e->str);
    return true;
}

}  // namespace

bool configFromDoc(const JsonDoc& d, RuntimeConfig* out, char* why, int whyLen) {
    *out = RuntimeConfig{};                       // defaults first: absent fields stay defaults
    if (whyLen > 0) why[0] = '\0';

    // schema / migration (§17): an older document simply lacks fields — those keep defaults.
    // A NEWER schema is refused: this build must not reinterpret a format it does not know.
    float schema = (float)kSchemaVersion;
    if (!wantNum(d, "schema", &schema, why, whyLen)) return false;
    if (!(schema >= 0.0f)) { whyAt(why, whyLen, "schema", "must be >= 0"); return false; }
    if (schema > (float)kSchemaVersion) { whyAt(why, whyLen, "schema", "newer than this build"); return false; }

    RuntimeConfig& c = *out;
    bool ok = true;
    int mode = (int)c.mode;
    ok = ok && wantInt (d, "mode", &mode, 0, 1, why, whyLen);
    if (ok) c.mode = (uint8_t)mode;
    ok = ok && wantInt (d, "selectedGas", &c.selectedGas, 0, kGasCount - 1, why, whyLen);
    ok = ok && wantNum (d, "gfLow", &c.gfLow, why, whyLen);
    ok = ok && wantNum (d, "gfHigh", &c.gfHigh, why, whyLen);
    ok = ok && wantBool(d, "icdGuard", &c.icdGuard, why, whyLen);
    ok = ok && wantNum (d, "icdRatio", &c.icdRatio, why, whyLen);
    ok = ok && wantNum (d, "rateAscentMPerMin", &c.rateAscentMPerMin, why, whyLen);
    ok = ok && wantNum (d, "rateDecoMPerMin", &c.rateDecoMPerMin, why, whyLen);
    ok = ok && wantNum (d, "rateDescentMPerMin", &c.rateDescentMPerMin, why, whyLen);
    ok = ok && wantNum (d, "lastStopM", &c.lastStopM, why, whyLen);
    ok = ok && wantNum (d, "spDeepBar", &c.spDeepBar, why, whyLen);
    ok = ok && wantNum (d, "spShallowBar", &c.spShallowBar, why, whyLen);
    ok = ok && wantNum (d, "spCrossoverM", &c.spCrossoverM, why, whyLen);
    ok = ok && wantNum (d, "ppO2MaxBar", &c.ppO2MaxBar, why, whyLen);
    ok = ok && wantNum (d, "ppO2MinBar", &c.ppO2MinBar, why, whyLen);
    ok = ok && wantNum (d, "heNarcFactor", &c.heNarcFactor, why, whyLen);
    ok = ok && wantNum (d, "densityMaxGL", &c.densityMaxGL, why, whyLen);
    ok = ok && wantNum (d, "ppO2AlarmHighBar", &c.ppO2AlarmHighBar, why, whyLen);
    ok = ok && wantNum (d, "ppO2AlarmLowBar", &c.ppO2AlarmLowBar, why, whyLen);
    ok = ok && wantNum (d, "cnsCeilingPct", &c.cnsCeilingPct, why, whyLen);
    ok = ok && wantNum (d, "cnsHalfTimeMin", &c.cnsHalfTimeMin, why, whyLen);
    ok = ok && wantNum (d, "pco2Bar", &c.pco2Bar, why, whyLen);
    ok = ok && wantNum (d, "sgWater", &c.sgWater, why, whyLen);
    ok = ok && wantNum (d, "altitudeHintM", &c.altitudeHintM, why, whyLen);
    ok = ok && wantNum (d, "heSwitchDepthM", &c.heSwitchDepthM, why, whyLen);
    ok = ok && wantBool(d, "solenoidEnable", &c.solenoidEnable, why, whyLen);
    ok = ok && wantNum (d, "valvePulseMsMin", &c.valvePulseMsMin, why, whyLen);
    ok = ok && wantNum (d, "valvePulseMsMax", &c.valvePulseMsMax, why, whyLen);
    ok = ok && wantNum (d, "valvePulsesPerMinMax", &c.valvePulsesPerMinMax, why, whyLen);
    ok = ok && wantNum (d, "valvePulseVolAmbL", &c.valvePulseVolAmbL, why, whyLen);
    ok = ok && wantNum (d, "vbatInjectMinV", &c.vbatInjectMinV, why, whyLen);
    ok = ok && wantNum (d, "metabolicSurfLpm", &c.metabolicSurfLpm, why, whyLen);
    ok = ok && wantNum (d, "loopVolumeL", &c.loopVolumeL, why, whyLen);
    ok = ok && wantBool(d, "wifiDuringDive", &c.wifiDuringDive, why, whyLen);
    for (int i = 0; i < 3 && ok; i++) {
        char p1[24];
        snprintf(p1, sizeof p1, "calSlope%d", i);
        ok = wantNum(d, p1, &c.calSlopeMvPerBar[i], why, whyLen);
        char p2[24];
        snprintf(p2, sizeof p2, "calHighMv%d", i);
        ok = ok && wantNum(d, p2, &c.calHighMv[i], why, whyLen);
    }
    ok = ok && wantU32(d, "calWallS", &c.calWallS, why, whyLen);
    ok = ok && wantU32(d, "lastWallS", &c.lastWallS, why, whyLen);
    ok = ok && wantU32(d, "lastWallUptimeS", &c.lastWallUptimeS, why, whyLen);

    for (int i = 0; i < kGasCount && ok; i++) {
        GasMix& g = c.gases[i];
        char p[24];
        snprintf(p, sizeof p, "gases[%d].name", i);
        ok = wantStr(d, p, g.name, (int)sizeof g.name, why, whyLen);
        snprintf(p, sizeof p, "gases[%d].fo2", i);
        ok = ok && wantNum(d, p, &g.fo2, why, whyLen);
        snprintf(p, sizeof p, "gases[%d].fhe", i);
        ok = ok && wantNum(d, p, &g.fhe, why, whyLen);
    }
    return ok;
}

bool configFromJson(const char* json, int len, RuntimeConfig* out,
                    JsonDoc* scratch, char* why, int whyLen) {
    if (!configParseJson(json, len, scratch, why, whyLen)) return false;
    return configFromDoc(*scratch, out, why, whyLen);
}

// --- RuntimeConfig -> JSON text -----------------------------------------------------
namespace {

struct W {
    char* b;
    int cap, k;
    bool bad;
    void prf(const char* fmt, ...);
};

void W::prf(const char* fmt, ...) {
    if (bad) return;
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(b + k, (size_t)(cap - k), fmt, ap);
    va_end(ap);
    if (r < 0 || r >= cap - k) bad = true;
    else k += r;
}

void jsonStr(W& w, const char* s) {         // escape '"' and '\\' defensively (names forbid them)
    w.prf("\"");
    for (const char* p = s; *p; p++) {
        if (*p == '"' || *p == '\\') w.prf("\\%c", *p);
        else w.prf("%c", *p);
    }
    w.prf("\"");
}

}  // namespace

int configToJson(const RuntimeConfig& c, char* buf, int cap) {
    W w{ buf, cap, 0, false };
    w.prf("{\"schema\":%d", kSchemaVersion);
    w.prf(",\"mode\":%d", (int)c.mode);
    w.prf(",\"selectedGas\":%d", c.selectedGas);
    w.prf(",\"gfLow\":%.6g", (double)c.gfLow);
    w.prf(",\"gfHigh\":%.6g", (double)c.gfHigh);
    w.prf(",\"icdGuard\":%s", c.icdGuard ? "true" : "false");
    w.prf(",\"icdRatio\":%.6g", (double)c.icdRatio);
    w.prf(",\"rateAscentMPerMin\":%.6g", (double)c.rateAscentMPerMin);
    w.prf(",\"rateDecoMPerMin\":%.6g", (double)c.rateDecoMPerMin);
    w.prf(",\"rateDescentMPerMin\":%.6g", (double)c.rateDescentMPerMin);
    w.prf(",\"lastStopM\":%.6g", (double)c.lastStopM);
    w.prf(",\"spDeepBar\":%.6g", (double)c.spDeepBar);
    w.prf(",\"spShallowBar\":%.6g", (double)c.spShallowBar);
    w.prf(",\"spCrossoverM\":%.6g", (double)c.spCrossoverM);
    w.prf(",\"ppO2MaxBar\":%.6g", (double)c.ppO2MaxBar);
    w.prf(",\"ppO2MinBar\":%.6g", (double)c.ppO2MinBar);
    w.prf(",\"heNarcFactor\":%.6g", (double)c.heNarcFactor);
    w.prf(",\"densityMaxGL\":%.6g", (double)c.densityMaxGL);
    w.prf(",\"ppO2AlarmHighBar\":%.6g", (double)c.ppO2AlarmHighBar);
    w.prf(",\"ppO2AlarmLowBar\":%.6g", (double)c.ppO2AlarmLowBar);
    w.prf(",\"cnsCeilingPct\":%.6g", (double)c.cnsCeilingPct);
    w.prf(",\"cnsHalfTimeMin\":%.6g", (double)c.cnsHalfTimeMin);
    w.prf(",\"pco2Bar\":%.6g", (double)c.pco2Bar);
    w.prf(",\"sgWater\":%.6g", (double)c.sgWater);
    w.prf(",\"altitudeHintM\":%.6g", (double)c.altitudeHintM);
    w.prf(",\"heSwitchDepthM\":%.6g", (double)c.heSwitchDepthM);
    w.prf(",\"solenoidEnable\":%s", c.solenoidEnable ? "true" : "false");
    w.prf(",\"valvePulseMsMin\":%.6g", (double)c.valvePulseMsMin);
    w.prf(",\"valvePulseMsMax\":%.6g", (double)c.valvePulseMsMax);
    w.prf(",\"valvePulsesPerMinMax\":%.6g", (double)c.valvePulsesPerMinMax);
    w.prf(",\"valvePulseVolAmbL\":%.6g", (double)c.valvePulseVolAmbL);
    w.prf(",\"vbatInjectMinV\":%.6g", (double)c.vbatInjectMinV);
    w.prf(",\"metabolicSurfLpm\":%.6g", (double)c.metabolicSurfLpm);
    w.prf(",\"loopVolumeL\":%.6g", (double)c.loopVolumeL);
    w.prf(",\"wifiDuringDive\":%s", c.wifiDuringDive ? "true" : "false");
    for (int i = 0; i < 3; i++) {
        w.prf(",\"calSlope%d\":%.6g", i, (double)c.calSlopeMvPerBar[i]);
        w.prf(",\"calHighMv%d\":%.6g", i, (double)c.calHighMv[i]);
    }
    w.prf(",\"calWallS\":%lu", (unsigned long)c.calWallS);
    w.prf(",\"lastWallS\":%lu", (unsigned long)c.lastWallS);
    w.prf(",\"lastWallUptimeS\":%lu", (unsigned long)c.lastWallUptimeS);
    w.prf(",\"gases\":[");
    for (int i = 0; i < kGasCount; i++) {
        if (i) w.prf(",");
        w.prf("{\"name\":");
        jsonStr(w, c.gases[i].name);
        w.prf(",\"fo2\":%.6g,\"fhe\":%.6g}", (double)c.gases[i].fo2, (double)c.gases[i].fhe);
    }
    w.prf("]}");
    return w.bad ? -1 : w.k;
}

}  // namespace cfg
