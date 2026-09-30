// ============================================================================
//  runtime.cpp — 注入体运行时总控
//
//  输出通道决策顺序:
//    1. 环境变量 EPSILON_PIPE_NAME 存在且连得上 → 走管道, UI 在注入器的终端里
//    2. 否则 → AllocConsole() 自己开一个控制台窗口, 直接读 stdin
//    3. 都不行 → Sink::none, 静默(但所有命令仍可经管道后补连接)
//
//  为什么不在 DllMain 里做这些: 注入线程持有加载器锁, 在锁内创建工作线程/
//  初始化 CRT/加载 DLL 都可能死锁。DllMain 只负责 CreateThread 然后立刻返回。
// ============================================================================
#include "payload/Runtime.h"

#include "common/Text.h"
#include "payload/Hooks.h"
#include "payload/Payload.h"
#include "payload/PipeClient.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace epsilon::payload {
namespace {

std::unique_ptr<ue::Engine> gEngine;
std::unique_ptr<CommandServer> gServer;
std::atomic<bool> gReady{false};
std::atomic<bool> gStopping{false};
std::mutex gLifecycleMu;

uint64_t     gModuleBase = 0;
uint64_t     gModuleSize = 0;
std::wstring gModulePath;

// --------------------------------------------------------------------------
// 帧回调: 故意保持极轻。只记帧数, 并在第一帧打一行日志,
// 让人确认"钩子真的挂上了、游戏在出帧"。
// --------------------------------------------------------------------------
std::atomic<bool> gFirstFrameLogged{false};

void onFrame(uint32_t frameIndex, int /*syncInterval*/, int /*flags*/) {
    if (frameIndex == 1 && !gFirstFrameLogged.exchange(true)) {
        trace("Present 钩子已生效, 游戏正在出帧");
    }
}

} // namespace

// ---------------------------------------------------------------------------
ue::Engine& engine() {
    static ue::Engine fallback;
    return gEngine ? *gEngine : fallback;
}

bool runtimeReady() { return gReady.load(std::memory_order_acquire); }

bool installFrameHook() {
    if (hooks().installed()) return true;
    if (!hooks().install()) return false;
    hooks().setFrameCallback(&onFrame);
    return true;
}

uint64_t moduleBase() { return gModuleBase; }
uint64_t moduleSize() { return gModuleSize; }
std::wstring const& modulePath() { return gModulePath; }

void setModuleInfo(uint64_t base, uint64_t size, std::wstring path) {
    gModuleBase = base;
    gModuleSize = size;
    gModulePath = std::move(path);
}

// ---------------------------------------------------------------------------
unsigned long __stdcall runtimeMain(void* /*param*/) {
    ::SetThreadDescription(::GetCurrentThread(), L"epsilon-runtime");

    // 文件日志最先开 —— 后面每一步都要留痕, 崩了才查得到。
    logFileOpen();
    trace(fmt("runtimeMain 进入 (pid {}, 模块基址 {}, 大小 {})",
              ::GetCurrentProcessId(), hex(gModuleBase, 16), humanBytes(gModuleSize)));

    // ---- 1) 决定输出通道 ----
    auto& c = ctx();
    c.pid = ::GetCurrentProcessId();
    c.moduleBase = gModuleBase;
    c.moduleSize = gModuleSize;
    c.modulePath = gModulePath;

    trace("正在连接注入器管道...");
    std::string pipeErr;
    if (connectInjectorPipe(3000, &pipeErr)) {
        c.sink = Sink::pipe;
        c.verbose = true;
        trace("已连接注入器管道, 输出走管道");
    } else {
        // 管道没连上: 不弹控制台(don't add fallback paths), 只留文件日志。
        // 注入体静默驻留, 日志里能看到原因。
        c.sink = Sink::none;
        trace(fmt("管道连接失败({}) —— 进入静默模式, 只有文件日志", pipeErr));
        trace(fmt("日志文件: {}", logFilePath()));
    }

    trace("正在定位引擎全局(GObjects / GNames)...");

    // ---- 2) 定位引擎 ----
    gEngine = std::make_unique<ue::Engine>();
    gEngine->setModule(gModuleBase, gModuleSize, gModulePath);

    bool ok = false;
    for (int attempt = 1; attempt <= 3 && !ok; ++attempt) {
        trace(fmt("引擎定位 第 {} 次尝试...", attempt));
        ok = gEngine->init();
        if (!ok) {
            logWarn(fmt("第 {} 次定位失败(游戏可能仍在加载), 1 秒后重试...", attempt));
            ::Sleep(1000);
        }
    }
    trace(ok ? "引擎定位成功" : "引擎定位失败(降级为元命令模式)");

    // ---- 3) 命令服务 ----
    gServer = std::make_unique<CommandServer>(*gEngine);

    if (ok) {
        logInfo("定位成功:");
        emit(gEngine->report());
    } else {
        logWarn("未能定位引擎 → 只能使用 rescan/status/help 等元命令");
    }

    // ---- 4) 帧钩子:**不自动安装** ----
    // 数据采集不依赖帧回调, 命令驱动已经够用。帧钩子要用 `hook` 命令显式触发。
    // (建临时 D3D 交换链取 Present 在普通进程里验证通过, 但在被注入的 DLL
    //  上下文里会把目标带崩 —— 尚未定位, 所以不放在必经路径上。)
    trace("帧钩子未自动安装(如需请用 hook 命令)");

    gReady.store(true, std::memory_order_release);
    trace("初始化完成, 进入命令循环");

    // ---- 5) 进入命令循环 ----
    if (c.sink == Sink::pipe) {
        // 命令来自注入器。先握手, 再报就绪。
        proto::Hello hello;
        hello.pid = c.pid;
        hello.moduleBase = c.moduleBase;
        hello.moduleSize = c.moduleSize;
        hello.imageBase = 0;
        hello.protocol = proto::versionValue;
        constexpr char kTag[] = "epsilonPayload";
        static_assert(sizeof(kTag) <= sizeof(hello.tag));
        std::memcpy(hello.tag, kTag, sizeof(kTag));

        c.pipe.sendPod(proto::Kind::hello, hello);
        c.pipe.send(proto::Kind::ready, ok ? "ready" : "ready-no-engine");

        pipeStartCommandReader([](std::string_view cmd) {
            if (!gServer) return;
            beginResponse();
            const bool cont = gServer->execute(cmd);
            endResponse();
            if (!cont) shutdownRuntime();
        });

        // 保持线程存活, 直到 shutdown。
        while (!gStopping.load(std::memory_order_acquire)) ::Sleep(50);
    } else {
        // 管道没连上: 静默驻留。这里是**唯一**的等待逻辑, 不做"稍后补连"的
        // 复杂补救 —— 注入器没起来就是没起来, 日志里有记录。
        trace("静默驻留(未连上注入器, 命令不可用)");
        while (!gStopping.load(std::memory_order_acquire)) ::Sleep(200);
    }

    return 0;
}

void shutdownRuntime() {
    std::lock_guard lk(gLifecycleMu);
    if (gStopping.exchange(true)) return;

    // 注意: 这里**不卸载钩子, 也不 FreeLibrary**。
    // 注入体一旦挂钩子/起线程, 半途卸载会留下悬空回调 → 目标必崩。
    // 本框架只做注入, 不做卸载; 要清掉注入体就重启目标进程。
    trace("运行时停止(注入体仍驻留, 重启目标进程可清除)");
    pipeClose();
}

} // namespace epsilon::payload
