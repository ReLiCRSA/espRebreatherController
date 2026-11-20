# HANDOFF.md — session transfer snapshot

**Written:** 2026-11-19, after the headless dive simulator was written and run on real hardware.
**Updated:** 2026-11-21 (fifth pass) — **M6 (web + config store + dive log) is IN PROGRESS, NOT
committed, and the ESP32 image does NOT link: `dram0_0_seg` overflowed by 57,376 B when the radio
went live.** Nothing from M6 is flashed; the board still runs the fourth-pass (M7) image. Read
**§7 "M6 — where it stands, and the DRAM blocker" before touching anything**: it holds the measured
map figures, the ordered cut list, what is already written and tested, and the acceptance path.
Native suite is green (**15 suites, ~173 cases**, incl. new `test_config` and `test_webjson`); only
the ESP32 link is failing.
**Previously (fourth pass), 2026-11-20,** after **M7: display template engine + FieldRegistry +
SSD1306 scenes** — engine complete in `lib/core` (decision 40), bench `o` runs the gate on-target
(validator OK/rejection, §12 fit report verbatim, SIM-stamped demo render 1426 µs, real panel
frame 22 ms), live 5 Hz render from boot, boot `[oled] SSD1306 up @0x3C`. Along the way the board
boot-looped (`TG1WDT_SYS_RESET`) and the cause was a struct, not a driver: a ~18 KB `Template`
local on the 8 KB loopTask — decision 40's memory rules close it (one shared scratch, footprint
pinned by test). 140 native cases, 13 suites, ASan-clean; flashed 528,505 B (33.6%), RAM 85,920 B
(26.2%). **Panel check: DONE — the user confirms the 0x3C module paints `o`'s pattern readable
and correct: genuine SSD1306-class, `SH1106Wire` fallback documented and unused.**
**Previously (third pass), 2026-11-20,** after **decision 39: three-tier depth sensing** —
MS5837-30BA primary → LPS28DFW (mode 2) armable fallback → BMP180 bench-only. `BaroLps28.h` +
`Lps28dfw.h` + `checkLps28()` + bench `b`'s LPS28 section written (part unprocured, ST vendor
driver anchored), `BARO_BENCH_ONLY = 27` raised when the BMP is the sole depth source, and the
**on-target baseline changed as intended: the BMP-only bench now prints "bench-only tier" +
`BARO_BENCH_ONLY` + `[arm] REFUSED`**. 123 native cases, 12 suites; flashed 506,989 B (32.2%).
Earlier the same day (second pass), after **M3's code completed**: `Depth.h` + learned
Psurf (proven on-target: 977.89 mbar σ 0.06), MS5837 + INA219 drivers written and natively pinned
(**parts unprocured/unfitted — written-not-measured**), identity-based baro selection, bench `w`,
`README.md` written — and after **decision 38 corrected the record's "~9 m" fallback ceiling to
under 1 m** (10x error found by writing the depth math). Earlier the same day: the M3 Bosch baro
driver was **flashed and proven on the part**, and the absolute-pressure cross-check's wrong-city
reference was corrected (§5g, AGENTS.md §20 item 18 closed). **Bench location, now a
named fact: Stockholm, Sweden** (§6).
**Purpose:** everything needed to continue this project on a different machine. `AGENTS.md` is still
the binding design record (decisions §18, risks §19, milestones §16); this file holds the
*machine-specific and hardware-measured* state that `AGENTS.md` does not, plus the immediate
resume point. Read `AGENTS.md` first, this second.

---

## 1. Project state in one paragraph

ESP32 classic CCR rebreather dive computer. `lib/core` (deco engine, voting, gas model, boot report,
the ADC register map, the GPIO pin map, the **source interfaces** in `SensorIf.h`, the
**Bosch baro map + compensation** in `BaroBosch.h`, and since 2026-11-20 late the **depth math**
(`Depth.h`: learned Psurf, salinity, computed range clamp), the **MS5837 map + compensation**
(`BaroMs5837.h`), the **INA219 map** (`PwrRegs.h`), the **LPS28DFW map + scaling**
(`BaroLps28.h`, decision 39 tier-2) and the **M7 display stack** (`Template.h` parse/validate/
layout/fit/render, `Fields.h` registry, `Framebuf.h`, `Font6x8.h`, `LiveState.h`, the
`DisplayDriver` interface, `SimDisplay.h`, `TemplateBundles.h` — decision 40)) plus
**`lib/sim`** (SimBaro, SimCells, LoopModel, ValveModel, CcrControlLaw, SimRunner) are complete and
regression-anchored: **173 native cases green** across **fifteen** suites (counted 2026-11-21, after
M6 added `test_config` and `test_webjson`). `src/` boots on the board:
safe-state pins first, bounded I2C presence probing, active converter checks, honest boot fault
report, arming refusal, hot re-probe every 5 s, two alternating-mux pair walkers running in `loop()`,
the 5 Hz display renderer live on the fitted 0x3C panel, serial bench commands (`s p r a d c t b w g
v o`, plus M6's `k n j` — **not flashed yet, see below**).
**M0, M1 and M7 (engine + fitted-panel path) are DONE; M3's code is DONE (2026-11-20), its gate is
not; M6 is WRITTEN and native-pinned but UNCOMMITTED and its ESP32 image DOES NOT LINK (DRAM region
over by 57,376 B — §7's first section is the recovery plan, and the board still runs the M7 image).**
Written and flashed: the BMP085/BMP180 fallback driver (`BaroBosch.h` + `src/sensors/Bmp085.h` +
`checkBaro()` — decision 37: the compensation is a transcription of the deployed Adafruit library,
wrong in four places until that cross-check caught it) — **proven on the part** (driver 977.75/977.78
vs Adafruit library 977.76 mbar, host hand recompute matches, squeeze moves the reading, §5g);
`Depth.h` + `learnSurface()` — **Psurf learned on-target: 977.89 mbar, σ 0.06 mbar over 16
samples → depth = gauge × 9.948 m/bar (SG 1.025)**; `BaroMs5837.h` + `src/sensors/Ms5837.h` +
`checkMs5837()` (identity = PROM read + CRC-4 + C1 band + a real D1/D2 conversion — **written but
not measured: the Bar30 is unprocured**); `PwrRegs.h` + `src/sensors/Ina219.h` + `checkPower()` +
new bench `w` (**part not fitted**); identity-based baro selection (a 0x76 squatter gets
`BARO_CONV_BAD` + blocks arming, the working fallback keeps its clamp banner); `README.md` per
AGENTS.md §15. **Decision 38, found by writing the depth math:** the record's "BMP085 fallback
saturates ~9 m" was **10x wrong** — the part's whole gauge range is ~87 mbar ≈ **under 1 m of
water** (~1.2 m at this bench). The banner now prints a *computed* ceiling (`BARO_RANGE_CLAMP
clamps 1.2m@978mbar`) instead of a recited figure. Consequence: **the fitted fallback is not a
dive depth source — the MS5837 procurement is load-bearing, and M3's gate ("fallback verified by
removing the MS5837") cannot close until a Bar30 is fitted.** **Later the same day, decision 39
(user-directed) made the depth path three tiers: MS5837-30BA primary → LPS28DFW (mode 2, FS
4060 hPa ABSOLUTE ⇒ ~30.7 m computed gauge ceiling at this bench, NOT the marketing "40 m")
armable fallback → BMP180 BENCH-ONLY.** `BaroLps28.h` + `src/sensors/Lps28dfw.h` + `checkLps28()`
are written (identity = WHO_AM_I 0xB4 + SWRESET self-clear + **FS_MODE latched on readback** + a
real conversion — no PROM on this silicon, so the capability chain carries the identity; ST's own
vendor driver is the transcription anchor per decision 37) — **written-not-measured: #6067 is
unprocured**. New fault `BARO_BENCH_ONLY = 27` (ERROR, blocks arming) fires when the BMP is the
only depth source that passed identity, and the **on-target baseline changed as intended**: the
BMP-only bench boots `BARO @0x77 BMP180: … — bench-only tier, clamps 0.9m SL1013mbar` +
`ERROR BARO_BENCH_ONLY BMP180 sole: bench only` + **`[arm] REFUSED`** (was: `[arm] checklist:
sensors ok`). Integration also found and fixed a real defect: fallback success paths used to clear
`BARO_CONV_BAD` unconditionally, erasing a 0x76-squatter's arming block; clears are now gated on a
per-pass squatter flag. Bench `v` runs the fixed 26-min profile **headless on the board**, closed
loop through the real voter/gas/ZHL stack, mole-balanced against hand calculations (136 pulses /
20.40 amb L, 2 pulses/min at the 30 m bottom — risk 20), SIM-stamped, ~30 s wall, worst tick
**41.5 ms measured on-target** (risk 19 — this was the "hang" that turned out to be cost). M1's last
item, the **alternating-mux scheduler** (`AdcPairWalker` in `src/sensors/Ads1115Bus`), is implemented
and measured (bench `t`): **7.2 SPS per cell achieved** on the fitted clones, interlock busy duty
**95-96%** — decision 34: M5's pulse scheduler must carve explicit conversion-free slots, the
incidental gap is only ~2-3 ms. **Fitted today: two ADS1115-class modules (0x48 + 0x49, the
clones — bench parts only, decision 31) on I2C1, and an SSD1306 (0x3C) plus the Adafruit BMP085
(0x77) on I2C0. Outstanding hardware, not code: the MS5837-30BA (Bar30), the **LPS28DFW breakout
(Adafruit #6067, decision 39 tier-2 — open item 19)** and the
INA219 are unprocured, and the 2x Adafruit #1085 converters have not arrived** — each has its
arrival acceptance test ready and named (§7). The
"BMP085" module on the bench is a genuine part at 0x77 — the earlier "clone" verdict was a
misdiagnosis caused by phantom probe addresses, now corrected (AGENTS.md decision 36, §5g) — and
decision 38 bounds what it can do: sub-1 m of water column, a cross-check, never a dive depth
source.

Two decisions were taken with the user on 2026-11-19 and are now binding (AGENTS.md §18): the ppO2
converters **shall be genuine TI ADS1115s** (decision 31 — the fitted clone stays on the bench for
timing work only), and **GPIO 33 is the Page button while the vibration motor moves to GPIO 16**
(decision 32). The pin map itself moved to `lib/core/src/PinMap.h` under native test (decision 33),
because it had been wrong twice as an untestable paragraph. A later decision the same day names the
procurement: **2x Adafruit ADS1115 breakout, product #1085** (decision 35) — board facts checked
against the Adafruit product page/guide (AGENTS.md §2): TI ADS1115 die, VIN 3.3 V, default 0x48 with
one back-side `A0` solder jumper giving 0x49 (close it on U2), ALRT/DRDY on the header, on-board 10k
I2C pull-ups (add none externally). Acceptance on arrival is unchanged: identity stays a
*measurement* claim — `d`/`a`/`t`, never the label.

The single most important hardware lesson: **neither fitted "ADS1115" module is a genuine ADS1115.**
The second module (0x49) was fitted after this file was first written and measures the same way as
the first — ID register `0x00`, ~61 ms conversion at the 32 SPS field — so `ADC_ID_UNKNOWN` /
`ADC_RATE_MISMATCH` are now raised per address (x2 each) and decision 31 has **two** parts to
replace, not one. Everything about timing in this firmware is derived from measurement, not from the
datasheet. Details in §5, consequences in AGENTS.md §2/§8/risk 16/open item 13.

**Transfer: the tree is a git repo, pushed to GitHub.** Clone on the new machine rather than copying
the directory:

```
git clone git@github.com:ReLiCRSA/espRebreatherController.git    # or the https form
cd espRebreatherController
```

`.pio/` is gitignored (build cache, ~72 MB) — the new machine rebuilds it on first `pio run`.
Branch `master`. **Check `git log --oneline -3` rather than trusting any commit hash written here** —
if §1 or §5 says something the remote does not contain, that is the gap. A fresh clone will not contain the
planned-but-empty directories from AGENTS.md §14 (`docs/`, `data/display/`, `src/dive/`, `src/ui/`,
`src/web/`): git does not track empty directories.

---

## 2. Toolchain on the source machine (what to reinstall on the new one)

PlatformIO Core **6.2.0** installed by the pioarduino installer at `~/.platformio/penv`.

```
curl -fsSL -o get-platformio.py https://raw.githubusercontent.com/pioarduino/pioarduino-core-installer/pioarduino/get-platformio.py
python3 get-platformio.py && source ~/.platformio/penv/bin/activate
```

Then `pio run -e esp32` downloads the rest on first build (platform zip + packages). What is on this
machine, for verification after a fresh install:

| Package | Value on source machine | How to check |
|---|---|---|
| platform | pioarduino `stable` (from the release zip URL in `platformio.ini`) | `pio pkg list` |
| Arduino core | **4.0.0** (not 3.3.x — this matters, see §5) | `grep ESP_ARDUINO_VERSION_MAJOR ~/.platformio/packages/framework-arduinoespressif32/cores/esp32/esp_arduino_version.h` |
| IDF | 5.5.x, precompiled libs; the I2C driver in use is `cores/esp32/esp32-hal-i2c-ng.c` | `ls ~/.platformio/packages/framework-arduinoespressif32-libs/esp32/lib` |
| esptool | 5.6.0, available as `python -m esptool` inside the venv | `python -m esptool version` |
| pyserial | installed in the venv (used for serial capture, §4) | `python -c "import serial; print(serial.__version__)"` |

If `pyserial` is missing on the new machine: `pip install pyserial` inside the venv.

Commands (all from the project root, venv sourced):

```
pio test -e native            # 117 cases, eleven suites, ~9 s, no hardware needed
pio run -e esp32              # links; Flash 503,509 B of 1.5 MB (32.0%), RAM 64,112 B (19.6%)
pio run -e esp32 -t upload    # flashes; verify "Hash of data verified" — port present on this
                              # machine as /dev/cu.usbserial-0001 (RTS auto-reset works, no BOOT
                              # button; opening the port resets the board — flush ~7 s of boot
                              # noise before sending bench commands)
pio device monitor            # or the python capture recipe in §4
pio run -t uploadfs           # LittleFS assets, when data/ exists (web UI, not yet written)
```

`pio test -e native` takes a test **filter** as `-f`, not a positional argument or `-n`
(`pio test -e native -f "test_sim"`). Unity aborts the process at the first failed assert, so a
suite stops at its first failure rather than reporting all of them. Suite convention: each suite
file ends in a plain `int main() { UNITY_BEGIN(); ...; return UNITY_END(); }` — PlatformIO's native
glue links against `main`, so a named entry like `test_baro_suite()` fails with an
undefined-`_main` link error.

Address/UB sanitizer repro pattern for the pure-C++ libraries (this is what caught a 3-channel
`readMv` overrun that nothing else would have shown):

```
clang++ -std=gnu++17 -g -fsanitize=address,undefined -Ilib/core/src -Ilib/sim/src \
    <driver.cpp> lib/sim/src/*.cpp lib/core/src/*.cpp -o /private/var/folders/_s/z5y4c95x3fb5pg9z7s7s2cgc0000gn/T/opencode/san && \
    /private/var/folders/_s/z5y4c95x3fb5pg9z7s7s2cgc0000gn/T/opencode/san
```

---

## 3. Board identity and serial access

Detected on this machine — re-check on the new one, the device name will differ:

```
pio device list
/dev/cu.usbserial-0001   USB VID:PID=10C4:EA60  CP2102 USB to UART Bridge
esptool: Chip type ESP32-D0WD-V3 (revision v3.0), Dual Core + LP Core, 240 MHz, crystal 40 MHz
         MAC c0:49:ef:ca:38:28      -> matches board = esp32doit-devkit-v1
```

- **No BOOT button needed**: RTS-based auto-reset works through the CP2102, both for upload and for
  the reset toggles in the capture recipe below.
- `python -m esptool --port <port> --connect-attempts 3 chip-id` works for a quick sanity check
  (ESP32 has no chip ID; it prints the MAC).
- If the port is busy or missing: `ls /dev/cu.usb*`, and close any monitor holding it.
- The board stays powered and enumerated on this desk, so `pio run -t upload` works without touching
  the hardware at all.
- **Exception, recorded 2026-11-20 (the current state at the time of writing):** after the M3 Bosch
  baro driver was built, the devkit stopped enumerating on USB — `ls /dev/cu.usb*` matches nothing,
  `system_profiler SPUSBDataType` shows no CP2102, and `pio run -e esp32 -t upload` exits with
  "Error: Please specify `upload_port`". **Recorded as a fact, no defect inferred** (AGENTS.md §20
  item 16's lesson — the usual causes are the cable, the port, or the board's power switch, and the
  firmware on the chip is unchanged and still running whatever was last flashed). First action on
  resume: reseat/reconnect USB (try another cable/port if needed), confirm `pio device list` shows
  the CP2102 again, then flash. Nothing in the toolchain or `platformio.ini` changed to cause this,
  and the same image builds and links fine.
  **Closed later the same day:** `/dev/cu.usbserial-0001` reappeared on its own (no cable was
  touched between attempts, per the user); two flashes verified after that. Recorded as a fact,
  no cause assigned — item 16's lesson. The board is back to "stays powered and enumerated"
  behaviour for now.

## 4. How to capture the boot log (the recipe that works)

`pio device monitor` is fine interactively, but for scripted capture — including a clean reset so
the banner is caught from byte 0 — this is what has been used every time:

```
source ~/.platformio/penv/bin/activate && python - <<'PY'
import serial, time
s = serial.Serial('/dev/cu.usbserial-0001', 115200, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(0.2); s.setRTS(False); s.reset_input_buffer()
end = time.time() + 13          # >5 s so one hot-reprobe cycle is included
buf = b''
while time.time() < end: buf += s.read(4096)
s.close()
print(buf.decode('utf-8', 'replace'))
PY
```

Send bench commands by writing to the same port (`s.write(b'a\n')`) between reads, then read for a
few seconds. `d` (rate characterisation) needs ~6 s and prints 8 lines per converter.

Console noise to expect: **`esp_core_dump_flash: No core dump partition found!` twice per boot**.
It is cosmetic (no core dump partition by design — no flash budget) and it is printed before
`setup()` runs, so `esp_log_level_set()` from `setup()` cannot silence it. Do not chase it.

## 5. Measured facts about this hardware — do not re-derive, do not regress

### 5a. I2C probing, Arduino core 4.0.0 / IDF 5.5

Core 4.0.0 uses the **new IDF-5 driver** (`cores/esp32/esp32-hal-i2c-ng.c`), not the legacy one. Its
semantics were measured on this board and are recorded in `src/sensors/BusProbe.cpp`; they are also
AGENTS.md decisions 25–26 and risks 14–15. **Source-confirmed** by reading `i2cWrite()` in that file:
a zero-length write routes to `i2c_master_probe()` (address phase, writes nothing); any payload
routes to `i2c_master_transmit()` **and** caches a per-address device handle, so a probing write also
leaves state behind. Addresses >= 128 are rejected outright.

| Probe shape | Result on a bare bus | Verdict |
|---|---|---|
| `beginTransmission(a); endTransmission(false)` | **0 at every address** | unusable — it is what made an empty bench report `ALL SENSORS OK` |
| `beginTransmission(a); endTransmission(true)` | **2** (`ESP_ERR_INVALID_RESPONSE`, NACK address) | **this is the probe** — routed to `i2c_master_probe()`, writes nothing |
| `beginTransmission(a); write(0x00); endTransmission(true)` | 4 | **banned**: writes 0x00 to a fitted device (INA219 calibration cleared, ADS1115 config reprogrammed) |
| `requestFrom(a, 1)` | 0 bytes + driver error line | cannot prove absence (SSD1306 is write-only), spams the console |

**What the returned number means**, read out of `Wire.cpp` (core 4.0.0) rather than guessed:
`0` ok · `1` data too long · **`2` NACK on the address — nothing there** · `3` NACK on data ·
`4` other failure · **`5` timeout — the bus is being held**. `2` vs `5` is a diagnosis, not noise: on
2026-11-19 the ADS1115 module booted once with `I2C1 SDA held low before init`, both converters
reporting `err 5`, `I2C_BUS_STUCK` raised, and nine clocks + a port re-init not freeing it — that is a
line being held (most often a device that lost its supply and is clamping SDA/SCL through its
protection diodes), and it looked completely different from an absent device. **The episode turned out
to be the user unplugging and re-fitting the module — bench intervention, not a fault** (AGENTS.md §20
item 16, closed). Keep the `2`-vs-`5` reading anyway: it is the difference between "nothing at that
address" and "something there is holding the bus", and an unpowered or unplugged device clamping
SDA/SCL through its diodes is exactly what a converter failing mid-dive will look like in the log.
Just do not diagnose a defect from it without knowing what happened at the bench.

The positive direction is **no longer a caveat**: `0x48` (ADS1115 module) and `0x3C` (SSD1306) both
ACK, `I2C1: 1 device(s) [0x48]`, `I2C0: 1 device(s) [0x3C]`, and the matching faults clear while the
missing ones stay raised.

Other measured facts:

- `Wire1` on **GPIO 18/19 initialises fine**. The earlier "I2C1 is broken" suspicion was a software
  defect, not hardware. **The pin map in AGENTS.md §3 stands unchanged.**
- `pinMode()` on a pin owned by a live Wire port goes through periman and **deinits that port**
  (`i2cIsInit` -> 0, later transactions report `bus is not initialized`). Stuck-SDA checks are only
  legal before `begin()` or after `end()`.
- periman bus types observed on I2C pins after init: `34` = `ESP32_BUS_TYPE_I2C_MASTER_SDA`,
  `35` = `..._SCL` (`perimanGetPinBusType()` is a useful diagnostic).
- Address conventions: `Wire` takes **7-bit**, and the 7-bit form must come from the datasheet
  itself, never from arithmetic on a quoted byte (AGENTS.md decision 36). Corrected set: MS5837 =
  **0x76** (TE DS "Address (7 bits) 0x76 (1110110 b)"; the circulating "0x7C" is the 8-bit write
  byte), BMP085/BMP180 = **0x77** (Bosch DS Table 7: bits `1110111`+R/W, bus bytes 0xEE/0xEF).
  The old 0x3E/0x3B were produced by halving those bytes and are **phantoms — no part answers
  there**. ADS1115 0x48/0x49, SSD1306 0x3C/0x3D, INA219 0x40 are already 7-bit in their datasheets.
  Caution: BMP280/BME280 also strap to 0x76/0x77, so presence at those two addresses never proves
  which part it is — MS5837's PROM read or a real Bosch conversion does.
- `CONFIG_LOG_DEFAULT_LEVEL` / `CONFIG_ARDUHAL_LOG_DEFAULT_LEVEL` are both **1 (ERROR)** in this
  core, so driver ERROR lines cannot be filtered below ERROR at runtime — the fix for console spam is
  bounded probing, not log suppression.
- Boot probing discipline: fitted address table only (~7 transactions/bus). The 112-address sweep
  is the `s` bench command; on a port that failed to init it emitted ~200 error lines and buried
  the banner.
- **Two addresses, one device** bites twice: baro (0x76=MS5837, 0x77=BMP085 — two *different*
  parts, primary and fallback) and OLED (0x3C/0x3D — one device, two strap addresses). Raising a
  fault from the strap that did not answer blocks arming against working hardware. Handled
  in `probeDevices()`; if a two-address device is added, extend that logic, do not add a third row and
  hope.

### 5b. The fitted ADS1115 module — read this before writing any timing code

Fitted at 0x48 on I2C1 (ADDR -> GND), measured 2026-11-19 with bench commands `a` and `d`:

- ACKs its address. **Config register reads and write-verify correctly** (wrote `0xA55A` to
  `lo_thresh`, read it back, restored it). It converts. It is not dead and it is not an I2C problem.
- **Power-on config word: `0x8583`** = single-shot, PGA gain 2 (+/-2.048 V), 8 SPS, mux AIN0/AIN1 —
  exactly the datasheet's documented default. That word is the anchor `test/test_adc/` decodes the
  field positions against; if a mask change stops it decoding that way, the mask moved.
- **ID register answers `0x00`. Pointer register answers `0x00` and does not track writes.** A genuine
  ADS1115 answers `0x85` at ID. So a zero ID proves nothing about which part it is, and the ID
  register cannot be used for part identity.
- **The RATE field implements four rates, not eight** — it ignores bit 4 and selects via bits 6:5:

  | RATE field | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
  |---|---|---|---|---|---|---|---|---|
  | datasheet SPS | 8 | 16 | 32 | 64 | 128 | 250 | 475 | 860 |
  | **measured SPS** | 8.1 | 8.1 | 16.1 | 16.1 | 31.9 | 31.9 | 63.7 | 63.7 |
  | **measured period (us)** | 123634 | 123622 | 62011 | 61996 | 31373 | 31369 | 15696 | 15699 |

  Reproduced across two runs; each adjacent pair agreed within 0.5%, which is what shows the field is
  quantised rather than the measurement noisy.
- **Consequences that are load-bearing:** the design's "32 SPS" field delivers **16.1 SPS**, so the
  per-cell rate after walking both pairs is **8 SPS, not 16**. The **coil/ADC interlock window is
  62 ms, not 31 ms** — a window coded from the datasheet would leave half of a live conversion
  unguarded, and coil noise looks exactly like cell disagreement, which trips voting. 860 SPS is
  unreachable. **Decided 2026-11-19 (AGENTS.md decisions 31 + 35): the replacements are 2x Adafruit
  ADS1115 breakout #1085** and this module stays bench-only for timing work. Outstanding action, not
  an open question: when the boards arrive (U2 = `A0` jumper closed for 0x49), `d` must measure
  8/16/32/64/128/250/475/860 SPS and `a` must read ID `0x85`, and `ADC_ID_UNKNOWN`/`ADC_RATE_MISMATCH`
  must clear by themselves. Note that **"ADS1115S" has no datasheet** — so identity here is always a
  measurement claim, never a part-name claim, Adafruit boards included until they pass `d`/`a`.
- **Config-register read granularity: 348–365 us per poll.** That is the polling cost of watching the
  OS bit, and it is why DRDY (open item 14) matters: with DRDY wired, the interlock becomes an
  interrupt instead of a 350 us poll loop.
- Cell inputs are **floating** right now — no 1 MOhm common-mode resistors, no cells. Conversion
  *values* are meaningless garbage; only the timing is meaningful. Do not let a plausible-looking
  mV reading from this bench be believed (AGENTS.md risk 6).

### 5c. Two register-map faults that were live in this code, and are now pinned by tests

Both produced plausible-looking numbers, which is the failure mode of this codebase (risk 13):

1. **RATE masked from bits 10:8** (overlapping PGA) instead of **6:4**. A write that looked like
   32 SPS set something else; the readback decoded as an ordinary-looking rate.
2. **MODE polarity inverted.** Bit 8: **1 = single-shot, 0 = continuous.** Leaving it clear put the
   part in continuous mode — which is why the first conversion test reported "OS bit stuck at 0", and
   which would also have locked the mux and made the two-pairs-per-converter scheme impossible.

Fix and prevention: the constants moved to `lib/core/src/AdcRegs.h` (pure C++, reachable by
`pio test -e native`) and `src/sensors/Ads1115Bus.h` aliases them — decision 28. `test/test_adc/`
tiles the config register's masks, decodes `0x8583`, and pins the LSB tables (**7.8125 uV at gain
16**, not the 0.125 mV often quoted, which is the gain-1 figure).

### 5d. GPIO pads — measured by bench command `g`, and now a tested table (decision 33)

The pin map lived as a paragraph in `src/config/Pins.h` and was wrong twice. It now lives in
**`lib/core/src/PinMap.h`** (pure C++, capability word per pad + the role assignment), `Pins.h` is an
alias, and `test/test_pins/` (10 cases) walks it. Bench command **`g`** prints, for every pad, what the
table claims and what the core actually owns (`perimanGetPinBusType`), plus measured pull and output
behaviour on pads with nothing wired. Measured on this board 2026-11-19:

| Claim | Measurement |
|---|---|
| Console pads | **GPIO1 = UART-TX, GPIO3 = UART-RX**, both reported owned by UART0. The old §3 note "16/17 are UART0 on most devkits" is **false on this board** (the core's own `doitESP32devkitV1` variant header says `TX = 1; RX = 3`) |
| GPIO 16 / 17 | free before use, **output-capable, internal pull-up/pull-down work** (pull-up reads 1, pull-down reads 0), driven level retained when left high-Z. On a WROVER these two are PSRAM CS/clock — this build is WROOM-32 |
| GPIO 32 / 33 | output-capable, pulls work. 33 is now **BTN_PAGE**; the vibrator moved to 16 (decision 32) |
| GPIO 34–39 | **input-only and no internal pull-up/pull-down** (driver: `GPIO number error`). A switch there needs an external resistor — a BOM item |
| GPIO 24, 28–31 | **do not exist on the ESP32-D0WD-V3 die**: periman answers `Invalid pin`. `g` does not even ask, because the table knows |
| GPIO 20, 37, 38 | real die pads that **WROOM-32 does not bond out** — a different reason, kept separate in the table (`kDieOnly`) |
| GPIO 6–11 | inside the module's SPI flash path |
| Spare budget | after the map, **exactly one spare output-capable pad: GPIO 17** (asserted as a number in the test, so freeing a pin forces the §3 budget to be re-derived). `g` also lists 34–36 and 39 as "spare (input-only)" — reachable, but they cannot drive anything, which is the trap the test closes |

Two consequences worth remembering: **an output role on 34–39 fails silently** (the haptic alarm simply
never fires — now a test failure), and **the core's default SPI pins are 18/19/23**, which collide with
I2C1 and `ADS_U1_DRDY`, so a SPI display panel cannot be added by calling `SPI.begin()` with defaults.
That is AGENTS.md open item 15, and it has to be argued together with the panel choice (item 6).

### 5e. On-target compute cost — measured 2026-11-19 (AGENTS.md risk 19)

Host timings do **not** transfer. Measured on this board, with the same binary running the same fixed
profile:

| What | native (x86, -O1) | ESP32 (xtensa soft-float) |
|---|---|---|
| `expf` | ~40 ns | **~3 us** |
| an `NDL=999` tick, before fixing | 0.5 ms | **1.15 s** |
| the same tick after decay-factor hoisting + phased NDL horizon | 0.07 ms | — |
| worst tick of the 26-min sim profile | — | **1145 ms → 41.5 ms** |
| whole headless sim run | ~0.8 s | ~30 s wall |

Three things follow, and all three are already in the code:
- Every engine loop is capped **and** its saturation is reportable: `DecoPlan.bounded`,
  `Engine::lastPlanSteps()`, and a `SIM SLOW` line printed *before* the next tick so a tick that never
  finishes still leaves its neighbourhood on the console.
- Bench `v` prints a **worst-tick cost line** (`SIM tick cost: worst NNNNus at t=N`). Read it; do not
  assume cost from a host run.
- A long-running bench command must yield. The idle task WDT is armed on this core, so `v` does
  `delay(1)` every 15 engine ticks — without it the first run died mid-profile with
  `TG1WDT_SYS_RESET`.

Memory, measured: `Snapshot` is 280 B, so the history ring is the whole footprint. `kHist` = **96** on
Arduino (DRAM settles at ~19%, 63,648 B) and 512 on the host. `SimRunner` is a **static local** in
`benchRunSim`, never a stack local — it is tens of kilobytes and the loop task stack is 8 KB. The ring
is a ring: bench `v` sets `setHistoryStride(16)` so 96 entries cover the whole 1560 s profile; at
stride 1 it would silently hold only the first 96 s. Re-measured after the risk-20 units fix, same
binary, same profile: worst tick **41,492 us at t=1081 s** (the `NDL=999`/plan-heavy ticks still
dominate; the physics change did not move cost), whole run ~30 s, and the gas figures are now the
mole-balanced ones (136 pulses / 20.40 amb L; 2 pulses/min through the 30 m bottom).

### 5f. Pair walkers — achieved ADC rate and the interlock duty, measured (decision 34)

Measured 2026-11-19 on this board with the walkers running in `loop()` (three `t` reads at uptimes
17 s / 60 s / 26 s). Do not re-derive these; re-read them with `t` after any part or firmware change:

| Figure | Measured |
|---|---|
| Boot-timed conversion at the 32 SPS field | 61,870–62,120 us (varies slightly per boot) |
| **Achieved per-cell rate** | **7.21–7.23 SPS** at both 0x48 and 0x49 |
| In-walk conversion period | min 65,498 / avg 66,319–66,871 / max ~67,100 us |
| Interlock **busy duty** | **95–96% of wall time** |
| 60 s continuous run | ioErr 0, convTimeout 0, lateDetect 0; sample ages < 200 ms |

What the numbers mean: the design asks for 16 SPS per cell; the clones' real sizing figure is 8.05;
the walker lands at ~7.2 because the OS-poll detection lag (~4.4 ms on average on top of the 62 ms
conversion) is paid once per conversion. In-walk periods exceed the boot measurement for the same
reason — with DRDY wired (open item 14) they would converge. Max-period spikes around 105 ms appear
when a bench print blocks the loop — that is the poll being late, not the part being slow.

**The number that changes M5: busy duty 95–96% means the incidental inter-conversion gap is only
~2–3 ms — a 20–250 ms coil pulse cannot fit in it.** "Pulses queue into the gap" (AGENTS.md §8)
therefore requires the pulse scheduler to *carve explicit conversion-free slots* into the walker's
cycle — decision 34 — not to hunt for gaps that measurement says are not there.

One reporting bug found by running this: `periodMin` printed 0 because `begin()` seeds the EMA from
the boot measurement, so the first-sample branch that initialises min/max never ran. Fixed; min now
reads 65,498 us. The counters are cumulative since `begin()` and include traffic stirred by bench
commands `a`/`d` (they write the config register mid-walk); the report says so rather than
pretending the run was clean.

### 5g. The fitted BMP085 is **genuine** — the earlier "clone" verdict was a misdiagnosis (AGENTS.md §2, item 17 closed, decision 36)

**Read this before repeating the mistake.** On 2026-11-20 the bench module was written up as a
Bosch-family clone on the strength of three "datasheet disagreements". All three were *report*
errors; the module is an Adafruit BMP085 (BMP180 die) behaving exactly as its datasheet says:

- **"Silent at 0x3B"**: 0x3B was produced by halving the datasheet's 0x77. Bosch
  BST-BMP180-DS000-09 Table 7 — "BMP180 addresses: A7..A1 = `1 1 1 0 1 1 1`, W/R" — the 7-bit
  address **is 0x77** (bus bytes 0xEE write / 0xEF read). The module answering at 0x77 is the
  *expected* behaviour; the probe table was the broken part. Fixed: table now probes 0x76/0x77.
- **"`0xD1=0x02` not 0x43"**: register 0xD1 (version MSB) is in the BMP085 manual only — the BMP180
  datasheet has no 0xD1. Reading an undefined register and getting 0x02 proves nothing.
- **"AC1=8604 not 7295"**: the 0xAA–0xBF calibration block is **per-chip factory trim**; 7295 is
  just the datasheet's worked-example value. 0x219C (8604) is normal.

Measured fingerprint (bench `b` — it *writes* the Bosch pointer register, so bench-only, same class
as `a`/`d`), at I2C0, after the fix:

```
[baro] identity by CHIPID reads (writes the Bosch pointer register — bench only, same class as 'a'/'d'):
  I2C0 @0x77 ACK  0xD0=0x55  0xD1=0x02(BMP085:0x43,BMP180:none)  AC1=0x219C  0x30=0x00  -> BMP085/BMP180 (CHIPID 0xD0=0x55)
```

`0xD0=0x55` at 0x77: genuine BMP180-die signature. Consequences:
- Boot now **finds the fallback**: `I2C0: 2 device(s) [0x77 0x3C]`, `ERROR BARO_RANGE_9M depth
  clamps 9 m`, `[arm] checklist: sensors ok` (INA219 and BARO_RANGE_9M are ERROR and
  deliberately non-blocking — §13 allows arming on the fallback with the permanent banner).
- **The fallback part question is settled — the fitted module is it.** `Bmp085.cpp` may be written
  against it; its conversion behaviour becomes believed only when it produces a real, sane
  pressure (register identity is necessary, not sufficient — same rule as the ADCs).
  **Update 2026-11-20: the driver is written AND proven on the die** (`lib/core/src/BaroBosch.h`
  + `src/sensors/Bmp085.h` + `checkBaro()`, decision 37 — see AGENTS.md §21 for the four
  transcription faults the Adafruit-library cross-check caught before any of it ran). The proving
  run happened, with `b` now also dumping the full cal ROM:

  ```
  cal ROM: AC1=8604 AC2=-1137 AC3=-14532 AC4=32654 AC5=24782 AC6=19804 B1=5498 B2=60 MB=-32768 MC=-11075 MD=2432
  driver (oss=2): 977.78 mbar -> 977.75 mbar  25.4 C   UT=28504 UP=162021 B5=4062
  Adafruit lib (oss=2): 977.76 mbar  25.4 C
  ```

  Three independent agreements close item 17's second half: firmware driver vs deployed library
  **0.02 mbar** (0.00–0.15 settled; worst 0.5 from reads 0.5 s apart in a fast thermal
  transient), and a **host hand recompute** from the dumped words (UT=28580 UP=161865 → B5=4136,
  T=25.9 °C, 97786 Pa) reproducing the on-target driver — xtensa == host arithmetic on real
  words. Stimulus: finger-squeeze drove die temp 25.2→31.0 °C (B5 4034→4953) with the reading
  held inside ±0.4 mbar (temperature rejection works); pressing the port moved it coherently
  (977.7→978.4). **The capture also produced — and same-day corrected — item 18:** the first
  absolute-pressure cross-check used **Cape Town** sea-level pressure (1017 hPa) — the wrong
  city, an assumption; the bench is in **Stockholm, Sweden** (§6). Against same-morning local
  stations (SMHI Stockholm-Observatoriekullen 986.7 hPa, Bromma SYNOP 987 hPa, NOAA ESSB
  altimeter 984 hPa, older), the module's 977.7 mbar at the user-confirmed 0–100 m elevation
  band implies **−9…+3 hPa** — consistent with the BMP180 absolute-accuracy window
  ("−4.0 / −1.0* / +2.0 hPa") for a bench at ~40–90 m, and inside the references' own ±3 hPa
  spread at Stockholm's typical 15–30 m. Item 18 closed; nothing indicts the part; the
  offset-vs-gain alarm (3.8%) is void. Lesson stack, all risk 21: the "~1013 mbar at the
  surface" expectation was never verified, and its replacement "anomaly" was computed against
  weather for a city the bench is not in. **A pressure reference is a measurement with a place,
  an hour and a source — state all three before calling anything an anomaly.**
- **The MS5837 (0x76) is still not procured/fitted.** Its probe-table entry is datasheet-derived,
  not measured here. And 0x76/0x77 are shared with BMP280/BME280 (CSB strap), so an MS5837 at 0x76
  is confirmed by its PROM read, never by the ACK; one baro per I2C0 max.
- The general lesson (AGENTS.md risk 21): honest measurements compared against an unverified
  expectation convict good parts. Confirm expected-presence with the `s` sweep, not the fitted
  table alone.

## 6. What is on the bench right now

- **Bench location: Stockholm, Sweden** (user, 2026-11-20), ground elevation inside the
  user-confirmed 0–100 m band. Any weather cross-check uses local stations — SMHI
  Stockholm-Observatoriekullen (986.7 hPa on 2026-11-20 04:00), Stockholm/Bromma SYNOP
  (987 hPa), ESSB METAR — never a city picked by assumption (item 18's lesson).
- ESP32 Doit devkit V3 over USB. **Wired: two ADS1115 modules on I2C1 — 0x48 (ADDR->GND) and 0x49
  (ADDR->VDD), both VDD 3.3 / SDA 18 / SCL 19 — one SSD1306 and one Adafruit BMP085 (BMP180 die,
  0x77, §5g) on I2C0.** These two ADC
  modules are the clones of §5b: fitted for timing work only, to be replaced by 2x Adafruit #1085
  (decision 35, §7 step 1). Not wired: the
  MS5837 (still unprocured), the LPS28DFW #6067 (decision 39 tier-2, also unprocured — open item
  19), the INA219, the solenoid, all buttons, buzzer, motor, watchdog.
  Cell inputs floating, DRDY
  lines not connected (open item 14).
- **Flashed image vs working tree (2026-11-21): the board runs the M7 image** (commit `662dbc3`);
  **M6's work is uncommitted in the working tree and does not link** (`git status`: modified
  `src/main.cpp`, `platformio.ini`, `BootReport.h`, `FaultCode.h`, `Voting.*`, `test_boot`,
  `test_voting`; new `lib/core/src/ConfigSchema.*`, `StateJson.h`, `DiveSchema.h`,
  `src/config/RuntimeConfig.*`, `src/dive/`, `src/web/`, `test/test_config/`, `test/test_webjson/`,
  plus `data/` still to write — it holds only an empty `display/templates/` tree, no `index.html`,
  `app.js` or `style.css`). Expect `dram0_0_seg` overflow on `pio run -e esp32` until §7's cut
  list is applied — that is a real defect to fix, not a configuration to work around.
- Firmware flashed = current `src/` at HEAD of this session, **including all of M3's code, the
  decision-39 three-tier baro path, and the whole M7 display stack**
  (BaroBosch/Bmp085, Depth/SurfaceLearner, BaroMs5837/Ms5837/checkMs5837, BaroLps28/Lps28dfw/
  checkLps28, PwrRegs/Ina219/
  checkPower, bench `w`, the decision-38 computed clamp banner, the decision-39 tiering +
  `BARO_BENCH_ONLY`; display engine + `src/ui/Ssd1306I2C` + `checkDisplay()` + 5 Hz
  `displayService()` + bench `o` — decision 40) — flashed 2026-11-20 (M7 pass), uploads verified
  ("Hash of data verified"), 528,505 B flash / 85,920 B RAM. Boot now learns Psurf and prints the **computed**
   clamp: `BARO @0x77 BMP180: 978.58 mbar 25.5 C (…) — bench-only tier, clamps 0.9m SL1013mbar` then
   `[psurf] learned 978.58 mbar … depth = gauge pressure x 9.948 m/bar (SG 1.025)`, with no
   `BARO_CONV_BAD`, plus `[oled] SSD1306 up @0x3C`. (977.7–978.6, not the ~1013 previously assumed here — that nominal
   expectation was never verified; and the item-18 "anomaly" computed against it was a wrong-city
   reference, corrected the same day — Stockholm's own SLP is 984–987 hPa, see §5g.)
   Captured boot output with exactly the above fitted, verbatim (**the M7 baseline: unchanged from
   the decision-39 baseline — `BARO_BENCH_ONLY` + `[arm] REFUSED` stay intended — plus the new
   `[oled]` boot line and the clipped-banner report below the help line**):

```
CCR dive computer — boot (never fatal), entering BENCH
[i2c] I2C0 sda=21 scl=22 ok
[i2c] I2C1 sda=18 scl=19 ok
  I2C0: 2 device(s)  [0x77 0x3C]
  I2C1: 2 device(s)  [0x48 0x49]
  ADS @0x48 ID 0x00 — not the documented ADS1115 ID
  ADS @0x48 converts: 62350 us for the 32 SPS field (DS 31250 us) = 99% off
  ADS @0x49 ID 0x00 — not the documented ADS1115 ID
  ADS @0x49 converts: 61808 us for the 32 SPS field (DS 31250 us) = 97% off
  BARO @0x77 BMP180: 978.58 mbar  25.5 C  (UT=28520 UP=162091 B5=4078 oss=2) — bench-only tier, clamps 0.9m SL1013mbar
[oled] SSD1306 up @0x3C — bench 'o' runs the template gate (validate/rejections/fit report) + demo render
[psurf] learned 978.58 mbar (sigma 0.08 mbar over 16 samples, 25.5 C) — depth = gauge pressure x 9.948 m/bar (SG 1.025)
  ERROR INA219                 x1  0x40 err 2
  WARN  ADC_ID_UNKNOWN         x2  0x49 ID 0x00 not 0x85
  WARN  ADC_RATE_MISMATCH      x2  0x49 61mS DS31mS 97%
  ERROR BARO_RANGE_CLAMP       x2  clamps 1.2m@979mbar
  ERROR BARO_BENCH_ONLY        x1  BMP180 sole: bench only
[boot] F:0 E:3 W:2 INA219 ADC_ID_UNKNOWN ADC_RATE_MISMATCH BARO_RANGE_CLAMP BARO_BENCH_ONLY 
[arm] REFUSED — see faults above
[adc] alternating-mux pair walkers running — bench 't' prints the achieved per-cell rate and the coil/ADC interlock window, measured
[o2] solenoid disabled — GPIO 26 will not be driven
bench commands: s=full bus scan  p=probe table + report  r=raw probe codes (destructive, bench only)  c=bus recovery  a=ADC self-test  d=ADC rate characterisation  t=pair-walker rate + interlock window  b=baro identity + live mbar/C reading  w=INA219 battery rail + raw registers  g=pad map + pad test  v=headless sim profile (SIM-stamped, touches no hardware)  o=display: validator rejections + fit report + demo render
[oled] bench frame clipped 1 segment(s) at the panel edge — banner is longer than one line; the fit report is the tool for that (§12)
```

  The clipped-banner line after the help line is the **live 5 Hz frame** reporting its own fit: the
  fault banner is longer than one 21-char line, so one segment clips at the panel edge — counted
  once per change, reported, not hidden (§12). It is expected until the live template's banner is
  shortened or the panel grows.

  The BMP085 at 0x77 is found, verified (CHIPID + cal ROM + real conversion), selected as the
  **bench tier**, and raises two banners — the permanent clamp with the **computed** ceiling and
  its reference (`clamps 0.9m SL1013mbar` at check time, before Psurf is learned and the reference
  is standard atmosphere; `clamps 1.2m@978mbar` in the report after `learnSurface()` learned
  977.93 mbar — decision 38 replaced the old
  `BARO_RANGE_9M depth clamps 9 m` string, which was 10x wrong) — **and since decision 39
  `BARO_BENCH_ONLY`, which does block: `[arm] REFUSED` is this bench's correct baseline while no
  dive-capable tier (MS5837 at 0x76 or LPS28DFW at 0x5C/0x5D) is fitted.** `BARO_RANGE_CLAMP`
  itself is ERROR but deliberately still does **not** block (§13); absent-INA219 does not either.
  If a boot shows `ERROR BARO … no MS5837/LPS28/BMP085` while the module is wired, suspect an
  address-table regression first (§5g), then the wiring. If `[arm]` says `checklist: sensors ok`
  with only the BMP fitted, the decision-39 gate regressed — `test_boot`'s
  `test_bench_only_baro_blocks_arming_but_range_banner_does_not` should be failing.

  (The conversion microseconds vary slightly boot to boot — 61.8-62.1 ms so far; it is the ~97%
  offset and the `x2` clone warnings that are the invariant.)

  Both fitted modules measure as clones — same ID `0x00`, same ~62 ms conversion — so the two WARN
  codes now carry x2 attempts, and `ADS1115_U2` is no longer FATAL. Decision 31's arrival test is
  per-address: `d` must measure the eight datasheet rates and `a` must read `0x85` at **both** 0x48
  and 0x49 (~31 ms conversion, not 62), after which both WARNs clear by themselves. If a future boot
  shows one address near 31 ms and the other at 62 ms, one replacement part arrived, not two.
  If instead something shows `ADC_CONV_BAD`, `ADC_PART_WRONG`, phantom devices, `ALL SENSORS OK` with
  nothing wired, or `bus is not initialized` spam, something regressed: start at
  `src/sensors/BusProbe.cpp`, then `checkAdcs()` in `src/main.cpp`.
  (`ADC_ID_UNKNOWN` and `ADC_RATE_MISMATCH` are WARN and **deliberately do not block arming** — see
  AGENTS.md §13. Do not "fix" them by making them fatal.)

- Bench commands over serial: `s` full 0x08..0x77 sweep · `p` probe table + fault report ·
  `r` raw four-shape diagnostic (needs `-DBENCH_RAW_PROBE=1`, **writes 0x00** to whatever ACKs — bare
  bus only) · `c` 9-clock bus recovery + port re-init · `a` ADC self-test (ID, config decode,
  write-verify + restore, DRDY line, one timed conversion) · `d` ADC rate characterisation (measured
  period for all eight RATE fields, plus the poll cost, ~6 s) · `t` pair-walker report (achieved
  per-cell SPS, in-walk period stats, interlock busy duty, error counters — the §5f numbers, live) ·
  `b` baro identity: Bosch CHIPID/cal-ROM reads **plus** a live temperature+pressure conversion
  through the firmware driver and the Adafruit BMP085 library on the same module (writes ctrl_meas —
  bench only, same class as `a`/`d`; the M3 acceptance run, §7 step 0), **plus the MS5837 identity
  chain on anything answering 0x76** — PROM read + CRC-4 + C1 band + D1/D2 at OSR4096 with the
  DS-MAX waits (runs the dive driver; that is the Bar30 arrival acceptance, §7) — **and the
  LPS28DFW chain on 0x5C/0x5D** — WHO_AM_I, driver init() with FS_MODE latch readback, two live
  10 Hz conversions, plus the computed depth ceiling (the #6067 arrival acceptance, open item 19;
  today 0x5C/0x5D honestly answer nothing) ·
  `w` INA219 battery rail: CONFIG write-verify + raw register dump through the dive driver
  (writes CONFIG/CAL — bench only); today it prints the honest "no INA219 at 0x40" plus the
  meter cross-check recipe ·
  `g` pad map + pad test (~48 lines:
  per-pad capability word from `PinMap.h`, the core's periman owner, and measured pull/output behaviour
  on pads with nothing wired — the source of every claim in §5d).
  `a` and `d` write the config register, so they are bench actions, not something the dive loop runs.
  `v` runs the fixed 26-min sim profile (~30 s wall, touches no hardware). Reference footer, verbatim
  from the current build — these are the mole-balanced numbers (risk 20), and a run whose gas totals
  drift from them has changed physics, not just display:

```
SIM done: pulses=136 injected=20.40 ambL  CNS=4.3% OTU=68  sources=SIM
SIM tick cost: worst 41492us at t=1081s (plan search saturating its step budget is bounded but slow — this number is how you notice)
```

  Hand anchors behind those numbers: 2 pulses/min through the 30 m bottom (0.305 amb L/min metabolic
  demand / 0.15 amb L per pulse), 14 pulses per 7-min hold against 2.15 amb L hand-computed, the
  diluent flush at 30 m landing expected 0.82 bar with worst cell deviation 0.00, and the board
  answering `p` immediately afterwards.

- **A non-healthy boot, for reference** — the module had been **removed from the bench by hand**, which
  is what the user confirms happened. It is recorded because it is the one time the fault report was
  exercised against a real converter-sized problem, and because the reading in its detail is the
  useful part. What the boot looked like:

```
[i2c] I2C1 SDA held low before init — 9-clock recovery
  I2C1: 0 device(s)  []
  FATAL ADS1115_U1             x1  0x48 err 5
  FATAL ADS1115_U2             x1  0x49 err 5
[boot] F:2 E:2 W:0 ADS1115_U1 ADS1115_U2 BARO INA219
```

  `err 5` is **timeout**, not `err 2` (NACK): the bus was being held, not the address unanswered — an
  unpowered device clamping SDA/SCL through its protection diodes. `c` did not free it, which is also
  what a physically absent module looks like. **Not a hardware fault**: an earlier version of this file
  called it a flaky jumper and asked for re-seating; that was a wrong inference, and AGENTS.md §20
  item 16 is closed with the correction. Treat this block as "what a converter that has gone missing
  looks like", useful for recognising the same signature mid-dive, and do not chase it.

## 7. Resume point — the next steps, in order

### M6 (web + config store + dive log) — WHERE IT STANDS, READ THIS FIRST

**Status: written, native-green, NOT committed, and the ESP32 image does not link.** `pio test -e
native` = **173 cases green across 15 suites** (measured 2026-11-21). `pio run -e esp32` = **FAILED:
`region 'dram0_0_seg' overflowed by 57376 bytes`**. The board still runs the M7 image; nothing of M6
has been flashed. Do not start M5 or M8 on top of a non-linking image.

**The DRAM budget, measured from the link map** (`platformio.ini` now emits
`-Wl,-Map=.pio/build/esp32/firmware.map`, kept on purpose):

| Figure | Value |
|---|---|
| `dram0_0_seg` origin / length | 0x3ffbdb5c, 0x1e6a4 = **124,580 B** |
| `.dram0.data` + `.dram0.bss` (map lines 116773 / 117861) | 0x7b39 (31,545) + 0x24b80 (150,400) |
| per-object sum over those two sections | **181,560 B** → overflow 57,376 (ld's figure) |
| **project** statics (our `src/` + `lib/core` + `lib/sim`) | **128,864 B** |
| framework/IDF statics (WiFi, lwIP, FreeRTOS, Arduino core — precompiled `.a`, not ours to cut) | 52,696 B |
| by file | `main.cpp.o` 64,839 · `WebServer.cpp.o` 36,050 · `RuntimeConfig.cpp.o` 26,407 · `DiveLog.cpp.o` 1,568 |

Honest caveat about that arithmetic: an earlier parse of the same map reported "library DRAM 11,255 B"
— a **parse artifact**, not a fact (it matched only one of ld's two line forms, and when a section
overflows some contribution lines carry no address). A wrong total here would send the next session
cutting the wrong 40 KB. The table above is the corrected parse of the two `.dram0` sections; after
any code change, re-derive it from the map instead of trusting these numbers.

**Biggest individual statics (from the map) and the cut that pays for each:**

| Static | B | Cut |
|---|---|---|
| `simRunner()` static instance | 28,020 | `SIM_HIST_CAPACITY` on Arduino: 96 → 32–48 Snapshots (280 B each), verify stride re-derived so the 5 fixed assertions still see the whole profile → **~13–18 KB** |
| `g_tmpl` (the ONE Template scratch, decision 40) | 18,442 | only by deliberately re-facing page/line/segment capacities and updating `test_display`'s pinned `sizeof` → **up to ~6 KB**, only if needed |
| `cfgstore` `g_doc` (`cfg::JsonDoc` node pool) | 8,968 | size the node pool to the real config document (count its nodes) → **~6 KB** |
| five `kJsonCap` (3,072) config buffers | 15,360 | merge to two buffers (read → parse → verify reuses the same storage) and set `kJsonCap` from the **measured** config JSON length + margin (bench `k` prints byte counts) → **~9–12 KB** |
| WebServer per-handler response statics (3,584 ×2, 3,072, 2,200, 1,160, 1,024 ×3, 900, 640, 320) | ~21,700 | **one shared response scratch** (~3,584): every HTTP callback runs on the single `async_tcp` task, serialized — the same argument that already justifies the one `g_body` accumulator → **~18 KB**, the single biggest win |
| `g_prevJson` 2,560 (main.cpp) + `g_prevJson` 2,560 (WebServer) | 5,120 | build the preview hex **directly into the web-side buffer** — safe because `previewAwait()` blocks until `previewDone()`, so the two tasks never touch it at once → **~2.5 KB** |
| `g_verify` (`sim::VerifyReport`) | 4,428 | `AssertionResult::message` 160 → 96 B → **~1.7 KB** |
| `g_body` | 4,096 | 2,048 if the measured config JSON fits with margin → **~2 KB** |
| `g_wsJson` 2,048, `g_simVerify` 1,400, `g_dispBuf` 1,024, `g_faults` 1,280, `g_jobText`/`g_activeTmplText` 1,152 ×2, `g_display` 2,172 | — | keep: payload, fault table, panel frame |

The identified cuts total **~59 KB against a 57.4 KB overflow** — it fits, but with little room, and
M5/M8 add more (DiveTask stack, solenoid buffers, §10's 30-min 1 Hz live-graph ring). Apply the cuts
biggest first and **re-measure the map after each one** (never estimate). If it still does not fit,
the next levers are the `Template` capacities and shrinking `Snapshot` itself — state the new
footprint numbers; several of these sizes are pinned by tests precisely so a capacity change is a
deliberate act.

**Already written and native-pinned (do not rewrite it):**
- `lib/core/src/ConfigSchema.{h,cpp}` — the config struct (**`sizeof(RuntimeConfig) == 384`, pinned
  by `test/test_config/`**), its hand-rolled JSON codec (no ArduinoJson on the config path), the
  server-side range table with per-field errors, and the migration model (absent fields take
  defaults; a `schema` newer than this build is refused). `kSchemaVersion = 1`, `kGasCount = 9`.
- `lib/core/src/StateJson.h` — LiveState → JSON **through `disp::FieldRegistry`** (§12's single
  source of truth: one registry row = one JSON key, same table feeds `/api/fields`), `sim_flag`
  always present (risk 11), and the checked writer `web::detail::Jw` — every append is
  bounds-checked, so an oversized payload is a hard failure, never a truncated document that looks
  complete. `kStateJsonCap` 2048, `kFieldsJsonCap` 3072.
- `lib/core/src/DiveSchema.h` — the §10 23-column `Sample`, the `Header`, `csvHeader()` (writer and
  test share one string) and the header JSON. `test/test_webjson/` (13 cases) pins BootReport JSON,
  state/fields JSON and the CSV row.
- `lib/core/src/BootReport.h` — `toJson()`; closed history entries report `"active":false`.
- `src/config/RuntimeConfig.{h,cpp}` — the store layer: trust rules between the two mirrors,
  **tmp+rename plus read-back verify on both**, the NVS mirror, the config-edit queue applied by
  loopTask, `factoryWipe`, the RAM-only wall clock (`wallNowS()` returns 0 = never set) and
  **`storeInspect()` for bench `k`** (per-store absent / valid / UNTRUSTED / DOWN — "absent" and
  "could not look" are different facts).
- `src/dive/DiveLog.{h,cpp}` — 5-dive ring + index + CSV writer, header written at dive start AND
  end, DELETE refused while a dive is open, all writes on loopTask.
- `src/web/WebServer.{h,cpp}` — the full §9 route set; SoftAP (`DiveCCR-%02X%02X` from MAC[4..5],
  WPA2 `dive-ccr-wifi`) + mDNS `diveccr.local`; the 1-deep `xQueueOverwrite` snapshot queue and the
  ~2 Hz WS push (`snapshotPublish()`); `serveStatic("/", LittleFS, "/")` with an `onNotFound` that
  tells the operator to `pio run -t uploadfs` rather than serving a blank page; and **no `/api/arm`
  route at all** (§13 — the web cannot arm the unit). `/api/solenoid/test` answers **501 with the
  reason** (M5 not built, valve not purchased) instead of pretending a GPIO toggle is a valve test.
- `src/main.cpp` — the M6 integration: boot order (safe pins → **LittleFS** → config → buses →
  identity probes → learnSurface → dive ring → active template → config queue → **web last**, so no
  route answers before the state it reports exists); the air-cal service against real walker
  readings; template save with the §12 SAFETY validator + the fit gate against the fitted panel;
  preview (renders into the panel framebuffer, **never `present()`**); the SIM web run in bounded
  slices (one SIM-stamped frame per tick shared by panel and WS); `webJobsService()`; and new bench
  commands **`k`** (config stores + live settings), **`n`** (AP/web status), **`j`** (the fixed SIM
  profile written into the dive ring, `source=SIM` in the header and every row — §10's real
  auto-start cannot fire until M2 produces a real ppO2). Bench letters now in use:
  `s p r a d c t b w g v o k n j`.
- **LittleFS mount finding (a trap that would have looked like a dead filesystem):**
  `LittleFS.begin()`'s default 4th argument is the literal partition label **`"spiffs"`**
  (`libraries/LittleFS/src/LittleFS.h`, read at source), while this project's data partition is
  named **`fs`** (`partitions.csv`) — the default asks esp_littlefs for a partition that does not
  exist, so the mount fails on a board with a perfectly good filesystem. Now
  `LittleFS.begin(false, "/littlefs", 10, "fs")`, where `false` = never format on a mount failure (a
  silent format would erase the dive ring and both config copies).
- **§4's core split is stated, not claimed:** there is no dive task yet, loopTask is the dive path,
  and AsyncTCP's `async_tcp` task is created unpinned (`CONFIG_ASYNC_TCP_RUNNING_CORE` default −1,
  read from the installed `AsyncTCP.h`), so today the JSON build and the WS enqueue happen on
  loopTask. The queue keeps the isolation that matters — no shared mutable dive state — and the
  pinning claim belongs to the milestone that creates the task (M5/M8).
- **Labels:** `modeLabel()` (main.cpp) maps `BreathingState` to strings that fit the 12-byte `mode`
  fields (`CCR`, `FLUSH-DIL`, `FLUSH-O2`, `BAILOUT`, `OC`); `gas::breathingStateName()`'s
  `"DILUENT_FLUSH"` is 13 characters and would be silently clipped in the CSV and on the panel.

**Next steps, in order:**
1. **Free the DRAM** with the cut table above, biggest first (WebServer shared response scratch →
   SimRunner history capacity → RuntimeConfig buffers + `g_doc` node pool → preview single buffer →
   `g_verify` message width → `g_body`), re-measuring `.pio/build/esp32/firmware.map` after each
   change until `pio run -e esp32` links. Keep `pio test -e native` at 173 green throughout.
2. **Write the web assets** `data/index.html`, `data/app.js`, `data/style.css` — §9's tabs (Live,
   Gases, Deco, Calibration, Dive log, Simulator, Display, System), a ~120-line hand-drawn canvas
   plotter, the WS client, **no CDN** (the phone has no internet at the water) — then
   `pio run -t uploadfs`.
3. **Flash + capture** (`/dev/cu.usbserial-0001` @115200; opening the port resets the board). Expect
   the M7 baseline unchanged (`BARO_RANGE_CLAMP`, `BARO_BENCH_ONLY`, **`[arm] REFUSED`** — intended)
   plus new `[fs] LittleFS mounted on partition 'fs' …`, `[cfg] …`, `[web] AP DiveCCR-xxxx …` lines,
   then bench `n`, `k`, `j`, `p`, and re-confirm `[arm] REFUSED` still stands.
4. **Verify HTTP/WS from the AP side — and leave the operator's network alone.** The user has said so
   explicitly. This firmware runs a SoftAP and never joins a network; acceptance is bench `n` plus
   curl/WS against `http://diveccr.local` from a machine already on the AP. If a browser check is
   genuinely needed, **ask the user to join the AP manually** — do not change the Mac's Wi-Fi.
5. **Commit + push**, then update AGENTS.md §16's M6 row and this file with the measured evidence
   (link sizes, the map-derived DRAM budget, the boot/`k`/`n`/`j` captures, the curl results).
   M6's gate — "phone on SoftAP, all tabs, offline graphs render" — closes only at step 4.
6. Then M5 (valve purchase, open item 1; the pulse scheduler must carve explicit conversion-free
   slots per decision 34) and M8. **Separately, still owed: cross-validating the M4 deco engine
   against a trusted computer at the specific profile being claimed (risk 13) — it is
   regression-anchored, not validated, and it is life-support math.**

### M0, M1, M7 and M3's code — done (context for the above)

M0, M1 and **M7 (engine + fitted-panel path — decision 40, 2026-11-20: template parse/validate/
fit/render + FieldRegistry + Framebuf/Font6x8/LiveState in `lib/core`, 17 native cases; thingpulse
transport; bench `o` gate captured on-target: bundles OK, validator rejection, §12 fit report,
render 1426 µs, panel frame 22 ms)** are **done** — the alternating-mux scheduler (`AdcPairWalker`,
bench `t`) is implemented and measured: 7.2 SPS/cell achieved, sized from the boot-measured period,
interlock duty 95-96% (§5f). **M3's code is done (2026-11-20 late)** — BMP180 fallback proven on the part, `Depth` +
`learnSurface()` proven on-target (Psurf 977.93 mbar), MS5837 + INA219 + LPS28DFW drivers written and
natively pinned but **not yet measured on parts** (unprocured/unfitted), `README.md` written.
Decision 38 corrected the fallback's depth ceiling from "~9 m" to **under 1 m** — read it before
trusting any depth number — and **decision 39 then made the depth path three tiers (MS5837-30BA
primary → LPS28DFW mode-2 armable fallback → BMP180 bench-only), so the BMP-only bench baseline is
now `[arm] REFUSED` with `BARO_BENCH_ONLY` (§6's current verbatim capture — intended policy, not a
regression). Next, in order:

 A. **[DONE 2026-11-20] Panel visual confirmation.** The user confirms the 0x3C module paints
    `o`'s pattern **readable and correct** — genuine SSD1306-class controller, not an SH1106 clone.
    `src/ui/Ssd1306I2C.h` stands as written; `SH1106Wire` stays a documented fallback, unused.
    Keep the `o` ASCII-vs-panel comparison in the procedure (§7b of the README) — it is the cheap
    discriminator if a future panel swap ever misbehaves.
 0. **[DONE 2026-11-20] Flash + capture the M3 acceptance run.** The port reappeared on its own
   (§3), two flashes verified, and the run passed with one expectation corrected:
   boot prints `BARO @0x77 BMP180: 977.72 mbar  25.2 C (UT=28474 UP=162080 B5=4034 oss=2) —
   fallback, 9 m clamp` *(string superseded later the same day by decision 38: the banner now
   prints the computed clamp — see §6's current verbatim capture: `clamps 0.9m SL1013mbar` →
   `clamps 1.2m@978mbar` after Psurf learning)*, `BARO_CONV_BAD` absent, `[arm] checklist: sensors ok` unchanged; `b`
   shows driver 977.75/977.78 vs Adafruit library 977.76 mbar (0.02 mbar, same °C), and the
   squeeze/port tests move the reading while T tracks 25.2→31.0 °C inside ±0.4 mbar. Numbers
   recorded in AGENTS.md §2 and §5g — **item 17's second half closed**. The "~1013 mbar"
   criterion was the wrong expectation (never verified against a local reference), and its
   replacement was wrong too — Cape Town weather, the wrong city. Against Stockholm stations
   (SLP 984–987 hPa) the module is consistent with the DS accuracy window: **item 18 opened
   and closed the same day** (§5g).
1. **Order 2x Adafruit ADS1115 breakout #1085** (AGENTS.md decision 31, procurement named by
   decision 35 — an outstanding action, not a question; **both** fitted modules measure as clones, so
   that is two boards). Fit: VIN+GND from the 3.3 V rail (never the A+/A- rails — those are the
   board's filtered outputs), SDA 18 / SCL 19, **U1 as shipped (0x48), U2 with the back-side `A0`
   solder jumper closed (0x49)**; the boards' own 10k I2C pull-ups are enough — add none. Optionally
   jumper ALRT→GPIO 23/4 for DRDY (open item 14 — measure polarity before the scheduler uses it).
   On arrival: `d` must measure 8/16/32/64/128/250/475/860 SPS at both addresses and `a` must read
   ID `0x85`, with `ADC_ID_UNKNOWN`/`ADC_RATE_MISMATCH` clearing by themselves (~31 ms conversion,
   not 62), and `t` re-derived (expect ~16 SPS/cell, a few ms of OS-poll lag on top). The walker
   re-begins from each part's own measurement at boot — no code change needed.
2. **Order the MS5837-30BA (Bar30, M10 bulkhead), an INA219 breakout (#904 class, 0.1 Ω), and the
   Adafruit LPS28DFW breakout #6067** (decision 39 tier-2, AGENTS.md open item 19 — $12.50; SA0
   default 0 → 7-bit 0x5C, solder jumper → 0x5D; VDD/GND/SDA/SCL to I2C0, coexists with everything
   there). All three drivers are written, natively pinned and integrated (`checkMs5837()`/`checkPower()`/`checkLps28()`,
   identity-based selection, bench `b` MS5837 + LPS28 sections, bench `w`) — **arrival acceptance, per
   part**: Bar30 → `b` shows PROM CRC VERIFIED + C1 in the 30BA band + D1/D2 conversions landing
   at a Stockholm-station mbar (named place/hour/source, §6 — expect ~975–990 mbar at the bench,
   not ~1013) and the mbar MUST move when the port is squeezed; then `BARO_RANGE_CLAMP` and
   `BARO_BENCH_ONLY` clear by themselves and M3's gate (fallback verified by removing the MS5837)
   can finally run. LPS28DFW → `b` shows WHO_AM_I 0xB4 + SWRESET self-clear + **FS_MODE=1 latched
   on readback** + two agreeing live conversions in the surface window + the computed ceiling
   (~30.7 m at this bench — the check that the ceiling comes from the latched mode, not the
   "40 m underwater" marketing), and it moves when squeezed; that makes the bench armable-with-
   banner while the Bar30 is still out. Adafruit's LPS2X library does **not** support the LPS28DFW
   — the hand-rolled driver here is the only path anyway. INA219 →
   `w` write-verifies CONFIG, reads a real V_BAT, and a **meter cross-check** (meter across V_BAT
   and in series with a known load) retires the 0.1 Ω assumption before any ampere figure is
   believed. Wire the INA219 on I2C0; the cell bias networks
   (1 MOhm + 10 ohm + RC, risk 6) stay a separate wiring step before any cell reading is believed.
3. **Before M5, settle the interlock design against decision 34**: the incidental inter-conversion
   gap is ~2-3 ms (§5f), so the pulse scheduler must carve explicit conversion-free slots into the
   walker's cycle. DRDY wiring (open item 14) would make the window exact rather than polled —
   decide it together with the pin budget (open item 15) and the panel choice (item 6).

After that: `src/dive` (M2 on-device cell wiring/cal/voting flow + M5 solenoid) and `src/web` (M6,
which also carries M7's remaining half — the Display tab and `GET /api/display/preview` over the
already-tested `SimDisplay`). `src/ui` is **begun** (M7: `Ssd1306I2C` transport + `Buttons`; the
layout engine deliberately lives in `lib/core`, decision 40; scenes grow with M8's alarm/banner
work and the RuntimeConfig-driven template selection). `README.md` is **written** (2026-11-20,
AGENTS.md §15's must-contain list — including
the address-arithmetic trap and the decision-38 sub-1 m fallback as listed wiring mistakes);
update it when hardware changes what it teaches.

## 8. Files that hold load-bearing state

| File | What it pins down |
|---|---|
| `AGENTS.md` | binding design record; §16 milestone status, §18 decisions 1–39, §19 risks 1–21, §20 open items 1–19 |
| `HANDOFF.md` (this file) | machine + bench state, measured I2C and ADC semantics, resume point |
| `lib/core/src/AdcRegs.h` | **the ADS1115 register map, PGA/RATE tables, LSB and mV scaling, and the measured bench-module rate table.** Pure C++ so it can be asserted; it was wrong twice while it lived only in `src/` |
| `lib/core/src/BaroBosch.h` | **the BMP085/BMP180 register map, the DS cal-ROM validity rule (no word 0/0xFFFF — the only check the DS states), the command/timing table, `assembleUp`/`assembleUt`, `b5FromUt`, `compensate`.** The compensation is a deliberate int32/uint32-faithful transcription of the deployed Adafruit BMP085 library — the PROVENANCE block at the top lists the four transcription faults the cross-check caught and the example anchor (B5=2400, 15.0 °C, 69964 Pa) that discriminates right from wrong arithmetic. Decision 37; pinned by `test/test_baro/`. Do not "tidy" the types or widen them — the bench cross-check depends on bit-for-bit agreement with the reference |
| `src/sensors/Ads1115Bus.{h,cpp}` | register driver: single-shot start (MODE written explicitly), `checkPart()` identity-by-timing (stores `measuredConvUs()` for the walker and the interlock), `measureAdcRates()`, `selfTest()`. The accepted conversion window (20–150 ms) and why it is wider than the datasheet figure. **`AdcPairWalker`**: the non-blocking alternating-mux scheduler (M1), sized from the measured period, carries the coil/ADC interlock window (`integrating()`, `coilSafeAfterUs()`), and reports achieved rate/counters via bench `t` — see §5f for its measured figures |
| `src/sensors/Bmp085.h` | header-only BMP085/180 driver implementing `Barometer`: CHIPID gate, cal-ROM read + DS rule, blocking T+P cycle with DS waits (`delayMicroseconds(conv+500)`), oss=2, XLSB always read (at oss>0 the 0xF6/0xF7 pair is only the top 16 bits), `Status` enum + raw UT/UP/B5 exposed for bench `b`, `rangeCeilingBar()` = 1.1 bar so `Depth` can clamp — which against a ~1013 mbar surface is **under 1 m of water**, decision 38; the header says so |
| `lib/core/src/Depth.h` | **the depth math as pure C++, pinned by `test/test_depth/`: `ceilingMeters()`/`metersPerBar()` (salt 9.9484, fresh 10.1972 m/bar at g=9.80665) and `depth::SurfaceLearner`** (16-sample window, lock at σ ≤ 2 mbar, 24-sample/120 ms budget — a drifting or breathed-on module must not lock). The decision-38 correction lives here: any behaviour-gating range is **computed from primary specs and printed with its reference**, never recited |
| `lib/core/src/BaroMs5837.h` | **MS5837-30BA PROM map, CRC-4, C1-band model classification (30BA/02BA/outside), compensation.** PROVENANCE: compensation transcribed from the deployed BlueRobotics library (decision 37's pattern); the ≤1 LSB floor-vs-truncate difference against the DS worked example is stated, not hidden; W0 type bits reported, never gated (DS C6 vs C2 revisions contradict). Written-not-measured: Bar30 unprocured |
| `lib/core/src/BaroLps28.h` + `src/sensors/Lps28dfw.h` | **decision 39 tier-2 (LPS28DFW, Adafruit #6067, unprocured):** register map, dual-FS scaling (4096 LSB/hPa FS1260 / 2048 FS4060, left-aligned assembly matched to the vendor shape on a grid), in-range windows, `rangeCeilingBar()` 4.06 bar ABS — the ~30.7 m ceiling is computed by `Depth.h` from the **latched** mode, never taken from the marketing "40 m". PROVENANCE: ST's own vendor driver `lps28dfw_reg.h/.c` ((c) 2024) — WHO_AM_I 0xB4, CTRL_REG2 ONESHOT0/SWRESET2/BDU3/FS_MODE6/BOOT7 (an earlier remembered 0xB8 WHO_AM_I was caught at source). Driver identity chain: WHO_AM_I → SWRESET self-clear **polled** → CTRL_REG1/2 write + readback with **FS_MODE latched** → real conversions, P_OR/`inRangeBar` flagged; `setAddress()` for the 0x5C/0x5D SA0 strap. Pinned by `test/test_lps28/`; written-not-measured until bench `b` runs it (open item 19) |
| `src/sensors/Ms5837.h` | header-only `Barometer` driver at 0x76: RESET → PROM → CRC verify → C1 band → D1/D2 at OSR4096 with the DS-MAX 9.04 ms waits ("maximum values must be used"), `identityVerified()`, raw D1/D2/PROM exposed for bench `b`, model-aware `rangeCeilingBar()` (an 02BA squatter with a valid CRC is a ~10 m part and says so). **Never run on silicon yet — every claim is datasheet-derived until `b` measures it** |
| `lib/core/src/PwrRegs.h` + `src/sensors/Ina219.h` | INA219 register map, presets (assume the 0.1 Ω #904-class shunt — stated, not assumed silently) and **exact-integer `calRegister`** (the float form truncated the datasheet's exact 4096 anchor to 4095; `test/test_power/` pins it). Driver identity = CONFIG write-verify + CAL read-back + real bus-voltage read (no CHIPID exists); CONFIG+CAL re-written before every current read (sharp-load defence — matters with solenoid pulses). Part not fitted |
| `src/sensors/BusProbe.cpp` | the measured four-shape probe table and the periman trap, in comments; the only probe form that is truthful and non-destructive |
| `src/sensors/BusProbe.h` | probe contract, 7-bit address warning, "port must be down" rule for line checks |
| `src/main.cpp` | boot order (solenoid safe-state first), bounded probing, `checkAdcs()` (presence is not identity), `checkBaro()` (same rule at the depth sensor: CHIPID + cal ROM + one real conversion at boot and on BARO presence transitions; raises/clears `BARO_CONV_BAD`), `g_bmp085(Wire, 0x77, oss=2)` global, two-address device selection for baro and OLED, bus dead-vs-absent reporting, 5 s hot re-probe and bus retry, bench command REPL (`s p r c a d t b w g v` — `b`/`baroIdentity()` now reads Bosch CHIPID/cal-ROM **and** runs a live conversion through the firmware driver plus the Adafruit BMP085 library on the same module: the §5g fingerprint and the M3 acceptance run; since M3 `b` also runs the full MS5837 identity chain — PROM/CRC/C1 + OSR4096 conversion — on anything at 0x76), **M3 integration: `g_ms5837`/`g_ina219`/`g_activeBaro` globals, identity-based baro selection (`checkMs5837()` before the BMP180 fallback; a 0x76 squatter gets `BARO_CONV_BAD` + blocks arming while a working fallback keeps its clamp banner), `learnSurface()` (learns Psurf via `depth::SurfaceLearner`, no lock ⇒ `BARO_CONV_BAD`), `checkPower()`, `baroRangeBanner()`/`raiseRangeBanner()` (the decision-38 computed clamp — `clamps %.1fm SL%.0fmbar` pre-Psurf, `clamps %.1fm@%.0fmbar` after), hot-plug `powerChanged`, bench `w`**, **decision 39: three-tier baro selection (`checkMs5837()` returns bool; new `checkLps28()` — WHO_AM_I + SWRESET-polled + FS_MODE-latch readback + real conversion; `selectBaroFallback()` also raises `BARO_BENCH_ONLY`; probe table 0x76/0x5C/0x5D/0x77; `g_lps28` global with `setAddress()` strap resolution; `g_baroSquatter` per-pass flag gating every `BARO_CONV_BAD` clear — found defect: fallback success used to erase a squatter's arming block; bench `b`'s LPS28 section prints identity + ceiling + the "40 m is ABS-FS-as-gauge" note)**, `gpioMapReport()`, `benchRunSim()` (static `SimRunner`, per-tick cost measurement, `SIM SLOW` line, history stride 16); walker globals `g_walk1`/`g_walk2` (begin after `checkAdcs()`, re-begin on converter hot-plug), and `loop()`'s heartbeat as **service windows** — walkers + REPL run inside the HIGH-20/LOW-30 shape so the converters pace the sampling, not the loop delay. **M7 (decision 40): display globals (`g_display`/`g_dispFb`/`g_dispPage`) and the single shared parse scratch `g_tmpl` — Template is ~18 KB, never a task-stack local and never one static per call site (DRAM overflow); `checkDisplay()` at boot + OLED-online hot-plug; `displayService()` 5 Hz live frame from identity-passed sensors only (ppO2 shows 0.00 until M2 — no fake data), 50 ms render budget → `DISPLAY_RENDER_SLOW` WARN + last good frame kept; `buildBenchLiveState()`; bench `o` (gate capture + ASCII preview + timed panel frame); Page-button page cycling** |
| `lib/core/src/Template.h` | the §12 layout engine: parse → **validate** (SAFETY fields required on every page, never inside `@if`; fmt sanitizer rejects user-typed `%`) → layout/fit (priority drop order, §12 fit report pinned verbatim) → render (last-good-frame semantics, truncation counted). Capacities fixed, no allocation in the render path. `sizeof` pinned by `test/test_display/` — footprint rule of decision 40: KB-scale structs are static/boot-time, never task-stack locals |
| `lib/core/src/Fields.h` `Framebuf.h` `Font6x8.h` `LiveState.h` `DisplayDriver.h` `SimDisplay.h` `TemplateBundles.h` | FieldRegistry (name/type/unit/priority/getter — the §12 single source of truth), the page-major 128x64 buffer with clip/gauge/shift, the 6x8 font (glyph anchors pinned; source: fetched Adafruit GFX glcdfont.c), the display snapshot struct, the driver **interface** (lib/core on purpose — §14 updated), the text/JSON preview renderer (the browser preview's engine, HTTP wrapper awaits M6), and the bundled templates + gate demos (`fitdemo_9on8`, `invalid_missing_safety`) |
| `src/ui/Ssd1306I2C.h` | the only panel-touching display code: subclasses thingpulse `SSD1306Wire` (sda/scl=-1 — we own `Wire.begin(21,22)`, decision 26; 400 kHz bus-wide, stated), `present()` = guarded memcpy into the library buffer + `display()` (geometry and nullptr guards, byte-layout provenance from OLEDDisplay.cpp); `pixelShift` = native-tested software wrap. If the panel turns out SH1106-class, `SH1106Wire` in the same library is the drop-in |
| `lib/core/src/FaultCode.h` | the fault enum plus `defaultSeverity`/`blocksArming`/`faultName`. `ADC_PART_WRONG`/`ADC_CONV_BAD` block arming; `ADC_ID_UNKNOWN`/`ADC_RATE_MISMATCH` deliberately do not; `BARO_CONV_BAD` (26) is ERROR and blocks, `BARO_RANGE` (13) deliberately does not (§13 fallback rule) — its `faultName` is `BARO_RANGE_CLAMP` since decision 38 (was `BARO_RANGE_9M`; the enum value never changed, so old logs with the old name read as this same code); **`BARO_BENCH_ONLY` (27, decision 39) is ERROR and blocks** — the BMP085/180 as sole depth source refuses an arm; `DISPLAY_RENDER_SLOW` (28) is WARN — a screen failure never mutes alarms; **`CONFIG_WRITE_FAILED` (29, M6) is ERROR and blocks**: a setting that is not read back is a setting that is not there |
| `lib/core/src/ConfigSchema.{h,cpp}` | **M6 config: `cfg::RuntimeConfig` (sizeof 384, pinned), `kSchemaVersion 1`, the range table + `ConfigErrors` per-field reporting, the JSON codec (hand-rolled; ArduinoJson is not on the config path) and the migration model** — absent fields take defaults, a newer `schema` is refused outright. Pure C++, pinned by `test/test_config/`; §17's rule that every threshold lives in RuntimeConfig, not in engine magic numbers, is enforced here |
| `lib/core/src/StateJson.h` + `lib/core/src/DiveSchema.h` | **M6 serialization faces, both built on the §12 registry rather than a hand-written field list** (a hand-written list is a second copy of every field — the divergence risk 17 describes). `web::detail::Jw` is the checked writer: every append is bounds-checked and a failed document reports itself instead of truncating. `DiveSchema` holds the §10 23-column `Sample`, `Header` and `csvHeader()` shared by writer and test. Pinned by `test/test_webjson/` (13 cases) |
| `src/config/RuntimeConfig.{h,cpp}` | **the config store layer (§9): LittleFS tmp+rename + NVS mirror, both read back before any reply claims success; store trust rules and the fault codes `CONFIG_CORRUPT`/`CONFIG_WRITE_FAILED`; the edit queue applied by loopTask; `factoryWipe`; RAM-only wall clock (`wallNowS()` 0 = never set); `storeInspect()` behind bench `k`.** Its buffers are DRAM-budget-critical — see §7's cut table before adding another |
| `src/dive/DiveLog.{h,cpp}` | §10's 5-dive ring (`/dives/NNN.json` + `.csv` + text index), header written at start **and** end, 60 s samples, oldest evicted, DELETE refused while a dive is open; all writes on loopTask, web reads only (esp_littlefs serializes each operation — read at source, provenance in the header) |
| `src/web/WebServer.{h,cpp}` | **M6 routes + SoftAP + mDNS + the snapshot queue and 2 Hz WS ticker; the header states the threading contract as measured** (async_tcp unpinned, core −1, so §4's core split is NOT claimed until DiveTask exists), the no-`/api/arm` rule, the 501-with-reason for the solenoid test, and the shared per-task scratch discipline that the DRAM budget depends on |
| `test/test_config/` `test/test_webjson/` | config parse/validate/migration (incl. `sizeof(RuntimeConfig) == 384`), and the JSON/CSV faces: boot report, live state, `/api/fields`, the 23-column dive row. Both are native-only — the web handlers themselves are bench/curl-tested |
| `.pio/build/esp32/firmware.map` | **the DRAM budget, measured not estimated.** Emitted by `build_flags` in `platformio.ini`. `dram0_0_seg` is 124,580 B and the radio stack's precompiled statics are not ours to cut, so every project KB-scale static is a budget decision — §7's table lists them and the cut that pays for each. When the region overflowed by 57 KB, arithmetic on the source would have guessed wrong; the map did not |
| `lib/core/src/BootReport.h` | `blocksArming()` refuses on **any active FATAL**, not only on listed codes |
| `lib/core/src/PinMap.h` | **the GPIO capability table (one word per pad, 0..39) and the role assignment.** Pure C++, asserted natively — it was a paragraph in a header before and was wrong twice. Carries `noSuchPad` (24, 28–31: no such pad on the ESP32-D0WD-V3 die) and `kDieOnly` (20, 37, 38: die pads WROOM-32 does not bond out) as separate claims, `unusableReason()`, and `SPARE_OUT = 17` |
| `src/config/Pins.h` | **an alias of `PinMap.h`, not a second copy of the numbers.** `BTN_PAGE = 33`, `VIBRATOR_OUT = 16` (moved off 33, decision 32), `SPARE_OUT = 17`. Drivers must still check `>= 0` before `pinMode`: a role may be unassigned on purpose |
| `src/config/Features.h` | defaults: `HAVE_SOLENOID 0`, `BENCH_MODE 1`, `WIFI_DURING_DIVE 0`, `BENCH_RAW_PROBE 0`, `POWER_RAIL "1S2P"` |
| `platformio.ini` | pioarduino `stable` platform (never the stock `espressif32`), `lib_ldf_mode = deep` + `lib_extra_dirs` + explicit core libs in `lib_deps`, `native` env with `test_build_src = no` |
| `partitions.csv` | 64 KB-aligned app slots, `data, littlefs, 0x320000, 0xC0000` |
| `lib/core/src/Zhl16c.cpp` | deco engine: ZH-L16A coefficients + Subsurface GF/ceiling form, plan search in depth/grid space, `bounded` flag, GF anchor in restorable state, decay-factor hoisting + phased NDL horizon (risk 19). **Do not change without native tests and a trusted-computer cross-check** (risk 13) |
| `lib/core/src/SensorIf.h` | `Barometer`, `CellSource`, `ClockSource`, `SourceKind`/`SourceMix` — the interfaces the real drivers **and** the sim both implement, which is what keeps simulation at the sensor layer. `CellSource::readMv` writes all three channels: callers must pass full arrays (a 1-element array was a real overrun, caught by ASan) |
| `lib/sim/src/LoopModel.{h,cpp}` | the breathing loop, integrated in **moles**. The unit convention is load-bearing (risk 20): `metabolicRateSurfLpm` is a molar draw (depth-independent), `ValveModel::pulseVolAmbL` is an ambient volume (depth-scaled); the file states why and the hand anchor (1.2 surf L/min into 8 L = 0.15 bar/min sag at any depth) |
| `lib/sim/src/SimRunner.{h,cpp}` | the tick path (profile -> SimBaro -> loop/valve -> SimCells -> real voter -> gas model -> ZHL/CNS/OTU/plan), ring history with `histDropped()` + stride, window queries with `windowCount()`, and `verify()`/`Metric` — an empty window is `NO DATA`/`unevaluated`, never a fabricated 0.0 |
| `lib/sim/src/{SimBaro,SimCells,ValveModel,CcrControlLaw}` | sensor-layer doubles + the valve (volt-seconds seat criterion, click signatures, three-fault taxonomy in one model) + the reference PI control law shared by sim and future firmware |
| `test/test_sim/test_sim.cpp` | hand-computed loop physics: depth-independent sag + 276 s floor, closed-loop hold asserted in pulse quanta, click taxonomy, cell faults through the real voter, flush agreement from the other side, source stamping, bit-identical repeats, ring/NO-DATA harness honesty |
| `test/test_adc/test_adc.cpp` | config-field masks tile 0xFFFF once; `0x8583` decodes as the DS default; design config = gain 16 + 32 SPS + explicit single-shot; LSB tables; air-signal code counts; the measured non-datasheet rate table |
| `test/test_baro/test_baro.cpp` | DS command/timing table; the cal-ROM rule as the **datasheet's 0/0xFFFF check, not a value check** (bench AC1=0x219C passes; every Table-5 position poisoned at its own index); the `assembleUp` oss truncation chain (34321/68643/137286/274572); the "use datasheet numbers!" example reproduced exactly (B5=2400, 15.0 °C, 69964 Pa); a 64-point grid match against a second copy of the deployed reference; monotonicity + oss scale invariance; out-of-range flagged; corrupt ROM refused before arithmetic |
| `test/test_boot/test_boot.cpp` | fault severities; absent solenoid is INFO and must **not** block arming; the whole-enum table walk (**gap list `{1,9},{32,39},{45,49},{55,59},{69,79}`, walked count 47** — `BARO_BENCH_ONLY` took 27, `DISPLAY_RENDER_SLOW` took 28 and `CONFIG_WRITE_FAILED` took 29, so the gap list is now shorter; keep it current when adding codes); unlisted FATAL still blocks; `BARO_RANGE_CLAMP` name + computed-clamp detail asserted; **decision 39: `BARO_BENCH_ONLY` blocks arming while the range banner does not — the two baro ERRORs gate-tested apart** |
| `test/test_depth/test_depth.cpp` | the water-hydrostatics anchors (1 mbar ≈ 1.02 cm; salt 9.9484 / fresh 10.1972 m/bar) and **the decision-38 conclusion itself** (BMP180 gauge range < 1 m at sea level, ~1.2 m at the bench Psurf); `SurfaceLearner` lock at σ ≤ 2 mbar, refusal on a drifting series, no half-learned state |
| `test/test_ms5837/test_ms5837.cpp` | CRC-4 against a verbatim copy of the DS C6 published example; C1-band classification; compensation against a verbatim BlueRobotics `br_pressure` copy (pins truncation semantics — code 1982/2381323465/1145816756 vs the DS's floor columns, ≤1 LSB); W0/W7 field extraction |
| `test/test_lps28/test_lps28.cpp` | decision 39 tier-2: vendor sensitivity anchors both FS modes (0x3E8000/0x1F4000 at 1000 hPa, both FS ends), left-aligned assembly vs a second vendor-shape copy across a grid, mode-scaling non-interchangeability (the silent 2x error the latch gate prevents), negative two's-complement branch, temp LSB/100 both signs, WHO_AM_I 0xB4 + 0x5C/0x5D, CTRL_REG1/2 bit positions (FS_MODE=bit 6, not BOOT bit 7), range-window edges, and the **derived ~30.7 m tier-2 ceiling** asserted < the "40 m" marketing figure and >10x the BMP tier |
| `test/test_power/test_power.cpp` | `pwr::calRegister` exact integer math (the 0.1 Ω/32 A preset lands the **exact 4096** — the float form gave 4095), LSB conversions, preset table vs the deployed Adafruit constants |
| `test/test_pins/test_pins.cpp` | no two roles share a GPIO (33 was the motor *and* the Page button); no output role on an input-only pad; no role on a strapping pin; the no-such-pad / die-only / flash sets each asserted separately; 34–39 input-only with no pulls; button pads have internal pull-ups; **the spare output budget is asserted to be exactly one pad (17)**, so freeing a pin forces the AGENTS.md §3 budget to be re-derived |

## 9. Open items that block code, not just BOM

- **Open item 13 is decided** (AGENTS.md decision 31; procurement named by decision 35: 2x Adafruit
  ADS1115 breakout #1085) and no longer blocks any code — the walker sizes itself from a measured
  period, not a constant. With the clones fitted the achieved per-cell rate is **7.2 SPS measured**
  (§5f); the genuine part should land near 16. The only outstanding action is ordering the two
  boards and re-running `d`/`a`/`t` (§7 step 1 has the fit details: 3.3 V rail, `A0` jumper on U2,
  no external pull-ups, optional ALRT jumpers).
- **Open item 14** (DRDY never actually wired, only read pulled-up and idle): the walker now *does*
  poll the OS bit at 10 ms cadence, and the cost is visible in the numbers — the ~4.4 ms average
  detection lag is most of the gap between the clone's 8.05 SPS sizing and the 7.2 SPS achieved
  (§5f). DRDY would make the interlock edge exact and close that gap; wire it before certifying the
  M5 interlock, and settle it against the pin budget (item 15).
- **Open item 15** (new): a SPI display panel needs four pads (SCK/MOSI/DC/CS) and **only GPIO 17 is
  spare and output-capable** after the current map; the core's default SPI pins 18/19/23 collide with
  I2C1 and `ADS_U1_DRDY`. Argue this with the panel choice (item 6) before buying anything — the
  options are freeing the DRDY pads (item 14's other side), taking a strapping pin against the §3
  policy, or keeping the panel on I2C and accepting the frame-time cost.
- **Open item 17 is CLOSED — it was a misdiagnosis by me, not a hardware fault** (§5g, AGENTS.md
  decision 36 / risk 21). The fitted BMP085 is genuine at 0x77; the probe table's 0x3E/0x3B were
  phantoms from halving datasheet bytes. Fixed and re-measured: boot finds it and the fallback path
  runs (`BARO_RANGE_9M`, arm allowed with the permanent banner — the string is now
  `BARO_RANGE_CLAMP` quoting a computed ceiling, decision 38, see the next bullet). **Update 2026-11-20: the driver is
  written** (`BaroBosch.h` + `Bmp085.h` + `checkBaro()`, decision 37) and pinned by `test/test_baro/`;
  its **second half — the proving conversion on the part** is now closed too (bench `b`: driver
  977.75/977.78 vs library 977.76 mbar, hand recompute matching, squeeze/port stimulus moving the
  reading, T tracking — §5g). Item 17 is fully closed. Item 18 (the module's absolute pressure
  "7–10x out of spec") **opened and closed the same day — wrong-city reference**: the check used
  Cape Town weather; the bench is in Stockholm (now a named fact, §6). Against SMHI/Bromma
  (SLP 984–987 hPa) the module's 977.7 mbar is consistent with the DS absolute-accuracy window
  for the confirmed 0–100 m band, and the offset-vs-gain concern is void. Lesson kept: a
  pressure reference has a place, an hour and a source — state all three. What remains
  genuinely open on the baro: the MS5837 (0x76) is unprocured, so that half of the table is
  datasheet-derived and unmeasured here.
- **Decision 38 (2026-11-20) — read before quoting any baro range:** the record's "BMP085 fallback
  saturates ~9 m" was **10x wrong**. 1100 mbar abs − ~1013 mbar surface = ~87 mbar gauge ≈ **under
  1 m of water** (~1.2 m at this bench's learned 977.9 mbar Psurf). Found while writing `Depth.h`,
  pinned by `test/test_depth/`. The banner now prints a **computed** ceiling with its reference
  (`BARO_RANGE_CLAMP clamps 1.2m@978mbar`); old logs saying `BARO_RANGE_9M` are the same enum (13).
  Consequence for the bench: the fitted fallback is a cross-check, **not a dive depth source** —
  the Bar30 (M3's gate) is load-bearing (AGENTS.md risk 1, decision 38, §21 entry).
  **Decision 39 (same day, user-directed) made that mechanical:** three tiers — MS5837-30BA primary,
  LPS28DFW (mode 2, ~30.7 m computed gauge ceiling, NOT the marketing "40 m") armable fallback,
  BMP180 **bench-only** — and `BARO_BENCH_ONLY` (ERROR, blocks arming) now fires when the BMP is the
  sole depth source that passed identity, so the fitted bench's `[arm] REFUSED` baseline is correct
  behaviour until a dive-capable tier arrives (open item 19: order #6067; §7 step 2 has its
  acceptance chain). Tier identity: no PROM on LPS28 silicon, so the identity is the capability
  chain — WHO_AM_I 0xB4 + SWRESET self-clear + **FS_MODE latched on readback** + a real conversion;
  the ceiling comes from the latched mode, never the write request.
- **Open item 16 is closed** (§6): the `err 5` / `SDA held low` boot was the module being unplugged by
  hand, not a fault. No action. The `2`-vs-`5` reading is still worth knowing.
- **Open item 12 is closed** (decision 32): 33 = Page button, motor on 16, `test/test_pins/` asserts
  the class of defect is gone.
- Everything else in AGENTS.md §20 (valve choice, pulse volumes, metabolic rate, display panel,
  enclosure) blocks BOM/calibration but not the next code steps.

## 10. Standing rules worth repeating to a fresh session

- This is **primary dive life-support firmware**, uncertified. Prefer explicit failure, no silent
  fallbacks. Boot is never fatal; fatality applies at the arming gate and to mid-dive ADC dropout.
- The failure mode of this codebase is **plausible-looking numbers** (risk 13). It is not confined to
  the deco engine: the register-field masks, the MODE bit, the arming-gate table and the **pin table**
  all produced wrong behaviour that read sensibly (GPIO 33 assigned to two roles at once; "free" pads
  at 20/24/28–31 that do not exist on this die). Remedy that has worked: put the constants where native
  tests can pin them, anchor tests to a *measured* value, and walk whole tables rather than trusting a
  `default`.
- **Measure before you size a timing window.** Datasheet tables describe a part, not the part on the
  desk (`d` produces the measured numbers at any time). The same applies to **compute cost**: a bound
  on a loop is not a bound on its cost, and xtensa `expf` is ~3 us where x86 is ~40 ns. Bench `v`
  prints worst-tick cost; read it on the target, never assume it from a host run (risk 19).
- **A gap you plan to schedule into is only real if you measured it.** The pair walker keeps a
  converter integrating 95-96% of wall time, so the incidental inter-conversion gap is ~2-3 ms —
  nowhere near a 20-250 ms pulse (§5f, decision 34). Any scheduler that "queues into the gap" must
  come with a duty-cycle report, and where the duty says there is no gap, the schedule must *make*
  one explicitly.
- **Quote the pressure a litre is measured at.** Metabolic rate is a molar draw in surface litres
  (depth-independent); valve pulse volume is an ambient litre (depth-scaled). Conflating them keeps
  the setpoint tracking perfectly while every gas figure is wrong by the ambient factor — audit the
  **mole balance**, not the ppO2 trace (risk 20). Same for assertions: a query that had no data must
  say `NO DATA`, never answer 0.0.
- Never write to I2C devices as a side effect of probing; never `pinMode` a live bus pin.
- **Never do arithmetic on a datasheet address** (decision 36, risk 21). Take the 7-bit form the
  datasheet states — BMP085/180 is **0x77** (DS Table 7: `1110111`+R/W), MS5837 is **0x76** — and
  when a part fails an expected-identity check, **suspect the expectation first**: the "clone"
  verdict of 2026-11-20 was three of my own errors (phantom 0x3B, a BMP180 register that does not
  exist, and per-chip cal trim read as a constant) convicted a good part. Identity claims must rest
  on datasheet text checked at the source, plus a measurement that means something (a PROM read,
  a real conversion) — not on a comparison against something I half-remembered.
- **When a datasheet defers an algorithm to vendor code, the deployed code is the datasheet**
  (decision 37). Bosch's BMP180 DS says "an ANSI C code is available from Bosch" and its formula
  appendix is page images — transcribe a deployed implementation (the Adafruit BMP085 library is in
  `lib_deps` for exactly this), anchor it at a self-consistent documented example (that library's
  "use datasheet numbers!" set yields 15.0 °C / 69964 Pa exactly under the right arithmetic),
  cross-check line-by-line, and **never anchor on remembered constants**: the "7295/23843" pair I
  recalled existed in no source and was not self-consistent. Four real transcription faults lived
  in `compensate()` until the cross-check; every one of them would have produced plausible mbar.
- **Derive the numbers that gate behaviour; never recite them** (decision 38, risk 21's fourth
  instance). The record repeated "BMP085 fallback saturates ~9 m" for a day — nobody had
  multiplied it out, and the truth is **under 1 m** (1100−1013 mbar gauge ≈ 0.87 m of water).
  Writing `Depth.h` forced the multiplication and the claim died. The rule now implemented:
  ranges/limits are computed in code from primary-spec inputs (`depth::ceilingMeters()`,
  `rangeCeilingBar()`) and **printed with their reference** (`clamps 1.2m@978mbar`,
  `... SL1013mbar` pre-Psurf), so a wrong figure cannot survive by being copied.
- Over-blocking is also a failure mode: a WARN that refuses a dive at the water trains a diver to
  override the gate. Say it out loud, log it, and only block on things that truly break trust.
- **The operator's network is off-limits.** This firmware runs a SoftAP and never joins a network, so
  it cannot and must not change anyone's Wi-Fi. Web acceptance is bench `n` plus curl/WS from a
  machine already on the AP; if a real browser check is needed, **ask the user to join the AP
  manually**. (Stated by the user on 2026-11-21; do not "just test it" from the Mac.)
- **DRAM is a budget, and the link map is the only honest ledger.** `dram0_0_seg` on ESP32 classic is
  124,580 B and the WiFi/lwIP statics inside it are precompiled in framework `.a` archives — the only
  lever is our own statics (M6 measured: project 128,864 B against framework 52,696 B, over by
  57,376). So: one shared scratch per serial path (decision 40), one shared response scratch for the
  serialized HTTP callbacks, capacities that are pinned by tests so a change is deliberate, and
  `-Wl,-Map=…` kept in `platformio.ini` so the next overflow is read from the map, not guessed from
  the source. A parse of the map is itself a measurement — an earlier one undercounted library DRAM
  by matching only one of ld's two line forms.
- **Read a library's default arguments at source before trusting them** (same class as decision 36,
  one level up): `LittleFS.begin()`'s default partition label is the literal `"spiffs"`
  (`libraries/LittleFS/src/LittleFS.h`) while `partitions.csv` names this board's data partition `fs`,
  so the default call fails the mount on a board with a healthy filesystem and would have been
  written up as dead flash. Pass the label; keep `formatOnFail = false` (a silent format would erase
  the dive ring and both config copies).
- Pure C++ goes in `lib/` (host-buildable); anything touching a pin, peripheral or socket goes in
  `src/` and is not compiled natively.
- **Record what happened at the bench alongside what the board reported.** A fault code cannot tell
  "the user unplugged this" from "this failed on its own", and the firmware should not have to guess.
  When you write up an anomaly, say which parts are measured and which are inference — this session
  mislabelled a hand-unplugged module as an intermittent connection, and the correction cost a second
  pass over three documents.
