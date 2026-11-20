// MS5837-30BA PROM/CRC and compensation tests (lib/core/src/BaroMs5837.h).
//
// Provenance discipline, identical to test_baro, because this part carries the same trap: every
// fact asserted here is either text-extracted from the TE MS5837-30BA DS REV C6 (2026-11-20),
// or taken from the field-deployed BlueRobotics MS5837 library (read 2026-11-20) — never from
// memory (decision 37, risk 21). The compensation anchor set is the DS's own worked example
// (C1=34982 ... C6=26146, D1=4958179, D2=6815414 -> dT=-5962, TEMP 19.81 C, OFF 2381323464,
// SENS 1145816755, 3999.8 mbar). The CRC transcription is discriminated against a verbatim
// second copy of the DS's published C code; the integer truncation chain and second-order
// branches against a verbatim second copy of the deployed library. Numerical agreement is still
// NOT belief: the part is unprocured, and when a Bar30 arrives bench 'b' must read its PROM,
// time its conversion at the design OSR, and produce a Stockholm-station mbar (984-987 hPa
// SLP band on 2026-11-20 — named place/hour/source, risk 21) before anything trusts it.
#include "BaroMs5837.h"
#include <stdio.h>
#include "unity.h"

using namespace ms5837;

void setUp() {}
void tearDown() {}

// ---- second independent copies of the published references (discriminators, not re-uses) ----

// Verbatim from DS C6 "C Code example for CRC-4 calculation" (published unsigned-int form).
static uint8_t crc4_ds_published(uint16_t n_prom[]) {
    int cnt;
    uint16_t n_rem = 0;
    n_prom[0] = ((n_prom[0]) & 0x0FFF);  // CRC byte is replaced by 0
    n_prom[7] = 0;                       // Subsidiary value, set to 0
    for (cnt = 0; cnt < 16; cnt++) {
        if (cnt % 2 == 1)
            n_rem ^= (uint16_t)((n_prom[cnt >> 1]) & 0x00FF);
        else
            n_rem ^= (uint16_t)(n_prom[cnt >> 1] >> 8);
        for (int n_bit = 8; n_bit > 0; n_bit--) {
            if (n_rem & (0x8000))
                n_rem = (uint16_t)((n_rem << 1) ^ 0x3000);
            else
                n_rem = (uint16_t)(n_rem << 1);
        }
    }
    n_rem = ((n_rem >> 12) & 0x000F);
    return (uint8_t)(n_rem ^ 0x00);
}

// Verbatim structure of the deployed BlueRobotics calculate()/30BA + 02BA branches, including
// its int64 truncation order and its second-order branch constants.
static int64_t br_pressure(uint16_t C[8], uint32_t D1, uint32_t D2, uint8_t model, int32_t* tempOut) {
    int32_t dT = (int32_t)D2 - (uint32_t)C[5] * 256l;
    int64_t SENS, OFF, P, SENS2, OFF2;
    int32_t SENSi = 0, OFFi = 0, Ti = 0;
    if (model == 1) {
        SENS = (int64_t)C[1] * 65536l + ((int64_t)C[3] * dT) / 128l;
        OFF = (int64_t)C[2] * 131072l + ((int64_t)C[4] * dT) / 64l;
        P = ((int64_t)D1 * SENS) / 2097152l - OFF;
        P = P / 32768l;
    } else {
        SENS = (int64_t)C[1] * 32768l + ((int64_t)C[3] * dT) / 256l;
        OFF = (int64_t)C[2] * 65536l + ((int64_t)C[4] * dT) / 128l;
        P = ((int64_t)D1 * SENS) / 2097152l - OFF;
        P = P / 8192l;
    }
    int32_t TEMP = 2000l + ((int64_t)dT * C[6]) / 8388608LL;
    if (model == 1) {
        if ((TEMP / 100) < 20) {
            Ti = (int32_t)((11ll * dT * dT) / 34359738368LL);
            OFFi = (int32_t)((31ll * (TEMP - 2000) * (TEMP - 2000)) / 8);
            SENSi = (int32_t)((63ll * (TEMP - 2000) * (TEMP - 2000)) / 32);
        }
    } else {
        if ((TEMP / 100) < 20) {
            Ti = (int32_t)((3ll * dT * dT) / 8589934592LL);
            OFFi = (int32_t)((3ll * (TEMP - 2000) * (TEMP - 2000)) / 2);
            SENSi = (int32_t)((5ll * (TEMP - 2000) * (TEMP - 2000)) / 8);
            if ((TEMP / 100) < -15) {
                OFFi += (int32_t)(7ll * (TEMP + 1500l) * (TEMP + 1500l));
                SENSi += (int32_t)(4ll * (TEMP + 1500l) * (TEMP + 1500l));
            }
        } else if ((TEMP / 100) >= 20) {
            Ti = (int32_t)(2ll * ((int64_t)dT * dT) / 137438953472LL);
            OFFi = (int32_t)((1ll * (TEMP - 2000) * (TEMP - 2000)) / 16);
            SENSi = 0;
        }
    }
    OFF2 = OFF - OFFi;
    SENS2 = SENS - SENSi;
    TEMP = TEMP - Ti;
    if (model == 1)
        P = (((int64_t)D1 * SENS2) / 2097152l - OFF2) / 32768l;
    else
        P = (((int64_t)D1 * SENS2) / 2097152l - OFF2) / 8192l;
    *tempOut = TEMP;
    return P;
}

// ---- DS facts, asserted so an edit cannot quietly change a command or a wait ----

void test_address_commands_and_osr_table_match_the_datasheet() {
    // DS C6 I2C section, quoted: "Address (7 bits) ... 0x76 (1110110 b)". Decision 36: this is
    // the form Wire takes; 0x7C is the 8-bit write byte, never to be halved again.
    TEST_ASSERT_EQUAL_HEX8(0x76, kAddr7);
    TEST_ASSERT_EQUAL_HEX8(0x1E, kCmdReset);
    TEST_ASSERT_EQUAL_HEX8(0x00, kCmdAdcRead);
    TEST_ASSERT_EQUAL_HEX8(0xA0, kCmdPromBase);
    // DS Figure 1 command table, every D1/D2 OSR row.
    const uint8_t d1[] = {0x40, 0x42, 0x44, 0x46, 0x48, 0x4A};
    const uint8_t d2[] = {0x50, 0x52, 0x54, 0x56, 0x58, 0x5A};
    for (int i = 0; i < 6; ++i) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(d1[i], cmdD1(static_cast<OsrIndex>(i)), "D1 OSR table");
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(d2[i], cmdD2(static_cast<OsrIndex>(i)), "D2 OSR table");
    }
    // Conversion-time table (DS: "Maximum values must be used to determine waiting times"),
    // MAX/typ per OSR in us: 600/540, 1170/1060, 2280/2080, 4540/4130, 9040/8220, 18080/16440.
    const uint32_t mx[] = {600, 1170, 2280, 4540, 9040, 18080};
    const uint32_t tp[] = {540, 1060, 2080, 4130, 8220, 16440};
    for (int i = 0; i < 6; ++i) {
        TEST_ASSERT_EQUAL_UINT32(mx[i], convUsMax(static_cast<OsrIndex>(i)));
        TEST_ASSERT_EQUAL_UINT32(tp[i], convUsTyp(static_cast<OsrIndex>(i)));
    }
    // The design point: OSR 4096, wait sized from the MAX column (9.04 ms -> the ~10 ms claim
    // in AGENTS.md 2 is the DS MAX rounded up, which is the correct way to round a wait).
    TEST_ASSERT_EQUAL_UINT8(kOsr4096, kDesignOsr);
    TEST_ASSERT_EQUAL_UINT32(9040, convUsMax(kDesignOsr));
}

// ---- CRC-4: transcription vs the published DS C code, plus structural properties ----

void test_crc4_matches_the_published_datasheet_code_and_its_structure() {
    // Deterministic pseudo-random PROM sweep: core vs a verbatim copy of the DS's own C code.
    uint32_t s = 0x583730BAu;
    int diffs = 0;
    for (int t = 0; t < 256; ++t) {
        uint16_t prom[8], prom2[8];
        for (int i = 0; i < 8; ++i) {
            s = s * 1664525u + 1013904223u;
            prom[i] = prom2[i] = static_cast<uint16_t>(s >> 16);
        }
        if (crc4(prom) != crc4_ds_published(prom2)) ++diffs;
        TEST_ASSERT_TRUE(crc4(prom) < 16);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, diffs, "core crc4 must equal the DS published code");

    uint16_t prom[8] = {0x0000, 34982, 36352, 20328, 22354, 26646, 26146, 0x0000};
    const uint8_t base = crc4(prom);
    // DS code masks: the CRC nibble itself (W0 15:12) and all of W7 do not feed the CRC.
    uint16_t p2[8];
    for (int i = 0; i < 8; ++i) p2[i] = prom[i];
    p2[0] |= 0xF000;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(base, crc4(p2), "CRC nibble must not feed the CRC");
    p2[0] &= 0x0FFF;
    for (int i = 0; i < 8; ++i) p2[i] = prom[i];
    p2[7] = 0xDEAD;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(base, crc4(p2), "W7 is zeroed by the DS code");
    // Every coefficient bit must feed it: one flipped bit in C3 changes the nibble.
    for (int i = 0; i < 8; ++i) p2[i] = prom[i];
    p2[3] ^= 0x0001;
    TEST_ASSERT_NOT_EQUAL(base, crc4(p2));
}

// ---- identity/classification: presence is not identity, and CRC-pass is not identity either ----

void test_w0_decode_and_c1_band_classification() {
    // W0 = crc:4 | type:7 | factory:5 (DS C6 word-0 programming table shape).
    const uint16_t w0 = (0xAu << 12) | (0x1Au << 5) | 0x03u;  // 0xA343
    TEST_ASSERT_EQUAL_UINT8(0x0A, w0Crc(w0));
    TEST_ASSERT_EQUAL_UINT8(0x1A, w0TypeBits(w0));
    TEST_ASSERT_EQUAL_UINT8(0x03, w0FactoryBits(w0));
    TEST_ASSERT_EQUAL_UINT16(0x559u, w7Serial(0xAB28));  // (0xAB28 >> 5) & 0x7FF = 1369

    // Bands are the deployed library's thresholds, boundaries asserted at the exact values
    // (an off-by-one here silently reclassifies a 2-bar part as the dive depth source).
    TEST_ASSERT_EQUAL_UINT8(255, (uint8_t)classifyModel(25999));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Model::k30BA, (uint8_t)classifyModel(26000));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Model::k30BA, (uint8_t)classifyModel(37000));  // ">", not ">="
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Model::k02BA, (uint8_t)classifyModel(37001));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Model::k02BA, (uint8_t)classifyModel(49000));
    TEST_ASSERT_EQUAL_UINT8(255, (uint8_t)classifyModel(49001));
    // The DS example C1=34982 classifies as 30BA — the example and the bands must be consistent.
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Model::k30BA, (uint8_t)classifyModel(34982));
}

// ---- the DS worked example, exactly ----

void test_datasheet_example_first_order_reproduces_exactly() {
    Prom prom{};
    prom.w[0] = 0x0000;  // type bits 0000000 per DS C6 "-30BA01" row; factory bits 0
    prom.w[1] = 34982;   // C1  (DS example column)
    prom.w[2] = 36352;   // C2
    prom.w[3] = 20328;   // C3
    prom.w[4] = 22354;   // C4
    prom.w[5] = 26646;   // C5
    prom.w[6] = 26146;   // C6
    prom.w[7] = 0x0000;
    // crcOk must follow the CRC nibble in W0, whatever it is: set it from the reference copy.
    const uint8_t crc = crc4_ds_published(prom.w);
    prom.w[0] = static_cast<uint16_t>((uint16_t)crc << 12);

    const Compensation a = compensate(prom, 4958179u, 6815414u, /*secondOrder=*/false);
    TEST_ASSERT_TRUE_MESSAGE(a.crcOk, "W0 crc nibble computed by the reference copy must verify");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)Model::k30BA, (uint8_t)a.model);
    TEST_ASSERT_EQUAL_INT32(-5962, a.dT);
    // The DS example column prints 19.81 C, OFF 2381323464, SENS 1145816755. Those columns are
    // self-consistent with ARITHMETIC-SHIFT (floor) semantics on the negative intermediates
    // (dT<0 here); the published C-style division truncates toward zero instead and produces
    // 1982 / 2381323465 / 1145816756 — which is what the field-deployed library computes on
    // these exact words. Decision 37: the deployed code is the reference; the difference is at
    // most one LSB of each variable (0.01 C, 0.1 mbar — at or below the DS's own stated
    // resolution) and is recorded here rather than chased. Belief anchor = exact agreement with
    // the deployed reference copy, not agreement with an example column whose rounding is
    // itself unstated.
    TEST_ASSERT_EQUAL_INT32(1982, a.tempCd);
    TEST_ASSERT_EQUAL_INT64(2381323465ll, a.off);       // DS column 2381323464: floor-vs-trunc
    TEST_ASSERT_EQUAL_INT64(1145816756ll, a.sens);      // DS column 1145816755: same
    TEST_ASSERT_TRUE(a.pCbar >= 39997 && a.pCbar <= 39999);  // DS: "3999.8 mbar" (0.1 mbar units)

    // The same example through the deployed library (with its second-order step on) is the
    // cross-implementation agreement: DS published formulas == deployed code on DS words.
    int32_t tRef = 0;
    uint16_t Cref[8];
    for (int i = 0; i < 8; ++i) Cref[i] = prom.w[i];
    const int64_t pRef = br_pressure(Cref, 4958179u, 6815414u, 0, &tRef);
    const Compensation b = compensate(prom, 4958179u, 6815414u, /*secondOrder=*/true);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(pRef, b.pCbar, "second-order chain must match the deployed reference");
    TEST_ASSERT_EQUAL_INT32(tRef, b.tempCd);
    // OFF/SENS stay the first-order values the struct reports; the second-order step is applied
    // inside P. At ~19.8 C that step nets ~+0.001 mbar here and truncation can absorb it, so the
    // belief assert is the agreement with the reference copy above — NOT "P must change"
    // (a knife-edge equality is exactly the kind of anchor that fails a year later by accident).
    TEST_ASSERT_EQUAL_INT64(2381323465ll, b.off);
    TEST_ASSERT_EQUAL_INT64(1145816756ll, b.sens);
}

// ---- truncation chain + second-order branches vs the deployed reference, on a grid ----

void test_compensation_matches_the_deployed_reference_on_a_grid() {
    const uint32_t d1s[] = {300u, 4958179u, 8388607u, 16777215u, 1u};
    const int32_t dTs[] = {-6000000, -200000, -5962, -1000, 0, 1000, 20000, 600000};
    // The dT set spans the very-low-temp (<-15 C), low-temp (<20 C) and high-temp branches of
    // both model variants; extremes of D1 span under-range to full 24-bit scale. This grid is
    // arithmetic discrimination, not physical profiles.
    for (uint8_t model = 0; model <= 1; ++model) {
        Prom prom{};
        prom.w[1] = model ? 40000 : 34982;  // inside each band per classifyModel
        prom.w[2] = 36352; prom.w[3] = 20328; prom.w[4] = 22354;
        prom.w[5] = 26646; prom.w[6] = 26146;
        uint16_t Cref[8];
        for (int i = 0; i < 8; ++i) Cref[i] = prom.w[i];
        for (uint32_t d1 : d1s) {
            for (int32_t dT : dTs) {
                const uint32_t d2 = static_cast<uint32_t>(static_cast<int64_t>(prom.w[5]) * 256 + dT);
                const Compensation a = compensate(prom, d1, d2, true);
                int32_t tRef = 0;
                const int64_t pRef = br_pressure(Cref, d1, d2, model, &tRef);
                char msg[64];
                snprintf(msg, sizeof(msg), "model %u D1 %lu dT %ld", model, (unsigned long)d1, (long)dT);
                TEST_ASSERT_EQUAL_INT64_MESSAGE(pRef, a.pCbar, msg);
                TEST_ASSERT_EQUAL_INT32_MESSAGE(tRef, a.tempCd, msg);
            }
        }
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_address_commands_and_osr_table_match_the_datasheet);
    RUN_TEST(test_crc4_matches_the_published_datasheet_code_and_its_structure);
    RUN_TEST(test_w0_decode_and_c1_band_classification);
    RUN_TEST(test_datasheet_example_first_order_reproduces_exactly);
    RUN_TEST(test_compensation_matches_the_deployed_reference_on_a_grid);
    return UNITY_END();
}
