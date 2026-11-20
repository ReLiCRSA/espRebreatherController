#ifndef BUTTONS_H
#define BUTTONS_H

/*
 * Buttons.h — debounced button scan for the panel/display path (M7) and later the
 * two-button arm sequence (M9, §13). Pads come from lib/core/src/PinMap.h — the same
 * asserted table test/test_pins/ walks (decision 33), not a second copy of the numbers.
 *
 * All three pads (27/32/33) have internal pull-ups on this die (measured, §3), so INPUT_PULLUP
 * is correct and idle-high/pressed-low is the polarity. Note 32/33 are also the 32.768 kHz
 * crystal pads (§3) — accepted there for buttons, and it forecloses adding that crystal later.
 *
 * This is intentionally a tiny edge-detector with a millis() debounce, no allocation, no ISR
 * (a GPIO ISR firing into the dive task is a can of worms M9 should open deliberately, not
 * as a side effect of M7).
 */

#include <Arduino.h>

#include "PinMap.h"

namespace ui {

enum class Button : uint8_t { None, ArmA, ArmB, Page };

struct ButtonScan {
    uint32_t debounceMs = 30;
    uint32_t lastChange[4] = {0, 0, 0, 0};  // indexed by Button
    bool lastState[4] = {true, true, true, true};  // idle HIGH with pull-up

    void begin() {
        pinMode(pins::BTN_ARM_A, INPUT_PULLUP);
        pinMode(pins::BTN_ARM_B, INPUT_PULLUP);
        pinMode(pins::BTN_PAGE, INPUT_PULLUP);
    }

    // Returns one newly-pressed button per call (edges are rare; the caller polls at
    // display cadence, ~5 Hz, which bounds press latency to 200 ms — fine for paging,
    // and M9's arming sequence will sample at dive-loop rate through this same table).
    Button poll() {
        const int pads[4] = {-1, pins::BTN_ARM_A, pins::BTN_ARM_B, pins::BTN_PAGE};
        const uint32_t now = millis();
        for (uint8_t b = 1; b <= 3; b++) {
            const bool raw = digitalRead(pads[b]);  // LOW = pressed
            if (raw == lastState[b]) continue;
            if (now - lastChange[b] < debounceMs) continue;
            lastChange[b] = now;
            lastState[b] = raw;
            if (!raw) return static_cast<Button>(b);  // press edge only
        }
        return Button::None;
    }
};

}  // namespace ui

#endif
