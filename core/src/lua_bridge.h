// lua_bridge.h — resolves the Garry's Mod client Lua interface from an injected
// DLL and provides a single safe entry point for calling into the embedded Lua
// agent (bootstrap.lua).
//
// All methods here MUST be called on the game's main thread (i.e. from the pump
// inside the D3D9 Present hook). LuaJIT is single-threaded.
#pragma once

#include <string>

namespace GarrysMod { namespace Lua { class ILuaInterface; } }

namespace mcp {

class LuaBridge {
public:
    // Tries to resolve ILuaShared -> client ILuaInterface via lua_shared.dll's
    // CreateInterface("LUASHARED003"). Safe to call repeatedly; returns true once
    // the client interface is available. (May fail early in process startup.)
    bool TryResolve();

    bool IsReady() const { return iface_ != nullptr; }

    // "client", "menu", or "none" — which realm we bridged into.
    const char* RealmName() const;

    // Loads bootstrap.lua into the client state (idempotent). Must run on the
    // main thread. Returns true on success.
    bool EnsureBootstrap();
    bool IsBootstrapped() const { return bootstrapped_; }

    // Force a re-read of bootstrap.lua from disk and re-run it (hot reload of the
    // Lua agent without restarting the game). Main thread only.
    bool ForceBootstrapReload();

    // Calls MCP._dispatch(name, argJson) inside Lua and returns the JSON string
    // it produces: an envelope of the form {"ok":bool,"result":...} or
    // {"ok":false,"error":"..."}. On a hard Lua/PCall failure, returns a
    // synthesised error envelope so callers always get valid JSON.
    std::string Dispatch(const char* name, const std::string& argJson);

private:
    GarrysMod::Lua::ILuaInterface* iface_ = nullptr;
    int realm_ = -1; // GarrysMod::Lua::State value, or -1
    bool bootstrapped_ = false;
};

// Process-wide bridge instance.
LuaBridge& Lua();

} // namespace mcp
