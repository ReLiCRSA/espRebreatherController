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
// Status: register layer, bring-up self-test (probeId / readReg / writeReg / selfTest), and the
// alternating-mux pair scheduler (AdcPairWalker) that M1 asked for. Its achieved rate and
// interlock window are measured on-target by bench command 't' — like every other timing figure
// here, they are read off the part, not off the datasheet.
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

    // The conversion period that checkPart() last measured, in µs (0 = never measured). Everything
    // downstream — the pair walker's poll cadence, its conversion-stall timeout, and the coil/ADC
    // interlock window of AGENTS.md 8 — is sized from this measured number, never from the
    // datasheet's rate table (decision 29, risk 16: the fitted clones run the "32 SPS" field at
    // ~62 ms, twice the datasheet figure).
    uint32_t measuredConvUs() const { return measConvUs_; }

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
    uint32_t measConvUs_ = 0;  // set by checkPart() when a conversion completes (any verdict)
};

// ---------------------------------------------------------------------------
// AdcPairWalker — the alternating-mux scheduler AGENTS.md 2 requires.
//
// One converter feeds two differential pairs (U1: cells 1+2; U2: cell 3 + coil shunt). Continuous
// mode locks the mux to one channel, so the converter has to be walked AIN0/1 -> AIN2/3 by
// re-writing the config register between single-shot conversions. That is this state machine, and
// it is deliberately **non-blocking**: the dive loop calls tick() alongside everything else, and
// each call performs at most one or two I2C transactions. A blocking waitReady() in the dive path
// would cost 62 ms per conversion on the fitted parts — minutes on a bad bus.
//
// Sizing comes from measurement, not the datasheet (decision 29 / risk 16): begin() reads
// adc.measuredConvUs(), the period checkPart() timed at boot (~62 ms on the fitted clones, where
// the datasheet promises 31.25 ms). So the design's "16 SPS per cell" is **8 SPS per cell** on
// this hardware, and the per-cell rate the walker reports (bench 't') is counted from conversions
// actually completed, not computed from the RATE field written.
//
// Coil/ADC interlock (AGENTS.md 8, risk 4): integrating() is true from the instant a conversion is
// started until its result has been read back. Coil pulses must not fire inside that window — a
// coil kick landing mid-conversion looks exactly like cell disagreement and trips voting.
// coilSafeAfterUs() is the pulse scheduler's estimate of the remaining window, built on the
// running average of **measured** conversion periods; integrating() itself is the authoritative
// signal. DRDY would make this exact rather than polled (open item 14) — until it is wired, the
// OS bit is polled at kPollGapUs cadence, and the poll cost (~350 us per config read, measured)
// is included in the duty figures 't' prints.
//
// Raw codes are stored exactly as the converter put them on the bus. The per-channel `invert` flag
// resolved on the bench is applied at consumption (CellArray, M2) — a log, display or test that
// quotes a raw value must quote the un-inverted one, or an inverted scale hides a railed input.
class AdcPairWalker {
  public:
    // OS-bit poll cadence. A config-register read measured ~350 us on this bus, so 10 ms costs
    // ~3.5% of one converter's bus time and puts ~6 polls inside one 62 ms conversion — enough
    // resolution to see the coil window close without spamming the bus.
    static constexpr uint32_t kPollGapUs = 10000;
    static constexpr uint32_t kBackoffMs = 1000;  // after I2C error or conversion stall

    AdcPairWalker(Ads1115& adc, uint16_t muxA, uint16_t muxB,
                  const char* labelA, const char* labelB)
        : adc_(adc), muxA_(muxA), muxB_(muxB), label_{labelA, labelB} {}

    // (Re)size from the converter's last measured conversion period and start walking. Called at
    // boot after checkAdcs(), and again whenever a converter appears/disappears — a hot-swapped
    // part gets its own measurement, not the previous part's. If the part has never been
    // characterised, sizing falls back to the datasheet figure **and says so in report()**: a
    // silent datasheet fallback in a timing path is exactly what risk 16 forbids.
    void begin();

    // Drive the state machine. Call at better than kPollGapUs cadence; costs nothing in ABSENT.
    void tick();

    bool integrating() const { return state_ == WAIT_A || state_ == WAIT_B; }
    // µs until this converter is expected to be coil-safe again; 0 when it is idle. An estimate —
    // integrating() going false is the authoritative edge. Never size a hardware window from the
    // datasheet: the basis here is the running average of measured periods.
    uint32_t coilSafeAfterUs() const;

    // Slot 0 = mux A (AIN0/1), slot 1 = mux B (AIN2/3).
    bool slotValid(int slot) const { return slot >= 0 && slot < 2 && valid_[slot]; }
    int16_t slotRaw(int slot) const { return raw_[slot]; }
    uint32_t slotAgeMs(int slot, uint32_t nowMs) const {
        return (slot >= 0 && slot < 2 && valid_[slot]) ? nowMs - stampMs_[slot] : 0;
    }

    // Achieved rate, period stats, interlock duty and error counters — the M1 gate numbers, from
    // the counter of conversions actually completed. Bench command 't'.
    void report(Print& out, uint32_t nowMs) const;

  private:
    enum State : uint8_t { OFF, ABSENT, BACKOFF, START_A, WAIT_A, START_B, WAIT_B };

    void enterBackoff() { state_ = BACKOFF; backoffMs_ = millis(); }

    Ads1115& adc_;
    uint16_t muxA_, muxB_;
    const char* label_[2];

    State state_ = OFF;
    uint16_t rateField_ = adc::kRateDesign;
    uint32_t convSizedUs_ = 0;      // the period begin() sized this walker from
    bool sizedFromMeasure_ = false; // false = datasheet fallback, and report() says so loudly
    uint32_t convStallUs_ = 0;      // elapsed past which the conversion is declared dead
    uint32_t convStartUs_ = 0, lastPollUs_ = 0, backoffMs_ = 0;

    int16_t raw_[2] = {0, 0};
    uint32_t stampMs_[2] = {0, 0};
    bool valid_[2] = {false, false};
    uint32_t convCount_[2] = {0, 0};

    // Counters are cumulative since begin() and deliberately include traffic stirred up by bench
    // commands ('a' and 'd' write the config register mid-walk): the report says so rather than
    // pretending the run was clean.
    uint32_t ioErr_ = 0, timeouts_ = 0, lateDetect_ = 0, polls_ = 0;
    uint32_t periodAvg_ = 0, periodMin_ = 0, periodMax_ = 0;
    uint32_t busyUs_ = 0;   // cumulative µs with a conversion in flight (interlock duty)
    uint32_t startMs_ = 0;  // uptime for achieved-SPS arithmetic
};

} // namespace sensors
