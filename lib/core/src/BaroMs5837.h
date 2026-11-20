// MS5837-30BA register map, PROM/CRC-4 and pressure compensation (AGENTS.md 2, M3).
//
// PROVENANCE — read at source, do not "remember" this part (decision 37, risk 21):
//  - TE Connectivity MS5837-30BA datasheet REV C6 (02/2025), text layer extracted 2026-11-20
//    (ENG_DS_MS5837-30BA_C6, amsys mirror): the 7-bit I2C address 0x76 — the DS states "Address
//    (7 bits) 0x76 (1110110 b)"; the "0x7C" circulating in driver code is the 8-bit write byte,
//    and 0xA0..0xAC are PROM *command* bytes, not slave addresses (decision 36). Commands, the
//    OSR/conversion-time table ("Maximum values must be used to determine waiting times"), the
//    PROM map (W0 = crc:4 + type:7 + factory:5, W1..W6 = C1..C6, W7 = serial:11 + crc:4, total
//    112 bits), the CRC-4 C code (poly 0x3000), the compensation formulas and their worked
//    example (C1=34982 ... C6=26146, D1=4958179, D2=6815414, dT=-5962, TEMP=19.81 C,
//    OFF=2381323464, SENS=1145816755, P=3999.8 mbar) are all taken from that text.
//  - The second-order branches and the exact integer truncation order were cross-checked against
//    the field-deployed BlueRobotics MS5837 library (read 2026-11-20, same API the ArduPilot/ROV
//    world runs on this part). The DS flowchart for the high-temperature branch and its Ti term
//    is image-only in the text layer; where DS text and deployed code are both silent-or-ambiguous,
//    the deployed code wins and says so here (decision 37's rule).
//  - DS REV CONTRADICTION, recorded honestly because it changes an expectation (risk 21): the C6
//    text extracted 2026-11-20 documents W0 bits[11:5] = 0000000 for product "MS5837-30BA01";
//    the C2 revision (captured 2026-11-19 via Mouser) shows the same rule under the product name
//    "MS5837-30BA26". The order-code/suffix story is unresolved, so W0 type bits are REPORTED,
//    never an identity gate. What gates identity: the CRC over the factory PROM (necessary, not
//    sufficient — an emulator can implement it) plus the C1 sensitivity band below, which is the
//    discriminator the deployed library actually uses. And an ACK at 0x76 proves nothing at all:
//    BMP280/BME280 live at 0x76/0x77 (CSB strap), and the deployed library notes the MS5637
//    shares the address and passes CRC — hence the band check.
//  - Part is UNPROCURED as of this writing: every number here is DS/reference-derived. When a
//    Bar30 is fitted, bench 'b' must measure the PROM, the conversion period at the design OSR
//    and the local-station mbar before anything trusts it (decision 29's rule — measurement,
//    never the datasheet, sizes timing).
//
// Units: D1/D2 are 24-bit raw ADC words; P from the 30BA formula comes out in 0.1 mbar (cbar)
// at 0.25 mbar resolution per the DS; TEMP is centidegC. The truncation chain is int64-exact and
// matches the deployed reference bit for bit (pinned by test/test_ms5837 on a 40-point grid) —
// do not "optimise" it to floats.
#pragma once

#include <stdint.h>

namespace ms5837 {

// DS C6, I2C section: "Module ref / Mode / Pins used / Address (7 bits): MS5837-30BA,
// I2C, SDA SCL, 0x76 (1110110 b)". Take the DS's 7-bit form; never do arithmetic on a quoted
// byte (decision 36 — the 0x3E phantom came from exactly that).
constexpr uint8_t kAddr7 = 0x76;

// DS C6 Figure 1 "Command Structure" — all five commands, byte for byte.
constexpr uint8_t kCmdReset = 0x1E;
constexpr uint8_t kCmdAdcRead = 0x00;
constexpr uint8_t kCmdPromBase = 0xA0;  // 0xA0..0xAC, word address in bits a2..a0 (even steps)

// Convert D1 (pressure): 0x40 + 2*osrIndex. Convert D2 (temperature): 0x50 + 2*osrIndex.
enum OsrIndex : uint8_t { kOsr256 = 0, kOsr512, kOsr1024, kOsr2048, kOsr4096, kOsr8192 };

inline uint8_t cmdD1(OsrIndex o) { return static_cast<uint8_t>(0x40u + 2u * static_cast<uint8_t>(o)); }
inline uint8_t cmdD2(OsrIndex o) { return static_cast<uint8_t>(0x50u + 2u * static_cast<uint8_t>(o)); }

// DS C6 ADC table, MAX column — the DS footnote is explicit: "Maximum values must be used to
// determine waiting times". min/typ/max per OSR (ms): 0.48/0.54/0.60, 0.95/1.06/1.17,
// 1.88/2.08/2.28, 3.72/4.13/4.54, 7.40/8.22/9.04, 14.80/16.44/18.08.
inline uint32_t convUsMax(OsrIndex o) {
    static const uint32_t kMax[] = {600, 1170, 2280, 4540, 9040, 18080};
    return kMax[static_cast<uint8_t>(o) & 7u];
}
inline uint32_t convUsTyp(OsrIndex o) {
    static const uint32_t kTyp[] = {540, 1060, 2080, 4130, 8220, 16440};
    return kTyp[static_cast<uint8_t>(o) & 7u];
}

// The design's depth-conversion OSR (AGENTS.md 2: OSR 4096, ~10 ms, 0.2 mbar resolution).
constexpr OsrIndex kDesignOsr = kOsr4096;
// DS C6 power spec: "Power supply hold off for internal reset (VDD < 0.1 V) 200 ms". The RESET
// *command* wait is not given as text anywhere in the DS text layer; the deployed library waits
// 10 ms after 0x1E, so that is what we do — provenance, not preference. A bench re-measurement
// of what the fitted part needs happens when one is fitted.
constexpr uint32_t kResetWaitMsDeployed = 10;

// Operating range 0..30 bar (DS "Operating Pressure Range Prange 0 30 bar"): the depth ceiling
// the dive model may use with this part fitted (risk 1 applies upward too — no extrapolation).
constexpr float kMaxAbsBar = 30.0f;
// Absolute accuracy per DS C6 (for display honesty, not clamping): -20..85 C full range -100/+100
// mbar; 0..45 C, 0..6 bar: -50/+50 mbar. Resolution at OSR 4096: 0.25 mbar.

// ---------------- CRC-4 over the 112-bit factory PROM ----------------

// Transcribed verbatim from the DS C6 "C Code example for CRC-4 calculation". The published code
// uses `unsigned int n_prom[]` with 16-bit-word indexing and relies on the implicit truncation
// when feeding bytes (its `unsigned short` casts); uint16_t here reproduces that behaviour
// exactly on any normal target. The DS code zeroes the CRC nibble (bits 15:12 of W0) and W7
// before walking 16 bytes; this function keeps that inside, so callers pass the PROM as read.
inline uint8_t crc4(const uint16_t prom[8]) {
    uint16_t n_rem = 0;
    const uint16_t w0 = static_cast<uint16_t>(prom[0] & 0x0FFF);  // "CRC byte is replaced by 0"
    const uint16_t w7 = 0;                                        // "Subsidiary value, set to 0"
    for (int cnt = 0; cnt < 16; cnt++) {                          // operation is performed on bytes
        const uint16_t word = (cnt >> 1) == 7 ? w7 : ((cnt >> 1) == 0 ? w0 : prom[cnt >> 1]);
        if (cnt % 2 == 1)
            n_rem ^= static_cast<uint16_t>(word & 0x00FF);
        else
            n_rem ^= static_cast<uint16_t>(word >> 8);
        for (int n_bit = 8; n_bit > 0; n_bit--) {
            if (n_rem & 0x8000)
                n_rem = static_cast<uint16_t>((n_rem << 1) ^ 0x3000);
            else
                n_rem = static_cast<uint16_t>(n_rem << 1);
        }
    }
    n_rem = static_cast<uint16_t>((n_rem >> 12) & 0x000F);  // final 4-bit remainder is CRC code
    return static_cast<uint8_t>(n_rem ^ 0x00);
}

// ---------------- PROM word decoding ----------------

// W0 layout (DS C6): bits 15:12 = CRC, bits 11:5 = product type, bits 4:0 = factory settings.
// DS C6 documents type=0000000 for "MS5837-30BA01"; the C2 revision shows "MS5837-30BA26" under
// the same rule (see PROVENANCE above). Reported, never gated.
inline uint8_t w0Crc(uint16_t w0) { return static_cast<uint8_t>((w0 >> 12) & 0xFu); }
inline uint8_t w0TypeBits(uint16_t w0) { return static_cast<uint8_t>((w0 >> 5) & 0x7Fu); }
inline uint8_t w0FactoryBits(uint16_t w0) { return static_cast<uint8_t>(w0 & 0x1Fu); }
// W7: bits 15:5 = serial number (11 bits), bits 3:0 = CRC rest per the PROM map figure.
inline uint16_t w7Serial(uint16_t w7) { return static_cast<uint16_t>((w7 >> 5) & 0x7FFu); }

// Which sensor family the PROM's sensitivity coefficient says this is. Thresholds are the
// deployed BlueRobotics library's (2026-11-20 read), which adopted them after a real field
// confusion (their comment links ArduPilot PR #29122): the MS5637 shares the address and passes
// the CRC check, and 02BA (2-bar) parts get mistaken for 30BA. A 30BA C1 sits near the DS
// example 34982; an 02BA reads much higher. This is a classification, not a provenance claim —
// outside both bands is UNRECOGNISED, and that is a number the bench prints, not a mystery.
enum class Model : uint8_t { k30BA = 0, k02BA = 1, kUnrecognised = 255 };

constexpr uint16_t kC1Min30BA = 26000;
constexpr uint16_t kC1Separation = 37000;
constexpr uint16_t kC1Max02BA = 49000;

inline Model classifyModel(uint16_t c1) {
    if (c1 < kC1Min30BA || c1 > kC1Max02BA) return Model::kUnrecognised;
    return c1 > kC1Separation ? Model::k02BA : Model::k30BA;
}

// ---------------- compensation ----------------

struct Prom {
    uint16_t w[8];  // w[0]=W0 (crc+type+factory), w[1..6]=C1..C6, w[7]=serial+crc
};

struct Compensation {
    bool crcOk = false;
    Model model = Model::kUnrecognised;
    int32_t dT = 0;        // DS dT = D2 - C5*2^8
    int64_t off = 0;       // first-order OFF
    int64_t sens = 0;      // first-order SENS
    int32_t tempCd = 0;    // first-order TEMP, centidegC (DS example: 1981 = 19.81 C)
    int64_t pCbar = 0;     // pressure in 0.1 mbar after whatever compensation was applied
    bool secondOrderApplied = false;
};

// First-order formulas, DS C6 "PRESSURE AND TEMPERATURE CALCULATION" for the -30BA, written in
// the int64 truncation order of the deployed reference (SENS = C1*2^15 + C3*dT/2^8;
// OFF = C2*2^16 + C4*dT/2^7; TEMP = 2000 + dT*C6/2^23;
// P = (D1*SENS/2^21 - OFF)/2^13, 0.1 mbar units).
// The 02BA variant differs only in scale (SENS=C1*2^16+C3*dT/2^7, OFF=C2*2^17+C4*dT/2^6,
// P divisor 2^15) — implemented so bench cross-checks against the deployed library can run for
// either die; the dive path only ever uses the 30BA branch.
inline int32_t firstOrderTempCd(int32_t dT, uint16_t c6) {
    return static_cast<int32_t>(2000ll + (static_cast<int64_t>(dT) * c6) / 8388608ll);
}

inline Compensation compensate(const Prom& prom, uint32_t d1, uint32_t d2, bool secondOrder) {
    Compensation r;
    r.crcOk = crc4(prom.w) == w0Crc(prom.w[0]);
    r.model = classifyModel(prom.w[1]);

    const int32_t dT = static_cast<int32_t>(d2) - (static_cast<int32_t>(prom.w[5]) << 8);
    r.dT = dT;
    const int64_t c1 = prom.w[1], c2 = prom.w[2], c3 = prom.w[3], c4 = prom.w[4];

    int64_t sens, off, div;
    if (r.model == Model::k02BA) {
        sens = (c1 << 16) + (c3 * dT) / 128ll;
        off = (c2 << 17) + (c4 * dT) / 64ll;
        div = 32768ll;
    } else {
        sens = (c1 << 15) + (c3 * dT) / 256ll;
        off = (c2 << 16) + (c4 * dT) / 128ll;
        div = 8192ll;
    }
    r.sens = sens;
    r.off = off;
    int32_t temp = firstOrderTempCd(dT, prom.w[6]);
    r.tempCd = temp;

    int64_t off2 = off, sens2 = sens;
    if (secondOrder) {
        // Second-order branches, DS C6 flowchart + deployed reference (identical for the 30BA).
        // The DS text layer garbles the high-temperature branch; the deployed reference says
        // Ti = 2*dT^2/2^37, OFFi = (TEMP-2000)^2/16, SENSi = 0 for TEMP >= 20 C, and that is
        // what runs here — provenance stated (decision 37).
        int64_t ti = 0, offi = 0, sensi = 0;
        if (r.model == Model::k02BA) {
            if (temp / 100 < 20) {
                ti = (11ll * dT * dT) / 34359738368ll;
                offi = (31ll * (temp - 2000) * (temp - 2000)) / 8;
                sensi = (63ll * (temp - 2000) * (temp - 2000)) / 32;
            }
        } else {
            if (temp / 100 < 20) {
                ti = (3ll * dT * dT) / 8589934592ll;
                offi = (3ll * (temp - 2000) * (temp - 2000)) / 2;
                sensi = (5ll * (temp - 2000) * (temp - 2000)) / 8;
                if (temp / 100 < -15) {
                    offi += 7ll * (temp + 1500) * (temp + 1500);
                    sensi += 4ll * (temp + 1500) * (temp + 1500);
                }
            } else {
                ti = (2ll * dT * dT) / 137438953472ll;
                offi = (1ll * (temp - 2000) * (temp - 2000)) / 16;
                sensi = 0;
            }
        }
        off2 = off - offi;
        sens2 = sens - sensi;
        temp = static_cast<int32_t>(temp - ti);
        r.secondOrderApplied = true;
        r.tempCd = temp;
    }
    r.pCbar = (static_cast<int64_t>(d1) * sens2) / 2097152ll;
    r.pCbar = (r.pCbar - off2) / div;
    return r;
}

// DS worked example, quoted here so the test anchor has a source line to point at:
// C1=34982 C2=36352 C3=20328 C4=22354 C5=26646 C6=26146, D1=4958179, D2=6815414 ->
// dT=-5962, OFF=2381323464, SENS=1145816755, TEMP "19.81 C", P=39998 (3999.8 mbar).
// Rounding subtlety found by test 2026-11-20: the example's intermediates are NEGATIVE (dT<0),
// and the printed columns are self-consistent only with arithmetic-shift (floor) semantics;
// C-style division in the published formulas and in the deployed library truncates toward zero
// and yields 1982 / 2381323465 / 1145816756. The references genuinely differ by <=1 LSB of each
// variable at negative intermediates. The dive path follows the deployed code's division
// semantics (decision 37 — and it is what a future bench cross-check against deployed code will
// see), with this difference stated, not papered over.

}  // namespace ms5837
