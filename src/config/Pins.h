// Pin map. See AGENTS.md section 3 and lib/core/src/PinMap.h.
//
// This header is deliberately an alias, not a second copy of the numbers. The capability table and
// the role assignment live in lib/core/src/PinMap.h, where `pio test -e native` can walk them —
// which is the only reason the two defects this map used to carry are gone:
//   * GPIO 33 was assigned to the vibration motor and the Page button at the same time. Resolved
//     2026-11-19: 33 stays the Page button, the vibrator moves to GPIO 16 (decision 32). A shared
//     pin fails in a way nobody notices until a motor back-biases a button read, so it gets tested
//     (test/test_pins/), not commented.
//   * 16/17 were written off as "UART0 on most devkits". On this board the console is GPIO1(TX) /
//     GPIO3(RX) — measured by bench command `g`, and the core's own variant header for
//     esp32doit-devkit-v1 says TX=1, RX=3 — so 16/17 are real free pads, and 16/17/17-plus-SPARE
//     are the only non-strapping output-capable ones left.
//   * The die has 40 GPIOs; this module bonds out far fewer. 20, 24, 28..31, 37 and 38 do not exist
//     on the headers, and 6..11 are inside the module's SPI flash path.
//
// Drivers must still check `>= 0` before pinMode: a role is allowed to be unassigned on purpose.
#pragma once

#include "PinMap.h"
