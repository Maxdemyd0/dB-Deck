#pragma once
// Local IPC server: a named pipe restricted to the current user (and
// SYSTEM), rejecting remote clients, carrying newline-delimited JSON.
// Each client has one overlapped-I/O thread and a bounded outgoing queue;
// droppable messages (meters) are discarded first when a client falls behind.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ComUtil.h"

namespace dbdeck::win {

std::wstring defaultPipeName();  // \\.\pipe\dB-Deck.Engine.v1.<user SID>

class PipeServer {
public:
    using OnMessage = std::function<void(uint64_t client, std::string line)>;
    using OnEvent = std::function<void(uint64_t client)>;

    ~PipeServer();
    bool start(const std::wstring& name, OnMessage onMessage, OnEvent onConnect, OnEvent onDisconnect,
               std::string& error);
    void stop();

    void send(uint64_t client, std::string line, bool droppable = false);
    void broadcast(const std::string& line, bool droppable = false);
    void disconnect(uint64_t client);
    size_t clientCount() const;

private:
    struct Client {
        uint64_t id = 0;
        UniqueHandle pipe;
        UniqueHandle queueEvent;
        UniqueHandle stopEvent;
        std::mutex mutex;
        std::deque<std::pair<std::string, bool>> queue;
        std::thread thread;
        std::atomic<bool> finished{false};
    };

    void acceptLoop();
    void clientLoop(std::shared_ptr<Client> c);
    void reap(bool all);
    HANDLE createInstance(bool first);

    std::wstring name_;
    OnMessage onMessage_;
    OnEvent onConnect_, onDisconnect_;
    UniqueHandle stopEvent_;
    std::thread acceptThread_;
    PSECURITY_DESCRIPTOR sd_ = nullptr;
    HANDLE first_ = nullptr;  // first pipe instance, created in start()
    mutable std::mutex mutex_;
    std::map<uint64_t, std::shared_ptr<Client>> clients_;
    uint64_t nextId_ = 1;
    std::atomic<bool> running_{false};
};

}  // namespace dbdeck::win
