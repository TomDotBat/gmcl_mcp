#include "ipc_server.h"
#include "common.h"
#include "dispatch.h"
#include "pump.h"
#include "command_queue.h"

#include <thread>
#include <atomic>
#include <string>
#include <future>

using nlohmann::json;

namespace mcp {

namespace {
    std::thread       g_thread;
    std::atomic<bool> g_running{false};

    bool ReadN(HANDLE pipe, void* buf, DWORD n) {
        DWORD done = 0;
        auto* p = static_cast<char*>(buf);
        while (done < n) {
            DWORD got = 0;
            if (!ReadFile(pipe, p + done, n - done, &got, nullptr) || got == 0)
                return false;
            done += got;
        }
        return true;
    }

    bool WriteN(HANDLE pipe, const void* buf, DWORD n) {
        DWORD done = 0;
        auto* p = static_cast<const char*>(buf);
        while (done < n) {
            DWORD put = 0;
            if (!WriteFile(pipe, p + done, n - done, &put, nullptr) || put == 0)
                return false;
            done += put;
        }
        return true;
    }

    bool ReadFrame(HANDLE pipe, std::string& out) {
        uint32_t len = 0;
        if (!ReadN(pipe, &len, 4)) return false;
        if (len == 0 || len > 64u * 1024u * 1024u) return false; // sanity cap (64 MiB)
        out.resize(len);
        return ReadN(pipe, &out[0], len);
    }

    bool WriteFrame(HANDLE pipe, const std::string& body) {
        uint32_t len = static_cast<uint32_t>(body.size());
        if (!WriteN(pipe, &len, 4)) return false;
        return WriteN(pipe, body.data(), len);
    }

    json HandleRequest(const json& req) {
        int id = req.value("id", 0);
        std::string method = req.value("method", std::string());
        json params = req.contains("params") ? req["params"] : json::object();
        if (!params.is_object()) params = json::object();

        json env;
        if (!TryDispatchOffThread(method, params, env)) {
            // Route to the main-thread pump and wait for the result.
            auto fut = Queue().Submit(method, params);
            WakePump(); // nudge the window subclass so the agent loads promptly
            if (fut.wait_for(std::chrono::seconds(8)) == std::future_status::ready) {
                env = fut.get();
            } else {
                env = json{{"ok", false},
                           {"error", "timeout waiting for game main thread (is it rendering?)"}};
            }
        }

        env["id"] = id;
        return env;
    }

    void ServeClient(HANDLE pipe) {
        while (g_running.load()) {
            std::string frame;
            if (!ReadFrame(pipe, frame)) return;

            json resp;
            try {
                json req = json::parse(frame);
                resp = HandleRequest(req);
            } catch (const std::exception& e) {
                resp = json{{"ok", false}, {"error", std::string("bad request: ") + e.what()}, {"id", 0}};
            }

            if (!WriteFrame(pipe, resp.dump())) return;
        }
    }

    void ServerLoop() {
        MCP_LOG("IPC server thread started");
        while (g_running.load()) {
            HANDLE pipe = CreateNamedPipeA(
                GMOD_MCP_PIPE_NAME,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                64 * 1024, 64 * 1024, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                MCP_ERR("CreateNamedPipe failed");
                Sleep(500);
                continue;
            }

            // Blocks until a client connects (or StopIpcServer pokes us with a
            // throwaway connection).
            BOOL connected = ConnectNamedPipe(pipe, nullptr);
            if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
                CloseHandle(pipe);
                continue;
            }
            if (!g_running.load()) {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                break;
            }

            ServeClient(pipe);

            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
        MCP_LOG("IPC server thread stopping");
    }
}

void StartIpcServer() {
    if (g_running.exchange(true)) return;
    g_thread = std::thread(ServerLoop);
}

void StopIpcServer() {
    if (!g_running.exchange(false)) return;
    // Wake a blocked ConnectNamedPipe by briefly connecting as a client.
    HANDLE poke = CreateFileA(GMOD_MCP_PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (poke != INVALID_HANDLE_VALUE) CloseHandle(poke);
    if (g_thread.joinable()) g_thread.join();
}

} // namespace mcp
