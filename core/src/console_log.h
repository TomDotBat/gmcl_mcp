// console_log.h — captures the game console output (engine Msg/Warning/Error and
// Lua print, which routes through Msg) by chaining tier0's spew output function.
// Stores recent lines in a mutex-guarded ring buffer that the IPC thread reads
// directly (no main-thread pump needed).
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace mcp {

struct ConsoleLine {
    uint64_t id;        // monotonically increasing
    int severity;       // 0=msg 1=warning 2=assert 3=error 4=log
    std::string text;
};

// Installs the spew hook (idempotent). Safe to call once initialization order
// allows tier0.dll to be present.
bool InstallConsoleCapture();
void RemoveConsoleCapture();

// Returns up to `maxLines` most recent lines with id > sinceId, optionally
// filtered by a case-insensitive substring and/or a minimum severity.
// `nextId` receives the highest id currently in the buffer (for incremental reads).
std::vector<ConsoleLine> ReadConsole(uint64_t sinceId, int maxLines,
                                     const std::string& filter, int minSeverity,
                                     uint64_t& nextId);

} // namespace mcp
