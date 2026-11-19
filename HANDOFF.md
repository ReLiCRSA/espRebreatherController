# HANDOFF.md — session transfer snapshot

**Written:** 2026-11-19, after the first ADC converter was fitted and measured on real hardware.
**Purpose:** everything needed to continue this project on a different machine. `AGENTS.md` is still
the binding design record (decisions §18, risks §19, milestones §16); this file holds the
*machine-specific and hardware-measured* state that `AGENTS.md` does not, plus the immediate
resume point. Read `AGENTS.md` first, this second.

---

## 1. Project state in one paragraph

ESP32 classic CCR rebreather dive computer. `lib/core` (deco engine, voting, gas model, boot report,
**the ADC register map, and now the GPIO pin map**) is complete and regression-anchored:
**80 native cases green** across six suites. `src/` boots on the board: safe-state pins first, bounded
I2C presence probing, **active converter checks**, honest boot fault report, arming refusal, hot
re-probe every 5 s, serial bench commands (`s p r c a d g`). **M0 is DONE. M1 is closer than it was** —
the boot/bench half is verified against *fitted* hardware, and the ADS1115 register layer is written
and write-verified on hardware. Still open at M1: the alternating-mux scheduler (per-cell rate derived
from the measured conversion period, not from the datasheet's 16 SPS), `lib/sim`, and the baro/INA219
drivers. **One ADS1115 module (0x48) and one SSD1306 (0x3C) are wired; the second converter (0x49),
the baro and the INA219 are not.**

Two decisions were taken with the user on 2026-11-19 and are now binding (AGENTS.md §18): the ppO2
converters **shall be genuine TI ADS1115s** (decision 31 — the fitted clone stays on the bench for
timing work only), and **GPIO 33 is the Page button while the vibration motor moves to GPIO 16**
(decision 32). The pin map itself moved to `lib/core/src/PinMap.h` under native test (decision 33),
because it had been wrong twice as an untestable paragraph.

The single most important hardware lesson: **the fitted "ADS1115" module is not a genuine ADS1115.**
Everything about timing in this firmware is derived from measurement, not from the datasheet. Details
in §5, consequences in AGENTS.md §2/§8/risk 16/open item 13.

**Transfer: the tree is a git repo, pushed to GitHub.** Clone on the new machine rather than copying
the directory:

```
git clone git@github.com:ReLiCRSA/espRebreatherController.git    # or the https form
cd espRebreatherController
```

`.pio/` is gitignored (build cache, ~72 MB) — the new machine rebuilds it on first `pio run`.
Branch `master`. The ADC work is commit `086c917`; the pin-map/pin-measurement work described in §5d
follows it. **Check `git log --oneline -3` rather than trusting any commit hash written here** — if §1
or §5 says something the remote does not contain, that is the gap. A fresh clone will not contain the
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
pio test -e native            # 80 cases, six suites, ~4 s, no hardware needed
pio run -e esp32              # links; 466,973 B flash (29.7%), 35,600 B RAM (10.9%)
pio run -e esp32 -t upload    # flashes; verify "Hash of data verified"
pio device monitor            # or the python capture recipe in §4
pio run -t uploadfs           # LittleFS assets, when data/ exists (web UI, not yet written)
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
- Address conventions: `Wire` takes **7-bit**. MS5837 = **0x3E**, BMP085 = **0x3B** (datasheets quote
  the 8-bit write forms 0x7C / 0x77). ADS1115 0x48/0x49, SSD1306 0x3C/0x3D, INA219 0x40 are already
  7-bit. A probe at 0x7C finds nothing forever.
- `CONFIG_LOG_DEFAULT_LEVEL` / `CONFIG_ARDUHAL_LOG_DEFAULT_LEVEL` are both **1 (ERROR)** in this
  core, so driver ERROR lines cannot be filtered below ERROR at runtime — the fix for console spam is
  bounded probing, not log suppression.
- Boot probing discipline: fitted address table only (~7 transactions/bus). The 112-address sweep
  is the `s` bench command; on a port that failed to init it emitted ~200 error lines and buried
  the banner.
- **Two addresses, one device** bites twice: baro (0x3E/0x3B) and OLED (0x3C/0x3D). Probing both and
  raising a fault from the strap that did not answer blocks arming against working hardware. Handled
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
  unreachable. **Decided 2026-11-19 (AGENTS.md decision 31): source known-genuine TI ADS1115s** and
  keep this module for bench timing work only. Outstanding action, not an open question: when new parts
  arrive, `d` must measure 8/16/32/64/128/250/475/860 SPS and `a` must read ID `0x85`, and
  `ADC_ID_UNKNOWN`/`ADC_RATE_MISMATCH` must clear by themselves. Note that **"ADS1115S" has no
  datasheet** — so identity here is always a measurement claim, never a part-name claim.
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

## 6. What is on the bench right now

- ESP32 Doit devkit V3 over USB. **Wired: one ADS1115 module on I2C1 at 0x48 (VDD 3.3, SDA 18,
  SCL 19, ADDR->GND), one SSD1306 on I2C0 at 0x3C.** Not wired: the second converter (0x49), the
  baro, the INA219, the solenoid, all buttons, buzzer, motor, watchdog. Cell inputs floating.
- Firmware flashed = current `src/` at HEAD of this session. Expected boot output with exactly the
  above fitted, verbatim:

```
CCR dive computer — boot (never fatal), entering BENCH
[i2c] I2C0 sda=21 scl=22 ok
[i2c] I2C1 sda=18 scl=19 ok
  I2C0: 1 device(s)  [0x3C]
  I2C1: 1 device(s)  [0x48]
  ADS @0x48 ID 0x00 — not the documented ADS1115 ID
  ADS @0x48 converts: 62074 us for the 32 SPS field (DS 31250 us) = 98% off
  FATAL ADS1115_U2             x1  0x49 err 2
  ERROR BARO                   x3  no MS5837/BMP085
  ERROR INA219                 x1  0x40 err 2
  WARN  ADC_ID_UNKNOWN         x1  0x48 ID 0x00 not 0x85
  WARN  ADC_RATE_MISMATCH      x1  0x48 62mS DS31mS 98%
[boot] F:1 E:2 W:2 ADS1115_U2 BARO INA219 ADC_ID_UNKNOWN ADC_RATE_MISMATCH
[arm] REFUSED — see faults above
[o2] solenoid disabled — GPIO 26 will not be driven
bench commands: s=full bus scan  p=probe table + report  r=raw probe codes (destructive, bench only)  c=bus recovery  a=ADC self-test  d=ADC rate characterisation  g=pad map + pad test
```

  Two numbers to watch on any future boot: the **conversion period** (~62 ms on this part) and
  **`98% off`**. If a genuine TI module is fitted, expect ~31 ms, `ID 0x85`, and both
  `ADC_ID_UNKNOWN` and `ADC_RATE_MISMATCH` gone from the banner — that is the test that decision 31's
  parts arrived.
  If instead something shows `ADC_CONV_BAD`, `ADC_PART_WRONG`, phantom devices, `ALL SENSORS OK` with
  nothing wired, or `bus is not initialized` spam, something regressed: start at
  `src/sensors/BusProbe.cpp`, then `checkAdcs()` in `src/main.cpp`.
  (`ADC_ID_UNKNOWN` and `ADC_RATE_MISMATCH` are WARN and **deliberately do not block arming** — see
  AGENTS.md §13. Do not "fix" them by making them fatal.)

- Bench commands over serial: `s` full 0x08..0x77 sweep · `p` probe table + fault report ·
  `r` raw four-shape diagnostic (needs `-DBENCH_RAW_PROBE=1`, **writes 0x00** to whatever ACKs — bare
  bus only) · `c` 9-clock bus recovery + port re-init · `a` ADC self-test (ID, config decode,
  write-verify + restore, DRDY line, one timed conversion) · `d` ADC rate characterisation (measured
  period for all eight RATE fields, plus the poll cost, ~6 s) · `g` pad map + pad test (~48 lines:
  per-pad capability word from `PinMap.h`, the core's periman owner, and measured pull/output behaviour
  on pads with nothing wired — the source of every claim in §5d).
  `a` and `d` write the config register, so they are bench actions, not something the dive loop runs.

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

1. **Order known-genuine TI ADS1115 modules** (AGENTS.md decision 31 — decided 2026-11-19, this is now
   an outstanding action, not a question). On arrival: `d` must measure
   8/16/32/64/128/250/475/860 SPS and `a` must read ID `0x85`, with `ADC_ID_UNKNOWN`/`ADC_RATE_MISMATCH`
   clearing by themselves (~31 ms conversion, not 62). Until then every timing figure in the firmware is
   derived from the measured table, so both parts work and neither is trusted from its label. The sample
   rate decides the control-loop deadband and the coil/ADC interlock window, so **do not write the
   scheduler around 32 SPS as though it were real.**
2. **`lib/sim`**: `SimBaro`, `SimCells`, `LoopModel`, `FaultInjector`, `ProfileRunner` — pure C++ in
   `lib/` so `pio test -e native` covers it, implementing the same `Barometer`/`CellArray` source
   interfaces as the real drivers (AGENTS.md §11: simulate at the sensor layer, closed loop against
   the firmware's own commanded pulses, stamp everything `SIM`). This needs no hardware and no
   decisions, so it can proceed right now.
3. **Alternating-mux scheduler** in `src/sensors/Ads1115Bus`: walk AIN0/1 -> AIN2/3 with
   single-shot conversions, **using the measured conversion period**, per-channel `invert`, and
   DRDY on 23/4 once it is actually wired (open item 14). Then wire cell 2 into AIN2/3 so the
   correlated-failure path (cells 1+2 share U1) can be exercised for real.
4. **`src/sensors` drivers**: `Ms5837` @0x3E, `Bmp085` @0x3B (with the 9 m clamp banner), `Ina219`
   @0x40, `Depth` with learned `Psurf`.

After that: `src/dive`, `src/ui`, `src/web`, `README.md` (the README content requirements in
AGENTS.md §15 are a user requirement, including the 7-bit-vs-8-bit address trap as a listed wiring
mistake).

## 8. Files that hold load-bearing state

| File | What it pins down |
|---|---|
| `AGENTS.md` | binding design record; §16 milestone status, §18 decisions 1–33, §19 risks 1–18, §20 open items 1–16 |
| `HANDOFF.md` (this file) | machine + bench state, measured I2C and ADC semantics, resume point |
| `lib/core/src/AdcRegs.h` | **the ADS1115 register map, PGA/RATE tables, LSB and mV scaling, and the measured bench-module rate table.** Pure C++ so it can be asserted; it was wrong twice while it lived only in `src/` |
| `src/sensors/Ads1115Bus.{h,cpp}` | register driver: single-shot start (MODE written explicitly), `checkPart()` identity-by-timing, `measureAdcRates()`, `selfTest()`. The accepted conversion window (20–150 ms) and why it is wider than the datasheet figure |
| `src/sensors/BusProbe.cpp` | the measured four-shape probe table and the periman trap, in comments; the only probe form that is truthful and non-destructive |
| `src/sensors/BusProbe.h` | probe contract, 7-bit address warning, "port must be down" rule for line checks |
| `src/main.cpp` | boot order (solenoid safe-state first), bounded probing, `checkAdcs()` (presence is not identity), two-address device selection for baro and OLED, bus dead-vs-absent reporting, 5 s hot re-probe and bus retry, bench command REPL (`s p r c a d g`), `gpioMapReport()` |
| `lib/core/src/FaultCode.h` | the fault enum plus `defaultSeverity`/`blocksArming`/`faultName`. `ADC_PART_WRONG`/`ADC_CONV_BAD` block arming; `ADC_ID_UNKNOWN`/`ADC_RATE_MISMATCH` deliberately do not |
| `lib/core/src/BootReport.h` | `blocksArming()` refuses on **any active FATAL**, not only on listed codes |
| `lib/core/src/PinMap.h` | **the GPIO capability table (one word per pad, 0..39) and the role assignment.** Pure C++, asserted natively — it was a paragraph in a header before and was wrong twice. Carries `noSuchPad` (24, 28–31: no such pad on the ESP32-D0WD-V3 die) and `kDieOnly` (20, 37, 38: die pads WROOM-32 does not bond out) as separate claims, `unusableReason()`, and `SPARE_OUT = 17` |
| `src/config/Pins.h` | **an alias of `PinMap.h`, not a second copy of the numbers.** `BTN_PAGE = 33`, `VIBRATOR_OUT = 16` (moved off 33, decision 32), `SPARE_OUT = 17`. Drivers must still check `>= 0` before `pinMode`: a role may be unassigned on purpose |
| `src/config/Features.h` | defaults: `HAVE_SOLENOID 0`, `BENCH_MODE 1`, `WIFI_DURING_DIVE 0`, `BENCH_RAW_PROBE 0`, `POWER_RAIL "1S2P"` |
| `platformio.ini` | pioarduino `stable` platform (never the stock `espressif32`), `lib_ldf_mode = deep` + `lib_extra_dirs` + explicit core libs in `lib_deps`, `native` env with `test_build_src = no` |
| `partitions.csv` | 64 KB-aligned app slots, `data, littlefs, 0x320000, 0xC0000` |
| `lib/core/src/Zhl16c.cpp` | deco engine: ZH-L16A coefficients + Subsurface GF/ceiling form, plan search in depth/grid space, `bounded` flag, GF anchor in restorable state. **Do not change without native tests and a trusted-computer cross-check** (risk 13) |
| `test/test_adc/test_adc.cpp` | config-field masks tile 0xFFFF once; `0x8583` decodes as the DS default; design config = gain 16 + 32 SPS + explicit single-shot; LSB tables; air-signal code counts; the measured non-datasheet rate table |
| `test/test_boot/test_boot.cpp` | fault severities; absent solenoid is INFO and must **not** block arming; the whole-enum table walk (gap list — keep it current when adding codes); unlisted FATAL still blocks |
| `test/test_pins/test_pins.cpp` | no two roles share a GPIO (33 was the motor *and* the Page button); no output role on an input-only pad; no role on a strapping pin; the no-such-pad / die-only / flash sets each asserted separately; 34–39 input-only with no pulls; button pads have internal pull-ups; **the spare output budget is asserted to be exactly one pad (17)**, so freeing a pin forces the AGENTS.md §3 budget to be re-derived |

## 9. Open items that block code, not just BOM

- **Open item 13 is decided** (AGENTS.md decision 31: genuine TI ADS1115s) and no longer blocks the
  scheduler — but the **parts are not yet fitted**, so the per-cell rate is still 8 SPS on the bench
  module and 16 SPS on a real one. Write the scheduler against a measured period read from the part,
  not a constant.
- **Open item 14** (DRDY never actually wired, only read pulled-up and idle): the scheduler can be
  written to poll the OS bit, but at 348–365 us a poll it is not the design in AGENTS.md §8. Wire
  DRDY before certifying the interlock.
- **Open item 15** (new): a SPI display panel needs four pads (SCK/MOSI/DC/CS) and **only GPIO 17 is
  spare and output-capable** after the current map; the core's default SPI pins 18/19/23 collide with
  I2C1 and `ADS_U1_DRDY`. Argue this with the panel choice (item 6) before buying anything — the
  options are freeing the DRDY pads (item 14's other side), taking a strapping pin against the §3
  policy, or keeping the panel on I2C and accepting the frame-time cost.
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
  desk (`d` produces the measured numbers at any time).
- Never write to I2C devices as a side effect of probing; never `pinMode` a live bus pin.
- Over-blocking is also a failure mode: a WARN that refuses a dive at the water trains a diver to
  override the gate. Say it out loud, log it, and only block on things that truly break trust.
- Pure C++ goes in `lib/` (host-buildable); anything touching a pin, peripheral or socket goes in
  `src/` and is not compiled natively.
- **Record what happened at the bench alongside what the board reported.** A fault code cannot tell
  "the user unplugged this" from "this failed on its own", and the firmware should not have to guess.
  When you write up an anomaly, say which parts are measured and which are inference — this session
  mislabelled a hand-unplugged module as an intermittent connection, and the correction cost a second
  pass over three documents.
