#include "screenshot.h"
#include "common.h"

#include <d3d9.h>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../third_party/stb_image_write.h"

namespace mcp {

namespace {
    const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string Base64(const unsigned char* data, size_t len) {
        std::string out;
        out.reserve(((len + 2) / 3) * 4);
        size_t i = 0;
        for (; i + 2 < len; i += 3) {
            uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
            out.push_back(kB64[(n >> 18) & 63]);
            out.push_back(kB64[(n >> 12) & 63]);
            out.push_back(kB64[(n >> 6) & 63]);
            out.push_back(kB64[n & 63]);
        }
        if (i < len) {
            uint32_t n = data[i] << 16;
            if (i + 1 < len) n |= data[i + 1] << 8;
            out.push_back(kB64[(n >> 18) & 63]);
            out.push_back(kB64[(n >> 12) & 63]);
            out.push_back((i + 1 < len) ? kB64[(n >> 6) & 63] : '=');
            out.push_back('=');
        }
        return out;
    }

    void StbWrite(void* ctx, void* data, int size) {
        auto* vec = static_cast<std::vector<unsigned char>*>(ctx);
        vec->insert(vec->end(), static_cast<unsigned char*>(data),
                    static_cast<unsigned char*>(data) + size);
    }
}

ScreenshotResult CaptureBackbuffer(IDirect3DDevice9* device, const ScreenshotRequest& req) {
    ScreenshotResult res;
    if (!device) { res.error = "no device"; return res; }

    IDirect3DSurface9* backbuffer = nullptr;
    IDirect3DSurface9* resolved = nullptr; // non-MSAA copy we can read back
    IDirect3DSurface9* sysmem = nullptr;
    HRESULT hr;

    hr = device->GetRenderTarget(0, &backbuffer);
    if (FAILED(hr) || !backbuffer) { res.error = "GetRenderTarget failed"; return res; }

    D3DSURFACE_DESC desc;
    backbuffer->GetDesc(&desc);

    // GMod's backbuffer is typically multisampled, and GetRenderTargetData cannot
    // read an MSAA surface directly. Resolve through a fresh non-MSAA render target
    // with StretchRect first (this is also a cheap copy for the non-MSAA case).
    hr = device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                                    D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr);
    if (FAILED(hr) || !resolved) {
        backbuffer->Release();
        res.error = "CreateRenderTarget (resolve target) failed";
        return res;
    }
    hr = device->StretchRect(backbuffer, nullptr, resolved, nullptr, D3DTEXF_NONE);
    backbuffer->Release();
    if (FAILED(hr)) {
        resolved->Release();
        res.error = "StretchRect resolve failed";
        return res;
    }

    hr = device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                             D3DPOOL_SYSTEMMEM, &sysmem, nullptr);
    if (FAILED(hr) || !sysmem) {
        resolved->Release();
        res.error = "CreateOffscreenPlainSurface failed";
        return res;
    }

    hr = device->GetRenderTargetData(resolved, sysmem);
    resolved->Release();
    if (FAILED(hr)) {
        sysmem->Release();
        res.error = "GetRenderTargetData failed";
        return res;
    }

    D3DLOCKED_RECT lr;
    hr = sysmem->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) { sysmem->Release(); res.error = "LockRect failed"; return res; }

    const int srcW = static_cast<int>(desc.Width);
    const int srcH = static_cast<int>(desc.Height);

    // Resolve crop region (clamped to frame).
    int cx = req.x < 0 ? 0 : req.x;
    int cy = req.y < 0 ? 0 : req.y;
    int cw = (req.w > 0) ? req.w : srcW - cx;
    int ch = (req.h > 0) ? req.h : srcH - cy;
    if (cx >= srcW) cx = srcW - 1;
    if (cy >= srcH) cy = srcH - 1;
    if (cx + cw > srcW) cw = srcW - cx;
    if (cy + ch > srcH) ch = srcH - cy;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;

    double scale = req.scale;
    if (scale <= 0.0 || scale > 1.0) scale = 1.0;
    int dstW = (std::max)(1, static_cast<int>(cw * scale));
    int dstH = (std::max)(1, static_cast<int>(ch * scale));

    // The backbuffer format A8R8G8B8 / X8R8G8B8 stores pixels as B,G,R,A in memory.
    // Emit 3-channel RGB (smaller; alpha is meaningless for a screenshot).
    std::vector<unsigned char> rgb(static_cast<size_t>(dstW) * dstH * 3);
    const auto* base = static_cast<const unsigned char*>(lr.pBits);

    for (int dy = 0; dy < dstH; ++dy) {
        int sy = cy + static_cast<int>(dy / scale);
        if (sy >= srcH) sy = srcH - 1;
        const unsigned char* srcRow = base + static_cast<size_t>(sy) * lr.Pitch;
        unsigned char* dstRow = &rgb[static_cast<size_t>(dy) * dstW * 3];
        for (int dx = 0; dx < dstW; ++dx) {
            int sx = cx + static_cast<int>(dx / scale);
            if (sx >= srcW) sx = srcW - 1;
            const unsigned char* p = srcRow + static_cast<size_t>(sx) * 4;
            dstRow[dx * 3 + 0] = p[2]; // R
            dstRow[dx * 3 + 1] = p[1]; // G
            dstRow[dx * 3 + 2] = p[0]; // B
        }
    }

    sysmem->UnlockRect();
    sysmem->Release();

    std::vector<unsigned char> encoded;
    int wrote;
    if (req.format == "png") {
        wrote = stbi_write_png_to_func(&StbWrite, &encoded, dstW, dstH, 3, rgb.data(), dstW * 3);
        res.format = "png";
    } else {
        int q = req.quality; if (q < 1) q = 1; if (q > 100) q = 100;
        wrote = stbi_write_jpg_to_func(&StbWrite, &encoded, dstW, dstH, 3, rgb.data(), q);
        res.format = "jpeg";
    }
    if (!wrote || encoded.empty()) { res.error = "image encode failed"; return res; }

    res.ok = true;
    res.width = dstW;
    res.height = dstH;
    res.base64 = Base64(encoded.data(), encoded.size());
    return res;
}

} // namespace mcp
