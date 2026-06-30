// dx9_hook.h — hooks IDirect3DDevice9::Present. The detour runs on the game's
// main render thread and serves as the "pump": every frame it (1) lazily resolves
// the Lua interface / loads the agent / installs console capture, (2) drains the
// command queue and executes each command on the main thread, and (3) is the
// place screenshots are taken (the device is live here).
#pragma once

struct IDirect3DDevice9;

namespace mcp {

class CommandQueue;

// Installs the Present hook via MinHook. Returns false if it could not build the
// device vtable or create the hook.
bool InstallD3D9Hook();
void RemoveD3D9Hook();

// The device captured from the most recent Present call (null until the first
// frame after injection). Only valid on the main thread.
IDirect3DDevice9* CurrentDevice();

// Process-wide command queue (IPC thread submits, pump drains).
CommandQueue& Queue();

} // namespace mcp
