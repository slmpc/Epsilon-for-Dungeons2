// ============================================================================
//  payload.cpp — 输出通道实现
//
//  emit() 的行为取决于当前 Sink:
//    * console : 直接写注入体自己控制台的 stdout
//    * pipe    : 攒进响应缓冲, 一条命令结束后整体发一个管道消息
//    * none    : 丢弃(绝不因为没地方写就崩)
// ============================================================================
#include "payload/payload.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mutex>

namespace mcd2::payload {
namespace {

std::mutex g_emit_mu;

} // namespace

Context& ctx() {
    static Context c;
    return c;
}

void emit(std::string_view text) {
    std::lock_guard lk(g_emit_mu);
    auto& c = ctx();
    if (c.pending_active) {
        c.pending.append(text);
        return;
    }
    if (c.sink == Sink::pipe) {
        c.pipe.send(proto::Kind::data, text);
    }
    // Sink::none: 丢弃。绝不因为没地方写就崩 —— 文件日志仍在记录。
}

void emit_line(std::string_view text) {
    if (!text.empty()) emit(text);
    emit("\n");
}

bool emit_now(proto::Kind kind, std::string_view text) {
    std::lock_guard lk(g_emit_mu);
    auto& c = ctx();
    if (c.sink != Sink::pipe) return false;
    return c.pipe.send(kind, text);
}

void begin_response() {
    std::lock_guard lk(g_emit_mu);
    auto& c = ctx();
    c.pending.clear();
    c.pending_active = true;
}

void end_response() {
    std::string body;
    Sink sink;
    {
        std::lock_guard lk(g_emit_mu);
        auto& c = ctx();
        c.pending_active = false;
        body = std::move(c.pending);
        c.pending.clear();
        sink = c.sink;
    }
    if (sink == Sink::pipe && !body.empty()) {
        ctx().pipe.send(proto::Kind::data, body);
    }
}

// ---------------------------------------------------------------------------
void log_info(std::string_view s) {
    emit_now(proto::Kind::status, fmt("[*] {}", s));
}

void log_warn(std::string_view s) {
    emit_now(proto::Kind::status, fmt("[!] {}", s));
}

void log_error(std::string_view s) {
    emit_now(proto::Kind::error, fmt("[x] {}", s));
}

void log_verbose(std::string_view s) {
    if (!ctx().verbose) return;
    emit_now(proto::Kind::status, fmt("[.] {}", s));
}

// ---------------------------------------------------------------------------
//  文件日志
// ---------------------------------------------------------------------------
namespace {

std::mutex g_file_mu;
HANDLE     g_log_file = INVALID_HANDLE_VALUE;
std::string g_log_path;

std::string make_log_path() {
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = ::GetTempPathW(static_cast<DWORD>(std::size(tmp)), tmp);
    std::string dir = (n > 0) ? to_utf8(std::wstring_view(tmp, n)) : std::string(".");
    if (!dir.empty() && dir.back() == '\\') dir.pop_back();
    return fmt("{}\\mcd2_payload_{}.log", dir, ::GetCurrentProcessId());
}

std::string timestamp() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return fmt("{:02}:{:02}:{:02}.{:03}", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

} // namespace

void log_file_open() noexcept {
    std::lock_guard lk(g_file_mu);
    if (g_log_file != INVALID_HANDLE_VALUE) return;

    g_log_path = make_log_path();
    const std::wstring wpath = to_utf16(g_log_path);

    // CREATE_ALWAYS: 每次注入重新开始一份, 免得新旧日志混在一起看串。
    HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    g_log_file = h;

    const std::string banner = fmt(
        "=== mcd2_payload pid={} 启动 {} ===\n",
        ::GetCurrentProcessId(), timestamp());
    DWORD written = 0;
    ::WriteFile(h, banner.data(), static_cast<DWORD>(banner.size()), &written, nullptr);
    ::FlushFileBuffers(h);
}

void log_file_close() noexcept {
    std::lock_guard lk(g_file_mu);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_log_file);
        g_log_file = INVALID_HANDLE_VALUE;
    }
}

std::string log_file_path() noexcept {
    std::lock_guard lk(g_file_mu);
    return g_log_path;
}

void trace(std::string_view msg) {
    // 先尝试送出, 再把"送没送出去"一起写进文件。
    // 排查"注入器收不到消息"这类问题时, 这一位信息是决定性的:
    // 它把"注入体没写"和"写了但没到"两种情况直接分开。
    const bool sent = emit_now(proto::Kind::status, msg);

    {
        std::lock_guard lk(g_file_mu);
        if (g_log_file != INVALID_HANDLE_VALUE) {
            // 每条都 flush: 如果下一步就崩了, 日志必须已经落盘。
            std::string line = fmt("[{}] {}{}\n", timestamp(),
                                   sent ? "" : "[未送出] ", msg);
            DWORD written = 0;
            ::WriteFile(g_log_file, line.data(), static_cast<DWORD>(line.size()),
                        &written, nullptr);
            ::FlushFileBuffers(g_log_file);
        }
    }
}

} // namespace mcd2::payload
