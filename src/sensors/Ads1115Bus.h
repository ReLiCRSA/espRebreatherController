// ADS1115 register driver for the two cell converters (AGENTS.md 2).
//
// Deliberately hand-rolled rather than Adafruit ADS1X15 (which is in lib_deps for now): the
// design requires *alternating single-shot* conversions — continuous mode locks the mux to one
// channel, so one converter must be walked AIN0/1 -> AIN2/3 by re-writing the config register
// between conversions. That is not what a continuous-mode Arduino driver does, and the coil-pulse
// scheduling in AGENTS.md 8 needs to know exactly when each converter is integrating. Keeping the
// register layer explicit also keeps the 16-bit big-endian register format visible at the point it
// is used, which is where the subtle bugs are.
//
// Status: register layer + bring-up self-test (probeId / readReg / writeReg / selfTest). The
// alternating 16 SPS-per-cell conversion scheduler is the next step and is NOT implemented here
// yet — do not read this file as the M1 gate being met.
#pragma once

#include <Arduino.h>
#include <Wire.h>

// The register map, field positions and field meanings live in lib/core/src/AdcRegs.h so native
// tests can pin them (they were wrong twice: rate field position, and the MODE bit). Everything
// below is an alias, not a second copy.
#include "AdcRegs.h"

namespace sensors {

class Ads1115 {
  public:
    static constexpr uint8_t kRegConv = adc::kRegConv;
    static constexpr uint8_t kRegConfig = adc::kRegConfig;
    static constexpr uint8_t kRegLoThresh = adc::kRegLoThresh;
    static constexpr uint8_t kRegHiThresh = adc::kRegHiThresh;
    static constexpr uint8_t kRegPtr = adc::kRegPtr;
    static constexpr uint8_t kRegId = adc::kRegId;

    // All register fields come from adc:: (lib/core/src/AdcRegs.h), asserted by test/test_adc.
    // Aliases only — a second copy of these constants in src/ is what let two of them be wrong.
    static constexpr uint16_t kOsBusy = adc::kOsMask;
    static constexpr uint16_t kMuxMask = adc::kMuxMask;
    static constexpr uint16_t kPgaMask = adc::kPgaMask;
    static constexpr uint16_t kSpsMask = adc::kRateMask;
    // MODE is written explicitly, always. Clearing it selects **continuous** mode, where the mux is
    // locked to one channel and the OS bit never returns: the first on-hardware conversion test
    // timed out for exactly this reason, and the code looked correct.
    static constexpr uint16_t kModeSingleShot = adc::kModeSingleShot;

    // Gain 16 = PGA 101 = +/-0.256 V FSR, LSB 7.8125 uV. A galvanic cell in air is ~9-13 mV, i.e.
    // 1150-1660 counts against +/-32767 of range: comfortable headroom for the bias network, and
    // fine resolution. (The 0.125 mV LSB sometimes quoted for this part is the *gain 1* value.)
    static constexpr uint16_t kGainDesign = adc::kPgaDesign;
    static constexpr float kLsbVoltsDesign = adc::lsbVolts(adc::designConfig());

    enum Mux : uint16_t { MUX_AIN0_AIN1 = adc::kMuxAin0Ain1, MUX_AIN0_AIN3 = adc::kMuxAin0Ain3,
                          MUX_AIN1_AIN3 = adc::kMuxAin1Ain3, MUX_AIN2_AIN3 = adc::kMuxAin2Ain3 };

    // Design sample rate, and the conversion period that goes with it.
    static constexpr uint16_t kSps32 = adc::kRate32;
    static constexpr uint16_t kSps8 = adc::kRate8;
    static constexpr uint16_t kSps860 = adc::kRate860;

    // Identity and fitness by timing, not by ID register. An ADS1015 shares the address, the
    // register map and every field position, but its RATE table starts at 800 SPS: the field that
    // means 32 SPS on an ADS1115 means 3200 SPS there, so one measured conversion period separates
    // them by 100x — and it separates the property the design depends on. The ID register cannot do
    // this job: measured 2026-11-19 the bench module answers 0x00 at register 0x0B and 0x00 at the
    // pointer register while its config register reads and writes correctly.
    //
    // The window is deliberately wider than the datasheet figure, because measurement says why: the
    // bench module completes a "32 SPS" conversion in ~62 ms, twice the datasheet's 31.25 ms. A
    // window pinned at 31 ms would refuse a converter that works and is merely slow, and refusing is
    // not the same as being safe. What the dive loop must do instead is size the coil/ADC interlock
    // on the *measured* period (measureAdcRates(), bench command 'd'), never on the datasheet table.
    static constexpr uint32_t kConv32SpNominalUs = 31250;
    static constexpr uint32_t kConvAcceptMinUs = 20000;   // far below: wrong part's rate table
    static constexpr uint32_t kConvAcceptMaxUs = 150000;  // far above: stuck at 8 SPS or worse

    // Characterise the converter's actual sample rates: one measured period per RATE field. This is
    // bench-only (it writes the config register eight times and takes ~0.3 s) and it is what the
    // interlock window and the per-cell sample rate get derived from.
    void measureAdcRates(Print& out);

    // The pointer register read on power-up names the last-accessed register; the DS says its
    // power-on value is not defined, so selfTest() writes a known register and verifies the
    // pointer follows. That is what distinguishes a real register map from a stuck bus.
    Ads1115(TwoWire& wire, uint8_t address, int drdyPin = -1)
        : wire_(wire), addr_(address), drdy_(drdyPin) {}

    uint8_t address() const { return addr_; }
    int drdyPin() const { return drdy_; }

    // Presence is owned by the boot probe table; the driver only mirrors it so bench commands can
    // skip a converter that is not there instead of issuing transactions into the void.
    void setPresent(bool p) { present_ = p; }
    bool present() const { return present_; }

    // A presence probe cannot tell an ADS1115 from an ADS1015, and it cannot tell a genuine TI
    // part from a remarked clone that answers the address but implements less of the register map.
    // The ID register is read for information only — see the timing note above for why it is not a
    // gate. Read-only, so it is safe on a unit wired for a dive.
    uint8_t probeId(uint8_t* idOut = nullptr);  // returns Wire error, 0 = ok
    uint8_t id() const { return id_; }

    bool readReg(uint8_t reg, uint16_t* out);
    bool writeReg(uint8_t reg, uint16_t value);

    // Start a single-shot conversion with (mux | gain | sps | single-shot mode).
    bool startSingleShot(uint16_t mux, uint16_t gain, uint16_t sps);
    // Polls the OS bit; returns false on timeout rather than blocking forever on a dead converter.
    bool waitReady(uint32_t timeoutMs);

    // Part sanity, measured. See the timing note above: this is the check that actually separates
    // an ADS1115 from an ADS1015, and it is the check that proves the converter converts.
    enum class PartCheck : uint8_t { OK, IO_FAIL, NO_CONVERSION, TOO_FAST_OTHER_PART,
                                       TOO_SLOW };
    PartCheck checkPart(uint32_t* convUsOut = nullptr, unsigned* pollsOut = nullptr);

    // Bring-up diagnostic: chip ID, a write-verify of one benign register, pointer-register
    // behaviour, DRDY line state and conversion timing. It changes nothing the cell chain depends
    // on (the one register it writes is restored).
    void selfTest(Print& out);

  private:
    TwoWire& wire_;
    uint8_t addr_;
    int drdy_;
    uint8_t id_ = 0;
    bool present_ = false;
};

} // namespace sensors
