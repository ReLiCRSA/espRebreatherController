// INA219 battery-rail monitor driver (AGENTS.md 2 "Sense: V_BAT, ... INA219 (0x40) on I2C0", M3).
//
// Same deliberate thinness as Bmp085/Ms5837: every register constant and conversion lives in
// lib/core/src/PwrRegs.h (transcribed from the deployed Adafruit INA219 library, provenance and
// the 0.1-ohm-shunt assumption stated there and pinned by test/test_power). This layer owns Wire
// transactions and the honest flag chain.
//
// There is no CHIPID on this part, so "presence is not identity" (risk 21) has only one honest
// shape: an ACTIVE check — write the config word, read it back, and take a real bus-voltage
// reading that lands in the plausible rail window. A part that ACKs 0x40 but cannot do that is
// reported, not assumed. Note the flip side the BMP180 episode taught: the check's expectation
// ("rail in 2.5-5.5 V") must match THIS design's rail (1S2P LiPo) — an expectation is a
// measurement with a source, and the source here is the power design, stated in PwrRegs.h.
//
// Two transcription behaviours kept because the deployed library documents them as load-bearing:
//  1. CAL is re-written before every CURRENT/POWER read: "sometimes a sharp load will reset the
//     INA219, which will reset the cal register, meaning CURRENT and POWER will not be
//     available" — a solenoid pulsing ~1 A off this rail is exactly that sharp load.
//  2. CONFIG is also re-written and verified there, one extra 4-byte transaction, because the
//     same reset clears CONFIG and a power-down-mode part silently reports zeros forever.
//     On a life-support rail the redundancy is the point; cost is ~0.1 ms at 400 kHz.
//
// STATUS: part not fitted as of 2026-11-20 — driver written, bench 'w' reports it absent, and
// no current figure here is trusted until a meter cross-check runs at the bench (the shunt-ohm
// assumption in PwrRegs.h is the one design-specific factor multiplying every current number).
#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "PwrRegs.h"

namespace sensors {

class Ina219 {
  public:
    explicit Ina219(TwoWire& wire, uint8_t addr = pwr::kAddr7)
        : wire_(wire), addr_(addr), preset_(pwr::kPreset32V2A) {}

    enum class Status : uint8_t {
        OK,
        IO_FAIL,          // a Wire transaction failed
        CFG_MISMATCH,     // config readback != what was written (SPI vs I2C variant? dead part?)
        CAL_MISMATCH,     // calibration word did not stick
        RAIL_IMPLAUSIBLE, // a real bus-voltage read outside the design's rail window
    };

    Status lastStatus() const { return lastStatus_; }
    const pwr::Preset& preset() const { return preset_; }

    // Raw words for the bench, beside the physical units (the same "show the wire" rule as
    // lastUT/lastUP on the baro — an anomaly must be diagnosable from the capture alone).
    uint16_t lastConfigWord() const { return cfgWord_; }
    uint16_t lastBusWord() const { return busWord_; }
    int16_t lastShuntRaw() const { return shuntRaw_; }
    int16_t lastCurrentRaw() const { return curRaw_; }
    int16_t lastPowerRaw() const { return pwrRaw_; }

    // Reset, then write + verify CAL and CONFIG. Writes registers, so boot and presence
    // transitions only (same rule as the other active checks).
    bool init() {
        if (!writeReg(pwr::kRegConfig, pwr::kCfgReset)) { lastStatus_ = Status::IO_FAIL; return false; }
        delay(2);  // power-on reset family wait; the deployed library does not state one for the
                   // command — 2 ms is a conservative floor, and the readbacks below are what
                   // actually prove the part took it.
        if (!writeReg(pwr::kRegCalibration, preset_.cal)) { lastStatus_ = Status::IO_FAIL; return false; }
        if (!writeReg(pwr::kRegConfig, preset_.config)) { lastStatus_ = Status::IO_FAIL; return false; }
        uint16_t cfg = 0, cal = 0;
        if (!readReg(pwr::kRegConfig, cfg) || !readReg(pwr::kRegCalibration, cal)) {
            lastStatus_ = Status::IO_FAIL;
            return false;
        }
        cfgWord_ = cfg;
        if (cfg != preset_.config) { lastStatus_ = Status::CFG_MISMATCH; return false; }
        if (cal != preset_.cal) { lastStatus_ = Status::CAL_MISMATCH; return false; }
        lastStatus_ = Status::OK;
        return true;
    }

    // One battery-rail sample. vBatt/currentA are optional (nullptr allowed); shuntmV is the
    // pre-scaling measurement path and stays exposed so a meter-vs-chip cross-check can be
    // done on the rawest number the chip offers.
    bool read(float* vBatt, float* currentA) {
        if (!writeReg(pwr::kRegConfig, preset_.config) ||
            !writeReg(pwr::kRegCalibration, preset_.cal)) {
            lastStatus_ = Status::IO_FAIL;  // the library's sharp-load re-write, kept (see header)
            return false;
        }
        uint16_t cfg = 0;
        if (!readReg(pwr::kRegConfig, cfg)) { lastStatus_ = Status::IO_FAIL; return false; }
        cfgWord_ = cfg;
        if (cfg != preset_.config) { lastStatus_ = Status::CFG_MISMATCH; return false; }

        if (!readReg(pwr::kRegBusVoltage, busWord_)) { lastStatus_ = Status::IO_FAIL; return false; }
        const float v = pwr::busVolts(busWord_);
        if (v < pwr::kRailPlausibleMinV || v > pwr::kRailPlausibleMaxV) {
            lastStatus_ = Status::RAIL_IMPLAUSIBLE;  // not hidden: reported with the raw word
            return false;
        }
        int16_t sh = 0, cur = 0, pw = 0;
        if (!readRegS(pwr::kRegShuntVoltage, sh) || !readRegS(pwr::kRegCurrent, cur) ||
            !readRegS(pwr::kRegPower, pw)) {
            lastStatus_ = Status::IO_FAIL;
            return false;
        }
        shuntRaw_ = sh;
        curRaw_ = cur;
        pwrRaw_ = pw;
        if (vBatt) *vBatt = v;
        if (currentA) *currentA = pwr::current_mA(cur, preset_.divider_mA) * 0.001f;
        lastStatus_ = Status::OK;
        return true;
    }

    float shuntmV() const { return pwr::shuntmV(shuntRaw_); }
    float power_mW() const { return pwr::power_mW(pwrRaw_, preset_.mult_mW); }

  private:
    bool writeReg(uint8_t reg, uint16_t value) {
        wire_.beginTransmission(addr_);
        wire_.write(reg);
        wire_.write(static_cast<uint8_t>(value >> 8));
        wire_.write(static_cast<uint8_t>(value & 0xFF));
        return wire_.endTransmission(true) == 0;
    }
    bool readReg(uint8_t reg, uint16_t& out) {
        wire_.beginTransmission(addr_);
        wire_.write(reg);
        if (wire_.endTransmission(false) != 0 || wire_.requestFrom(addr_, (uint8_t)2) != 2)
            return false;
        const uint16_t msb = wire_.read(), lsb = wire_.read();
        out = static_cast<uint16_t>((msb << 8) | lsb);
        return true;
    }
    bool readRegS(uint8_t reg, int16_t& out) {
        uint16_t u = 0;
        if (!readReg(reg, u)) return false;
        out = static_cast<int16_t>(u);
        return true;
    }

    TwoWire& wire_;
    uint8_t addr_;
    pwr::Preset preset_;
    Status lastStatus_ = Status::IO_FAIL;
    uint16_t cfgWord_ = 0, busWord_ = 0;
    int16_t shuntRaw_ = 0, curRaw_ = 0, pwrRaw_ = 0;
};

}  // namespace sensors
