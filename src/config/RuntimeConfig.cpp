#include "config/RuntimeConfig.h"

#include <LittleFS.h>
#include <Preferences.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace cfgstore {

cfg::RuntimeConfig g_cfg;

namespace {

constexpr const char* kPath   = "/config.json";
constexpr const char* kTmp    = "/config.json.tmp";
constexpr const char* kNvsNs  = "divectl";
constexpr const char* kNvsKey = "cfg";

// Statics, not locals (decision 40 rule 1: KB-scale structs never touch a task stack —
// these run on loopTask's 8 KB at boot, and applyAndSave runs in the service windows).
static char g_fsBuf[kJsonCap];
static char g_nvsBuf[kJsonCap];
static char g_outBuf[kJsonCap];
static cfg::JsonDoc g_doc;

bool g_fsUp = false;
bool g_nvsUp = false;

SemaphoreHandle_t g_mtx = nullptr;
cfg::RuntimeConfig g_webCfg;

// --- LittleFS side ---------------------------------------------------------------
// states: 0 absent, 1 loaded into buf, 2 present but unreadable/truncated
int readFs(char* buf) {
    if (!g_fsUp) return 0;
    if (!LittleFS.exists(kPath)) return 0;
    File f = LittleFS.open(kPath, "r");
    if (!f) return 2;
    size_t sz = f.size();
    if (sz == 0 || sz >= (size_t)kJsonCap) { f.close(); return 2; }   // empty/oversize = untrusted
    int got = f.readBytes(buf, sz);
    f.close();
    if (got != (int)sz) return 2;
    buf[sz] = '\0';
    return 1;
}

bool writeFs(const char* text) {
    if (!g_fsUp) return true;                       // store down: caller tracks that separately
    File t = LittleFS.open(kTmp, "w");
    if (!t) return false;
    t.print(text);
    t.flush();
    t.close();
    if (LittleFS.exists(kPath)) LittleFS.remove(kPath);
    if (!LittleFS.rename(kTmp, kPath)) return false;
    // Write-verify: read the file back and compare. A save that does not survive its own
    // filesystem is exactly the corruption the NVS mirror exists to survive.
    static char verify[kJsonCap];
    if (readFs(verify) != 1) return false;
    return strcmp(verify, text) == 0;
}

// --- NVS side ---------------------------------------------------------------------
// getString is capped at maxLen, so an oversized value reads back mismatched and is
// treated as corrupt — same all-or-nothing rule as the file path.
int readNvs(char* buf) {
    if (!g_nvsUp) return 0;
    Preferences prefs;
    if (!prefs.begin(kNvsNs, true)) return 2;
    // Read at source (core 4.0.0 libraries/Preferences/src/Preferences.cpp): the char* form
    // returns 0 both for "key absent" and "value longer than maxLen", so absence is
    // distinguished with getStringLength() (which is the nvs length including the NUL) —
    // otherwise an oversized value would be reported as a missing key and the store pair
    // would silently look "fresh" instead of corrupt.
    const size_t len = prefs.getStringLength(kNvsKey);
    if (len == 0) { prefs.end(); return 0; }                  // key absent
    if (len >= (size_t)kJsonCap) { prefs.end(); return 2; }    // over our buffer: corrupt
    const size_t got = prefs.getString(kNvsKey, buf, (size_t)kJsonCap);
    prefs.end();
    if (got == 0 || buf[0] == '\0') return 2;
    return 1;
}

bool writeNvs(const char* text) {
    if (!g_nvsUp) return true;
    Preferences prefs;
    if (!prefs.begin(kNvsNs, false)) return false;
    size_t wrote = prefs.putString(kNvsKey, text);
    prefs.end();
    if (wrote != strlen(text) + 1) return false;
    static char verify[kJsonCap];
    if (readNvs(verify) != 1) return false;                    // read-back verify, same rule
    return strcmp(verify, text) == 0;
}

// --- validity: parse + build + validate (the full gate, on a throwaway copy) -------
bool docValid(char* text, cfg::RuntimeConfig* into) {
    char why[64];
    if (!cfg::configFromJson(text, (int)strlen(text), into, &g_doc, why, sizeof why)) return false;
    cfg::ConfigErrors errs;
    return cfg::configValidate(*into, &errs);
}

}  // namespace

bool loadAtBoot(boot::Report& rep, uint32_t nowMs, bool fsMounted) {
    g_fsUp = fsMounted;
    if (!g_mtx) g_mtx = xSemaphoreCreateMutex();

    Preferences probe;
    // Read-write, deliberately: a read-only open of a namespace that has never been written
    // fails (nvs_open returns NOT_FOUND), which would report the mirror as DOWN on every
    // fresh board and raise an NVS fault for nothing. RW open creates it.
    g_nvsUp = probe.begin(kNvsNs, false);
    if (g_nvsUp) probe.end();
    else rep.raise(FaultCode::NVS, nowMs, "nvs namespace divectl will not open");

    int fsState = readFs(g_fsBuf);
    int nvsState = readNvs(g_nvsBuf);

    static cfg::RuntimeConfig fsCfg, nvsCfg;
    bool fsValid = (fsState == 1) && docValid(g_fsBuf, &fsCfg);
    bool nvsValid = (nvsState == 1) && docValid(g_nvsBuf, &nvsCfg);

    const char* corruptDetail = nullptr;
    bool useFs = false, useNvs = false, loaded = false;

    if (fsValid && nvsValid) {
        loaded = true;
        useFs = true;                                        // LittleFS is canonical
        if (strcmp(g_fsBuf, g_nvsBuf) != 0) corruptDetail = "stores differ, took LittleFS";
    } else if (fsValid) {
        loaded = true;
        useFs = true;
        if (nvsState == 2) corruptDetail = "nvs mirror corrupt, took LittleFS";
        else if (g_nvsUp) corruptDetail = "nvs mirror missing, took LittleFS";
        // nvs store DOWN: the NVS fault above already says the unit runs single-copy.
    } else if (nvsValid) {
        loaded = true;
        useNvs = true;
        if (fsState == 2) corruptDetail = "littlefs config corrupt, took nvs mirror";
        else if (g_fsUp) corruptDetail = "littlefs config missing, took nvs mirror";
    } else if (fsState != 0 || nvsState != 0) {
        corruptDetail = "all stored configs failed parse/validation, defaults in use";
    }
    // else: nothing stored anywhere — factory defaults, fresh board, no fault.

    g_cfg = useFs ? fsCfg : (useNvs ? nvsCfg : cfg::RuntimeConfig{});

    if (corruptDetail) rep.raise(FaultCode::CONFIG_CORRUPT, nowMs, corruptDetail);

    // Repair: if a live store lacks the selected document (or disagrees), re-write both.
    // Success CLEARS CONFIG_CORRUPT (history kept, hot-plug-close pattern); failure leaves
    // the fault raised and blocking arming until a later clean save.
    int n = cfg::configToJson(g_cfg, g_outBuf, kJsonCap);
    if (corruptDetail && n > 0) {
        bool repaired = writeFs(g_outBuf) && writeNvs(g_outBuf);
        if (repaired) rep.clear(FaultCode::CONFIG_CORRUPT, nowMs);
    }

    publishWebMirror();
    return loaded;
}

int applyAndSave(boot::Report& rep, uint32_t nowMs,
                 const cfg::RuntimeConfig& candidate, cfg::ConfigErrors& errors) {
    // §9: server-side validation first — a rejected edit persists NOTHING and touches nothing.
    if (!cfg::configValidate(candidate, &errors)) return 0;

    int n = cfg::configToJson(candidate, g_outBuf, kJsonCap);

    // The candidate goes live either way: the diver must see what they typed. Persistence
    // failure then refuses ARMING via CONFIG_WRITE_FAILED, so "live but unsaved" can never
    // become "dive on settings that differ from the stored ones".
    g_cfg = candidate;
    publishWebMirror();

    if (n <= 0 || (!g_fsUp && !g_nvsUp)) {
        rep.raise(FaultCode::CONFIG_WRITE_FAILED, nowMs,
                  n <= 0 ? "config does not fit its buffer" : "no config store available");
        return -1;
    }

    bool fsOk = writeFs(g_outBuf);
    bool nvsOk = writeNvs(g_outBuf);
    if (fsOk && nvsOk) {
        rep.clear(FaultCode::CONFIG_WRITE_FAILED, nowMs);
        rep.clear(FaultCode::CONFIG_CORRUPT, nowMs);          // a clean double-write heals it
        return 1;
    }

    char detail[48];
    snprintf(detail, sizeof detail, "fs %s, nvs %s", fsOk ? "ok" : "FAIL", nvsOk ? "ok" : "FAIL");
    rep.raise(FaultCode::CONFIG_WRITE_FAILED, nowMs, detail);
    return -1;
}

void publishWebMirror() {
    if (!g_mtx) return;
    if (xSemaphoreTake(g_mtx, portMAX_DELAY) == pdTRUE) {
        g_webCfg = g_cfg;
        xSemaphoreGive(g_mtx);
    }
}

bool copyWebMirror(cfg::RuntimeConfig* out) {
    if (!out || !g_mtx) return false;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    *out = g_webCfg;
    xSemaphoreGive(g_mtx);
    return true;
}

bool parseCandidate(const char* json, int len, cfg::RuntimeConfig* into,
                    char* why, int whyLen) {
    if (!json || !into) return false;
    return cfg::configFromJson(json, len, into, &g_doc, why, whyLen);
}

int configJsonForWeb(char* buf, int cap) {
    if (!g_mtx || cap <= 0) return -1;
    int n = -1;
    if (xSemaphoreTake(g_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        n = cfg::configToJson(g_webCfg, buf, cap);           // ~1 ms — human-rate GET only
        xSemaphoreGive(g_mtx);
    }
    return n;
}

// --- edit queue ---------------------------------------------------------------------
namespace {
static QueueHandle_t g_cfgQ = nullptr;
static SemaphoreHandle_t g_cfgDone = nullptr;
static volatile int g_cfgResult = -99;
static cfg::ConfigErrors g_cfgErrs;      // most recent apply's validation report
}  // namespace

bool configQueueInit() {
    if (!g_cfgQ) g_cfgQ = xQueueCreate(2, sizeof(cfg::RuntimeConfig));
    if (!g_cfgDone) g_cfgDone = xSemaphoreCreateBinary();
    return g_cfgQ != nullptr && g_cfgDone != nullptr;
}

bool configQueuePush(const cfg::RuntimeConfig& candidate) {
    if (!g_cfgQ) return false;
    return xQueueSend(g_cfgQ, &candidate, 0) == pdTRUE;
}

int serviceConfigQueue(boot::Report& rep, uint32_t nowMs) {
    if (!g_cfgQ) return -1;
    cfg::RuntimeConfig candidate;
    if (xQueueReceive(g_cfgQ, &candidate, 0) != pdTRUE) return -1;
    // Re-validated here: the queue crossing must not be trusted, and the reply the waiting
    // POST builds comes out of this validation, not out of whatever the web task thought.
    int code = applyAndSave(rep, nowMs, candidate, g_cfgErrs);
    g_cfgResult = code;
    if (g_cfgDone) xSemaphoreGive(g_cfgDone);
    return code;
}

bool configAwaitApply(int* code, uint32_t waitMs) {
    if (!g_cfgDone) return false;
    if (xSemaphoreTake(g_cfgDone, pdMS_TO_TICKS(waitMs)) != pdTRUE) return false;
    if (code) *code = g_cfgResult;
    return true;
}

// --- last validation report ----------------------------------------------------------
bool configLastErrors(cfg::ConfigErrors* out) {
    if (!out) return false;
    *out = g_cfgErrs;
    return g_cfgErrs.count > 0 || g_cfgErrs.overflowed;
}

// --- wall clock request (RAM only, applied by loopTask) -------------------------------
namespace {
static volatile bool g_timePending = false;
static volatile uint32_t g_timeValue = 0;
}

void requestWallClock(uint32_t unixS) {
    g_timeValue = unixS;
    g_timePending = true;         // value before flag; the reader takes the flag first
}

bool serviceWallClock(uint32_t* unixS) {
    if (!g_timePending) return false;
    const uint32_t v = g_timeValue;
    g_timePending = false;
    g_cfg.lastWallS = v;
    g_cfg.lastWallUptimeS = millis() / 1000u;
    publishWebMirror();
    if (unixS) *unixS = v;
    return true;
}

// --- factory reset: remove the config from BOTH stores, defaults go live ---------------
bool factoryWipe(boot::Report& rep, uint32_t nowMs) {
    bool ok = true;
    if (g_fsUp) {
        if (LittleFS.exists(kTmp)) LittleFS.remove(kTmp);
        if (LittleFS.exists(kPath) && !LittleFS.remove(kPath)) ok = false;
    }
    if (g_nvsUp) {
        Preferences prefs;
        if (!prefs.begin(kNvsNs, false)) ok = false;
        else { prefs.remove(kNvsKey); prefs.end(); }   // removing an absent key is fine
    }

    g_cfg = cfg::RuntimeConfig{};                       // factory defaults, live
    publishWebMirror();

    // Verify the wipe, do not assume it (the same rule as every write in this file): if a
    // store still answers, defaults are live while an old config is still on flash — the
    // divergence CONFIG_WRITE_FAILED exists for.
    if (readFs(g_fsBuf) != 0 || readNvs(g_nvsBuf) != 0) ok = false;

    if (!ok) {
        rep.raise(FaultCode::CONFIG_WRITE_FAILED, nowMs, "factory reset incomplete");
        Serial.println(F("[cfg] FACTORY RESET INCOMPLETE — a store still holds a config; "
                         "CONFIG_WRITE_FAILED blocks arming until a clean save"));
    } else {
        rep.clear(FaultCode::CONFIG_CORRUPT, nowMs);
        rep.clear(FaultCode::CONFIG_WRITE_FAILED, nowMs);
        Serial.println(F("[cfg] factory reset done: no stored config, factory defaults live"));
    }
    return ok;
}

void storeInspect(StoreState* out) {
    if (!out) return;
    *out = StoreState{};
    out->fsMounted = g_fsUp;
    out->nvsOpenable = g_nvsUp;

    // Scratch reuse is deliberate: g_fsBuf/g_nvsBuf have exactly two users — loadAtBoot, which
    // finishes before the web server starts, and this bench read, which runs on loopTask after
    // boot. The shared JsonDoc is NOT used here (after boot its user is the radio task in
    // parseCandidate), which is why the schema number is read as text: configToJson always
    // writes {"schema":N,...}, so scanning firmware-written text is a lookup, not a parse.
    const int fs = readFs(g_fsBuf);
    const int nvs = readNvs(g_nvsBuf);
    out->fs = g_fsUp ? fs : 3;
    out->nvs = g_nvsUp ? nvs : 3;
    if (fs == 1) {
        out->fsBytes = strlen(g_fsBuf);
        const char* p = strstr(g_fsBuf, "\"schema\"");
        if (p) { p += 8; while (*p && (*p == ':' || *p == ' ')) p++; out->fsSchema = atoi(p); }
    }
    if (nvs == 1) {
        out->nvsBytes = strlen(g_nvsBuf);
        const char* p = strstr(g_nvsBuf, "\"schema\"");
        if (p) { p += 8; while (*p && (*p == ':' || *p == ' ')) p++; out->nvsSchema = atoi(p); }
    }
    out->identical = (fs == 1 && nvs == 1 && strcmp(g_fsBuf, g_nvsBuf) == 0);
}

}  // namespace cfgstore
