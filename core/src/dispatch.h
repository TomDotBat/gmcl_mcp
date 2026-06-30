// dispatch.h — routes a parsed IPC request to the right subsystem.
//
// Most methods touch Lua or D3D and therefore run on the main thread (called by
// the pump). A couple are read-only and can be served straight from the IPC
// thread (ping, console_log).
#pragma once

#include <string>
#include "../third_party/json.hpp"

namespace mcp {

// Returns true and fills `out` if the method can be served off the main thread.
bool TryDispatchOffThread(const std::string& method, const nlohmann::json& params,
                          nlohmann::json& out);

// Runs on the main thread (from the pump). Always returns an envelope:
// {"ok":true,"result":...} or {"ok":false,"error":"..."}.
nlohmann::json DispatchMainThread(const std::string& method, const nlohmann::json& params);

} // namespace mcp
