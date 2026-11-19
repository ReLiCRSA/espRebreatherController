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

// lib/core is a library directory, so these resolve from lib/core/src.
#include "BootReport.h"

// lib/sim is also a library directory. The headless bench run ('v') drives the same sim stack
// the native test suite exercises (AGENTS.md 11, M1 gate: "sim profile runs headless"); it is
// pure C++ and touches no pin.
#include "SimRunner.h"

namespace {

boot::Report g_faults;

// All addresses are 7-bit, which is what Wire takes. The baro datasheets quote 8-bit forms:
// MS5837 "0x7C" -> 0x3E, BMP085 "0x77" -> 0x3B. Getting this wrong means the unit never finds
// its depth sensor and reports it missing forever.
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
    {FaultCode::BARO, 0x3E, &Wire, "MS5837", false},
    {FaultCode::BARO, 0x3B, &Wire, "BMP085", false},
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

    // Baro selection: MS5837 primary; BMP085 only arms with a permanent 9 m clamp banner.
    bool ms5837 = false, bmp085 = false;
    for (const DevSpec& d : g_devices) {
        if (d.code != FaultCode::BARO || !d.present) continue;
        if (d.addr == 0x3E) ms5837 = true;
        if (d.addr == 0x3B) bmp085 = true;
    }
    if (ms5837) {
        // Either the MS5837 answered at boot, or it hot-plugged later: in both cases the
        // fallback clamp no longer applies.
        g_faults.clear(FaultCode::BARO_RANGE, millis());
        g_faults.clear(FaultCode::BARO, millis());
    } else if (bmp085) {
        g_faults.raise(FaultCode::BARO_RANGE, Severity::ERROR, millis(), "depth clamps 9 m");
        g_faults.clear(FaultCode::BARO, millis());
    } else {
        g_faults.raise(FaultCode::BARO, Severity::ERROR, millis(), "no MS5837/BMP085");
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
            static const uint8_t kDiag[] = {0x3C, 0x3E, 0x3B, 0x40, 0x48, 0x49};
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
                             "interlock window  g=pad map + pad test  "
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
        for (DevSpec& d : g_devices) {
            if (!busUp(d.bus)) continue;
            const bool was = d.present;
            d.present = sensors::probeAddress(*d.bus, d.addr).ok;
            if (d.present && !was) {
                Serial.printf("INFO: %s ONLINE @0x%02X\n", d.label, d.addr);
                g_faults.clear(d.code, millis());
                if (d.bus == &Wire1) adcChanged = true;
            } else if (!d.present && was) {
                Serial.printf("ERROR: %s OFFLINE\n", d.label);
                if (d.bus == &Wire1) adcChanged = true;
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
    }

    // External watchdog heartbeat: HIGH 20 ms, LOW 30 ms, same shape as before. The delays are
    // now service windows: the pair walkers need ~10 ms polling resolution, and a bare 50 ms
    // loop would pace the walkers instead of the converters deciding the rate.
    if (pins::WDT_HEARTBEAT >= 0) digitalWrite(pins::WDT_HEARTBEAT, HIGH);
    serviceWindow(20);
    if (pins::WDT_HEARTBEAT >= 0) digitalWrite(pins::WDT_HEARTBEAT, LOW);
    serviceWindow(30);
}
