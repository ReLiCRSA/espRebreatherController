// Dive-log ring on LittleFS (§10): exactly 5 dives, files /dives/NNN.json (header) and
// /dives/NNN.csv (60 s samples, the 23-column row pinned by test/test_webjson), oldest
// evicted, plus a plain-text index so listing never has to open five JSON files.
//
// Threading (§4): ALL writes here run on loopTask (dive lifecycle + bench 'j'). The web
// task only reads: file listing, header GET, CSV send, DELETE. DELETE is refused while a
// dive is open. LittleFS itself serializes every operation under a per-partition
// recursive mutex — joltwallet/esp_littlefs esp_littlefs.c, sem_take()/sem_give() around
// the file operations (read at source 2026-11-21, not remembered: risk 21/37 rule) — so
// concurrent read-while-append cannot corrupt the volume; it can only show a diver a
// partial last row of an in-progress dive, which is stated rather than papered over.
//
// The header JSON is written at dive START (so an interrupted dive still exists) and
// rewritten at END with the final stats — tmp+rename like the config store, because a
// half-written header is a dive that silently reports wrong deco time.
#pragma once
#include <Arduino.h>

#include "BootReport.h"
#include "DiveSchema.h"

namespace divelog {

constexpr int kRing = 5;

// Ensure /dives + index exist. LittleFS mount state is the caller's (setup() raises
// LITTLEFS itself); when the FS is down every operation here fails quietly-by-absence
// and the fault banner already says the FS is missing.
bool begin(boot::Report& rep, uint32_t nowMs);

// --- dive lifecycle: loopTask only -----------------------------------------------------
bool isOpen();
int  openId();                       // -1 when no dive open
bool startDive(const dlog::Header& hStart);   // allocates a slot, evicts oldest if ring full
bool appendSample(const dlog::Sample& s);    // appends to the open dive's CSV
void endDive(const dlog::Header& hFinal);    // rewrites the header JSON, closes the CSV

// --- read-only surface (web task + bench) ----------------------------------------------
int  listJson(char* buf, int cap);   // GET /api/dives
bool readHeader(int id, char* buf, int cap);  // file contents are already the headerToJson form
bool csvPath(int id, char* out, int cap);     // for request->send(LittleFS, path)
bool removeDive(int id);             // web thread only when !isOpen() (see header note)

}  // namespace divelog
