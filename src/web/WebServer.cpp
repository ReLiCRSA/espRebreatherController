// M6 web server implementation. See WebServer.h for the threading contract and its
// measured provenance. Handler code here runs on the AsyncTCP service task ONLY — every
// value it needs from the dive path was published by loopTask first.
#include "web/WebServer.h"

#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>

#include <ESPAsyncWebServer.h>

#include <string.h>
#include <stdlib.h>

#include "StateJson.h"
#include "TemplateBundles.h"
#include "config/RuntimeConfig.h"
#include "dive/DiveLog.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

namespace webnet {

namespace {

// ------------------------------------------------------------------ shared objects
boot::Report* g_rep = nullptr;
AsyncWebServer g_server(80);
AsyncWebSocket g_ws("/ws");
bool g_running = false;
bool g_handlersUp = false;
char g_ssid[20] = "";

// §9 SoftAP: the phone has no internet at the water; WPA2 with a fixed passphrase
// documented in the README. An open AP beside a life-support device is not acceptable
// either, so it is WPA2 both ways.
constexpr const char* kApPass = "dive-ccr-wifi";
constexpr const char* kMdnsHost = "diveccr";

// ------------------------------------------------------------------ snapshot transport
static QueueHandle_t g_snapQ = nullptr;   // 1-deep, xQueueOverwrite (§4)
static volatile uint32_t g_snapT = 0;
static char g_wsJson[web::kStateJsonCap];  // loopTask-owned: ticker builds and pushes
static uint32_t g_lastWsPush = 0;

// ------------------------------------------------------------------ POST bodies
// One accumulator: the AsyncTCP service task is single-task (one async_tcp service task,
// AsyncTCP.cpp:742 as read), so handler callbacks are serialized and no two handlers
// ever run at once. Bodies arrive via onBody chunks before the completion handler.
constexpr int kBodyCap = 4096;            // config JSON ~1.3 KB; lib aborts >16 KB first
// kTemplateCap comes from WebServer.h (shared with main.cpp's save service).
static char g_body[kBodyCap];
static size_t g_bodyLen = 0;
static bool g_bodyOverflow = false;

// ------------------------------------------------------------------ job status blocks
// loopTask writes, web reads at human rate. Scalars are volatile; the msg/slope fields
// can in principle tear mid-update while a job runs — cosmetic on a status line, stated
// rather than locked.
struct JobStatus {
    volatile int state = 0;
    volatile int tS = 0;
    volatile int extra = 0;       // sim totalS | cal passMask
    float sigmaMv = 0.0f;         // cal
    float slopes[3] = {0, 0, 0};  // cal
    char msg[64] = "";
};
static JobStatus g_cal, g_sim;
// 1400 B, not 320: the fixed profile's five assertions each carry a sentence, and a report
// that silently lost the third item would be a §11 regression suite lying about its coverage.
static char g_simVerify[1400] = "";

// ------------------------------------------------------------------ template save job
static char g_tmplName[24] = "";
static char g_tmplText[kTemplateCap] = "";
static volatile bool g_tmplPending = false;
static SemaphoreHandle_t g_tmplDone = nullptr;
static volatile bool g_tmplOk = false;
static char g_tmplMsg[128] = "";

// ------------------------------------------------------------------ preview job
static char g_prevName[24] = "";
static volatile int g_prevW = 0, g_prevH = 0;
static volatile bool g_prevPending = false;
static SemaphoreHandle_t g_prevDone = nullptr;
static char g_prevJson[2560] = "";
static volatile int g_prevLen = -1;

static volatile bool g_calStartReq = false;
static volatile bool g_simStartReq = false;
static volatile bool g_simStopReq = false;
static volatile bool g_factoryReq = false;

// The active display template's name lives on LittleFS (/display/active.txt), which the
// loopTask save service owns. The web task may not read it per request — reading it means
// a flash open on the radio task — so loopTask mirrors it here on boot and on every save.
char g_activeTmpl[24] = "bench_128x64";
void setActiveTemplate(const char* name) {
    strncpy(g_activeTmpl, name ? name : "", sizeof g_activeTmpl - 1);
    g_activeTmpl[sizeof g_activeTmpl - 1] = '\0';
}

// ------------------------------------------------------------------ tiny helpers
// The web task parses these bodies, so these helpers exist; the VALUES they extract
// still cross a queue into loopTask, which re-validates everything (§9). Extracting a
// number here is convenience, never trust.
bool bodyGetInt(const char* body, const char* key, long* out) {
    char pat[24];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char* p = strstr(body, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char* end = nullptr;
    const long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

bool bodyConfirm(const char* body) {
    return strstr(body, "\"confirm\":true") || strstr(body, "\"confirm\": true");
}

bool nameOk(const char* n) {
    int i = 0;
    for (; n[i]; i++) {
        if (i >= 23) return false;
        const char c = n[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return i > 0;
}

void sendJson(AsyncWebServerRequest* r, int code, const char* body) {
    r->send(code, "application/json", body);
}

// URI tail after a prefix: "/api/dives/3.csv" after "/api/dives/" -> "3.csv"
bool uriTail(const String& url, const char* prefix, char* out, int cap) {
    if (!url.startsWith(prefix)) return false;
    const char* t = url.c_str() + strlen(prefix);
    if ((int)strlen(t) >= cap) return false;
    strcpy(out, t);
    return true;
}

void onBody(AsyncWebServerRequest* /*r*/, uint8_t* data, size_t len, size_t index, size_t /*total*/) {
    if (index == 0) { g_bodyLen = 0; g_bodyOverflow = false; g_body[0] = '\0'; }
    if (g_bodyLen + len >= (size_t)kBodyCap) { g_bodyOverflow = true; return; }
    memcpy(g_body + g_bodyLen, data, len);
    g_bodyLen += len;
    g_body[g_bodyLen] = '\0';
}

}  // namespace

// ============================================================ cross-task API (§4)

bool takeCalStartRequest() {
    if (!g_calStartReq) return false;
    g_calStartReq = false;
    return true;
}
void requestCalStart() { g_calStartReq = true; }

void setCalStatus(int state, int tS, float sigmaMv, int passMask,
                  const float slopes[3], const char* msg) {
    g_cal.state = state; g_cal.tS = tS; g_cal.extra = passMask; g_cal.sigmaMv = sigmaMv;
    for (int i = 0; i < 3; i++) g_cal.slopes[i] = slopes[i];
    strncpy(g_cal.msg, msg ? msg : "", sizeof g_cal.msg - 1);
    g_cal.msg[sizeof g_cal.msg - 1] = '\0';
}

bool takeSimStartRequest() {
    if (!g_simStartReq) return false;
    g_simStartReq = false;
    return true;
}
void requestSimStart() { g_simStartReq = true; }
void requestSimStop() { g_simStopReq = true; }
bool simStopRequested() { return g_simStopReq; }
void clearSimStopRequest() { g_simStopReq = false; }

void setSimStatus(int state, int tS, int totalS) {
    g_sim.state = state; g_sim.tS = tS; g_sim.extra = totalS;
}
void setSimVerify(const char* json) {
    strncpy(g_simVerify, json ? json : "", sizeof g_simVerify - 1);
    g_simVerify[sizeof g_simVerify - 1] = '\0';
}

bool requestTemplateSave(const char* name, const char* text, int len) {
    if (g_tmplPending || !nameOk(name) || len <= 0 || len >= kTemplateCap) return false;
    strcpy(g_tmplName, name);
    memcpy(g_tmplText, text, (size_t)len);
    g_tmplText[len] = '\0';
    g_tmplPending = true;
    return true;
}

bool takeTemplateSave(char* name, int nameCap, char* text, int textCap) {
    if (!g_tmplPending) return false;
    snprintf(name, (size_t)nameCap, "%s", g_tmplName);
    snprintf(text, (size_t)textCap, "%s", g_tmplText);
    return true;   // caller reports via templateSaveDone(); flag clears there
}

void templateSaveDone(bool ok, const char* msg) {
    g_tmplOk = ok;
    strncpy(g_tmplMsg, msg ? msg : "", sizeof g_tmplMsg - 1);
    g_tmplMsg[sizeof g_tmplMsg - 1] = '\0';
    g_tmplPending = false;
    if (g_tmplDone) xSemaphoreGive(g_tmplDone);
}

bool templateSaveAwait(bool* ok, char* msg, int cap, uint32_t waitMs) {
    if (!g_tmplDone) return false;
    if (xSemaphoreTake(g_tmplDone, pdMS_TO_TICKS(waitMs)) != pdTRUE) return false;
    if (ok) *ok = g_tmplOk;
    if (msg) { strncpy(msg, g_tmplMsg, (size_t)cap - 1); msg[cap - 1] = '\0'; }
    return true;
}

bool requestPreview(const char* name, int w, int h) {
    if (g_prevPending || !nameOk(name)) return false;
    if ((w == 128 && (h == 64 || h == 32)) || (w == 64 && h == 48)) {
        g_prevW = w; g_prevH = h;
    } else {
        return false;   // framebuffer sizes: bigger panels are open items 6/15, not a pretend buffer
    }
    strcpy(g_prevName, name);
    g_prevPending = true;
    return true;
}

bool takePreview(char* name, int nameCap, int* w, int* h) {
    if (!g_prevPending) return false;
    snprintf(name, (size_t)nameCap, "%s", g_prevName);
    *w = g_prevW; *h = g_prevH;
    return true;   // flag clears in previewDone()
}

void previewDone(const char* json, int len) {
    if (len > 0 && len < (int)sizeof g_prevJson) {
        memcpy(g_prevJson, json, (size_t)len);
        g_prevJson[len] = '\0';
        g_prevLen = len;
    } else {
        g_prevJson[0] = '\0';
        g_prevLen = -1;
    }
    g_prevPending = false;
    if (g_prevDone) xSemaphoreGive(g_prevDone);
}

bool previewAwait(char* json, int cap, int* len, uint32_t waitMs) {
    if (!g_prevDone) return false;
    if (xSemaphoreTake(g_prevDone, pdMS_TO_TICKS(waitMs)) != pdTRUE) return false;
    const int n = g_prevLen;
    if (n > 0) { strncpy(json, g_prevJson, (size_t)cap - 1); json[cap - 1] = '\0'; }
    if (len) *len = n;
    return true;
}

void requestFactoryReset() { g_factoryReq = true; }
bool takeFactoryResetRequest() {
    if (!g_factoryReq) return false;
    g_factoryReq = false;
    return true;
}

const char* activeTemplate() { return g_activeTmpl; }

void snapshotPublish(const disp::LiveState& s, uint32_t tMs) {
    if (g_snapQ) xQueueOverwrite(g_snapQ, (void*)&s);
    g_snapT = tMs;
    if (!g_running || g_ws.count() == 0) return;
    if (tMs - g_lastWsPush < 500) return;      // ~2 Hz (§9)
    g_lastWsPush = tMs;
    const int n = web::liveStateToJson(s, g_wsJson, (int)sizeof g_wsJson, tMs);
    if (n > 0) g_ws.textAll(g_wsJson, (size_t)n);
    else g_ws.textAll("{\"error\":\"state frame overflow\"}", 31);   // never silence (§17)
    g_ws.cleanupClients();
}

}  // namespace webnet

// ============================================================ request handlers
// (AsyncTCP service task only — see header for the serialization argument.)

namespace webnet {
namespace {

// ----------------------------------------------------------- state / config / health

void hState(AsyncWebServerRequest* r) {
    static disp::LiveState st;
    static char json[web::kStateJsonCap];
    if (!g_snapQ || xQueuePeek(g_snapQ, &st, 0) != pdTRUE)
        return sendJson(r, 503, "{\"error\":\"no snapshot published yet\"}");
    const int n = web::liveStateToJson(st, json, (int)sizeof json, g_snapT);
    if (n < 0) return sendJson(r, 500, "{\"error\":\"state frame overflow — /api/fields partials impossible; bench 'p'\"}");
    sendJson(r, 200, json);
}

void hConfigGet(AsyncWebServerRequest* r) {
    static char buf[cfgstore::kJsonCap];
    const int n = cfgstore::configJsonForWeb(buf, (int)sizeof buf);
    if (n < 0) return sendJson(r, 500, "{\"error\":\"config serialization failed\"}");
    sendJson(r, 200, buf);
}

// Shared reply builder for the queue-crossing writes (config / mode / gas): the answer
// is the TRUE apply result from loopTask, not "queued" — §9's contract with the editor.
void respondApplyResult(AsyncWebServerRequest* r) {
    int code = -99;
    if (!cfgstore::configAwaitApply(&code, 2000))
        return sendJson(r, 202, "{\"queued\":true,\"applied\":\"pending — re-read /api/config\"}");
    if (code == 1) return sendJson(r, 200, "{\"ok\":true,\"saved\":true}");
    if (code == 0) {
        static char out[900];
        cfg::ConfigErrors errs;
        cfgstore::configLastErrors(&errs);
        int k = snprintf(out, sizeof out, "{\"ok\":false,\"errors\":[");
        for (int i = 0; i < errs.count && k < (int)sizeof out - 96; i++) {
            const int w = snprintf(out + k, sizeof out - (size_t)k, "%s{\"field\":\"%s\",\"msg\":\"%s\"}",
                                   i ? "," : "", errs.items[i].field, errs.items[i].msg);
            if (w > 0) k += w;
        }
        snprintf(out + k, sizeof out - (size_t)k, "%s],\"overflowed\":%s}",
                 errs.count ? "" : "", errs.overflowed ? "true" : "false");
        return sendJson(r, 400, out);
    }
    sendJson(r, 500, "{\"ok\":false,\"applied\":\"live-but-not-persisted\",\"fault\":\"CONFIG_WRITE_FAILED blocks arming\"}");
}

void hConfigPost(AsyncWebServerRequest* r) {
    if (g_bodyOverflow) return sendJson(r, 413, "{\"ok\":false,\"errors\":[{\"field\":\"document\",\"msg\":\"body over 4 KB\"}]}");
    cfg::RuntimeConfig cand;
    char why[64];
    if (!cfgstore::parseCandidate(g_body, (int)g_bodyLen, &cand, why, (int)sizeof why)) {
        char out[160];
        snprintf(out, sizeof out, "{\"ok\":false,\"errors\":[{\"field\":\"document\",\"msg\":\"%s\"}]}", why);
        return sendJson(r, 400, out);
    }
    if (!cfgstore::configQueuePush(cand)) return sendJson(r, 503, "{\"ok\":false,\"errors\":[{\"field\":\"queue\",\"msg\":\"dive loop queue full or absent\"}]}");
    respondApplyResult(r);
}

void hModePost(AsyncWebServerRequest* r) {
    long v = -1;
    if (!bodyGetInt(g_body, "mode", &v) || (v != 0 && v != 1))
        return sendJson(r, 400, "{\"ok\":false,\"errors\":[{\"field\":\"mode\",\"msg\":\"mode must be 0 (CCR) or 1 (OC)\"}]}");
    cfg::RuntimeConfig cand;
    if (!cfgstore::copyWebMirror(&cand)) return sendJson(r, 503, "{\"ok\":false,\"errors\":[{\"field\":\"mirror\",\"msg\":\"no config mirror yet\"}]}");
    cand.mode = (uint8_t)v;
    if (!cfgstore::configQueuePush(cand)) return sendJson(r, 503, "{\"ok\":false,\"errors\":[{\"field\":\"queue\",\"msg\":\"dive loop queue full\"}]}");
    respondApplyResult(r);
}

void hGasPost(AsyncWebServerRequest* r) {
    long slot = -1;
    if (!bodyGetInt(g_body, "slot", &slot) || slot < 0 || slot >= cfg::kGasCount)
        return sendJson(r, 400, "{\"ok\":false,\"errors\":[{\"field\":\"slot\",\"msg\":\"slot out of range\"}]}");
    cfg::RuntimeConfig cand;
    if (!cfgstore::copyWebMirror(&cand)) return sendJson(r, 503, "{\"ok\":false,\"errors\":[{\"field\":\"mirror\",\"msg\":\"no config mirror yet\"}]}");
    if (cand.gases[slot].name[0] == '\0')
        return sendJson(r, 400, "{\"ok\":false,\"errors\":[{\"field\":\"slot\",\"msg\":\"that gas slot is not configured\"}]}");
    cand.selectedGas = (int)slot;
    if (!cfgstore::configQueuePush(cand)) return sendJson(r, 503, "{\"ok\":false,\"errors\":[{\"field\":\"queue\",\"msg\":\"dive loop queue full\"}]}");
    respondApplyResult(r);
}

void hTimePost(AsyncWebServerRequest* r) {
    long v = -1;
    if (!bodyGetInt(g_body, "unixS", &v) || v < 1000000000L)
        return sendJson(r, 400, "{\"ok\":false,\"errors\":[{\"field\":\"unixS\",\"msg\":\"unix seconds missing or implausible\"}]}");
    cfgstore::requestWallClock((uint32_t)v);   // RAM only: no flash write per page load
    char out[64];
    snprintf(out, sizeof out, "{\"ok\":true,\"unixS\":%ld,\"persisted\":\"at next config save\"}", v);
    sendJson(r, 200, out);
}

void hHealthGet(AsyncWebServerRequest* r) {
    static char buf[3584];
    if (!g_rep) return sendJson(r, 503, "{\"error\":\"no boot report\"}");
    const int n = g_rep->toJson(buf, (int)sizeof buf - 320);
    if (n < 0) return sendJson(r, 500, "{\"error\":\"fault list over its buffer — bench 'p'\"}");
    char ap[24];
    apSsid(ap, sizeof ap);
    // Append the radio + build block inside the object: the page's System tab is one fetch.
    snprintf(buf + n - 1, sizeof buf - (size_t)(n - 1), ",\"wifi\":{\"on\":%s,\"ap\":\"%s\",\"stations\":%d,\"ip\":\"%s\"},\"fw\":\"%s %s\"}",
             g_running ? "true" : "false", ap, stationCount(),
             g_running ? WiFi.softAPIP().toString().c_str() : "-", __DATE__, __TIME__);
    sendJson(r, 200, buf);
}

void hFaultsGet(AsyncWebServerRequest* r) {
    static char buf[3584];
    if (!g_rep) return sendJson(r, 503, "{\"error\":\"no boot report\"}");
    const int n = g_rep->toJson(buf, (int)sizeof buf);
    if (n < 0) return sendJson(r, 500, "{\"error\":\"fault list over its buffer — bench 'p'\"}");
    sendJson(r, 200, buf);
}

void hFieldsGet(AsyncWebServerRequest* r) {
    static disp::LiveState st;
    static char json[web::kFieldsJsonCap];
    if (!g_snapQ || xQueuePeek(g_snapQ, &st, 0) != pdTRUE)
        return sendJson(r, 503, "{\"error\":\"no snapshot published yet\"}");
    const int n = web::fieldsToJson(st, json, (int)sizeof json);
    if (n < 0) return sendJson(r, 500, "{\"error\":\"fields frame overflow\"}");
    sendJson(r, 200, json);
}

// ----------------------------------------------------------- air cal (§6) + sim (§11)

void hCalAir(AsyncWebServerRequest* r) {
    const String url = r->url();
    if (url == "/api/cal/air/status" && r->method() == HTTP_GET) {
        static char out[320];
        snprintf(out, sizeof out, "{\"state\":%d,\"tS\":%d,\"sigmaMv\":%.3f,\"passMask\":%d,"
                                  "\"slopes\":[%.2f,%.2f,%.2f],\"msg\":\"%s\"}",
                 g_cal.state, g_cal.tS, (double)g_cal.sigmaMv, g_cal.extra,
                 (double)g_cal.slopes[0], (double)g_cal.slopes[1], (double)g_cal.slopes[2], g_cal.msg);
        return sendJson(r, 200, out);
    }
    if (url == "/api/cal/air" && r->method() == HTTP_POST) {
        if (g_cal.state == 1) return sendJson(r, 409, "{\"error\":\"air cal already running\"}");
        requestCalStart();   // preconditions (baro 900-1100 mbar) are checked by the dive path at start
        return sendJson(r, 202, "{\"state\":\"requested\",\"flow\":\"60 s sampling, sigma < 1 mV gate\"}");
    }
    sendJson(r, 405, "{\"error\":\"use POST /api/cal/air or GET /api/cal/air/status\"}");
}

void hSim(AsyncWebServerRequest* r) {
    const String url = r->url();
    if (url == "/api/sim/status" && r->method() == HTTP_GET) {
        static char out[640];
        const char* verify = (g_sim.state >= 2 && g_simVerify[0]) ? g_simVerify : "";
        snprintf(out, sizeof out, "{\"state\":%d,\"tS\":%d,\"totalS\":%d,\"profile\":\"30m/8min CCR air + diluent flush\"%s%s}",
                 g_sim.state, g_sim.tS, g_sim.extra,
                 verify[0] ? ",\"verify\":" : "", verify);
        return sendJson(r, 200, out);
    }
    if (url == "/api/sim/run" && r->method() == HTTP_POST) {
        if (g_sim.state == 1) return sendJson(r, 409, "{\"error\":\"sim run already active\"}");
        requestSimStart();
        return sendJson(r, 202, "{\"state\":\"requested\",\"note\":\"SIM DATA, stamped in every WS frame (risk 11)\"}");
    }
    if (url == "/api/sim/stop" && r->method() == HTTP_POST) {
        if (g_sim.state != 1) return sendJson(r, 409, "{\"error\":\"no sim run active\"}");
        requestSimStop();
        return sendJson(r, 202, "{\"state\":\"stopping\"}");
    }
    sendJson(r, 405, "{\"error\":\"POST /api/sim/run | POST /api/sim/stop | GET /api/sim/status\"}");
}

// ----------------------------------------------------------- dive log (§10)

void hDives(AsyncWebServerRequest* r) {
    char tail[16] = "";
    uriTail(r->url(), "/api/dives/", tail, sizeof tail);
    if (r->url() == "/api/dives" || (tail[0] == '\0' && r->method() == HTTP_GET)) {
        static char out[1024];
        const int n = divelog::listJson(out, (int)sizeof out);
        if (n < 0) return sendJson(r, 503, "{\"error\":\"dive index unavailable (LittleFS down?)\"}");
        return sendJson(r, 200, out);
    }
    int id = 0;
    char ext[8] = "";
    if (sscanf(tail, "%d%7s", &id, ext) < 1) return sendJson(r, 400, "{\"error\":\"dive id missing\"}");
    if (strcmp(ext, ".csv") == 0) {
        char path[32];
        if (!divelog::csvPath(id, path, (int)sizeof path)) return sendJson(r, 404, "{\"error\":\"no such CSV\"}");
        return r->send(LittleFS, path, "text/csv");
    }
    if (ext[0] != '\0') return sendJson(r, 400, "{\"error\":\"use /api/dives/{id} or /api/dives/{id}.csv\"}");
    if (r->method() == HTTP_DELETE) {
        // FS write from the radio task is allowed ONLY with no dive open (writers otherwise
        // stay on loopTask; esp_littlefs serializes each operation — see header).
        if (divelog::isOpen()) return sendJson(r, 409, "{\"error\":\"a dive is open — cannot delete mid-dive\"}");
        const bool ok = divelog::removeDive(id);
        return sendJson(r, ok ? 200 : 404, ok ? "{\"ok\":true}" : "{\"error\":\"no such dive\"}");
    }
    static char hdr[1024];
    if (!divelog::readHeader(id, hdr, (int)sizeof hdr)) return sendJson(r, 404, "{\"error\":\"no such dive header\"}");
    sendJson(r, 200, hdr);
}

// ----------------------------------------------------------- display templates (§12)

const char* bundleText(const char* name) {
    if (!strcmp(name, "default_128x64")) return disp::bundles::default_128x64();
    if (!strcmp(name, "compact_128x32")) return disp::bundles::compact_128x32();
    if (!strcmp(name, "bench_128x64")) return disp::bundles::bench_128x64();
    return nullptr;   // fitdemo_9on8 / invalid_missing_safety are bench gate demos, not dive layouts

}

void hTemplates(AsyncWebServerRequest* r) {
    char tail[24] = "";
    uriTail(r->url(), "/api/display/templates/", tail, sizeof tail);
    const String url = r->url();

    if (tail[0] == '\0') {
        if (r->method() == HTTP_GET) {
            static char out[1024];
            int k = snprintf(out, sizeof out, "{\"active\":\"%s\",\"templates\":["
                                              "{\"name\":\"default_128x64\",\"origin\":\"bundled\"},"
                                              "{\"name\":\"compact_128x32\",\"origin\":\"bundled\"},"
                                              "{\"name\":\"bench_128x64\",\"origin\":\"bundled\"}",
                             activeTemplate());
            if (LittleFS.exists("/display/templates")) {
                File dir = LittleFS.open("/display/templates");
                if (dir) {
                    File f = dir.openNextFile();
                    while (f) {
                        const char* fn = f.name();   // e.g. "/display/templates/mine.txt"
                        const char* base = strrchr(fn, '/');
                        base = base ? base + 1 : fn;
                        char nm[32];
                        snprintf(nm, sizeof nm, "%s", base);
                        char* dot = strstr(nm, ".txt");
                        if (dot) *dot = '\0';
                        if (nameOk(nm)) {
                            const int w = snprintf(out + k, sizeof out - (size_t)k,
                                                   ",{\"name\":\"%s\",\"origin\":\"custom\"}", nm);
                            if (w > 0) k += w;
                        }
                        f = dir.openNextFile();
                    }
                }
            }
            snprintf(out + k, sizeof out - (size_t)k, "]}");
            return sendJson(r, 200, out);
        }
        return sendJson(r, 405, "{\"error\":\"use /api/display/templates/<name>\"}");
    }

    if (!nameOk(tail)) return sendJson(r, 400, "{\"error\":\"template name: [A-Za-z0-9_]{1,23}\"}");

    if (r->method() == HTTP_GET) {
        if (const char* t = bundleText(tail)) return r->send(200, "text/plain", t);
        char path[64];
        snprintf(path, sizeof path, "/display/templates/%s.txt", tail);
        static char txt[kTemplateCap + 8];
        if (!LittleFS.exists(path)) return sendJson(r, 404, "{\"error\":\"no such template\"}");
        File f = LittleFS.open(path, "r");
        if (!f) return sendJson(r, 500, "{\"error\":\"template unreadable\"}");
        const int n = f.readBytes(txt, (int)sizeof txt - 1);
        f.close();
        if (n <= 0) return sendJson(r, 500, "{\"error\":\"template empty\"}");
        txt[n] = '\0';
        return r->send(200, "text/plain", txt);
    }

    if (r->method() == HTTP_POST || r->method() == HTTP_PUT) {
        if (g_tmplPending) return sendJson(r, 409, "{\"ok\":false,\"error\":\"a template save is already in flight\"}");
        if (g_bodyOverflow || g_bodyLen == 0) return sendJson(r, 400, "{\"ok\":false,\"error\":\"empty or oversized body (max 1152 B)\"}");
        if (!requestTemplateSave(tail, g_body, (int)g_bodyLen))
            return sendJson(r, 400, "{\"ok\":false,\"error\":\"name rejected or save busy\"}");
        bool ok = false;
        char msg[128] = "";
        if (!templateSaveAwait(&ok, msg, (int)sizeof msg, 2000))
            return sendJson(r, 202, "{\"ok\":false,\"queued\":true,\"note\":\"verdict pending — re-read the list\"}");
        char out[200];
        snprintf(out, sizeof out, "{\"ok\":%s,\"msg\":\"%s\"}", ok ? "true" : "false", msg);
        return sendJson(r, ok ? 200 : 400, out);
    }
    sendJson(r, 405, "{\"error\":\"GET | PUT\"}");
}

void hPreview(AsyncWebServerRequest* r) {
    const String name = r->arg("template");
    const String res = r->arg("res");
    int w = 128, h = 64;
    if (res.length()) {
        if (sscanf(res.c_str(), "%dx%d", &w, &h) != 2)
            return sendJson(r, 400, "{\"error\":\"res is WIDTHxHEIGHT (128x64 or 128x32)\"}");
    }
    char nm[24];
    snprintf(nm, sizeof nm, "%s", name.c_str());
    if (!requestPreview(nm, w, h)) {
        if (!nameOk(nm)) return sendJson(r, 400, "{\"error\":\"bad template name\"}");
        return sendJson(r, 400, "{\"error\":\"no framebuffer for that size (128x64/128x32 only — SPI panel is open items 6/15)\"}");
    }
    static char out[2200];
    int len = -1;
    if (!previewAwait(out, (int)sizeof out, &len, 800))
        return sendJson(r, 503, "{\"error\":\"renderer did not answer in 800 ms\"}");
    if (len < 0) return sendJson(r, 422, "{\"error\":\"preview failed — parse/validate/fit rejection; see msg\"}");
    sendJson(r, 200, out);
}

void hNotFound(AsyncWebServerRequest* r) {
    if (r->url() == "/" || r->url() == "/index.html") {
        r->send(503, "text/html",
                "<!doctype html><meta charset=utf-8><title>DiveCCR</title>"
                "<h3>Web assets are not on the flash.</h3>"
                "<p>The ESP32 serves its own page (no CDN, by design). Upload them: "
                "<code>pio run -t uploadfs</code>, then reload. Sensor data is still live: "
                "<a href=/api/health>/api/health</a>, <a href=/api/state>/api/state</a>.</p>");
        return;
    }
    r->send(404, "application/json", "{\"error\":\"not found\"}");
}

void wsEvent(AsyncWebSocket* /*ws*/, AsyncWebSocketClient* client, AwsEventType type,
             void* /*arg*/, uint8_t* /*data*/, size_t /*len*/) {
    // The only client-visible action is the ticker push in snapshotPublish(); a connect
    // gets its first frame within one 500 ms tick. Client->server messages are unused:
    // the page must never be able to ask the dive path for anything extra (§4).
    (void)client;
    (void)type;
}

void registerHandlers() {
    if (g_handlersUp) return;
    g_server.on("/api/state", HTTP_GET, hState);
    g_server.on("/api/config", HTTP_GET, hConfigGet);
    g_server.on("/api/config", HTTP_POST, hConfigPost, nullptr, onBody);
    g_server.on("/api/mode", HTTP_POST, hModePost, nullptr, onBody);
    g_server.on("/api/gas", HTTP_POST, hGasPost, nullptr, onBody);
    g_server.on("/api/time", HTTP_POST, hTimePost, nullptr, onBody);
    g_server.on("/api/health", HTTP_GET, hHealthGet);
    g_server.on("/api/faults", HTTP_GET, hFaultsGet);
    g_server.on("/api/fields", HTTP_GET, hFieldsGet);
    g_server.on("/api/cal/air", HTTP_ALL, hCalAir);
    g_server.on("/api/sim", HTTP_ALL, hSim);
    g_server.on("/api/dives", HTTP_ALL, hDives);
    g_server.on("/api/display/templates", HTTP_ALL, hTemplates);
    g_server.on("/api/display/preview", HTTP_GET, hPreview);
    g_server.on("/api/solenoid/test", HTTP_POST, [](AsyncWebServerRequest* r) {
        // M5 is not built and the valve is not purchased (open items 1-4): refuse loudly
        // with the reason instead of pretending a GPIO toggle is a valve test.
        r->send(501, "application/json",
                "{\"error\":\"solenoid driver (M5) not built; valve not fitted/purchased — see AGENTS open items 1-4\"}");
    });
    g_server.on("/api/factory-reset", HTTP_POST, [](AsyncWebServerRequest* r) {
        if (!bodyConfirm(g_body))
            return sendJson(r, 400, "{\"error\":\"this deletes both config stores. Send {\\\"confirm\\\":true}\"}");
        requestFactoryReset();
        sendJson(r, 202, "{\"state\":\"requested — applied by the dive loop\"}");
    }, nullptr, onBody);

    g_ws.onEvent(wsEvent);
    g_server.addHandler(&g_ws);
    g_server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    g_server.onNotFound(hNotFound);
    g_handlersUp = true;
}

}  // namespace

// ============================================================ lifecycle

bool start(boot::Report& rep) {
    if (g_running) return true;
    g_rep = &rep;
    if (!g_snapQ) g_snapQ = xQueueCreate(1, sizeof(disp::LiveState));
    if (!g_tmplDone) g_tmplDone = xSemaphoreCreateBinary();
    if (!g_prevDone) g_prevDone = xSemaphoreCreateBinary();
    if (!g_snapQ || !g_tmplDone || !g_prevDone) {
        Serial.println(F("[web] FAILED: snapshot queue / rendezvous semaphores"));
        return false;
    }
    registerHandlers();

    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    snprintf(g_ssid, sizeof g_ssid, "DiveCCR-%02X%02X", mac[4], mac[5]);

    WiFi.persistent(false);   // no flash writes from radio events (flash endurance, §9)
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(g_ssid, kApPass)) {
        Serial.println(F("[web] FAILED: softAP would not start — network is OFF, dive functions unaffected"));
        WiFi.mode(WIFI_OFF);
        return false;
    }
    if (MDNS.begin(kMdnsHost)) MDNS.addService("http", "tcp", 80);
    else Serial.println(F("[web] mDNS did not start — use the AP IP directly"));
    g_server.begin();
    g_running = true;
    Serial.printf("[web] AP %s  pass %s  http://%s.local  ip %s  (WiFi OFF during dives unless config says otherwise)\n",
                  g_ssid, kApPass, kMdnsHost, WiFi.softAPIP().toString().c_str());
    return true;
}

void stop() {
    if (!g_running) return;
    g_server.end();
    MDNS.end();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    g_running = false;
}

bool running() { return g_running; }

void apSsid(char* out, int cap) { snprintf(out, (size_t)cap, "%s", g_ssid[0] ? g_ssid : "-"); }
int stationCount() { return g_running ? WiFi.softAPgetStationNum() : 0; }

}  // namespace webnet
