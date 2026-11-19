// ADS1115 register driver — see Ads1115Bus.h for the design rationale and for the measurements
// that fixed the config-register field positions.
#include "Ads1115Bus.h"

namespace sensors {

namespace {
// Written by selfTest() to make the pointer-register test deterministic: the DS says the power-on
// value is undefined, so comparing against a value it refuses to guarantee proves nothing.
constexpr uint8_t kPtrProbeReg = Ads1115::kRegLoThresh;
} // namespace

uint8_t Ads1115::probeId(uint8_t* idOut) {
    uint8_t err = 0;
    wire_.beginTransmission(addr_);
    wire_.write(kRegId);
    err = wire_.endTransmission(true);
    if (err) return err;
    if (wire_.requestFrom(static_cast<int>(addr_), 1) != 1) return 10;
    id_ = wire_.read();
    if (idOut) *idOut = id_;
    return 0;
}

bool Ads1115::writeReg(uint8_t reg, uint16_t value) {
    wire_.beginTransmission(addr_);
    wire_.write(reg);
    wire_.write(static_cast<uint8_t>(value >> 8));  // the ADS1115 is big-endian
    wire_.write(static_cast<uint8_t>(value & 0xFF));
    return wire_.endTransmission(true) == 0;
}

bool Ads1115::readReg(uint8_t reg, uint16_t* out) {
    // Two separate transactions on purpose: the combined write-then-repeated-start form is the
    // transaction shape that measured unreliable on this core (BusProbe.cpp, decision 25). A STOP
    // between the phases costs nothing here and removes any doubt the answer came from a read.
    wire_.beginTransmission(addr_);
    wire_.write(reg);
    if (wire_.endTransmission(true) != 0) return false;
    if (wire_.requestFrom(static_cast<int>(addr_), 2) != 2) return false;
    const uint16_t hi = wire_.read();
    const uint16_t lo = wire_.read();
    if (out) *out = static_cast<uint16_t>((hi << 8) | lo);
    return true;
}

bool Ads1115::startSingleShot(uint16_t mux, uint16_t gain, uint16_t sps) {
    // OS=1 starts a conversion and reads back 1 when it is done. MODE is set explicitly to
    // single-shot: leaving that bit clear selects continuous mode, in which the mux is locked to one
    // channel and OS never returns — so the two-pairs-per-converter scheme of AGENTS.md 2 would be
    // impossible, and the converter would be integrating continuously under a coil pulse (risk 4).
    return writeReg(kRegConfig, static_cast<uint16_t>(kOsBusy | kModeSingleShot | mux | gain | sps));
}

bool Ads1115::waitReady(uint32_t timeoutMs) {
    const uint32_t start = millis();
    uint16_t cfg = 0;
    for (;;) {
        if (!readReg(kRegConfig, &cfg)) return false;
        if ((cfg & kOsBusy) != 0) return true;
        if (millis() - start >= timeoutMs) return false;
        delay(1);
    }
}

Ads1115::PartCheck Ads1115::checkPart(uint32_t* convUsOut, unsigned* pollsOut) {
    if (!startSingleShot(MUX_AIN0_AIN1, kGainDesign, kSps32)) return PartCheck::IO_FAIL;

    const uint32_t t0 = micros();
    uint16_t cfg = 0;
    unsigned polls = 0;
    for (;;) {
        if (!readReg(kRegConfig, &cfg)) return PartCheck::IO_FAIL;
        ++polls;
        if (cfg & kOsBusy) {
            const uint32_t us = micros() - t0;
            if (convUsOut) *convUsOut = us;
            if (pollsOut) *pollsOut = polls;
            // An ADS1115 at 32 SPS cannot finish faster than ~31 ms. Coming back far below that
            // means the rate table belongs to another part: an ADS1015 answers this address, this
            // register map and these field positions, and would sample at 3200 SPS while the control
            // loop assumed 32 — 100x more often, with a 12-bit code instead of 16.
            if (us < kConvAcceptMinUs) return PartCheck::TOO_FAST_OTHER_PART;
            if (us > kConvAcceptMaxUs) return PartCheck::TOO_SLOW;
            return PartCheck::OK;
        }
        if (micros() - t0 >= 250000) {
            if (pollsOut) *pollsOut = polls;
            return PartCheck::NO_CONVERSION;  // OS never returned: continuous mode, or not running
        }
    }
}

void Ads1115::measureAdcRates(Print& out) {
    char line[120];
    // Poll overhead first: the measurement resolution is one config-register read, so quoting a
    // period without it would overstate the accuracy at the fast end.
    const uint32_t tPoll = micros();
    uint16_t dummy = 0;
    for (int i = 0; i < 20; ++i) readReg(kRegConfig, &dummy);
    const uint32_t pollUs = (micros() - tPoll) / 20;
    snprintf(line, sizeof(line), "  config-read granularity: %lu us/poll",
             static_cast<unsigned long>(pollUs));
    out.println(line);

    for (uint16_t field = 0; field < 8; ++field) {
        const uint16_t rate = static_cast<uint16_t>(field << 4);
        if (!startSingleShot(MUX_AIN0_AIN1, kGainDesign, rate)) {
            out.println(F("  rate write FAILED"));
            return;
        }
        const uint32_t t0 = micros();
        uint16_t cfg = 0;
        bool done = false;
        for (;;) {
            if (!readReg(kRegConfig, &cfg)) break;
            if (cfg & kOsBusy) { done = true; break; }
            if (micros() - t0 >= 400000) break;
        }
        const uint32_t us = micros() - t0;
        // Measured SPS is what the scheduler must use. The DS column is there so a divergence is
        // visible on the bench instead of inferred from a bad reading later.
        snprintf(line, sizeof(line), "  rate field %d: DS %4d SPS (%6lu us) | measured %6lu us = %5.1f SPS %s",
                 field, adc::spsField(field), static_cast<unsigned long>(adc::convUsField(field)),
                 static_cast<unsigned long>(done ? us : 0),
                 done ? 1000000.0 / static_cast<double>(us) : 0.0, done ? "" : "** NO CONVERSION **");
        out.println(line);
    }
    // Leave the converter where the design expects it.
    startSingleShot(MUX_AIN0_AIN1, kGainDesign, kSps32);
}

void Ads1115::selfTest(Print& out) {
    char line[120];

    const uint8_t idErr = probeId();
    if (idErr) {
        snprintf(line, sizeof(line), "  ADS @0x%02X  ID read FAILED err=%u", addr_, idErr);
        out.println(line);
    } else {
        // Informational. 0x85 is the ADS1115's documented ID; 0x05 is an ADS1015. 0x00 means the
        // register is not implemented — the bench module on 2026-11-19 reads 0x00 here *and* 0x00
        // in the pointer register while its config register reads back the datasheet power-on
        // default exactly, which is the signature of a marked-not-TI die. It works, but it is not
        // a part to take into the water on the strength of its markings.
        snprintf(line, sizeof(line), "  ADS @0x%02X  ID=0x%02X %s", addr_, id_,
                 id_ == 0x85 ? "ADS1115 (as expected)"
                             : (id_ == 0x05 ? "ADS1015 — WRONG PART for this design"
                                            : "register not implemented — unmarked/clone die?"));
        out.println(line);
    }

    uint16_t cfg = 0;
    if (!readReg(kRegConfig, &cfg)) {
        out.println(F("  config read FAILED - register map not responding"));
        return;
    }
    // Decoded through the adc:: helpers, which are asserted by test/test_adc. Note the MODE
    // polarity: bit 8 set = single-shot. Printing this line is what makes a wrong setting visible at
    // the bench instead of a wrong reading later.
    snprintf(line, sizeof(line), "  cfg=0x%04X  mux=%d pga=%d (+/-%.3fV gain%.0f) rate=%d SPS mode=%s os=%d",
             cfg, adc::muxField(cfg), adc::pgaField(cfg), adc::fsrVolts(cfg),
             adc::gainField(adc::pgaField(cfg)), adc::sps(cfg),
             adc::isSingleShot(cfg) ? "single" : "CONTINUOUS", adc::isConverting(cfg) ? 0 : 1);
    out.println(line);
    if (adc::isContinuous(cfg))
        out.println(F("  ** MODE=continuous: mux locked, OS never returns — not usable here **"));
    if (adc::isPowerDownMux(cfg))
        out.println(F("  ** mux is a power-down encoding: the input is floating, not measuring **"));

    // Write-verify on the low-threshold register: read, write a known value, read back, restore.
    // Nothing in the cell chain uses it, so a bring-up run cannot change a measurement. This is
    // what separates "ACKs its address" from "I2C reads and writes actually work".
    uint16_t lo = 0;
    if (!readReg(kRegLoThresh, &lo)) {
        out.println(F("  lo_thresh read FAILED"));
        return;
    }
    const bool wrote = writeReg(kRegLoThresh, 0xA55A);
    uint16_t check = 0;
    const bool back = readReg(kRegLoThresh, &check);
    uint16_t ptr = 0;
    const bool ptrOk = readReg(kRegPtr, &ptr);
    snprintf(line, sizeof(line), "  write-verify lo_thresh: wrote 0xA55A read 0x%04X %s | ptr=0x%04X %s",
             check, (wrote && back && check == 0xA55A) ? "OK" : "**FAILED**", ptr,
             ptrOk ? (ptr == kPtrProbeReg ? "follows the write (as expected)"
                                          : "does not track the write (register absent?)")
                   : "read FAILED");
    out.println(line);
    if (!writeReg(kRegLoThresh, lo)) out.println(F("  ** could not restore lo_thresh **"));

    // DRDY is an open-drain output: it needs a pull-up, and it is only meaningful when wired. A
    // floating reading is normal with nothing connected and must not be reported as a fault.
    if (drdy_ >= 0) {
        pinMode(drdy_, INPUT_PULLUP);
        delayMicroseconds(20);
        snprintf(line, sizeof(line), "  DRDY gpio %d = %d (pulled up; floating if not wired)",
                 drdy_, digitalRead(drdy_));
        out.println(line);
    } else {
        out.println(F("  DRDY pin not assigned - timing verified by polling the OS bit instead"));
    }

    // Conversion test, and the part identity check. With floating inputs the *value* is
    // meaningless — a differential pair with no bias network reads anything (AGENTS.md 19 risk 6:
    // missing 1 MOhm resistors gives exactly this, which is why they are mandatory before any cell
    // reading is believed). What is meaningful is that a conversion starts, completes, and takes
    // the time the datasheet says: the coil-pulse interlock in AGENTS.md 8 is sized on that.
    uint32_t convUs = 0;
    unsigned polls = 0;
    const PartCheck pc = checkPart(&convUs, &polls);
    switch (pc) {
        case PartCheck::OK: {
            uint16_t conv = 0;
            if (!readReg(kRegConv, &conv)) {
                out.println(F("  conv register read FAILED"));
                return;
            }
            const int16_t raw = static_cast<int16_t>(conv);
            snprintf(line, sizeof(line),
                     "  32SPS conv: %lu us (DS %lu), %u polls, raw=%d = %+.3f mV at gain16",
                     static_cast<unsigned long>(convUs),
                     static_cast<unsigned long>(kConv32SpNominalUs), polls, raw,
                     raw * (kLsbVoltsDesign * 1000.0f));
            out.println(line);
            out.println(F("  (value is expected nonsense with floating inputs - timing is the point)"));
            break;
        }
        case PartCheck::TOO_FAST_OTHER_PART:
            snprintf(line, sizeof(line),
                     "  32SPS conv returned in %lu us — ** rate table is not an ADS1115's **",
                     static_cast<unsigned long>(convUs));
            out.println(line);
            break;
        case PartCheck::NO_CONVERSION:
            snprintf(line, sizeof(line),
                     "  conv never completed after %u polls / 250 ms — ** not converting **", polls);
            out.println(line);
            break;
        case PartCheck::IO_FAIL:
            out.println(F("  conv check aborted: I2C transaction failed mid-conversion"));
            break;
    }
}

} // namespace sensors
