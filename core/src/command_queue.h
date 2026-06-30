// command_queue.h — thread-safe hand-off from the IPC thread to the main-thread pump.
//
// The IPC thread parses a request and submits it here, receiving a std::future.
// The pump (running inside the D3D9 Present hook, on the game's main thread)
// drains the queue and fulfils each promise. This is the ONLY place the IPC
// thread and the main thread synchronise.
#pragma once

#include <queue>
#include <mutex>
#include <future>
#include <memory>
#include <string>
#include <functional>

#include "../third_party/json.hpp"

namespace mcp {

struct Command {
    std::string method;
    nlohmann::json params;
    std::promise<nlohmann::json> promise; // fulfilled with an envelope: {ok, result|error}
};

class CommandQueue {
public:
    // Called from the IPC thread. Returns a future that the pump will fulfil.
    std::future<nlohmann::json> Submit(std::string method, nlohmann::json params) {
        auto cmd = std::make_shared<Command>();
        cmd->method = std::move(method);
        cmd->params = std::move(params);
        auto fut = cmd->promise.get_future();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            queue_.push(cmd);
        }
        return fut;
    }

    // Called from the main thread (pump). Executes `handler` for each queued
    // command. The handler is responsible for fulfilling the command's promise.
    void Drain(const std::function<void(Command&)>& handler) {
        for (;;) {
            std::shared_ptr<Command> cmd;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (queue_.empty()) return;
                cmd = queue_.front();
                queue_.pop();
            }
            handler(*cmd);
        }
    }

private:
    std::mutex mutex_;
    std::queue<std::shared_ptr<Command>> queue_;
};

} // namespace mcp
