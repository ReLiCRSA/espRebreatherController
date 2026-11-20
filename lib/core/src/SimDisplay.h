#ifndef SIMDISPLAY_H
#define SIMDISPLAY_H

/*
 * SimDisplay.h — a DisplayDriver that renders to text (§12's browser-preview half).
 * Host-testable and also usable on-target for the bench console preview. ascii() paints
 * the Framebuf with '#'/' ' so a template's layout can be eyeballed in a terminal and
 * asserted in tests; M6 will serve the same frame to GET /api/display/preview.
 */

#include "DisplayDriver.h"

namespace disp {

class SimDisplay : public DisplayDriver {
public:
    SimDisplay(uint8_t* buf, uint16_t w, uint16_t h)
        : fb_(buf, w, h), w_(w), h_(h), on_(true), contrast_(255) {}

    bool init() override { on_ = true; return true; }
    void present(const Framebuf& fb) override {
        if (!on_) return;
        fb_.clear();
        if (shiftRows_) fb.shiftedInto(fb_, shiftRows_);
        else {
            for (size_t i = 0; i < fb_.byteCount() && i < fb.byteCount(); i++)
                fb_.data()[i] = fb.data()[i];
        }
    }
    DisplaySize size() const override { return DisplaySize{w_, h_}; }
    void setContrast(uint8_t c) override { contrast_ = c; }
    void pixelShift(uint8_t rows) override { shiftRows_ = rows; }
    void power(bool on) override { on_ = on; }

    const Framebuf& frame() const { return fb_; }
    uint8_t contrast() const { return contrast_; }
    bool isOn() const { return on_; }

    // ASCII preview into caller's buffer: one char per pixel + '\n' per row.
    // Need cap >= w*h + h + 1. Returns chars written.
    size_t ascii(char* out, size_t cap, char onCh = '#', char offCh = '.') const {
        size_t n = 0;
        for (uint16_t y = 0; y < h_ && n + w_ + 2 < cap; y++) {
            for (uint16_t x = 0; x < w_; x++)
                out[n++] = fb_.getPixel(x, y) ? onCh : offCh;
            out[n++] = '\n';
        }
        out[n < cap ? n : cap - 1] = '\0';
        return n;
    }

private:
    Framebuf fb_;
    uint16_t w_, h_;
    bool on_;
    uint8_t contrast_;
    uint8_t shiftRows_ = 0;
};

}  // namespace disp

#endif
