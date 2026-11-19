// Compile-time feature switches (AGENTS.md 15). Defaults are the safe ones: nothing that can
// move a valve or radiate is enabled by default.
#pragma once

// Solenoid control. False means the GPIO is never touched and the loop only reports what it
// *would* inject (AGENTS.md 8). Deliberately opt-in.
#ifndef HAVE_SOLENOID
#define HAVE_SOLENOID 0
#endif

// Bench mode is the boot state. A unit that comes up ready to arm is a unit that can arm
// itself on a bench with no cells fitted.
#ifndef BENCH_MODE
#define BENCH_MODE 1
#endif

// Power rail topology, reported in /api/health and used by the coil voltage compensation.
// A string, not a bare token, so `-DPOWER_RAIL=...` can never be pasted into an expression.
#ifndef POWER_RAIL
#define POWER_RAIL "1S2P"
#endif

// WiFi during a dive. Off by default: battery, radio energy in the loop, and the OLED is the
// in-water display (AGENTS.md 19 decision 19).
#ifndef WIFI_DURING_DIVE
#define WIFI_DURING_DIVE 0
#endif

// Raw I2C transaction diagnostic. Off by default and it should stay that way on a unit with
// sensors fitted: the diagnostic *writes* 0x00 to probed addresses, which clears the INA219
// calibration register and reprograms the ADS1115 config register (AGENTS.md 19, new risk).
// Build the bench image with -DBENCH_RAW_PROBE=1 for bring-up work on a bare bus.
#ifndef BENCH_RAW_PROBE
#define BENCH_RAW_PROBE 0
#endif
