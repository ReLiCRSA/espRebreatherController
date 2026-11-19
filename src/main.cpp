// ESP32 CCR rebreather dive computer — boot and bench entry point.
//
// Boot is never fatal (AGENTS.md 4, decision 15). Every missing device is recorded and
// reported; nothing here refuses to start. What is fatal is *arming*, and what is fatal
// mid-dive is a sensor dropping out. The default state is BENCH.
//
// Probing discipline, measured on this hardware 2026-11-19 (see sensors/BusProbe.h):
//  - presence = address phase of a zero-length write with STOP. No other transaction shape is
//    both truthful and non-destructive on Arduino core 3.x / IDF 5.5.
//  - boot probes the fitted address table (7 transactions), never a 112-address sweep: a sweep
//    on a port that failed to init emitted ~200 driver error lines and buried the boot banner.
//    The full sweep is an explicit bench command ('s').
//  - the raw four-shape diagnostic *writes* to whatever answers. It is compiled out unless
//    BENCH_RAW_PROBE=1 (Features.h), and must never run on a unit wired for a dive.
#include <Arduino.h>
#include <esp_log.h>
#include <Wire.h>

// src/ is on the include path via `-Isrc`, so headers are addressed project-relative.
#include "config/Features.h"
#include "config/Pins.h"
#include "sensors/BusProbe.h"

// lib/core is a library directory, so these resolve from lib/core/src.
#include "BootReport.h"

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
        case 'c':
            Serial.println(F("[bus] 9-clock recovery + port re-init:"));
            for (BusState& b : g_buses) {
                sensors::recoverBus(*b.wire, b.sda, b.scl);
                b.wire->end();
                initBus(b);
            }
            break;
        case 'h':
        default:
            Serial.println(F("bench commands: s=full bus scan  p=probe table + report  "
                             "r=raw probe codes (destructive, bench only)  c=bus recovery"));
            break;
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
    printBootReport();

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
        for (DevSpec& d : g_devices) {
            if (!busUp(d.bus)) continue;
            const bool was = d.present;
            d.present = sensors::probeAddress(*d.bus, d.addr).ok;
            if (d.present && !was) {
                Serial.printf("INFO: %s ONLINE @0x%02X\n", d.label, d.addr);
                g_faults.clear(d.code, millis());
            } else if (!d.present && was) {
                Serial.printf("ERROR: %s OFFLINE\n", d.label);
            }
        }
        probeDevices();
    }

    while (Serial.available()) handleCommand(static_cast<char>(Serial.read()));

    if (pins::WDT_HEARTBEAT >= 0) digitalWrite(pins::WDT_HEARTBEAT, HIGH);
    delay(20);
    if (pins::WDT_HEARTBEAT >= 0) digitalWrite(pins::WDT_HEARTBEAT, LOW);
    delay(30);
}
