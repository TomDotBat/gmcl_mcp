// common.h — shared definitions, logging, version for the injected core.dll
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <cstdio>

#define GMOD_MCP_VERSION "0.1.0"

// Named pipe the injected DLL listens on; the TypeScript MCP server connects here.
#define GMOD_MCP_PIPE_NAME "\\\\.\\pipe\\gmod_mcp"

namespace mcp {

// Minimal logger: writes to the debugger output and an optional log file in %TEMP%.
// Visible via DebugView or by tailing %TEMP%\gmod_mcp.log.
inline void LogLine(const char* level, const std::string& msg) {
    char buf[2048];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[gmod-mcp][%s] %s\n", level, msg.c_str());
    OutputDebugStringA(buf);

    static FILE* f = nullptr;
    if (!f) {
        char path[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, path);
        if (n > 0 && n < MAX_PATH) {
            std::string full = std::string(path) + "gmod_mcp.log";
            fopen_s(&f, full.c_str(), "a");
        }
    }
    if (f) { fputs(buf, f); fflush(f); }
}

} // namespace mcp

#define MCP_LOG(msg)  ::mcp::LogLine("info", (msg))
#define MCP_WARN(msg) ::mcp::LogLine("warn", (msg))
#define MCP_ERR(msg)  ::mcp::LogLine("err",  (msg))
