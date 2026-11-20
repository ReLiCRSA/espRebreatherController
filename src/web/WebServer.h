// M6 web server (§9): SoftAP + mDNS + REST + WebSocket on the ESP32.
//
// Threading (§4): the HTTP/WS callbacks run on the AsyncTCP service task — installed
// here unpinned (CONFIG_ASYNC_TCP_RUNNING_CORE default -1 = any core, priority 10,
// measured by reading .pio/libdeps/esp32/AsyncTCP/src/AsyncTCP.h 2026-11-21, not
// remembered). §4's "web on core 1, dive loop core 0" split is NOT claimed until the
// dive task exists (M5+): today loopTask is the dive path and the radio task may land
// on either core. Stated rather than assumed. What IS enforced here:
//   - the web task NEVER mutates dive state. Snapshots arrive from loopTask through a
//     1-deep xQueueOverwrite queue (§4's transport); config edits cross a queue and are
//     applied by loopTask (cfgstore); template save, preview, air-cal and sim runs are
//     request flags loopTask services. The rendezvous calls below are named by side.
//   - the web task cannot arm the unit: there is no /api/arm route at all (§13).
//
// LittleFS reads from this task (static assets, dive CSVs) while loopTask writes:
// esp_littlefs guards every operation with a per-partition recursive mutex (read at
// source, joltwallet/esp_littlefs esp_littlefs.c sem_take/sem_give) — operation-level
// interleaving is safe; all WRITERS stay on loopTask except dive DELETE, which is
// refused while a dive is open.
#pragma once
#include <Arduino.h>

#include "BootReport.h"
#include "LiveState.h"

namespace webnet {

// Template bodies are bounded: a 128x64 page is ~8 lines x ~21 chars, and the engine's own
// parse capacities are of that order. 1152 B is the hard limit a PUT body may carry; the
// loopTask validator then rejects anything that does not parse, whatever its size.
constexpr int kTemplateCap = 1152;

// ---- lifecycle ---------------------------------------------------------------------
// Boot is never fatal: failure returns false, prints why, and bench 'n' reports it.
// (No new fault code: a dead radio costs no dive function, and inventing a code that
// blocks nothing is noise on the banner — §13 severity discipline.)
bool start(boot::Report& rep);
void stop();                          // §9 wifiDuringDive=false path; start() again after
bool running();
void apSsid(char* out, int cap);
int  stationCount();

// ---- loopTask -> web ---------------------------------------------------------------
// Publish the immutable snapshot. Also the WS ticker: pushes at ~2 Hz (§9) to connected
// clients, SIM-stamped inside the payload (risk 11 — the stamp is in the frame).
void snapshotPublish(const disp::LiveState& s, uint32_t tMs);

// ---- web -> loopTask requests (flag + pull pattern; loopTask owns the work) --------
// Air calibration (§6 flow, 60 s): POST sets the request; loopTask checks preconditions
// at start (baro present, 900-1100 mbar) and runs the sampling. status is human-rate;
// scalar tearing while running is cosmetic and stated.
void requestCalStart();
bool takeCalStartRequest();                       // loopTask
void setCalStatus(int state, int tS, float sigmaMv, int passMask,
                  const float slopes[3], const char* msg);   // loopTask
// states: 0 idle, 1 running, 2 pass, 3 fail/refused

// Simulator (§11): fixed-profile run driven in slices by loopTask; the web stream
// carries the SIM stamp from the payload itself.
void requestSimStart();
bool takeSimStartRequest();
void requestSimStop();
bool simStopRequested();
void clearSimStopRequest();
void setSimStatus(int state, int tS, int totalS);          // loopTask
void setSimVerify(const char* json);                       // loopTask, once at completion
// states: 0 idle, 1 running, 2 done, 3 stopped

// Template save: text lands in one static here; loopTask validates with the SINGLE
// g_tmpl scratch and writes LittleFS (decision 40 rule 2 — a second Template would
// cost 18 KB DRAM). POST waits for the verdict so the editor shows the true result.
bool requestTemplateSave(const char* name, const char* text, int len);   // web
bool takeTemplateSave(char* name, int nameCap, char* text, int textCap); // loopTask
void templateSaveDone(bool ok, const char* msg);                         // loopTask
bool templateSaveAwait(bool* ok, char* msg, int cap, uint32_t waitMs);   // web

// Display preview (§12 browser preview): loopTask renders into the shared scratch and
// hands back a hex frame; sizes are capped to what a framebuffer exists for (128x64 /
// 128x32) — bigger panels are the SPI-panel decision (open items 6/15), not a buffer
// to pretend at here.
bool requestPreview(const char* name, int w, int h);                    // web
bool takePreview(char* name, int nameCap, int* w, int* h);             // loopTask
void previewDone(const char* json, int len);                           // loopTask
bool previewAwait(char* json, int cap, int* len, uint32_t waitMs);     // web

// Factory reset (§9 route): loopTask removes the config from BOTH stores, then
// re-loads defaults. Destructive: the POST body must contain {"confirm":true}.
void requestFactoryReset();
bool takeFactoryResetRequest();       // loopTask

// Active display template: loopTask mirrors /display/active.txt here so the web task never
// opens a file, and reads it back for the templates listing.
void setActiveTemplate(const char* name);
const char* activeTemplate();

}  // namespace webnet
