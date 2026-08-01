#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <memory>
#include <string>

namespace colorbot {

// A region-of-interest screen grabber producing a top-down BGRA image.
//
// Two backends exist because the obvious one is slow. GDI BitBlt from the
// screen device context pulls pixels back from GPU memory through the display
// driver, which measured 6.9 ms for a 256x192 region: roughly five times the
// cost of the entire detection pipeline, and enough on its own to cap the loop
// near 144 FPS. Desktop Duplication receives the desktop as a GPU texture and
// copies only the requested box into a staging buffer, which avoids the
// full-surface readback.
class ScreenCapture {
public:
    virtual ~ScreenCapture() = default;

    // Grabs the region whose top-left corner is at the given screen position.
    [[nodiscard]] virtual bool capture(int screenX, int screenY) = 0;

    [[nodiscard]] virtual const std::uint8_t* pixels() const = 0;
    [[nodiscard]] virtual int stride() const = 0;
    [[nodiscard]] virtual const char* backendName() const = 0;
};

enum class CaptureBackend {
    Automatic,  // Desktop Duplication when available, GDI otherwise.
    Gdi,
    DesktopDuplication,
};

// Throws when the requested backend cannot be created. Automatic never throws
// for backend reasons alone: it falls back to GDI and reports why through
// fallbackReason.
[[nodiscard]] std::unique_ptr<ScreenCapture> createScreenCapture(
    CaptureBackend backend,
    int width,
    int height,
    std::string& fallbackReason);

}  // namespace colorbot
