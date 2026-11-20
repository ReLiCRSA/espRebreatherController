// Boot / runtime fault report (AGENTS.md 4, "boot is never fatal").
//
// Pure C++ so it is host-testable, and allocation-free so it can be raised from the dive
// task. Every missing device has to end up here, because the rule is no silent fallbacks:
// if a sensor is absent, something must say so.
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FaultCode.h"

namespace boot {

constexpr size_t kMaxDetail = 24; // "no response at 0x48", "SDA held low"
constexpr size_t kCapacity = 32;  // active faults tracked at once

struct Entry {
    FaultCode code = FaultCode::NONE;
    Severity severity = Severity::INFO;
    uint32_t firstMs = 0;
    uint32_t lastMs = 0;
    uint16_t attempts = 0;
    bool present = false; // false once a device has come back online
    char detail[kMaxDetail] = {};
};

class Report {
  public:
    // Raises a fault, or escalates an existing one. Re-raising the same code with the same
    // severity just bumps `attempts`, so a device re-probed every 5 s does not flood the log.
    void raise(FaultCode code, Severity sev, uint32_t nowMs, const char* detail = nullptr,
               int attempt = 1) {
        Entry* e = find(code);
        if (!e) {
            e = freeSlot();
            if (!e) return; // capacity exhausted: keep the oldest, never overwrite in place
            e->code = code;
            e->firstMs = nowMs;
            e->attempts = 0;
            e->present = true;
        }
        if (sev > e->severity) e->severity = sev;
        e->lastMs = nowMs;
        e->attempts = static_cast<uint16_t>(e->attempts + (attempt > 0 ? attempt : 1));
        if (detail) copyDetail(e->detail, detail);
        e->present = true;
    }

    void raise(FaultCode code, uint32_t nowMs, const char* detail = nullptr, int attempt = 1) {
        raise(code, defaultSeverity(code), nowMs, detail, attempt);
    }

    // A device that was missing and now answers: the entry is closed, not deleted, so the
    // history (and /api/health) can still show that it dropped out at boot.
    void clear(FaultCode code, uint32_t nowMs) {
        if (Entry* e = find(code)) {
            e->present = false;
            e->lastMs = nowMs;
        }
    }

    bool active(FaultCode code) const {
        const Entry* e = find(code);
        return e && e->present;
    }

    const Entry* findEntry(FaultCode code) const { return find(code); }

    // Any active fault whose code is on the arming block list (AGENTS.md 13). The arming
    // gate asks this; the boot path never calls it, because boot is never fatal.
    bool blocksArming() const {
        for (size_t i = 0; i < kCapacity; ++i)
            if (slots_[i].code != FaultCode::NONE && slots_[i].present &&
                // AGENTS.md 13: an unresolved FATAL refuses arming on its own, so a new fatal code
                // cannot become armable just because nobody remembered to list it in the table.
                (slots_[i].severity == Severity::FATAL || ::blocksArming(slots_[i].code)))
                return true;
        return false;
    }

    Severity highest() const {
        Severity h = Severity::INFO;
        for (size_t i = 0; i < kCapacity; ++i)
            if (slots_[i].present && slots_[i].code != FaultCode::NONE && slots_[i].severity > h)
                h = slots_[i].severity;
        return h;
    }

    // One-line OLED/serial summary: "F:1 E:1 W:0 ADS1115_U1 BARO". The severity counts are
    // always over the whole set, names are written whole or not at all, and if any had to be
    // dropped the line ends in "+..." — a clipped line must never look like an all-clear, and
    // a half-written code ("ADS1115_") reads like a different code.
    size_t summaryLine(char* out, size_t cap) const {
        if (!out || cap == 0) return 0;
        int fatal = 0, err = 0, warn = 0, active = 0, shown = 0;
        for (size_t i = 0; i < kCapacity; ++i) {
            const Entry& e = slots_[i];
            if (!e.present || e.code == FaultCode::NONE) continue;
            ++active;
            if (e.severity == Severity::FATAL) ++fatal;
            else if (e.severity == Severity::ERROR) ++err;
            else if (e.severity == Severity::WARN) ++warn;
        }
        if (active == 0) return terminate(cap, out, copyInto(cap, out, 0, "ALL SENSORS OK"));

        static const char kClip[] = "+...";
        size_t n = 0;
        n = copyInto(cap, out, n, "F:");
        n = copyNumber(cap, out, n, fatal);
        n = copyInto(cap, out, n, " E:");
        n = copyNumber(cap, out, n, err);
        n = copyInto(cap, out, n, " W:");
        n = copyNumber(cap, out, n, warn);
        n = copyInto(cap, out, n, " ");

        int remaining = active;
        for (size_t i = 0; i < kCapacity && remaining > 0; ++i) {
            const Entry& e = slots_[i];
            if (!e.present || e.code == FaultCode::NONE) continue;
            const char* nm = faultName(e.code);
            const size_t len = strLen(nm);
            // Leave room for the clip marker while names are still waiting.
            const size_t reserve = remaining > 1 ? strLen(kClip) : 0;
            if (n + len + 1 + reserve > cap - 1) break;
            for (size_t k = 0; k < len; ++k) out[n++] = nm[k];
            out[n++] = ' ';
            ++shown;
            --remaining;
        }
        if (shown < active) n = copyInto(cap, out, n, kClip);
        return terminate(cap, out, n);
    }

    // M6 web (§9): /api/health and /faults serialize the whole report — ACTIVE faults and
    // closed history entries, because §4's rule is that a device which came back keeps its
    // entry so the web can still show it dropped out at boot. `armOk` repeats the gate
    // answer so the page shows why arming is refused, next to the codes that refuse it.
    // Returns the length, or -1 when the buffer was too small: a half-written fault list
    // is exactly the silent-all-clear failure mode this project refuses (§17), so an
    // overflow is an error the caller must see, never a clipped string.
    int toJson(char* out, size_t cap) const {
        if (!out || cap == 0) return -1;
        int n = 0;
        int active = 0;
        for (size_t i = 0; i < kCapacity; ++i)
            if (slots_[i].code != FaultCode::NONE && slots_[i].present) ++active;
        if (!emit(out, cap, n, "{\"armOk\":%s,\"highest\":\"%s\",\"active\":%d,\"faults\":[",
                  blocksArming() ? "false" : "true", severityName(highest()), active))
            return -1;
        bool first = true;
        for (size_t i = 0; i < kCapacity; ++i) {
            const Entry& e = slots_[i];
            if (e.code == FaultCode::NONE) continue;
            if (!emit(out, cap, n, "%s{\"code\":\"%s\",\"sev\":\"%s\",\"active\":%s,"
                                   "\"attempts\":%u,\"firstMs\":%lu,\"lastMs\":%lu",
                      first ? "" : ",", faultName(e.code), severityName(e.severity),
                      e.present ? "true" : "false", (unsigned)e.attempts,
                      (unsigned long)e.firstMs, (unsigned long)e.lastMs))
                return -1;
            if (e.detail[0]) {
                char esc[kMaxDetail * 2 + 3];
                escapeInto(esc, sizeof esc, e.detail);
                if (!emit(out, cap, n, ",\"detail\":\"%s\"", esc)) return -1;
            }
            if (!emit(out, cap, n, "}")) return -1;
            first = false;
        }
        if (!emit(out, cap, n, "]}")) return -1;
        return n;
    }

    static const char* severityName(Severity s) {
        switch (s) {
            case Severity::FATAL: return "FATAL";
            case Severity::ERROR: return "ERROR";
            case Severity::WARN:  return "WARN";
            default:              return "INFO";
        }
    }

    int countActive(Severity atLeast = Severity::INFO) const {
        int n = 0;
        for (size_t i = 0; i < kCapacity; ++i)
            if (slots_[i].present && slots_[i].code != FaultCode::NONE &&
                slots_[i].severity >= atLeast)
                ++n;
        return n;
    }

    size_t count() const {
        size_t n = 0;
        for (size_t i = 0; i < kCapacity; ++i)
            if (slots_[i].code != FaultCode::NONE) ++n;
        return n;
    }

    // Iteration for the web /api/health and /faults endpoints.
    template <typename Fn>
    void forEach(Fn&& fn) const {
        for (size_t i = 0; i < kCapacity; ++i)
            if (slots_[i].code != FaultCode::NONE) fn(slots_[i]);
    }

    void reset() { memset(slots_, 0, sizeof(slots_)); }

  private:
    // printf-style append with a hard failure on overflow: every call site checks the
    // return, so the JSON is either complete or the whole toJson returns -1.
    static bool emit(char* out, size_t cap, int& n, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        int r = vsnprintf(out + n, cap - (size_t)n, fmt, ap);
        va_end(ap);
        if (r < 0 || (size_t)r >= cap - (size_t)n) return false;
        n += r;
        return true;
    }
    // detail strings are firmware-authored, so this is defensive against a future site
    // that puts a quote or backslash in one — never against user text reaching here.
    static void escapeInto(char* dst, size_t cap, const char* src) {
        size_t k = 0;
        for (const char* p = src; *p && k + 2 < cap; ++p) {
            if (*p == '"' || *p == '\\') dst[k++] = '\\';
            dst[k++] = *p;
        }
        dst[k] = '\0';
    }

    Entry* find(FaultCode code) {
        for (size_t i = 0; i < kCapacity; ++i)
            if (slots_[i].code == code && slots_[i].code != FaultCode::NONE) return &slots_[i];
        return nullptr;
    }
    const Entry* find(FaultCode code) const {
        for (size_t i = 0; i < kCapacity; ++i)
            if (slots_[i].code == code && slots_[i].code != FaultCode::NONE) return &slots_[i];
        return nullptr;
    }
    Entry* freeSlot() {
        for (size_t i = 0; i < kCapacity; ++i)
            if (slots_[i].code == FaultCode::NONE) return &slots_[i];
        return nullptr;
    }
    static void copyDetail(char* dst, const char* src) {
        size_t i = 0;
        for (; src[i] && i + 1 < kMaxDetail; ++i) dst[i] = src[i];
        dst[i] = '\0';
    }
    static size_t terminate(size_t cap, char* out, size_t n) {
        out[n < cap ? n : cap - 1] = '\0';
        return n;
    }
    static size_t strLen(const char* s) {
        size_t n = 0;
        while (s[n]) ++n;
        return n;
    }
    // Appends s at position pos, clipped to cap-1 characters so there is always room for the
    // terminator the caller writes. Returns the new length, never >= cap.
    static size_t copyInto(size_t cap, char* out, size_t pos, const char* s) {
        if (pos >= cap) return cap - 1;
        for (size_t i = 0; s[i]; ++i) {
            if (pos + 1 >= cap) break;
            out[pos++] = s[i];
        }
        return pos;
    }
    static size_t copyNumber(size_t cap, char* out, size_t pos, int value) {
        char buf[12];
        size_t len = 0;
        if (value < 0) { buf[len++] = '-'; value = -value; }
        char tmp[12];
        size_t t = 0;
        do { tmp[t++] = static_cast<char>('0' + (value % 10)); value /= 10; } while (value);
        while (t && pos + 1 < cap) out[pos++] = tmp[--t];
        (void)buf;
        return pos;
    }

    Entry slots_[kCapacity];
};

} // namespace boot
