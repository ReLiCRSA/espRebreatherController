#include "dive/DiveLog.h"

#include <LittleFS.h>

#include <string.h>

namespace divelog {

namespace {

constexpr int kIdxCap = 256;      // 5 lines x ~34 chars + slop
constexpr int kLineCap = 40;

bool g_fsUp = false;
static char g_idx[kIdxCap] = "";  // cached index text; writers keep it current
static int g_openId = -1;
static File g_csv;
static int g_samples = 0;

struct IdxLine { int id; uint32_t wallS; int durationS; float maxDepthM; int source; int samples; };

void pathFor(int id, const char* ext, char* out, int cap) {
    snprintf(out, (size_t)cap, "/dives/%03d%s", id, ext);
}

// The index is plain text — grep-able from the bench, and it means a corrupt dive JSON
// cannot hide the fact that a dive slot is occupied (§17: no silent states).
bool loadIndex() {
    if (!g_fsUp) return false;
    g_idx[0] = '\0';
    if (!LittleFS.exists("/dives/index.txt")) return true;   // empty ring is a valid state
    File f = LittleFS.open("/dives/index.txt", "r");
    if (!f) return false;
    int n = f.readBytes(g_idx, kIdxCap - 1);
    f.close();
    if (n < 0) return false;
    g_idx[n] = '\0';
    return true;
}

bool saveIndex() {
    if (!g_fsUp) return false;
    File t = LittleFS.open("/dives/index.txt.tmp", "w");
    if (!t) return false;
    t.print(g_idx);
    t.flush();
    t.close();
    if (LittleFS.exists("/dives/index.txt")) LittleFS.remove("/dives/index.txt");
    return LittleFS.rename("/dives/index.txt.tmp", "/dives/index.txt");
}

int parseIndex(IdxLine* lines, int max) {
    int n = 0;
    const char* p = g_idx;
    while (*p && n < max) {
        IdxLine L{};
        const int used = sscanf(p, "%d,%lu,%d,%f,%d,%d\n",
                                &L.id, (unsigned long*)&L.wallS, &L.durationS,
                                &L.maxDepthM, &L.source, &L.samples);
        if (used < 6) break;
        lines[n++] = L;
        const char* nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return n;
}

bool lineInIndex(int id) {
    IdxLine lines[kRing];
    const int n = parseIndex(lines, kRing);
    for (int i = 0; i < n; i++) if (lines[i].id == id) return true;
    return false;
}

void removeFiles(int id) {
    char p[32];
    pathFor(id, ".json", p, sizeof p);
    if (LittleFS.exists(p)) LittleFS.remove(p);
    pathFor(id, ".csv", p, sizeof p);
    if (LittleFS.exists(p)) LittleFS.remove(p);
}

bool writeHeader(int id, const dlog::Header& h) {
    static char json[1024];
    const int n = dlog::headerToJson(h, json, sizeof json);
    if (n <= 0) return false;                        // header larger than its buffer: refuse
    char p[32];
    pathFor(id, ".json", p, sizeof p);
    char t[40];
    snprintf(t, sizeof t, "%s.tmp", p);
    File f = LittleFS.open(t, "w");
    if (!f) return false;
    f.print(json);
    f.flush();
    f.close();
    if (LittleFS.exists(p)) LittleFS.remove(p);
    return LittleFS.rename(t, p);
}

}  // namespace

bool begin(boot::Report& rep, uint32_t nowMs) {
    (void)rep; (void)nowMs;              // mount fault is raised by setup(); here we only use it
    if (!g_fsUp) return false;
    if (!LittleFS.exists("/dives")) LittleFS.mkdir("/dives");
    if (!LittleFS.exists("/dives")) return false;
    return loadIndex();
}

bool isOpen() { return g_openId >= 0; }
int  openId() { return g_openId; }

bool startDive(const dlog::Header& hStart) {
    if (!g_fsUp || g_openId >= 0) return false;

    IdxLine lines[kRing];
    int n = parseIndex(lines, kRing);
    if (n >= kRing) {                    // ring full: evict the OLDEST (index is oldest-first)
        removeFiles(lines[0].id);
        const char* nl = strchr(g_idx, '\n');
        memmove(g_idx, nl ? nl + 1 : g_idx + strlen(g_idx), strlen(g_idx) + 1);
        n--;
    }
    int id = 1;                          // smallest free slot 1..5
    while (id <= kRing && lineInIndex(id)) id++;
    if (id > kRing) return false;        // unreachable if eviction worked; refuse rather than double-book

    if (!writeHeader(id, hStart)) return false;

    char p[32];
    pathFor(id, ".csv", p, sizeof p);
    g_csv = LittleFS.open(p, "w");       // incremental append for the whole dive (§10):
    if (!g_csv) return false;            // a dive that loses power still keeps every 60 s row
    g_csv.printf("%s\n", dlog::csvHeader());

    char line[kLineCap];
    snprintf(line, sizeof line, "%d,%lu,%d,%.1f,%d,0\n", id, (unsigned long)hStart.startWallS,
             0, (double)hStart.maxDepthM, (int)hStart.source);
    if (strlen(g_idx) + strlen(line) >= (size_t)kIdxCap) { g_csv.close(); return false; }
    strcat(g_idx, line);
    saveIndex();

    g_openId = id;
    g_samples = 0;
    return true;
}

bool appendSample(const dlog::Sample& s) {
    if (g_openId < 0 || !g_csv) return false;
    static char row[256];
    const int n = dlog::csvRow(s, row, sizeof row);
    if (n <= 0) return false;            // a row that does not fit is dropped, and the caller counts it
    g_csv.print(row);
    g_csv.print('\n');
    g_csv.flush();                       // 60 s cadence: flush cost is noise, power-loss coverage is not
    g_samples++;
    return true;
}

void endDive(const dlog::Header& hFinal) {
    if (g_openId < 0) return;
    if (g_csv) g_csv.close();
    g_csv = File();
    writeHeader(g_openId, hFinal);

    // Update the index line for this id with final stats (rewrite whole index — 5 lines).
    IdxLine lines[kRing];
    const int n = parseIndex(lines, kRing);
    char fresh[kIdxCap];
    fresh[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (lines[i].id == g_openId) {
            lines[i].durationS = hFinal.durationS;
            lines[i].maxDepthM = hFinal.maxDepthM;
            lines[i].samples = g_samples;
        }
        char line[kLineCap];
        snprintf(line, sizeof line, "%d,%lu,%d,%.1f,%d,%d\n", lines[i].id,
                 (unsigned long)lines[i].wallS, lines[i].durationS,
                 (double)lines[i].maxDepthM, lines[i].source, lines[i].samples);
        if (strlen(fresh) + strlen(line) < (size_t)kIdxCap) strcat(fresh, line);
    }
    memcpy(g_idx, fresh, kIdxCap);
    saveIndex();
    g_openId = -1;
    g_samples = 0;
}

int listJson(char* buf, int cap) {
    if (!g_fsUp || !buf || cap < 32) return -1;
    IdxLine lines[kRing];
    const int n = parseIndex(lines, kRing);
    int k = snprintf(buf, (size_t)cap, "{\"count\":%d,\"dives\":[", n);
    if (k < 0 || k >= cap) return -1;
    for (int i = 0; i < n; i++) {
        char item[96];
        snprintf(item, sizeof item,
                 "%s{\"id\":%d,\"wallS\":%lu,\"durationS\":%d,\"maxDepthM\":%.1f,\"source\":%d,"
                 "\"samples\":%d,\"open\":%s}",
                 i ? "," : "", lines[i].id, (unsigned long)lines[i].wallS, lines[i].durationS,
                 (double)lines[i].maxDepthM, lines[i].source, lines[i].samples,
                 (lines[i].id == g_openId) ? "true" : "false");
        const int need = (int)strlen(item);
        if (k + need >= cap) return -1;              // complete or nothing, same rule as every writer here
        memcpy(buf + k, item, (size_t)need);
        k += need;
    }
    if (k + 2 >= cap) return -1;
    buf[k++] = ']';
    buf[k++] = '}';
    buf[k] = '\0';
    return k;
}

bool readHeader(int id, char* buf, int cap) {
    if (!g_fsUp) return false;
    char p[32];
    pathFor(id, ".json", p, sizeof p);
    if (!LittleFS.exists(p)) return false;
    File f = LittleFS.open(p, "r");
    if (!f) return false;
    const int n = f.readBytes(buf, cap - 1);
    f.close();
    if (n <= 0) return false;
    buf[n] = '\0';
    return true;
}

bool csvPath(int id, char* out, int cap) {
    if (id < 1 || id > kRing) return false;
    pathFor(id, ".csv", out, cap);
    return g_fsUp && LittleFS.exists(out);
}

bool removeDive(int id) {
    if (g_openId == id) return false;    // cannot delete the dive being written right now
    IdxLine lines[kRing];
    const int n = parseIndex(lines, kRing);
    int k = 0;
    char fresh[kIdxCap];
    fresh[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (lines[i].id == id) continue;
        char line[kLineCap];
        snprintf(line, sizeof line, "%d,%lu,%d,%.1f,%d,%d\n", lines[i].id,
                 (unsigned long)lines[i].wallS, lines[i].durationS,
                 (double)lines[i].maxDepthM, lines[i].source, lines[i].samples);
        if (strlen(fresh) + strlen(line) < (size_t)kIdxCap) strcat(fresh, line);
        k++;
    }
    removeFiles(id);
    memcpy(g_idx, fresh, kIdxCap);
    return saveIndex() && k >= 0;
}

}  // namespace divelog
