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

std::unique_ptr<ue::Engine> gEngine;
std::unique_ptr<CommandServer> gServer;
std::atomic<bool> gReady{false};
std::atomic<bool> gStopping{false};
std::mutex gLifecycleMu;

// 功能模块的驱动线程。
//
// 为什么单独一条线程, 而不是挂在 Present 帧钩子上:
//   帧钩子在本项目里是**显式安装**的可选项, 而且其交换链探测在被注入的上下文中
//   有已知崩溃风险(见 hooks.h)。功能模块不该依赖一个默认关闭、且有风险的机制
//   才能工作。
//
// 为什么是 30 Hz 而不是每帧:
//   这类模块改的是"手感的标量"(最大步速/跳跃初速度), 30 Hz 已经完全跟得上;
//   再高只是徒增跨线程的内存读。真正的每帧精度留给将来的瞄准类功能 —— 那种
//   应当走帧钩子。
//
// 线程安全: 线程只做"读游戏内存 + 写几个 float"。Module::onFrame 的实现都只
// 碰自己的标量状态, 不与命令线程或渲染线程共享容器; 配置落盘也在本线程内串行
// 完成(见下面的 saveIfDirty 调用)。
constexpr DWORD featureTickIntervalMs = 33;
constexpr DWORD configSaveIntervalMs  = 5000;

HANDLE gFeatureThread = nullptr;

// 自动安装 Present 钩子的状态。
bool gAutoHookDone = false;

// 首次尝试前的等待时间。
//
// ⚠️ 这个延迟不是随手加的。实测:
//   * 由 `hook` 命令触发(注入后数秒到数分钟) —— 稳定, 多次成功, 曾连续渲染
//     33000+ 帧
//   * 由功能线程在初始化后立刻触发 —— 两次把目标进程带走
//     (其中一次为 EXCEPTION_ACCESS_VIOLATION writing)
// 两者唯一的已知差别就是**时机**。因此自动安装推迟到注入后 10 秒再试,
// 让目标进程的 D3D/加载状态先稳定下来。
//
// 这只是基于现有证据的缓解, 不是已证实的根因。若仍然崩, 用
// EPSILON_NO_AUTO_HOOK=1 关掉, 改用 `hook` 命令(那条路已验证)。
constexpr uint64_t autoHookDelayMs = 10000;

uint64_t gAutoHookNotBefore = 0;

// 前向声明: 定义在 featureTickTrampoline 之后, 但被它调用。
void maybeAutoInstallHook();

// 读取自动安装开关。**默认开启**, 设 EPSILON_NO_AUTO_HOOK=1 关闭。
//
// 为什么保留关闭途径且用环境变量:
//   如果这个钩子在某台机器/某个版本上导致崩溃, 用户需要一条**在游戏之外**
//   就能生效的关闭途径 —— 不必先找到配置目录, 也不必让游戏成功启动过一次。
bool autoHookAllowed() {
    char buf[8]{};
    if (::GetEnvironmentVariableA("EPSILON_NO_AUTO_HOOK", buf, sizeof(buf)) > 0) {
        if (::atoi(buf) != 0) return false;
    }

    // 再给一个**不用重启游戏**就能生效的开关: 存在这个标记文件就跳过自动安装。
    //
    // 为什么需要它: 环境变量属于游戏进程, 想在"游戏已经开着"的时候改变行为
    // 是做不到的。而调试时经常需要在同一个进程里再注入一个不同名字的注入体
    // (见 commitSingleton 的槽位说明), 那种情况下第二个注入体**不应该**再去
    // 挂钩子 —— 同一个 Present 被挂两次没有意义, 还有风险。
    // 建/删这个文件比重启游戏快得多。
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = ::GetTempPathW(static_cast<DWORD>(std::size(tmp)), tmp);
    if (n > 0 && n < std::size(tmp)) {
        std::wstring flag = tmp;
        flag += L"epsilonPayload_no_autohook";
        if (::GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    }
    return true;
}

// 自动安装 Present 钩子。**每帧都调用**, 内部自己决定要不要真动手。
//
// 之前的版本用一个"下次尝试时间戳"做门控, 结果把自己的重试逻辑绕进了死胡同
// (门控值与线程推进时机对不上, 表现为"启用了但从不尝试")。现在改成最简单的
// 形态: 第一次调用就把该做的做完 —— 延迟不是一个有用的保护, 反而是一个
// 容易出错的额外状态。
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

    // 只试一次。不重试的理由: 如果这条路径会崩, 反复踩只是多冒几次把目标
    // 带走的险; 如果它失败但不崩, 命令 `hook` 随时可以手工再试。
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

    // 这个线程是静默的, 没有这条记录就完全看不出它到底有没有起来。
    trace("功能线程已启动(模块 tick + Present 自动安装)");

    uint64_t loopCount = 0;
    while (!gStopping.load(std::memory_order_acquire)) {
        // 心跳: 这个线程平时是静默的, 每 ~5 秒留一行便于确认它还活着。
        //
        // 这条心跳还有个实际用途: 自动安装 Present 钩子调用 findPresent 时会
        // 建临时 D3D12 交换链。在**没有 D3D12 的目标**上(比如自测靶子)这一步
        // 可能长时间不返回 —— 心跳停了就等于告诉你卡在那里了。
        if (++loopCount % 150 == 0) {
            trace(fmt("功能线程心跳 loop={}", loopCount));
        }

        // 推进一帧: 内部会(带节流地)重新解析玩家移动组件, 然后驱动各模块。
        epsilon::feature::tickFrame();
        epsilon::feature::ModuleManager::instance().onFrame();
        // 推进一帧: 内部会(带节流地)重新解析玩家移动组件, 然后驱动各模块。
        epsilon::feature::tickFrame();
        epsilon::feature::ModuleManager::instance().onFrame();

        // ---------------- Present 钩子自动安装 ----------------
        // 默认开启(用户要求), 放在**功能线程里**而不是 runtimeMain 的启动路径上。
        // 万一 findPresent 把目标带走, 至少管道已连、日志已落盘, 有迹可循。
        maybeAutoInstallHook();

        const uint64_t now = ::GetTickCount64();
        if (now - lastSave >= configSaveIntervalMs) {
            lastSave = now;
            // 只在真的有改动时才写盘 —— 避免每 5 秒无条件重写一遍配置文件,
            // 那会让 SSD 上的配置文件目录一直在变, 也让"改了什么"难以追踪。
            epsilon::feature::ConfigManager::instance().saveIfDirty();
        }

        ::Sleep(featureTickIntervalMs);
    }
    return 0;
}

void startFeatureThread() {
    if (gFeatureThread) return;
    // 与 PipeChannel 同样的理由: 线程里会用到 CRT(string/format/mutex), 必须
    // _beginthreadex, 不能用 CreateThread。
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
// 单实例守卫: 每个进程只允许一个注入体实例真正启动。
//
// 为什么需要它: 注入器可以对同一进程反复注入同一个 DLL。LoadLibrary 对
// "已在进程里的模块"只是加引用计数, 不会重跑 DllMain —— 但**每次注入都会
// 让注入器新起一个远端线程去调 LoadLibraryW**, 而我们的 DllMain 每次被调用
// 都会建一个新的 runtimeMain 线程。重复注入 N 次就留下 N 个 runtimeMain,
// 全都在抢同一个管道名。
//
// 后果不是"浪费点资源"这么轻: 新实例连不上管道(名字被旧实例占着), 注入器
// 于是报"等注入体连接管道超时", 看起来像注入失败, 实际是旧实例赢了竞争。
// 实测连续迭代时被这个坑到过 —— 会让人误以为代码坏了。
//
// 用命名互斥体而不是"枚举模块名": 后者要处理路径/文件名/重命名副本等一堆
// 边界, 而且判断与创建之间有竞争窗口。互斥体由内核保证原子性, 一次成功
// 创建就代表独占。
HANDLE gSingletonMutex = nullptr;

bool claimSingletonOrExit() {
    wchar_t name[192]{};
    // 名字里带 PID **与模块槽位**:
    //   * PID 让作用域限定在当前进程内 —— 我们只关心"同一进程里别重复起实例",
    //     不同进程各有各的注入体是正常且期望的。
    //   * 槽位(模块文件名)让**不同文件名的**注入体可以并存。这不是放宽守卫:
    //     守卫要防的是同名实例争抢同一条管道, 而不同槽位对应不同管道, 不存在
    //     竞争。好处是改完代码换个 DLL 名字就能注入, 不必重启游戏。
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

    // 单实例守卫必须放在最前面: 重复实例如果先连上管道, 会让注入器误判
    // 成"注入成功但命令没响应"。详见 claimSingletonOrExit 的注释。
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
    // 重试要足够长: 如果同一个进程里已经被注入过本 DLL 的旧实例, 它们会短暂
    // 抢占管道名。旧实例连上后大多因注入器退出而断开, 让重试窗口覆盖住这段
    // 时间, 新实例就能等到管道空出来。
    if (connectInjectorPipe(8000, &pipeErr)) {
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

    // ---- 3.5) 功能模块 ----
    // 注册 + 绑定引擎 + 起驱动线程。**即使引擎没就绪也要走这一步**:
    // 玩家可能还没进关卡, 模块需要在那之后自己解析成功并生效 —— 这正是
    // MovementAccess 的节流重试所服务的场景。
    {
        epsilon::feature::initModules();
        epsilon::feature::bindEngine(gEngine.get());

        // 配置: 读出上次保存的开关与设置。失败不致命 —— 模块会停在默认值。
        auto& cfg = epsilon::feature::ConfigManager::instance();
        if (cfg.initialize()) {
            cfg.applyToModules();
        } else {
            logWarn(fmt("配置初始化失败, 模块使用默认值: {}", cfg.lastError()));
        }

        startFeatureThread();
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
