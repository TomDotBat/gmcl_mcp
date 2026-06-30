#include "dx9_hook.h"
#include "common.h"
#include "command_queue.h"
#include "dispatch.h"
#include "lua_bridge.h"
#include "console_log.h"

#include <d3d9.h>
#include "../third_party/minhook/include/MinHook.h"

namespace mcp {

namespace {
    typedef HRESULT(WINAPI* PresentFn)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

    PresentFn        g_originalPresent = nullptr;
    IDirect3DDevice9* g_device = nullptr;
    bool             g_hooked = false;

    // Create a throwaway device just to read the IDirect3DDevice9 vtable so we
    // can find Present's address. The real game device shares the same vtable.
    void** GetDeviceVTable() {
        HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
        if (!d3d9) d3d9 = LoadLibraryA("d3d9.dll");
        if (!d3d9) { MCP_ERR("d3d9.dll unavailable"); return nullptr; }

        auto pDirect3DCreate9 = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(
            GetProcAddress(d3d9, "Direct3DCreate9"));
        if (!pDirect3DCreate9) { MCP_ERR("Direct3DCreate9 not found"); return nullptr; }

        IDirect3D9* d3d = pDirect3DCreate9(D3D_SDK_VERSION);
        if (!d3d) { MCP_ERR("Direct3DCreate9 returned null"); return nullptr; }

        // A hidden window for the dummy device.
        HWND hwnd = CreateWindowExA(0, "STATIC", "gmod_mcp_dummy", WS_OVERLAPPED,
                                    0, 0, 1, 1, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);

        D3DPRESENT_PARAMETERS pp = {};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = hwnd;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.BackBufferWidth = 1;
        pp.BackBufferHeight = 1;

        IDirect3DDevice9* dummy = nullptr;
        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
                                       &pp, &dummy);
        if (FAILED(hr) || !dummy) {
            // Retry with the default window (some drivers dislike the STATIC hwnd).
            hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, GetDesktopWindow(),
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
                                   &pp, &dummy);
        }

        void** vtable = nullptr;
        if (SUCCEEDED(hr) && dummy) {
            vtable = *reinterpret_cast<void***>(dummy);
            dummy->Release();
        } else {
            MCP_ERR("dummy CreateDevice failed");
        }
        d3d->Release();
        if (hwnd) DestroyWindow(hwnd);
        return vtable;
    }

    // The pump: lazy init + drain the command queue. Runs every frame on the
    // main thread.
    void Pump() {
        // Lazy, idempotent initialisation that must happen on the main thread.
        if (Lua().TryResolve()) Lua().EnsureBootstrap();
        InstallConsoleCapture();

        Queue().Drain([](Command& cmd) {
            nlohmann::json env = DispatchMainThread(cmd.method, cmd.params);
            cmd.promise.set_value(std::move(env));
        });
    }

    HRESULT WINAPI HookedPresent(IDirect3DDevice9* device, const RECT* src, const RECT* dest,
                                 HWND wnd, const RGNDATA* dirty) {
        g_device = device;
        // The backbuffer holds the finished frame at this point — ideal for both
        // draining commands and (inside dispatch) capturing screenshots.
        Pump();
        return g_originalPresent(device, src, dest, wnd, dirty);
    }
}

bool InstallD3D9Hook() {
    if (g_hooked) return true;

    if (MH_Initialize() != MH_OK) { MCP_ERR("MH_Initialize failed"); return false; }

    void** vtable = GetDeviceVTable();
    if (!vtable) return false;

    void* presentTarget = vtable[17]; // IDirect3DDevice9::Present
    if (MH_CreateHook(presentTarget, &HookedPresent,
                      reinterpret_cast<void**>(&g_originalPresent)) != MH_OK) {
        MCP_ERR("MH_CreateHook(Present) failed");
        return false;
    }
    if (MH_EnableHook(presentTarget) != MH_OK) {
        MCP_ERR("MH_EnableHook(Present) failed");
        return false;
    }

    g_hooked = true;
    MCP_LOG("D3D9 Present hook installed");
    return true;
}

void RemoveD3D9Hook() {
    if (!g_hooked) return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_hooked = false;
}

IDirect3DDevice9* CurrentDevice() { return g_device; }

CommandQueue& Queue() {
    static CommandQueue q;
    return q;
}

} // namespace mcp
