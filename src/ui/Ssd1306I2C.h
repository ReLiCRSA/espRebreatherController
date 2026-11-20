#ifndef SSD1306I2C_H
#define SSD1306I2C_H

/*
 * Ssd1306I2C.h — I2C SSD1306 128x64 panel transport (src/ui implementation of the pure
 * disp::DisplayDriver interface in lib/core). The renderer never talks to a panel; it
 * hands this driver a finished Framebuf.
 *
 * PROVENANCE (decision 37 pattern — transport transcribed from the deployed library,
 * not from memory): thingpulse/ESP8266 and ESP32 OLED driver for SSD1306 displays
 * ^4.6.2, installed at .pio/libdeps/esp32/... and read 2026-11-20:
 *  - SSD1306Wire(address, sda, scl, geometry, i2cBus, frequency): "sda = -1 means
 *    don't change pins, someone else has called begin for us" — that is exactly our
 *    situation: I2C0 (Wire, GPIO 21/22) is owned by BusProbe/baro/INA219, and touching
 *    those pads any other way would deinit the port (decision 26 / risk 15).
 *  - _frequency is applied with _wire->setClock() — this raises the WHOLE I2C0 clock.
 *    We ask 400000: §12's frame-cost figure (~25 ms for 1024 B) is a 400 kHz figure,
 *    and the co-resident baro parts are 400 kHz-rated (BMP180 DS, MS5837 DS). Stated,
 *    not hidden, because a bus-wide change deserves a sentence.
 *  - OLEDDisplay::buffer is a protected uint8_t* of displayBufferSize bytes, laid out
 *    page-major as buffer[x + y * width()] with y in pages (display(), OLEDDisplay.cpp)
 *    — byte-identical to disp::Framebuf's documented layout, so present() is a memcpy.
 *  - init() = allocateBuffer + sendInitCommands + resetDisplay (OLEDDisplay.cpp).
 *  - setContrast(contrast, precharge=241, comdetect=64), displayOn()/displayOff().
 * §15's decision-27 "hand-rolled, library stays out" rule is a SENSOR-path rule; §12
 * does not demand a hand-rolled OLED driver, and the geometry/init sequences are the
 * riskiest part of this path to write from memory (risk 21). So the library is the
 * transport and our code owns layout, fit, validation, and the buffer.
 *
 * Failure policy: init() returning false is a reported fault, never a crash, and the
 * dive task never depends on the panel (§13: screen failure never mutes alarms).
 *
 * Known caveat to settle at first render: generic "128x64 I2C" modules sometimes carry
 * an SH1106 clone (132-column controller, image offset 2). The library ships an
 * SH1106Wire fallback in the same package; if the panel at 0x3C paints garbled or
 * shifted, that is the drop-in to try, and bench 'o' + the ASCII preview is how you
 * tell software (wrong layout) from controller (wrong init) before touching wiring.
 */

#include <Arduino.h>
#include <cstring>

#include <SSD1306Wire.h>  // thingpulse, see lib_deps

#include "DisplayDriver.h"
#include "Framebuf.h"

namespace ui {

class Ssd1306I2C : public SSD1306Wire, public disp::DisplayDriver {
public:
    static constexpr uint16_t kCols = 128;
    static constexpr uint16_t kRows = 64;

    explicit Ssd1306I2C(uint8_t address = 0x3C)
        // sda/scl -1: we do NOT own the bus — Wire (GPIO 21/22) is begun by the boot
        // probe (AGENTS.md 3). i2cBus is then irrelevant; _wire is only used by
        // connect(), which skips begin() because _sda == -1.
        : SSD1306Wire(address, -1, -1, GEOMETRY_128_64, I2C_ONE, 400000),
          fb_(buf_, kCols, kRows), shifted_(shiftBuf_, kCols, kRows) {}

    bool init() override {
        if (!SSD1306Wire::init()) return false;  // alloc + init cmds + reset (provenance above)
        // Geometry guard, not an assumption: if the library's buffer does not match our
        // Framebuf byte-for-byte, present() could write past it. Fail instead.
        if (displayBufferSize != fb_.byteCount()) return false;
        clear();
        displayOn();
        return true;
    }

    void present(const disp::Framebuf& fb) override {
        // Uninitialised (buffer NULL) or geometry-mismatched driver must never memcpy:
        // present() on a dead panel is a no-op, and the fault path says so upstream.
        if (buffer == nullptr || displayBufferSize != fb.byteCount()) return;
        if (shiftRows_ != 0) {
            // Burn-in defence (§12) as a software wrap of the frame — fully native-tested
            // (Framebuf::shiftedInto), no remembered controller command involved.
            fb.shiftedInto(shifted_, shiftRows_);
            std::memcpy(buffer, shifted_.data(), displayBufferSize);
        } else {
            std::memcpy(buffer, fb.data(), displayBufferSize);
        }
        display();  // SSD1306Wire::display(): page-addressed write of the whole buffer
    }

    disp::DisplaySize size() const override { return disp::DisplaySize{kCols, kRows}; }

    void setContrast(uint8_t contrast) override { SSD1306Wire::setContrast(contrast); }

    void pixelShift(uint8_t rows) override { shiftRows_ = rows; }

    void power(bool on) override { on ? displayOn() : displayOff(); }

private:
    uint8_t buf_[kCols * kRows / 8];
    uint8_t shiftBuf_[kCols * kRows / 8];
    disp::Framebuf fb_;      // owned fallback frame; present() consumes the caller's
    disp::Framebuf shifted_; // staging buffer for pixelShift
    uint8_t shiftRows_ = 0;
};

}  // namespace ui

#endif
