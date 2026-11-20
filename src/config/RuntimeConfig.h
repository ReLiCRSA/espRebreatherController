// RuntimeConfig persistence — §9: atomic tmp+rename on LittleFS, mirrored to NVS, with a
// versioned schema. The schema/validation/migration itself lives in lib/core (pinned by
// test/test_config/); this file is the store layer: trust rules between the two mirrors,
// write-verify (a config write that is not read back is a write assumed, and this project
// has been burned by assumed writes twice — INA219 CONFIG, LPS28 FS_MODE), and the fault
// codes CONFIG_CORRUPT / CONFIG_WRITE_FAILED.
//
// Store trust rules (decided here, stated plainly):
//   - neither store holds a config            -> factory defaults, NO fault (fresh board)
//   - both hold identical valid configs       -> load it, no fault
//   - both valid but DIFFERENT                -> load LittleFS (canonical), raise CONFIG_CORRUPT
//   - one valid, the other corrupt or missing -> load the valid one, raise CONFIG_CORRUPT,
//                                                attempt a repair re-write
//   - both corrupt                            -> factory defaults, raise CONFIG_CORRUPT
//   - a store that is DOWN (mount/NVS open failed) is not "missing": its fault (LITTLEFS /
//     NVS) already says out loud that the unit runs single-copy; that alone does not raise
//     CONFIG_CORRUPT and does not refuse an arm. A config that FAILED to load does.
// CONFIG_CORRUPT clears on a later clean double-write (hot-plug-close pattern: history kept).
//
// Threading (§4): g_cfg is dive-path state — only loopTask reads or writes it. Web sees a
// mutex-guarded mirror published by loopTask after boot and after every apply. Config reads
// and writes are human-rate operations, so a short mutex (not the LiveState queue pattern)
// is the right tool here; the WS/live path never touches it.
#pragma once
#include <Arduino.h>

#include "BootReport.h"
#include "ConfigSchema.h"

namespace cfgstore {

constexpr int kJsonCap = 3072;   // full config JSON ~1.3 KB; also the NVS string budget
                                  // (comfortably under the ~4 KB NVS value ceiling)

extern cfg::RuntimeConfig g_cfg;  // loopTask-owned live settings

// Read both stores, apply the trust rules, select, repair if possible. `fsMounted` is the
// caller's LittleFS state (setup() mounts and raises LITTLEFS itself). Returns true when a
// stored config was loaded, false when running on factory defaults.
bool loadAtBoot(boot::Report& rep, uint32_t nowMs, bool fsMounted);

// Validate candidate (§9 server-side gate). If valid: persist to both live stores with
// read-back verification; on success g_cfg becomes the candidate and the config faults
// clear. On partial or total persistence failure the candidate still goes live IN RAM
// (so the diver sees what they typed) but CONFIG_WRITE_FAILED is raised and blocks
// arming — the unit must not dive on settings that differ from what persisted.
// Returns:  1 applied+saved, 0 validation rejected (errors filled, nothing persisted),
//          -1 applied live but persistence failed.
int applyAndSave(boot::Report& rep, uint32_t nowMs,
                 const cfg::RuntimeConfig& candidate, cfg::ConfigErrors& errors);

// Publish g_cfg to the web mirror. loopTask calls this after boot and after every apply.
void publishWebMirror();

// Copy the web mirror into `out` (web thread: POST /api/mode and /api/gas edit ONE field
// of the current config and push the whole candidate through the same validated queue —
// there is no second, thinner write path).
bool copyWebMirror(cfg::RuntimeConfig* out);

// Parse a JSON config document into a candidate (defaults fill absent fields — that IS
// the migration model — and cfg::configValidate stays the caller's next step). This uses
// the store's shared JsonDoc scratch: its only other user is loadAtBoot, which completes
// strictly before the web server starts, and applyAndSave never touches it — so after
// boot the async_tcp task is the sole user and no lock is needed. Stated because a
// second 9 KB JsonDoc is not a DRAM luxury this build has.
bool parseCandidate(const char* json, int len, cfg::RuntimeConfig* into,
                    char* why, int whyLen);

// Serialize the web mirror (NOT g_cfg) for GET /api/config. Safe from any task.
int configJsonForWeb(char* buf, int cap);

// --- §4 queue discipline: config edits arrive on a queue and are applied by the
// dive-path task (loopTask today) at a safe point — never inside a web callback.
// POST /api/config serialises itself (one edit in flight), pushes the parsed+validated
// candidate, and waits briefly for the apply result so the HTTP reply can report the
// TRUE persistence outcome, not just "queued".

bool configQueueInit();                        // call once from setup(), before web starts
bool configQueuePush(const cfg::RuntimeConfig& candidate);   // web thread
// loopTask, every service window: -1 = nothing queued, else applyAndSave's code.
int serviceConfigQueue(boot::Report& rep, uint32_t nowMs);
// Rendezvous for the waiting POST: blocks up to `waitMs` for the apply to run.
// Returns false on timeout (the edit may still land — the fault banner is the backstop).
bool configAwaitApply(int* code, uint32_t waitMs);

// Field errors from the most recent apply attempt (validation rejected: code 0). Read by
// the web thread after configAwaitApply to build the 400 reply. Safe because exactly one
// edit is in flight at a time (the queue is 2 deep and the POST waits for its result).
bool configLastErrors(cfg::ConfigErrors* out);

// Wall clock (§9 POST /api/time): the ESP32 classic has no RTC, so the phone sets it.
// Web thread calls requestWallClock(); loopTask applies it to g_cfg in the next service
// window. RAM only — a page load must not burn a flash write. It reaches the stores with
// the next config save, which is what makes the cal-age gate survive a reboot.
void requestWallClock(uint32_t unixS);
bool serviceWallClock(uint32_t* unixS);   // loopTask; true when a request was pending

// Factory reset (§9 route): removes the config from BOTH stores and puts factory defaults
// live. It does NOT re-write the defaults back: after a wipe the board is in the same
// state as a fresh board (no stored config, no fault). loopTask only. Returns false when
// a store still holds a config afterwards — which raises CONFIG_WRITE_FAILED, because
// "defaults live, old settings persisted" is exactly the divergence that code exists for.
bool factoryWipe(boot::Report& rep, uint32_t nowMs);

// Bench 'k' evidence: re-read BOTH stores and report them side by side without touching
// g_cfg. This is the acceptance path for "double-written and read-back verified" — the claim
// is only worth anything if it can be inspected, and a store that is DOWN is reported as
// down, never as empty: "absent" and "could not look" are different facts.
// Per-store state: 0 absent, 1 present and parses, 2 present but untrusted (unreadable,
// oversize, syntax error, or schema newer than this build), 3 store not openable.
struct StoreState {
    int  fs = 0, nvs = 0;
    int  fsSchema = 0, nvsSchema = 0;
    size_t fsBytes = 0, nvsBytes = 0;
    bool identical = false;         // byte-identical text in both stores
    bool fsMounted = false, nvsOpenable = false;
};
void storeInspect(StoreState* out);

}  // namespace cfgstore
