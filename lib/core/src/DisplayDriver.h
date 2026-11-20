#ifndef DISPLAYDRIVER_H
#define DISPLAYDRIVER_H

/*
 * DisplayDriver.h — §12 panel abstraction. The renderer only ever hands a concrete
 * driver a finished Framebuf; screen failure never suppresses audible/haptic alarms
 * (§13), and this interface lives in lib/core so SimDisplay and the native tests can
 * exercise the same pipeline the panel path uses.
 *
 * (§14 lists DisplayDriver under src/ui; moving the pure interface to lib/core is the
 * decision-28 pattern again — host-testable core, ESP-only implementation.)
 */

#include <cstdint>
#include "Framebuf.h"

namespace disp {

struct DisplaySize { uint16_t w, h; };

class DisplayDriver {
public:
    virtual ~DisplayDriver() {}
    virtual bool init() = 0;                    // false = panel absent/dead (a fault, not a crash)
    virtual void present(const Framebuf& fb) = 0;
    virtual DisplaySize size() const = 0;
    virtual void setContrast(uint8_t contrast) = 0;
    virtual void pixelShift(uint8_t rows) = 0;  // burn-in defence (§12), applied to next present()
    virtual void power(bool on) = 0;
};

}  // namespace disp

#endif
