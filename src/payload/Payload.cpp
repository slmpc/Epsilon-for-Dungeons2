// Payload.cpp — 输出通道实现
// 细节见 docs/payload/output.md
#include "payload/Payload.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mutex>

namespace epsilon::payload {
namespace {

std::mutex gEmitMu;

std::mutex gFileMu;
HANDLE     gLogFile = INVALID_HANDLE_VALUE;
std::string gLogPath;

// 调用方需已持有 gFileMu。
void appendToLogFileLocked(std::string_view text) {
    if (text.empty() || gLogFile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    ::WriteFile(gLogFile, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

// 命令输出同时落盘: 注入器退出后结果仍能从日志读回, 不依赖对端存活。
void mirrorToLogFile(std::string_view text) {
    if (text.empty()) return;
    std::lock_guard lk(gFileMu);
    appendToLogFileLocked(text);
}

} // namespace

Context& ctx() {
    static Context c;
    return c;
}

void emit(std::string_view text) {
    // 先落盘再发管道 —— 管道对端可能已经没了, 落盘不会失败到需要回滚。
    mirrorToLogFile(text);
    std::lock_guard lk(gEmitMu);
    auto& c = ctx();
    if (c.pendingActive) {
        c.pending.append(text);
        return;
    }
    if (c.sink == Sink::pipe) {
        c.pipe.send(proto::Kind::data, text);
    }
}

void emitLine(std::string_view text) {
    if (!text.empty()) emit(text);
    emit("\n");
}

bool emitNow(proto::Kind kind, std::string_view text) {
    std::lock_guard lk(gEmitMu);
    auto& c = ctx();
    if (c.sink != Sink::pipe) return false;
    return c.pipe.send(kind, text);
}

void beginResponse() {
    std::lock_guard lk(gEmitMu);
    auto& c = ctx();
    c.pending.clear();
    c.pendingActive = true;
}

void endResponse() {
    std::string body;
    Sink sink;
    {
        std::lock_guard lk(gEmitMu);
        auto& c = ctx();
        c.pendingActive = false;
        body = std::move(c.pending);
        c.pending.clear();
        sink = c.sink;
    }
    if (sink == Sink::pipe && !body.empty()) {
        ctx().pipe.send(proto::Kind::data, body);
    }
}

void logInfo(std::string_view s) {
    emitNow(proto::Kind::status, fmt("[*] {}", s));
}

void logWarn(std::string_view s) {
    emitNow(proto::Kind::status, fmt("[!] {}", s));
}

void logError(std::string_view s) {
    emitNow(proto::Kind::error, fmt("[x] {}", s));
}

void logVerbose(std::string_view s) {
    if (!ctx().verbose) return;
    emitNow(proto::Kind::status, fmt("[.] {}", s));
}

// ---- 文件日志 ----
namespace {

std::string makeLogPath() {
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = ::GetTempPathW(static_cast<DWORD>(std::size(tmp)), tmp);
    std::string dir = (n > 0) ? toUtf8(std::wstring_view(tmp, n)) : std::string(".");
    if (!dir.empty() && dir.back() == '\\') dir.pop_back();

    // 文件名带本模块基址: 同一 DLL 的多个副本各写各的, 才不会撞共享冲突。
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&makeLogPath), &self);

    return fmt("{}\\epsilonPayload_{}_{:X}.log", dir, ::GetCurrentProcessId(),
               reinterpret_cast<uint64_t>(self));
}

std::string timestamp() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    return fmt("{:02}:{:02}:{:02}.{:03}", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

} // namespace

void logFileOpen() noexcept {
    std::lock_guard lk(gFileMu);
    if (gLogFile != INVALID_HANDLE_VALUE) return;

    gLogPath = makeLogPath();
    const std::wstring wpath = toUtf16(gLogPath);

    // CREATE_ALWAYS: 每次注入重新开始一份日志。
    // ⚠️ 共享模式必须同时给 READ 与 WRITE —— 同进程里已加载的旧注入体还持着
    //    这个文件的句柄, 只给 FILE_SHARE_READ 会让本次打开静默失败。
    HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    gLogFile = h;

    const std::string banner = fmt(
        "=== epsilonPayload pid={} 启动 {} ===\n",
        ::GetCurrentProcessId(), timestamp());
    DWORD written = 0;
    ::WriteFile(h, banner.data(), static_cast<DWORD>(banner.size()), &written, nullptr);
    ::FlushFileBuffers(h);
}

void logFileClose() noexcept {
    std::lock_guard lk(gFileMu);
    if (gLogFile != INVALID_HANDLE_VALUE) {
        ::CloseHandle(gLogFile);
        gLogFile = INVALID_HANDLE_VALUE;
    }
}

std::string logFilePath() noexcept {
    std::lock_guard lk(gFileMu);
    return gLogPath;
}

void trace(std::string_view msg) {
    // 先尝试送出, 再把"送没送出去"写进文件 —— 这一位把"没写"与"写了没到"分开。
    const bool sent = emitNow(proto::Kind::status, msg);

    {
        std::lock_guard lk(gFileMu);
        if (gLogFile != INVALID_HANDLE_VALUE) {
            // 每条都 flush: 下一步崩了日志也必须已经落盘。
            std::string line = fmt("[{}] {}{}\n", timestamp(),
                                   sent ? "" : "[未送出] ", msg);
            DWORD written = 0;
            ::WriteFile(gLogFile, line.data(), static_cast<DWORD>(line.size()),
                        &written, nullptr);
            ::FlushFileBuffers(gLogFile);
        }
    }
}

} // namespace epsilon::payload
