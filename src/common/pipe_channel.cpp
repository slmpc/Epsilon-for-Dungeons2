// ============================================================================
//  pipe_channel.cpp — 双向命名管道消息通道
//
//  帧格式: [12 字节头][length 字节负载], 一次 WriteFile 写完整条消息。
//
//  实现要点(踩过的坑都在这):
//    1. 用**字节模式**管道, 不用 PIPE_TYPE_MESSAGE。
//       消息模式下 WriteFile 语义要求每次写完整消息, 且跨 WriteFile 的消息
//       长度若超出缓冲区会被拒(ERROR_MORE_DATA 处理起来很碎)。
//       字节模式 + "先 Peek 够整条再一次性读" 更简单也更稳:
//       PeekNamedPipe 能告诉我们"当前可读多少字节", 而 PeekNamedPipe 带缓冲区
//       时会把数据**拷贝出来但不消费**, 于是可以先看头拿到长度, 等负载到齐
//       再一次性 ReadFile。完全避免半包。
//    2. ConnectNamedPipe 必须放在**独立线程**里。
//       它阻塞直到客户端连接。如果主线程同步调用, 调用方就永远等不到自己的
//       超时逻辑 —— 之前注入器卡死就是这个原因。
//    3. 停止时必须能打断阻塞中的 ConnectNamedPipe。
//       Windows 的 ConnectNamedPipe 没有超时参数, 关句柄也未必可靠,
//       所以 stop() 里往自己的管道发一次连接把它"顶"出来。这是标准做法。
// ============================================================================
#include "common/pipe_channel.h"
#include "common/text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace mcd2 {
namespace {

constexpr DWORD kBufSize = 64 * 1024;

HANDLE as_handle(void* p) { return static_cast<HANDLE>(p); }
void*  as_void(HANDLE h)  { return reinterpret_cast<void*>(h); }

std::wstring utf8_to_wide_local(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// 探测管道上是否已有**一条完整消息**。
//   返回 SIZE_MAX = 管道出错/断开(调用方应结束)
//   返回 0        = 暂时没有完整消息(调用方应稍后再看)
//   返回 n        = 一条完整消息的总字节数(头 + 负载)
size_t pending_message_size(HANDLE pipe) {
    DWORD avail = 0;
    if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) return SIZE_MAX;
    if (avail < sizeof(proto::Header)) return 0;

    // Peek 带缓冲区 = 拷贝但不消费, 所以这里读头不会破坏后续读负载。
    proto::Header hdr{};
    DWORD peeked = 0;
    if (!::PeekNamedPipe(pipe, &hdr, sizeof(hdr), &peeked, nullptr, nullptr)) return SIZE_MAX;
    if (peeked < sizeof(hdr)) return 0;

    if (hdr.magic != proto::kMagic || hdr.version != proto::kVersion) return SIZE_MAX;
    if (hdr.length > proto::kMaxPayload) return SIZE_MAX;

    const size_t total = sizeof(proto::Header) + hdr.length;
    if (avail < total) return 0;      // 负载还没写全, 等下一轮
    return total;
}

// 把整条消息取出来。buf 必须至少有 size 字节空间。
bool take_message(HANDLE pipe, size_t size, std::vector<char>& buf, proto::Kind& kind,
                  std::string_view& body) {
    if (buf.size() < size) buf.resize(size);
    DWORD got = 0;
    if (!::ReadFile(pipe, buf.data(), static_cast<DWORD>(size), &got, nullptr)) return false;
    if (got < sizeof(proto::Header)) return false;

    proto::Header hdr{};
    std::memcpy(&hdr, buf.data(), sizeof(hdr));
    const size_t body_len = got - sizeof(hdr);
    kind = static_cast<proto::Kind>(hdr.kind);
    body = std::string_view(buf.data() + sizeof(hdr), body_len);
    return true;
}

// 组一条消息并一次写出。
bool write_message(HANDLE pipe, proto::Kind kind, std::string_view payload) {
    if (!pipe || payload.size() > proto::kMaxPayload) return false;

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

std::wstring make_pipe_name(std::wstring_view prefix) {
    static std::atomic<uint32_t> counter{0};
    const uint32_t n = counter.fetch_add(1, std::memory_order_relaxed);
    wchar_t buf[128]{};
    ::swprintf_s(buf, L"%.*s.%u.%u", static_cast<int>(prefix.size()), prefix.data(),
                 ::GetCurrentProcessId(), n);
    return buf;
}

std::wstring default_pipe_name(uint32_t target_pid) {
    wchar_t buf[64]{};
    ::swprintf_s(buf, L"MCD2HotInject.%u", target_pid);
    return buf;
}

std::wstring pipe_full_path(std::wstring_view name) {
    std::wstring p = L"\\\\.\\pipe\\";
    p += name;
    return p;
}

// ===========================================================================
//  PipeServer (注入器侧)
// ===========================================================================
PipeServer::~PipeServer() { stop(); }

bool PipeServer::start(std::wstring const& pipe_name, MessageHandler on_message,
                       DisconnectHandler on_disconnect, std::string* error) {
    stop();

    name_ = pipe_name;
    on_message_ = std::move(on_message);
    on_disconnect_ = std::move(on_disconnect);
    stopping_ = false;
    connected_ = false;

    // 字节模式(PIPE_TYPE_BYTE)+ PIPE_WAIT: 见文件头注释。
    const std::wstring full = pipe_full_path(name_);
    HANDLE h = ::CreateNamedPipeW(
        full.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                  // 一次注入只需要一个实例
        kBufSize, kBufSize,
        0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = fmt("CreateNamedPipeW 失败 GetLastError={}", ::GetLastError());
        return false;
    }
    pipe_ = as_void(h);

    stop_event_ = as_void(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    connected_event_ = as_void(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop_event_ || !connected_event_) {
        if (error) *error = "CreateEventW 失败";
        close_handles();
        return false;
    }

    // 连接 + 读循环放进后台线程: ConnectNamedPipe 会阻塞, 不能占着调用方。
    HANDLE t = ::CreateThread(nullptr, 0,
                              reinterpret_cast<LPTHREAD_START_ROUTINE>(&acceptor_trampoline),
                              this, 0, nullptr);
    if (!t) {
        if (error) *error = fmt("CreateThread 失败 GetLastError={}", ::GetLastError());
        close_handles();
        return false;
    }
    ::SetThreadDescription(t, L"mcd2-pipe-acceptor");
    acceptor_ = as_void(t);
    return true;
}

unsigned long __stdcall PipeServer::acceptor_trampoline(void* self) {
    static_cast<PipeServer*>(self)->acceptor_loop();
    return 0;
}

void PipeServer::acceptor_loop() {
    // ---- 1) 等客户端连接 ----
    const BOOL ok = ::ConnectNamedPipe(as_handle(pipe_), nullptr);
    const DWORD err = ::GetLastError();

    // ERROR_PIPE_CONNECTED: 客户端在 ConnectNamedPipe 之前就来了 —— 同样是成功。
    const bool connected = ok || err == ERROR_PIPE_CONNECTED;
    if (!connected || stopping_.load(std::memory_order_acquire)) {
        if (!stopping_.load()) {
            connected_.store(false);
            ::SetEvent(as_handle(connected_event_));   // 让等待方别干等到超时
            if (on_disconnect_) on_disconnect_();
        }
        return;
    }

    connected_.store(true, std::memory_order_release);
    ::SetEvent(as_handle(connected_event_));

    // ---- 2) 进入读循环 ----
    reader_loop();

    // ---- 3) 断开 ----
    const bool was_connected = connected_.exchange(false);
    if (was_connected && on_disconnect_) on_disconnect_();
}

void PipeServer::reader_loop() {
    std::vector<char> buf(kBufSize);
    while (!stopping_.load(std::memory_order_acquire)) {
        const size_t avail = pending_message_size(as_handle(pipe_));
        if (avail == SIZE_MAX) break;          // 出错/断开
        if (avail == 0) {
            // 没有完整消息。用 stop_event 做可中断的小睡, 避免空转烧 CPU。
            if (::WaitForSingleObject(as_handle(stop_event_), 15) == WAIT_OBJECT_0) break;
            continue;
        }

        proto::Kind kind{};
        std::string_view body;
        if (!take_message(as_handle(pipe_), avail, buf, kind, body)) break;
        if (on_message_) on_message_(kind, body);
    }
}

bool PipeServer::wait_for_client(uint32_t timeout_ms) {
    if (!pipe_ || !connected_event_) return false;

    // 连接由 acceptor 线程负责, 这里只等它把事件点亮 —— 所以超时是可靠的。
    HANDLE waits[2] = {as_handle(connected_event_), as_handle(stop_event_)};
    const DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, timeout_ms);
    if (w == WAIT_OBJECT_0) return connected_.load(std::memory_order_acquire);
    return false;
}

bool PipeServer::send(proto::Kind kind, std::string_view payload) {
    if (!pipe_ || !connected_.load(std::memory_order_acquire)) return false;
    return write_message(as_handle(pipe_), kind, payload);
}

bool PipeServer::send_command(std::string_view line) { return send(proto::Kind::command, line); }

void PipeServer::close_handles() {
    if (pipe_) {
        ::DisconnectNamedPipe(as_handle(pipe_));
        ::CloseHandle(as_handle(pipe_));
        pipe_ = nullptr;
    }
    if (stop_event_) { ::CloseHandle(as_handle(stop_event_)); stop_event_ = nullptr; }
    if (connected_event_) { ::CloseHandle(as_handle(connected_event_)); connected_event_ = nullptr; }
}

void PipeServer::stop() {
    if (!pipe_ && !stop_event_ && !acceptor_) return;

    stopping_.store(true, std::memory_order_release);
    if (stop_event_) ::SetEvent(as_handle(stop_event_));

    // 关键: acceptor 可能正阻塞在 ConnectNamedPipe 上。
    // Windows 上它没有超时参数, 关句柄的行为也没保证, 所以往自己管道发一次
    // 连接把这个调用"顶"出去 —— 一旦连上, ConnectNamedPipe 立刻返回。
    if (pipe_ && !connected_.load(std::memory_order_acquire)) {
        HANDLE self = ::CreateFileW(pipe_full_path(name_).c_str(),
                                    GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
        if (self != INVALID_HANDLE_VALUE) {
            ::WaitForSingleObject(as_handle(acceptor_), 500);   // 等 acceptor 退出
            ::CloseHandle(self);
        }
    }

    if (acceptor_) {
        ::WaitForSingleObject(as_handle(acceptor_), 2000);
        ::CloseHandle(as_handle(acceptor_));
        acceptor_ = nullptr;
    }
    close_handles();
    connected_.store(false);
}

// ===========================================================================
//  PipeClient (注入体侧)
// ===========================================================================
PipeClient::~PipeClient() { close(); }

bool PipeClient::connect(std::wstring const& pipe_name, uint32_t retry_ms, std::string* error) {
    close();
    const std::wstring full = pipe_full_path(pipe_name);
    const ULONGLONG deadline = ::GetTickCount64() + retry_ms;
    DWORD last_err = 0;

    for (;;) {
        HANDLE h = ::CreateFileW(full.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            ::SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
            pipe_ = as_void(h);
            stop_event_ = as_void(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
            return true;
        }

        last_err = ::GetLastError();
        if (last_err != ERROR_PIPE_BUSY && last_err != ERROR_FILE_NOT_FOUND) break;
        if (::GetTickCount64() >= deadline) break;
        if (last_err == ERROR_PIPE_BUSY) ::WaitNamedPipeW(full.c_str(), 200);
        else ::Sleep(50);
    }

    if (error) *error = fmt("连接管道失败 GetLastError={}", last_err);
    return false;
}

bool PipeClient::send(proto::Kind kind, std::string_view payload) {
    // 注入体会从多个线程发消息(命令响应 / 帧回调 / 日志), 必须串行化,
    // 否则两次 WriteFile 会交错成垃圾。
    static std::mutex wmu;
    std::lock_guard lk(wmu);
    if (!pipe_) return false;
    return write_message(as_handle(pipe_), kind, payload);
}

bool PipeClient::recv(proto::Kind& kind, std::string& payload, uint32_t timeout_ms) {
    if (!pipe_) return false;
    const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
    std::vector<char> buf(kBufSize);

    for (;;) {
        const size_t avail = pending_message_size(as_handle(pipe_));
        if (avail == SIZE_MAX) return false;
        if (avail > 0) {
            std::string_view body;
            if (!take_message(as_handle(pipe_), avail, buf, kind, body)) return false;
            payload.assign(body);
            return true;
        }
        if (::WaitForSingleObject(as_handle(stop_event_), 10) == WAIT_OBJECT_0) return false;
        if (::GetTickCount64() >= deadline) return false;
    }
}

unsigned long __stdcall PipeClient::reader_trampoline(void* self) {
    static_cast<PipeClient*>(self)->reader_loop();
    return 0;
}

bool PipeClient::start_reader(std::function<void(proto::Kind, std::string_view)> on_message) {
    if (!pipe_) return false;
    on_message_ = std::move(on_message);
    HANDLE t = ::CreateThread(nullptr, 0,
                              reinterpret_cast<LPTHREAD_START_ROUTINE>(&reader_trampoline),
                              this, 0, nullptr);
    if (!t) return false;
    ::SetThreadDescription(t, L"mcd2-pipe-reader");
    reader_ = as_void(t);
    return true;
}

void PipeClient::reader_loop() {
    std::vector<char> buf(kBufSize);
    for (;;) {
        const size_t avail = pending_message_size(as_handle(pipe_));
        if (avail == SIZE_MAX) break;             // 出错/断开
        if (avail == 0) {
            if (::WaitForSingleObject(as_handle(stop_event_), 15) == WAIT_OBJECT_0) break;
            continue;
        }
        proto::Kind kind{};
        std::string_view body;
        if (!take_message(as_handle(pipe_), avail, buf, kind, body)) break;
        if (on_message_) on_message_(kind, body);
    }
}

void PipeClient::close() {
    if (stop_event_) ::SetEvent(as_handle(stop_event_));
    // 关掉管道句柄会让阻塞中的 ReadFile/PeekNamedPipe 立刻失败返回, 读线程随之退出。
    if (pipe_) {
        ::CloseHandle(as_handle(pipe_));
        pipe_ = nullptr;
    }
    if (reader_) {
        ::WaitForSingleObject(as_handle(reader_), 2000);
        ::CloseHandle(as_handle(reader_));
        reader_ = nullptr;
    }
    if (stop_event_) { ::CloseHandle(as_handle(stop_event_)); stop_event_ = nullptr; }
}

} // namespace mcd2
