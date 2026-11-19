#include "BusProbe.h"

#include "config/Features.h"   // BENCH_RAW_PROBE
#include "esp32-hal-i2c.h"     // i2cIsInit(): whether the Wire port actually initialised

namespace sensors {

bool sdaHeldLow(int sdaPin) {
    if (sdaPin < 0) return false;
    // MEASURED TRAP (2026-11-19): in Arduino core 4.x, `pinMode()` on a pin the running driver
    // has claimed goes through periman, which **detaches and deinits that bus**. Calling this
    // while the Wire port is initialised therefore tears the bus down underneath itself — every
    // later transaction answers "bus is not initialized" and i2cIsInit() goes to 0. It is only
    // safe when the port is down: before begin(), or after end() in the recovery path.
    pinMode(sdaPin, INPUT_PULLUP);
    delayMicroseconds(50); // let the internal pull-up (45 kOhm typ) actually lift the line
    return digitalRead(sdaPin) == LOW;
}

bool recoverBus(TwoWire&, int sdaPin, int sclPin) {
    if (sdaPin < 0 || sclPin < 0) return false;
    pinMode(sdaPin, INPUT_PULLUP);
    pinMode(sclPin, OUTPUT);
    // 9 clocks: any device caught mid-byte finishes it and releases. Then a STOP: SDA rising
    // while SCL is high.
    for (int i = 0; i < 9; ++i) {
        digitalWrite(sclPin, LOW);
        delayMicroseconds(5);
        digitalWrite(sclPin, HIGH);
        delayMicroseconds(5);
    }
    pinMode(sdaPin, OUTPUT);
    digitalWrite(sdaPin, LOW);
    delayMicroseconds(5);
    digitalWrite(sclPin, HIGH);
    delayMicroseconds(5);
    digitalWrite(sdaPin, HIGH); // STOP
    delayMicroseconds(5);
    return !sdaHeldLow(sdaPin);
}

ProbeResult probeAddress(TwoWire& wire, uint8_t address) {
    ProbeResult r;
    // Measured on a bare devkit on 2026-11-19. Four transaction shapes, and only one is usable:
    //
    //   beginTransmission(a); endTransmission(false)   -> **0 at every address**, with nothing
    //       connected. The repeated-start form never completes the address phase in the IDF-5
    //       driver, so the error never surfaces. This is what made the first boot report
    //       "ALL SENSORS OK" on an empty bench — it must never be used as a probe.
    //   beginTransmission(a); endTransmission(true)    -> 2 (ESP_ERR_INVALID_RESPONSE, NACK on
    //       the address phase) when absent. This is the probe: the driver runs
    //       i2c_master_probe() for a zero-length write, evaluates the address phase, and writes
    //       nothing to the device.
    //   beginTransmission(a); write(0x00); endTransmission(true)
    //                                                  -> 4 (ESP_ERR_INVALID_STATE / timeout) on
    //       an absent address, and a **real write** to a present one: 0x00 into the INA219 config
    //       register clears its calibration, into the ADS1115 config register reprograms
    //       mux/gain/PS1. Banned as a probe.
    //   requestFrom(a, 1)                              -> 0 bytes plus a driver error line; the
    //       SSD1306 is write-only and never answers a read even when present. Cannot prove
    //       absence, and it spams the console. Not used.
    //
    // So: no payload, STOP condition, and `err == 0` means a device acknowledged its address.
    // Positive ACK has still not been observed on this hardware (no sensors fitted yet); the
    // first wired ADS1115 must show err=0 here before anything else about it is trusted.
    wire.beginTransmission(address);
    r.err = wire.endTransmission(true);
    r.ok = (r.err == 0);
    return r;
}

static ScanResult scanWith(TwoWire& wire, const uint8_t* list, int n, int sdaPin, int sclPin) {
    ScanResult r;
    r.initialized = i2cIsInit(wire.getBusNum());
    if (!r.initialized) return r; // probing an uninitialised port only produces driver error spam
    // No stuck-SDA check here, deliberately: detecting it needs pinMode() on the SDA line, which
    // detaches the pin from the live driver and deinits the port (see sdaHeldLow). A stuck slave
    // makes i2cInit fail instead, and initBus reports that as a bus fault — which is the honest
    // answer. Recovery is the explicit bench path in main.cpp: end(), 9 clocks, re-init.
    if (list) {
        for (int i = 0; i < n; ++i) {
            if (probeAddress(wire, list[i]).ok) {
                ++r.count;
                if (r.count <= 16) r.addresses[r.count - 1] = list[i];
            }
        }
    } else {
        for (uint8_t a = 8; a <= 0x77; ++a) { // 0x00-0x07 and 0x78-0x7F are reserved
            if (probeAddress(wire, a).ok) {
                ++r.count;
                if (r.count <= 16) r.addresses[r.count - 1] = a;
            }
        }
    }
    return r;
}

ScanResult scanBus(TwoWire& wire, int sdaPin, int sclPin) {
    return scanWith(wire, nullptr, 0, sdaPin, sclPin);
}

ScanResult scanKnown(TwoWire& wire, const uint8_t* addresses, int n, int sdaPin, int sclPin) {
    return scanWith(wire, addresses, n, sdaPin, sclPin);
}

void probeReport(TwoWire& wire, const uint8_t* addresses, int n, int sdaPin, int sclPin) {
#if BENCH_RAW_PROBE
    for (int i = 0; i < n; ++i) {
        const uint8_t a = addresses[i];

        // The four shapes side by side: this is how the "every address ACKs" result above was
        // diagnosed, and it is the first thing to reach for when a probe disagrees with what is
        // visibly wired. Remember that shape (3) writes 0x00 to whatever answers it.
        wire.beginTransmission(a);
        const uint8_t e1 = wire.endTransmission(false); // no payload, repeated start
        wire.beginTransmission(a);
        const uint8_t e2 = wire.endTransmission(true);  // no payload, stop  <- the real probe
        wire.beginTransmission(a);
        wire.write(0x00);
        const uint8_t e3 = wire.endTransmission(true);  // payload, stop     <- DESTRUCTIVE
        const uint8_t e4 = static_cast<uint8_t>(wire.requestFrom(static_cast<int>(a), 1));

        Serial.printf("  0x%02X  empty-restart=%u empty-stop=%u write1=%u read1=%u  sda=%d scl=%d\n",
                      a, e1, e2, e3, e4, digitalRead(sdaPin), digitalRead(sclPin));
    }
#else
    (void)wire; (void)addresses; (void)n; (void)sdaPin; (void)sclPin;
    Serial.println(F("  raw probe disabled: build with -DBENCH_RAW_PROBE=1"));
#endif
}

} // namespace sensors
