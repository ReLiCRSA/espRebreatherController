// Bosch BMP085/BMP180 driver — the fallback barometer (AGENTS.md 2, M3).
//
// Deliberately thin: every constant and the whole compensation live in lib/core/src/BaroBosch.h,
// where test/test_baro pins them (decision 28's pattern again — a second copy of the arithmetic in
// src/ is exactly how four transcription faults survived in this algorithm until the 2026-11-20
// cross-check against the deployed Adafruit library).
//
// What this layer owns: Wire transactions, the DS-mandated conversion waits (integer us, from the
// DS table in BaroBosch.h — the same measurement-over-datasheet rule as the ADC, but for this part
// nothing measured contradicts the DS, and 'b' re-checks behaviour on real silicon anyway), the
// cal-ROM read and its DS validity rule, and the honest flag chain: a reading is only true when
// the conversion completed AND the result sits inside the part's 300-1100 hPa range.
//
// Range: this part measures 300-1100 hPa absolute — a water column UNDER 1 m at sea level
// (~1.2 m at a 978 mbar bench), not the "~9 m" the early design record asserted (decision 38;
// test/test_depth pins the hydrostatics). rangeCeilingBar() reports
// that ceiling so Depth can clamp — the part must never be extrapolated past it (risk 1).
#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "BaroBosch.h"
#include "SensorIf.h"

namespace sensors {

class Bmp085 : public Barometer {
  public:
    // addr: the DS-quoted 7-bit 0x77 (Bosch DS Table 7 bits 1110111+R/W) — not 0x3B, which was a
    // phantom from address arithmetic (decision 36).
    // oss=2 (13.5 ms): 18 mm resolution, and the part has no 300 m depth to chase here — its own
    // ceiling is under 1 m. oss=3 would add 12 ms per sample for resolution this sensor cannot use.
    explicit Bmp085(TwoWire& wire, uint8_t addr = 0x77, int oss = 2)
        : wire_(wire), addr_(addr), oss_(oss) {}

    float rangeCeilingBar() const override { return baro::kRangeMaxBar; }

    enum class Status : uint8_t { OK, NO_CHIPID, CAL_ROM_BAD, NO_CONV, OUT_OF_RANGE };

    Status lastStatus() const { return lastStatus_; }
    int calRomBadIndex() const { return calBadIdx_; }  // Table 5 index, when CAL_ROM_BAD
    // Raw chain for the bench: what actually came off the wire, next to the physical units.
    uint16_t lastUt() const { return lastUt_; }
    uint32_t lastUp() const { return lastUp_; }
    int32_t lastB5() const { return lastB5_; }
    // Cal ROM after a successful init(): bench 'b' prints all 11 words. An absolute-pressure
    // error can only come from the part's trim or its analog front end — the words are what
    // distinguish those, and both compensation implementations read the same words, so
    // their agreement proves the arithmetic, never the trim.
    const baro::Cal& calRom() const { return cal_; }

    bool init() override {
        calBadIdx_ = -1;
        uint8_t id = 0;
        if (!readReg(0xD0, id) || id != baro::kChipIdBmp085180) {
            lastStatus_ = Status::NO_CHIPID;  // DS memory map: D0h = 55h
            return false;
        }
        // 0xAA..0xBF, 11 words big-endian, Table 5 order (DS-verified).
        int16_t w[baro::kCalWords];
        for (int i = 0; i < baro::kCalWords; ++i) {
            uint8_t msb = 0, lsb = 0;
            if (!readReg(static_cast<uint8_t>(baro::kCalFirstReg + 2 * i), msb) ||
                !readReg(static_cast<uint8_t>(baro::kCalFirstReg + 2 * i + 1), lsb)) {
                lastStatus_ = Status::CAL_ROM_BAD;
                calBadIdx_ = i;  // the read itself failed at this word
                return false;
            }
            w[i] = static_cast<int16_t>((static_cast<uint16_t>(msb) << 8) | lsb);
        }
        cal_.ac1 = w[0];  cal_.ac2 = w[1];  cal_.ac3 = w[2];
        cal_.ac4 = static_cast<uint16_t>(w[3]); cal_.ac5 = static_cast<uint16_t>(w[4]);
        cal_.ac6 = static_cast<uint16_t>(w[5]);
        cal_.b1 = w[6];   cal_.b2 = w[7];
        cal_.mb = w[8];   cal_.mc = w[9];   cal_.md = w[10];
        calBadIdx_ = baro::calRomFirstBadWord(cal_);
        if (calBadIdx_ >= 0) {  // the DS's own rule: no word 0 or 0xFFFF
            lastStatus_ = Status::CAL_ROM_BAD;
            return false;
        }
        lastStatus_ = Status::OK;
        return true;
    }

    // Blocking single temperature + pressure cycle, DS order (temperature first: pressure
    // compensation needs its B5). Waits use the DS max conversion times from BaroBosch.h.
    // false carries a reason in lastStatus_ — a failed read must reach the dive loop as a fault,
    // never as a repeated stale pressure (Barometer contract).
    bool read(float* ambBar, float* tempC) override {
        uint8_t msb = 0, lsb = 0, xlsb = 0;
        // The waits are not fallible transactions — the reads around them are. Chain accordingly.
        if (!startConvert(baro::kCmdTemperature)) {
            lastStatus_ = Status::NO_CONV;
            return false;
        }
        waitUs(baro::kTempConvUs);
        if (!readReg(baro::kRegAdcMsb, msb) || !readReg(baro::kRegAdcLsb, lsb)) {
            lastStatus_ = Status::NO_CONV;
            return false;
        }
        const uint16_t ut = baro::assembleUt(msb, lsb);

        if (!startConvert(baro::pressureCommand(oss_))) {
            lastStatus_ = Status::NO_CONV;
            return false;
        }
        waitUs(baro::pressureConvUs(oss_));
        if (!readReg(baro::kRegAdcMsb, msb) || !readReg(baro::kRegAdcLsb, lsb) ||
            !readReg(baro::kRegAdcXlsb, xlsb)) {
            lastStatus_ = Status::NO_CONV;
            return false;
        }
        // XLSB always read: at oss>0 the 0xF6/0xF7 pair is only the top 16 bits (DS 5.7), and the
        // deployed reference reads it that way too.
        const uint32_t up = baro::assembleUp(msb, lsb, xlsb, true, oss_);

        const baro::Reading r = baro::compensate(cal_, ut, up, oss_);
        lastUt_ = ut;
        lastUp_ = up;
        lastB5_ = r.b5;
        if (!r.converted) {
            lastStatus_ = Status::CAL_ROM_BAD;  // ROM rejected mid-flight: refuse, do not report
            return false;
        }
        if (!r.inRange) {  // outside the part's own 300-1100 hPa: a flag, never an extrapolation
            lastStatus_ = Status::OUT_OF_RANGE;
            return false;
        }
        if (ambBar) *ambBar = r.pressurePa / 100000.0f;
        if (tempC) *tempC = r.tempC;
        lastAmbBar_ = r.pressurePa / 100000.0f;
        lastTempC_ = r.tempC;
        lastStatus_ = Status::OK;
        return true;
    }

    float lastAmbBar() const { return lastAmbBar_; }
    float lastTempC() const { return lastTempC_; }

  private:
    bool readReg(uint8_t reg, uint8_t& out) {
        wire_.beginTransmission(addr_);
        wire_.write(reg);
        if (wire_.endTransmission(false) != 0) return false;
        if (wire_.requestFrom(addr_, (uint8_t)1) != 1) return false;
        out = wire_.read();
        return true;
    }
    bool startConvert(uint8_t cmd) {
        wire_.beginTransmission(addr_);
        wire_.write(baro::kRegCtrlMeas);
        wire_.write(cmd);
        return wire_.endTransmission(true) == 0;
    }
    static void waitUs(uint32_t us) { delayMicroseconds(us + 500); }  // DS max + small margin

    TwoWire& wire_;
    uint8_t addr_;
    int oss_;
    baro::Cal cal_{};
    Status lastStatus_ = Status::NO_CHIPID;
    int calBadIdx_ = -1;
    uint16_t lastUt_ = 0;
    uint32_t lastUp_ = 0;
    int32_t lastB5_ = 0;
    float lastAmbBar_ = 0.0f;
    float lastTempC_ = 0.0f;
};

}  // namespace sensors
