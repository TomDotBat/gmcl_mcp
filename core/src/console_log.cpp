#include "console_log.h"
#include "common.h"

#include <deque>
#include <mutex>
#include <algorithm>
#include <cctype>

namespace mcp {

namespace {
    constexpr size_t kMaxLines = 4000;

    std::mutex g_mutex;
    std::deque<ConsoleLine> g_lines;
    uint64_t g_nextId = 1;
    bool g_installed = false;

    void PushLine(int severity, const char* msg) {
        if (!msg) return;
        std::string text(msg);
        // Trim a single trailing CR/LF (LoggingSystem messages usually end with \n).
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        if (text.empty()) return;
        std::lock_guard<std::mutex> lk(g_mutex);
        g_lines.push_back(ConsoleLine{ g_nextId++, severity, std::move(text) });
        while (g_lines.size() > kMaxLines) g_lines.pop_front();
    }

    // --- x86-64 branch: tier0 LoggingSystem -------------------------------------
    // public/tier0/logging.h ABI (minimal). ILoggingListener has a single virtual
    // method, Log, and NO virtual destructor — so our subclass must match exactly.
    struct Color4 { unsigned char r, g, b, a; };
    struct LoggingContext_t {
        int               m_ChannelID;
        int               m_Flags;
        int               m_Severity; // LS_MESSAGE=0, LS_WARNING=1, LS_ASSERT=2, LS_ERROR=3
        Color4            m_Color;
    };
    struct ILoggingListener {
        virtual void Log(const LoggingContext_t* ctx, const char* message) = 0;
    };
    typedef void (*RegisterListenerFn)(ILoggingListener*);

    struct McpLoggingListener : public ILoggingListener {
        void Log(const LoggingContext_t* ctx, const char* message) override {
            int sev = ctx ? ctx->m_Severity : 0;
            PushLine(sev, message);
        }
    };
    McpLoggingListener g_listener;

    bool TryLoggingSystem(HMODULE tier0) {
        auto reg = reinterpret_cast<RegisterListenerFn>(
            GetProcAddress(tier0, "LoggingSystem_RegisterLoggingListener"));
        if (!reg) return false;
        reg(&g_listener);
        MCP_LOG("console capture installed (LoggingSystem listener)");
        return true;
    }

    // --- 32-bit / legacy branch: tier0 spew func --------------------------------
    enum SpewType_t { SPEW_MESSAGE = 0, SPEW_WARNING, SPEW_ASSERT, SPEW_ERROR, SPEW_LOG };
    enum SpewRetval_t { SPEW_DEBUGGER = 0, SPEW_CONTINUE, SPEW_ABORT };
    typedef SpewRetval_t(*SpewOutputFunc_t)(SpewType_t, const char*);
    typedef void (*SetSpewFn)(SpewOutputFunc_t);
    typedef SpewOutputFunc_t(*GetSpewFn)();

    SpewOutputFunc_t g_originalSpew = nullptr;

    SpewRetval_t SpewHook(SpewType_t type, const char* msg) {
        PushLine(static_cast<int>(type), msg);
        if (g_originalSpew) return g_originalSpew(type, msg);
        return SPEW_CONTINUE;
    }

    bool TrySpewFunc(HMODULE tier0) {
        auto setFn = reinterpret_cast<SetSpewFn>(GetProcAddress(tier0, "SetSpewOutputFunc"));
        auto getFn = reinterpret_cast<GetSpewFn>(GetProcAddress(tier0, "GetSpewOutputFunc"));
        if (!setFn || !getFn) return false;
        g_originalSpew = getFn();
        setFn(&SpewHook);
        MCP_LOG("console capture installed (legacy spew func)");
        return true;
    }
}

bool InstallConsoleCapture() {
    if (g_installed) return true;

    HMODULE tier0 = GetModuleHandleA("tier0.dll");
    if (!tier0) return false; // not loaded yet

    // Prefer the modern LoggingSystem (x86-64 branch); fall back to the legacy
    // spew function (older 32-bit branch).
    if (TryLoggingSystem(tier0) || TrySpewFunc(tier0)) {
        g_installed = true;
        return true;
    }
    MCP_ERR("no supported tier0 logging entry point found");
    return false;
}

void RemoveConsoleCapture() {
    // LoggingSystem has no clean per-listener unregister, and we never safely
    // unload from the loader anyway — leave the listener in place.
    g_installed = false;
}

std::vector<ConsoleLine> ReadConsole(uint64_t sinceId, int maxLines,
                                     const std::string& filter, int minSeverity,
                                     uint64_t& nextId) {
    std::string needle = filter;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::vector<ConsoleLine> out;
    std::lock_guard<std::mutex> lk(g_mutex);
    nextId = g_nextId;
    if (maxLines <= 0) maxLines = 200;

    for (const auto& line : g_lines) {
        if (line.id <= sinceId) continue;
        if (line.severity < minSeverity) continue;
        if (!needle.empty()) {
            std::string hay = line.text;
            std::transform(hay.begin(), hay.end(), hay.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (hay.find(needle) == std::string::npos) continue;
        }
        out.push_back(line);
    }
    if (static_cast<int>(out.size()) > maxLines)
        out.erase(out.begin(), out.end() - maxLines);
    return out;
}

} // namespace mcp
