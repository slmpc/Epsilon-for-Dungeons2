// ============================================================================
//  runtime.cpp — 注入体运行时总控
//
//  输出通道决策顺序:
//    1. 环境变量 MCD2_PIPE_NAME 存在且连得上 → 走管道, UI 在注入器的终端里
//    2. 否则 → AllocConsole() 自己开一个控制台窗口, 直接读 stdin
//    3. 都不行 → Sink::none, 静默(但所有命令仍可经管道后补连接)
//
//  为什么不在 DllMain 里做这些: 注入线程持有加载器锁, 在锁内创建工作线程/
//  初始化 CRT/加载 DLL 都可能死锁。DllMain 只负责 CreateThread 然后立刻返回。
// ============================================================================
#include "payload/runtime.h"

#include "common/text.h"
#include "payload/hooks.h"
#include "payload/payload.h"
#include "payload/pipe_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace mcd2::payload {
namespace {

std::unique_ptr<ue::Engine> g_engine;
std::unique_ptr<CommandServer> g_server;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_stopping{false};
std::mutex g_lifecycle_mu;

uint64_t     g_module_base = 0;
uint64_t     g_module_size = 0;
std::wstring g_module_path;

// --------------------------------------------------------------------------
// 帧回调: 故意保持极轻。只记帧数, 并在第一帧打一行日志,
// 让人确认"钩子真的挂上了、游戏在出帧"。
// --------------------------------------------------------------------------
std::atomic<bool> g_first_frame_logged{false};

void on_frame(uint32_t frame_index, int /*sync_interval*/, int /*flags*/) {
    if (frame_index == 1 && !g_first_frame_logged.exchange(true)) {
        trace("Present 钩子已生效, 游戏正在出帧");
    }
}

} // namespace

// ---------------------------------------------------------------------------
ue::Engine& engine() {
    static ue::Engine fallback;
    return g_engine ? *g_engine : fallback;
}

bool runtime_ready() { return g_ready.load(std::memory_order_acquire); }

bool install_frame_hook() {
    if (hooks().installed()) return true;
    if (!hooks().install()) return false;
    hooks().set_frame_callback(&on_frame);
    return true;
}

uint64_t module_base() { return g_module_base; }
uint64_t module_size() { return g_module_size; }
std::wstring const& module_path() { return g_module_path; }

void set_module_info(uint64_t base, uint64_t size, std::wstring path) {
    g_module_base = base;
    g_module_size = size;
    g_module_path = std::move(path);
}

// ---------------------------------------------------------------------------
unsigned long __stdcall runtime_main(void* /*param*/) {
    ::SetThreadDescription(::GetCurrentThread(), L"mcd2-runtime");

    // 文件日志最先开 —— 后面每一步都要留痕, 崩了才查得到。
    log_file_open();
    trace(fmt("runtime_main 进入 (pid {}, 模块基址 {}, 大小 {})",
              ::GetCurrentProcessId(), hex(g_module_base, 16), human_bytes(g_module_size)));

    // ---- 1) 决定输出通道 ----
    auto& c = ctx();
    c.pid = ::GetCurrentProcessId();
    c.module_base = g_module_base;
    c.module_size = g_module_size;
    c.module_path = g_module_path;

    trace("正在连接注入器管道...");
    std::string pipe_err;
    if (connect_injector_pipe(3000, &pipe_err)) {
        c.sink = Sink::pipe;
        c.verbose = true;
        trace("已连接注入器管道, 输出走管道");
    } else {
        // 管道没连上: 不弹控制台(don't add fallback paths), 只留文件日志。
        // 注入体静默驻留, 日志里能看到原因。
        c.sink = Sink::none;
        trace(fmt("管道连接失败({}) —— 进入静默模式, 只有文件日志", pipe_err));
        trace(fmt("日志文件: {}", log_file_path()));
    }

    trace("正在定位引擎全局(GObjects / GNames)...");

    // ---- 2) 定位引擎 ----
    g_engine = std::make_unique<ue::Engine>();
    g_engine->set_module(g_module_base, g_module_size, g_module_path);

    bool ok = false;
    for (int attempt = 1; attempt <= 3 && !ok; ++attempt) {
        trace(fmt("引擎定位 第 {} 次尝试...", attempt));
        ok = g_engine->init();
        if (!ok) {
            log_warn(fmt("第 {} 次定位失败(游戏可能仍在加载), 1 秒后重试...", attempt));
            ::Sleep(1000);
        }
    }
    trace(ok ? "引擎定位成功" : "引擎定位失败(降级为元命令模式)");

    // ---- 3) 命令服务 ----
    g_server = std::make_unique<CommandServer>(*g_engine);

    if (ok) {
        log_info("定位成功:");
        emit(g_engine->report());
    } else {
        log_warn("未能定位引擎 → 只能使用 rescan/status/help 等元命令");
    }

    // ---- 4) 帧钩子:**不自动安装** ----
    // 数据采集不依赖帧回调, 命令驱动已经够用。帧钩子要用 `hook` 命令显式触发。
    // (建临时 D3D 交换链取 Present 在普通进程里验证通过, 但在被注入的 DLL
    //  上下文里会把目标带崩 —— 尚未定位, 所以不放在必经路径上。)
    trace("帧钩子未自动安装(如需请用 hook 命令)");

    g_ready.store(true, std::memory_order_release);
    trace("初始化完成, 进入命令循环");

    // ---- 5) 进入命令循环 ----
    if (c.sink == Sink::pipe) {
        // 命令来自注入器。先握手, 再报就绪。
        proto::Hello hello;
        hello.pid = c.pid;
        hello.module_base = c.module_base;
        hello.module_size = c.module_size;
        hello.image_base = 0;
        hello.protocol = proto::kVersion;
        constexpr char kTag[] = "mcd2_payload";
        static_assert(sizeof(kTag) <= sizeof(hello.tag));
        std::memcpy(hello.tag, kTag, sizeof(kTag));

        c.pipe.send_pod(proto::Kind::hello, hello);
        c.pipe.send(proto::Kind::ready, ok ? "ready" : "ready-no-engine");

        pipe_start_command_reader([](std::string_view cmd) {
            if (!g_server) return;
            begin_response();
            const bool cont = g_server->execute(cmd);
            end_response();
            if (!cont) shutdown_runtime();
        });

        // 保持线程存活, 直到 shutdown。
        while (!g_stopping.load(std::memory_order_acquire)) ::Sleep(50);
    } else {
        // 管道没连上: 静默驻留。这里是**唯一**的等待逻辑, 不做"稍后补连"的
        // 复杂补救 —— 注入器没起来就是没起来, 日志里有记录。
        trace("静默驻留(未连上注入器, 命令不可用)");
        while (!g_stopping.load(std::memory_order_acquire)) ::Sleep(200);
    }

    return 0;
}

void shutdown_runtime() {
    std::lock_guard lk(g_lifecycle_mu);
    if (g_stopping.exchange(true)) return;

    // 注意: 这里**不卸载钩子, 也不 FreeLibrary**。
    // 注入体一旦挂钩子/起线程, 半途卸载会留下悬空回调 → 目标必崩。
    // 本框架只做注入, 不做卸载; 要清掉注入体就重启目标进程。
    trace("运行时停止(注入体仍驻留, 重启目标进程可清除)");
    pipe_close();
}

} // namespace mcd2::payload
