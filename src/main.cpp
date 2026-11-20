// ESP32 CCR rebreather dive computer — boot and bench entry point.
//
// Boot is never fatal (AGENTS.md 4, decision 15). Every missing device is recorded and
// reported; nothing here refuses to start. What is fatal is *arming*, and what is fatal
// mid-dive is a sensor dropping out. The default state is BENCH.
//
// Probing discipline, measured on this hardware 2026-11-19 (see sensors/BusProbe.h):
//  - presence = address phase of a zero-length write with STOP. No other transaction shape is
//    both truthful and non-destructive on Arduino core 4.x / IDF 5.5.
//  - boot probes the fitted address table (7 transactions), never a 112-address sweep: a sweep
//    on a port that failed to init emitted ~200 driver error lines and buried the boot banner.
//    The full sweep is an explicit bench command ('s').
//  - the raw four-shape diagnostic *writes* to whatever answers. It is compiled out unless
//    BENCH_RAW_PROBE=1 (Features.h), and must never run on a unit wired for a dive.
//  - presence is not identity: an ACKing 0x48 could be an ADS1015. The ID register is read at boot
//    and on hot-plug for anything that ACKs, and a mismatch raises ADC_PART_WRONG (blocks arming).
#include <Arduino.h>
#include <esp_log.h>
#include <Wire.h>
// perimanGetPinBusType(): which peripheral the core has routed to a pad. This is the observable
// behind decision 26 — the reason a stuck-line check or a re-purposed pin can silently deinit a bus.
#include <esp32-hal-periman.h>

// src/ is on the include path via `-Isrc`, so headers are addressed project-relative.
#include "config/Features.h"
#include "config/Pins.h"
#include "sensors/Ads1115Bus.h"
#include "sensors/BusProbe.h"
#include "sensors/Bmp085.h"
#include "sensors/Ina219.h"
#include "sensors/Lps28dfw.h"
#include "sensors/Ms5837.h"

// Bench path only ('b'): the field-deployed Adafruit implementation of the same Bosch API, used
// as a second compensation implementation cross-checking lib/core/src/BaroBosch.h on the same
// physical module. Never in the dive path — decision 27's rule, applied to the baro.
#include <Adafruit_BMP085.h>

// lib/core is a library directory, so these resolve from lib/core/src.
#include "BootReport.h"
#include "Depth.h"  // learned Psurf, salinity, computed range clamp (M3; pinned by test_depth)

// lib/sim is also a library directory. The headless bench run ('v') drives the same sim stack
// the native test suite exercises (AGENTS.md 11, M1 gate: "sim profile runs headless"); it is
// pure C++ and touches no pin.
#include "SimRunner.h"

namespace {

boot::Report g_faults;

// All addresses are 7-bit, which is what Wire takes. Corrected 2026-11-20 against the datasheets
// themselves — an earlier note here "halved" the quoted bytes (0x77 -> 0x3B, 0x7C -> 0x3E) and
// both results were phantoms no part ever answered at:
//   BMP085/180: Bosch DS Table 7 gives address bits 1110111 + R/W, i.e. **7-bit 0x77**
//     (bus bytes 0xEE write / 0xEF read). There is no 0x3B anywhere.
//   MS5837: TE DS, "Module ref / Mode / Pins used / Address (7 bits): MS5837-07BA I2C SDA, SCL
//     **0x76 (1110110 b)**". The "0x7C" seen in some code is the 8-bit write byte 11101100.
//   LPS28DFW (decision 39 tier-2 fallback): ST quotes bits 101110xb — 7-bit **0x5C** (SA0=0,
//     the Adafruit #6067 default) / **0x5D** (SA0=1 solder jumper); the vendor header's 0xB8/0xB9
//     are the 8-bit bytes for SA0=0. Quoted from the vendor register header, same rule (decision 36).
// Lesson for the README's common-wiring-mistakes list: a datasheet address may be quoted as
// 7-bit, 8-bit, or as command bytes (MS5837's PROM reads are 0xA0..0xAC) — never do address
// arithmetic on them; take the 7-bit form the datasheet states, and confirm with a bus sweep.
struct DevSpec {
    FaultCode code;
    uint8_t addr;
    TwoWire* bus;
    const char* label;
    bool present;
};

DevSpec g_devices[] = {
    {FaultCode::ADS1115_U1, 0x48, &Wire1, "ADS1115 U1", false},
    {FaultCode::ADS1115_U2, 0x49, &Wire1, "ADS1115 U2", false},
    {FaultCode::BARO, 0x76, &Wire, "MS5837", false},
    {FaultCode::BARO, 0x5C, &Wire, "LPS28DFW A", false},
    {FaultCode::BARO, 0x5D, &Wire, "LPS28DFW B", false},
    {FaultCode::BARO, 0x77, &Wire, "BMP085", false},
    {FaultCode::OLED, 0x3C, &Wire, "SSD1306 A", false},
    {FaultCode::OLED, 0x3D, &Wire, "SSD1306 B", false},
    {FaultCode::INA219, 0x40, &Wire, "INA219", false},
};

// The two cell converters get real driver objects; the presence table above only answers "is
// something at that address", which is not the same question (see Ads1115Bus.h).
sensors::Ads1115 g_ads1(Wire1, 0x48, pins::ADS_U1_DRDY);
sensors::Ads1115 g_ads2(Wire1, 0x49, pins::ADS_U2_DRDY);
sensors::Ads1115* g_adcs[] = {&g_ads1, &g_ads2};
constexpr size_t kAdcCount = sizeof(g_adcs) / sizeof(g_adcs[0]);

// Every sensor-class address gets a real driver object, for the same reason (checkBaro/checkPower
// below): the presence table answers "is this address answered", not "does the part there do what
// its label claims" (risk 21). The MS5837 driver and its PROM/CRC math are written and natively
// pinned (test_ms5837), but the PART is unprocured as of 2026-11-20 — its side of the address
// table stays datasheet-derived until a Bar30 is fitted and bench 'b' measures its PROM, its
// conversion period at the design OSR, and a Stockholm-station mbar (decision 29's rule).
sensors::Bmp085 g_bmp085(Wire, 0x77, /*oss=*/2);
sensors::Ms5837 g_ms5837(Wire, ms5837::kAddr7);   // 7-bit form straight from the DS (decision 36)
// Tier-2 fallback (decision 39). UNPROCURED as of 2026-11-20 — written-but-never-measured, like
// the MS5837 side. Address resolves to the strap that ACKs (0x5C/0x5D, SA0 jumper) at check time.
sensors::Lps28dfw g_lps28(Wire, lps28::kAddr7);
sensors::Ina219 g_ina219(Wire, pwr::kAddr7);

// Which barometer the dive path will actually use, established by ACTIVE checks at boot and on
// presence transitions — never by which address ACKed (0x76/0x77 are shared with BMP280/BME280,
// decision 36). Null until a part passes identity + a real conversion.
Barometer* g_activeBaro = nullptr;

// True while a baro-class address ACKed but failed its identity chain this pass (a squatter at
// 0x76/0x5C/0x77). It is what keeps BARO_CONV_BAD raised after a working fallback selects itself:
// the older code cleared CONV_BAD unconditionally on fallback success, erasing the squatter's
// arming block the comment above promises. Selection order (decision 39): MS5837-30BA -> LPS28DFW
// (mode 2) -> BMP085/180 bench-only.
bool g_baroSquatter = false;

// Psurf is LEARNED at the surface (AGENTS.md 2), from the fitted barometer itself: a hard-coded
// 1013.25 mbar is the wrong-city-reference error class (risk 21). Locked by learnSurface() at
// boot; the dive path (M4+) reads g_psurfBar only when locked.
depth::SurfaceLearner g_psurf;
float g_psurfBar = 0.0f;
float g_salinity = 1.025f;  // placeholder until RuntimeConfig (M6); salt default

// The alternating-mux pair walkers (AGENTS.md 2): U1 walks cell 1 (AIN0/1) -> cell 2 (AIN2/3),
// U2 walks cell 3 (AIN0/1) -> coil shunt (AIN2/3). The coil slot reads a floating input until
// M5 fits the shunt stage, but walking it keeps the per-cell rate honest — all four pairs get
// the same rate the converter can actually deliver, which bench 't' measures rather than assumes.
// Cells 1+2 sharing U1 is the documented correlated-failure point (risk 2), not an accident of
// this mapping.
sensors::AdcPairWalker g_walk1(g_ads1, sensors::Ads1115::MUX_AIN0_AIN1,
                               sensors::Ads1115::MUX_AIN2_AIN3, "cell1 U1.AIN0/1",
                               "cell2 U1.AIN2/3");
sensors::AdcPairWalker g_walk2(g_ads2, sensors::Ads1115::MUX_AIN0_AIN1,
                               sensors::Ads1115::MUX_AIN2_AIN3, "cell3 U2.AIN0/1",
                               "coil U2.AIN2/3");
sensors::AdcPairWalker* g_walkers[] = {&g_walk1, &g_walk2};

// Bus init state. A port that failed to initialise must never be probed (its answers are
// meaningless) and must never be reported as "sensors absent" — that would blame the devices
// for the controller's failure (no silent fallbacks, AGENTS.md 17).
struct BusState {
    TwoWire* wire;
    int sda, scl;
    const char* label;
    bool up;
};

BusState g_buses[] = {
    {&Wire, pins::I2C0_SDA, pins::I2C0_SCL, "I2C0", false},
    {&Wire1, pins::I2C1_SDA, pins::I2C1_SCL, "I2C1", false},
};

constexpr size_t kDevCount = sizeof(g_devices) / sizeof(g_devices[0]);

bool busUp(TwoWire* w) {
    for (const BusState& b : g_buses)
        if (b.wire == w) return b.up;
    return false;
}

// The BARO_RANGE banner quotes a COMPUTED ceiling, never a recited one (decision 38: the record's
// "~9 m" for the BMP180 fallback was 10x wrong — 1.1 bar abs minus ~1.013 bar surface is ~87 mbar
// gauge, and 1 mbar supports ~1.02 cm of water, so the real figure is UNDER 1 m at sea level,
// ~1.2 m at the 978 mbar Stockholm bench; test_depth pins the arithmetic). Before Psurf is
// learned there is no measured reference, so the banner says so and uses standard atmosphere —
// flagged "SL ref", because a reference is a measurement with a source (risk 21).
//
// The < 50 m gate is a stated design heuristic, not a datasheet number: the design's primary
// covers ~288 m, and any fitted part whose water column tops out below 50 m is fallback-class
// and must wear the banner. The banner is ERROR-class and does NOT block arming (AGENTS.md 13 —
// the gate is "arm shallow or not at all"); what blocks is BARO_CONV_BAD, raised separately.
float baroRangeBanner(char* detail, size_t cap) {
    if (!g_activeBaro) return -1.0f;
    const bool havePsurf = g_psurf.locked();
    const float refPsurf = havePsurf ? g_psurfBar : 1.01325f;
    const float ceilM = depth::ceilingMeters(refPsurf, g_activeBaro->rangeCeilingBar(), g_salinity);
    if (ceilM >= 50.0f) return ceilM;  // full-range part: no banner owed
    snprintf(detail, cap, "clamps %.1fm%s%.0fmbar", ceilM, havePsurf ? "@" : " SL",
                             refPsurf * 1000.0f);
    return ceilM;
}

void raiseRangeBanner() {
    char detail[32];
    if (baroRangeBanner(detail, sizeof(detail)) >= 0.0f)
        g_faults.raise(FaultCode::BARO_RANGE, Severity::ERROR, millis(), detail);
}

// Decision 39: the BMP085/180 is a BENCH sensor. Bench mode works fully with it and it is a fine
// cross-check, but when it is the only depth source that passed identity, arming is refused —
// its computed water column (banner above: under 1.3 m at this bench) cannot carry a dive. Same
// ERROR class as the range banner, but unlike that one this DOES block: the tier below tier 2
// exists for bring-up, not for depth.
void raiseBaroBenchOnly() {
    g_faults.raise(FaultCode::BARO_BENCH_ONLY, Severity::ERROR, millis(), "BMP180 sole: bench only");
}

void probeDevices() {
    for (DevSpec& d : g_devices) {
        if (!busUp(d.bus)) {
            d.present = false;
            char detail[24];
            snprintf(detail, sizeof(detail), "0x%02X bus dead", d.addr);
            g_faults.raise(d.code, Severity::ERROR, millis(), detail);
            continue;
        }
        const sensors::ProbeResult pr = sensors::probeAddress(*d.bus, d.addr);
        d.present = pr.ok;
        for (sensors::Ads1115* a : g_adcs)
            if (d.bus == &Wire1 && d.addr == a->address()) a->setPresent(pr.ok);
        if (!pr.ok) {
            char detail[24];
            snprintf(detail, sizeof(detail), "0x%02X err %u", d.addr, pr.err);
            g_faults.raise(d.code, millis(), detail);
        }
    }

    // Baro selection, three tiers (decision 39). Identity, never ACK: 0x76 counts as the MS5837
    // primary only after checkBaro()'s PROM+CRC+conversion chain verified it (BMP280/BME280 squat
    // at 0x76/0x77 — decision 36 — and the deployed-library note says the MS5637 shares 0x76 and
    // passes CRC); 0x5C/0x5D counts as the LPS28DFW tier-2 only after checkLps28()'s WHO_AM_I +
    // FS_MODE-latch + real-conversion chain verified it. A part that ACKs but fails identity while
    // a lower tier works gets BOTH banners: the computed range clamp for the tier in use, and
    // BARO_CONV_BAD for the squatter, which blocks arming until it is replaced. This runs before
    // checkBaro() at boot, so the identity flags may be from the last transition — checkBaro()
    // re-derives everything and re-raises the banners after.
    bool ms5837 = false, lps28 = false, bmp085 = false;
    for (const DevSpec& d : g_devices) {
        if (d.code != FaultCode::BARO || !d.present) continue;
        if (d.addr == ms5837::kAddr7 && g_ms5837.identityVerified()) ms5837 = true;
        if ((d.addr == lps28::kAddr7 || d.addr == lps28::kAddr7Alt) && g_lps28.identityVerified())
            lps28 = true;
        if (d.addr == 0x77) bmp085 = true;
    }
    if (ms5837) {
        // Either the MS5837 was verified at boot, or it hot-plugged later: in both cases the
        // fallback clamp and the bench-only refusal no longer apply.
        g_faults.clear(FaultCode::BARO_RANGE, millis());
        g_faults.clear(FaultCode::BARO_BENCH_ONLY, millis());
        g_faults.clear(FaultCode::BARO, millis());
    } else if (lps28) {
        raiseRangeBanner();  // tier-2 ~31 m computed ceiling (decision 39); armable with the banner
        g_faults.clear(FaultCode::BARO_BENCH_ONLY, millis());
        g_faults.clear(FaultCode::BARO, millis());
    } else if (bmp085) {
        raiseRangeBanner();  // computed sub-1 m clamp, decision 38 — never "9 m" again
        // The bench-only refusal is about the SELECTED depth source, so it follows g_activeBaro;
        // checkBaro() re-derives that on every presence transition.
        if (g_activeBaro == &g_bmp085) raiseBaroBenchOnly();
        g_faults.clear(FaultCode::BARO, millis());
    } else {
        g_faults.raise(FaultCode::BARO, Severity::ERROR, millis(), "no MS5837/LPS28/BMP085");
    }

    // Same two-address-one-device shape for the OLED (0x3C, with 0x3D as the alternate strap).
    // Without this the loop above raises OLED from whichever strap did not answer, so a display
    // that is fitted and answering at 0x3C still reports "OLED missing 0x3D err 2" — measured on
    // the bench 2026-11-19, and it would have blocked arming forever against a working screen.
    bool oled = false;
    for (const DevSpec& d : g_devices)
        if (d.code == FaultCode::OLED && d.present) oled = true;
    if (oled) g_faults.clear(FaultCode::OLED, millis());
    else g_faults.raise(FaultCode::OLED, millis(), "no 0x3C/0x3D");
    // The else matters: without it the detail on an absent display is "0x3D err 2" — the strap that
    // happened to be probed last — which reads like a strap-configuration problem and sends someone
    // looking for an address jumper on a unit with no display wired at all.
}

// Presence is not identity. This checks, for every converter that ACKs, that it actually converts
// at the design rate — the measured way to tell an ADS1115 from an ADS1015 (Ads1115Bus.h) — and
// records the ID register as information. It writes the config register, so it runs at boot and on
// a presence transition, not on every 5 s re-probe.
void checkAdcs() {
    for (size_t i = 0; i < kAdcCount; ++i) {
        sensors::Ads1115* adc = g_adcs[i];
        DevSpec* spec = nullptr;
        for (DevSpec& d : g_devices)
            if (d.bus == &Wire1 && d.addr == adc->address()) { spec = &d; break; }
        if (!spec || !spec->present || !busUp(&Wire1)) continue;

        char detail[40];
        uint8_t id = 0;
        const uint8_t idErr = adc->probeId(&id);
        if (!idErr && id != 0x85) {
            // 0x85 is the ADS1115's documented ID. Anything else — including 0x00, which the bench
            // module gives while otherwise behaving like an ADS1115 — means the marked part is not
            // a TI ADS1115 with a complete register map. Warn, do not ground the unit on it: the
            // timing check below is what proves fitness, and a zero ID alone proves nothing.
            snprintf(detail, sizeof(detail), "0x%02X ID 0x%02X not 0x85", spec->addr, id);
            g_faults.raise(FaultCode::ADC_ID_UNKNOWN, Severity::WARN, millis(), detail);
            Serial.printf("  ADS @0x%02X ID 0x%02X — not the documented ADS1115 ID\n", spec->addr, id);
        } else {
            g_faults.clear(FaultCode::ADC_ID_UNKNOWN, millis());
        }

        uint32_t convUs = 0;
        const auto pc = adc->checkPart(&convUs);
        switch (pc) {
            case sensors::Ads1115::PartCheck::OK: {
                g_faults.clear(FaultCode::ADC_PART_WRONG, millis());
                g_faults.clear(FaultCode::ADC_CONV_BAD, millis());
                // Converting at the right *order* of rate is not the same as converting at the
                // designed rate. ±25% of the datasheet period is where the claim stops being true,
                // and the first bench module sits at +98%: it delivers 16.1 SPS for the field the
                // datasheet says is 32 SPS, so the per-cell sample rate and the coil/ADC interlock
                // window have to be re-derived from measurement, not from the datasheet.
                const int pct = adc::rateErrorPercent(convUs, adc::convUs(adc::designConfig()));
                if (pct > 25 || pct < -25) {
                    // Compact: the fault table's detail field is 32 chars, and a clipped detail is
                    // a fault that stops explaining itself.
                    snprintf(detail, sizeof(detail), "0x%02X %lumS DS%umS %d%%", spec->addr,
                             static_cast<unsigned long>(convUs / 1000),
                             static_cast<unsigned int>(adc::convUs(adc::designConfig()) / 1000),
                             pct);
                    g_faults.raise(FaultCode::ADC_RATE_MISMATCH, Severity::WARN, millis(), detail);
                } else {
                    g_faults.clear(FaultCode::ADC_RATE_MISMATCH, millis());
                }
                Serial.printf("  ADS @0x%02X converts: %lu us for the %d SPS field (DS %lu us) = %d%% off\n",
                              spec->addr, static_cast<unsigned long>(convUs),
                              adc::sps(adc::designConfig()),
                              static_cast<unsigned long>(adc::convUs(adc::designConfig())), pct);
                break;
            }
            case sensors::Ads1115::PartCheck::TOO_FAST_OTHER_PART:
                snprintf(detail, sizeof(detail), "0x%02X conv %lu us too fast", spec->addr,
                         static_cast<unsigned long>(convUs));
                g_faults.raise(FaultCode::ADC_CONV_BAD, Severity::ERROR, millis(), detail);
                Serial.printf("  ADS @0x%02X conv %lu us — ** not an ADS1115 rate table **\n",
                              spec->addr, static_cast<unsigned long>(convUs));
                break;
            case sensors::Ads1115::PartCheck::TOO_SLOW:
                // Converting, but not at a rate the control loop and the coil/ADC interlock can be
                // built on. bench command 'd' characterises the whole rate table.
                snprintf(detail, sizeof(detail), "0x%02X conv %lu ms", spec->addr,
                         static_cast<unsigned long>(convUs / 1000));
                g_faults.raise(FaultCode::ADC_CONV_BAD, Severity::ERROR, millis(), detail);
                Serial.printf("  ADS @0x%02X conv %lu us — outside the accepted window, run \'d\'\n",
                              spec->addr, static_cast<unsigned long>(convUs));
                break;
            case sensors::Ads1115::PartCheck::NO_CONVERSION:
            case sensors::Ads1115::PartCheck::IO_FAIL:
                snprintf(detail, sizeof(detail), "0x%02X no conversion", spec->addr);
                g_faults.raise(FaultCode::ADC_CONV_BAD, Severity::ERROR, millis(), detail);
                Serial.printf("  ADS @0x%02X ** not converting **\n", spec->addr);
                break;
        }
    }
}

// Presence is not identity, and this applies at the depth sensor as hard as at the ADCs (risk 21).
// The boot probe says an address is answered; this says whether the part there IS what it claims
// and MEASURES. Three tiers (decision 39): MS5837-30BA primary — identity = factory PROM read +
// CRC-4 verify + C1 band classification + one real D1/D2 pair landing in the surface window
// (900-1100 mbar, the same window as the air-cal gate, AGENTS.md 6). An ACKing 0x76 that fails any
// of that is a squatter (BMP280/BME280 class, decision 36) or a dead PROM: BARO_CONV_BAD, ERROR,
// blocks arming — "something answers at the primary address" is not a depth source.
// LPS28DFW tier 2 (decision 39): WHO_AM_I 0xB4 + SWRESET self-clear + FS_MODE=1 latching on
// readback + one real conversion inside the latched 4.06 bar window — armable, with the computed
// ~31 m clamp banner.
// BMP085/180 tier 3, BENCH ONLY (decision 39): CHIPID + cal ROM + one real conversion (the chain
// that closed item 17). It may be selected on the bench, but selecting it raises BARO_BENCH_ONLY,
// which blocks arming.
// All paths write commands, so this runs at boot and on presence transitions only — never on the
// 5 s re-probe. The 02BA classification is reported and refused for the dive path (a 2-bar die is
// a ~10 m ceiling, the wrong part for this design); it falls through to the lower tiers.
bool checkMs5837() {
    char detail[40];
    if (!g_ms5837.init()) {
        const auto st = g_ms5837.lastStatus();
        const char* why = st == sensors::Ms5837::Status::CRC_FAIL
                              ? "PROM CRC fail"
                              : st == sensors::Ms5837::Status::IO_FAIL ? "IO fail" : "init fail";
        snprintf(detail, sizeof(detail), "0x76 %s", why);
        g_faults.raise(FaultCode::BARO_CONV_BAD, Severity::ERROR, millis(), detail);
        Serial.printf("  BARO @0x76 ** %s — not a CRC-verified MS5837 PROM **\n", why);
        return false;
    }
    const auto model = g_ms5837.model();
    if (model != ms5837::Model::k30BA) {
        const char* m = model == ms5837::Model::k02BA ? "MS5837-02BA (2 bar)" : "C1 outside both bands";
        snprintf(detail, sizeof(detail), "0x76 %s — wrong range", m);
        g_faults.raise(FaultCode::BARO_CONV_BAD, Severity::ERROR, millis(), detail);
        Serial.printf("  BARO @0x76 PROM OK but %s — not the design's 30 mbar-range depth source\n", m);
        return false;
    }
    float ambBar = 0.0f, tempC = 0.0f;
    if (!g_ms5837.read(&ambBar, &tempC)) {
        const auto st = g_ms5837.lastStatus();
        const char* why = st == sensors::Ms5837::Status::ZERO_ADC
                              ? "ADC words 0 (DS: no conversion behind the read)"
                              : "read fail";
        snprintf(detail, sizeof(detail), "0x76 %s", why);
        g_faults.raise(FaultCode::BARO_CONV_BAD, Severity::ERROR, millis(), detail);
        Serial.printf("  BARO @0x76 ** %s — D1=%lu D2=%lu **\n", why,
                      (unsigned long)g_ms5837.lastD1(), (unsigned long)g_ms5837.lastD2());
        return false;
    }
    if (ambBar < 0.90f || ambBar > 1.10f) {
        // Same surface window the air-cal gate uses; the expectation's source is stated: bench at
        // 0-100 m elevation, Stockholm-area SLP 984-987 hPa measured 2026-11-20 (risk 21's
        // place/hour/source rule). Outside it, the part converts to a number nobody should trust.
        snprintf(detail, sizeof(detail), "0x76 surface %.0fmbar odd", ambBar * 1000.0f);
        g_faults.raise(FaultCode::BARO_CONV_BAD, Severity::ERROR, millis(), detail);
        Serial.printf("  BARO @0x76 ** %.1f mbar at the surface — outside 900-1100 **\n",
                      ambBar * 1000.0f);
        return false;
    }
    // The CONV_BAD clear is NOT here: checkBaro() owns it and refuses to clear while any other
    // baro-class address failed identity this pass (the squatter bug — an unconditional clear here
    // used to erase the arming block this function's own failure path raised).
    g_activeBaro = &g_ms5837;
    g_faults.clear(FaultCode::BARO, millis());
    g_faults.clear(FaultCode::BARO_BENCH_ONLY, millis());
    raiseRangeBanner();  // clears itself for a full-range part, re-quotes a computed figure otherwise
    Serial.printf("  BARO @0x76 MS5837-30BA: %.2f mbar  %.2f C  (D1=%lu D2=%lu CRC ok W0=0x%04X "
                  "type=%u) — primary\n",
                  ambBar * 1000.0f, tempC, (unsigned long)g_ms5837.lastD1(),
                  (unsigned long)g_ms5837.lastD2(), g_ms5837.prom().w[0],
                  ms5837::w0TypeBits(g_ms5837.prom().w[0]));
    return true;
}

// Tier-2 identity (decision 39). This silicon has no factory PROM to CRC — that is the part's
// design, not a shortcut — so identity is the active capability chain from BaroLps28.h's header
// comment: WHO_AM_I 0xB4, SWRESET self-clearing, the dual-full-scale write LATCHING (FS_MODE=1 is
// what this part is for; a chip that will not present 4060 hPa must not get a 4.06 bar ceiling),
// and one real conversion inside that latched window. The dive model's clamp comes from the mode
// read back, never from the mode requested. Writes config registers, so boot/transition only.
bool checkLps28() {
    char detail[40];
    if (!g_lps28.init()) {
        const auto st = g_lps28.lastStatus();
        const char* why = st == sensors::Lps28dfw::Status::WRONG_ID     ? "WHO_AM_I != 0xB4"
                        : st == sensors::Lps28dfw::Status::FS_NOT_LATCHED ? "config/FS_MODE did not latch"
                        : st == sensors::Lps28dfw::Status::RESET_STUCK  ? "SWRESET stuck"
                        : st == sensors::Lps28dfw::Status::IO_FAIL      ? "IO fail"
                                                                        : "init fail";
        snprintf(detail, sizeof(detail), "0x%02X %s", g_lps28.address(), why);
        g_faults.raise(FaultCode::BARO_CONV_BAD, Severity::ERROR, millis(), detail);
        Serial.printf("  BARO @0x%02X ** %s — not a verified LPS28DFW **\n", g_lps28.address(), why);
        return false;
    }
    float ambBar = 0.0f, tempC = 0.0f;
    if (!g_lps28.read(&ambBar, &tempC)) {
        const auto st = g_lps28.lastStatus();
        const char* why = st == sensors::Lps28dfw::Status::OUT_OF_RANGE ? "outside latched FS window"
                        : st == sensors::Lps28dfw::Status::NO_DATA      ? "P_DA never set"
                                                                        : "read fail";
        snprintf(detail, sizeof(detail), "0x%02X %s", g_lps28.address(), why);
        g_faults.raise(FaultCode::BARO_CONV_BAD, Severity::ERROR, millis(), detail);
        Serial.printf("  BARO @0x%02X ** %s — raw=0x%08lX **\n", g_lps28.address(), why,
                      static_cast<unsigned long>(g_lps28.lastRawAligned()));
        return false;
    }
    if (ambBar < 0.90f || ambBar > 1.10f) {
        // Same surface window as the tier-1 gate and the air-cal gate: the reference is this bench
        // at this moment (Stockholm 0-100 m, SLP 984-987 hPa band, risk 21), not a remembered
        // 1013. Part converts, but to a number nobody should trust at the surface.
        snprintf(detail, sizeof(detail), "0x%02X surface %.0fmbar odd", g_lps28.address(),
                 ambBar * 1000.0f);
        g_faults.raise(FaultCode::BARO_CONV_BAD, Severity::ERROR, millis(), detail);
        Serial.printf("  BARO @0x%02X ** %.1f mbar at the surface — outside 900-1100 **\n",
                      g_lps28.address(), ambBar * 1000.0f);
        return false;
    }
    g_activeBaro = &g_lps28;
    g_faults.clear(FaultCode::BARO, millis());
    g_faults.clear(FaultCode::BARO_BENCH_ONLY, millis());  // tier 2 is armable; bench-only lifted
    raiseRangeBanner();  // computed ~31 m ceiling at this bench, decision 38's rule
    char banner[32];
    baroRangeBanner(banner, sizeof(banner));
    Serial.printf("  BARO @0x%02X LPS28DFW: %.2f mbar  %.2f C  (WHO=0x%02X CTRL1=0x%02X "
                  "CTRL2=0x%02X raw=0x%08lX) — tier-2 fallback, %s\n",
                  g_lps28.address(), ambBar * 1000.0f, tempC, g_lps28.whoAmI(), g_lps28.lastCtrl1(),
                  g_lps28.lastCtrl2(), static_cast<unsigned long>(g_lps28.lastRawAligned()), banner);
    return true;
}

void selectBaroFallback() {
    g_activeBaro = &g_bmp085;
    raiseRangeBanner();    // computed sub-1 m clamp, decision 38 — never "9 m" again
    raiseBaroBenchOnly();  // decision 39: the bench tier may run the bench, not arm a dive
}

void checkBaro() {
    bool ack76 = false, ack5C = false, ack77 = false;
    uint8_t lpsAddr = lps28::kAddr7;  // which strap ACKed (0x5C/0x5D), for the driver
    for (const DevSpec& d : g_devices) {
        if (d.code != FaultCode::BARO || !d.present || !busUp(&Wire)) continue;
        if (d.addr == ms5837::kAddr7) ack76 = true;
        if (d.addr == lps28::kAddr7 || d.addr == lps28::kAddr7Alt) { ack5C = true; lpsAddr = d.addr; }
        if (d.addr == 0x77) ack77 = true;
    }
    g_activeBaro = nullptr;
    bool squatter = false;  // something ACKed a baro-class address and failed identity this pass

    if (ack76) { if (!checkMs5837()) squatter = true; }
    if (!g_activeBaro && ack5C) {
        g_lps28.setAddress(lpsAddr);
        if (!checkLps28()) squatter = true;
    }

    char detail[40];
    if (!g_activeBaro && ack77) {  // bench tier, or both dive-capable tiers absent/failed identity
        if (!g_bmp085.init()) {
            squatter = true;
            if (g_bmp085.lastStatus() == sensors::Bmp085::Status::CAL_ROM_BAD) {
                snprintf(detail, sizeof(detail), "0x77 cal word %d bad", g_bmp085.calRomBadIndex());
                g_faults.raise(FaultCode::BARO_CONV_BAD, Severity::ERROR, millis(), detail);
                Serial.printf("  BARO @0x77 ** cal ROM fails the DS 0/0xFFFF rule (word %d) **\n",
                              g_bmp085.calRomBadIndex());
            } else {
                g_faults.raise(FaultCode::BARO_CONV_BAD, Severity::ERROR, millis(), "0x77 no CHIPID");
                Serial.println(F("  BARO @0x77 ** D0 != 0x55 — not reading as BMP085/180 **"));
            }
        } else {
            float ambBar = 0.0f, tempC = 0.0f;
            if (!g_bmp085.read(&ambBar, &tempC)) {
                squatter = true;
                const char* why = g_bmp085.lastStatus() == sensors::Bmp085::Status::OUT_OF_RANGE
                                      ? "outside 300-1100 hPa"
                                      : "no conversion";
                snprintf(detail, sizeof(detail), "0x77 %s", why);
                g_faults.raise(FaultCode::BARO_CONV_BAD, Severity::ERROR, millis(), detail);
                Serial.printf("  BARO @0x77 ** %s — raw UT=%u UP=%lu **\n", why, g_bmp085.lastUt(),
                              static_cast<unsigned long>(g_bmp085.lastUp()));
            } else {
                // A real pressure at the bench is the whole point: identity by register read is
                // necessary, not sufficient (item 17 closure). ~1 atm expected indoors; 'b' shows
                // it moving when squeezed. The clamp figure is computed, decision 38.
                selectBaroFallback();  // also raises BARO_BENCH_ONLY (decision 39) — arm refused
                g_faults.clear(FaultCode::BARO, millis());
                char banner[32];
                baroRangeBanner(banner, sizeof(banner));
                Serial.printf("  BARO @0x77 BMP180: %.2f mbar  %.1f C  (UT=%u UP=%lu B5=%ld "
                              "oss=2) — bench-only tier, %s\n",
                              ambBar * 1000.0f, tempC, g_bmp085.lastUt(),
                              static_cast<unsigned long>(g_bmp085.lastUp()),
                              (long)g_bmp085.lastB5(), banner);
            }
        }
    }
    // Squatter bug fix, decision 39 integration: a selected lower tier must NOT erase the
    // CONV_BAD block that a failed identity at a higher tier's address just raised. Old code
    // cleared unconditionally on fallback success, contradicting the comment above it.
    g_baroSquatter = squatter;
    if (g_activeBaro && !squatter) g_faults.clear(FaultCode::BARO_CONV_BAD, millis());
    if (!g_activeBaro) {
        g_faults.clear(FaultCode::BARO_RANGE, millis());      // no depth source at all:
        g_faults.clear(FaultCode::BARO_BENCH_ONLY, millis()); // these banners would blame the wrong part
    }
    // BARO_RANGE would blame the wrong part. The active fault is BARO (none fitted) or
    // BARO_CONV_BAD (something ACKs but cannot be trusted), and both block arming.
}

// The baro that selects itself at presence transitions must also re-learn nothing stale: Psurf
// belongs to a surface moment, so the dive path (M4+) re-arms learning on every descent gate —
// for now learnSurface() runs once at boot and says exactly what it measured.
void learnSurface() {
    if (!g_activeBaro) {
        Serial.println(F("[psurf] no trusted depth source — Psurf NOT learned (depth has no reference)"));
        return;
    }
    depth::SurfaceLearner l;
    float lastAmb = 0.0f, lastAmbAgain = 0.0f, tempC = 0.0f;
    for (int i = 0; i < 24 && !l.locked(); ++i) {
        float a = 0.0f, t = 0.0f;
        if (!g_activeBaro->read(&a, &t)) continue;
        lastAmbAgain = lastAmb;
        lastAmb = a;
        tempC = t;
        l.push(a);
        delay(120);  // samples spread over ~3 s: a breathed-on or drifting module must NOT lock
    }
    if (!l.locked()) {
        char detail[40];
        snprintf(detail, sizeof(detail), "Psurf sigma %.1fmbar", l.sigmaBar() * 1000.0f);
        g_faults.raise(FaultCode::BARO_CONV_BAD, Severity::ERROR, millis(), detail);
        Serial.printf("  [psurf] ** no lock in 24 samples — sigma %.2f mbar, samples %.2f/%.2f mbar "
                      "** (baro stable enough to arm? no)\n",
                      l.sigmaBar() * 1000.0f, lastAmbAgain * 1000.0f, lastAmb * 1000.0f);
        return;
    }
    g_psurf = l;
    g_psurfBar = l.psurfBar();
    // Only clear CONV_BAD when it can only have come from Psurf instability or a re-checked
    // fallback — a squatter at another baro address keeps its block (checkBaro owns that).
    if (!g_baroSquatter) g_faults.clear(FaultCode::BARO_CONV_BAD, millis());
    raiseRangeBanner();  // re-quote the clamp against a LEARNED reference: "SL ref" disappears
    Serial.printf("[psurf] learned %.2f mbar (sigma %.2f mbar over %d samples, %.1f C) — "
                  "depth = gauge pressure x %.3f m/bar (SG %.3f)\n",
                  g_psurfBar * 1000.0f, l.sigmaBar() * 1000.0f, l.samples(), tempC,
                  depth::metersPerBar(g_salinity), g_salinity);
}

// The INA219 has no CHIPID, so identity here is fully active: config write-verify + cal read-back
// + a real bus-voltage reading inside the rail window (2.5-5.5 V — wider than every operating
// threshold, the check asks "is this sensor measuring", not "is the pack charged"; PwrRegs.h).
// Writes registers, so boot and presence transitions only. V_BAT is an arming requirement
// (AGENTS.md 13), so an ACKing part that cannot do this blocks arming via INA219 (ERROR).
void checkPower() {
    bool present = false;
    for (const DevSpec& d : g_devices)
        if (d.code == FaultCode::INA219 && d.present && busUp(&Wire)) present = true;
    if (!present) return;  // absence is already the INA219 fault from probeDevices

    char detail[40];
    if (!g_ina219.init()) {
        snprintf(detail, sizeof(detail), "0x40 init status %d", (int)g_ina219.lastStatus());
        g_faults.raise(FaultCode::INA219, Severity::ERROR, millis(), detail);
        Serial.printf("  PWR @0x40 ** init failed (status %d) — ACKs, not trusted for V_BAT **\n",
                      (int)g_ina219.lastStatus());
        return;
    }
    float v = 0.0f, a = 0.0f;
    if (!g_ina219.read(&v, &a)) {
        snprintf(detail, sizeof(detail), "0x40 read status %d", (int)g_ina219.lastStatus());
        g_faults.raise(FaultCode::INA219, Severity::ERROR, millis(), detail);
        Serial.printf("  PWR @0x40 ** read failed (status %d, bus word 0x%04X) **\n",
                      (int)g_ina219.lastStatus(), g_ina219.lastBusWord());
        return;
    }
    g_faults.clear(FaultCode::INA219, millis());
    Serial.printf("  PWR @0x40 INA219: V_BAT=%.3f V  I=%+.1f mA  shunt=%+.2f mV  (cfg 0x%04X "
                  "verify-ok)\n",
                  v, a * 1000.0f, g_ina219.shuntmV(), g_ina219.lastConfigWord());
    Serial.println(F("       caveat: current figures assume the 0.1 ohm breakout shunt "
                     "(PwrRegs.h) — meter cross-check pending, part not fitted on this bench"));
}

void adcSelfTests() {
    Serial.println(F("[ads] converter self-test (reads ID/registers, write-verifies lo_thresh and "
                     "restores it, times one 32 SPS conversion):"));
    if (!busUp(&Wire1)) {
        Serial.println(F("  I2C1 is down — nothing to test"));
        return;
    }
    for (sensors::Ads1115* adc : g_adcs) {
        bool present = false;
        for (const DevSpec& d : g_devices)
            if (d.bus == &Wire1 && d.addr == adc->address()) present = d.present;
        if (!present) {
            Serial.printf("  ADS @0x%02X not present — skipped\n", adc->address());
            continue;
        }
        adc->selfTest(Serial);
    }
}

void printScanLine(const char* label, const sensors::ScanResult& r) {
    if (!r.initialized) {
        Serial.printf("  %s port NOT INITIALISED — results below are meaningless\n", label);
        return;
    }
    // No stuck-SDA note here: that check cannot run while the port is live (BusProbe.h), and a
    // stuck line makes i2cInit fail, which initBus reports as a bus fault instead.
    Serial.printf("  %s: %u device(s)  [", label, r.count);
    const int shown = r.count < 16 ? r.count : 16;
    for (int i = 0; i < shown; ++i) Serial.printf("%s0x%02X", i ? " " : "", r.addresses[i]);
    Serial.println("]");
}

void fullScan() {
    for (BusState& b : g_buses) {
        printScanLine(b.label, sensors::scanBus(*b.wire, b.sda, b.scl));
    }
}

void knownScan() {
    // Only meaningful for the addresses we fit; the table is the whole point.
    uint8_t addrs[kDevCount];
    for (size_t i = 0; i < kDevCount; ++i) addrs[i] = g_devices[i].addr;
    for (BusState& b : g_buses)
        printScanLine(b.label, sensors::scanKnown(*b.wire, addrs, kDevCount, b.sda, b.scl));
}

// Baro identity by measurement — presence is not identity (decision 29). The candidate list is
// the datasheet 7-bit forms (Bosch DS Table 7: 1110111+R/W = 0x77; TE MS5837 DS: "Address (7
// bits) 0x76"; ST LPS28DFW: bits 101110xb = 0x5C/0x5D), plus the BMP280/BME280 strap pair, which
// share 0x76/0x77 (CSB selects) — so an ACK at 0x76 is *not* proof of an MS5837 until the PROM
// read says so. Bosch CHIPID reads (pointer write + read of 0xD0, then 0x30 for the BMP3xx
// family) are WRITES to the pointer register, and the LPS28DFW check runs the dive driver's
// init() (SWRESET + config writes) — all bench-only, same class as 'a'/'d', and the banner says so.
void baroIdentity() {
    static const uint8_t kBaro[] = {0x76, 0x77, lps28::kAddr7, lps28::kAddr7Alt};
    bool any = false;
    for (BusState& b : g_buses) {
        if (!busUp(b.wire)) continue;
        for (uint8_t a : kBaro) {
            if (!sensors::probeAddress(*b.wire, a).ok) continue;
            any = true;

            // MS5837 identity lives in its factory PROM, not in a CHIPID register (M3). Run the
            // real dive driver's chain: RESET -> PROM read -> CRC-4 -> C1 band -> one D1/D2 pair
            // with DS-MAX waits. Writes the reset/conversion commands, so bench path, same class
            // as the Bosch pointer writes below and 'a'/'d' on the ADCs.
            if (a == ms5837::kAddr7 && b.wire == &Wire) {
                if (!g_ms5837.init()) {
                    Serial.printf("    MS5837 driver: init FAILED (status %d) — no CRC-verified "
                                  "PROM, 0x76 is not proven to be an MS5837\n",
                                  (int)g_ms5837.lastStatus());
                } else {
                    const ms5837::Prom& pr = g_ms5837.prom();
                    Serial.printf("    PROM: W0=0x%04X (crc=%X type=%u factory=0x%02X)  "
                                  "C1=%u C2=%u C3=%u C4=%u C5=%u C6=%u  W7=0x%04X serial=%u\n",
                                  pr.w[0], ms5837::w0Crc(pr.w[0]), ms5837::w0TypeBits(pr.w[0]),
                                  ms5837::w0FactoryBits(pr.w[0]), pr.w[1], pr.w[2], pr.w[3],
                                  pr.w[4], pr.w[5], pr.w[6], pr.w[7], ms5837::w7Serial(pr.w[7]));
                    Serial.printf("    CRC: read %u computed %u -> %s   C1 %u -> %s\n",
                                  g_ms5837.crcRead(), g_ms5837.crcComputed(),
                                  g_ms5837.crcRead() == g_ms5837.crcComputed() ? "VERIFIED" : "FAIL",
                                  pr.w[1],
                                  g_ms5837.model() == ms5837::Model::k30BA ? "30BA band"
                                  : g_ms5837.model() == ms5837::Model::k02BA ? "02BA band (2 bar — wrong part for this design)"
                                                                             : "OUTSIDE both bands");
                    // DS C6 programs W0 bits[11:5] per product, but the revisions disagree on the
                    // suffix (-30BA01 vs -30BA26), so type bits are REPORTED, never gated: an
                    // unverified expectation is what condemned a good BMP180 here once (item 17).
                    Serial.printf("    W0 type bits %u reported, not gated (DS revisions contradict "
                                  "on the product suffix — see BaroMs5837.h provenance)\n",
                                  ms5837::w0TypeBits(pr.w[0]));
                    float a1 = 0, a2 = 0, t1 = 0;
                    if (g_ms5837.read(&a1, &t1) && g_ms5837.read(&a2, &t1)) {
                        Serial.printf("    driver (OSR4096, DS-MAX 9.04ms waits): %.2f mbar -> %.2f mbar  "
                                      "%.2f C  D1=%lu D2=%lu\n",
                                      a1 * 1000.0f, a2 * 1000.0f, t1,
                                      (unsigned long)g_ms5837.lastD1(), (unsigned long)g_ms5837.lastD2());
                        Serial.println(F("    acceptance when a Bar30 arrives: Stockholm-station mbar "
                                         "(SLP 984-987 hPa band, 2026-11-20 — named place/hour/source, "
                                         "risk 21), and the mbar MUST move when the port is squeezed"));
                    } else {
                        Serial.printf("    driver read FAILED status %d (D1=%lu D2=%lu) — zero words "
                                      "mean no conversion completed behind the ADC read (DS)\n",
                                      (int)g_ms5837.lastStatus(), (unsigned long)g_ms5837.lastD1(),
                                      (unsigned long)g_ms5837.lastD2());
                    }
                    Serial.println(F("    caveat: MS5837 compensation is written-not-measured "
                                     "(part unprocured) — the math is DS-C6 + deployed-library "
                                     "anchored (test_ms5837); this is its first run on silicon"));
                }
            }

            // LPS28DFW (decision 39 tier-2): no factory PROM on this silicon, so the bench runs
            // the same active chain the dive path uses — WHO_AM_I, then the real driver's init()
            // (SWRESET + config writes + readback verify, bench class) and a live conversion,
            // plus the COMPUTED depth ceiling against the latched full scale (decision 38: never
            // recite, and the "40 m underwater" marketing treats an ABSOLUTE full scale as gauge).
            if (a == lps28::kAddr7 || a == lps28::kAddr7Alt) {
                uint8_t who = 0;
                bool gotWho = false;
                b.wire->beginTransmission(a);
                b.wire->write(lps28::kRegWhoAmI);
                gotWho = b.wire->endTransmission(false) == 0 &&
                         b.wire->requestFrom(a, (uint8_t)1) == 1;
                if (gotWho) who = b.wire->read();
                Serial.printf("  %s @0x%02X ACK  WHO_AM_I=%s0x%02X (LPS28DFW: 0xB4) -> %s\n",
                              b.label, a, gotWho ? "" : "no-read ", who,
                              (gotWho && who == lps28::kWhoAmI) ? "claims LPS28DFW"
                                                                : "NOT an LPS28DFW WHO_AM_I");
                if (b.wire != &Wire) {
                    Serial.println(F("    driver check skipped: module answers but the design fits "
                                     "the baro on I2C0 (Wire) — check the wiring"));
                } else {
                    g_lps28.setAddress(a);
                    if (!g_lps28.init()) {
                        Serial.printf("    driver: init FAILED (status %d, WHO=0x%02X CTRL1=0x%02X "
                                      "CTRL2=0x%02X) — no verified identity, no ceiling claim\n",
                                      (int)g_lps28.lastStatus(), g_lps28.whoAmI(), g_lps28.lastCtrl1(),
                                      g_lps28.lastCtrl2());
                    } else {
                        float a1 = 0, a2 = 0, t1 = 0;
                        if (g_lps28.read(&a1, &t1) && g_lps28.read(&a2, &t1)) {
                            const float refPsurf = g_psurf.locked() ? g_psurfBar : 1.01325f;
                            const float ceilM = depth::ceilingMeters(refPsurf,
                                                                     g_lps28.rangeCeilingBar(),
                                                                     g_salinity);
                            Serial.printf("    driver (10 Hz, AVG4, FS4060 latched=%d): %.2f mbar -> "
                                          "%.2f mbar  %.2f C  raw=0x%08lX CTRL1=0x%02X CTRL2=0x%02X\n",
                                          g_lps28.fsMode2(), a1 * 1000.0f, a2 * 1000.0f, t1,
                                          static_cast<unsigned long>(g_lps28.lastRawAligned()),
                                          g_lps28.lastCtrl1(), g_lps28.lastCtrl2());
                            Serial.printf("    ceiling %.1fm @ %.0fmbar ref (computed abs-FS minus "
                                          "Psurf — Adafruit's \"40 m underwater\" quotes ABS FS as "
                                          "if gauge)\n", ceilM, refPsurf * 1000.0f);
                            Serial.println(F("    squeeze the module and re-run 'b': the mbar MUST move"));
                        } else {
                            Serial.printf("    driver read FAILED status %d (raw=0x%08lX)\n",
                                          (int)g_lps28.lastStatus(),
                                          static_cast<unsigned long>(g_lps28.lastRawAligned()));
                        }
                        Serial.println(F("    caveat: LPS28DFW driver is written-not-measured (part "
                                         "unprocured) — scaling is ST vendor-driver anchored "
                                         "(test_lps28); this is its first run on silicon"));
                    }
                }
                continue;  // the Bosch CHIPID/cal-ROM dump below does not apply to ST silicon
            }

            uint8_t d0 = 0x00, r30 = 0x00, d1 = 0x00, ac1msb = 0x00, ac1lsb = 0x00;
            bool gotD0 = false, got30 = false, gotD1 = false, gotAc1 = false;
            auto readReg = [&](uint8_t reg, uint8_t& out, bool& ok) {
                b.wire->beginTransmission(a);
                b.wire->write(reg);
                ok = b.wire->endTransmission(false) == 0 && b.wire->requestFrom(a, (uint8_t)1) == 1;
                if (ok) out = b.wire->read();
            };
            readReg(0xD0, d0, gotD0);
            readReg(0x30, r30, got30);
            readReg(0xD1, d1, gotD1);           // "version MSB": BMP085 says 0x43; the BMP180
            bool okLsb = false;                 // datasheet has no 0xD1 at all, so 0x02 there
            readReg(0xAA, ac1msb, gotAc1);      // is an undefined-register read, not a mismatch.
            readReg(0xAB, ac1lsb, okLsb);       // AC1 is per-chip factory trim (0xAA..0xBF block
            gotAc1 = gotAc1 && okLsb;           // varies chip to chip) — sanity only, never ID.
            const char* who = "?";
            if (d0 == 0x55) who = "BMP085/BMP180 (CHIPID 0xD0=0x55)";
            else if (d0 == 0x58 || d0 == 0x40 || d0 == 0x41) who = "BMP280 (CHIPID 0xD0=0x58; 0x40/0x41 older revs)";
            else if (d0 == 0x60) who = "BME280 (CHIPID 0xD0=0x60)";
            else if (r30 == 0x03) who = "BMP388-class (CHIPID 0x30=0x03)";
            else if (r30 == 0x01) who = "BMP390-class (CHIPID 0x30=0x01)";
            else if (a == 0x76) who = "no Bosch CHIPID — MS5837-class? identity = the PROM/CRC chain above, not this ACK";
            Serial.printf("  %s @0x%02X ACK  0xD0=%s0x%02X  0xD1=%s0x%02X(BMP085:0x43,BMP180:none)  "
                          "AC1=0x%02X%02X  0x30=%s0x%02X  -> %s\n",
                          b.label, a,
                          gotD0 ? "" : "no-read ", d0,
                          gotD1 ? "" : "no-read ", d1,
                          gotAc1 ? ac1msb : 0, gotAc1 ? ac1lsb : 0,
                          got30 ? "" : "no-read ", r30, who);
            if (d0 == 0x55)
                Serial.println(F("    note: 0x77 is the datasheet 7-bit address for this family "
                                 "(bits 1110111 + R/W). CHIPID 0x55 claims a BMP085/180 register "
                                 "map; conversion correctness is still proved by a real reading."));

            // Identity by register read is necessary, not sufficient (item 17). Run the real
            // firmware driver, then the field-deployed Adafruit implementation of the same Bosch
            // API, on the same module: two compensation implementations agreeing on the same raw
            // silicon is the bench-grade cross-check for the transcription in BaroBosch.h.
            // This writes ctrl_meas, so it is bench path, same class as 'a'/'d'.
            if (d0 == 0x55 && a == 0x77) {
                if (b.wire != &Wire) {
                    Serial.println(F("    driver check skipped: module answers but the design fits "
                                     "the baro on I2C0 (Wire) — check the wiring"));
                } else {
                    if (!g_bmp085.init()) {
                        Serial.printf("    driver: init FAILED (status %d, cal word %d) — no trustworthy "
                                      "ROM, no number\n",
                                      (int)g_bmp085.lastStatus(), g_bmp085.calRomBadIndex());
                    } else {
                        // All 11 trim words, Table 5 order. Both compensation implementations read
                        // these same words, so driver-vs-library agreement proves the arithmetic —
                        // absolute pressure can only be as good as the trim. Printed so an
                        // absolute-offset finding is diagnosable from the capture alone.
                        const baro::Cal& c = g_bmp085.calRom();
                        Serial.printf("    cal ROM: AC1=%d AC2=%d AC3=%d AC4=%u AC5=%u AC6=%u "
                                      "B1=%d B2=%d MB=%d MC=%d MD=%d\n",
                                      c.ac1, c.ac2, c.ac3, c.ac4, c.ac5, c.ac6, c.b1, c.b2,
                                      c.mb, c.mc, c.md);
                        float a1 = 0, a2 = 0, t1 = 0, t2 = 0;
                        if (g_bmp085.read(&a1, &t1) && g_bmp085.read(&a2, &t2)) {
                            Serial.printf("    driver (oss=2): %.2f mbar -> %.2f mbar  %.1f C   "
                                          "UT=%u UP=%lu B5=%ld\n",
                                          a1 * 1000.0f, a2 * 1000.0f, t1, g_bmp085.lastUt(),
                                          static_cast<unsigned long>(g_bmp085.lastUp()),
                                          static_cast<long>(g_bmp085.lastB5()));
                            Serial.println(F("    squeeze the module and re-run 'b': the mbar MUST move"));
                        } else {
                            Serial.printf("    driver read FAILED status %d (UT=%u UP=%lu)\n",
                                          (int)g_bmp085.lastStatus(), g_bmp085.lastUt(),
                                          static_cast<unsigned long>(g_bmp085.lastUp()));
                        }
                        Adafruit_BMP085 ref;
                        if (ref.begin(BMP085_HIGHRES, b.wire)) {
                            const float rp = ref.readPressure();  // Pa
                            const float rt = ref.readTemperature();
                            Serial.printf("    Adafruit lib (oss=2): %.2f mbar  %.1f C\n",
                                          rp / 100.0f, rt);
                        } else {
                            Serial.println(F("    Adafruit lib begin() failed — cross-check skipped"));
                        }
                    }
                }
            }
        }
    }
    if (!any) Serial.println(F("  no baro-class address (0x76/0x77/0x5C/0x5D) ACKed on any live bus"));
    Serial.println(F("  note: the design fits baro on I2C0 only (SDA 21/SCL 22) — an ACK on I2C1 "
                     "means the module is on the ADC bus, not where the firmware will look."));
}

void printBootReport() {
    char line[96];
    g_faults.summaryLine(line, sizeof(line));
    g_faults.forEach([&](const boot::Entry& e) {
        if (!e.present) return;
        Serial.printf("  %-5s %-22s x%u  %s\n",
                      e.severity == Severity::FATAL ? "FATAL"
                      : e.severity == Severity::ERROR ? "ERROR"
                      : e.severity == Severity::WARN  ? "WARN"
                                                      : "INFO",
                      faultName(e.code), e.attempts, e.detail);
    });
    Serial.printf("[boot] %s\n", line);
    Serial.println(g_faults.blocksArming() ? "[arm] REFUSED — see faults above"
                                           : "[arm] checklist: sensors ok");
}

bool initBus(BusState& b) {
    // Safe only before begin(): pinMode on a pin the driver owns deinits the port (BusProbe.h).
    // A stuck line is a real hardware fault and gets said out loud, not silently retried forever.
    if (sensors::sdaHeldLow(b.sda)) {
        Serial.printf("[i2c] %s SDA held low before init — 9-clock recovery\n", b.label);
        sensors::recoverBus(*b.wire, b.sda, b.scl);
        g_faults.raise(FaultCode::I2C_BUS_STUCK, Severity::ERROR, millis(), b.label);
    }
    const bool ok = b.wire->begin(b.sda, b.scl);
    if (ok) b.wire->setClock(400000);
    Serial.printf("[i2c] %s sda=%d scl=%d %s\n", b.label, b.sda, b.scl, ok ? "ok" : "FAILED");
    if (ok) {
        // A port that was down and now came back: close its fault, but leave the history in the
        // report (AGENTS.md 4). Deliberately *after* begin(): if begin() fails on a stuck line we
        // must not clear the stuck-SDA fault raised just above, or the report would contradict the
        // console three lines earlier.
        if (!b.up) g_faults.clear(FaultCode::I2C_BUS_STUCK, millis());
    } else {
        char detail[24];
        snprintf(detail, sizeof(detail), "%s init failed", b.label);
        g_faults.raise(FaultCode::I2C_BUS_STUCK, Severity::ERROR, millis(), detail);
    }
    b.up = ok;
    return ok;
}

// Which peripheral the Arduino core thinks it has routed to a pad. This is the thing that makes
// decision 26 measurable: pinMode() on a pad a live Wire port owns detaches that port, so before
// anyone bit-bangs or re-purposes a pin, this is the question to ask. Values are compared to the
// enum names, never to numbers — the enum is compiled conditionally per SoC and its numbering is
// not stable across cores.
const char* padOwner(int p) {
    switch (perimanGetPinBusType(p)) {
        case ESP32_BUS_TYPE_INIT:          return "free";
        case ESP32_BUS_TYPE_GPIO:          return "GPIO";
        case ESP32_BUS_TYPE_UART_RX:       return "UART-RX";
        case ESP32_BUS_TYPE_UART_TX:       return "UART-TX";
        case ESP32_BUS_TYPE_UART_CTS:      return "UART-CTS";
        case ESP32_BUS_TYPE_UART_RTS:      return "UART-RTS";
        case ESP32_BUS_TYPE_I2C_MASTER_SDA: return "I2C-SDA";
        case ESP32_BUS_TYPE_I2C_MASTER_SCL: return "I2C-SCL";
        case ESP32_BUS_TYPE_ADC_ONESHOT:   return "ADC";
        case ESP32_BUS_TYPE_LEDC:          return "LEDC";
        case ESP32_BUS_TYPE_TOUCH:         return "TOUCH";
        default:                           return "other";
    }
}

bool padOwnedByLiveBus(int p) {
    const int t = perimanGetPinBusType(p);
    return t == ESP32_BUS_TYPE_I2C_MASTER_SDA || t == ESP32_BUS_TYPE_I2C_MASTER_SCL ||
           t == ESP32_BUS_TYPE_I2C_SLAVE_SDA || t == ESP32_BUS_TYPE_I2C_SLAVE_SCL;
}

// Bench command 'g': the pad table, plus the two things the table cannot answer by itself — what the
// core has actually claimed, and whether a pad's internal pulls and output path work on this die.
// The table in lib/core/src/PinMap.h is asserted natively, but it is still a claim about a module;
// this is the measurement next to it.
//
// Safety, in order of how badly getting it wrong hurts:
//  - never touch a pad the console owns (GPIO1/3 here): that silences the report printing it, and
//    on a dive computer a lost console during bring-up costs the diagnosis;
//  - never touch a pad a live Wire port owns (decision 26 / risk 15) — pinMode() would deinit I2C
//    and every sensor read would stop, which is the exact failure this command exists to prevent;
//  - never drive a pad that has a load on it. The probe list below is only pads with nothing wired
//    on this bench; the solenoid, buzzer and watchdog heartbeat are excluded by name.
// What is proven here is reachability and switching. Drive strength into a real load is a meter
// measurement at the header, and this command says so rather than implying otherwise.
void gpioMapReport() {
    Serial.println(F("[gpio] pad map for ESP32-WROOM-32 (caps = PinMap.h claim, owner = core periman)"));
    for (int p = 0; p < pins::kGpioCount; ++p) {
        const uint32_t c = pins::caps(p);
        char cap[8] = "------";
        cap[0] = (c & pins::kBonded)  ? 'b' : '-';
        cap[1] = (c & pins::kOutput)  ? 'o' : '-';
        cap[2] = (c & pins::kInput)   ? 'i' : '-';
        cap[3] = (c & pins::kPull)    ? 'p' : '-';
        cap[4] = (c & pins::kAdc1) ? '1' : ((c & pins::kAdc2) ? '2' : '-');
        cap[5] = (c & pins::kStrap) ? 'S' : ((c & pins::kConsole) ? 'U' : '-');
        // Assigned pads show their role; unclaimed pads show why they are unusable, or "spare".
        const pins::Assignment* a = pins::findAssignment(p);
        const char* note = a ? a->role : pins::unusableReason(p);
        if (!note) note = pins::outputCapable(p) ? "spare" : "spare (input-only)";
        // Asking the core who owns a pad that does not exist emits a driver ERROR line per pad
        // (measured: "Invalid pin: 24"), which is the console-spam failure mode decision 26 exists to
        // avoid. The capability table already knows, so ask it instead.
        const char* owner = (pins::noSuchPad(p) || pins::dieOnlyPad(p)) ? "n/a" : padOwner(p);
        Serial.printf("  %2d  [%s] %-9s %s%s\n", p, cap, owner, note,
                      p == pins::SPARE_OUT ? " — declared spare (SPI panel clock)" : "");
    }

    // Pads with nothing wired on this bench, plus 34/35 to confirm the input-only claim instead of
    // trusting it. Add to this list only after checking what is actually connected.
    static const int kProbe[] = {16, 17, 32, 33, 34, 35};
    Serial.println(F("[gpio] pad behaviour, measured (only pads with nothing wired):"));
    for (int p : kProbe) {
        if (pins::consolePad(p) || pins::flashPad(p)) {
            Serial.printf("  %2d  skipped — %s\n", p, pins::unusableReason(p));
            continue;
        }
        if (padOwnedByLiveBus(p)) {
            Serial.printf("  %2d  skipped — owned by a live I2C port (pinMode would kill it)\n", p);
            continue;
        }
        if (!pins::inputCapable(p)) {
            Serial.printf("  %2d  skipped — not input-capable on this die\n", p);
            continue;
        }
        if (!pins::pullCapable(p)) {
            // The table already knows these pads have no pulls; asking for one costs a driver ERROR
            // line and proves nothing. Measured once on 34/35 and it is recorded in PinMap.h.
            Serial.printf("  %2d  no internal pull-up/pull-down (input-only pad) — a button or "
                          "switch here needs an external resistor, which is a BOM item\n", p);
            continue;
        }
        pinMode(p, INPUT_PULLUP);
        delayMicroseconds(200);
        const int hi = digitalRead(p);
        pinMode(p, INPUT_PULLDOWN);
        delayMicroseconds(200);
        const int lo = digitalRead(p);
        if (pins::outputCapable(p)) {
            Serial.printf("  %2d  pull-up read %d, pull-down read %d -> internal pulls %s\n", p, hi, lo,
                          (hi && !lo) ? "work" : "DO NOT WORK as documented");
            // Read-back after driving: the input path is off while a pad is a pure output on this
            // core, so re-open it as a high-Z input and see whether the pad kept the level.
            pinMode(p, OUTPUT);
            digitalWrite(p, HIGH);
            delayMicroseconds(50);
            pinMode(p, INPUT);
            delayMicroseconds(50);
            const int keptHi = digitalRead(p);
            pinMode(p, OUTPUT);
            digitalWrite(p, LOW);
            delayMicroseconds(50);
            pinMode(p, INPUT);
            delayMicroseconds(50);
            const int keptLo = digitalRead(p);
            Serial.printf("      driven hi -> hi-Z reads %d, driven lo -> hi-Z reads %d (a floating "
                          "pad keeps what it was driven to)", keptHi, keptLo);
            Serial.println((keptHi && !keptLo) ? " — switches both ways" : " — SUSPECT");
            Serial.println(F("      (drive strength into a real load is a meter measurement, not this)"));
        } else {
            Serial.printf("  %2d  pull-up read %d, pull-down read %d -> input-only pad, and internal "
                          "pulls are %s on it (34..39 have none: external resistor required)\n",
                          p, hi, lo, (hi && !lo) ? "present" : "absent");
        }
        // Put every probed pad back the way its owner expects it.
        const pins::Assignment* a = pins::findAssignment(p);
        if (a && a->needsOutput) {
            pinMode(p, OUTPUT);
            digitalWrite(p, LOW);
        } else if (a) {
            pinMode(p, INPUT_PULLUP);
        } else {
            pinMode(p, INPUT_PULLDOWN);  // a spare pad must not be left floating
        }
    }
    Serial.println(F("[gpio] never wire to these: 24, 28-31 (no such pad on the ESP32-D0WD-V3 die); "
                     "20, 37, 38 (die pads this module does not bond out); 6-11 (inside the module's "
                     "SPI flash path)"));
}

// Headless simulator run (M1 gate, AGENTS.md 11). Everything below is lib/sim + lib/core: the
// depth comes from SimBaro, the millivolts from SimCells, and ppO2 is *not supplied to anything* —
// it falls out of metabolism against the pulses the control law commanded through the valve model,
// then runs through the real voter, gas model and ZHL engine. It touches no pin, no bus and no
// radio, and every line is stamped SIM (AGENTS.md 19 risk 11: source stamping is not decoration).
//
// This runs in one go inside the bench loop. A profile of a few minutes of dive time is tens of
// thousands of arithmetic operations; on the ESP32 that is tens of seconds (worst tick 41.5 ms
// measured — AGENTS.md risk 19), and the idle-task WDT *is* armed on this core: the loop below
// yields with delay(1) every 15 engine ticks. Do not wire it to anything while it runs.
void benchRunSim(Stream& out) {
    // NOT a local: SimRunner is ~145 KB (the 512-entry snapshot history dominates). The Arduino
    // loop task stack is ~8 KB, and constructing it there smashed memory and came back as
    // TG1WDT_SYS_RESET with no panic output — measured on the first 'v' run. Static storage is
    // also the AGENTS.md 17 rule in miniature: no large allocation in the running path.
    static sim::SimRunner r;
    r.resetRun();
    sim::CalSet cal{};              // shipped air-cal shape: 47.6 mV/bar, 0 offset
    zhl::Config ec{};               // GF 30/70, 6 m last stop, defaults from Zhl16c.h
    GasModelConfig gc{};            // dual setpoint 0.7 above / 1.3 below the 21 m crossover
    sim::ControlConfig cc{};
    cc.enabled = true;              // the law computes and injects inside the MODEL only
    r.setConfig(ec, gc, cal, cc);
    r.setBattery(3.7f);

    sim::GasSet gases{};            // air diluent, escape air, O2 — the bench default set
    r.setGases(gases);

    sim::Profile p{};
    auto seg = [&](int i, float depthM, float min, BreathingState mode) {
        p.seg[i].depthM = depthM;
        p.seg[i].durationMin = min;
        p.seg[i].mode = mode;
    };
    seg(0, 30.0f, 5.0f, BreathingState::CCR_CLOSED);      // descent (ramped at the profile rate)
    seg(1, 30.0f, 8.0f, BreathingState::CCR_CLOSED);      // bottom
    seg(2, 30.0f, 1.0f, BreathingState::CCR_DILUENT_FLUSH);  // flush: independent cell check
    seg(3, 21.0f, 3.0f, BreathingState::CCR_CLOSED);
    seg(4, 6.0f, 6.0f, BreathingState::CCR_CLOSED);       // deco stop
    seg(5, 0.0f, 3.0f, BreathingState::CCR_CLOSED);
    p.count = 6;
    p.defaultRateMPerMin = 8.0f;
    r.setProfile(p);
    // 96-entry ring on the Arduino x 16 s stride = the whole 1560 s profile retained. At stride 1
    // the ring would silently hold only the first 96 seconds and every window query about the
    // later dive would be answering from nothing (the NO-DATA rule in SimRunner::verify).
    r.setHistoryStride(16);

    out.println(F("[sim] SIM run — 30 m / 8 min CCR on air, diluent flush at depth, GF 30/70"));
    out.println(F("      SIM DATA, bench only: no pin, no bus, no radio is touched"));
    uint32_t nextReport = 0;
    bool flushChecked = false;
    int ticksSinceYield = 0;
    // This core arms the task WDT on its idle tasks: a bench command that blocks for seconds
    // without yielding takes TG1WDT_SYS_RESET (measured on the first 'v' run — the run died
    // mid-profile and rebooted the board). Yielding every 15 engine ticks feeds the dog and
    // costs nothing; wall-clock is not the point of a headless model run.
    //
    // Per-tick cost is measured, not assumed (the same discipline as the ADC rate table): a
    // deco search that saturates its step budget is bounded but expensive — seconds per tick
    // here — and that is exactly what looks like a hang from the console. The SLOW line is
    // printed *before* the next tick so a tick that never finishes still leaves its
    // neighbourhood on the wire.
    uint32_t worstTickUs = 0;
    uint32_t worstTickAt = 0;
    uint32_t prevTickUs = 0;
    uint32_t prevTickAt = 0;
    int prevPlanSteps = 0;
    while (!r.finished()) {
        if (++ticksSinceYield >= 15) {
            ticksSinceYield = 0;
            delay(1);
        }
        if (prevTickUs > 100000)
            out.printf("  SIM SLOW: t=%us took %luus plansteps=%d — next tick starting\n",
                       (unsigned)prevTickAt, (unsigned long)prevTickUs, prevPlanSteps);
        const uint32_t t0 = micros();
        const sim::Snapshot& s = r.tick();
        prevTickUs = micros() - t0;
        prevTickAt = s.tSec;
        prevPlanSteps = r.engine.lastPlanSteps();
        if (prevTickUs > worstTickUs) { worstTickUs = prevTickUs; worstTickAt = s.tSec; }
        if (s.tSec >= nextReport) {
            out.printf("  SIM t=%4um d=%5.1fm ppO2=%4.2f sp=%4.2f pul=%5ld inj=%6.2fL "
                       "vote=%d pass=%d NDL=%5.0f ceil=%4.1fm TTS=%5.1f CNS=%3.0f%% src=%s\n",
                       (unsigned)(s.tSec / 60), s.depthM, s.ppO2Used, s.setpointBar,
                       (long)s.pulsesTotal, s.injectedAmbL, (int)s.vote, s.passing,
                       s.ndlMin, s.ceilingM, s.ttsMin, s.cnsPct, sourceName(s.sources));
            nextReport += 60;
        }
        if (!flushChecked && s.mode == BreathingState::CCR_DILUENT_FLUSH) {
            flushChecked = true;
            // The check the diver actually gets: do all three cells independently land on the
            // diluent's expected ppO2? (AGENTS.md 5 — voting cannot fake this.)
            CellReading rd[CELL_COUNT];
            for (int i = 0; i < CELL_COUNT; ++i) {
                rd[i].mv = s.cellMv[i];
                rd[i].present = s.cellPresent[i];
            }
            VoteOutcome v = evaluateCells(rd, cal.cal, cal.vote, false);
            const float expected = expectedPpO2ForFlush(gases.diluent.fo2, s.ambBar, gc.pH2OBar);
            float worst = 0.0f;
            const bool ok = diluentFlushAgrees(v, expected, 0.15f, &worst);
            out.printf("  SIM diluent flush @%5.1fm: expected ppO2 %4.2f, worst cell dev %4.2f -> %s\n",
                       s.depthM, expected, worst, ok ? "PASS" : "FAIL");
        }
    }
    const sim::Snapshot& f = r.last();
    out.printf("  SIM done: pulses=%lu injected=%.2f ambL  CNS=%.1f%% OTU=%.0f  sources=%s\n",
               (unsigned long)f.pulsesTotal, f.injectedAmbL, f.cnsPct, f.otu,
               sourceName(f.sources));
    out.printf("  SIM tick cost: worst %luus at t=%us (plan search saturating its step budget is "
               "bounded but slow — this number is how you notice)\n",
               (unsigned long)worstTickUs, (unsigned)worstTickAt);
    out.println(F("  honest limits (AGENTS.md 19 risk 12): metabolic rate and loop volume are "
                  "unmeasured defaults; this tunes the control law, it does not validate the loop"));
}

void handleCommand(char c) {
    switch (c) {
        case 's':
            Serial.println(F("[scan] full 0x08..0x77 sweep:"));
            fullScan();
            break;
        case 'p':
            Serial.println(F("[probe] known-address table:"));
            knownScan();
            probeDevices();
            printBootReport();
            break;
        case 'r':
            Serial.println(F("[diag] raw transaction codes — WRITES 0x00 to whatever answers, "
                             "only safe on a bare bus:"));
            // The addresses we fit; extend on the bench when bring-up needs more.
            static const uint8_t kDiag[] = {0x3C, 0x76, 0x77, 0x40, 0x48, 0x49};
            for (BusState& b : g_buses)
                if (busUp(b.wire)) sensors::probeReport(*b.wire, kDiag, sizeof(kDiag), b.sda, b.scl);
            break;
        case 'a':
            adcSelfTests();
            break;
        case 'd':
            Serial.println(F("[ads] rate characterisation (writes config, ~0.3 s per converter):"));
            for (sensors::Ads1115* adc : g_adcs)
                if (adc->present()) adc->measureAdcRates(Serial);
            break;
        case 'c':
            Serial.println(F("[bus] 9-clock recovery + port re-init:"));
            for (BusState& b : g_buses) {
                sensors::recoverBus(*b.wire, b.sda, b.scl);
                b.wire->end();
                initBus(b);
            }
            break;
        case 't':
            // The M1 scheduler gate, measured: conversions actually completed per cell pair, the
            // measured conversion periods behind the coil/ADC interlock window, and the error
            // counters. 'a' and 'd' write the config register mid-walk, so they stir these counters —
            // the report says so rather than pretending the run was clean.
            Serial.println(F("[adc] pair walkers: achieved per-cell rate + interlock window, "
                             "counted since begin. Cell inputs float: raw values are expected "
                             "nonsense — the rates and windows are the point."));
            for (sensors::AdcPairWalker* w : g_walkers) w->report(Serial, millis());
            break;
        case 'b':
            Serial.println(F("[baro] identity: Bosch CHIPID/cal-ROM + a REAL conversion through the "
                             "dive driver and the Adafruit library, the MS5837 PROM/CRC + OSR4096 "
                             "conversion chain, and the LPS28DFW WHO_AM_I + FS_MODE-latch + 10 Hz "
                             "conversion chain (writes commands — bench only, same class as "
                             "'a'/'d'):"));
            baroIdentity();
            break;
        case 'w':
            Serial.println(F("[pwr] INA219 battery rail: config write-verify + raw register dump "
                             "through the dive driver (writes CONFIG/CAL — bench only):"));
            if (!busUp(&Wire)) {
                Serial.println(F("  I2C0 is down — nothing to test"));
                break;
            }
            if (!sensors::probeAddress(Wire, pwr::kAddr7).ok) {
                Serial.println(F("  no INA219 at 0x40 — the part is not fitted on this bench yet; "
                                 "no current figure in this firmware is trusted until it is, and "
                                 "the 0.1 ohm shunt assumption in PwrRegs.h gets a meter "
                                 "cross-check the day it is"));
            } else if (!g_ina219.init()) {
                Serial.printf("  init FAILED status %d (cfg readback 0x%04X) — ACKs, not an "
                              "INA219 that holds config\n",
                              (int)g_ina219.lastStatus(), g_ina219.lastConfigWord());
            } else {
                float v = 0, a = 0;
                if (!g_ina219.read(&v, &a)) {
                    Serial.printf("  read FAILED status %d (bus word 0x%04X)\n",
                                  (int)g_ina219.lastStatus(), g_ina219.lastBusWord());
                } else {
                    Serial.printf("  V_BAT=%.3f V  I=%+.1f mA  P=%.0f mW  shunt=%+.2f mV\n",
                                  v, a * 1000.0f, g_ina219.power_mW(), g_ina219.shuntmV());
                    Serial.printf("  raw: CFG=0x%04X (written 0x%04X, verified) BUS=0x%04X "
                                  "SHNT=%d CUR=%d PWR=%d  CAL=%u (preset 32V_2A)\n",
                                  g_ina219.lastConfigWord(), pwr::kConfig32V2A,
                                  g_ina219.lastBusWord(), g_ina219.lastShuntRaw(),
                                  g_ina219.lastCurrentRaw(), g_ina219.lastPowerRaw(),
                                  (unsigned)pwr::kPreset32V2A.cal);
                    Serial.println(F("  cross-check recipe: meter across V_BAT must match within the "
                                     "meter's own spec; meter in series with a known load must "
                                     "match I — until then every ampere figure carries the 0.1 ohm "
                                     "assumption (stated in PwrRegs.h, risk 21)"));
                }
            }
            break;
        case 'g':
            gpioMapReport();
            break;
        case 'v':
            benchRunSim(Serial);
            break;
        case 'h':
        default:
            Serial.println(F("bench commands: s=full bus scan  p=probe table + report  "
                             "r=raw probe codes (destructive, bench only)  c=bus recovery  "
                             "a=ADC self-test  d=ADC rate characterisation  t=pair-walker rate + "
                             "interlock window  b=baro identity + live mbar/C reading  w=INA219 "
                             "battery rail + raw registers  g=pad map + pad test  "
                             "v=headless sim profile (SIM-stamped, touches no hardware)"));
            break;
    }
}

// One heartbeat phase of housekeeping: drive the pair walkers and the bench REPL for ms. The
// walkers self-throttle their OS-bit polling (kPollGapUs), so an idle walker costs nothing, and
// the serial path stays responsive at ~2 ms instead of 50. This is why the old bare delay(20)/
// delay(30) pair is gone: at a 50 ms loop cadence every achieved-rate figure 't' prints would be
// paced by the loop, not by the converter.
void serviceWindow(uint32_t ms) {
    const uint32_t end = millis() + ms;
    for (;;) {
        for (sensors::AdcPairWalker* w : g_walkers) w->tick();
        while (Serial.available()) handleCommand(static_cast<char>(Serial.read()));
        if (millis() >= end) return;
        delay(2);
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    delay(50);
    // The IDF prints "No core dump partition found!" twice on every boot. There is no core
    // dump partition in this table by design (no flash budget for one), and a cosmetic ERROR
    // line on every boot trains the reader to skim past the fault banner. Everything else from
    // the driver stack stays visible.
    esp_log_level_set("esp_core_dump_flash", ESP_LOG_NONE);
    Serial.println();
    Serial.println(F("CCR dive computer — boot (never fatal), entering BENCH"));

    // First thing after reset: put the solenoid drive in its safe state. The 10k gate pulldown
    // already holds it off, but firmware must not rely on that as its only defence.
    pinMode(pins::SOLENOID_OUT, OUTPUT);
    digitalWrite(pins::SOLENOID_OUT, LOW);

    pinMode(pins::BUZZER_OUT, OUTPUT);
    digitalWrite(pins::BUZZER_OUT, LOW);
    if (pins::VIBRATOR_OUT >= 0) {
        pinMode(pins::VIBRATOR_OUT, OUTPUT);
        digitalWrite(pins::VIBRATOR_OUT, LOW);
    }
    if (pins::WDT_HEARTBEAT >= 0) pinMode(pins::WDT_HEARTBEAT, OUTPUT);

    for (BusState& b : g_buses) initBus(b);

    probeDevices();
    knownScan();
    checkAdcs();
    checkBaro();
    checkPower();
    // Psurf is learned from the barometer that just passed its identity chain — never from a
    // reference pressure looked up for some city (risk 21, item 18: a reference is a measurement
    // with a place, an hour and a source; here the source is the fitted part, at this bench, now).
    learnSurface();
    printBootReport();

    // Start the pair walkers only after checkAdcs() has timed a real conversion on each present
    // converter — the walker sizes its poll cadence, stall timeout and interlock window from
    // that measured period, not from the datasheet (AGENTS.md 2 / risk 16).
    for (sensors::AdcPairWalker* w : g_walkers) w->begin();
    Serial.println(F("[adc] alternating-mux pair walkers running — bench 't' prints the achieved "
                     "per-cell rate and the coil/ADC interlock window, measured"));

#if !HAVE_SOLENOID
    g_faults.raise(FaultCode::SOLENOID_DISABLED, Severity::INFO, millis(), "inert, not fitted");
    Serial.println(F("[o2] solenoid disabled — GPIO 26 will not be driven"));
#endif
    handleCommand('h');
}

void loop() {
    // Hot re-probe every 5 s so plugging a sensor in reports ONLINE instead of needing a reboot
    // (AGENTS.md 4). A bus that failed to init is retried on the same cadence: hot-plugging the
    // first device on I2C1 is a bench bring-up case, not a reboot case.
    static uint32_t lastProbe = 0;
    if (millis() - lastProbe >= 5000) {
        lastProbe = millis();
        for (BusState& b : g_buses) {
            if (!b.up && initBus(b)) Serial.printf("INFO: %s ONLINE\n", b.label);
        }
        bool adcChanged = false;
        bool baroChanged = false;
        bool powerChanged = false;
        for (DevSpec& d : g_devices) {
            if (!busUp(d.bus)) continue;
            const bool was = d.present;
            d.present = sensors::probeAddress(*d.bus, d.addr).ok;
            if (d.present && !was) {
                Serial.printf("INFO: %s ONLINE @0x%02X\n", d.label, d.addr);
                g_faults.clear(d.code, millis());
                if (d.bus == &Wire1) adcChanged = true;
                if (d.code == FaultCode::BARO) baroChanged = true;
                if (d.code == FaultCode::INA219) powerChanged = true;
            } else if (!d.present && was) {
                Serial.printf("ERROR: %s OFFLINE\n", d.label);
                if (d.bus == &Wire1) adcChanged = true;
                if (d.code == FaultCode::BARO) baroChanged = true;
                if (d.code == FaultCode::INA219) powerChanged = true;
            }
        }
        probeDevices();
        // A converter that just appeared has to be checked, not assumed: presence says an address
        // is answered, it does not say the part converts at the design rate.
        if (adcChanged) {
            checkAdcs();
            // And the walkers re-size from the new part's own measured period. Uptime and
            // achieved-rate counters restart with it, so 't' never mixes two parts' history.
            for (sensors::AdcPairWalker* w : g_walkers) w->begin();
        }
        // Same rule one bus over: an ACK at 0x76/0x77/0x5C/0x5D does not say the part is a depth source.
        if (baroChanged) checkBaro();
        // And the same rule again for the rail monitor: plenty of parts ACK at 0x40.
        if (powerChanged) checkPower();
    }

    // External watchdog heartbeat: HIGH 20 ms, LOW 30 ms, same shape as before. The delays are
    // now service windows: the pair walkers need ~10 ms polling resolution, and a bare 50 ms
    // loop would pace the walkers instead of the converters deciding the rate.
    if (pins::WDT_HEARTBEAT >= 0) digitalWrite(pins::WDT_HEARTBEAT, HIGH);
    serviceWindow(20);
    if (pins::WDT_HEARTBEAT >= 0) digitalWrite(pins::WDT_HEARTBEAT, LOW);
    serviceWindow(30);
}
