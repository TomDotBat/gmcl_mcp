// engine_console.cpp — the ONLY file that pulls in Source SDK headers. Keeping the
// SDK isolated here avoids its macros/types leaking into the rest of the DLL.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstdlib> // free, for MemAlloc_Free below

// The x86-64 sourcesdk-minimal's tier1/utlstring.h calls MemAlloc_Free, but
// tier0/memalloc.h does NOT declare it under NO_MALLOC_OVERRIDE (which we set so
// the SDK doesn't hijack this DLL's global new/delete). Supply a plain CRT
// wrapper before the SDK headers are parsed. The matching allocator
// (MemAlloc_Realloc) lives out-of-line in the SDK lib we don't link, so the
// CUtlString heap paths that would call this are never reached here — this only
// needs to exist to satisfy the compiler.
inline void MemAlloc_Free(void* p) { free(p); }

#include "cdll_int.h" // IVEngineClient, VENGINE_CLIENT_INTERFACE_VERSION

#include "engine_console.h"

namespace mcp {

namespace {
    typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);
    IVEngineClient* g_engine = nullptr;

    IVEngineClient* Resolve() {
        if (g_engine) return g_engine;
        HMODULE eng = GetModuleHandleA("engine.dll");
        if (!eng) return nullptr;
        auto factory = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(eng, "CreateInterface"));
        if (!factory) return nullptr;

        // ClientCmd_Unrestricted lives in IVEngineClient013, so any version that
        // derives from it (013..current) exposes it at the same vtable slot.
        for (int v = 13; v <= 21; ++v) {
            char name[32];
            _snprintf_s(name, sizeof(name), _TRUNCATE, "VEngineClient%03d", v);
            g_engine = reinterpret_cast<IVEngineClient*>(factory(name, nullptr));
            if (g_engine) {
                char dbg[64];
                _snprintf_s(dbg, sizeof(dbg), _TRUNCATE, "[gmod-mcp] engine console: %s\n", name);
                OutputDebugStringA(dbg);
                break;
            }
        }
        return g_engine;
    }
}

bool EngineConsoleReady() { return Resolve() != nullptr; }

bool RunEngineCommand(const char* command) {
    IVEngineClient* e = Resolve();
    if (!e || !command) return false;
    e->ClientCmd_Unrestricted(command);
    return true;
}

} // namespace mcp
