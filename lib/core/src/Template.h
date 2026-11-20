#ifndef TEMPLATE_H
#define TEMPLATE_H

/*
 * Template.h — the §12 templated display layout: parse, VALIDATE, lay out (fit report),
 * render. Pure C++, fully host-testable; the panel driver only ever receives a Framebuf.
 *
 * Grammar (line-oriented text; printf-style format specs):
 *
 *   @size 128x64          required, exactly once, before any content line
 *   @font 8               only 8 supported today (Font6x8)
 *   @page N               starts a new page (1..4); content attaches to current page
 *   @if <field> <text>    line shown only when <field> is truthy (numeric != 0,
 *                         non-empty string) — matches §12's `@if sim  SIM {{sim_flag}}`
 *   big|row|text <text>   text line; {{name}} or {{name:fmt}} substitutions
 *   bar {{name}}          gauge across the line from the field's declared range
 *   blink <text>          parses and flags; on-panel blink timing is M8+ (renders as
 *                         plain text meanwhile — stated, not hidden)
 *
 * Validator rules (rejection, not advisory — §12 "safety invariants, validator-enforced"):
 *   - every registered SAFETY field must appear on EVERY page, unconditionally
 *   - a SAFETY field inside @if is rejected (a condition must never hide ppO2/depth/TTS)
 *   - unknown field / primitive / directive, bad format (fmtSanitize, Fields.h), bad
 *     @size/@font, capacity exhaustion: rejected with a message + line number.
 *
 * Fit (§12): a page with more lines than panelH/8 drops whole lines in priority order —
 * DETAIL first, then SECONDARY, then PRIMARY, later lines first within a class; SAFETY
 * lines are never dropped. The FitReport names every dropped field:
 * "template needs 9 lines, panel has 8 - dropped: fhe, end_m, gas_density".
 *
 * Capacities are fixed (no allocation in the render path, §17). A template over
 * capacity is REJECTED with its own error, never silently truncated.
 */

#include <cstdint>
#include <cstring>
#include "Fields.h"
#include "Framebuf.h"

namespace disp {
namespace tmpl {

constexpr uint8_t  kMaxPages = 4;
constexpr uint8_t  kMaxLines = 12;     // parse capacity > 64px/8 so a 9-on-8 page is a
                                       // FIT report case, not a parse rejection (§12 example)
constexpr uint8_t  kMaxSegs  = 10;     // 4 space-separated fields = 7 segs (F L F L F L F)
constexpr uint8_t  kLitCap   = 24;
constexpr uint8_t  kFmtCap   = 12;
constexpr uint8_t  kNameCap  = 24;
constexpr uint16_t kBarWidthPx = 40;   // §12 `bar` gauge width

enum Prim : uint8_t { kText, kBig, kRow, kBar, kBlink };

struct Seg {
    bool isField;
    char lit[kLitCap];
    FieldId field;
    char fmt[kFmtCap];
};

struct Line {
    Prim prim;
    bool conditional;
    FieldId condField;
    uint8_t segCount;
    Seg seg[kMaxSegs];
};

struct Page { Line line[kMaxLines]; uint8_t lineCount; };

struct Template {
    uint16_t w, h;         // from @size
    uint8_t font;          // from @font (8)
    uint8_t pageCount;     // pages actually used
    Page page[kMaxPages];
};

enum ErrCode : uint8_t {
    kOk = 0, kEmpty, kNoSize, kBadDirective, kBadSize, kBadFont, kBadPage,
    kUnknownPrim, kUnknownField, kBadFormat, kBadRef, kNoFields,
    kTooManyPages, kTooManyLines, kTooManySegs, kLitTooLong, kLineTooLong
};

struct ParseResult {
    bool ok;
    ErrCode err;
    uint16_t lineNo;        // 1-based, for web editor feedback
    char msg[96];
};

// Parse template text into `t`. On failure `res` carries code + line + message and `t`
// is untouched. Pure function of the input text; no panel needed yet.
inline bool parse(const char* text, Template& t, ParseResult& res) {
    // memset, NOT `t = Template{}`: that form materialises an ~18 KB temporary, which can
    // land on the CALLER's stack. These structs are KB-scale (see the footprint test in
    // test/test_display/) — the same 8 KB loopTask stack that a Template local already
    // overflowed (2026-11-20 post-M7 boot-loop, TG1WDT_SYS_RESET). The SimRunner rule
    // generalised: KB-scale structs are static or boot-time, never task-stack locals.
    std::memset(&t, 0, sizeof t);
    res = ParseResult{true, kOk, 0, {}};
    bool sizeSeen = false;
    uint8_t curPage = 0;
    t.pageCount = 1;

    const char* p = text;
    while (*p) {
        res.lineNo++;
        // skip one line
        const char* eol = p;
        while (*eol && *eol != '\n') eol++;
        size_t len = (size_t)(eol - p);
        if (len > 200) { res = {false, kLineTooLong, res.lineNo, "line too long"}; return false; }
        char ln[208];
        std::memcpy(ln, p, len); ln[len] = '\0';
        p = (*eol) ? eol + 1 : eol;

        // trim leading spaces
        char* s = ln;
        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        if (*s == '\0') continue;  // blank line

        // conditional content line: "@if <field> <rest-as-text>" — must be checked BEFORE
        // the directive branch, or "@if" reads as an unknown directive.
        if (std::strncmp(s, "@if", 3) == 0 && (s[3] == ' ' || s[3] == '\t')) {
            // handled in the content path below
        } else if (*s == '@') {
            // directive line: one or more space-separated "@name value" pairs
            // (§12's example puts @size, @font and @page on one line)
            char* q = s;
            while (true) {
                while (*q == ' ' || *q == '\t' || *q == '\r') q++;
                if (*q == '\0') break;
                if (*q != '@') { res = {false, kBadDirective, res.lineNo, "text after directives"}; return false; }
                char tok[8]; size_t tn = 0;
                while (*q && *q != ' ' && *q != '\t' && *q != '\r' && tn < sizeof(tok) - 1) tok[tn++] = *q++;
                tok[tn] = '\0';
                while (*q == ' ') q++;
                const char* v = q;               // value token starts here
                if (std::strcmp(tok, "@size") == 0) {
                    if (sizeSeen) { res = {false, kBadDirective, res.lineNo, "duplicate @size"}; return false; }
                    uint16_t w = 0, h = 0;
                    while (*v >= '0' && *v <= '9') { w = (uint16_t)(w * 10 + (*v - '0')); v++; }
                    if (*v != 'x') { res = {false, kBadSize, res.lineNo, "@size wants WxH"}; return false; }
                    v++;
                    while (*v >= '0' && *v <= '9') { h = (uint16_t)(h * 10 + (*v - '0')); v++; }
                    if (*v && *v != ' ' && *v != '\t' && *v != '\r') {
                        res = {false, kBadSize, res.lineNo, "@size wants WxH"}; return false;
                    }
                    if (w == 0 || h == 0 || (h % 8) != 0 || w > 256 || h > 64) {
                        res = {false, kBadSize, res.lineNo, "@size wants WxH (h x8, <=256x64)"}; return false;
                    }
                    t.w = w; t.h = h; sizeSeen = true;
                } else if (std::strcmp(tok, "@font") == 0) {
                    int fv = 0;
                    while (*v >= '0' && *v <= '9') { fv = fv * 10 + (*v - '0'); v++; }
                    if (*v && *v != ' ' && *v != '\t' && *v != '\r') {
                        res = {false, kBadFont, res.lineNo, "bad @font value"}; return false;
                    }
                    if (fv != 8) { res = {false, kBadFont, res.lineNo, "only @font 8 is implemented"}; return false; }
                    t.font = 8;
                } else if (std::strcmp(tok, "@page") == 0) {
                    int n = 0;
                    while (*v >= '0' && *v <= '9') { n = n * 10 + (*v - '0'); v++; }
                    if (*v && *v != ' ' && *v != '\t' && *v != '\r') {
                        res = {false, kBadPage, res.lineNo, "bad @page value"}; return false;
                    }
                    if (n < 1 || n > kMaxPages) { res = {false, kBadPage, res.lineNo, "@page N, 1..4"}; return false; }
                    curPage = (uint8_t)(n - 1);
                    if ((uint8_t)n > t.pageCount) t.pageCount = (uint8_t)n;
                } else {
                    res = {false, kBadDirective, res.lineNo, "unknown directive"}; return false;
                }
                q = (char*)v;
            }
            continue;
        }

        if (!sizeSeen) { res = {false, kNoSize, res.lineNo, "@size must precede content"}; return false; }

        // conditional line: "@if <field> <rest-as-text>"
        Line line{};
        line.prim = kText;
        line.conditional = false;
        line.condField = kFieldCount;
        if (std::strncmp(s, "@if", 3) == 0 && (s[3] == ' ' || s[3] == '\t')) {
            line.conditional = true;
            s += 3;
            while (*s == ' ') s++;
            char nm[kNameCap];
            size_t n = 0;
            while (*s && *s != ' ' && *s != '\t' && n < kNameCap - 1) nm[n++] = *s++;
            nm[n] = '\0';
            while (*s == ' ') s++;
            FieldId f = fieldByName(nm);
            if (f == kFieldCount) {
                res = {false, kUnknownField, res.lineNo, "unknown @if field"}; return false;
            }
            line.condField = f;
        } else {
            // primitive token
            char tok[8]; size_t n = 0;
            char* q = s;
            while (*q && *q != ' ' && *q != '\t' && n < sizeof(tok) - 1) tok[n++] = *q++;
            tok[n] = '\0';
            while (*q == ' ' || *q == '\t') q++;
            if (std::strcmp(tok, "big") == 0)        line.prim = kBig;
            else if (std::strcmp(tok, "row") == 0)   line.prim = kRow;
            else if (std::strcmp(tok, "text") == 0)  line.prim = kText;
            else if (std::strcmp(tok, "bar") == 0)   line.prim = kBar;
            else if (std::strcmp(tok, "blink") == 0) line.prim = kBlink;
            else { res = {false, kUnknownPrim, res.lineNo, "unknown primitive"}; return false; }
            s = q;
        }

        // segment scan: literals + {{name}} / {{name:fmt}}
        bool anyField = false;
        const char* segStart = s;
        for (const char* q = s;; q++) {
            bool atRef = (q[0] == '{' && q[1] == '{');
            bool atEnd = (*q == '\0');
            if (!atRef && !atEnd) continue;
            // flush pending literal
            if (q > segStart) {
                size_t n = (size_t)(q - segStart);
                // only strip CR (CRLF files); interior and inter-field spaces are content —
                // trimming them would glue "SP {{x}} GF" into "SPxGF"
                while (n > 0 && segStart[n-1] == '\r') n--;
                if (n > 0) {
                    if (line.segCount >= kMaxSegs) { res = {false, kTooManySegs, res.lineNo, "too many segments"}; return false; }
                    if (n >= kLitCap) { res = {false, kLitTooLong, res.lineNo, "literal too long"}; return false; }
                    Seg& sg = line.seg[line.segCount++];
                    sg = Seg{};
                    sg.isField = false;
                    std::memcpy(sg.lit, segStart, n); sg.lit[n] = '\0';
                }
            }
            if (atEnd) break;
            // consume {{ ... }}
            const char* r = q + 2;
            const char* end = r;
            while (!(end[0] == '}' && end[1] == '}') && *end) end++;
            if (!(end[0] == '}' && end[1] == '}')) { res = {false, kBadRef, res.lineNo, "unterminated {{"}; return false; }
            size_t inner = (size_t)(end - r);
            if (inner == 0 || inner > kNameCap + kFmtCap) { res = {false, kBadRef, res.lineNo, "bad {{ref}}"}; return false; }
            char nm[kNameCap]; char fm[kFmtCap];
            const char* colon = nullptr;
            for (size_t j = 0; j < inner; j++) if (r[j] == ':' && !colon) { colon = r + j; break; }
            size_t nlen = colon ? (size_t)(colon - r) : inner;
            if (nlen >= kNameCap) { res = {false, kBadRef, res.lineNo, "field name too long"}; return false; }
            std::memcpy(nm, r, nlen); nm[nlen] = '\0';
            fm[0] = '\0';
            if (colon) {
                size_t flen = inner - nlen - 1;
                if (flen >= kFmtCap) { res = {false, kBadRef, res.lineNo, "format too long"}; return false; }
                std::memcpy(fm, colon + 1, flen); fm[flen] = '\0';
            }
            FieldId fid = fieldByName(nm);
            if (fid == kFieldCount) { res = {false, kUnknownField, res.lineNo, "unknown field"}; return false; }
            if (fm[0] && !fmtSanitize(fm, fieldById(fid)->kind)) {
                res = {false, kBadFormat, res.lineNo, "format rejected for field kind"}; return false;
            }
            if (line.segCount >= kMaxSegs) { res = {false, kTooManySegs, res.lineNo, "too many segments"}; return false; }
            Seg& sg = line.seg[line.segCount++];
            sg = Seg{};
            sg.isField = true; sg.field = fid;
            std::strncpy(sg.fmt, fm, kFmtCap - 1);
            anyField = true;
            q = end + 1;          // loop's q++ lands on end+2, the char after "}}"
            segStart = q + 1;     // and the next literal starts there too — the inter-field
                                  // space belongs to the NEXT literal ("NDL " then " TTS "),
                                  // so rows end at most 15 segs wide: kMaxSegs covers it
        }

        if (line.prim == kBar) {
            if (!anyField) { res = {false, kNoFields, res.lineNo, "bar needs a field"}; return false; }
        }

        Page& pg = t.page[curPage];
        if (pg.lineCount >= kMaxLines) { res = {false, kTooManyLines, res.lineNo, "page over capacity"}; return false; }
        pg.line[pg.lineCount++] = line;
    }

    if (!sizeSeen) { res = {false, kNoSize, 0, "template has no @size"}; return false; }
    return true;
}

// ---------------------------------------------------------------- validation

struct ValidationResult {
    bool ok;
    ErrCode err;
    char msg[112];
};

// Line priority = lowest priority among its fields (a DETAIL line goes first).
inline FieldPriority linePriority(const Line& ln) {
    FieldPriority best = FieldPriority::Primary;  // no fields: ordinary PRIMARY weight
    bool saw = false;
    for (uint8_t i = 0; i < ln.segCount; i++) {
        if (!ln.seg[i].isField) continue;
        const FieldDef* d = fieldById(ln.seg[i].field);
        if (!d) continue;
        if (!saw || (uint8_t)d->priority > (uint8_t)best) best = d->priority;
        saw = true;
    }
    return best;
}

// §12 safety invariants, enforced per page. Validation runs on EVERY page — M7's gate.
inline bool validate(const Template& t, ValidationResult& vr) {
    vr = ValidationResult{true, kOk, {}};
    for (uint8_t pg = 0; pg < t.pageCount; pg++) {
        const Page& p = t.page[pg];
        bool safetySeen[kFieldCount] = {};
        for (uint8_t li = 0; li < p.lineCount; li++) {
            const Line& ln = p.line[li];
            for (uint8_t si = 0; si < ln.segCount; si++) {
                if (!ln.seg[si].isField) continue;
                FieldId f = ln.seg[si].field;
                if (fieldIsSafety(f)) {
                    if (ln.conditional) {
                        vr = {false, kBadRef, {}};
                        std::snprintf(vr.msg, sizeof(vr.msg),
                            "SAFETY field %s may not be inside @if (page %u)",
                            fieldById(f)->name, pg + 1);
                        return false;
                    }
                    safetySeen[f] = true;
                }
            }
        }
        for (uint8_t fi = 0; fi < kFieldCount; fi++) {
            if (kFields[fi].priority != FieldPriority::Safety) continue;
            if (!safetySeen[kFields[fi].id]) {
                vr = {false, kNoFields, {}};
                std::snprintf(vr.msg, sizeof(vr.msg),
                    "page %u omits required SAFETY field: %s", pg + 1, kFields[fi].name);
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- fit / layout

struct FitReport {
    bool fits;
    uint16_t need;          // lines the page wants
    uint16_t cap;           // lines the panel holds
    uint8_t droppedCount;
    char dropped[8][kNameCap];   // field names lost
    char message[128];      // §12 fit-report sentence
};

struct Layout {
    bool keep[kMaxLines];   // which page lines survive the fit
    uint8_t keptCount;
    FitReport fit;
};

// Lay one page out for a panel of panelW x panelH. Width mismatch cannot be fixed by
// dropping lines, so it fails; height overflow drops lines by priority (§12).
inline void layoutPage(const Template& t, uint8_t pageIdx, uint16_t panelW,
                       uint16_t panelH, Layout& out) {
    out = Layout{};
    FitReport& fr = out.fit;
    const Page& p = t.page[pageIdx];
    fr.need = p.lineCount;
    fr.cap = panelH / 8;

    if (t.w != panelW) {
        fr.fits = false;
        std::snprintf(fr.message, sizeof(fr.message),
            "template @size %ux%u does not fit panel %ux%u - width cannot be dropped",
            t.w, t.h, panelW, panelH);
        return;
    }

    bool drop[kMaxLines] = {};
    for (uint8_t i = 0; i < p.lineCount; i++) out.keep[i] = true;
    int kept = p.lineCount;

    while (kept > (int)fr.cap) {
        // lowest-priority droppable line; later lines first within a class
        int pick = -1;
        FieldPriority bestPri = FieldPriority::Safety;
        for (int i = p.lineCount - 1; i >= 0; i--) {
            if (drop[i]) continue;
            FieldPriority pri = linePriority(p.line[i]);
            if (pri == FieldPriority::Safety) continue;   // never dropped
            if (pick < 0 || (uint8_t)pri > (uint8_t)bestPri) { bestPri = pri; pick = i; }
        }
        if (pick < 0) break;                               // only SAFETY left
        drop[pick] = true;
        kept--;
        // name the dropped fields
        for (uint8_t si = 0; si < p.line[pick].segCount; si++) {
            if (!p.line[pick].seg[si].isField) continue;
            if (fr.droppedCount < 8) {
                std::strncpy(fr.dropped[fr.droppedCount++],
                             fieldById(p.line[pick].seg[si].field)->name, kNameCap - 1);
            }
        }
    }

    for (uint8_t i = 0; i < p.lineCount; i++) out.keep[i] = !drop[i];
    out.keptCount = (uint8_t)kept;

    // fits is decided by capacity, NOT by whether we dropped anything: a page that
    // could not drop enough lines (only SAFETY left) must report fits=false even with
    // zero drops — a "nothing was dropped" shortcut here once hid an over-capacity
    // SAFETY-only page as fitting.
    if (kept <= (int)fr.cap) {
        fr.fits = true;
        if (kept == (int)p.lineCount) { fr.message[0] = '\0'; return; }
        int n = std::snprintf(fr.message, sizeof(fr.message),
            "template needs %u lines, panel has %u - dropped: ",
            (unsigned)p.lineCount, (unsigned)fr.cap);
        if (n < 0) n = 0;
        if (n < (int)sizeof(fr.message)) {
            for (uint8_t i = 0; i < fr.droppedCount && n < (int)sizeof(fr.message); i++)
                n += std::snprintf(fr.message + n, sizeof(fr.message) - n,
                                   "%s%s", i ? ", " : "", fr.dropped[i]);
        }
    } else {
        fr.fits = false;
        std::snprintf(fr.message, sizeof(fr.message),
            "template needs %u lines, panel has %u - cannot drop SAFETY lines",
            (unsigned)p.lineCount, (unsigned)fr.cap);
    }
}

// ---------------------------------------------------------------- render

struct RenderStats {
    uint8_t linesDrawn;
    uint8_t truncated;     // lines clipped at the panel edge
    uint8_t fieldErrors;   // renderField failures shown as "??", never stale values
};

inline bool condTruthy(const LiveState& s, FieldId f) {
    char tmp[16];
    const FieldDef* d = fieldById(f);
    if (!d) return false;
    if (d->kind == FieldKind::Str) {
        if (!renderField(s, f, "%s", tmp, sizeof(tmp))) return false;
        return tmp[0] != '\0';
    }
    if (!renderField(s, f, d->kind == FieldKind::Float ? "%g" : "%d", tmp, sizeof(tmp))) return false;
    return std::strcmp(tmp, "0") != 0 && std::strcmp(tmp, "-0") != 0 && std::strcmp(tmp, "0.00") != 0;
}

// Render one laid-out page into fb at 8-px rows in keep order.
inline void renderPage(const Template& t, const Layout& lay, uint8_t pageIdx,
                       const LiveState& s, Framebuf& fb, RenderStats& st) {
    st = RenderStats{};
    fb.clear();
    const Page& p = t.page[pageIdx];
    int16_t row = 0;
    for (uint8_t li = 0; li < p.lineCount; li++) {
        if (!lay.keep[li]) continue;
        const Line& ln = p.line[li];
        if (ln.conditional && !condTruthy(s, ln.condField)) { row++; continue; }
        int16_t x = 0;
        if (ln.prim == kBar) {
            for (uint8_t si = 0; si < ln.segCount; si++) {
                if (ln.seg[si].isField) {
                    double frac = 0.0;
                    if (fieldGaugeFrac(s, ln.seg[si].field, &frac)) fb.gauge(x, row, kBarWidthPx, frac);
                    else st.fieldErrors++;
                }
            }
            st.linesDrawn++;
            row++;
            continue;
        }
        // text-ish: big/row/text/blink (blink renders as text until M8 timing)
        for (uint8_t si = 0; si < ln.segCount; si++) {
            // Off-edge check BEFORE drawing: a row that ends exactly at the panel edge
            // has not clipped (an earlier post-draw check double-counted it), and the
            // "}}" parse keeps inter-field spaces as the leading byte of the next
            // literal, which must not read as a truncation either.
            if (x >= (int16_t)fb.width()) { st.truncated++; break; }
            const Seg& sg = ln.seg[si];
            if (!sg.isField) {
                if (!fb.drawText(x, row, sg.lit)) st.truncated++;
                x += fb.textWidth(sg.lit);
            } else {
                // tmp must be >= the widest string LiveState can hold (fault_banner[48]):
                // a smaller buffer turns a long banner into "??" instead of a clipped line.
                char tmp[48];
                if (!renderField(s, sg.field, sg.fmt, tmp, sizeof(tmp))) {
                    std::strncpy(tmp, "??", sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0';
                    st.fieldErrors++;
                }
                if (!fb.drawText(x, row, tmp)) st.truncated++;
                x += fb.textWidth(tmp);
            }
        }
        st.linesDrawn++;
        row++;
    }
}

// One-shot: parse -> validate -> layout -> render for a page, for callers that just
// want the frame (bench `o`, M6 preview). Returns false on parse/validate/fit failure.
// The caller supplies the Template scratch: it is ~18 KB (see the footprint test), so
// ONE shared static instance per serial path is the target pattern — never a per-call
// local or a hidden per-function static (DRAM is as scarce as stack; the 2026-11-20
// boot-loop fix found four statics overflowing dram0_0_seg by 35 KB).
inline bool renderTemplate(const char* text, Template& t, uint8_t pageIdx, uint16_t panelW,
                           uint16_t panelH, const LiveState& s, Framebuf& fb,
                           ParseResult& pres, ValidationResult& vr, Layout& lay,
                           RenderStats& st) {
    vr = ValidationResult{true, kOk, {}};
    if (!parse(text, t, pres)) return false;
    if (!validate(t, vr)) return false;
    if (pageIdx >= t.pageCount) { vr = {false, kBadPage, {"page index out of range"}}; return false; }
    layoutPage(t, pageIdx, panelW, panelH, lay);
    if (!lay.fit.fits) {
        vr.ok = false; vr.err = kTooManyLines;
        std::strncpy(vr.msg, lay.fit.message, sizeof(vr.msg) - 1);
        vr.msg[sizeof(vr.msg) - 1] = '\0';
        return false;
    }
    renderPage(t, lay, pageIdx, s, fb, st);
    return true;
}

}  // namespace tmpl
}  // namespace disp

#endif
