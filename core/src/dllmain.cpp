// dllmain.cpp — entry point for the injected core.dll.
//
// On attach we spin up a worker thread (you must not do heavy work or load
// libraries from inside DllMain under the loader lock). The worker installs the
// D3D9 Present hook and starts the IPC server. The Present hook then performs the
// remaining main-thread initialisation (resolving Lua, loading the agent,
// installing console capture) lazily, frame by frame.
#include "common.h"
#include "dx9_hook.h"
#include "ipc_server.h"
#include "console_log.h"

HMODULE g_hSelfModule = nullptr;

namespace {
    DWORD WINAPI WorkerMain(LPVOID) {
        MCP_LOG("core.dll worker starting (v" GMOD_MCP_VERSION ")");
        if (!mcp::InstallD3D9Hook())
            MCP_ERR("D3D9 hook failed; screenshots and the command pump are unavailable");
        mcp::StartIpcServer();
        MCP_LOG("core.dll initialised");
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        g_hSelfModule = module;
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, &WorkerMain, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        // IMPORTANT: it is unsafe to join threads, uninitialise MinHook, or touch
        // other loaded modules here — DllMain runs under the loader lock and that
        // deadlocks/crashes. Hot-eject therefore cannot cleanly unload us; the
        // supported way to reload a rebuilt DLL is to restart the game. We only
        // do work for an explicit, cooperative shutdown (see ipc "shutdown"),
        // never from the loader. So: no-op here.
        break;
    }
    return TRUE;
}
