// screenshot.h — captures the D3D9 backbuffer to a PNG/JPEG and base64-encodes it.
// Must be called on the render thread (from inside the Present hook) with a live
// device pointer.
#pragma once

#include <string>
#include <cstdint>

struct IDirect3DDevice9;

namespace mcp {

struct ScreenshotRequest {
    std::string format = "jpeg"; // "jpeg" or "png"
    int quality = 80;            // jpeg quality 1-100
    // Optional crop region in pixels (w/h <= 0 means full frame).
    int x = 0, y = 0, w = 0, h = 0;
    // Optional downscale factor in (0,1]; 1.0 = native resolution.
    double scale = 1.0;
};

struct ScreenshotResult {
    bool ok = false;
    std::string error;
    int width = 0;
    int height = 0;
    std::string format;
    std::string base64; // encoded image bytes
};

ScreenshotResult CaptureBackbuffer(IDirect3DDevice9* device, const ScreenshotRequest& req);

} // namespace mcp
