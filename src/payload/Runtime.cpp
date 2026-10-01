#include "payload/Runtime.h"

#include "common/Text.h"
#include "payload/Hooks.h"
#include "payload/Payload.h"
#include "payload/PipeClient.h"
#include "payload/feature/GameContext.h"
#include "payload/feature/Modules.h"
#include "payload/feature/config/ConfigManager.h"
#include "payload/feature/module/ModuleManager.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>     // _beginthreadex

#include <atomic>
#include <cstdlib>   // atoi
#include <memory>
#include <mutex>
#include <string>

namespace epsilon::payload {
namespace {

std::unique_ptr<game::ue::Engine> gEngine;
std::unique_ptr<CommandServer> gServer;
std::atomic<bool> gReady{false};
std::atomic<bool> gStopping{false};
std::mutex gLifecycleMu;

// 功能线程的节拍。模块改的是"手感标量", 30 Hz 足够; 线程只碰自己的标量状态,
// 不与命令线程或渲染线程共享容器, 配置落盘也在本线程内串行完成。
constexpr DWORD featureTickIntervalMs = 33;
constexpr DWORD configSaveIntervalMs  = 5000;

HANDLE gFeatureThread = nullptr;

// 自动安装 Present 钩子的状态。
bool gAutoHookDone = false;

// 首次尝试前的等待时间: 让目标进程的 D3D / 加载状态先稳定下来。
// 这只是基于现有证据的缓解, 不是已证实的根因 —— 见 docs/payload/hooks.md。
constexpr uint64_t autoHookDelayMs = 10000;

uint64_t gAutoHookNotBefore = 0;

// 前向声明: 定义在 featureTickTrampoline 之后, 但被它调用。
void maybeAutoInstallHook();

// 自动安装开关。默认开启; EPSILON_NO_AUTO_HOOK=1 关闭。
// 关闭途径刻意放在**游戏之外**, 用户不必先找到配置目录或让游戏成功启动过一次。
bool autoHookAllowed() {
    char buf[8]{};
    if (::GetEnvironmentVariableA("EPSILON_NO_AUTO_HOOK", buf, sizeof(buf)) > 0) {
        if (::atoi(buf) != 0) return false;
    }

    // 另一个开关: 存在这个标记文件就跳过自动安装。用于"游戏已经开着"时改行为。
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = ::GetTempPathW(static_cast<DWORD>(std::size(tmp)), tmp);
    if (n > 0 && n < std::size(tmp)) {
        std::wstring flag = tmp;
        flag += L"epsilonPayload_no_autohook";
        if (::GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    }
    return true;
}

// 每帧调用, 内部自己决定要不要真动手。只试一次 —— 失败后命令 `hook` 可手工重试。
void maybeAutoInstallHook() {
    if (gAutoHookDone) return;
    if (hooks().installed()) { gAutoHookDone = true; return; }

    const uint64_t now = ::GetTickCount64();
    if (gAutoHookNotBefore == 0) {
        gAutoHookNotBefore = now + autoHookDelayMs;
        if (!autoHookAllowed()) {
            gAutoHookDone = true;
            trace("Present 钩子自动安装已由 EPSILON_NO_AUTO_HOOK 关闭");
            return;
        }
        trace(fmt("Present 钩子自动安装已启用(默认); 将在 {} 秒后尝试, "
                  "如需关闭请设 EPSILON_NO_AUTO_HOOK=1", autoHookDelayMs / 1000));
        return;
    }
    if (now < gAutoHookNotBefore) return;

    gAutoHookDone = true;
    trace("Present 钩子自动安装: 开始尝试(命令路径 hook 亦可手工重试)");

    if (installFrameHook()) {
        trace("Present 钩子自动安装成功");
        return;
    }
    trace(fmt("Present 钩子自动安装失败: {}", hooks().status().error));
    trace("  覆盖层不可用; 命令采集与功能模块不受影响。可手工执行 `hook` 重试。");
}

unsigned long __stdcall featureTickTrampoline(void*) {
    uint64_t lastSave = ::GetTickCount64();

    trace("功能线程已启动(模块 tick + Present 自动安装)");

    uint64_t loopCount = 0;
    while (!gStopping.load(std::memory_order_acquire)) {
        // 心跳: 线程平时是静默的, 每 ~5 秒一行便于确认它还活着。
        // findPresent 在**没有 D3D12 的目标**上可能长时间不返回 —— 心跳停了就是卡在那里。
        if (++loopCount % 150 == 0) {
            trace(fmt("功能线程心跳 loop={}", loopCount));
        }

        // 推进一帧: tickFrame 内部会带节流地重新解析玩家移动目标。
        epsilon::feature::tickFrame();
        epsilon::feature::ModuleManager::instance().onFrame();

        maybeAutoInstallHook();

        // 只在真有改动时才写盘 —— 避免每 5 秒无条件重写一遍配置文件。
        const uint64_t now = ::GetTickCount64();
        if (now - lastSave >= configSaveIntervalMs) {
            lastSave = now;
            epsilon::feature::ConfigManager::instance().saveIfDirty();
        }

        ::Sleep(featureTickIntervalMs);
    }
    return 0;
}

void startFeatureThread() {
    if (gFeatureThread) return;
    // 线程里会用到 CRT(string/format/mutex), 必须 _beginthreadex。
    uintptr_t t = _beginthreadex(
        nullptr, 0,
        reinterpret_cast<unsigned(__stdcall*)(void*)>(&featureTickTrampoline),
        nullptr, 0, nullptr);
    if (!t) {
        logWarn(fmt("功能模块线程创建失败 errno={} —— 模块开关将不会自动生效",
                    errno));
        return;
    }
    ::SetThreadDescription(reinterpret_cast<HANDLE>(t), L"epsilon-feature-tick");
    gFeatureThread = reinterpret_cast<HANDLE>(t);
}

uint64_t     gModuleBase = 0;
uint64_t     gModuleSize = 0;
std::wstring gModulePath;

// 帧回调故意保持极轻: 只记帧数, 并在第一帧打一行日志确认钩子真的挂上了。
std::atomic<bool> gFirstFrameLogged{false};

void onFrame(uint32_t frameIndex, int /*syncInterval*/, int /*flags*/) {
    if (frameIndex == 1 && !gFirstFrameLogged.exchange(true)) {
        trace("Present 钩子已生效, 游戏正在出帧");
    }
}

} // namespace

// ---------------------------------------------------------------------------
game::ue::Engine& engine() {
    static game::ue::Engine fallback;
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
// 单实例守卫: 每个进程只允许一个注入体实例真正启动。
// 命名互斥体名带 PID 与模块槽位 —— 详见 docs/payload/lifecycle.md。
HANDLE gSingletonMutex = nullptr;

bool claimSingletonOrExit() {
    wchar_t name[192]{};
    const std::wstring slot = ownModuleSlot();
    if (slot.empty()) {
        ::_snwprintf_s(name, _TRUNCATE, L"Local\\epsilonPayload_instance_%lu",
                       ::GetCurrentProcessId());
    } else {
        ::_snwprintf_s(name, _TRUNCATE, L"Local\\epsilonPayload_instance_%lu_%s",
                       ::GetCurrentProcessId(), slot.c_str());
    }

    ::SetLastError(0);
    HANDLE h = ::CreateMutexW(nullptr, FALSE, name);
    if (!h) return true;                       // 建不出来就别拦, 让它继续
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(h);
        return false;                          // 已有实例
    }
    gSingletonMutex = h;                       // 持有到进程结束
    return true;
}

unsigned long __stdcall runtimeMain(void* /*param*/) {
    ::SetThreadDescription(::GetCurrentThread(), L"epsilon-runtime");

    // 文件日志最先开 —— 后面每一步都要留痕, 崩了才查得到。
    logFileOpen();

    if (!claimSingletonOrExit()) {
        trace("本进程里已有一个注入体实例在运行 —— 本次实例退出");
        trace("(预期行为: 避免多个实例争抢同一管道名)");
        logFileClose();
        return 0;
    }

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
    // 重试窗口要覆盖旧实例退出后释放管道名的那段时间。
    if (connectInjectorPipe(8000, &pipeErr)) {
        c.sink = Sink::pipe;
        c.verbose = true;
        trace("已连接注入器管道, 输出走管道");
    } else {
        c.sink = Sink::none;
        trace(fmt("管道连接失败({}) —— 进入静默模式, 只有文件日志", pipeErr));
        trace(fmt("日志文件: {}", logFilePath()));
    }

    trace("正在定位引擎全局(GObjects / GNames)...");

    // ---- 2) 定位引擎 ----
    gEngine = std::make_unique<game::ue::Engine>();
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

    // ---- 3.5) 功能模块 ----
    // **引擎没就绪也要走这一步**: 玩家可能还没进关卡, 模块需要在之后自己解析成功
    // 并生效 —— 这正是 MovementResolver 的节流重试所服务的场景。
    {
        epsilon::feature::initModules();
        epsilon::feature::bindEngine(gEngine.get());

        auto& cfg = epsilon::feature::ConfigManager::instance();
        if (cfg.initialize()) {
            cfg.applyToModules();
        } else {
            logWarn(fmt("配置初始化失败, 模块使用默认值: {}", cfg.lastError()));
        }

        startFeatureThread();
    }

    // ---- 4) 帧钩子 ----
    // 自动安装由功能线程在延迟后尝试(见 maybeAutoInstallHook), 不放在启动必经路径上:
    // 万一 findPresent 把目标带走, 至少管道已连、日志已落盘, 有迹可循。
    trace(fmt("帧钩子将由功能线程在 {} 秒后自动尝试安装(可设 EPSILON_NO_AUTO_HOOK=1 关闭)",
              autoHookDelayMs / 1000));

    gReady.store(true, std::memory_order_release);
    trace("初始化完成, 进入命令循环");

    // ---- 5) 进入命令循环 ----
    if (c.sink == Sink::pipe) {
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

        while (!gStopping.load(std::memory_order_acquire)) ::Sleep(50);
    } else {
        // 管道没连上: 静默驻留, 不做"稍后补连"的补救 —— 日志里有记录。
        trace("静默驻留(未连上注入器, 命令不可用)");
        while (!gStopping.load(std::memory_order_acquire)) ::Sleep(200);
    }

    return 0;
}

void shutdownRuntime() {
    std::lock_guard lk(gLifecycleMu);
    if (gStopping.exchange(true)) return;

    // 不卸载钩子, 也不 FreeLibrary: 半途卸载会留下悬空回调, 目标必崩。
    trace("运行时停止(注入体仍驻留, 重启目标进程可清除)");
    pipeClose();
}

} // namespace epsilon::payload
