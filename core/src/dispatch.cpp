#include "dispatch.h"
#include "common.h"
#include "lua_bridge.h"
#include "console_log.h"
#include "engine_console.h"

using nlohmann::json;

namespace mcp {

namespace {
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
        // Note: "screenshot" is handled in Lua (MCP.Screenshot via render.Capture),
        // routed through the generic forward below — it must run inside the agent's
        // PostRender hook, which is exactly where the pump calls us from.

        if (method == "reload_bootstrap") {
            if (!Lua().IsReady()) return ErrEnvelope("lua interface not ready");
            bool ok = Lua().ForceBootstrapReload();
            if (!ok) return ErrEnvelope("bootstrap reload failed");
            return json{{"ok", true}, {"result", {{"reloaded", true}, {"realm", Lua().RealmName()}}}};
        }

        if (method == "console_command") {
            // Run through the engine's console command buffer (unrestricted), so
            // map/connect/disconnect/echo/etc. all work. Queued to the engine and
            // executed next frame — safe to call from the pump.
            std::string cmd = params.value("command", std::string());
            if (cmd.empty()) return ErrEnvelope("command required");
            if (!RunEngineCommand(cmd.c_str()))
                return ErrEnvelope("engine console not resolved yet (engine.dll)");
            return json{{"ok", true}, {"result", {{"ran", cmd}}}};
        }

        // Off-thread methods are also valid on the main thread (server may route
        // them here under load); serve them too.
        json off;
        if (TryDispatchOffThread(method, params, off)) return off;

        // Everything else is forwarded by name to the Lua agent's MCP._dispatch,
        // which maps it to an MCP.* function. New Lua-backed tools therefore need
        // no native change.
        if (!Lua().IsReady())        return ErrEnvelope("lua interface not ready");
        if (!Lua().IsBootstrapped()) return ErrEnvelope("lua agent not loaded yet");

        json env = json::parse(Lua().Dispatch(method.c_str(), params.dump()), nullptr, false);
        // If the Lua state was reset under us (e.g. a changelevel reused the
        // interface but cleared globals), reload the agent and retry once.
        if (env.is_object() && !env.value("ok", true) &&
            env.value("error", std::string()).find("MCP table missing") != std::string::npos) {
            Lua().ForceBootstrapReload();
            env = json::parse(Lua().Dispatch(method.c_str(), params.dump()), nullptr, false);
        }
        return env;
    } catch (const std::exception& e) {
        return ErrEnvelope(std::string("dispatch exception: ") + e.what());
    }
}

} // namespace mcp
