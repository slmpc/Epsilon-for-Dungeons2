// ============================================================================
//  payload.cpp — 输出通道实现
//
//  emit() 的行为取决于当前 Sink:
//    * console : 直接写注入体自己控制台的 stdout
//    * pipe    : 攒进响应缓冲, 一条命令结束后整体发一个管道消息
//    * none    : 丢弃(绝不因为没地方写就崩)
// ============================================================================
#include "payload/Payload.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mutex>

namespace epsilon::payload {
namespace {

std::mutex gEmitMu;

// 文件日志句柄。放在最前面: emit() 需要把命令输出镜像落盘(见 mirrorToLogFile),
// 而 emit() 定义在这两个变量之后是不能用的 —— 所以它们必须在这里先声明。
std::mutex gFileMu;
HANDLE     gLogFile = INVALID_HANDLE_VALUE;
std::string gLogPath;

// 把一段文本落到文件日志。调用方需已持有 gFileMu。
void appendToLogFileLocked(std::string_view text) {
    if (text.empty() || gLogFile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    ::WriteFile(gLogFile, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

// 把一条已生成的输出同时落到文件日志。
//
// ⚠️ 为什么需要: 命令输出原本**只**走管道。注入器一旦退出(比如 `--exec` 跑完
// 就关), 还在路上的响应就彻底丢了 —— 排查时表现为"命令执行了但没有任何输出",
// 让人误以为是命令本身失败。实测被这个坑掉过一轮: findprop 的输出一个字都没
// 留下, 分不清是没命中还是根本没跑。
//
// 落盘之后, 无论注入器是否还在, 命令结果都能从
//     %TEMP%\epsilonPayload_<pid>_<base>.log
// 里读回来。这条路径不依赖任何对端存活。
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
    // 先落盘再发管道: 落盘不会失败到需要回滚, 而管道可能对端已经没了。
    // 这样"命令有没有输出"这件事不再依赖注入器是否还活着。
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
    // Sink::none: 只落盘, 不送管道 —— 绝不因为没地方写就崩。
}

void emitLine(std::string_view text) {
    if (!text.empty()) emit(text);
    emit("\n");
}

// 把一条已生成的输出同时落到文件日志。
//
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

// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
//  文件日志
// ---------------------------------------------------------------------------
namespace {

// gFileMu / gLogFile / gLogPath 的文件作用域声明已移到文件顶部 ——
// emit() 需要用它们做输出镜像, 而 emit() 定义在前面。

std::string makeLogPath() {
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = ::GetTempPathW(static_cast<DWORD>(std::size(tmp)), tmp);
    std::string dir = (n > 0) ? toUtf8(std::wstring_view(tmp, n)) : std::string(".");
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
    // 先尝试送出, 再把"送没送出去"一起写进文件。
    // 排查"注入器收不到消息"这类问题时, 这一位信息是决定性的:
    // 它把"注入体没写"和"写了但没到"两种情况直接分开。
    const bool sent = emitNow(proto::Kind::status, msg);

    {
        std::lock_guard lk(gFileMu);
        if (gLogFile != INVALID_HANDLE_VALUE) {
            // 每条都 flush: 如果下一步就崩了, 日志必须已经落盘。
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
