#include "pump.h"
#include "common.h"
#include "command_queue.h"
#include "dispatch.h"
#include "lua_bridge.h"
#include "console_log.h"

namespace mcp {

namespace {
    WNDPROC g_origWndProc = nullptr;
    HWND    g_window = nullptr;

    struct EnumCtx { DWORD pid; HWND hwnd; };
    BOOL CALLBACK EnumWndProc(HWND h, LPARAM lp) {
        auto* ctx = reinterpret_cast<EnumCtx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == ctx->pid && GetWindow(h, GW_OWNER) == nullptr && IsWindowVisible(h)) {
            ctx->hwnd = h;
            return FALSE;
        }
        return TRUE;
    }

    HWND FindGameWindow() {
        HWND w = FindWindowA("Valve001", nullptr);
        if (w) return w;
        EnumCtx ctx{ GetCurrentProcessId(), nullptr };
        EnumWindows(&EnumWndProc, reinterpret_cast<LPARAM>(&ctx));
        return ctx.hwnd;
    }

    // Runs on the main thread (the thread that pumps the window's messages).
    // Keeps the Lua interface resolved and the agent loaded into the *current* Lua
    // state. After a map load (new client state) this re-resolves and reloads the
    // agent, which re-registers the PostRender pump hook.
    LRESULT CALLBACK PumpWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
        if (Lua().TryResolve())
            Lua().EnsureBootstrap();
        InstallConsoleCapture();

        // In the client realm the agent's PostRender hook drains the queue (a
        // render context, needed for screenshots). At the main menu there is no
        // client PostRender, so drain here instead — enough for console_command
        // (e.g. `map` to start a game) and lua/state queries. Draining in only one
        // place per realm avoids a screenshot being consumed outside a render pass.
        const char* realm = Lua().RealmName();
        if (realm && realm[0] == 'm') // "menu"
            NativePump(nullptr);

        return CallWindowProcA(g_origWndProc, h, msg, w, l);
    }
}

bool InstallPump() {
    if (g_window) return true;
    g_window = FindGameWindow();
    if (!g_window) { MCP_ERR("game window not found; pump not installed"); return false; }
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&PumpWndProc)));
    MCP_LOG("pump installed (window subclass, no D3D device)");
    return true;
}

void RemovePump() {
    if (g_window && g_origWndProc) {
        SetWindowLongPtrA(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        g_window = nullptr;
        g_origWndProc = nullptr;
    }
}

void WakePump() {
    if (g_window) PostMessageA(g_window, WM_NULL, 0, 0);
}

int NativePump(lua_State* /*L*/) {
    // Called from the agent's PostRender hook (a render context, so the screenshot
    // handler's render.Capture works). Runs on the main thread.
    Queue().Drain([](Command& cmd) {
        nlohmann::json env = DispatchMainThread(cmd.method, cmd.params);
        cmd.promise.set_value(std::move(env));
    });
    return 0;
}

CommandQueue& Queue() {
    static CommandQueue q;
    return q;
}

} // namespace mcp
