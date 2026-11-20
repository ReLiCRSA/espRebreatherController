#ifndef FRAMEBUF_H
#define FRAMEBUF_H

/*
 * Framebuf.h — monochrome framebuffer, pure C++, host-testable (decision 28 pattern).
 *
 * Layout is SSD1306-native on purpose: page-major, byte = page * width + x, bit0 = TOP
 * pixel of an 8-pixel column. The ThingPulse SSD1306Wire buffer (the deployed driver we
 * transcribe transport from) uses exactly this layout — buffer[x + y * width()], y in
 * pages (OLEDDisplay.cpp display(), read 2026-11-20) — so present() is a memcpy when the
 * panel width matches, and the Font6x8 glyph columns blit in with no bit surgery.
 *
 * No allocation: the caller supplies storage. Max supported panel here is 256x64 (2 KB)
 * per §12's panel ladder; anything larger (colour) comes with its own driver later.
 */

#include <cstdint>
#include <cstring>
#include "Font6x8.h"

namespace disp {

class Framebuf {
public:
    // caller-owned storage, at least (w*h/8) bytes
    Framebuf(uint8_t* buf, uint16_t w, uint16_t h)
        : buf_(buf), w_(w), h_(h), pages_(h / 8) {}

    uint16_t width() const { return w_; }
    uint16_t height() const { return h_; }
    uint8_t* data() { return buf_; }
    const uint8_t* data() const { return buf_; }
    size_t byteCount() const { return (size_t)w_ * pages_; }

    void clear() { std::memset(buf_, 0, byteCount()); }

    void setPixel(int16_t x, int16_t y, bool on) {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
        uint16_t idx = (uint16_t)(y >> 3) * w_ + (uint16_t)x;
        uint8_t mask = (uint8_t)(1u << (y & 7));
        if (on) buf_[idx] |= mask;
        else    buf_[idx] &= (uint8_t)~mask;
    }

    bool getPixel(int16_t x, int16_t y) const {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return false;
        return (buf_[(uint16_t)(y >> 3) * w_ + (uint16_t)x] >> (y & 7)) & 1u;
    }

    // Draw one glyph at (x, row*8). Rows must be 8-aligned (the template grid guarantees
    // it; anything else would need bit-shifting across page bytes and is deliberately
    // not supported). Returns true if fully inside, false if clipped at the right edge.
    bool drawGlyph(int16_t x, int16_t row, char ch) {
        uint8_t c = (uint8_t)ch;
        const uint8_t* g = (c >= font::kFirstChar && c <= font::kLastChar)
                             ? font::kGlyphs[c - font::kFirstChar]
                             : font::kGlyphs['?' - font::kFirstChar];
        bool clipped = false;
        for (uint8_t col = 0; col < font::kCellCols; col++) {
            int16_t px = x + col;
            if (px >= w_) { clipped = true; break; }
            if (px < 0) continue;
            uint8_t bits = (col < font::kGlyphCols) ? g[col] : 0;
            buf_[(uint16_t)row * w_ + (uint16_t)px] = bits;  // row is a page index
        }
        return !clipped;
    }

    int16_t textWidth(const char* s) const {
        int16_t n = 0;
        while (*s++) n += font::kCellCols;
        return n;
    }

    // Left-aligned draw starting at x on 8-px `row`. Stops at the right edge.
    // Returns true if the whole string fit.
    bool drawText(int16_t x, int16_t row, const char* s) {
        bool fits = true;
        int16_t cx = x;
        for (const char* p = s; *p; p++) {
            if (cx >= w_) { fits = false; break; }
            drawGlyph(cx, row, *p);
            cx += font::kCellCols;
        }
        return fits;
    }

    void hline(int16_t x, int16_t y, int16_t len) {
        for (int16_t i = 0; i < len; i++) setPixel(x + i, y, true);
    }
    void vline(int16_t x, int16_t y, int16_t len) {
        for (int16_t i = 0; i < len; i++) setPixel(x, y + i, true);
    }
    void rect(int16_t x, int16_t y, int16_t w, int16_t h) {
        hline(x, y, w); hline(x, y + h - 1, w);
        vline(x, y, h); vline(x + w - 1, y, h);
    }
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h) {
        for (int16_t j = 0; j < h; j++) hline(x, y + j, w);
    }

    // Gauge per §12's `bar` primitive: outlined box with `frac` (0..1) filled inside.
    void gauge(int16_t x, int16_t row, int16_t w, double frac) {
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        int16_t h = 7;  // fits inside one 8-px text row
        rect(x, row * 8, w, h);
        int16_t inner = w - 2;
        int16_t fill = (int16_t)(frac * inner + 0.5);
        if (fill > 0) fillRect(x + 1, row * 8 + 1, fill, h - 2);
    }

    // Vertical wrap-shift of this frame into `dst` by `rows` pixel rows (positive = up).
    // This is how pixelShift() (§12 burn-in defence) is implemented without relying on a
    // remembered controller command — fully native-testable instead.
    void shiftedInto(Framebuf& dst, int16_t rows) const {
        dst.clear();
        for (int16_t y = 0; y < h_; y++) {
            for (int16_t x = 0; x < w_; x++) {
                if (!getPixel(x, y)) continue;
                int16_t ny = y - rows;
                if (ny < 0) ny += h_;
                if (ny < dst.h_) dst.setPixel(x, ny, true);
            }
        }
    }

private:
    uint8_t* buf_;
    uint16_t w_, h_, pages_;
};

}  // namespace disp

#endif
