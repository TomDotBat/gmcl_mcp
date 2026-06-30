// ipc_server.h — named-pipe server (\\.\pipe\gmod_mcp). Runs on its own thread,
// reads length-prefixed JSON requests, dispatches them (directly for read-only
// methods, or via the main-thread command queue), and writes length-prefixed
// JSON responses.
#pragma once

namespace mcp {

// Starts the IPC server thread (idempotent).
void StartIpcServer();
// Signals the server thread to stop and joins it.
void StopIpcServer();

} // namespace mcp
