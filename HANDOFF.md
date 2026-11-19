# HANDOFF.md — session transfer snapshot

**Written:** 2026-11-19, at the point where the firmware is flashed and running on real hardware.
**Purpose:** everything needed to continue this project on a different machine. `AGENTS.md` is still
the binding design record (decisions §18, risks §19, milestones §16); this file holds the
*machine-specific and hardware-measured* state that `AGENTS.md` does not, plus the immediate
resume point. Read `AGENTS.md` first, this second.

---

## 1. Project state in one paragraph

ESP32 classic CCR rebreather dive computer. `lib/core` (deco engine, voting, gas model, boot
report) is complete and regression-anchored: **58 native cases green**. `src/` boots on the board:
safe-state pins first, bounded I2C presence probing, honest boot fault report, arming refusal on an
empty bench, hot re-probe every 5 s, serial bench commands. **M0 is DONE. M1 is half done** (boot +
bench verified on hardware; `lib/sim` and the ADS1115 driver not written). Nothing is wired yet —
the devkit is bare, and the probe has only ever seen *negative* ACK (NACK = 2).

**Transfer: the tree is now a git repo, pushed to GitHub.** Clone on the new machine rather than
copying the directory:

```
git clone git@github.com:ReLiCRSA/espRebreatherController.git    # or the https form
cd espRebreatherController
```

`.pio/` is gitignored (build cache, ~72 MB) — the new machine rebuilds it on first `pio run`. The
initial commit is `1f12e13`, branch `master`, and it contains exactly the state described in this
file: 28 files, 4,379 insertions. Nothing else is on the remote; if `git status` is not clean on
this machine, uncommitted work exists only here.

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
| IDF | 5.5.x, precompiled libs | `ls ~/.platformio/packages/framework-arduinoespressif32-libs/esp32/lib` |
| esptool | 5.6.0, available as `python -m esptool` inside the venv | `python -m esptool version` |
| pyserial | installed in the venv (used for serial capture, §4) | `python -c "import serial; print(serial.__version__)"` |

If `pyserial` is missing on the new machine: `pip install pyserial` inside the venv.

Commands (all from the project root, venv sourced):

```
pio test -e native            # 58 cases, four suites, ~3 s, no hardware needed
pio run -e esp32              # links; 458,633 B flash (29.2%), 35,552 B RAM (10.8%)
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

Send bench commands by writing to the same port (`s.write(b's\n')`) between reads.

Console noise to expect: **`esp_core_dump_flash: No core dump partition found!` twice per boot**.
It is cosmetic (no core dump partition by design — no flash budget) and it is printed before
`setup()` runs, so `esp_log_level_set()` from `setup()` cannot silence it. Do not chase it.

## 5. Measured facts about this toolchain's I2C — do not re-derive, do not regress

Arduino core 4.0.0 uses the **new IDF-5 driver** (`cores/esp32/esp32-hal-i2c-ng.c`), not the legacy
one. Its semantics were measured on this board and are recorded in
`src/sensors/BusProbe.cpp`; they are also AGENTS.md decisions 25–26 and risks 14–15. The numbers:

| Probe shape | Result on a bare bus | Verdict |
|---|---|---|
| `beginTransmission(a); endTransmission(false)` | **0 at every address** | unusable — it is what made an empty bench report `ALL SENSORS OK` |
| `beginTransmission(a); endTransmission(true)` | **2** (`ESP_ERR_INVALID_RESPONSE`, NACK address) | **this is the probe** — routed to `i2c_master_probe()`, writes nothing |
| `beginTransmission(a); write(0x00); endTransmission(true)` | 4 | **banned**: writes 0x00 to a fitted device (INA219 calibration cleared, ADS1115 config reprogrammed) |
| `requestFrom(a, 1)` | 0 bytes + driver error line | cannot prove absence (SSD1306 is write-only), spams the console |

Other measured facts:

- `Wire1` on **GPIO 18/19 initialises fine** (`i2cInit` returns OK, `i2cIsInit(1)=1`). The earlier
  "I2C1 is broken" suspicion was a software defect, not hardware. **The pin map in AGENTS.md §3
  stands unchanged.**
- `pinMode()` on a pin owned by a live Wire port goes through periman and **deinits that port**
  (`i2cIsInit` -> 0, later transactions report `bus is not initialized`). Stuck-SDA checks are only
  legal before `begin()` or after `end()`.
- periman bus types observed on I2C pins after init: `34` = `ESP32_BUS_TYPE_I2C_MASTER_SDA`,
  `35` = `..._SCL` (`perimanGetPinBusType()` is a useful diagnostic).
- Address conventions: `Wire` takes **7-bit**. MS5837 = **0x3E**, BMP085 = **0x3B** (datasheets quote
  the 8-bit write forms 0x7C / 0x77). ADS1115 0x48/0x49, SSD1306 0x3C/0x3D, INA219 0x40 are already
  7-bit. A probe at 0x7C finds nothing forever.
- `CONFIG_LOG_DEFAULT_LEVEL` / `CONFIG_ARDUHAL_LOG_DEFAULT_LEVEL` are both **1 (ERROR)** in this
  core, so the driver's ERROR lines cannot be filtered below ERROR at runtime — the fix for console
  spam is bounded probing, not log suppression.
- Boot probing discipline: fitted address table only (~7 transactions/bus). The 112-address sweep
  is the `s` bench command; on a port that failed to init it emitted ~200 error lines and buried
  the banner.

## 6. What is on the bench right now

- ESP32 Doit devkit V3, **nothing connected to its GPIO** (bare board over USB).
- Firmware currently flashed = current `src/` at HEAD of this session. Boot output on an empty
  bench, verbatim expected:

```
CCR dive computer — boot (never fatal), entering BENCH
[i2c] I2C0 sda=21 scl=22 ok
[i2c] I2C1 sda=18 scl=19 ok
  I2C0: 0 device(s)  []
  I2C1: 0 device(s)  []
  FATAL ADS1115_U1             x1  0x48 err 2
  FATAL ADS1115_U2             x1  0x49 err 2
  ERROR BARO                   x3  no MS5837/BMP085
  ERROR OLED                   x2  0x3D err 2
  ERROR INA219                 x1  0x40 err 2
[boot] F:2 E:3 W:0 ADS1115_U1 ADS1115_U2 BARO OLED INA219
[arm] REFUSED — see faults above
[o2] solenoid disabled — GPIO 26 will not be driven
bench commands: s=full bus scan  p=probe table + report  r=raw probe codes (destructive, bench only)  c=bus recovery
```

  If the new machine shows anything else (phantom devices, `bus is not initialized` spam, `ALL
  SENSORS OK` with nothing wired), something regressed — start at `src/sensors/BusProbe.cpp`.

- Bench commands over serial: `s` full 0x08..0x77 sweep · `p` probe table + fault report ·
  `r` raw four-shape diagnostic (needs a build with `-DBENCH_RAW_PROBE=1`, and it **writes 0x00** to
  whatever ACKs — bare bus only) · `c` 9-clock bus recovery + port re-init.

## 7. Resume point — the next three steps, in order

1. **Close the M0 caveat and validate the probe's positive direction:** wire **one ADS1115 to I2C1**
   (VDD 3.3, SDA 18, SCL 19, ADDR -> GND for 0x48). `p` must then report `0x48 err 0` and
   `I2C1: 1 device(s) [0x48]`, and `ADS1115_U1` must clear to `INFO`-closed history with
   `INFO: ADS1115 U1 ONLINE @0x48` on the hot-reprobe cycle. Until this is seen, nothing in the
   sensing chain has ever been observed to *succeed*.
2. **`lib/sim`**: `SimBaro`, `SimCells`, `LoopModel`, `FaultInjector`, `ProfileRunner` — pure C++ in
   `lib/` so `pio test -e native` covers it, implementing the same `Barometer`/`CellArray` source
   interfaces as the real drivers (AGENTS.md §11: simulate at the sensor layer, closed loop against
   the firmware's own commanded pulses, stamp everything `SIM`).
3. **`src/sensors` drivers**: `Ads1115Bus` (alternating single-shot AIN0/1 -> AIN2/3 at SPS 32,
   per-channel `invert`, DRDY on 23/4), `Ms5837` @0x3E, `Bmp085` @0x3B, `Ina219` @0x40, `Depth`
   with learned `Psurf`.

After that: `src/dive`, `src/ui`, `src/web`, `README.md` (the README content requirements in
AGENTS.md §15 are a user requirement, including the 7-bit-vs-8-bit address trap as a listed wiring
mistake).

## 8. Files that hold load-bearing state

| File | What it pins down |
|---|---|
| `AGENTS.md` | binding design record; §16 milestone status, §18 decisions 1–26, §19 risks 1–15, §20 open items 1–12 |
| `HANDOFF.md` (this file) | machine + bench state, measured I2C semantics, resume point |
| `src/sensors/BusProbe.cpp` | the measured four-shape probe table and the periman trap, in comments; the only probe form that is truthful and non-destructive |
| `src/sensors/BusProbe.h` | probe contract, 7-bit address warning, "port must be down" rule for line checks |
| `src/main.cpp` | boot order (solenoid safe-state first), bounded probing, bus dead-vs-absent reporting, 5 s hot re-probe and bus retry, bench command REPL |
| `src/config/Pins.h` | pin map; **`BTN_PAGE = -1`** because AGENTS.md §20 open item 12 is unresolved (GPIO 33 claimed by both the vibration motor and the Page button) — needs the user's choice, do not silently share a pin |
| `src/config/Features.h` | defaults: `HAVE_SOLENOID 0`, `BENCH_MODE 1`, `WIFI_DURING_DIVE 0`, `BENCH_RAW_PROBE 0`, `POWER_RAIL "1S2P"` |
| `platformio.ini` | pioarduino `stable` platform (never the stock `espressif32`), `lib_ldf_mode = deep` + `lib_extra_dirs` + explicit core libs in `lib_deps`, `native` env with `test_build_src = no` |
| `partitions.csv` | 64 KB-aligned app slots, `data, littlefs, 0x320000, 0xC0000` |
| `lib/core/src/Zhl16c.cpp` | deco engine: ZH-L16A coefficients + Subsurface GF/ceiling form, plan search in depth/grid space, `bounded` flag, GF anchor in restorable state |
| `test/test_boot/test_boot.cpp` | pins fault severities and the rule that an absent solenoid is INFO and must **not** block arming |

## 9. Open items that block code, not just BOM

- **Open item 12** (GPIO 33 double-assignment): blocks the button/motor drivers. Needs a free pin
  chosen (35 is input-only and could work if it is not used as a button) or the motor moved.
- Everything else in AGENTS.md §20 (valve choice, pulse volumes, metabolic rate, display panel,
  enclosure) blocks BOM/calibration but not the next code steps.

## 10. Standing rules worth repeating to a fresh session

- This is **primary dive life-support firmware**, uncertified. Prefer explicit failure, no silent
  fallbacks. Boot is never fatal; fatality applies at the arming gate and to mid-dive ADC dropout.
- The failure mode of this codebase is **plausible-looking numbers** (risk 13): every deco defect
  found so far looked reasonable. Any deco change: `pio test -e native` **and** a cross-check
  against a trusted computer at the specific claimed profile.
- Never write to I2C devices as a side effect of probing; never `pinMode` a live bus pin.
- Pure C++ goes in `lib/` (host-buildable); anything touching a pin, peripheral or socket goes in
  `src/` and is not compiled natively.
