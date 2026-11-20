# espRebreatherController — ESP32 CCR rebreather dive computer

**Read this first: what this is, and what it is not.**

This is development firmware for a **closed-circuit rebreather (CCR) dive computer** on an ESP32
classic, intended to drive an O2 injection solenoid and compute decompression (ZH-L16A 16
compartments, N2/He, gradient factors). **It is uncertified, unaudited, single-developer
firmware.** It is written with life-support discipline — no silent fallbacks, measured-not-
assumed timing, arming gates, watchdogs — but discipline is not certification.

**Never dive on this as your only electronics.** A second independent computer plus a hand board
is mandatory (AGENTS.md §13). Triple O2-cell voting shares one firmware and (cells 1+2) one ADC,
so it does not protect against a common-mode firmware or supply fault. The firmware must never be
the only path to gas: a **manual inject** and a **manual gas bypass that no firmware can inhibit**
are hardware requirements, not options.

The design record is `AGENTS.md` (binding: decisions §18, risks §19). Machine/bench state is in
`HANDOFF.md`. This README holds the BOM, wiring, the mistakes that were actually made here, the
power math, valve guidance, maintenance and the pre-water bench procedure.

**Build status (2026-11-20):** M0 (flash/boot/bench report) and M1 (sim + bench + alternating-mux
ADC scheduler) are done and measured on hardware. M3 (barometers, depth, power monitor) is in
progress: the BMP180 fallback is fitted and proven; the MS5837 and INA219 drivers are written and
natively tested but the parts are not fitted. The dive loop, display scenes and web server are not
written yet. Today's firmware boots into BENCH and reports hardware honestly; it does not dive.

```
pio test -e native            # 117 cases, 11 suites, no hardware needed
pio run -e esp32              # builds
pio run -e esp32 -t upload    # flash (auto-reset via RTS, no BOOT button needed)
pio device monitor            # 115200 baud
```

---

## 1. Bill of materials

| Item | Part / spec | Notes |
|---|---|---|
| Depth sensor (primary) | **TE MS5837-30BA** (Bar30 bulkhead sensor, M10) | 0–30 bar, 0.2 mbar @ OSR4096, gel-filled — immersion limit §7. **Unprocured as of 2026-11-20.** 7-bit I2C **0x76** |
| Depth sensor (fallback) | **Adafruit BMP085 breakout** (BMP180 die) | 300–1100 hPa absolute ⇒ **under 1 m of usable water column** at sea level (~1.2 m at a 978 mbar bench) — fallback for shallow/monitoring use only, never a dive depth source. 7-bit I2C **0x77**. Measured genuine on this bench (CHIPID 0x55) |
| O2 ADCs | **2x Adafruit "ADS1115 16-Bit ADC – 4 Channel with PGA", product #1085** | The named procurement (decision 31/35): identity is still proven by measurement on arrival (`d` measures the eight datasheet rates, `a` reads ID 0x85), never by the label |
| O2 cells | 3x galvanic (e.g. Alphasense O2-A2 class), annual replacement | Self-powered, ~9–13 mV in air. **Never apply a voltage to a cell lead** |
| Current/power monitor | **Adafruit INA219 breakout (#904 class, 0.1 Ω shunt)** | V_BAT + rail current, 7-bit **0x40** on I2C0. Current figures assume that 0.1 Ω — meter cross-check when fitted |
| Display | SSD1306 128x64 I2C (0x3C) today; larger panels must use **SPI**, not I2C | Frame-time rule: >128x64 never on I2C |
| Solenoid drive | Logic-level low-side MOSFET (AO3400 / AO22A4 class) | 100 Ω gate resistor, **10 kΩ gate pulldown** (fails closed on reset/watchdog bite), flyback Schottky to V_BAT |
| Coil current shunt | **10 mΩ** in coil return, read differentially on U2 AIN2/3 | Click verification — the primary safety net, not the voltage compensation |
| Cell bias network | **1 MΩ x12** (each cell lead to GND) + **10 Ω x12** series + ~100 nF per pair | Without the 1 MΩs the differential inputs rail and read plausible-looking garbage (risk 6) |
| Battery | **1S2P LiPo**, single pack, PCM protected **≥3 A** | Pick cells for pulse current ≥1 A and firm 3.4 V cutoff under 400 mA — capacity is never the constraint (math in §5) |
| Rail protection | Or-ing Schottky + **470 µF low-ESR** bulk on the 3.3 V/analog branch only | A coil kick modulating the ADC reference is measured in 10 mV cell noise |
| Hardware watchdog | TPS3840 / MAX6369 class | Fed by GPIO 14 heartbeat; it must reset the MCU **and** hold the solenoid gate low |
| Alarm | Buzzer ≥90 dB (GPIO 25) + coin vibrator (GPIO 16, transistor-driven with gate pulldown) | A screen failure must never mute them |
| Buttons | Arm A (27) + Arm B/Select (32), Manual inject (13), Page (33) | Arming is a two-button physical sequence; the web page cannot arm |
| Valve | Not yet purchased — selection table §6 | |

## 2. Pin map (asserted by `test/test_pins/` — edit `lib/core/src/PinMap.h`, never this table alone)

| Function | GPIO |
|---|---|
| I2C0 SDA / SCL — OLED 0x3C, baro 0x76/0x77, INA219 0x40 | 21 / 22 |
| I2C1 SDA / SCL — 2x ADS1115 | 18 / 19 |
| ADS1115 U1 / U2 DRDY (ALRT) | 23 / 4 |
| Solenoid drive (10k gate pulldown + flyback) | 26 |
| Arm A / Arm B (Select) | 27 / 32 |
| Manual inject (hardware bypass path) | 13 |
| Page button | 33 |
| Buzzer | 25 |
| Vibration motor | 16 |
| External watchdog heartbeat | 14 |
| Declared spare (SPI panel clock, open item 15) | 17 |

Never wire: 0/2/5/12/15 (boot strapping), 6–11 (module SPI flash path), 1/3 (this board's console
UART0), 24 and 28–31 (do not exist on the ESP32-D0WD-V3 die), 20/37/38 (die pads WROOM-32 does not
bond out). 34–39 are input-only with **no internal pulls** — a switch there needs an external
resistor (BOM item). Bench command `g` measures all of this on your board.

## 3. Wiring

All I2C addresses are **7-bit, in the form each datasheet states** — see §4 mistake 1 before
second-guessing one.

### 3a. MS5837-30BA / Bar30 (primary depth) — I2C0

Bar30 DF13 colours: **red = power, black = GND, yellow = SDA, blue = SCL.**

```
 Bar30 (MS5837-30BA)          ESP32 WROOM-32
 red   ── 3.3 V               
 black ── GND                
 yellow ── SDA (GPIO 21)      I2C0, 7-bit address 0x76
 blue  ── SCL (GPIO 22)       ("0x7C" in circulating code is the 8-bit write byte, not an address)
```

One baro on I2C0 max (0x76/0x77 clash with BMP280/BME280 is by CSB strap). The MS5837 is
confirmed by its **factory PROM read + CRC**, never by the ACK (risk 21). It is gel-filled:
**max 24 h continuous immersion, then 2 h drying** — long immersion destroys it (§7).

### 3b. BMP085/BMP180 fallback — I2C0

```
 Adafruit BMP085              ESP32
 VCC ── 3.3 V
 GND ── GND
 SDA ── SDA (GPIO 21)         7-bit 0x77  (Bosch DS Table 7: bits 1110111 + R/W)
 SCL ── SCL (GPIO 22)
```

It arms only with a permanent clamp banner quoting its **computed** ceiling (<1 m at sea level —
see §4 mistake 2). Its useful role on a dive unit is as a presence/sanity cross-check, not depth.

### 3c. Two ADS1115 (#1085) — I2C1, differential cell pairs

```
 Adafruit #1085 U1            Adafruit #1085 U2
 VIN ── 3.3 V                 VIN ── 3.3 V          (run at 3.3 V so logic matches the ESP32;
 GND ── GND                   GND ── GND            VIN accepts 2.0–5.5 V)
 SDA ── 18                    SDA ── 18
 SCL ── 19                    SCL ── 19
 (addr 0x48 as shipped)       (addr 0x49: close the back-side A0 solder jumper)
 ALRT ── GPIO 23 (+pull-up)   ALRT ── GPIO 4 (+pull-up)     (optional until item 14 is settled)
 A0/A1 = cell 1               A0/A1 = cell 3
 A2/A3 = cell 2               A2/A3 = coil shunt (10 mΩ, differential)
```

`A+`/`A-` on these boards are **ferrite-filtered analog rails — outputs, not inputs**; land the
cell networks on A0..A3. The boards carry 10 k I2C pull-ups; two boards parallel to ~5 k —
**add no external pull-ups on I2C1**. Gain 16 (±256 mV), LSB 7.8125 µV; a cell in air (~9–13 mV)
is ~1150–1660 counts. Cells 1 and 2 share U1 — a documented correlated-failure point (risk 2),
compensated by the diluent-flush check, not by voting.

### 3d. Galvanic cell inputs — per cell pair (this is the network that keeps inputs honest)

```
 cell + ──┬── 10 Ω ──── AINp (ADS1115)          galvanic cell: self-powered,
          │                                     ** NEVER apply a voltage to a cell lead **
          └── 1 MΩ ── GND                       (a wiring error here also destroys the ADS1115)
 cell − ──┬── 10 Ω ──── AINn (ADS1115)
          │
          └── 1 MΩ ── GND
 between AINp and AINn: ~100 nF (RC low-pass ~1 kΩ x 100 nF ≈ 1.6 ms)
```

Without the 1 MΩ common-mode resistors the differential inputs float, rail, and read
**plausible-looking garbage** (risk 6 — this bench proved it with floating inputs). Galvanic O2
signal polarity may be negative (reduction at the sensing electrode): a per-channel `invert`
flag is resolved on the bench, raw codes are stored as the converter put them on the bus.

### 3e. Solenoid drive stage — from the battery rail, no boost

```
 V_BAT (1S2P, 3.4–4.2 V)
   ├── coil (+)
   ├── Schottky cathode ──┐            (flyback: no diode IN the coil path — 0.3 V at 3.4 V
   └── 10 mΩ shunt ── coil (−) ──┤     is a 9% coil-voltage loss)
                                 ├── U2 AIN2 (+), AIN3 (−)  via 10 Ω + RC  → click verification
                                 └── MOSFET drain
 MOSFET: drain = coil return, source = GND
 gate: GPIO 26 ── 100 Ω ── gate,  gate ── 10 kΩ ── GND   (fails closed on reset and watchdog bite)
```

Drive law is **fixed-width voltage pulses (20–250 ms) at a variable rate**, never LEDC duty —
inductive CCR valves meter gas by pulses, not duty cycle, and pulse widths are battery-voltage
compensated. Pulse-then-release only: **a latching valve held energised is an O2 flood** (risk 7).
Never fire the coil while a converter is integrating — the interlock window is the **measured**
conversion period (62 ms on the fitted clones, ~31 ms on genuine parts — decision 34 says the
pulse scheduler must carve explicit conversion-free slots; the incidental gap is 2–3 ms).

### 3f. Power, alarms, buttons, watchdog

- INA219 (#904): V+/VIN+ to the battery+, V− to the load side of its 0.1 Ω shunt, SDA/SCL to I2C0,
  address 0x40 (A0/A1 to GND). It measures the load return path — put it where you actually want
  the battery current.
- Buzzer GPIO 25 (transistor or driver, ≥90 dB), vibrator GPIO 16 through a transistor **with a
  gate pulldown** (an internal GPIO pull is not a fail-safe).
- Buttons to GPIO with **internal pull-ups**: 27 (Arm A), 32 (Arm B/Select), 33 (Page);
  manual inject input 13. 34–39 have no pulls — external resistor required if used.
- Watchdog heartbeat out GPIO 14 to the supervisor; the supervisor's reset output must hold the
  solenoid gate low (gate pulldown does this even with firmware dead).
- Power: 1S2P pack → PCM (≥3 A) → diode-OR + 470 µF bulk on the 3.3 V/analog branch. **No boost
  stage** for the coil: it runs directly off the battery rail. Do not parallel two packs through
  diodes — one 2P pack, one PCM.

## 4. Common wiring mistakes — every entry here is a trap this project hit or measured

1. **Doing address arithmetic on a datasheet number.** This project's probe table carried phantom
   addresses 0x3E and 0x3B for a day because someone "halved" 0x7C and 0x77. Neither exists; every
   honest measurement against them produced false conclusions about a good part. The datasheets
   state 7-bit forms — MS5837: "Address (7 bits) 0x76 (1110110 b)"; BMP180: bits 1110111+R/W =
   0x77. `Wire` takes 7-bit. **Take the form the datasheet states and confirm with a full sweep
   (`s`), never with the fitted table alone.** MS5837's 0xA0–0xAC are PROM *command* bytes —
   a third trap in the same datasheet.
2. **Believing a fallback baro has depth range.** BMP085/BMP180 measure 300–1100 hPa absolute;
   against a ~1013 mbar surface that is ~87 mbar gauge — **under 1 m of water**, not the "9 m" an
   early design note asserted (a 10x arithmetic error, corrected by decision 38; test/test_depth
   pins the hydrostatics). Never let depth logic extrapolate past a part's range — the clamp +
   banner exists for exactly this.
3. **Powering a galvanic cell.** Cells are self-powered; any voltage on a cell lead destroys the
   cell and usually the ADS1115. The ADS1115 inputs are the only connection.
4. **Missing the 1 MΩ common-mode resistors** on the differential pairs — inputs rail and read
   numbers that look plausible. Measured behaviour on this bench.
5. **Both ADDR straps tied the same way** — two converters collide at 0x48. On #1085 the second
   board is 0x49 by **closing the back-side `A0` jumper**; do not hunt for a header jumper.
6. **Solenoid without flyback** (kills the FET/MCU), or **a diode in series in the coil path**
   (9% coil voltage at 3.4 V — the flyback goes drain→V_BAT, not in series).
7. **ADC2 pads for analog reads with WiFi active** — use the INA219 for battery V/I, not the ESP32
   ADC. (Digital I/O on ADC2 pads is fine.)
8. **Outputs on GPIO 34–39** — input-only, and the failure is silent: the haptic alarm just never
   fires. Asserted by `test/test_pins/` so it is a build failure, not a water-side discovery.
9. **A big display on I2C** — a 128x64 I2C frame is already ~25 ms; larger panels must use SPI
   (and note the pin budget: the core's default SPI pins 18/19/23 collide with I2C1 and DRDY).
10. **Adding I2C pull-ups on top of the #1085 boards' own** — two boards already parallel to ~5 kΩ.

## 5. Power and battery budget

- Injection energy: ~0.3 J/pulse; at full-control rates ~30 mW average. **Electronics baseline
  ~0.4 W.** Capacity is never the constraint — voltage headroom and transient droop are.
- 1S2P ~6000 mAh ≈ **50 h with WiFi off** — far past any dive. Specify cells by **pulse current
  ≥1 A and firm 3.4 V cutoff under 400 mA**, not mAh.
- **Low-voltage lockout: injection stops below ~3.4 V** (alarm, hand control to bailout) rather
  than chattering a valve on a dying rail.
- The coil kick is an analog problem, not a power problem: we are measuring 10 mV signals, so the
  or-ing Schottky, the 470 µF low-ESR bulk, the 10 mΩ shunt placement and the ADC-window interlock
  are load-bearing. Bench acceptance: fire 10 pulses at a discharged 3.4 V pack with a scope on
  the 3.3 V rail — ADS1115 readings must move <1 mV.

## 6. Valve selection (valve not yet purchased — this is the design intent)

| Parameter | Requirement | Why |
|---|---|---|
| Type | pilot-operated, normally closed, at **8–10 bar O2 intermediate pressure** | production-CCR practice; keeps the seat differential modest |
| Seat rating | **≥12 bar differential** | the seat must hold IP with margin; a leaking seat is a silent O2 addition |
| Per-pulse volume | 0.05–0.3 **ambient L** | sets pulse-rate headroom vs metabolic draw |
| Opening time | <15 ms | control-loop authority |
| Rate | ≥120 pulses/min, ≤25% duty | thermal + supply headroom |
| Service | **O2-service rated, O2-clean**; PTFE diaphragm/seat preferred over NBR/Viton | oxygen compatibility is not optional |
| Latching option | if chosen: **pulse-then-release both directions, never hold current** | a held latching valve is a stuck-open O2 flood — the worst failure mode this device has |

Solenoid faults are distinguished three ways by the 10 mΩ shunt click signature plus ppO2
response: `SOLENOID_NO_CLICK` (open coil/dead FET), `SOLENOID_SLOW_CLICK` (fouling/low voltage),
`SOLENOID_FAIL` (clicks but ppO2 unchanged — empty supply, frozen regulator). Three faults, three
diver responses.

## 7. Maintenance

- **MS5837 gel-filled sensor: max 24 h continuous immersion, then 2 h drying.** A long immersion
  destroys it. Track cumulative immersion on long live-aboard days; this is a scheduled task, not
  a memory.
- **O2 cells: replace annually** regardless of reading; a spare cell travels in the housing.
  No temperature compensation is implemented (~2%/°C) — MS5837 temperature is logged every sample
  so post-dive review can see excursions.
- Air calibration at 0.21 ppO2 / 1 bar is the required flow (900–1100 mbar ambient gate, 60 s,
  σ <1 mV, slope ≥30 mV/bar, >20% disagreement rejected). A single-point air cal proves nothing
  about linearity at 1.3–1.6 bar and a current-limited cell calibrates perfectly in air (risk 3) —
  the optional 0.98 O2 high-point check and the 6 m O2-flush bench procedure exist because of this.
  **Cal older than 24 h blocks arming.**
- Connector corrosion, moisture management and potting: humidity kills this hardware; enclosure
  design is an open item (AGENTS.md §20 item 9) — until it is done, treat every connector as a
  failure candidate and check jumpers before any water entry.
- Diluent-flush verification at depth is the independent check the diver should actually use: all
  three cells must converge on the diluent's expected ppO2 — voting cannot fake it (risk 2).

## 8. Pre-water bench procedure (run this before any water, every time)

1. Power up — boot lands in **BENCH** and is never fatal. Read the fault banner: `F:0` expected
   with fitted hardware; anything FATAL/ERROR must be explained, not skipped.
2. `p` — probe table + report: all fitted devices present, `BARO_RANGE`/`INA219` behaviour as
   fitted. `s` — full sweep if anything is unexpected (a device at an address the table forgot is
   a sweep finding, not a mystery).
3. `a` + `d` — converters: ID `0x85` and the eight datasheet rates (8/16/32/64/128/250/475/860
   SPS). Until both fitted boards pass this, they are bench parts, not dive parts (decision 31).
4. `t` — pair walkers: per-cell rate and the coil/ADC interlock window measured, zero I2C errors
   over 60 s.
5. `b` — baro identity + live conversion: MS5837 PROM/CRC + real mbar, or BMP180 CHIPID + real
   mbar agreeing with a local weather reference (**named place, hour and source** — e.g. SMHI
   Stockholm station, never a city picked by assumption). Squeeze the module: the reading MUST move.
6. `w` — INA219 config write-verify + rail voltage cross-checked against a meter.
7. `g` — pad map: nothing moved, no pad owned by two roles.
8. Cell bench checks: bias sanity (each pair within ±250 mV and not railed), air cal flow passes,
   all three cells agree within 20% in air. Diluent-flush check on the sim (`v`) or on gas.
9. Solenoid (once fitted, dry): bounded `solenoid test` N-pulse run with click verification on the
   shunt; three-fault taxonomy demonstrated with injected faults (sim `v`).
10. Arm sequence on hardware (two buttons): the checklist must refuse with any gate failing and
    pass only with all gates met (cal age <24 h, V_BAT ≥3.6 V, click self-test, no active FATAL…).
11. `v` — headless sim profile: mole-balanced pulse totals and closed-loop setpoint hold, worst
    tick cost line sane.

Then: **second independent computer + hand board on the unit, and the firmware is never the only
thing between you and gas.**
