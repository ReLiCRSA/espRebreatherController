# AGENTS.md — ESP32 CCR Rebreather Dive Computer

**Status:** design complete, no source code written yet. Project root: `/Users/wvanghent/esp32`.
**Last updated:** 2026-11-19.

---

## 0. How to use this file

Read all of it before changing anything. Section 18 (decision log) and section 19 (risks) are
binding: re-open a logged decision only with the user's explicit agreement. When you make a
non-trivial decision, append it to 18 with rationale. When you close an item in 20, move it with
its resolution — do not delete it.

This is **primary dive life-support firmware**. It is uncertified. Prefer conservative behaviour,
explicit failure, and no silent fallbacks.

---

## 1. Summary

ESP32 (classic WROOM devkit) closed-circuit rebreather dive computer:

- 3x galvanic O2 cells read by **two ADS1115** ADCs, **3-of-3 voting** establishes ppO2.
- **MS5837** depth primary, **BMP085 fallback**; depth/pressure feeds decompression.
- **ZHL-16C, 3-gas (N2/He/O2) + gradient factors**, ICD guard, CNS/OTU, MOD / hypoxic floor /
  END / gas-density limits, NDL, TTS, ceiling, stop schedule.
- **CCR and OC modes**; the model tracks *inspired* gas state so mode and gas switches take
  effect immediately.
- **5 V O2 injection solenoid**, fully automatic pulse-driven control, coil-current click
  verification. Optional hardware — never blocks boot.
- **SSD1306 OLED** showing ppO2, depth, NDL/TTS. Layout is a **templated, web-editable** system
  so the screen can be swapped for a larger one and fields added without recompiling.
- **Web server on the ESP32** (SoftAP + WebSocket): settings, gas setup incl. bailout, deco
  config, air cell calibration, live ppO2/pressure, last-5-dive graphs, dive simulator.
- **Bench mode is the default at boot.** Nothing is fatal at boot; every missing device is
  logged and reported. Fatality applies at the **arming gate**.

---

## 2. Hardware

### Board
ESP32 classic (WROOM-32 devkit). Two cores are load-bearing: dive loop on core 0, web on core 1.

### O2 cells
3x **galvanic** cells (self-powered, ~9–13 mV in air, internal burden resistor).
- **Never apply a voltage to a cell lead.** ADS1115 inputs are the only connection.
- Annual replacement items; a spare cell is expected in the housing.
- No temperature compensation implemented (~2%/degC); MS5837 temperature is logged every sample
  so post-dive review can see excursions.
- ~10 min humidity/temperature acclimation gate before arming (not a polarographic warm-up).

### ADC — two ADS1115, true differential pairs
**Procured part: 2x Adafruit "ADS1115 16-Bit ADC - 4 Channel with PGA" (product #1085)** — decision
35, the named way decision 31's "genuine TI ADS1115" is satisfied. Board-level facts checked against
the Adafruit product page and the Adafruit 4-Channel ADC Breakouts guide (2026-11-19):
- Carries the TI **ADS1115** (Adafruit publishes full schematic/EagleCAD; chip spec is TI SBAS444,
  which is what `AdcRegs.h` implements). Two board revisions exist — current **STEMMA QT** and the
  older header-only — functionally identical; either is acceptable.
- **Supply VIN 2.0–5.5 V; run it from the 3.3 V rail** so board logic level matches the ESP32.
  **A+ / A- are the board's ferrite-filtered analog rails — outputs, not inputs**; power at VIN/GND
  and land the cell/coil networks on the **A0..A3** pads.
- **Address: default 0x48; one back-side solder jumper `A0` closed -> 0x49.** That is exactly the
  0x48/0x49 pair this design uses. 0x4A/0x4B (ADDR->SDA/SCL) exist on the chip but the board does
  not strap ADDR that way — do not plan a third or fourth converter on those addresses.
- **ALRT pin is on the header** (comparator output or conversion-ready / DRDY, open-drain,
  active-low, needs a pull-up). Open item 14 becomes a jumper + pull-up once these boards are
  fitted, with no extra hardware.
- **On-board 10k I2C pull-ups to VIN on SDA/SCL.** Two boards parallel to ~5k on I2C1 — fine;
  **do not add external pull-ups to I2C1** on top.
- Power-LED cut jumper on the back (optional for the dive build). Board 25.4 x 17.8 mm.

| Device | Addr | AIN0/AIN1 | AIN2/AIN3 | DRDY (ALRT) |
|---|---|---|---|---|
| U1 | 0x48 (#1085 as shipped: `A0` jumper open) | cell 1 | cell 2 | GPIO 23 |
| U2 | 0x49 (`A0` jumper **closed**) | cell 3 | coil shunt (differential) | GPIO 4 |

- Both on **I2C1** (SDA 18 / SCL 19). I2C0 carries OLED + baro + INA219 only.
- **Gain 16 (+/-256 mV)**. Continuous mode locks the mux to one channel, so the driver must
  **alternate single-shot conversions** AIN0/1 -> AIN2/3 at **SPS 32**, giving 16 SPS per cell.
  SPS 32 is what the design asks for; what the fitted part actually delivers at that field is
  measured, not assumed (see the measured rate table below — the bench module gives 16.1 SPS there,
  so the real per-cell rate is 8 SPS until the part or the field is changed). The implemented
  alternating-mux walker achieves **7.2 SPS per cell measured** on the fitted clones (bench `t`):
  8 SPS sized, less the OS-poll detection lag — in-walk conversion periods measure 65.5-67.1 ms
  against the 61.9 ms timed at boot. **Decision 31: the part gets replaced** with a known-genuine
  TI ADS1115 (open item 13 is decided; decision 35 names the Adafruit #1085 breakout as the
  procured part); the fitted module stays on the bench for timing work and never goes near a dive.
  On the genuine chip the design's 32 SPS field is 31.25 ms — so expect ~16 SPS per cell from the
  walker and a ~31-33 ms interlock window — but those stay **measurement-derived expectations, not
  sizing inputs**: the walker sizes from each part's own boot measurement either way (decision 29).
- **LSB 7.8125 uV at gain 16** (FSR/32768 — not the 0.125 mV often quoted, which is the *gain 1*
  figure). A galvanic cell in air (~9-13 mV) is therefore ~1150-1660 counts of a +/-32767 range:
  ample headroom for the bias network, ample resolution. `lib/core/src/AdcRegs.h` holds the register
  map, the PGA/RATE tables and this arithmetic; `test/test_adc/` asserts them.
- Differential cells float: **1 MOhm from each cell lead to GND/AIN_COM** to define common mode,
  plus 10 ohm series and an RC low-pass per pair. Without the resistors the inputs rail and read
  garbage.
- O2 is reduced at the sensing electrode, so signal polarity may be negative. Per-channel
  `invert` flag resolved on the bench.
- **Measured on the first module on the bench (2026-11-19): it is not a genuine ADS1115.** It ACKs
  at 0x48, its config register reads/writes correctly (power-on `cfg=0x8583`, exactly the datasheet
  default), it converts — but the ID register answers 0x00, the pointer register answers 0x00 and
  does not track writes, and its RATE field implements only **four** rates selected by bits 6:5:
  measured 8.1/8.1/16.1/16.1/31.9/31.9/63.7/63.7 SPS for fields 0..7 (reproduced across two runs,
  each field pairing to the same period within 0.5%), against the datasheet's
  8/16/32/64/128/250/475/860. So the design's "32 SPS" field delivers 16.1 SPS (8 SPS per cell
  pair-cycle), and 860 SPS is unreachable. Consequences are in section 8 (interlock window) and
  risk 16; `GET`/bench command `d` re-measures the table.
- Cells 1 and 2 share one converter (one mux/reference/supply) — a known correlated-failure
  point, compensated by the diluent-flush check and the arming gate, and documented as a reason
  to carry a second independent computer.

### Depth
- **MS5837** (7-bit **0x3E**; the datasheet's "0x7C" is the 8-bit write form) primary: 0–30 bar
  (~300 m), 0.2 mbar resolution, OSR 4096 (~10 ms) at 8 Hz.
  Gel-covered: max 24 h continuous immersion then 2 h drying — long immersion destroys it.
- **BMP085** (7-bit **0x3B**; datasheet "0x77" is the 8-bit write form) fallback only:
  300–1100 hPa => **~9 m usable depth ceiling**. If it is what is
  fitted, arm is allowed but a permanent `DEPTH RANGE 9m` banner shows and all depth-dependent
  logic clamps at 9 m. Never extrapolate.
- Boot probe order: 0x3E then 0x3B. Selected driver reported in `/api/health` and on OLED boot line.
- **Measured on the bench (2026-11-20, bench `b`): the fitted "BMP085" module is not a BMP085.**
  It ACKs at 7-bit **0x77** — a genuine Bosch BMP085/180 has its 7-bit address **fixed at 0x3B**, and
  is silent there — answers CHIPID `0xD0=0x55` (the BMP180 ID), but the fixed version byte answers
  **`0xD1=0x02` where a genuine part reads `0x43`**, and the factory calibration ROM reads
  **AC1=0x219C (8604) against the BMP180 spec value 7295**. A Bosch-family clone: same defect class
  as risk 16, now on the depth sensor. It has also been seen on **I2C1** (the ADC bus) across bench
  captures — the design fits baro on I2C0. It is not trusted for depth and stays a bench object;
  fallback procurement is open item 17. `b` re-measures the fingerprint (it writes the Bosch pointer
  register — bench only, same class as `a`/`d`).
- Depth = `(Pamb - Psurf) * k`; `Psurf` is **learned at boot** (only relative accuracy matters,
  +/-2 mbar ~ 2 cm, not the +/-200 mbar absolute spec). Salinity configurable (fresh 1.000,
  salt 1.025). Learned Psurf also gates air calibration.

### Solenoid / power
- Valve **not yet purchased**. Default assumption: pilot-operated **normally closed** valve at
  ~8–10 bar O2 intermediate pressure, seat rated **>=12 bar differential**, per-pulse volume
  **0.05–0.3 ambient L**, opening <15 ms, >=120 pulses/min, 25% duty max, **O2-service rated and
  O2-clean**, PTFE diaphragm/seat preferred over NBR/Viton.
- **Power: 1S2P LiPo** (two 3.7 V cells in parallel, 3.7–4.2 V). Coil driven **directly from the
  battery rail — no boost stage**.
- Drive: low-side logic-level MOSFET (AO3400/AO22A4 class), 100R gate resistor, **10k gate
  pulldown** (fails closed on reset and watchdog bite), flyback Schottky to **V_BAT**,
  **10 mOhm shunt** in coil return read differentially on U2.
- **No diode in the coil path** (0.3 V loss at 3.4 V is a 9% coil-voltage loss). Or-ing Schottky +
  470 uF low-ESR bulk on the 3.3 V / analog branch only — a coil kick must not modulate analog
  ground or the ADC reference, because we are measuring 10 mV signals.
- Sense: V_BAT, 3.3 V rail (the real brown-out trip point), coil shunt current. **INA219** (0x40)
  on I2C0 for battery V/I.
- Energy reality: ~0.3 J/pulse, ~30 mW average vs ~0.4 W electronics. Capacity is never the
  constraint; voltage headroom and transient droop are. 1S2P ~6000 mAh = ~50 h with WiFi off.
  Pick cells for **pulse current >=1 A and firm 3.4 V cutoff under 400 mA**, not capacity.
  Single 2P pack with one PCM rated >=3 A — do not parallel two packs through diodes.

### Display
Current target SSD1306 128x64 I2C (0x3C, probe 0x3D). See section 13 — the layout is templated so
the panel can be swapped (128x32, 256x64 SSD1322, 128x128 colour, ST7789 TFT). Larger panels should
use **SPI**, keeping I2C0 free for baro/INA219.

---

## 3. Pin map (`lib/core/src/PinMap.h`, aliased by `src/config/Pins.h`)

This table is **asserted by `test/test_pins/`** (decision 33). It used to be a paragraph in a header
nobody could test, and it was wrong twice — GPIO 33 was assigned to both the vibration motor and the
Page button, and whole groups of pads were either written off or assumed free based on the die rather
than on this module.

| Function | GPIO |
|---|---|
| I2C0 SDA / SCL (OLED 0x3C + baro 0x3E/0x3B + INA219 0x40, all 7-bit) | 21 / 22 |
| I2C1 SDA / SCL (2x ADS1115) | 18 / 19 |
| ADS1115 U1 / U2 DRDY | 23 / 4 |
| Solenoid drive (10k pulldown + flyback) | 26 |
| Arm buttons A / B (two-button sequence; B is "Select") | 27 / 32 |
| Manual inject (hardware bypass) | 13 |
| Page button | 33 |
| Buzzer (>=90 dB) | 25 |
| Vibration motor | **16** — moved off 33 (decision 32); drive it through a transistor with a gate pulldown |
| External watchdog heartbeat out | 14 |
| Declared spare (see the budget note below) | 17 |
| Battery sense | INA219 (I2C0) |

Measured on this board with bench command `g` (2026-11-19) rather than assumed:

* **The console is UART0 on GPIO 1 (TX) / 3 (RX)**: periman reports both pads as owned by UART0, and
  the boot log and bench RX both work there. The old note "avoid 16/17 (UART0 on most devkits)" is
  **false for this board** — 16 and 17 are real, free, output-capable pads and their internal
  pull-up/pull-down work (measured: pull-up reads 1, pull-down reads 0, driven level retained when
  left high-Z). On a WROVER module 16/17 are PSRAM CS/clock; this build is WROOM-32.
* **GPIO 24 and 28–31 do not exist on the ESP32-D0WD-V3 die** (the core answers `Invalid pin`), and
  **20, 37, 38** are die pads that the WROOM-32 module does not bond out. Any spare list computed by
  counting 0..39 is a phantom, and chasing one costs an evening.
* **34–39 are input-only and have no internal pull-up or pull-down** (the driver answers
  `GPIO number error`). A switch there needs an external resistor — a BOM item, not a detail.
* After the map above, **exactly one spare output-capable pad remains: GPIO 17.** The test asserts
  that count, so freeing a pin (dropping DRDY interrupts for OS-bit polling, open item 14) is a
  deliberate change with the budget re-derived, not an accident discovered at the water. **A SPI
  display (open items 6 and 15) needs four pads and cannot be added without moving something** — that
  argument belongs here, in advance, not in a bring-up panic.
* Still avoided: 0/2/5/12/15 (boot strapping: a load or a held button changes how the unit starts —
  the test asserts no role sits on one), 6–11 (inside the module's SPI flash path), 1/3 (console).
* ADC2 pads cannot be used for **analog** reads while WiFi is active — hence INA219 rather than the
  ESP32 ADC. Digital I/O on an ADC2 pad (4/13/25/26/27) is unaffected.
* 32/33 are also the 32.768 kHz crystal pads: using them for buttons precludes adding that crystal later.

---

## 4. Firmware architecture

```
cells -> ADS1115 x2 -> CellArray (mV->ppO2, cal, VOTE) -> ppO2_used
MS5837/BMP085 -> Barometer -> Depth
        |
        v  DiveTask (core 0, priority 24 = above WiFi at 23, 10 ms tick, Task WDT)
           ZHL-16C + GF | CNS/OTU | alarms | CcrControl | DiveLog | TissuePersist
        |  snapshot queue (xQueueOverwrite)      ^ config queue (xQueue)
        v                                        |
     WebServer (core 1, async) -------------------+
```

Hard rules:
- **Dive loop on core 0, priority 24** — must outrank the WiFi stack. Web server on core 1.
- Web sees only an **immutable `LiveState` snapshot** via `xQueueOverwrite` (lock-free, single
  producer/consumer). Config edits arrive on a queue and are applied by the dive task at a safe
  point. **Never mutate dive state from the web thread.**
- Task watchdog attached to the dive task; plus an **external hardware watchdog** (TPS3840 /
  MAX6369 class) fed by a GPIO heartbeat on 14 — it resets the MCU and holds the solenoid gate low.
  Software-only watchdogs are not sufficient for primary life support.
- Brown-out detector enabled, 3.0 V floor: log, alarm, flush tissue state, then shut down.
- `ClockSource` abstraction: real vs simulated. Engine integrates at a **fixed 1 s step**
  (sub-stepped when needed) so time-compressed runs are numerically identical.
- Every sample carries `source: REAL | SIM`. In `ARMED_DIVE`, SIM sources are refused and the OLED
  prints `SIM DATA REJECTED`. Bench output always shows a `SIM` marker.
- `Barometer`, `CellArray`, `DisplayDriver` are interfaces; drivers and simulators are swappable.

### State machine
```
BENCH (default) --arm checklist pass--> ARMED_DIVE --> SURFACE_INTERVAL --> BENCH
      |                                     |
      +-- SIM allowed, solenoid inert       +-- any FATAL --> FAULT_LOCKOUT --> BENCH
```
Arming requires a **two-button physical sequence**. The web page cannot arm the unit.

### Boot fault report (boot is never fatal)
Probed at boot (7-bit addresses, which is what `Wire` takes — the baro datasheets quote 8-bit
forms): ADS1115 @0x48, @0x49, MS5837 @**0x3E**, BMP085 @**0x3B**, SSD1306 @0x3C/0x3D, INA219 @0x40,
cell input bias sanity (each pair within +/-250 mV and not railed), LittleFS mount, NVS, config
schema version, tissue-state recovery validity, V_BAT and 3.3 V rail.

Boot probes the **fitted address table only** (~7 transactions per bus). The full 0x08..0x77 sweep
is an explicit bench command: measured on hardware, a sweep is 112 transactions and on a port that
failed to init it emitted ~200 driver error lines that buried the boot banner. A console that must
be read at the water cannot start by shouting two hundred lines. Stuck-SDA detection runs **before
`Wire.begin()`** (checking it while the port is live deinits the port — risk 15); if a port will not
init, that is reported as a bus fault and its devices are reported as "bus dead", never as absent.

**Presence is not identity.** Every converter that ACKs also gets an active check (`checkAdcs()` at
boot, and again on a presence transition — not on every 5 s re-probe, because it writes the config
register): one single-shot conversion at the design rate, timed. That is what tells an ADS1115 from
an ADS1015 (same address, same register map, same field positions, rate tables 100x apart), and the
ID register cannot do the job — the bench module answers 0x00 there. The outcomes are four codes:
`ADC_PART_WRONG` (far too fast: another part's rate table), `ADC_CONV_BAD` (no conversion, or
outside the accepted 20-150 ms window), `ADC_ID_UNKNOWN` (WARN: ID register is not the ADS1115's)
and `ADC_RATE_MISMATCH` (WARN: measured period is >25% from the datasheet's for the field written).
The two WARNs are what the bench module raises today; they say out loud that the fitted part is not
what it is marked, without refusing a dive for it at the water.

Each result: `code`, `severity (FATAL/ERROR/WARN/INFO)`, `detail`, `timestamp`, `attempts`.
Destinations: serial, `GET /faults` (web System tab), **`faults.json` ring on LittleFS (last 100,
survives reboot)**, OLED one-line summary. `GET /api/health` lists the active set and which block
arming. Absent devices are **re-probed every 5 s** so hot-plugging reports `INFO: <dev> ONLINE`
instead of needing a reboot; a bus that failed to init is retried on the same cadence. Active
faults are written into dive-log headers too.

---

## 5. Cell sensing and voting

- `ppO2_i = (mV_i - offset_i) / slope_i`, per cell, from stored calibration.
- **Vote:** median of 3 is the reference. A cell passes if within `max(0.05, 20%)` of the median.
  `ppO2_used` = mean of passing cells. **0 pass -> use the lowest** + `VOTING_FAILED`.
  **1 pass -> use it** + `SENSOR_DRIFT`. In deco / at a stop, force the **lowest** passing cell.
- Cell health: raw mV outside -50…1000 mV -> cell fault. Air-cal slope **<30 mV/bar -> aged /
  current-limited cell**, blocks arming. Warm-up/stability gate: sigma < 1 mV over 10 s before
  injection is enabled.
- **Diluent flush verification:** at depth after a diluent flush, all three cells must converge on
  the diluent's expected ppO2 (`FO2 * (Pamb - pH2O)`). Pass/fail displayed and logged. This is an
  *independent* check that voting cannot fake, and the most useful safety feature this hardware
  offers.

---

## 6. Calibration

- Required flow (user requirement): **air calibration at 0.21 ppO2, 1 bar ambient**.
  `POST /api/cal/air` -> refuse unless the baro is present and ambient is **900–1100 mbar** ->
  60 s countdown sampling median mV per cell, requiring sigma < 1 mV -> `slope_i = mV_i / 0.21`;
  reject any cell with an out-of-range slope or >20% disagreement; persist to LittleFS; return a
  per-cell pass/fail report.
- Optional (off by default): pure-O2 (**0.98**) high-point check. Records high mV, computes
  divergence from expected, flags linearity problems. A single 0.21 point sets scale but proves
  nothing about slope linearity up at 1.3–1.6 bar, and the air signal is only ~10 mV — hence the
  optional check and the on-page warning text.
- Cal age is tracked; **cal age > 24 h blocks arming.**

---

## 7. Decompression engine — ZHL-16C, 3-gas, gradient factors

- 16 compartments, **N2 + He per compartment**; O2 exposure is tracked separately (CNS/OTU), not as
  a tissue compartment. Coefficient set is **ZH-L16A as used by Subsurface** (`core/deco.cpp`), in
  `bar`/dimensionless form, as `static const float` tables: N2 `a` 1.1696 … 0.2327, `b` 0.5578 …
  0.9653, half-times 5 … 635 min. **He half-times are NOT the N2 half-times** — in this
  parameterisation they are 1.88, 3.02, 4.72, 6.99, 10.21, 14.48, 20.53, 29.11, 41.20, 55.19,
  70.69, 90.34, 115.29, 147.42, 188.24, 240.03 min, with `a` 1.6189 … 0.5119 and `b` 0.4770 …
  0.9267. (The earlier design note claiming identical He half-times and the 1995 ZH-L16C pairing was
  wrong; a 50 m / 20 min air dive produced *no* decompression obligation. Corrected 2026-11-19,
  decision 20.)
- Inspired loading: OC -> `(Pamb - 0.0627) * F_gas`. CCR -> `pO2 = setpoint`,
  `pN2 = Pamb - pO2 - pCO2 - 0.0627`, `pHe = FHe_diluent * (Pamb - pO2 - pCO2 - 0.0627)`.
  `pCO2` configurable, default 0.053 bar, CCR only.
- **Gradient factors, exact implemented form** (matches Subsurface `tissue_tolerance_calc` /
  `get_gf`, which is field-deployed; my first closed form did not and is superseded):
  - per compartment, `a` and `b` are **blended by inert-gas saturation share**
    (`(aN2*pn2 + aHe*phe)/(pn2+phe)`), *not* evaluated as separate per-gas ceilings;
  - `lowestCeiling_i(gf) = (b*p - gf*a*b) / ((1-b)*gf + b)`, `p` = compartment inert tension in bar;
  - `gfLowPressure` = running dive maximum of `lowestCeiling_i(gfLow)` — the GF-low **anchor**;
  - `GF(P) = max(GF_low, (P - Psurf)/(anchor - Psurf)*(GF_low - GF_high) + GF_high)` — **GF_high at
    the surface, GF_low from the anchor downward**;
  - the ceiling is where a compartment's tolerance line **crosses the GF ramp**; a compartment with
    no crossing imposes nothing. Max over the 16 blended compartments is the ceiling, and the
    compartment index is reported on the OLED and in the log — with helium it changes mid-dive.
- **ICD guard, on by default:** after a switch that raises inspired helium, fast-tissue N2 may not
  off-gas faster than **80%** of He onloading. Applies to fast compartments only. Configurable off.
  This rule is ours, not Subsurface's, and is labelled as a heuristic in the code.
- Recomputed every 1 s: NDL (0 once GF ceiling >= current depth), ceiling depth, TTS, stop
  schedule in 3 m increments + configurable last stop (3/6 m), deco volume, optimal-ascent limiter
  (default 8 m/min), CNS%, OTU.
- **Stop-schedule semantics (plan search):** the plan walks the whole ascent on the stop grid, so
  TTS is planner-style (all future stops), not a current-ceiling estimate. Stop decisions are made
  in **depth/grid space with one 5 cm tolerance** — comparing pressures with per-call millibar
  epsilons stalled the search at a rung boundary. The schedule **ends when the ceiling rises above
  the stop floor (`lastStopM`)**: an obligation shallower than the last stop is not a stop that can
  be made, and holding at the last stop until the model is fully satisfied while still breathing at
  that depth reloads the intermediate compartments and does not converge (measured 67 min at 6 m
  for a 50 m / 20 min air dive, against ~10 min on a GF 30/70 computer). The diver's `lastStopM`
  *is* the choice of how much residual tolerance is left unpaid. `out.ceilingM` always carries the
  raw model value, so the grid can never hide the real ceiling (section 19 risk 10). `DecoPlan`
  carries a `bounded` flag: when the search did not converge the schedule and TTS must be treated
  as untrusted and shown as such — never silently published.
- **The GF anchor is dive state, not derived state:** `gfLowPressure` must be persisted and restored
  with the compartments (`Engine::setGfLowPressureBar`). Restoring tissues without it re-bases the
  GF ramp and the reported ceiling jumps to zero across a power loss — this was a real defect found
  by test, and it is the same failure mode as section 19 risk 8.
- **Per-gas limits, enforced not just displayed:** hyperoxic MOD, **hypoxic floor** (ppO2 0.16–0.18
  minimum MOD), **END** with configurable He counter-narcosis factor (default 0.6), and **gas
  density at depth** vs a configurable HPNS limit (~6 g/L). Selecting a gas that fails any limit is
  refused with an alarm.
- **CNS:** accepted single-exposure limits table (1.6 -> 240 min … 0.6 -> 1800 min), Dooley-style
  per-minute accumulation, washout half-time configurable (default 125 min). Marked in README as
  "verify before diving".
- **Tissue persistence:** compartments + CNS + OTU + surface-interval timer to LittleFS every 30 s
  and on clean power-down; replayed on boot if the gap is < 40 h. Without this, repeated dives
  silently reset the model.

---

## 8. CCR control and the solenoid

- **Breathing-state machine** — in a CCR the inspired gas is a state, not a mix. Resolve inspired
  gas from: `CCR_CLOSED`, `CCR_DILUENT_FLUSH`, `CCR_O2_FLUSH`, `MANUAL_BAILOUT`, `OC_OPEN`,
  plus switch events. Each transition is logged with timestamp and depth and changes the tissue
  loading term immediately. This is what makes CCR<->OC switching and diluent changes "count".
- **Setpoint:** dual setpoint (`sp_deep` / `sp_shallow` with crossover depth, e.g. 0.7 above 21 m,
  1.3 below). OC mode instead has a target ppO2 plus the MOD guard.
- **Drive law is pulses, not PWM.** Inductive CCR valves do not meter gas by duty cycle; they need
  full voltage to open repeatably against a seat. So: **fixed-width voltage pulses (20–250 ms) at
  a variable rate**, configurable min/max width, max pulses/min. Injected O2 is tracked as
  pulses x a per-valve calibration constant.
- **Battery-voltage compensation:** `width_used = width_table * V_nom / V_bat_measured`, clamped.
  A valve that opens in 60 ms at 4.1 V may need 120 ms at 3.4 V. Without this, injection drifts as
  the pack empties.
- **Click verification from the shunt is the primary safety net**, not the compensation. Three
  distinct faults, three distinct diver responses:
  - `SOLENOID_NO_CLICK` — commanded pulse, no current signature (coil open, FET dead)
  - `SOLENOID_SLOW_CLICK` — damped/slow signature (fouling, low voltage, failing valve)
  - `SOLENOID_FAIL` — click present but ppO2 unchanged (supply empty, frozen regulator)
- **Pulse scheduling against the ADC:** never fire the coil while an ADS1115 conversion is
  integrating. The window is **the measured conversion period, never the datasheet's**: on the fitted
  module the design's 32 SPS field measures 62 ms, not 31 ms (section 2, risk 16), so a window coded
  from the datasheet would leave 31 ms of a live conversion unguarded. The dive task owns the DRDY
  semaphore, so pulses queue into the gap. Costs a few ms of injection latency, removes a class of
  correlated ppO2 noise — dangerous specifically because noise looks like cell disagreement and trips
  voting.
- **Measured 2026-11-19 (bench `t`), and it changes M5's job:** the alternating-mux walker keeps a
  converter integrating **95-96% of wall time** at the fitted clones' rate (mean in-walk conversion
  period 66.4 ms), so the incidental gap between conversions is only ~2-3 ms — nowhere near enough
  for a 20-250 ms pulse. "Pulses queue into the gap" only works if a gap exists: the pulse scheduler
  must **carve explicit conversion-free slots** into the walker's cycle (decision 34), not rely on
  incidental ones. `t` prints busy duty, achieved per-cell rate and the interlock window on demand.
- **Low-voltage lockout:** below ~3.4 V stop injecting automatically, alarm, hand control to manual
  bailout rather than chattering a valve on a dying rail.
- If a latching valve is chosen: **pulse-then-release in both directions, never hold current** —
  a latching valve held energised is a stuck-open O2 flood, the worst failure mode this device has.
- If no solenoid is fitted/disabled: the loop still computes and displays a simulated duty
  (`would inject n%`), the GPIO is never touched. `solenoid_enabled` defaults **false**.
- Web "solenoid test" = N pulses at width W, bounded (<=2 s), requires `solenoid_enable` plus a dry-
  test acknowledgement, auto-disables.
- **Manual inject is a hardware path** (GPIO 13 / mechanical override) that works with firmware dead,
  and there must be a **manual gas bypass no firmware can inhibit**.

---

## 9. Web server

- **SoftAP** `DiveCCR-xxxx` (STA join optional), mDNS `http://diveccr.local`. **WiFi off during the
  dive by default** — the page is a pre-/post-dive tool, the OLED is the in-water display.
- **No CDN.** The phone has no internet here: HTML/CSS/JS served from LittleFS, graphs drawn by a
  ~120-line self-contained canvas plotter.
- Stack: `esp32async/ESPAsyncWebServer@^3.12.1` + `esp32async/AsyncTCP@^3.4.0` (the maintained
  fork; `mathieucarbou/*` is archived). WebSocket push at ~2 Hz.
- Routes: `GET /api/state|config|health|faults|dives|dives/{id}`, `POST /api/config|mode|gas|time|
  factory-reset|cal/air|solenoid/test`, `POST /api/sim/load|run|pause|reset|verify`,
  `GET /api/dives/{id}.csv`, `DELETE /api/dives/{id}`, `GET|PUT /api/display/templates...`, `WS /ws`.
- Tabs: **Live** (3 cells mV+ppO2 + voting pass/fail, median, depth, ambient bar, NDL/TTS/ceiling,
  CNS/OTU, setpoint, pulse counter, V_BAT, 3.3 V rail, battery) · **Gases** (diluent, O2/setpoint,
  escape, up to 6 staged bottom/decay gases, each `FO2 + FHe` with argon balance implied; shows and
  enforces hyperoxic MOD, hypoxic floor, END, density; radio-select the gas actually breathed) ·
  **Deco** (GF low/high, interpolation mode, last stop, ascent rates, ppO2 alarm bounds, CNS
  ceiling, pCO2, salinity, altitude) · **Calibration** (air cal flow + optional O2 check + per-cell
  slope history) · **Dive log** · **Simulator** · **Display** (template editor + preview) ·
  **System** (sensor presence, fault history, solenoid enable, WiFi-during-dive, firmware build).
- Config writes are **server-side range-validated** (FO2 0.10–1.00, FO2+FHe <= 1.0, setpoint
  0.4–1.6, GF_low <= GF_high, GF_low >= 20, ppO2_hi <= 1.7, last stop >= 3 m, He only above a
  configurable switch depth), rejected with 400 + field errors, written **atomically (tmp+rename)**
  with a schema version and migration, and **double-written to a LittleFS + NVS mirror** so a
  corrupt filesystem cannot brick arming.

---

## 10. Dive log

- Auto-start: ppO2 valid + depth >= 1 m for 10 s. Auto-end: <= 0.5 m for 120 s, or power-off flush.
- **LittleFS ring of exactly 5 dives** (`dives/NNN.json` + index), oldest evicted. Sim runs land in
  the same ring tagged `SIM`.
- Header: start time, mode, all gas mixes, GFs, cal slopes per cell, max depth, min/max ppO2 per
  cell, deco time, CNS, OTU, battery curve, active boot faults.
- **Samples every 60 s** (user requirement): `t, depth, amb_bar, ppO2_used, cell1_mV, cell2_mV,
  cell3_mV, ndl, ceiling, tts, cns, otu, ppHe, fHe, controlling_compartment, END, mode, gas,
  pulses, V_BAT, coil_shunt_peak, temp_C, source`. ~15 KB for 5 dives.
- Events interleaved as graph markers: gas switch, mode change, diluent flush, alarms, faults.
- Separate 30-min **1 Hz RAM ring** for the live graph only.
- Per-dive CSV export.

---

## 11. Bench mode and the dive simulator

- **Boot lands in `BENCH` by default.** Works fully with zero sensors attached. Bench mode: solenoid
  GPIO inert unless explicitly commanded by a bounded bench test.
- **Simulate at the ADC/sensor layer, not the answer layer.** `SimBaro` and `SimCells` implement the
  same `Barometer` / `CellSource` source interfaces as the real drivers (declared once in
  `lib/core/src/SensorIf.h`, so the sim and the real drivers are interchangeable by construction), so
  ADS1115 mux scheduling, cell offset/slope calibration, voting, bias sanity checks, RNS depth
  smoothing, surface-pressure learning and diluent-flush verification all stay in the loop. A
  simulator that injects ppO2 and depth directly would test the deco maths and nothing else.
  **As built (2026-11-19):** `lib/sim/` = `SimBaro`, `SimCells` (the cell fault injector lives inside
  it, because a fault has to be injected before the mV come out), `LoopModel`, `ValveModel`,
  `CcrControlLaw`, `SimRunner`. Bench `v` runs it headless on the board through the real
  voter/gas/ZHL stack. Not yet built: the web Simulator panel and `POST /api/sim/verify` assertions,
  which come with M6.
- **Closed loop model, not a scripted ppO2:** the sim holds `metabolic O2 rate` (configurable,
  ~1.2 **surface** L/min — a molar draw, depth-independent; see risk 20 — and workload-dependent)
  and `loop volume` (~8 L) and integrates ppO2 from **the firmware's own commanded pulses**. If
  pulse widths are too short, ppO2 sags and the low-ppO2 alarm fires; if the deadband is wrong,
  you see the oscillation. This is how a bad control law is caught before water.
- **Fault injectors:** per cell — stuck value, slow drift, dead cell (0 mV), step change, added
  noise, t90 lag, **current-limited cell** (correct in air, progressively low above 1.0 bar; the
  classic failure air calibration cannot detect, and it only appears at depth). Solenoid — no click,
  damped click, click-but-ppO2-unchanged.
- **Mixed sources** (`REAL | SIM | MIXED`): e.g. real galvanic cells + simulated depth. There is no
  pressure chamber, so the sim supplies ambient pressure while real cells sense — this exercises
  setpoint crossover, MOD, hypoxic floor, diluent flush at depth and the ICD guard on a diluent
  switch with real analog hardware.
- **Web Simulator panel:** segment queue of **depth (m), duration (min), setpoint (bar)** plus
  per-segment gas/mode (CCR closed / diluent flush / O2 flush / OC / bailout), ascent-descent rates,
  water temp, workload. Controls: run · pause · step · jump-to-segment · reset. Speed **1x / 10x /
  60x** — 60x turns an 80-minute trimix profile into an 80-second test. Live dual-axis plot of sim
  depth + ppO2 with the **engine's real outputs overlaid** (NDL, TTS, GF ceiling, CNS, OTU, stop
  schedule, controlling compartment) and every alarm/gas switch as a marker. Per-cell live table
  showing mV and voting pass/fail. Manual-inject button, pulse counter.
- **`POST /api/sim/verify`** runs a profile unattended against assertions ("TTS at 40 m must be
  18–22 min", "current-limited cell 2 must raise `CELL_AGED` above 24 m") and returns a PASS/FAIL
  report — a regression suite runnable after every code change.
- Limits to keep honest: at 60x you cannot test timing-dependent behaviour (real-elapsed ascent-rate
  limiting, warm-up gates, watchdog) — those stay on the 1x path. `metabolic rate` and `loop volume`
  are guesses until measured, so sim ppO2 dynamics are indicative for control-law tuning, not a
  substitute for a wet test with real cells at depth.

---

## 12. Display: driver abstraction + templated layout

The panel may be swapped later, so layout must be data, not code.

### `DisplayDriver` interface
`init() / present(fb) / size() / setContrast() / pixelShift() / power()`. Implementations:
`Ssd1306I2C` (128x64, 128x32, thingpulse), `Ssd1322Spi` (256x64), `St7789Spi` (colour TFT),
`NullDriver`, and `SimDisplay` (renders to text/JSON for browser preview). **Larger panels use
SPI**, keeping I2C0 free for baro + INA219. Frame cost matters: a 128x64 I2C frame is 1024 B,
~25 ms at 400 kHz; a 256x64 frame is 2 KB, ~50 ms — so the renderer runs on core 1 at **5 Hz**, and
any panel above 128x64 should not be put on I2C.

### `FieldRegistry` — one source of truth
Each field is registered once with `name, type, unit, format, priority-class, getter(LiveState)`.
The **same registry** feeds the WebSocket JSON, the CSV/log columns, and the display templates, and
is exposed as `GET /api/fields` (name, type, unit, current sample value). Adding a field means one
registration: it then appears on the web Live tab, in the dive log, and is available to templates.
Available field classes (extend freely): `ppo2_used`, `ppo2_c1/c2/c3_mV/bar`, `voting_pass`,
`depth_m`, `amb_bar`, `ndl`, `tts`, `ceiling_m`, `last_stop_m`, `stop_schedule`, `gf_now`,
`controlling_compartment`, `cns_pct`, `otu`, `mod_m`, `hypoxic_mod_m`, `end_m`, `gas_density`,
`mode`, `gas`, `fo2`, `fhe`, `ppn2`, `pphe`, `setpoint`, `pulses_per_min`, `injected_l`,
`solenoid_state`, `v_bat`, `v_3v3`, `battery_pct`, `temp_c`, `surface_interval`, `dive_time`,
`tts_stops`, `fault_banner`, `sim_flag`, `cal_age_h`, `asc_rate`, `best_mix`, `dgu`.

### Template format
Line-oriented text, stored on LittleFS at `/display/templates/<name>.txt`, edited and saved from the
web **Display** tab, versioned and validated server-side before activation.

```
@size 128x64 @font 8 @page 1
@reserved alarm                  # cannot be removed by any template
big  {{ppo2_used:2.2f}}  {{depth_m:0.1f}}m
row  NDL {{ndl:d}}  TTS {{tts:d}}  CEIL {{ceiling_m:0.0f}}
row  SET {{setpoint:2.2f}}  CNS {{cns_pct:d}}%  {{gas}} MOD {{mod_m:d}}
row  V_BAT {{v_bat:2.2f}}  {{fault_banner}}
@if sim   SIM {{sim_flag}}
```
Primitives: text row, right/left aligned value, `bar` gauge, glyph/alarm icon, blink, page
breaks, `@if` conditions. Format spec is printf-style.

### Fit and priority
Every field has a priority class: `SAFETY`, `PRIMARY`, `SECONDARY`, `DETAIL`. The renderer lays the
template out for the target panel; if it does not fit it drops the lowest-priority cells and emits a
**fit report** back to the web editor ("template needs 9 lines, panel has 8 — dropped: fhe, end_m,
gas_density"). This is what makes "will this layout survive a bigger screen?" a question the tool
answers instead of a surprise at the water.

### Safety invariants (validator-enforced, not advisory)
- The alarm/ppO2 banner region is **reserved**. A template may restyle `SAFETY`-class fields but may
  **never omit or hide** them; a template that does is rejected at save time.
- A template that throws or exceeds 50 ms render time is abandoned; the last good frame is kept and
  the fault is reported. The display task has its own watchdog and **cannot** affect the dive task.
- The display is a presentation-only consumer of the `LiveState` snapshot. Screen failure never
  suppresses audible or haptic alarms.

### Bundled templates
`default_128x64`, `compact_128x32`, `wide_256x64`, `color_128x128`, `bench_128x64` (shows `SIM`
marker, per-cell mV, and active faults).

### Browser preview
`GET /api/display/preview?template=wide_256x64&res=256x64` returns a rendered frame from
`SimDisplay`, and the editor can live-preview against the running WebSocket state. Layout iteration
happens in the browser with no hardware attached.

### OLED care (a dive computer shows a static layout for an hour)
Auto-dim after N seconds, scheduled pixel-shift, contrast ceiling in config, and a brightness
schedule for battery. Burn-in is a real failure mode here, not a cosmetic one.

---

## 13. Safety policy and arming gates

As **primary computer**, these are requirements:

- **Required to arm:** 2x ADS1115, baro (MS5837 or BMP085), OLED, cell bias sane, `V_BAT >= 3.6 V`,
  cal age < 24 h, per-cell air slope >= 30 mV/bar, all three cells agreeing within 20% in air,
  coil click self-test passing, no unresolved FATAL. Bench-only mode (screen/baro absent) is allowed
  on the bench, never for arming.
- **The gate is table-driven, and an active FATAL blocks on its own** — a code does not have to be
  listed in `blocksArming()` to refuse arming, so a new fatal code cannot become armable by being
  forgotten in a table. That was a real hole: `RAIL_3V3_LOW` was FATAL and *not* listed, so the
  3.3 V brown-out rail did not block arming. OLED (required by this section), `I2C_BUS_STUCK` and
  `EXTERNAL_WATCHDOG` were also missing and are now listed. `test/test_boot/` pins all of it.
- `ADC_PART_WRONG` and `ADC_CONV_BAD` block arming (ppO2 cannot be trusted from a converter that is
  the wrong part or is not sampling). `ADC_ID_UNKNOWN` and `ADC_RATE_MISMATCH` deliberately do
  **not**: they are true statements about a converter that is working, and refusing a dive at the
  water for a cosmetic code trains a diver to override the gate. They block nothing and hide nothing
  — they are printed at boot, in `/api/health`, and in the dive-log header.
- `FATAL_NO_ADS1115` means: recorded at boot, arming refused, but bench/web/sim/config all work.
  **Mid-dive** dropout of either converter is a fatal alarm + `FAULT_LOCKOUT`.
- Arm via **two-button physical sequence**, never from the web page. The OLED prints the second-
  independent-computer reminder at arm time.
- **A second independent computer plus a hand board is mandatory** — triple-cell voting shares one
  firmware and (cells 1–2) one converter, so it does not protect against a common-mode firmware or
  supply fault.
- Audible (>=90 dB) **and** haptic alarms driven from the dive task; a screen failure cannot mute them.
- **Redundant power integrity:** external watchdog, brown-out detection, 3.3 V rail sensing, config
  double-write, tissue-state flush before shutdown.
- Physical **manual inject** and a **manual gas bypass no firmware can inhibit**.
- Firmware is **uncertified and unaudited**. Bench-test it and never dive it as the only electronics.

---

## 14. Directory layout

```
platformio.ini  partitions.csv  README.md  AGENTS.md
lib/            host-buildable pure C++, no Arduino headers — this is what `pio test -e native`
  core/src/     covers. (Moved out of src/core deliberately: src/ can never compile natively.)
                FaultCode  BootReport  GasMix  GasModel  Voting  O2Toxicity  Zhl16c  AdcRegs  PinMap
  sim/          SimBaro  SimCells(+fault injectors)  LoopModel  ValveModel  CcrControlLaw  SimRunner
                built and running headless on-target (bench `v`); web sim panel + /api/sim/verify await M6
src/            ESP32-only (Arduino/ESP-IDF headers allowed here)
  main.cpp                     setup(), boot probe, hot re-probe loop, bench command REPL
  config/  Pins.h (alias of core/PinMap.h)  Features.h  RuntimeConfig.{h,cpp}
  sensors/ BusProbe  Ads1115Bus  CellArray  Barometer(iface)  Ms5837  Bmp085  Depth  Ina219
  dive/    CcrControl  SolenoidDrive  BreathingState  Alarms  DiveLog  TissuePersist
  ui/      DisplayDriver(iface)  Ssd1306I2C  TemplateEngine  FieldRegistry  Scenes  Buttons
  web/     WebServer  Api  assets/(index.html app.js style.css)
test/      test_voting/ test_gas/ test_zhl/ test_boot/ test_adc/ test_pins/
           native Unity suites (test_* required)
docs/      TUNING.md   (README.md holds wiring + BOM + bench procedure)
data/      LittleFS: index.html app.js style.css display/templates/*.txt
```

Note: `GasModel` (inspired-gas resolution), `BootReport`, `AdcRegs` **and `PinMap`** are in `lib/core`,
not in `src/` — all are pure C++ and all are under native test, which `src/` cannot be. `PinMap.h`
carries the GPIO capability table and the role assignment; `src/config/Pins.h` includes it rather than
holding a second copy of the numbers (decision 33).

---

## 15. Toolchain and build

pioarduino Core 6.2.0 **is installed** on this machine (`~/.platformio/penv`), with the espressif32
platform, `framework-arduinoespressif32` and `toolchain-xtensa-esp-elf` downloaded. Installer, for
a fresh machine:

```
curl -fsSL -o get-platformio.py https://raw.githubusercontent.com/pioarduino/pioarduino-core-installer/pioarduino/get-platformio.py
python3 get-platformio.py && source ~/.platformio/penv/bin/activate
```

`platformio.ini`: `board = esp32doit-devkit-v1`, `framework = arduino`,
`platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip`
(Arduino core **4.0.0 as installed** / IDF 5.5.6 — read from `cores/esp32/esp_arduino_version.h`,
not 3.3.x as earlier notes assumed. The core version is load-bearing: core 4.x uses the new
`esp32-hal-i2c-ng` driver, whose probe and periman semantics differ from the legacy driver's —
decisions 25 and 26). **Do not use the stock `espressif32` platform** — it is pinned to
Arduino core 2.x.

`lib_deps` (versions verified against the registry 2026-11-19; the earlier ones in this file did
**not resolve**): `esp32async/AsyncTCP@^3.4.0`, `esp32async/ESPAsyncWebServer@^3.12.1`,
`adafruit/Adafruit ADS1X15@^2.6.2` (**2.x, not 1.x** — `^1.4.0` fails resolution outright),
`adafruit/Adafruit BMP085 Library@^1.1.3`,
`thingpulse/ESP8266 and ESP32 OLED driver for SSD1306 displays@^4.6.2` (4.6.2 is the newest; `^4.6.5`
does not exist), `bblanchon/ArduinoJson@^7.2.0`.

Three build-system traps found while getting the first ESP32 binary to link, all of them silent
until they break the build:

1. **Adafruit BusIO includes `SPI.h` unconditionally**, so the framework's SPI library must be in the
   build even though every sensor here is I2C.
2. **Arduino core 3.x core libraries depend on each other** (`WiFi` → `Network`, ESPAsyncWebServer →
   `WiFi`) and the default dependency scanner does not chain them. Fix, both required:
   `lib_extra_dirs = ${platformio.packages_dir}/framework-arduinoespressif32/libraries` and
   `lib_ldf_mode = deep`, with `SPI Wire WiFi Network FS LittleFS DNSServer ESPmDNS` listed explicitly
   in `lib_deps`.
3. **`partitions.csv` app partitions must start on a 64 KB boundary** (`Offset 0x11000 is not
   aligned to 0x10000`), and the LittleFS partition needs `SubType = littlefs`, not `spiffs`.
   Actual layout: `app0 @0x20000 0x180000`, `app1 @0x1A0000 0x180000`,
   `fs @0x320000 0xC0000` (768 KB, not the ~1 MB originally planned — 4 MB flash does not fit 2x1.5 MB
   apps plus 1 MB of filesystem).

`board_build.filesystem = littlefs`, plus a `native` env for `pio test -e native`
(`test_build_src = no`). Compile flags: `POWER_RAIL="1S2P"` (a string — a bare `1S2P` token is not a
valid pp-token if anything ever expands it), `BENCH_MODE=1`, `HAVE_SOLENOID` (default **0**),
`WIFI_DURING_DIVE` (default 0), `BENCH_RAW_PROBE` (default **0**; =1 compiles in the raw I2C
transaction diagnostic, which **writes** to whatever ACKs — bench bring-up on a bare bus only,
decision 25).

First ESP32 link result (`pio run -e esp32`, 2026-11-19): **SUCCESS**, Flash 457 KB of 1.5 MB
(29%), RAM 35.5 KB of 320 KB (10.8%). Boot probe + I2C scan only — the web stack is pulled in by
`lib_deps`, which is why the number is already that size.

**M0 upload gate met on real hardware (2026-11-19).** Board: `/dev/cu.usbserial-0001`, CP2102
(VID:PID 10C4:EA60), chip `ESP32-D0WD-V3 (rev v3.0)`, MAC `c0:49:ef:ca:38:28` — matches
`esp32doit-devkit-v1`. `pio run -e esp32 -t upload` verifies ("Hash of data verified"); RTS-based
auto-reset works, so no BOOT button is needed, and serial capture via pyserial at 115200 works.
App now 466,973 B flash (29.7%) and 35,600 B RAM (10.9%). Both buses init at the documented pins
(`Wire` 21/22, `Wire1` 18/19 — **I2C1 is usable, the pin map stands**), and with nothing connected
the boot report is honest: 0 devices on both buses, FATAL ADS1115_U1/U2, ERROR BARO/OLED/INA219,
`[arm] REFUSED`, solenoid INFO. Bench commands over serial: `s` full sweep, `p` probe table +
report, `r` raw probe codes (needs `BENCH_RAW_PROBE=1`), `c` 9-clock recovery + port re-init,
`a` ADS1115 self-test (ID register, config decode, write-verify, DRDY line, one timed conversion),
`d` ADS1115 rate characterisation — measured conversion period for all eight RATE fields, which is
what the coil/ADC interlock window and the per-cell sample rate are derived from, not the datasheet,
`t` pair-walker report — achieved per-cell SPS, measured in-walk conversion periods, interlock busy
duty and error counters, counted from conversions actually completed (decision 34's numbers come
from here),
and `g` pad map + pad test — who owns each GPIO according to the core (periman), plus measured
pull-up/pull-down and output behaviour on pads with nothing wired,
and `b` baro identity — Bosch-family CHIPID/version/cal-ROM reads at the candidate addresses, which
is how the fitted "BMP085" was measured to be a clone (§2) rather than inferred from its label. `g` is what settled §3, and `v`
runs the fixed headless sim profile (see §11): it prints per-minute SIM-stamped lines plus a
measured **worst-tick cost** line — per-tick compute cost is derived from the target, never assumed
from the host (risk 19).
Probe error codes mean different hardware stories and are worth reading: **2** = NACK on the address
(nothing there), **5** = timeout (the bus is being held — measured when an ADS1115 module lost its
supply and clamped SDA through its protection diodes; nine clocks did not free it).
Cosmetic only: IDF prints `esp_core_dump_flash: No core dump partition found!` twice per boot
(there is no core dump partition by design); it is emitted before `setup()` runs, so
`esp_log_level_set` in `setup()` cannot silence it.

Commands: `pio run`, `pio run -t upload`, `pio device monitor`, `pio test -e native`,
`pio run -t uploadfs`.

**README.md must contain** (user requirement): the safety statement and what this is not; BOM
(Bar30/MS5837-30BA breakout, 2x Adafruit ADS1115 breakout #1085 (decision 35), MOSFET + flyback Schottky + 10 mOhm shunt, INA219,
1 MOhm x12, 10 ohm x12, watchdog IC, 2P PCM >=3 A, valve); full wiring tables plus an ASCII harness
diagram for MS5837 (3.3 V, 7-bit 0x3E / datasheet 8-bit 0x7C, Bar30 DF13 colours: red power /
black GND / yellow SDA / blue SCL),
BMP085 fallback (7-bit 0x3B), both ADS1115 with ADDR straps 0x48/0x49 and DRDY lines, the per-cell 1 MOhm +
10 ohm + RC network and polarity, OLED, solenoid drive stage, buzzer, motor, buttons, watchdog,
INA219; **common wiring mistakes** (powering a galvanic cell, missing common-mode resistors, both
ADDR straps at GND, solenoid without flyback, a diode in the coil path, ADC2 pins, GPIO 34–39 as
outputs, panel on I2C that should be SPI, **searching for the baro at 0x7C when `Wire` wants 0x3E**);
power and battery budget including the 0.3 J/pulse vs
0.4 W math; valve selection table and the seat-pressure warning; maintenance (MS5837 24 h immersion /
2 h drying, annual cell replacement, connector corrosion, moisture and potting); and the pre-water
bench procedure.

---

## 16. Test strategy

**Current state (2026-11-19): `pio test -e native` is green — 94 cases across seven suites
(`test_voting`, `test_gas`, `test_zhl`, `test_boot`, `test_adc`, `test_pins`, `test_sim`);
`pio run -e esp32` links **and is flashed and running on the devkit** with two ADS1115 modules and
an SSD1306 actually attached (both converters measure as clones — risk 16, decision 31). The
positive-ACK caveat that M0 left open is closed: `I2C1: 2 device(s) [0x48 0x49]`,
`I2C0: 1 device(s) [0x3C]`, and the corresponding faults clear while the missing ones stay raised.
Reads and writes on the attached converter are verified, not assumed: `a` write-verifies a register
and restores it, `d` measures the conversion period field by field — which is how the register-map
faults and the non-standard rate table in section 2 were found — and `g` measures who owns each GPIO
and what its pads can do, which is how the pin map in section 3 was settled. The simulator runs
**headless on the board** (bench `v`), and its on-target numbers are mole-balanced against hand
calculations (risk 20). The alternating-mux pair walker runs in `loop()` and is measured by bench
`t`: **7.2 SPS per cell achieved** on the fitted clones, in-walk conversion periods 65.5-67.1 ms
(boot-timed 61.9 ms + OS-poll detection lag), interlock busy duty 95-96%, and a 60 s continuous run
with zero I2C errors, zero conversion timeouts and the board responsive afterwards.**
Layout note: PlatformIO only collects suites from `test/test_<name>/` folders (a bare `test/native/`
group folder collects nothing), so the suites are `test/test_voting/`, `test/test_gas/`,
`test/test_zhl/`, `test/test_boot/`, `test/test_adc/`, `test/test_pins/`, `test/test_sim/`. `test_build_src = no` in the
`native` env: `src/` is ESP32-only by design and is not built on the host. `lib/core` is built
natively and linked into every suite.

Covered now: voting truth tables (agree, outlier, dead cell, two disagreeing -> lowest, three
disagreeing -> DRIFT_ONE on the median, stuck-high, railed input, aged cell, zero-ppO2 tolerance
floor), diluent-flush verification including the correlated-error case voting cannot see and the
"all three cells must be present" rule, calibration sigma gate; MOD / hypoxic floor / END / density
against hand-computed values (air MOD 66.1 m, EAN32 39.9 m, 10/80 hypoxic 7.9 m, air at 60 m
8.3 g/L), best-mix selection, gas validation, setpoint crossover, inspired pressures summing to
ambient, CCR<->OC switch changing inert loading, bailout using the bailout gas, CNS table and
accumulation/washout, OTU; ZHL single-compartment kinematics against the exact exponential, no
obligation cases, NDL ordering and its dependence on the breathing state, 50 m/20 min air deco
obligation, TTS monotonicity in depth/bottom time/GF, stop grid and last-stop floor, violation
detection, GF ramp anchor/monotonicity/clamping, helium engaging the fast compartments, ICD guard,
tissue + GF-anchor round trip, plan convergence across a 12-profile spread; boot report
(default severities, escalation-only, hot-plug close keeps history, reprobe bumps attempts without
duplicating, BMP085 range banner, absent solenoid is INFO and must **not** block arming, summary
line clips to whole codes with a `+...` marker and never writes past its buffer, capacity
exhaustion keeps the earliest faults, **the fault tables walked in full** (every enumerator must have
a name and a non-INFO default severity, so a code added to the enum and forgotten in a `switch` fails
immediately — it caught a real omission the day it was written), wrong-ADC-part and bad-conversion
block arming while a bare unknown-ID warning must **not**, and an unlisted FATAL still blocks.
`test_adc`: config-register field masks tile 0xFFFF exactly once and the rate field sits below PGA;
the measured power-on word 0x8583 decodes as single-shot / PGA gain 2 / 8 SPS; the design config
selects gain 16 + 32 SPS + explicit single-shot; the PGA gain/FSR/LSB tables (7.8125 uV at gain 16);
9 mV and 13 mV air signals as code counts, with clamping at full scale; conversion periods; mux
power-down encodings excluded; ADS1115 vs ADS1015 separated 100x by the same rate field; and the
bench module's measured rate table recorded as *not* the datasheet's).
`test_pins`: no two roles share a GPIO (the defect that started it — 33 was the motor *and* the Page
button); every output role sits on a pad that can actually drive (34–39 cannot, and an alarm that
never actuates is silent); no role on a boot strapping pin; the no-such-pad set (24, 28–31), the
die-pads-not-bonded-out set (20, 37, 38) and the flash set (6–11) each asserted as their own claim;
34–39 input-only and without internal pulls; button pads asserted to have internal pull-ups; every
pad number 0..39 has a decided capability word; the ADC/bus wiring matches the §3 table; and the
spare output budget is asserted to be exactly one pad (17), so freeing a pin forces the §3 budget to
be re-derived on purpose.
`test_sim`: loop physics against hand calculations — **depth-independent** metabolic sag (0.152
bar/min at both 10 m and 30 m, the risk-20 regression) with the zero-mole floor at the hand-computed
276 s; closed-loop setpoint hold asserted in **pulse quanta** (~14 pulses over a 7-min hold at 30 m
against the hand-computed 2.15 amb L demand); volt-seconds seat criterion (low battery clicks but
delivers nothing); width compensation; the three click-fault signatures distinguished in the right
dimension; a no-click fault sagging ppO2 **and** flagging electrically; current-limited / stuck /
dead / absent cells reaching the voter as millivolts; deterministic noise; diluent-flush agreement
from the other side (fraction -> mV -> cal -> vote) including the stuck-cell case that makes it
fail; source stamping; bit-identical repeat runs; and the assertion harness itself — ring overwrite
with `histDropped` accounting, strides, and `NO DATA`/`unevaluated` so an empty window can never
"pass" an assertion.

- `pio test -e native` covers `lib/core/` **and** `lib/sim/` — the same physics models run on the
  host with no hardware. (They live under `lib/`, not `src/core`, so they build natively; `src/` is
  ESP32-only.) Voting truth tables (stuck high, stuck low, all disagree, near-zero,
  current-limited at depth), ADS1115 alternating-mux schedule and `invert` flags, ZHL-16C against
  published schedules (50 m/20 min air at GF 30/70; a 45 m 21/35 trimix schedule with the He
  compartment controlling), ICD guard cases, CNS table, MOD / hypoxic / END / density refusals,
  GF interpolation, log ring wrap, config schema migration, boot report severities.
- On-target: **diluent-flush agreement**, O2 flush linearity check at 6 m, dilute-flush expected-ppO2
  check, solenoid click tests, watchdog bite measured, brown-out flush and recovery, tissue-state
  recovery across power loss, config double-write integrity, 24 h soak, 20% droop test at a
  discharged 3.4 V pack with a scope on the 3.3 V rail while firing 10 pulses (ADS1115 readings must
  move <1 mV), arm-checklist rejection tests, displayless alarm test, manual-inject with firmware
  dead, template validator rejection tests.

### Milestones (each independently verifiable)
| # | Deliverable | Gate | Status (2026-11-19) |
|---|---|---|---|
| M0 | installer, build, flash, I2C scan, boot report | `pio run -t upload` works on this Mac | **DONE** — flashed and verified on the devkit (CP2102, ESP32-D0WD-V3): "Hash of data verified", clean boot banner, boot report correct on an empty bench, `[arm] REFUSED`. Section 15 holds the evidence. |
| M1 | **Simulator + bench mode + boot fault report**; 2x ADS1115 driver with alternating mux | boots with zero sensors and reports every miss; sim profile runs headless | **DONE (2026-11-19).** Boot/bench and the simulator were verified earlier; the last open item, the **alternating-mux scheduler**, is now implemented and measured: `AdcPairWalker` in `src/sensors/Ads1115Bus` — non-blocking pair-step state machine, sized from the boot-measured conversion period (never the datasheet), polled OS bit at 10 ms cadence (DRDY still unwired, open item 14), I2C-error backoff, and a coil/ADC interlock window carried per converter. Bench `t` measures it: **7.2 SPS per cell achieved** on the fitted clones (design 16, clone-sized 8), in-walk periods 65.5-67.1 ms, interlock busy duty **95-96%** → decision 34: M5 must carve explicit pulse slots. `lib/sim` runs the fixed 26-min profile headless on the board (bench `v`) at hand-computed pulse demand; **94 native cases green** in seven suites. Open item 13's outstanding action (order two Adafruit #1085 breakouts — decision 35 — and re-run `d`/`a`/`t`) stands — the walker re-sizes itself from the new parts' own measurement at boot. |
| M2 | air cal + voting + diluent-flush check + alarms | native voting tests pass | voting + diluent-flush + cal sigma gate done natively; on-device wiring not started |
| M3 | `Barometer` iface + MS5837 + BMP085 fallback + learned Psurf + README wiring | fallback verified by removing the MS5837 | presence probe finds 0x3E/0x3B (datasheet 8-bit 0x7C/0x77 corrected) and raises `BARO_RANGE_9M` when only the BMP085 answers; drivers not written |
| M4 | ZHL-16C 3-gas + GF + ICD + CNS + MOD/END/density | validated natively **before** hardware depends on it | **engine complete and regression-anchored natively** (47 cases); *not* validated against a trusted computer yet — risk 13 |
| M5 | pulse-driven solenoid + click verification + breathing-state machine + bailout | three-way solenoid fault taxonomy + ADC-window interlock | not started |
| M6 | web + WS + settings + cal UI + dive-log graphs | phone on SoftAP, all tabs, offline graphs render | not started (deps resolve and compile) |
| M7 | display template engine + FieldRegistry + SSD1306 scenes + browser preview | template fit report and validator rejections | not started |
| M8 | hardening: watchdog, brown-out, persistence, GF-anchor + tissue flush, soak at discharged pack | measured watchdog bite, recovery tests | not started; the GF-anchor half is done in `lib/core` (decision 24) |
| M9 | in-water readiness: arming sequence, displayless alarms, WiFi-off default, bypass paths | arm refusal matrix tested | not started; `blocksArming()` is the hook and is tested |

---

## 17. Conventions

- C++17, no exceptions in the dive task, no dynamic allocation after boot in the dive path.
- Pure C++ (no Arduino headers, host-testable) lives in `lib/`; anything that touches a pin, a
  peripheral or the network lives in `src/`. The split is enforced by the build, not by convention:
  `src/` is not compiled natively.
- All thresholds and configuration live in `RuntimeConfig`, never as magic numbers in the engine.
- Config schema is versioned; every change needs a migration and a native migration test.
- Fault codes are an enum in one place (`FaultCode.h`), with severity, and are stable once shipped —
  they appear in logs and in user-facing docs.
- No silent fallbacks: if a sensor is missing, something must display or log that it is missing.
- Units: bar for pressure, metres for depth, Celsius, milliVolts for cells. Convert at the edges.

---

## 18. Decision log

Re-open only with the user's explicit agreement.

| # | Decision | Rationale / status |
|---|---|---|
| 1 | PlatformIO + Arduino framework, pioarduino `stable` platform | Arduino core 3.3.x; stock `espressif32` is core 2.x only. Chosen by user. |
| 2 | ZHL-16C + gradient factors | Chosen by user over NDL-table and VPM-B. |
| 3 | **Helium path implemented now**, not deferred | Chosen by user. 3-gas, He tables allocated, ICD guard default on. |
| 4 | Full automatic O2 injection control loop | Chosen by user over manual-only and dry-contact-alarm-only. |
| 5 | WebSocket push ~2 Hz (ESP32Async ESPAsyncWebServer 3.12.x) | Chosen by user over HTTP JSON polling. |
| 6 | ESP32 classic (WROOM devkit) | Chosen by user; drives the pin map in section 3. |
| 7 | **MS5837 primary, BMP085 fallback**; wiring documented in README | Chosen by user. BMP085 caps at ~9 m — fallback only. |
| 8 | **Two ADS1115s, differential pairs** | Chosen by user. Better CMRR than single-ended on one ADC; cells 1+2 still share a converter (see 19). |
| 9 | **Galvanic cells** | Chosen by user. No bias supply, ~10 mV in air, gain 16. |
| 10 | **This is the PRIMARY dive computer** | Chosen by user. Inverts the original "screen/baro optional" stance: optional for bench, **required for arming**. Adds external watchdog, arming gates, second-computer requirement. |
| 11 | Solenoid: 5 V valve originally stated, **valve not yet purchased**; power is **1S2P LiPo** | Chosen by user. Coil driven from the battery rail, **no boost stage**, voltage-compensated pulses, no diode in coil path. |
| 12 | Drive law = **fixed-width pulses at variable rate**, not LEDC duty | Inductive valves need full voltage to open repeatably; also enables click verification. |
| 13 | Coil-current shunt for **click verification** and 3-way fault taxonomy | Detects a failed valve directly instead of inferring it from ppO2. |
| 14 | Injection pulses scheduled into ADS1115 conversion gaps | Prevents coil-kick noise from masquerading as cell disagreement and tripping voting. |
| 15 | **Boot is never fatal**; boot fault report + LittleFS fault ring + hot re-probe; **default state is BENCH** | Chosen by user. Supersedes the earlier "ADS1115 missing = error only" at boot: that requirement now applies at the **arming gate** and to mid-dive dropout. |
| 16 | **Dive simulator in bench mode**: depth / duration / setpoint, 1x-10x-60x, fault injection | Chosen by user. Implemented at the sensor layer; closed loop with the real control law; results stamped `SIM`. |
| 17 | **Display layout is templated** (LittleFS text templates + FieldRegistry + priority fit), web-editable with browser preview | Chosen by user, to allow a bigger OLED and more on-screen data without reflashing. Validator keeps SAFETY-class fields unremovable. |
| 18 | No CDN; assets from LittleFS; graphs hand-drawn on canvas | The phone connects to the ESP32's AP with no internet. |
| 19 | WiFi off during the dive by default | Battery, radio in the loop, and the OLED is the in-water display. |
| 20 | **Deco model uses the ZH-L16A coefficient set and the Subsurface GF/ceiling form**; He half-times differ from N2, `a`/`b` blended per compartment by saturation share | Verified against Subsurface `core/deco.cpp` (`tissue_tolerance_calc`, `get_gf`, `deco_allowed_depth`), which is field-deployed. My original 1995-pairing closed form (`gfRequired = p/(a + p/b)`) produced **no decompression obligation for a 50 m / 20 min air dive** — a lethal error, caught before any hardware depended on it. Still regression-anchored, not validated: cross-check on a trusted computer before diving. |
| 21 | **TTS is planner-style** (the plan walks the whole ascent, holding each stop until the ceiling clears the next rung) | Deliberately more conservative than a current-ceiling estimate, and it is what makes TTS stable instead of jumping as the diver moves. Cost: longer published TTS. |
| 22 | **The stop schedule ends at the stop floor (`lastStopM`)**: an obligation shallower than the last stop is dropped, and the raw ceiling stays on the display | Holding at the last stop until the model is fully satisfied while breathing at that depth does not converge (intermediate compartments re-saturate at the stop depth): measured 67 min at 6 m for a 50 m / 20 min air dive vs ~10 min on a GF 30/70 computer. `lastStopM` is the diver's explicit choice of unpaid tolerance. |
| 23 | **`DecoPlan.bounded`**: an unconverged plan search is reported and its schedule withheld, never silently published | No silent fallbacks (section 17). A deco search that stalls must degrade to "no trustworthy number", not to a plausible-looking one. |
| 24 | **The GF-low anchor (`gfLowPressure`) is persisted and restored with the tissue compartments** | It is dive state, not derived state. Tests showed restoring tissues without it re-bases the GF ramp and reports a zero ceiling after a power loss — section 19 risk 8 in a new disguise. |
| 25 | **I2C presence = address phase of a zero-length write with STOP** (`beginTransmission(a); endTransmission(true)`; `err==0` present, `2` absent). `endTransmission(false)` with no payload, `write(0x00)`-style probes and `requestFrom` probes are all banned. | Measured on the devkit 2026-11-19, Arduino core 4.0.0 / IDF 5.5 driver: the repeated-start form returned **0 at every address** on a bare bus, so the boot report claimed 16 devices per bus and "ALL SENSORS OK" with nothing connected — a silent all-clear on a life-support device. A payload makes the driver wait for a data-phase ACK (error 4 when absent) and **writes** to a present device: 0x00 clears the INA219 calibration register and reprograms the ADS1115 config register. `requestFrom` cannot prove absence (the SSD1306 is write-only) and spams the console. The chosen form is what the driver routes to `i2c_master_probe()`: truthful and non-destructive. |
| 26 | **Never touch an I2C pin with `pinMode`/`digitalWrite` while its Wire port is initialised**; stuck-SDA detection runs only before `begin()` or after `end()`, and boot probing is bounded to the fitted address table (full sweep = explicit bench command). | Measured: in core 4.x `pinMode` on a driver-claimed pin goes through periman, which **detaches and deinits that bus** — `i2cIsInit()` drops to 0 and every later transaction answers "bus is not initialized". A stuck-line check inside a scan therefore destroyed the bus it was checking. Bounded probing exists because a 112-address sweep on a dead port emitted ~200 driver error lines and buried the boot banner. |
| 27 | **Hand-rolled ADS1115 driver** (`src/sensors/Ads1115Bus.{h,cpp}`) instead of Adafruit ADS1X15, which stays in `lib_deps` unused for now. | The design needs alternating single-shot conversions with the mux re-written between them, explicit knowledge of when a converter is integrating (for the coil interlock), and register-level checks that distinguish a working part from one that merely ACKs. A continuous-mode Arduino driver does none of those. Also removes the BusIO/SPI dependency from the sensor path. |
| 28 | **The register map, PGA/RATE tables and cell scaling live in `lib/core/src/AdcRegs.h`** (pure C++), not in the ESP32 driver. | They were wrong twice — the rate field position and the MODE bit polarity — and constants that exist only in `src/` cannot be reached by `pio test -e native`. Moving them is what makes them assertable; `test/test_adc/` now pins them against the word measured on the bench module. |
| 29 | **Converter identity and sample rate are established by measurement** (one timed single-shot conversion at the design field, plus bench command `d` for the whole rate table), **not by the ID register and not by the datasheet.** | The ID register cannot do the job: the bench module answers 0x00 there, at the pointer register too, while its config register behaves exactly as documented. Timing can: an ADS1015 differs 100x at the same rate field. And the datasheet's timing turned out not to describe the fitted part (62 ms where 31.25 ms is promised), so the interlock window and per-cell rate are derived from measurement — decisions 29 and risk 16 are the same fact seen from the code side and the parts side. |
| 30 | **Arming refuses on any active FATAL, independently of the `blocksArming()` code table.** | Section 13 says "no unresolved FATAL", and the table contradicted it: `RAIL_3V3_LOW` is FATAL and was not listed, so a 3.3 V brown-out rail did not block arming. OLED, `I2C_BUS_STUCK` and `EXTERNAL_WATCHDOG` were also missing despite section 13 requiring them. The table stays for the ERROR-and-above cases; the FATAL rule is the safety net that a new code cannot fall through. |
| 31 | **The ppO2 converters shall be genuine TI ADS1115s.** The fitted module stays on the bench for timing work only; it does not go near a dive. Resolves open item 13 as option (a). | User's call, 2026-11-19 ("use the ADS1115, not the ADS1115S"). Risk 16 already said sourcing known-genuine parts is a safety action, not a preference; this makes it the plan. Note for honesty: **"ADS1115S" has no findable datasheet**, so part identity here stays a *measurement* claim (decision 29: timed conversion at the design RATE field, `d` for the whole table), never a part-name claim. Independent corroboration that generic breakouts carry non-TI dies with off-datasheet rate tables: Hackaday 2025-10-15 "How Bad Can A Cheap Knockoff ADS1115 ADC Be?" and excamera.substack.com "How bad can a $2.97 ADC be?" (which reports relabelled ADS1015 dies). Acceptance test when new parts arrive: `d` must measure 8/16/32/64/128/250/475/860 SPS, `a` must read ID 0x85, and `ADC_ID_UNKNOWN`/`ADC_RATE_MISMATCH` must clear on their own. |
| 32 | **GPIO 33 is the Page button; the vibration motor moves to GPIO 16.** Resolves open item 12. | User's call, 2026-11-19. The old note "avoid 16/17 (UART0 on most devkits)" was measured false on this board — bench command `g` shows the console is UART0 on **GPIO1(TX)/GPIO3(RX)**, periman confirms both, and the core's own `doitESP32devkitV1` variant header says `TX = 1; RX = 3`. 16/17 are free, output-capable, and their internal pulls work (measured). 33 is kept for the button because it has an internal pull-up (34–39 do not) and reading a button is what that pad is for. The motor keeps an external gate pulldown either way — an internal pull is not a fail-safe. 17 is declared the spare. |
| 33 | **The GPIO capability table and the role assignment live in `lib/core/src/PinMap.h`** (pure C++, asserted by `test/test_pins/`); `src/config/Pins.h` is an alias, not a second copy. | Same remedy as decision 28, applied to a different class of defect. The pin map was a paragraph nobody could test, and it was wrong twice: GPIO 33 assigned to two functions, and pad groups assumed free or unusable based on the die instead of the module (measured: 24 and 28–31 do not exist on the ESP32-D0WD-V3 die at all; 20/37/38 exist but WROOM-32 does not bond them out). A duplicate pin or an output on an input-only pad is now a test failure rather than a bench-chase. |
| 34 | **The M5 coil-pulse scheduler must carve explicit conversion-free slots into the ADC cycle; the incidental inter-conversion gap is measured ~2–3 ms and shall not be designed against.** | Implemented-and-measured 2026-11-19, before M5 exists: the alternating-mux walker (`AdcPairWalker`) keeps both converters integrating 95–96% of wall time at the fitted clones' rate (mean in-walk conversion period 66.4 ms, bench `t`). §8's "pulses queue into the gap" assumed a gap exists; at 20–250 ms pulse widths it does not. Recorded now so M5 is designed against a measured figure instead of discovering the collision at the water. This is **inherent to the walk scheme, not an artifact of the clones**: the walker re-arms the next conversion immediately, so back-to-back conversions leave the same ~µs gap at genuine-part rates too — only the window size changes (62 ms → ~31 ms). |
| 35 | **The two ppO2 converters are specified as Adafruit "ADS1115 16-Bit ADC – 4 Channel with PGA" breakouts (product #1085), 2x.** This is the named procurement that satisfies decision 31; it changes nothing about driver or acceptance. | User's call, 2026-11-19. Facts checked against the Adafruit product page and the 4-Channel ADC Breakouts guide: the board carries the TI ADS1115 (chip spec SBAS444 — the same table `AdcRegs.h` implements and `test/test_adc/` asserts, so **no register-map, PGA, LSB or rate change anywhere**), VIN 2.0–5.5 V (run at 3.3 V), default address 0x48 with one back-side `A0` solder jumper giving 0x49 — exactly this design's 0x48/0x49 pair — ALRT/DRDY broken out on the header (open item 14 becomes wireable), and on-board 10k I2C pull-ups (two boards ≈ 5k total; add none externally). Caveats kept on the record: **identity remains a measurement claim, never a brand claim** (decision 29) — the arrival acceptance test is unchanged (`d` measures the eight datasheet rates, `a` reads ID `0x85`, `ADC_ID_UNKNOWN`/`ADC_RATE_MISMATCH` clear by themselves); decision 27 also stands — the Adafruit ADS1X15 library is a bring-up cross-check at most, never in the dive path. Two board revisions (STEMMA QT / header-only) are both acceptable. |

---

## 19. Known risks — do not regress

1. **BMP085 fallback saturates at ~9 m.** Never let depth-dependent logic extrapolate past it.
2. **Cells 1 and 2 share one ADS1115** (one mux, reference, supply). Voting is not protection
   against that correlated failure. Diluent-flush verification and the arming gate exist because of
   this; do not remove them for flash or RAM reasons.
3. **Single-point air calibration proves nothing about linearity** at 1.3–1.6 bar, and a current-
   limited cell calibrates perfectly in air. The >=30 mV/bar slope check, the optional O2 check, and
   the 6 m O2-flush bench procedure exist because of this.
4. **Coil kick vs 10 mV analog signals.** Shared rail or missing bulk capacitance shows up as ppO2
   jitter and trips voting. The ADC-window interlock, or-ing diode and local bulk cap are load-bearing.
5. **Galvanic cell leads must never see a voltage.** A wiring error here also destroys the ADS1115.
6. **Missing 1 MOhm common-mode resistors** on differential cell pairs -> railed inputs, garbage
   readings that can look plausible.
7. **A latching valve held energised is an O2 flood.** Pulse-then-release only.
8. **Tissue state not persisted** = repeated dives silently reset the decompression model.
9. **Time compression at 60x invalidates timing-dependent tests.** Never certify ascent-rate
   limiting, warm-up gates or watchdog behaviour from a compressed run.
10. **A template must never be able to hide ppO2, depth or TTS.** Validator-enforced.
11. Simulator data reaching dive-state output is a fatal design bug. `source` stamping is not
    decoration.
12. `metabolic rate` and `loop volume` are unmeasured guesses. Sim dynamics tune the control law;
    they do not validate the loop.
13. **Plausible-looking deco numbers are the failure mode of this codebase.** Every defect found so far
    in `lib/core` produced numbers that *looked* reasonable: the wrong coefficient pairing gave no
    obligation for a 50 m/20 min air dive; a stalled plan search reported a truncated schedule as if
    it were complete; restoring tissues without the GF anchor reported ceiling 0 after a power loss.
    Any change to the deco model must run `pio test -e native` **and** be cross-checked against a
    trusted computer at the specific profile being claimed. Numerical self-consistency is not
    evidence of correctness.
14. **An I2C probe can be destructive or can lie.** `endTransmission(false)` with no payload ACKs
    nothing and reports success at every address (that is how an empty bench reported "ALL SENSORS
    OK"); a payload-bearing probe writes to the device — 0x00 wipes the INA219 calibration and
    reprograms the ADS1115 config register mid-flight. If you change probing, re-measure on a bare
    bus **and** on a fitted one, and never let a scan gain a write back.
15. **`pinMode`/`digitalWrite` on a pin owned by a live Wire port deinits that port** (periman
    detach, core 4.x). Bit-banging bus recovery, stuck-line checks or a software I2C fallback on
    18/19/21/22 while the driver is up silently kills every sensor read on that bus — in a CCR that
    means ppO2 and depth both stop. Any bus-recovery path must `end()` the port first and re-init
    after. See decision 26.
16. **The fitted ADS1115 module is not a genuine ADS1115.** It ACKs, its config register reads and
    writes correctly and it converts, but the ID and pointer registers answer 0x00 (the pointer does
    not track writes) and the RATE field implements only four rates via bits 6:5 — 8/16/32/64 SPS
    measured, where the datasheet promises eight up to 860. Consequences are load-bearing: the design
    field for 32 SPS yields 16.1 SPS (so 8 SPS per cell, not 16), and the coil/ADC interlock window is
    62 ms, not 31 ms. Two rules follow. Never size a timing window from the datasheet when a measured
    value is available (`d` produces it at any time), and treat **sourcing known-genuine TI parts as a
    safety action**, not a preference: a die whose register map differs from its markings has no
    characterised noise, timing or power-on behaviour, and this device is primary life support.
    **Decision 31 has now made that the plan**: genuine TI parts go in the ppO2 path, this module stays
    on the bench for timing work. Generic "ADS1115" breakouts carrying non-TI dies is a documented,
    independently-measured phenomenon (Hackaday 2025-10-15; excamera "How bad can a $2.97 ADC be?",
    which also reports relabelled ADS1015 dies), so the acceptance test on arrival is measurement, not
    the label: `d` measuring the eight datasheet rates and `a` reading ID 0x85. There is **no datasheet
    for "ADS1115S"** — identity remains a measurement claim (decision 29), never a part-name claim.
17. **A fault code that is not in a `switch` silently becomes INFO/"UNKNOWN" and stops blocking
    anything.** Every table in `FaultCode.h` has a `default`, so adding an enumerator without adding
    it to `defaultSeverity`, `blocksArming` and `faultName` produces a fault that reaches the fault
    banner as a blank, unclassified line. This happened for real the day the tables-walk test was
    written. The test enumerates the whole code range against an explicit gap list, so adding a code
    forces the tables to be updated; keep that gap list current when adding codes, and prefer making
    the *severity* carry the gate (risk 17 + decision 30) over adding special cases.
18. **A pin table can be wrong in a way that only shows up as a dead actuator or a phantom spare.**
    This one table managed both: GPIO 33 was assigned to the vibration motor *and* the Page button at
    the same time (a motor back-biasing a button read, found only by touching hardware), 16/17 were
    written off as the console when this board's console is GPIO1/GPIO3, and the "free pins" a careful
    reader computes from the die — 20, 24, 28–31 — do not exist on this die or are not bonded out by
    this module. An output role on an input-only pad (34–39) fails **silently**: the haptic alarm just
    never fires. The pin map therefore lives in `lib/core/src/PinMap.h` under `test/test_pins/`
    (decision 33) and the pad claims/pulls are measured by bench command `g`. When adding a role, edit
    the table and let the test say whether the pad exists, can drive, and is free.
19. **A bounded loop is not necessarily a cheap loop, and host timing does not transfer to the
    target.** The first headless sim runs looked like a hang: the console went silent at the minute
    the profile reached its deco obligation, no watchdog fired, and the board never answered again
    until it was reflashed. Nothing was wrong with the physics — every plan-search loop was capped —
    but an `NDL=999` tick paid a full 999-minute sweep at a 5 s step: 11,880 iterations, each
    recomputing 32 `expf`. That is 0.5 ms on x86 and **1.15 s on xtensa**, because the ESP32's
    soft-float `expf` is ~3 us, not the PC's ~40 ns. Bounded, numerically correct, and effectively a
    hang at the bench. Fix: hoist the tissue decay factors per search call (bit-identical — the
    `expf` inputs are the same) and phase the NDL horizon (fine step to 5 min, 15 s to 60 min,
    120 s beyond). Measured: that tick went 1.3 ms → 0.07 ms natively, and the board's worst tick
    went **1145 ms → 41.5 ms**. Rules that follow: treat per-tick compute cost as a **measured
    quantity on the target**, never a host number (bench `v` prints worst-tick cost for this reason);
    where a loop is capped, make cap-saturation *reportable* (`DecoPlan.bounded`,
    `Engine::lastPlanSteps()`, the `SIM SLOW` line) — a search that burns its whole budget must be
    visible, not merely survivable; and a watchdog is no substitute for cost discipline, because a
    loop that yields every 15 ticks starves the display and the alarms for minutes without ever
    tripping anything.
20. **Ambient litres and surface litres both produce self-consistent setpoint tracking, so a units
    error between them is invisible in ppO2 and wrong only in the gas figures.** Metabolic O2 use
    is a molar draw, quoted in surface (STPD) litres and depth-independent; a valve's per-pulse
    volume is an *ambient* litre and scales with pressure. The first loop model multiplied a
    surface-quoted 1.2 L/min by ambient pressure, quadrupling consumption at 30 m — and the closed
    loop still held 0.68 against a 0.70 setpoint, because the control law just injected 4x more.
    The pulse count, injected litres, diluent consumption and sim runtime were all 4x wrong and all
    looked plausible; the sim's own test encoded the wrong convention as a "hand-computed" anchor.
    Found by auditing the mole balance of the on-target capture against a hand calculation, not by
    any test. Rules that follow: the loop integrates **moles**; metabolism is
    `metabolicRateSurfLpm`, injection is `pulseVolAmbL`, and the two conversions are stated in
    `LoopModel.h`; and a gas figure is only believed after a **mole-balance** check — at steady
    state the injected ambient litres must equal metabolic surf L x Pamb/Psurf (measured: 2 pulses
    per minute against a hand-computed 2.0, 14 pulses over a 7-min hold against a hand-computed
    2.15 amb L). A ppO2 trace that holds setpoint is not evidence the gas accounting is right.

---

## 20. Open items

| # | Item | Current default |
|---|---|---|
| 1 | **Valve purchase**: pilot-operated NC at 8–10 bar IP, vs regulating O2 close to loop pressure | Default: pilot-operated NC at ~8–10 bar IP (production-CCR practice). Needs the user's choice; decides the BOM. |
| 2 | Latching vs single-coil valve | Latching preferred if available; if single-coil, drop the bidirectional pulse logic. |
| 3 | O2 intermediate pressure value for pulse-volume calibration | Untuned placeholder in config until bench-measured. |
| 4 | Per-valve pulse volume (ambient L/pulse) | To be measured on the bench; simulator uses an estimate. |
| 5 | Metabolic O2 rate and loop volume for the sim | 1.2 L/min and 8 L defaults; must be measured. |
| 6 | Replacement display panel choice (256x64 SSD1322 vs colour TFT) | Driver abstraction is SPI-ready; nothing decided. |
| 7 | Helium enabled above what depth | Config field exists, default unset. |
| 8 | Buzzer / vibration motor part numbers | 45 mm piezo buzzer and a coin vibration motor assumed. |
| 9 | Enclosure, potting, moisture management, connector scheme | Not designed. Humidity kills this hardware; must be scheduled. |
| 10 | Second independent computer + hand board selection | Required by section 13, not yet chosen. |
| 11 | MS5837 mounting (Bar30 M10 bulkhead) and its drying discipline in the maintenance schedule | Part suggested, procedure written, not yet validated. |
| 12 | **GPIO 33 is assigned to both the vibration motor and the Page button** (section 3) | **RESOLVED 2026-11-19** (decision 32): 33 stays the Page button, the motor moves to GPIO 16. Measured by bench command `g`: this board's console is GPIO1/GPIO3, not 16/17. `test/test_pins/` now asserts no two roles share a pad, so the class of defect is closed too, not just this instance. |
| 13 | **The fitted ADS1115 module's rate table is not the datasheet's** (risk 16): 4 rates not 8, and the design's 32 SPS field gives 16.1 SPS | **DECIDED 2026-11-19** (decision 31, procurement named by decision 35): option (a) — **2x Adafruit ADS1115 breakout #1085**; the fitted module stays on the bench for timing work only. Outstanding action, not an open question: order the two boards, close the `A0` jumper on U2 for 0x49, then re-run `d` (must measure 8/16/32/64/128/250/475/860 SPS), `a` (must read ID 0x85) and `t` (expect ~16 SPS/cell) and confirm `ADC_ID_UNKNOWN`/`ADC_RATE_MISMATCH` clear by themselves. Until then every timing figure in the firmware is derived from the measured table, so both parts work and neither is trusted from its label — including the Adafruit boards until they pass that test. |
| 14 | **DRDY is wired to GPIO 23 but the line has only ever been read pulled-up and idle** | The driver currently polls the OS bit over I2C (348 us/poll measured). DRDY would cut that to an interrupt and make the coil/ADC interlock exact rather than polled. Needs the module's DRDY actually connected and the polarity/behavior measured (open-drain, needs a pull-up) before the scheduler can depend on it. The Adafruit #1085 (decision 35) **breaks ALRT out on its header**, so once those boards are fitted this is one jumper + one pull-up per converter — not a board-modification project. Still must be measured before the scheduler trusts it, and settled against the pin budget (item 15). |
| 15 | **Display panel vs the pin budget** (new, from closing item 12): a SPI panel needs four pads (SCK/MOSI/DC/CS) and after the current map **only GPIO 17 is spare and output-capable** | The core's own SPI defaults (18/19/23) collide with I2C1 and ADS_U1_DRDY, so a panel cannot simply take `SPI.begin()` defaults. Options, none free: free the two DRDY pads by polling the OS bit instead (costs 348 us/poll and an exact interlock, item 14), take strapping pins against the §3 policy, or keep the panel on I2C and accept the frame-time cost (§12 says a >128x64 panel must not go on I2C). Must be argued with item 6 when the panel is chosen — not during bring-up at the water. |
| 16 | **The ADS1115 module stopped answering on I2C1** (`SDA held low before init`, probes returning **err 5 = timeout**, nine clocks + port re-init did not free it) | **CLOSED 2026-11-19 — not a fault.** The user confirms the dropouts were **their own intervention on the bench** (the module being unplugged/re-fitted), not a flaky connection. Nothing to fix and no maintenance action. What the episode is kept for is the diagnostic it produced: `err 5` (bus being held) vs `err 2` (address unanswered) are different hardware stories, and **a converter that has lost its supply clamps SDA/SCL through its protection diodes** — that is exactly what a disconnected or unpowered module looks like, and exactly what a converter failing mid-dive will look like in the log. Securing jumpers for the dive build stays good practice (§20 item 9, enclosure/connector work), but it is not a corrective action for anything seen so far. |
| 17 | **The fitted "BMP085" fallback module is a Bosch-family clone** (ACKs 0x77, silent 0x3B; `0xD0=0x55`, `0xD1=0x02` not 0x43, AC1=8604 not 7295) — measured 2026-11-20 by bench `b` | **OPEN.** It is not a BMP085 and is not trusted for depth: the address, the version byte and the calibration ROM all disagree with the datasheet, so its conversion behaviour is unknown. The MS5837 primary path (decision 7) is unaffected. Outstanding question, not an action: source a genuine BMP085 (or BMP280, with its own driver) for the fallback, or drop the fallback and carry only the MS5837 plus the second independent computer. Until decided, the boot probe stays 0x3E/0x3B — a clone sitting at 0x77 is deliberately invisible to the design, and `b` names it truthfully. |

---

## 21. Change history

- 2026-11-19: initial design captured. No code written yet. Decisions 1–19 logged; open items 1–11
  unresolved. Next action when out of Plan mode: create the project scaffold, then M0 and M1.
- 2026-11-19 (later): pioarduino Core 6.2.0 installed; scaffold (`platformio.ini`, `partitions.csv`,
  `lib/core`, `test/`) created; `lib/core` written — `FaultCode`, `GasMix`, `Voting`, `O2Toxicity`,
  `GasModel`, `Zhl16c`. Native tests added: **47 cases green** (`pio test -e native`).
  Section 7 corrected (it was factually wrong about the coefficient set, the He half-times and the
  GF ramp). Decisions 20–24 and risk 13 added. Defects found and fixed by the tests, all in the deco
  engine, all producing plausible-looking numbers:
  1. wrong ZH-L16 coefficient pairing + wrong GF closed form → 50 m/20 min air showed *no* deco
     obligation (fixed by porting the Subsurface `tissue_tolerance_calc`/`get_gf` form);
  2. plan search spun at the last-stop band and hit its step cap, so *every* schedule was published
     from a truncated search (`bounded` was silently true);
  3. the hold criterion at a stop used pressure epsilons that a stop depth round-tripped through
     `ambAt()`/`depthOf()` missed by 1e-5 bar, stalling the search at a rung boundary → stop
     decisions moved to depth/grid space with one 5 cm tolerance and progress made structural;
  4. the GF-low anchor was not part of restorable state, so a restored model reported ceiling 0.
  Also fixed: gas-name validation rejected `TX 21/35` (it now allows `/` and `%` while still
  rejecting markup). Reference copies of Subsurface `deco.cpp`/`deco.h`/`constants.h` were kept for
  cross-checking. No hardware code written yet; `src/`, `lib/sim` and `README.md` are still to do.
- 2026-11-19 (build started): **first ESP32 binary produced** — `pio run -e esp32` links, 457 KB flash
  (29% of the 1.5 MB app slot), 35.5 KB RAM (10.8%). Written: `src/main.cpp` (safe-state pins first:
  solenoid drive driven low before anything else; boot probe; 5 s hot re-probe that prints
  `<dev> ONLINE/OFFLINE`), `src/config/Pins.h`, `src/config/Features.h`,
  `src/sensors/BusProbe.{h,cpp}` (bus scan + stuck-SDA detection + 9-clock recovery + zero-length
  write probe), `lib/core/src/BootReport.h` (allocation-free fault table: severity escalation only,
  attempts counting, hot-plug close keeps history, arming-block query, clipped summary line).
  New native suite `test/test_boot/` — **58 cases green** in total. Toolchain traps fixed and
  documented in section 15: ADS1X15 is 2.x not 1.x, thingpulse tops out at 4.6.2, Adafruit BusIO
  needs the framework SPI library, core 3.x needs `lib_ldf_mode = deep` plus explicit
  `SPI Wire WiFi Network FS LittleFS DNSServer ESPmDNS` in `lib_deps`, and app partitions must be
  64 KB aligned (partition layout rederived: 768 KB LittleFS, not 1 MB).
  `SOLENOID_DISABLED` reclassified WARN -> **INFO**: a valve that is not fitted is a configuration,
  and a warning on every bench boot trains the reader to ignore the fault banner. It still never
  blocks arming. Section 3 pin map carries a new conflict: **GPIO 33 is assigned twice** (vibration
  motor and Page button) — `BTN_PAGE` is left unassigned in code, open item 12.
- 2026-11-19 (on hardware): **M0 gate met — firmware flashed and running on the devkit.**
  CP2102 at `/dev/cu.usbserial-0001`, chip `ESP32-D0WD-V3 (rev v3.0)`, MAC `c0:49:ef:ca:38:28`;
  upload verifies, RTS auto-reset works (no BOOT button needed). Both buses initialise at the
  documented pins, so **the pin map stands and I2C1/18-19 is not the problem** the earlier console
  spam suggested. The installed core is **Arduino core 4.0.0 (IDF 5.5, new `esp32-hal-i2c-ng`
  driver)**, not the 3.3.x earlier notes assumed — that matters because the driver's probe
  semantics differ from the legacy driver's.
  Defects found by running it, all in the boot/probe path, all of the "plausible-looking" class:
  1. **the presence probe lied**: `endTransmission(false)` with no payload returned 0 at every
     address, so an empty bench reported 16 devices per bus and `ALL SENSORS OK`. Fixed to the
     zero-length-write-with-STOP form, which the driver routes to `i2c_master_probe()` and which
     returns NACK (2) when nothing is there — decision 25, risk 14. The four transaction shapes and
     their measured return codes are recorded in `src/sensors/BusProbe.cpp`.
  2. **the probe was destructive**: the interim fix wrote 0x00 to every probed address, which clears
     the INA219 calibration register and reprograms the ADS1115 config register. Banned; the raw
     four-shape diagnostic is compiled out behind `BENCH_RAW_PROBE` (default 0).
  3. **baro addresses were wrong in code and design**: datasheets quote 8-bit forms, `Wire` takes
     7-bit — MS5837 is **0x3E**, BMP085 is **0x3B**. Sections 2, 3 and 4 corrected; README must teach
     this as a wiring mistake, because "the baro is not answering" would otherwise be chased as
     hardware forever.
  4. **stuck-SDA detection destroyed the bus it was checking**: `pinMode` on a driver-claimed pin
     detaches and deinits that Wire port (periman). Detection moved to before `begin()`;
     decision 26, risk 15.
  5. **`sizeof(g_devices)` passed as an element count** (84 bytes, not 7 devices) made the
     known-address scan walk off the end of a 7-byte array, probing 0xF5/0xF6/0xF4 garbage and
     burying the boot banner in driver errors. Now `kDevCount`.
  Boot probing is **bounded** (fitted address table only; the 112-address sweep is the `s` bench
  command), and a port that failed to init is reported as a bus fault with its devices marked
  "bus dead", never "absent". Serial bench commands: `s`/`p`/`r`/`c`. `pio test -e native` still
  58 green. Cosmetic: IDF prints `No core dump partition found!` twice per boot before `setup()`
  runs, so it cannot be silenced from `setup()`.
  **`HANDOFF.md` added at project root**: machine-specific state for continuing on another device —
  toolchain versions to reinstall, board identity and the working serial-capture recipe, the measured
  I2C probe/periman semantics table, expected empty-bench boot output, the resume point (wire one
  ADS1115 to I2C1 first, so a positive ACK is finally observed), and which files hold load-bearing
  state. The tree is now a git repo pushed to `git@github.com:ReLiCRSA/espRebreatherController.git`
  (branch `master`, initial commit `1f12e13`); `.pio/` is ignored. A fresh clone will not contain the
  planned-but-empty directories from §14 (`docs/`, `data/display/`, `src/dive/`, `src/ui/`,
  `src/web/`) — git does not track empty directories; they appear when the next file is written.





- 2026-11-19 (ADC on hardware): **the first converter is fitted and believed only as far as it was
  measured.** One ADS1115 module on I2C1 at 0x48 and one SSD1306 on I2C0 at 0x3C. M0's open caveat is
  closed — `I2C1: 1 device(s) [0x48]`, `I2C0: 1 device(s) [0x3C]`, those faults clear, `ADS1115_U2`
  stays FATAL, `[arm] REFUSED` stays correct. Written: `src/sensors/Ads1115Bus.{h,cpp}` (hand-rolled
  register driver — decision 27), `lib/core/src/AdcRegs.h` (register map + PGA/RATE tables + cell
  scaling, moved where native tests can reach it — decision 28), new suite `test/test_adc/` (9 cases).
  **`pio test -e native` is 70 green**; app 463,801 B flash (29.5%), 35,600 B RAM (10.9%). Bench
  commands `a` (converter self-test) and `d` (rate characterisation) added.
  Faults found by running it — the same "plausible-looking" class as risk 13, now twice outside the
  deco engine:
  1. **the RATE field was masked from bits 10:8**, overlapping PGA. A write that looked like 32 SPS
     set something else, and the config readback decoded as an ordinary-looking rate. Fixed to 6:4;
     the anchor is the measured power-on word `cfg=0x8583`, which only decodes as the datasheet's
     documented default (single-shot, PGA gain 2, 8 SPS) with the rate in 6:4.
  2. **MODE polarity was inverted**: bit 8 = **1 is single-shot**, 0 is continuous. Leaving it clear
     put the part in continuous mode, which is exactly why the first conversion test reported "OS bit
     stuck at 0" — and continuous mode locks the mux, so the two-pairs-per-converter scheme of §2
     would have been impossible. MODE is now always written explicitly.
  3. **the OLED raised `OLED 0x3D err 2` while a display was answering at 0x3C** — two addresses, one
     device, and the un-answered strap permanently blocked arming against working hardware. Fixed the
     way BARO was already handled.
  4. **an arming hole, not an ADC bug**: `RAIL_3V3_LOW` is FATAL and was *not* in `blocksArming()`,
     so the 3.3 V brown-out rail did not refuse an arm. §13 says "no unresolved FATAL", so the gate
     now refuses on any active FATAL regardless of the table (decision 30); OLED, `I2C_BUS_STUCK` and
     `EXTERNAL_WATCHDOG` were also missing and are listed. `test/test_boot/` grew a test that walks
     the **whole enum range** against an explicit gap list — it caught a missing `faultName` entry the
     day it was written (risk 17).
  **Hardware measurement recorded** (§2, risk 16, open item 13): the fitted module is **not a genuine
  ADS1115**. It ACKs, its config register reads and writes correctly, and it converts — but the ID
  register answers 0x00, the pointer register answers 0x00 and does not track writes, and `d` measured
  the RATE fields at **8.1/8.1/16.1/16.1/31.9/31.9/63.7/63.7 SPS** where the datasheet says
  8/16/32/64/128/250/475/860: only four rates, selected by bits 6:5, bit 4 ignored, 860 SPS
  unreachable. So the design's 32 SPS field delivers **16.1 SPS** (8 SPS per cell, not 16) and the
  coil/ADC interlock window is **62 ms, not 31 ms** (§8). Part identity is therefore established by
  *measured conversion period*, never by the ID register and never by the datasheet (decision 29) —
  an ADS1015 shares the address, the register map and every field position, and differs 100x in rate.
  Four codes carry this: `ADC_PART_WRONG` and `ADC_CONV_BAD` block arming; `ADC_ID_UNKNOWN` and
  `ADC_RATE_MISMATCH` are WARN and deliberately do **not** (§13) — the fitted unit raises both, and
  refusing a dive at the water for a cosmetic code trains a diver to override the gate. Current bench
  banner: `F:1 E:2 W:2 ADS1115_U2 BARO INA219 ADC_ID_UNKNOWN ADC_RATE_MISMATCH`, detail
  `0x48 62mS DS31mS 98%`. Also measured: config-read granularity 348-352 us/poll (why the scheduler
  should use DRDY, open item 14), and core version confirmed **Arduino 4.0.0 / IDF 5.5** from
  `esp_arduino_version.h`, with `esp32-hal-i2c-ng.c` read to source-confirm decision 25 (a
  zero-length `i2cWrite()` takes the `i2c_master_probe()` branch; any payload takes
  `i2c_master_transmit()` *and* caches a per-address device handle).

- 2026-11-19 (pin map + sourcing): two user decisions taken and logged — **the ppO2 converters shall
  be genuine TI ADS1115s** (decision 31, closes open item 13 as option (a): the fitted variant stays
  bench-only for timing work) and **GPIO 33 stays the Page button, the vibration motor moves to
  GPIO 16** (decision 32, closes open item 12).
  Chasing item 12 exposed that the "free pins" list in §3 was fiction, so the pin map was moved into
  `lib/core/src/PinMap.h` as an asserted table (decision 33, the decision-28 remedy applied to a
  different defect class) and `src/config/Pins.h` became an alias. New suite `test/test_pins/`:
  **80 native cases green** across six suites.
  **Measured on hardware, all of it by the new bench command `g`** (pad map + pad test):
  1. **the console is UART0 on GPIO1(TX)/GPIO3(RX)** — periman reports both as UART-owned, and the §3
     note "avoid 16/17, they are UART0 on most devkits" is **false on this board**. The core's own
     `doitESP32devkitV1` variant header says `TX = 1; RX = 3`. So 16/17 are genuine free pads, which
     is what made the motor move possible.
  2. **16 and 17 are output-capable and their internal pull-up/pull-down work** (pull-up reads 1,
     pull-down reads 0, driven level retained when left high-Z); **34–39 have no pulls at all** (the
     driver answers `GPIO number error`) — a switch there is an external-resistor BOM item.
  3. **GPIO 24 and 28–31 do not exist on the ESP32-D0WD-V3 die** (periman: `Invalid pin`), and
     **20, 37, 38** are die pads the WROOM-32 module does not bond out. The capability table now keeps
     those two reasons separate (`noSuchPad` vs `kDieOnly`) instead of calling both "not bonded".
  4. **After the map there is exactly one spare output-capable pad (GPIO 17)** — asserted as a number,
     so the next pin freed forces the §3 budget to be re-derived. That immediately surfaced a new open
     item (15): a SPI display needs four pads and the core's default SPI pins (18/19/23) collide with
     I2C1 and `ADS_U1_DRDY`, so the panel decision (item 6) and the pin budget have to be argued
     together, in advance.
  Also read out of `Wire.cpp` rather than guessed, and recorded in `BusProbe.cpp`: probe return codes
  **2 = NACK on address (nothing there)** vs **5 = timeout (the bus is being held)**. The difference
  earned its keep the same day — the ADS1115 module booted once with `I2C1 SDA held low`, both
  converters `err 5`, `I2C_BUS_STUCK` raised, nine clocks and a port re-init not freeing it. The boot
  report was truthful and the probe codes were read correctly; the conclusion drawn from them was
  wrong (item 16 — bench intervention, not a flaky jumper, closed in the entry below).
  `ADC_ID_UNKNOWN`/`ADC_RATE_MISMATCH` and the 62 ms conversion period
  reproduced unchanged on the module after it came back (`0x48 61mS DS31mS 98%`).
  App now 466,973 B flash (29.7%), 35,600 B RAM (10.9%); flashed and running. §2/§3/§14/§15/§16
  updated, decisions 31–33, risk 18, open items 12/13 closed with their resolutions and 15/16 added.
  Verified afterwards on the board: the converter answered again at the next boot (`0x48`, ID `0x00`,
  `61979 us` at the 32 SPS field, `98% off` — the clone's behaviour unchanged) and `g` reproduced the
  whole pad table.

- 2026-11-19 (correction, logged because the wrong inference is the lesson): **open item 16 was a
  misdiagnosis and is closed as not-a-fault.** I read the `err 5` / `SDA held low` episodes as an
  intermittent jumper and wrote up a maintenance action ("re-seat and secure the jumpers"). The user
  confirms the dropouts were **their own intervention** — the module being unplugged and re-fitted
  while working on the bench. Nothing is wrong with that hardware and there is nothing to fix.
  The same class of defect as risk 13, in a new place: **an inference that fits the evidence is not a
  finding.** The boot report was right, the probe codes were read right, and the conclusion was still
  wrong because the one variable that mattered — a hand on the bench — was never in the record. What
  the episode is kept for is the diagnostic that is real: `err 5` (bus being held) vs `err 2` (address
  unanswered) is a genuine distinction, and an unpowered device clamping the bus through its
  protection diodes is what a disconnected module looks like — and what a converter failing mid-dive
  will look like in a dive log. Going forward: when a fault report could be caused by operator action,
  say so in the report rather than assigning a defect.

- 2026-11-19 (simulator): **M1's simulator gate met — the sim profile runs headless on the board.**
  Written: `lib/core/src/SensorIf.h` (`Barometer`, `CellSource`, `ClockSource`,
  `SourceKind`/`SourceMix` — the same interfaces the real drivers will implement, which is what keeps
  the sim at the ADC/sensor layer as §11 requires), and `lib/sim/` — `SimBaro`, `SimCells`,
  `LoopModel`, `ValveModel`, `CcrControlLaw`, `SimRunner`. Each 1 s tick goes through the real stack:
  profile → SimBaro → LoopModel/ValveModel (what O2 the firmware's own pulses actually delivered) →
  SimCells (millivolts, faults injectable) → `evaluateCells()` (the real voter) → `resolveInspired()`
  → `zhl::Engine::step()` → CNS/OTU/`plan()`/`ndlMin()`. Nothing downstream is handed an answer.
  Bench command `v` runs a fixed 26-min, 6-segment profile (30 m for 8 min on air CCR, diluent flush
  at depth, GF 30/70, dual setpoint 1.30/0.70 with crossover at 21 m).
  **Measured on-target:** closed loop holds ppO2 0.70 against a 0.70 setpoint at 30 m at
  **2 pulses/min** (hand-computed demand: 1.2 surf L/min = 0.305 amb L/min at 3.96 amb, into
  0.15 amb L/pulse = 2.0 pulses/min), crossover takes it to 1.30 at the surface segment,
  diluent-flush verification PASSes (expected 0.82 bar, worst cell deviation 0.00), deco ceiling
  7.1 m / TTS 3.8 min appears on schedule at t=12 min, 136 pulses / 20.40 ambient L for the whole
  profile, CNS 4.3% / OTU 68, every line `SIM`-stamped, ~30 s wall, and the board returns to
  bench and answers `p` afterwards. New suite `test/test_sim/` (14 cases, physics anchors
  hand-computed) — **94 native cases green** across seven suites.
  Six defects found on the way, all mine; the first four by running rather than reasoning, the
  last two by auditing the capture's numbers against hand calculations — no test caught them:
  1. **stack smash**: `SimRunner` was a local in the bench command, ~28 KB (later ~145 KB) on an
     8 KB task stack — the run died in the loop task with `TG1WDT_SYS_RESET`. It is a static local
     now, and that is a rule, not a preference.
  2. **DRAM overflow**: a 512-entry history ring put the image 52 KB over. `SIM_HIST_CAPACITY` is
     per-target now — 96 snapshots on Arduino (Snapshot is 280 B; DRAM settles at ~19%), 512 on the
     host where the tests want the whole trace.
  3. **3-channel overrun**: `CellSource::readMv` writes all three channels, a caller passing a
     1-element array wrote past it. Caught by ASan (`-fsanitize=address,undefined`); the interface
     now says plainly that callers must pass full arrays.
  4. **the "hang"** — see risk 19. Two runs ended mid-profile with no watchdog, no crash, no further
     output, and the console dead. The plan search was capped, so the first suspicion (an unbounded
     loop) was wrong, and the second (platform libm noise putting a knife-edge stop comparison on
     the other side) was also wrong. The instrumentation that settled it was per-tick `micros()`
     cost plus `Engine::lastPlanSteps()`: `NDL=999` ticks cost **1.15 s on the board** against 0.5 ms
     natively, so a 26-minute profile was a 25–45 minute run and the captures were simply ending
     early. Fixed by hoisting the per-compartment decay factors out of the search loops (bit-identical,
     `expf` on the same input) and giving `ndlMin` a phased horizon. Worst tick measured on-target:
     **1145 ms → 41.5 ms**, with `pulses=250 / injected=37.50 ambL` unchanged — the physics did not
     move, only the cost. (Those gas figures were themselves wrong for a different reason — defect 5;
     cost and units were each fixed and re-measured separately, which is what kept the two effects
     separable.) A 240-min hold budget (`kMaxHoldMin`) also landed in the plan search: a
     single stop holding that long is non-progress, and it now sets `bounded`/`noProgress` instead of
     silently spending the 20k step budget.
  5. **metabolic-units defect — AGENTS.md risk 20.** Auditing the capture's mole balance by hand:
     at 30 m the loop drained 0.59 bar/min where 1.2 L/min into 8 L is 0.15 bar/min **at any depth**.
     The model multiplied a surface-quoted metabolic rate by ambient pressure. Setpoint tracking was
     impeccable; every gas figure was 4x wrong; and the sim's own test asserted the wrong rate as its
     "hand-computed" anchor, so the suite could not see it. Fixed: `metabolicRateSurfLpm` with
     `surfRefBar`, the unit convention stated in `LoopModel.h`, and the anchors re-derived — sag is
     now asserted **depth-independent** (0.396 bar after 2 min at both 10 m and 30 m, zero-mole floor
     at the hand-computed 276 s), and the closed-loop case asserts ~14 pulses over a 7-min hold
     against the hand-computed 2.15 amb L (measured 14; the old model needed ~4x). Re-measured
     on-target after the fix: 136 pulses / 20.40 amb L for the profile, bottom hold 0.70 at
     2 pulses/min, mole balance closed at the hand number.
  6. **history ring and empty-window assertions.** `hist_` stopped storing at capacity, so an
     on-target run kept only its first 96 s; and window queries over a dropped span returned `0.0f`,
     which sits inside most assertion bands — `verify()` could "pass" without ever looking at the
     run. Fixed: a true ring with `histDropped()` accounting, `setHistoryStride()` (bench `v` uses
     16: 96 x 16 s covers the whole profile), `windowCount()`, and `verify()` now reports
     `NO DATA`/`unevaluated` — `allPassed()` requires zero unevaluated items. Pinned by a new test,
     `test_history_ring_overwrites_and_reports_no_data`.
  Two lessons, both risk 13 in a new currency. **Cost is a measured quantity like timing**, and
  "bounded" is not "cheap" — `v` prints a worst-tick cost line so the number is never assumed.
  And **a trace that behaves is not a model that computes correctly**: setpoint holding hid a 4x
  units error only a mole balance could see, so every gas figure the sim prints is now quoted
  against a hand calculation (risk 20).
  App 486,109 B flash (30.9% of the 1.5 MB app slot), 63,648 B RAM (19.4%); flashed. **Both ADS1115
  modules are now fitted**
  (0x48 + 0x49 on I2C1) and **both raise the clone warnings** — `ADC_ID_UNKNOWN`/`ADC_RATE_MISMATCH`
  at 0x49 too (ID `0x00`, 61 ms conversion at the 32 SPS field), so decision 31 has two parts to
  replace, not one; `ADS1115_U2` is no longer FATAL and the bench banner is `F:0 E:2 W:2` with
  `[arm] REFUSED` now for BARO/INA219 — the report stays honest as hardware changes under it.
  Still open at M1: the alternating-mux scheduler in `src/sensors/Ads1115Bus` at the measured rate.

- 2026-11-19 (scheduler): **M1 is complete — the alternating-mux scheduler is implemented and
  measured on-target.** Written: `AdcPairWalker` in `src/sensors/Ads1115Bus.{h,cpp}` — a
  non-blocking pair-step state machine per converter (START AIN0/1 → poll OS → read → START AIN2/3
  → …), sized from the conversion period `checkPart()` timed at boot (`Ads1115::measuredConvUs()`),
  never from the datasheet rate table: the OS-bit poll cadence (10 ms), the conversion-stall
  timeout (4× sizing, floor 250 ms), and the coil/ADC interlock window (running EMA of **measured**
  in-walk periods) all come off that figure. OS bit is polled, not DRDY-interrupted (open item 14 —
  DRDY still unwired). Raw codes are stored un-inverted; per-channel `invert` is applied at
  consumption (CellArray, M2), so logs quote what the converter put on the bus. `main.cpp` fits two
  walkers — U1: cell 1 → cell 2, U2: cell 3 → coil shunt (the coil slot walks a floating input to
  keep per-cell rates honest until M5) — ticks them from `loop()`, re-begins them on converter
  hot-plug so a swapped part is sized from its own measurement, and adds bench command **`t`**
  (achieved rate, period stats, interlock window, error counters). `loop()`'s bare delay(20)/
  delay(30) heartbeat became **service windows**: same HIGH-20/LOW-30 heartbeat shape, but the
  walkers and the REPL run inside it, because at a 50 ms loop cadence the achieved rate would be
  paced by the loop, not by the converters.
  **Measured on-target** (three reads across two captures, uptimes 17 s / 60 s / 26 s): boot timed
  61,870-62,120 us at the design field; **7.21-7.23 SPS per cell achieved** at both addresses (design asks 16, the clone's
  real sizing figure is 8.05, the ~10% shortfall is the OS-poll detection lag); in-walk conversion
  periods **65,498-67,112 us, avg 66.3-66.9 ms**; interlock **busy duty 95-96%** → the incidental
  inter-conversion gap is ~2-3 ms, too short for a 20-250 ms pulse, so **decision 34**: M5's pulse
  scheduler must carve explicit conversion-free slots, not queue into incidental ones. 60 s
  continuous run: `ioErr 0, convTimeout 0, lateDetect 0`, sample ages < 200 ms, `p` responsive
  afterwards. One report bug found by running it: `periodMin` printed 0 because `begin()` seeded the
  EMA from the boot measurement, so the first-sample branch (the only min/max initialiser) never
  ran — fixed and re-measured (min 65,498 us). `a`/`d` mid-walk writes stir the counters; the report
  says so rather than pretending the run was clean.
  App 488,497 B flash (31.1%), 63,864 B RAM (19.5%). Native suite unchanged at **94 green** (`src/`
  is not built natively — that split is what makes that claim cheap to keep honest).

- 2026-11-19 (ADC procurement): **the ppO2 converters are now specified by product: 2x Adafruit
  "ADS1115 16-Bit ADC – 4 Channel with PGA" breakout, product #1085** (decision 35 — the named
  procurement that satisfies decision 31). Specs checked against that product, not assumed: the
  Adafruit product page and the Adafruit 4-Channel ADC Breakouts guide (pinouts page) were read on
  2026-11-19. What the check found:
  1. **No chip-level spec change anywhere.** The board carries the TI ADS1115 against datasheet
     SBAS444 — the same register map, PGA/RATE tables, LSB 7.8125 uV at gain 16 and power-on word
     `0x8583` that `lib/core/src/AdcRegs.h` already implements and `test/test_adc/` already asserts.
     Design values matched the datasheet before; the parts didn't. The fix was procurement, not the
     tables.
  2. **Board-level facts recorded in §2**: VIN 2.0–5.5 V (run from the 3.3 V rail); `A+`/`A-` are
     the board's ferrite-filtered analog rails — outputs, not supply inputs; address is default
     **0x48 with one back-side `A0` solder jumper giving 0x49**, which is exactly this design's
     pair — the chip's 0x4A/0x4B (ADDR→SDA/SCL) options are **not** strapped out on this board, so
     a third/fourth converter on those addresses is not an option; **ALRT/DRDY is broken out on the
     header**, which makes open item 14 a jumper + pull-up once fitted; **on-board 10k I2C pull-ups
     to VIN**, two boards ≈ 5k total on I2C1 — add none externally; power-LED cut jumper; current
     STEMMA QT and older header-only revisions are functionally identical.
  3. **Caveats kept on the record in decision 35**: identity remains a *measurement* claim, never a
     brand claim (decision 29) — the arrival acceptance test is unchanged and applies to the
     Adafruit boards like to any part: `d` measures the eight datasheet rates, `a` reads ID `0x85`,
     `ADC_ID_UNKNOWN`/`ADC_RATE_MISMATCH` clear by themselves, `t` re-derives (~31 ms conversion,
     ~16 SPS/cell expected — expectations, never sizing inputs). Decision 27 also stands: the
     Adafruit ADS1X15 library stays a bring-up cross-check at most, out of the dive path.
  4. **Decision 34 annotated**: the ~2–3 ms inter-conversion gap is inherent to the alternating-mux
     walk (the walker re-arms conversions back-to-back), not an artifact of the clones — at genuine
     rates only the window size changes (62 ms → ~31 ms), so M5's conversion-free-slot scheduling is
     required with the new boards too.
  Updated: §2 (procured-part block + address-strap table rewritten to the `A0` jumper), §15 BOM line,
  §16 M1 row, §18 decision 34 annotation + decision 35, §20 items 13/14, HANDOFF.md (header, §5b,
  §6, §7 step 1 with fit details, §8, §9). **Docs-only change: no code touched, native suite
  unchanged at 94 green.** Outstanding action stays: order the two boards, close `A0` on U2, run
  `d`/`a`/`t`.

- 2026-11-20 (baro bring-up start): **the bench's "BMP085" is a clone — found by measurement, not
  inference.** Compiled, flashed and read the boot report on request (checking for the BMP085):
  boot stays honest — `I2C0: [0x3C]`, `ERROR BARO x3 no MS5837/BMP085`, `[arm] REFUSED`. The full
  sweep (`s`) found a third device, **0x77 — on I2C1**, and it moved between I2C0 and I2C1 across
  captures (bench intervention; §20 item 16's lesson applied — stated as a fact, no fault inferred).
  A genuine BMP085 has its 7-bit address **fixed at 0x3B** (datasheet "0x77" is the 8-bit form), so
  0x77 could not be one — and "presence is not identity" demanded the register read, not the
  address guess. New bench command **`b`** (baro identity: Bosch CHIPID 0xD0, version 0xD1, cal-ROM
  AC1 at candidate addresses; writes the pointer register, so bench-only, documented in the banner):
  the module answers `0xD0=0x55` (BMP180's chip ID) but **`0xD1=0x02` where a genuine part reads the
  fixed `0x43`**, and **AC1=8604 against the BMP180 spec 7295**. Address, version byte and factory
  ROM all disagree with the datasheet: a Bosch-family clone, risk 16's defect class arrived at the
  depth sensor. Recorded in §2 and as **open item 17** (source a genuine BMP085/BMP280 for the
  fallback, or drop the fallback; the clone is deliberately invisible to the 0x3E/0x3B boot probe and
  not trusted for depth). No change to the boot probe table, the fault set, or any dive-path code.
  App 490,081 B flash (31.2%), 63,864 B RAM (19.5%); flashed and verified on the board. Native
  suite unchanged at 94 green.
