#include "dispatch.h"
#include "common.h"
#include "lua_bridge.h"
#include "console_log.h"
#include "screenshot.h"
#include "dx9_hook.h"

#include <map>

using nlohmann::json;

namespace mcp {

namespace {
    // method name -> MCP.<fn> in bootstrap.lua
    const std::map<std::string, std::string> kLuaMethods = {
        {"lua_run",         "Run"},
        {"get_state",       "GetState"},
        {"input_set",       "SetInput"},
        {"input_clear",     "ClearInput"},
        {"look",            "Look"},
        {"press",           "Press"},
        {"dump_vgui",       "DumpVGUI"},
        {"cursor",          "Cursor"},
        {"type",            "Type"},
        {"console_command", "ConCommand"},
    };

    json ErrEnvelope(const std::string& msg) {
        return json{{"ok", false}, {"error", msg}};
    }

    json HandleConsoleLog(const json& p) {
        uint64_t sinceId = p.value("sinceId", 0ull);
        int lines = p.value("lines", 200);
        std::string filter = p.value("filter", std::string());
        int minSeverity = p.value("severity", 0);
        uint64_t nextId = 0;
        auto rows = ReadConsole(sinceId, lines, filter, minSeverity, nextId);

        json arr = json::array();
        for (const auto& r : rows)
            arr.push_back(json{{"id", r.id}, {"severity", r.severity}, {"text", r.text}});
        return json{{"ok", true}, {"result", {{"lines", arr}, {"nextId", nextId}}}};
    }
}

bool TryDispatchOffThread(const std::string& method, const json& params, json& out) {
    if (method == "ping") {
        out = json{{"ok", true}, {"result", {
            {"pong", true},
            {"version", GMOD_MCP_VERSION},
            {"lua_ready", Lua().IsReady()},
            {"realm", Lua().RealmName()},
            {"bootstrapped", Lua().IsBootstrapped()},
        }}};
        return true;
    }
    if (method == "console_log") {
        out = HandleConsoleLog(params);
        return true;
    }
    return false;
}

json DispatchMainThread(const std::string& method, const json& params) {
    try {
        if (method == "screenshot") {
            ScreenshotRequest req;
            req.format  = params.value("format", std::string("jpeg"));
            req.quality = params.value("quality", 80);
            req.x       = params.value("x", 0);
            req.y       = params.value("y", 0);
            req.w       = params.value("w", 0);
            req.h       = params.value("h", 0);
            req.scale   = params.value("scale", 1.0);

            IDirect3DDevice9* dev = CurrentDevice();
            if (!dev) return ErrEnvelope("no D3D9 device captured yet (is the game rendering?)");

            ScreenshotResult r = CaptureBackbuffer(dev, req);
            if (!r.ok) return ErrEnvelope(r.error.empty() ? "screenshot failed" : r.error);
            return json{{"ok", true}, {"result", {
                {"format", r.format}, {"width", r.width}, {"height", r.height},
                {"base64", r.base64},
            }}};
        }

        if (method == "reload_bootstrap") {
            if (!Lua().IsReady()) return ErrEnvelope("lua interface not ready");
            bool ok = Lua().ForceBootstrapReload();
            if (!ok) return ErrEnvelope("bootstrap reload failed");
            return json{{"ok", true}, {"result", {{"reloaded", true}, {"realm", Lua().RealmName()}}}};
        }

        // Off-thread methods are also valid on the main thread (server may route
        // them here under load); serve them too.
        json off;
        if (TryDispatchOffThread(method, params, off)) return off;

        auto it = kLuaMethods.find(method);
        if (it != kLuaMethods.end()) {
            if (!Lua().IsReady())      return ErrEnvelope("lua interface not ready");
            if (!Lua().IsBootstrapped()) return ErrEnvelope("lua agent not loaded yet");
            std::string envStr = Lua().Dispatch(it->second.c_str(), params.dump());
            return json::parse(envStr, nullptr, false); // already an envelope
        }

        return ErrEnvelope("unknown method: " + method);
    } catch (const std::exception& e) {
        return ErrEnvelope(std::string("dispatch exception: ") + e.what());
    }
}

} // namespace mcp
