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

    // 文件名带上本模块的基址。
    //
    // 为什么必须唯一: 同一个 DLL 可以被注入多次(换个文件名就行), 而每个副本
    // 都会尝试打开同一个日志文件。Windows 的共享冲突判定看的是**已存在的
    // 句柄允许了什么**, 不是新打开者请求了什么 —— 只要先来的那个副本用
    // FILE_SHARE_READ 打开过, 后来者的 CREATE_ALWAYS 就永远失败, 表现为
    // "注入了但看不到任何日志"。带上模块基址, 每个副本各写各的, 彻底绕开。
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&make_log_path), &self);

    return fmt("{}\\mcd2_payload_{}_{:X}.log", dir, ::GetCurrentProcessId(),
               reinterpret_cast<uint64_t>(self));
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
    //
    // ⚠️ 共享模式必须同时给 READ 和 WRITE。只给 FILE_SHARE_READ 的话, 同一个
    //    进程里已经加载过的旧注入体还持着这个文件的句柄, 新注入体的
    //    CREATE_ALWAYS 会撞共享冲突而静默失败 —— 于是"第二次注入看不到任何
    //    日志", 排查时等于被蒙住眼睛。同一个 DLL 可以被注多次(改个文件名
    //    就行), 所以这个场景是常态而非特例。
    HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
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
