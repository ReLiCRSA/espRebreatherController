// MS5837-30BA driver — the primary barometer (AGENTS.md 2, M3).
//
// Deliberately thin, same pattern as Bmp085.h: commands, the PROM/CRC, the OSR timing table and
// the whole compensation live in lib/core/src/BaroMs5837.h, where test/test_ms5837 pins them
// against the DS text and the field-deployed BlueRobotics library. This layer owns Wire
// transactions and the honest flag chain only.
//
// STATUS: the part is UNPROCURED (AGENTS.md 20: MS5837/Bar30 still to buy). This driver is
// therefore written-but-never-measured, and says so in every bench line it prints. Timing comes
// from the DS MAX column (the DS's own instruction), which is the one case where a datasheet
// wait is the right input — a conversion read early returns 0 and corrupts the sample (DS text),
// so the wait is a floor, never a target; when a Bar30 is fitted, bench 'b' measures the actual
// conversion period and the interlock-style sizing moves to the measured number (decision 29).
//
// Identity chain ("presence is not identity", risk 21, and decision 36's warning that 0x76 is
// shared with BMP280/BME280): ACK at 0x76 -> reset -> PROM read -> CRC-4 over the factory PROM
// must verify -> C1 sensitivity band classifies 30BA/02BA/unrecognised -> one real conversion
// pair must return non-zero words (DS: an ADC read with no conversion behind it answers 0).
// The W0 product-type bits are REPORTED, not gated: the DS revisions contradict each other on
// the product suffix (see PROVENANCE in BaroMs5837.h), and an unverified expectation is the
// exact failure that condemned a good BMP180 on this bench (item 17).
#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "BaroMs5837.h"
#include "SensorIf.h"

namespace sensors {

class Ms5837 : public Barometer {
  public:
    // 7-bit 0x76 exactly as the DS states it (decision 36 — no address arithmetic, ever).
    explicit Ms5837(TwoWire& wire, uint8_t addr = ms5837::kAddr7,
                    ms5837::OsrIndex osr = ms5837::kDesignOsr)
        : wire_(wire), addr_(addr), osr_(osr) {}

    // 0..30 bar operating range (DS). ~288 m of water against a learned Psurf — the clamp that
    // matters here is the part's ADC limit, not a wall the dive model would ever reach.
    // Model-aware: the C1 band can classify an MS5837-02BA (0..2 bar, DS family), which shares
    // the address, the command set and a valid CRC — a 2-bar die gives a ~10 m ceiling, and the
    // clamp must say so rather than quoting 30 bar for a part that cannot measure it.
    float rangeCeilingBar() const override {
        return model_ == ms5837::Model::k02BA ? 2.0f : ms5837::kMaxAbsBar;
    }

    enum class Status : uint8_t {
        OK,
        IO_FAIL,        // a Wire transaction failed (check bus state before believing "absent")
        CRC_FAIL,       // factory PROM fails its own CRC: not a working MS5837, whatever ACKs
        ZERO_ADC,       // conversion(s) produced 0 — DS says that means no conversion happened
        NO_IDENTITY,    // init() never succeeded; read() called on an unverified part
    };

    Status lastStatus() const { return lastStatus_; }
    bool identityVerified() const { return identityOk_; }
    ms5837::Model model() const { return model_; }

    // Bench visibility: the raw words behind any claim. 'b' prints all of this.
    const ms5837::Prom& prom() const { return prom_; }
    uint8_t crcRead() const { return crcRead_; }
    uint8_t crcComputed() const { return crcComputed_; }
    uint32_t lastD1() const { return d1_; }
    uint32_t lastD2() const { return d2_; }
    int32_t lastTempCd() const { return tempCd_; }
    int64_t lastPCbar() const { return pCbar_; }

    // Reset + PROM + CRC + classification. Writes the RESET command, so boot and presence
    // transitions only — same rule as checkAdcs()/checkBaro() for every write-y check.
    bool init() override {
        identityOk_ = false;
        if (!writeCmd(ms5837::kCmdReset)) { lastStatus_ = Status::IO_FAIL; return false; }
        // DS text gives no t-reset for the command; the deployed library waits 10 ms (provenance
        // in BaroMs5837.h). Floor, not target.
        delay(ms5837::kResetWaitMsDeployed);

        for (int i = 0; i < 8; ++i) {
            uint8_t msb = 0, lsb = 0;
            if (!writeCmd(static_cast<uint8_t>(ms5837::kCmdPromBase + 2 * i))) {
                lastStatus_ = Status::IO_FAIL;
                return false;
            }
            wire_.beginTransmission(addr_);
            wire_.write(ms5837::kCmdAdcRead);  // the PROM-read second half (DS: 16-bit result)
            if (wire_.endTransmission(false) != 0 || wire_.requestFrom(addr_, (uint8_t)2) != 2) {
                lastStatus_ = Status::IO_FAIL;
                return false;
            }
            msb = wire_.read();
            lsb = wire_.read();
            prom_.w[i] = static_cast<uint16_t>((static_cast<uint16_t>(msb) << 8) | lsb);
        }
        crcRead_ = ms5837::w0Crc(prom_.w[0]);
        crcComputed_ = ms5837::crc4(prom_.w);
        if (crcRead_ != crcComputed_) { lastStatus_ = Status::CRC_FAIL; return false; }
        model_ = ms5837::classifyModel(prom_.w[1]);
        identityOk_ = true;  // CRC-verified factory PROM: necessary, not sufficient — see header
        lastStatus_ = Status::OK;
        return true;
    }

    // One D1+D2 pair at the design OSR with DS-MAX waits, compensated with the DS flowchart's
    // second-order step on (the deployed reference applies it every read; the DS flowchart
    // includes it in the standard path). Blocking, ~20 ms + I2C: boot/check path and (later) the
    // 1 Hz-ish dive cadence, never a hot inner loop.
    bool read(float* ambBar, float* tempC) override {
        if (!identityOk_) { lastStatus_ = Status::NO_IDENTITY; return false; }
        if (!startAndFetch(ms5837::cmdD1(osr_), &d1_)) { lastStatus_ = Status::IO_FAIL; return false; }
        if (!startAndFetch(ms5837::cmdD2(osr_), &d2_)) { lastStatus_ = Status::IO_FAIL; return false; }
        if (d1_ == 0 || d2_ == 0) {
            // DS conversion-sequence text: an ADC read with no completed conversion answers 0.
            lastStatus_ = Status::ZERO_ADC;
            return false;
        }
        const ms5837::Compensation c = ms5837::compensate(prom_, d1_, d2_, /*secondOrder=*/true);
        pCbar_ = c.pCbar;
        tempCd_ = c.tempCd;
        if (ambBar) *ambBar = static_cast<float>(pCbar_) * 0.0001f;  // 0.1 mbar -> bar
        if (tempC) *tempC = static_cast<float>(tempCd_) * 0.01f;
        lastStatus_ = Status::OK;
        return true;
    }

  private:
    bool writeCmd(uint8_t cmd) {
        wire_.beginTransmission(addr_);
        wire_.write(cmd);
        return wire_.endTransmission(true) == 0;
    }

    bool startAndFetch(uint8_t cmd, uint32_t* out) {
        if (!writeCmd(cmd)) return false;
        delay((ms5837::convUsMax(osr_) + 999) / 1000);  // DS MAX wait, rounded UP to ms
        wire_.beginTransmission(addr_);
        wire_.write(ms5837::kCmdAdcRead);
        if (wire_.endTransmission(false) != 0 || wire_.requestFrom(addr_, (uint8_t)3) != 3)
            return false;
        *out = (static_cast<uint32_t>(wire_.read()) << 16) |
               (static_cast<uint32_t>(wire_.read()) << 8) | wire_.read();
        return true;
    }

    TwoWire& wire_;
    uint8_t addr_;
    ms5837::OsrIndex osr_;
    ms5837::Prom prom_{};
    bool identityOk_ = false;
    ms5837::Model model_ = ms5837::Model::kUnrecognised;
    Status lastStatus_ = Status::NO_IDENTITY;
    uint8_t crcRead_ = 0, crcComputed_ = 0;
    uint32_t d1_ = 0, d2_ = 0;
    int32_t tempCd_ = 0;
    int64_t pCbar_ = 0;
};

}  // namespace sensors
