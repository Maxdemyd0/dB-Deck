#include "PipeServer.h"

#include <sddl.h>

#include <vector>

#include "protocol/Protocol.h"
#include "util/Log.h"

namespace dbdeck::win {

namespace {

constexpr size_t kMaxClients = 8;
constexpr size_t kMaxQueuedDroppable = 64;
constexpr size_t kMaxQueued = 4096;

std::wstring currentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    UniqueHandle t(token);
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buf(size);
    if (!GetTokenInformation(token, TokenUser, buf.data(), size, &size)) return {};
    wchar_t* sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) return {};
    std::wstring s = sid;
    LocalFree(sid);
    return s;
}

}  // namespace

std::wstring defaultPipeName() { return L"\\\\.\\pipe\\dB-Deck.Engine.v1." + currentUserSid(); }

PipeServer::~PipeServer() { stop(); }

HANDLE PipeServer::createInstance(bool first) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd_, FALSE};
    const DWORD open = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    return CreateNamedPipeW(name_.c_str(), open,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
}

bool PipeServer::start(const std::wstring& name, OnMessage onMessage, OnEvent onConnect, OnEvent onDisconnect,
                       std::string& error) {
    name_ = name;
    onMessage_ = std::move(onMessage);
    onConnect_ = std::move(onConnect);
    onDisconnect_ = std::move(onDisconnect);
    const std::wstring sid = currentUserSid();
    if (sid.empty()) {
        error = "cannot determine current user SID";
        return false;
    }
    // Protected DACL: access for the current user and SYSTEM only.
    const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")(A;;GA;;;SY)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd_, nullptr)) {
        error = "security descriptor: " + hrToString(HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    // FILE_FLAG_FIRST_PIPE_INSTANCE fails if another process already owns the
    // name (duplicate engine or a squatter).
    first_ = createInstance(true);
    if (first_ == INVALID_HANDLE_VALUE) {
        error = "pipe already exists or cannot be created: " + hrToString(HRESULT_FROM_WIN32(GetLastError()));
        first_ = nullptr;
        return false;
    }
    stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    running_ = true;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void PipeServer::stop() {
    if (!running_.exchange(false)) return;
    SetEvent(stopEvent_.get());
    if (acceptThread_.joinable()) acceptThread_.join();
    {
        std::lock_guard lock(mutex_);
        for (auto& [id, c] : clients_) SetEvent(c->stopEvent.get());
    }
    reap(true);
    if (sd_) {
        LocalFree(sd_);
        sd_ = nullptr;
    }
}

void PipeServer::acceptLoop() {
    UniqueHandle connectEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    HANDLE pipe = first_;
    first_ = nullptr;
    while (running_) {
        if (!pipe) pipe = createInstance(false);
        if (pipe == INVALID_HANDLE_VALUE) {
            log::error("CreateNamedPipe failed: " + hrToString(HRESULT_FROM_WIN32(GetLastError())));
            pipe = nullptr;
            if (WaitForSingleObject(stopEvent_.get(), 1000) == WAIT_OBJECT_0) break;
            continue;
        }
        OVERLAPPED ov{};
        ov.hEvent = connectEvent.get();
        ResetEvent(connectEvent.get());
        bool connected = false;
        if (ConnectNamedPipe(pipe, &ov)) {
            connected = true;
        } else {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE hs[2] = {connectEvent.get(), stopEvent_.get()};
                const DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0) {
                    DWORD n = 0;
                    connected = GetOverlappedResult(pipe, &ov, &n, FALSE) != FALSE;
                } else {
                    CancelIoEx(pipe, &ov);
                    CloseHandle(pipe);
                    break;
                }
            }
        }
        if (!connected) {
            CloseHandle(pipe);
            pipe = nullptr;
            continue;
        }
        reap(false);
        std::shared_ptr<Client> c;
        {
            std::lock_guard lock(mutex_);
            if (clients_.size() >= kMaxClients) {
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                pipe = nullptr;
                log::warn("IPC: too many clients, connection refused");
                continue;
            }
            c = std::make_shared<Client>();
            c->id = nextId_++;
            c->pipe.reset(pipe);
            c->queueEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            c->stopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            clients_[c->id] = c;
        }
        pipe = nullptr;
        if (onConnect_) onConnect_(c->id);
        c->thread = std::thread([this, c] { clientLoop(c); });
    }
}

void PipeServer::clientLoop(std::shared_ptr<Client> c) {
    UniqueHandle readEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    UniqueHandle writeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    std::vector<char> buf(64 * 1024);
    std::string inbox;
    OVERLAPPED rov{};
    bool readPending = false;
    bool alive = true;

    while (alive && running_) {
        if (!readPending) {
            rov = OVERLAPPED{};
            rov.hEvent = readEvent.get();
            ResetEvent(readEvent.get());
            if (!ReadFile(c->pipe.get(), buf.data(), static_cast<DWORD>(buf.size()), nullptr, &rov) &&
                GetLastError() != ERROR_IO_PENDING)
                break;
            readPending = true;
        }
        HANDLE hs[3] = {readEvent.get(), c->queueEvent.get(), c->stopEvent.get()};
        const DWORD w = WaitForMultipleObjects(3, hs, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) {
            DWORD n = 0;
            if (!GetOverlappedResult(c->pipe.get(), &rov, &n, FALSE)) break;
            readPending = false;
            inbox.append(buf.data(), n);
            size_t pos;
            while ((pos = inbox.find('\n')) != std::string::npos) {
                std::string line = inbox.substr(0, pos);
                inbox.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty() && onMessage_) onMessage_(c->id, std::move(line));
            }
            if (inbox.size() > kMaxMessageBytes) {
                log::warn("IPC: client exceeded message size limit, disconnecting");
                break;
            }
        } else if (w == WAIT_OBJECT_0 + 1) {
            for (;;) {
                std::string msg;
                {
                    std::lock_guard lock(c->mutex);
                    if (c->queue.empty()) break;
                    msg = std::move(c->queue.front().first);
                    c->queue.pop_front();
                }
                msg.push_back('\n');
                OVERLAPPED wov{};
                wov.hEvent = writeEvent.get();
                ResetEvent(writeEvent.get());
                if (!WriteFile(c->pipe.get(), msg.data(), static_cast<DWORD>(msg.size()), nullptr, &wov) &&
                    GetLastError() != ERROR_IO_PENDING) {
                    alive = false;
                    break;
                }
                HANDLE ws[2] = {writeEvent.get(), c->stopEvent.get()};
                if (WaitForMultipleObjects(2, ws, FALSE, INFINITE) != WAIT_OBJECT_0) {
                    CancelIoEx(c->pipe.get(), &wov);
                    alive = false;
                    break;
                }
                DWORD n = 0;
                if (!GetOverlappedResult(c->pipe.get(), &wov, &n, FALSE)) {
                    alive = false;
                    break;
                }
            }
        } else {
            break;
        }
    }
    if (readPending) {
        CancelIoEx(c->pipe.get(), &rov);
        DWORD n = 0;
        GetOverlappedResult(c->pipe.get(), &rov, &n, TRUE);
    }
    FlushFileBuffers(c->pipe.get());
    DisconnectNamedPipe(c->pipe.get());
    c->finished = true;
    if (onDisconnect_) onDisconnect_(c->id);
}

void PipeServer::reap(bool all) {
    std::vector<std::shared_ptr<Client>> done;
    {
        std::lock_guard lock(mutex_);
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (all || it->second->finished) {
                done.push_back(it->second);
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& c : done)
        if (c->thread.joinable()) c->thread.join();
}

void PipeServer::send(uint64_t client, std::string line, bool droppable) {
    std::shared_ptr<Client> c;
    {
        std::lock_guard lock(mutex_);
        auto it = clients_.find(client);
        if (it == clients_.end() || it->second->finished) return;
        c = it->second;
    }
    {
        std::lock_guard lock(c->mutex);
        if (droppable) {
            size_t queuedDroppable = 0;
            for (auto& m : c->queue) queuedDroppable += m.second;
            if (queuedDroppable >= kMaxQueuedDroppable) return;  // client is behind: drop meters
        }
        if (c->queue.size() >= kMaxQueued) {
            SetEvent(c->stopEvent.get());  // hopelessly behind: disconnect
            return;
        }
        c->queue.emplace_back(std::move(line), droppable);
    }
    SetEvent(c->queueEvent.get());
}

void PipeServer::broadcast(const std::string& line, bool droppable) {
    std::vector<uint64_t> ids;
    {
        std::lock_guard lock(mutex_);
        for (auto& [id, c] : clients_) ids.push_back(id);
    }
    for (auto id : ids) send(id, line, droppable);
}

void PipeServer::disconnect(uint64_t client) {
    std::lock_guard lock(mutex_);
    if (auto it = clients_.find(client); it != clients_.end()) SetEvent(it->second->stopEvent.get());
}

size_t PipeServer::clientCount() const {
    std::lock_guard lock(mutex_);
    size_t n = 0;
    for (auto& [id, c] : clients_) n += !c->finished;
    return n;
}

}  // namespace dbdeck::win
