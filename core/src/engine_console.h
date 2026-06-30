// engine_console.h — run commands through the engine's console command buffer
// (IVEngineClient::ClientCmd_Unrestricted), exactly as if typed into the console.
// Unlike Lua's RunConsoleCommand, this is NOT FCVAR-filtered, so `map`, `connect`,
// `disconnect`, `echo`, etc. all work. Commands are queued to the engine command
// buffer and executed on the next frame (outside our render hook) — safe.
//
// This header is intentionally free of Source SDK includes; all SDK headers are
// confined to engine_console.cpp.
#pragma once

namespace mcp {

// True once IVEngineClient has been resolved from engine.dll.
bool EngineConsoleReady();

// Queue a console command for unrestricted execution. Returns false if the engine
// interface could not be resolved.
bool RunEngineCommand(const char* command);

} // namespace mcp
