// I2C bus presence probing (AGENTS.md 4: every missing device must be reported).
//
// Two hard lessons live in this file, both measured on real hardware on 2026-11-19 with a bare
// devkit — before them the boot report confidently printed sixteen phantom devices per bus and
// "ALL SENSORS OK" with nothing connected.
//
// 1. Addresses here are 7-bit, because that is what `Wire` takes. Datasheets are inconsistent,
//    and address arithmetic on a quoted number is banned — corrected 2026-11-20 against the
//    datasheets themselves after "halving" produced two phantom addresses (0x3E/0x3B) that no
//    part ever answered at. Bosch BMP085/180 DS Table 7: address bits 1110111 + R/W = **0x77**
//    (bus bytes 0xEE/0xEF). TE MS5837 DS: "Address (7 bits) **0x76** (1110110 b)"; the "0x7C"
//    that circulates in driver code is its 8-bit write byte. The ADS1115 (0x48/0x49), SSD1306
//    (0x3C/0x3D) and INA219 (0x40) are already 7-bit in their datasheets. Take the 7-bit form
//    the datasheet states, and confirm with a bus sweep — never divide a number in half.
//
// 2. A zero-length write is not a presence test, and writing a byte to probe is destructive.
//    See probeAddress() for the four transaction shapes and what each one actually returns.
#pragma once

#include <Arduino.h>
#include <Wire.h>

namespace sensors {

struct ProbeResult {
    bool ok = false;
    // Wire::endTransmission() code from the address phase: 0 = acknowledged (device present),
    // 2 = NACK on address (nothing there), 4 = timeout (bus dead / driver unhappy).
    uint8_t err = 0;
};

struct ScanResult {
    uint8_t addresses[16];
    int count = 0;
    bool initialized = false; // the Wire port was not initialised: results are meaningless
    bool stuckSda = false;    // set by the pre-init line check, never by a scan (see sdaHeldLow)
    bool recovered = false;
};

// SDA low with our pull-up enabled and nothing addressed means a device is holding it.
// **Only valid while the Wire port is down** (before begin(), or after end()): pinMode() on a
// pin the driver has claimed detaches and deinits that bus — measured 2026-11-19.
bool sdaHeldLow(int sdaPin);

// Clock 9 pulses with SDA released, then a STOP. Freed device state machines let go of SDA.
// Call only with the port down (end() first), for the same periman reason as sdaHeldLow.
bool recoverBus(TwoWire& wire, int sdaPin, int sclPin);

// Non-destructive presence test: address phase + STOP, no payload. The only shape that is both
// truthful and safe (probeAddress in the .c explains the measured alternatives).
ProbeResult probeAddress(TwoWire& wire, uint8_t address);

// One pass over the assignable 7-bit range (0x08..0x77). 112 transactions: a **bench command**,
// not something to run at boot. Boot probes the fitted address table instead, so a dead port
// cannot emit two hundred driver error lines over the boot banner.
ScanResult scanBus(TwoWire& wire, int sdaPin, int sclPin);

// Scan only the addresses we actually fit. This is what boot uses. Neither scan checks for a
// stuck SDA line, because that check would tear the live port down (see sdaHeldLow); a stuck
// slave shows up as i2cInit failing at boot instead.
ScanResult scanKnown(TwoWire& wire, const uint8_t* addresses, int n, int sdaPin, int sclPin);

// Writes 0x00 to each address and prints the raw driver codes for all four transaction shapes.
// **DESTRUCTIVE** — 0x00 into the INA219 config register clears its calibration, and into the
// ADS1115 config register reprograms the mux/gain/PS1 bit. Bench bring-up only, compiled out
// unless BENCH_RAW_PROBE=1, and never on a unit with sensors wired for a dive.
void probeReport(TwoWire& wire, const uint8_t* addresses, int n, int sdaPin, int sclPin);

} // namespace sensors
