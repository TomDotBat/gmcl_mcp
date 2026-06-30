// dllmain.cpp — entry point for the injected core.dll.
//
// On attach we spin up a worker thread (you must not do heavy work or load
// libraries from inside DllMain under the loader lock). The worker subclasses the
// game window (the "pump") and starts the IPC server. Everything else — resolving
// Lua, loading the agent, draining commands — happens on the main thread via the
// window subclass and the agent's PostRender hook. No Direct3D device is created.
#include "common.h"
#include "pump.h"
#include "ipc_server.h"

HMODULE g_hSelfModule = nullptr;

namespace {
    DWORD WINAPI WorkerMain(LPVOID) {
        MCP_LOG("core.dll worker starting (v" GMOD_MCP_VERSION ")");
        // Let the game settle after LoadLibrary before subclassing the window.
        Sleep(300);
        if (!mcp::InstallPump())
            MCP_ERR("pump install failed (game window not found); commands unavailable");
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
