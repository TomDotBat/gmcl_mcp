// pump.h — the main-thread command pump, with NO Direct3D device creation.
//
// Two pieces, both on the game's main thread:
//   1. A window subclass on the game window. Each message we (re)resolve the Lua
//      interface and (re)load the agent — this survives map changes, which
//      recreate the client Lua state.
//   2. NativePump: a C function exposed to Lua as _mcp_native_pump and called from
//      the agent's PostRender hook every frame. It drains the command queue and
//      dispatches each command. Because it runs inside PostRender, the screenshot
//      handler can use render.Capture.
#pragma once

struct lua_State;

namespace mcp {

class CommandQueue;

// Subclass the game window to drive agent (re)load on the main thread. Idempotent.
bool InstallPump();
void RemovePump();

// Nudge the window so the subclass runs promptly (called when a command arrives).
void WakePump();

// The C function registered into Lua as _mcp_native_pump. Drains + dispatches the
// command queue; called from the agent's PostRender hook.
int NativePump(lua_State* L);

// Process-wide command queue (IPC thread submits, the pump drains).
CommandQueue& Queue();

} // namespace mcp
