// PipeChannel.cpp — 管道消息通道实现。帧格式: [12 字节头][length 字节负载], 一次
// WriteFile 写完整条消息; 用字节模式管道 + PeekNamedPipe 凑齐整条再读, 避免半包。
#include "common/PipeChannel.h"
#include "common/Text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>   // _beginthreadex

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace epsilon {
namespace {

constexpr DWORD bufSize = 64 * 1024;

HANDLE asHandle(void* p) { return static_cast<HANDLE>(p); }
void*  asVoid(HANDLE h)  { return reinterpret_cast<void*>(h); }

std::wstring utf8ToWideLocal(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// 探测管道上是否已有一条完整消息: SIZE_MAX = 出错/断开, 0 = 还不完整, n = 消息总字节数。
size_t pendingMessageSize(HANDLE pipe) {
    DWORD avail = 0;
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) return SIZE_MAX;
    if (avail < sizeof(proto::Header)) return 0;

    // Peek 带缓冲区 = 拷贝但不消费, 所以这里读头不会破坏后续读负载。
    proto::Header hdr{};
    DWORD peeked = 0;
    if (!::PeekNamedPipe(pipe, &hdr, sizeof(hdr), &peeked, nullptr, nullptr)) return SIZE_MAX;
    if (peeked < sizeof(hdr)) return 0;

    if (hdr.magic != proto::magicValue || hdr.version != proto::versionValue) return SIZE_MAX;
    if (hdr.length > proto::maxPayload) return SIZE_MAX;

    const size_t total = sizeof(proto::Header) + hdr.length;
    if (avail < total) return 0;
    return total;
}

// 把整条消息取出来。buf 必须至少有 size 字节空间。
bool takeMessage(HANDLE pipe, size_t size, std::vector<char>& buf, proto::Kind& kind,
                  std::string_view& body) {
    if (buf.size() < size) buf.resize(size);
    DWORD got = 0;
    if (!::ReadFile(pipe, buf.data(), static_cast<DWORD>(size), &got, nullptr)) return false;
    if (got < sizeof(proto::Header)) return false;

    proto::Header hdr{};
    std::memcpy(&hdr, buf.data(), sizeof(hdr));
    const size_t bodyLen = got - sizeof(hdr);
    kind = static_cast<proto::Kind>(hdr.kind);
    body = std::string_view(buf.data() + sizeof(hdr), bodyLen);
    return true;
}

// 组一条消息并一次写出。
bool writeMessage(HANDLE pipe, proto::Kind kind, std::string_view payload) {
    if (!pipe || payload.size() > proto::maxPayload) return false;

    proto::Header hdr;
    hdr.kind = static_cast<uint16_t>(kind);
    hdr.length = static_cast<uint32_t>(payload.size());

    std::string msg;
    msg.resize(sizeof(hdr) + payload.size());
    std::memcpy(msg.data(), &hdr, sizeof(hdr));
    if (!payload.empty()) std::memcpy(msg.data() + sizeof(hdr), payload.data(), payload.size());

    DWORD written = 0;
    if (!::WriteFile(pipe, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr))
        return false;
    return written == msg.size();
}

} // namespace

std::wstring makePipeName(std::wstring_view prefix) {
    static std::atomic<uint32_t> counter{0};
    const uint32_t n = counter.fetch_add(1, std::memory_order_relaxed);
    wchar_t buf[128]{};
    ::swprintf_s(buf, L"%.*s.%u.%u", static_cast<int>(prefix.size()), prefix.data(),
                 ::GetCurrentProcessId(), n);
    return buf;
}

std::wstring moduleSlotFromPath(std::wstring_view pathOrName) {
    const size_t slash = pathOrName.find_last_of(L"\\/");
    std::wstring_view base =
        (slash == std::wstring_view::npos) ? pathOrName : pathOrName.substr(slash + 1);

    if (base.size() > 4) {
        const std::wstring_view ext = base.substr(base.size() - 4);
        if (ext == L".dll" || ext == L".DLL" || ext == L".Dll") {
            base = base.substr(0, base.size() - 4);
        }
    }

    // 只留字母数字 —— 这个名字会进管道名与互斥体名。
    std::wstring out;
    out.reserve(base.size());
    for (wchar_t c : base) {
        if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
            (c >= L'A' && c <= L'Z')) {
            out.push_back(c);
        }
    }
    return out;
}

std::wstring defaultPipeName(uint32_t targetPid, std::wstring_view slot) {
    wchar_t buf[160]{};
    const std::wstring s(slot);
    if (s.empty()) {
        ::swprintf_s(buf, L"EpsilonHotPipe2.%u", targetPid);
    } else {
        ::swprintf_s(buf, L"EpsilonHotPipe2.%u.%s", targetPid, s.c_str());
    }
    return buf;
}

std::wstring pipeFullPath(std::wstring_view name) {
    std::wstring p = L"\\\\.\\pipe\\";
    p += name;
    return p;
}

PipeServer::~PipeServer() { stop(); }

bool PipeServer::start(std::wstring const& pipeName, MessageHandler onMessage,
                       DisconnectHandler onDisconnect, std::string* error) {
    stop();

    name_ = pipeName;
    on_message_ = std::move(onMessage);
    on_disconnect_ = std::move(onDisconnect);
    stopping_ = false;
    connected_ = false;

    // 字节模式 + PIPE_WAIT: 消息模式下的半包处理不划算(见文件头)。
    const std::wstring full = pipeFullPath(name_);
    HANDLE h = ::CreateNamedPipeW(
        full.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        bufSize, bufSize,
        0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = fmt("CreateNamedPipeW 失败 GetLastError={}", ::GetLastError());
        return false;
    }
    pipe_ = asVoid(h);

    stopEvent_ = asVoid(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    connectedEvent_ = asVoid(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent_ || !connectedEvent_) {
        if (error) *error = "CreateEventW 失败";
        closeHandles();
        return false;
    }

    // 线程里会用 CRT(std::string / std::format / std::mutex), 必须 _beginthreadex 而非 CreateThread。
    uintptr_t t = _beginthreadex(
        nullptr, 0,
        reinterpret_cast<unsigned(__stdcall*)(void*)>(&acceptorTrampoline),
        this, 0, nullptr);
    if (!t) {
        if (error) *error = fmt("_beginthreadex 失败 errno={}", errno);
        closeHandles();
        return false;
    }
    ::SetThreadDescription(reinterpret_cast<HANDLE>(t), L"epsilon-pipe-acceptor");
    acceptor_ = asVoid(reinterpret_cast<HANDLE>(t));
    return true;
}

unsigned long __stdcall PipeServer::acceptorTrampoline(void* self) {
    static_cast<PipeServer*>(self)->acceptorLoop();
    return 0;
}

void PipeServer::acceptorLoop() {
    const BOOL ok = ::ConnectNamedPipe(asHandle(pipe_), nullptr);
    const DWORD err = ::GetLastError();

    const bool connected = ok || err == ERROR_PIPE_CONNECTED;
    if (!connected || stopping_.load(std::memory_order_acquire)) {
        if (!stopping_.load()) {
            connected_.store(false);
            ::SetEvent(asHandle(connectedEvent_));
            if (on_disconnect_) on_disconnect_();
        }
        return;
    }

    connected_.store(true, std::memory_order_release);
    ::SetEvent(asHandle(connectedEvent_));

    readerLoop();

    const bool wasConnected = connected_.exchange(false);
    if (wasConnected && on_disconnect_) on_disconnect_();
}

void PipeServer::readerLoop() {
    std::vector<char> buf(bufSize);
    while (!stopping_.load(std::memory_order_acquire)) {
        const size_t avail = pendingMessageSize(asHandle(pipe_));
        if (avail == SIZE_MAX) break;
        if (avail == 0) {
            // 可中断的小睡, 避免空转烧 CPU。
            if (::WaitForSingleObject(asHandle(stopEvent_), 15) == WAIT_OBJECT_0) break;
            continue;
        }

        proto::Kind kind{};
        std::string_view body;
        if (!takeMessage(asHandle(pipe_), avail, buf, kind, body)) break;
        if (on_message_) on_message_(kind, body);
    }
}

bool PipeServer::waitForClient(uint32_t timeoutMs) {
    if (!pipe_ || !connectedEvent_) return false;

    HANDLE waits[2] = {asHandle(connectedEvent_), asHandle(stopEvent_)};
    const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, timeoutMs);
    if (w == WAIT_OBJECT_0) return connected_.load(std::memory_order_acquire);
    return false;
}

bool PipeServer::send(proto::Kind kind, std::string_view payload) {
    if (!pipe_ || !connected_.load(std::memory_order_acquire)) return false;
    return writeMessage(asHandle(pipe_), kind, payload);
}

bool PipeServer::sendCommand(std::string_view line) { return send(proto::Kind::command, line); }

void PipeServer::closeHandles() {
    if (pipe_) {
        ::DisconnectNamedPipe(asHandle(pipe_));
        ::CloseHandle(asHandle(pipe_));
        pipe_ = nullptr;
    }
    if (stopEvent_) { ::CloseHandle(asHandle(stopEvent_)); stopEvent_ = nullptr; }
    if (connectedEvent_) { ::CloseHandle(asHandle(connectedEvent_)); connectedEvent_ = nullptr; }
}

void PipeServer::stop() {
    if (!pipe_ && !stopEvent_ && !acceptor_) return;

    stopping_.store(true, std::memory_order_release);
    if (stopEvent_) ::SetEvent(asHandle(stopEvent_));

    // acceptor 可能正阻塞在 ConnectNamedPipe 上, 而它没有超时参数: 往自己的管道
    // 发一次连接把该调用"顶"出来, 否则 stop() 会挂住。
    if (pipe_ && !connected_.load(std::memory_order_acquire)) {
        HANDLE self = ::CreateFileW(pipeFullPath(name_).c_str(),
                                    GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        if (self != INVALID_HANDLE_VALUE) {
            ::WaitForSingleObject(asHandle(acceptor_), 500);
            ::CloseHandle(self);
        }
    }

    if (acceptor_) {
        ::WaitForSingleObject(asHandle(acceptor_), 2000);
        ::CloseHandle(asHandle(acceptor_));
        acceptor_ = nullptr;
    }
    closeHandles();
    connected_.store(false);
}

PipeClient::~PipeClient() { close(); }

bool PipeClient::connect(std::wstring const& pipeName, uint32_t retryMs, std::string* error) {
    close();
    const std::wstring full = pipeFullPath(pipeName);
    const ULONGLONG deadline = ::GetTickCount64() + retryMs;
    DWORD lastErr = 0;

    for (;;) {
        HANDLE h = ::CreateFileW(full.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            ::SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
            pipe_ = asVoid(h);
            stopEvent_ = asVoid(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
            return true;
        }

        lastErr = ::GetLastError();
        if (lastErr != ERROR_PIPE_BUSY && lastErr != ERROR_FILE_NOT_FOUND) break;
        if (::GetTickCount64() >= deadline) break;
        if (lastErr == ERROR_PIPE_BUSY) ::WaitNamedPipeW(full.c_str(), 200);
        else ::Sleep(50);
    }

    if (error) *error = fmt("连接管道失败 GetLastError={}", lastErr);
    return false;
}

bool PipeClient::send(proto::Kind kind, std::string_view payload) {
    // 注入体会从多个线程发消息, 必须串行化, 否则两次 WriteFile 会交错成垃圾。
    static std::mutex wmu;
    std::lock_guard lk(wmu);
    if (!pipe_) return false;
    return writeMessage(asHandle(pipe_), kind, payload);
}

bool PipeClient::recv(proto::Kind& kind, std::string& payload, uint32_t timeoutMs) {
    if (!pipe_) return false;
    const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
    std::vector<char> buf(bufSize);

    for (;;) {
        const size_t avail = pendingMessageSize(asHandle(pipe_));
        if (avail == SIZE_MAX) return false;
        if (avail > 0) {
            std::string_view body;
            if (!takeMessage(asHandle(pipe_), avail, buf, kind, body)) return false;
            payload.assign(body);
            return true;
        }
        if (::WaitForSingleObject(asHandle(stopEvent_), 10) == WAIT_OBJECT_0) return false;
        if (::GetTickCount64() >= deadline) return false;
    }
}

unsigned long __stdcall PipeClient::readerTrampoline(void* self) {
    static_cast<PipeClient*>(self)->readerLoop();
    return 0;
}

bool PipeClient::startReader(std::function<void(proto::Kind, std::string_view)> onMessage) {
    if (!pipe_) return false;
    on_message_ = std::move(onMessage);
    // 同上: 读线程也碰 CRT, 用 _beginthreadex。
    uintptr_t t = _beginthreadex(
        nullptr, 0,
        reinterpret_cast<unsigned(__stdcall*)(void*)>(&readerTrampoline),
        this, 0, nullptr);
    if (!t) return false;
    ::SetThreadDescription(reinterpret_cast<HANDLE>(t), L"epsilon-pipe-reader");
    reader_ = asVoid(reinterpret_cast<HANDLE>(t));
    return true;
}

void PipeClient::readerLoop() {
    std::vector<char> buf(bufSize);
    for (;;) {
        const size_t avail = pendingMessageSize(asHandle(pipe_));
        if (avail == SIZE_MAX) break;
        if (avail == 0) {
            if (::WaitForSingleObject(asHandle(stopEvent_), 15) == WAIT_OBJECT_0) break;
            continue;
        }
        proto::Kind kind{};
        std::string_view body;
        if (!takeMessage(asHandle(pipe_), avail, buf, kind, body)) break;
        if (on_message_) on_message_(kind, body);
    }
}

void PipeClient::close() {
    if (stopEvent_) ::SetEvent(asHandle(stopEvent_));
    // 关掉管道句柄会让阻塞中的 ReadFile/PeekNamedPipe 立刻失败返回, 读线程随之退出。
    if (pipe_) {
        ::CloseHandle(asHandle(pipe_));
        pipe_ = nullptr;
    }
    if (reader_) {
        ::WaitForSingleObject(asHandle(reader_), 2000);
        ::CloseHandle(asHandle(reader_));
        reader_ = nullptr;
    }
    if (stopEvent_) { ::CloseHandle(asHandle(stopEvent_)); stopEvent_ = nullptr; }
}

} // namespace epsilon
