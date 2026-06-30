#include "lua_bridge.h"
#include "common.h"
#include "pump.h"

#include "GarrysMod/Lua/LuaShared.h"
#include "GarrysMod/Lua/LuaInterface.h"
#include "GarrysMod/Lua/Types.h"

#include <vector>

using namespace GarrysMod::Lua;

// Defined in dllmain.cpp — handle of our injected module, used to locate
// bootstrap.lua sitting next to core.dll.
extern HMODULE g_hSelfModule;

namespace mcp {

namespace {
    typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);

    // Read bootstrap.lua from the directory containing core.dll.
    bool ReadBootstrapSource(std::string& out) {
        char dllPath[MAX_PATH];
        if (!GetModuleFileNameA(g_hSelfModule, dllPath, MAX_PATH)) return false;
        std::string dir(dllPath);
        size_t slash = dir.find_last_of("\\/");
        if (slash == std::string::npos) return false;
        std::string luaPath = dir.substr(0, slash + 1) + "bootstrap.lua";

        FILE* f = nullptr;
        if (fopen_s(&f, luaPath.c_str(), "rb") != 0 || !f) {
            MCP_ERR("could not open " + luaPath);
            return false;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len <= 0) { fclose(f); return false; }
        out.resize(static_cast<size_t>(len));
        size_t rd = fread(&out[0], 1, static_cast<size_t>(len), f);
        fclose(f);
        out.resize(rd);
        return true;
    }
}

bool LuaBridge::TryResolve() {
    // Resolve ILuaShared once (it persists for the process lifetime).
    if (!shared_) {
        HMODULE luaShared = GetModuleHandleA("lua_shared.dll");
        if (!luaShared) return false; // not loaded yet

        auto factory = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(luaShared, "CreateInterface"));
        if (!factory) { MCP_ERR("lua_shared!CreateInterface not found"); return false; }

        shared_ = reinterpret_cast<ILuaShared*>(factory(GMOD_LUASHARED_INTERFACE, nullptr));
        if (!shared_) { MCP_ERR("CreateInterface(LUASHARED003) returned null"); return false; }
        MCP_LOG("resolved ILuaShared");
    }

    // Re-fetch the active interface EVERY call. GMod recreates the client Lua
    // state on map loads (the `map` command is a full restart), producing a new
    // ILuaInterface; caching the old one means using a stale/dead state (or
    // crashing). When the pointer changes we reset bootstrapped_ so the pump
    // reloads the agent into the fresh state.
    // Require a live lua_State, not just a non-null interface. During a
    // connect/map-load GMod constructs the client CLuaInterface a few frames
    // before it calls SetState() on it, so GetLuaInterface(CLIENT) briefly
    // returns an interface whose internal lua_State is still null. Adopting it
    // and running Lua (RunStringEx in EnsureBootstrap, or a dispatch) against a
    // null state crashes natively — this is the menu->server transition crash.
    // The same window opens in reverse on disconnect as the state is torn down.
    ILuaInterface* client = shared_->GetLuaInterface(State::CLIENT);
    if (client && client->GetState()) {
        if (iface_ != client) {
            iface_ = client; realm_ = State::CLIENT; bootstrapped_ = false;
            MCP_LOG("CLIENT interface (re)resolved");
        }
        return true;
    }

    // No live client state (main menu, or mid map-load) — fall back to the menu
    // realm, again only once its lua_State is live.
    ILuaInterface* menu = shared_->GetLuaInterface(State::MENU);
    if (menu && menu->GetState()) {
        if (iface_ != menu || realm_ != State::MENU) {
            iface_ = menu; realm_ = State::MENU; bootstrapped_ = false;
            MCP_LOG("MENU interface resolved");
        }
        return true;
    }

    iface_ = nullptr; realm_ = -1; bootstrapped_ = false;
    return false;
}

const char* LuaBridge::RealmName() const {
    using namespace GarrysMod::Lua;
    if (realm_ == State::CLIENT) return "client";
    if (realm_ == State::MENU) return "menu";
    return "none";
}

bool LuaBridge::EnsureBootstrap() {
    if (bootstrapped_) return true;
    if (!iface_ || !iface_->GetState()) return false; // not resolved, or state not live yet

    std::string src;
    if (!ReadBootstrapSource(src)) return false;

    // Expose the native command pump to Lua: _G._mcp_native_pump = NativePump.
    // The agent's PostRender hook calls this every frame to drain commands.
    iface_->PushSpecial(SPECIAL_GLOB);
    iface_->PushCFunction(&NativePump);
    iface_->SetField(-2, "_mcp_native_pump");
    iface_->Pop(1);

    // run=true, printErrors=true, dontPushErrors=true, noReturns=true
    bool ok = iface_->RunStringEx("gmod_mcp/bootstrap.lua", "", src.c_str(), true, true, true, true);
    if (!ok) { MCP_ERR("bootstrap.lua failed to run"); return false; }

    bootstrapped_ = true;
    MCP_LOG("bootstrap.lua loaded");
    return true;
}

bool LuaBridge::ForceBootstrapReload() {
    if (!iface_) return false;
    bootstrapped_ = false;
    return EnsureBootstrap();
}

std::string LuaBridge::Dispatch(const char* name, const std::string& argJson) {
    if (!iface_) return R"({"ok":false,"error":"lua interface not resolved"})";
    if (!iface_->GetState()) return R"({"ok":false,"error":"lua interface not ready - state transition"})";

    ILuaInterface* L = iface_;
    const int top = L->Top();

    // Stack: push _G, then MCP, then MCP._dispatch.
    L->PushSpecial(SPECIAL_GLOB);
    L->GetField(-1, "MCP");
    if (L->GetType(-1) != Type::Table) {
        L->Pop(L->Top() - top);
        return R"({"ok":false,"error":"MCP table missing - bootstrap not loaded"})";
    }
    L->GetField(-1, "_dispatch");
    if (L->GetType(-1) != Type::Function) {
        L->Pop(L->Top() - top);
        return R"({"ok":false,"error":"MCP._dispatch missing"})";
    }

    // Args: (name, argJson)
    L->PushString(name);
    L->PushString(argJson.c_str());

    // Protected call: 2 args, 1 result.
    int status = L->PCall(2, 1, 0);

    std::string result;
    if (status != 0) {
        const char* err = L->GetString(-1);
        std::string e = err ? err : "lua error";
        // Escape minimally for embedding into JSON.
        std::string esc;
        for (char c : e) {
            if (c == '"' || c == '\\') { esc.push_back('\\'); esc.push_back(c); }
            else if (c == '\n') esc += "\\n";
            else if (c == '\r') {}
            else esc.push_back(c);
        }
        result = std::string("{\"ok\":false,\"error\":\"lua pcall failed: ") + esc + "\"}";
    } else {
        const char* s = L->GetString(-1);
        result = s ? s : R"({"ok":false,"error":"dispatch returned nil"})";
    }

    L->Pop(L->Top() - top); // restore stack
    return result;
}

LuaBridge& Lua() {
    static LuaBridge bridge;
    return bridge;
}

} // namespace mcp
