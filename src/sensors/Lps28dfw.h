// LPS28DFW driver — tier-2 depth sensor (AGENTS.md 2, decision 39).
//
// Fallback when the MS5837-30BA is absent or failed identity; the BMP180 beneath it is bench-only
// now. Same thin-driver pattern as Bmp085.h/Ms5837.h: every constant and the scaling live in
// lib/core/src/BaroLps28.h (transcribed from ST's own vendor driver), pinned by test/test_lps28.
// This layer owns Wire transactions, the config write-verify, and the honest flag chain.
//
// STATUS: part UNPROCURED as of 2026-11-20 — written-but-never-measured, and the bench lines say
// so. Arrival acceptance is bench 'b' (WHO_AM_I, FS_MODE writeback, a real bench mbar, squeeze
// movement), same acceptance shape as every other part here (decision 29: identity is a
// measurement claim).
//
// Identity, tiered like the rest of the project (presence is not identity, risk 21):
//   ACK @0x5C/0x5D -> WHO_AM_I == 0xB4 -> SWRESET self-clears -> CTRL_REG1/2 written and READ
//   BACK with FS_MODE=1 latched (the dual-FS capability IS this part; the ceiling the dive model
//   uses comes from the readback, never from the write request) -> one real conversion with
//   P_DA set and the result inside the latched full-scale window.
// No PROM/CRC exists on this silicon — that is the part's design, and the active chain above is
// what substitutes for it. Writes config registers, so the driver's init() runs at boot and on
// presence transitions only — same rule as checkAdcs()/checkBaro()/checkPower().
#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "BaroLps28.h"
#include "SensorIf.h"

namespace sensors {

class Lps28dfw : public Barometer {
  public:
    explicit Lps28dfw(TwoWire& wire, uint8_t addr = lps28::kAddr7)
        : wire_(wire), addr_(addr) {}

    // SA0 strap: 0x5C or 0x5D. The presence table knows which one ACKed; checkBaro tells the
    // driver before init(). Changing address invalidates any identity held from the other strap.
    uint8_t address() const { return addr_; }
    void setAddress(uint8_t a) { if (a != addr_) { addr_ = a; identityOk_ = false; } }

    // Reports the ceiling of the full-scale mode ACTUALLY LATCHED (verified readback), per
    // decision 38's rule: the clamp number comes from the part's state, not from the design
    // record's prose. Mode 2 = 4.06 bar abs; Depth turns that into the computed ~31 m banner.
    float rangeCeilingBar() const override { return lps28::rangeCeilingBar(fsMode2_); }
    bool fsMode2() const { return fsMode2_; }

    enum class Status : uint8_t {
        OK,
        IO_FAIL,        // a Wire transaction failed (bus state first, before believing "absent")
        NO_IDENTITY,    // init() never passed; read() called on an unverified part
        WRONG_ID,       // WHO_AM_I != 0xB4 — something else answers at 0x5C/0x5D
        RESET_STUCK,    // SWRESET never self-cleared within the timeout
        FS_NOT_LATCHED, // FS_MODE write did not read back — not a dual-FS part, or reg write failed
        NO_DATA,        // P_DA never set: not converting
        OUT_OF_RANGE,   // conversion landed outside the latched full-scale window
    };

    Status lastStatus() const { return lastStatus_; }
    bool identityVerified() const { return identityOk_; }

    // Bench visibility: the raw words behind any claim ('b' prints these).
    uint8_t whoAmI() const { return whoAmI_; }
    uint8_t lastCtrl1() const { return ctrl1Rb_; }
    uint8_t lastCtrl2() const { return ctrl2Rb_; }
    int32_t lastRawAligned() const { return rawAligned_; }
    float lastAmbBar() const { return lastAmbBar_; }
    float lastTempC() const { return lastTempC_; }

    // WHO_AM_I + SWRESET + config + readback verify. Writes registers: boot/transition only.
    bool init() override {
        identityOk_ = false;
        if (!readReg(lps28::kRegWhoAmI, whoAmI_)) { lastStatus_ = Status::IO_FAIL; return false; }
        if (whoAmI_ != lps28::kWhoAmI) { lastStatus_ = Status::WRONG_ID; return false; }

        // Soft reset, then POLL the self-clearing bit (vendor exposes it; no recited DS wait —
        // risk 21 says measure, and here polling IS the measurement).
        uint8_t r2 = 0;
        if (!readReg(lps28::kRegCtrl2, r2)) { lastStatus_ = Status::IO_FAIL; return false; }
        r2 |= (1u << lps28::kBitSwReset);
        if (!writeReg(lps28::kRegCtrl2, r2)) { lastStatus_ = Status::IO_FAIL; return false; }
        const uint32_t t0 = millis();
        bool cleared = false;
        while (millis() - t0 < lps28::kSwResetTimeoutMs) {
            if (!readReg(lps28::kRegCtrl2, r2)) { lastStatus_ = Status::IO_FAIL; return false; }
            if (!(r2 & (1u << lps28::kBitSwReset))) { cleared = true; break; }
            delay(2);
        }
        if (!cleared) { lastStatus_ = Status::RESET_STUCK; return false; }

        // Design config: continuous ODR 10 Hz + AVG 4 (CTRL_REG1), BDU + FS_MODE=1 (CTRL_REG2).
        // BDU because the dive path reads the 3-byte pressure word without a burst transaction:
        // BDU holds the LSBs until the MSB is read (vendor DRV_RDY sets it for the same reason).
        const uint8_t wantCtrl1 =
            static_cast<uint8_t>((lps28::kDesignOdr << lps28::kOdrShift) | lps28::kDesignAvg);
        const uint8_t wantCtrl2 =
            static_cast<uint8_t>((1u << lps28::kBitFsMode) | (1u << lps28::kBitBdu));
        if (!writeReg(lps28::kRegCtrl1, wantCtrl1)) { lastStatus_ = Status::IO_FAIL; return false; }
        if (!writeReg(lps28::kRegCtrl2, wantCtrl2)) { lastStatus_ = Status::IO_FAIL; return false; }
        if (!readReg(lps28::kRegCtrl1, ctrl1Rb_) || !readReg(lps28::kRegCtrl2, ctrl2Rb_)) {
            lastStatus_ = Status::IO_FAIL;
            return false;
        }
        if (((ctrl1Rb_ >> lps28::kOdrShift) & lps28::kOdrMask) != lps28::kDesignOdr ||
            (ctrl1Rb_ & lps28::kAvgMask) != lps28::kDesignAvg) {
            lastStatus_ = Status::FS_NOT_LATCHED;  // config register does not hold what was written
            return false;
        }
        if (!(ctrl2Rb_ & (1u << lps28::kBitFsMode))) {
            // The single most identity-bearing write on this part: a chip that will not present
            // the 4060-hPa full scale is not an LPS28DFW, and the dive model must NOT clamp at
            // 4.06 bar for it. Fall back below the same way a failed MS5837 identity does.
            lastStatus_ = Status::FS_NOT_LATCHED;
            return false;
        }
        fsMode2_ = true;
        identityOk_ = true;
        lastStatus_ = Status::OK;
        return true;
    }

    // One continuous-mode sample: P_DA gate, 5-byte burst 0x28..0x2C (XL,L,H,Tl,Th — contiguous,
    // and BDU is verified set), vendor scaling, window check. Non-destructive: unlike init(),
    // this is safe on the dive cadence.
    bool read(float* ambBar, float* tempC) override {
        if (!identityOk_) { lastStatus_ = Status::NO_IDENTITY; return false; }

        uint8_t st = 0;
        const uint32_t t0 = millis();
        for (;;) {
            if (!readReg(lps28::kRegStatus, st)) { lastStatus_ = Status::IO_FAIL; return false; }
            if (st & (1u << lps28::kBitPData)) break;
            // P_DA is set by each ODR tick; first read after config must wait one period out,
            // so the same timeout covers both cases. 10 Hz -> one period is 100 ms.
            if (millis() - t0 > 500) { lastStatus_ = Status::NO_DATA; return false; }
            delay(5);
        }

        wire_.beginTransmission(addr_);
        wire_.write(lps28::kRegPressXl);
        if (wire_.endTransmission(false) != 0 || wire_.requestFrom(addr_, (uint8_t)5) != 5) {
            lastStatus_ = Status::IO_FAIL;
            return false;
        }
        const uint8_t xl = wire_.read(), l = wire_.read(), h = wire_.read();
        const uint8_t tl = wire_.read(), th = wire_.read();

        if (st & (1u << 4)) {  // STATUS.P_OR: over-pressure flag — at or past full scale
            lastStatus_ = Status::OUT_OF_RANGE;
            return false;
        }

        rawAligned_ = lps28::pressureRawAligned(xl, l, h);
        const float hPa = lps28::hPaFromRaw(rawAligned_, fsMode2_);
        const float amb = hPa / 1000.0f;
        const float tc = lps28::tempCFromRaw(tl, th);
        if (!lps28::inRangeBar(amb, fsMode2_)) {  // a flag, never an extrapolation (risk 1)
            lastStatus_ = Status::OUT_OF_RANGE;
            return false;
        }
        lastAmbBar_ = amb;
        lastTempC_ = tc;
        if (ambBar) *ambBar = amb;
        if (tempC) *tempC = tc;
        lastStatus_ = Status::OK;
        return true;
    }

  private:
    bool readReg(uint8_t reg, uint8_t& out) {
        wire_.beginTransmission(addr_);
        wire_.write(reg);
        if (wire_.endTransmission(false) != 0) return false;
        if (wire_.requestFrom(addr_, (uint8_t)1) != 1) return false;
        out = wire_.read();
        return true;
    }
    bool writeReg(uint8_t reg, uint8_t val) {
        wire_.beginTransmission(addr_);
        wire_.write(reg);
        wire_.write(val);
        return wire_.endTransmission(true) == 0;
    }

    TwoWire& wire_;
    uint8_t addr_;
    bool identityOk_ = false;
    bool fsMode2_ = false;
    uint8_t whoAmI_ = 0, ctrl1Rb_ = 0, ctrl2Rb_ = 0;
    int32_t rawAligned_ = 0;
    float lastAmbBar_ = 0.0f, lastTempC_ = 0.0f;
    Status lastStatus_ = Status::NO_IDENTITY;
};

}  // namespace sensors
