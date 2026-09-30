// ============================================================================
//  pipe_client.cpp — 注入体的输出通道
//
//  ⚠️ 历史坑(务必保留这条注释):
//    这里原本有一个模块级静态 PipeClient g_pipe 用来连接, 而 payload.cpp 的
//    emit_now() 往 Context::pipe 发送 —— **两个不同的对象**。
//    结果是: 连接成功、日志显示"已连接注入器管道", 但每一次 send 都打到
//    那个从没连接过的实例上, 于是返回 false, 注入器一条消息都收不到。
//    症状极具误导性: 注入器→注入体 方向正常(命令能收到), 反向全丢。
//
//    现在统一到 ctx().pipe 这一个实例, 并由 ctx().pipe_connected 记状态。
//    任何新增的收发路径都必须走它。
// ============================================================================
#include "payload/pipe_client.h"

#include "common/text.h"
#include "payload/payload.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mutex>

namespace mcd2::payload {
namespace {

std::mutex            g_mu;
std::function<void()> g_on_disconnect;

} // namespace

bool connect_injector_pipe(uint32_t retry_ms, std::string* error) {
    std::lock_guard lk(g_mu);

    auto& c = ctx();

    // 管道名 = 由本进程 PID 推导的约定名。
    //   注入器用目标 PID 算出同一个字符串, 注入体用自己的 PID 也得到它 ——
    //   两端零传递。这是**唯一**的通路: 早先还试过用环境变量传递, 实测
    //   GetEnvironmentVariableW 读不到被外部改写的 PEB, 那条路从未生效,
    //   已删除(连同注入器侧整个 PEB 改写代码)。
    const std::wstring name = default_pipe_name(::GetCurrentProcessId());

    std::string err;
    if (!c.pipe.connect(name, retry_ms, &err)) {
        // 连不上就把管道子系统的状态一并报出来。对着真游戏排查时, 光一句
        // "GetLastError=5" 完全不够 —— 分不清是"管道不存在/被占用", 还是
        // "本进程被禁止创建/打开管道"。
        const std::wstring full = pipe_full_path(name);
        std::string diag = fmt("管道连接失败: {}; 目标={}", err, to_utf8(full));

        // 1) 本进程能不能创建管道? 能创建就说明不是全局被禁, 问题在打开那一侧。
        HANDLE srv = ::CreateNamedPipeW(
            pipe_full_path(name + L".probe").c_str(),
            PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 512, 512, 0, nullptr);
        if (srv != INVALID_HANDLE_VALUE) {
            diag += "; 本进程可创建管道=是";
            ::CloseHandle(srv);
        } else {
            diag += fmt("; 本进程可创建管道=否(err={})", ::GetLastError());
        }

        // 2) 直接再打开一次, 单独记录错误码, 避免与重试逻辑的判定混淆。
        HANDLE probe = ::CreateFileW(full.c_str(), GENERIC_READ | GENERIC_WRITE,
                                     0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (probe != INVALID_HANDLE_VALUE) {
            diag += "; 二次打开=成功";
            ::CloseHandle(probe);
        } else {
            const DWORD e = ::GetLastError();
            diag += fmt("; 二次打开失败 err={}", e);
            if (e == ERROR_ACCESS_DENIED)  diag += "(ACCESS_DENIED)";
            if (e == ERROR_FILE_NOT_FOUND) diag += "(FILE_NOT_FOUND 管道不存在)";
            if (e == ERROR_PIPE_BUSY)      diag += "(PIPE_BUSY 已被占用)";
        }

        if (error) *error = diag;
        return false;
    }
    c.pipe_connected = true;
    return true;
}

bool pipe_connected() {
    std::lock_guard lk(g_mu);
    auto& c = ctx();
    return c.pipe_connected && c.pipe.connected();
}

bool pipe_send(proto::Kind kind, std::string_view payload) {
    std::lock_guard lk(g_mu);
    auto& c = ctx();
    if (!c.pipe_connected || !c.pipe.connected()) return false;
    return c.pipe.send(kind, payload);
}

bool pipe_start_command_reader(std::function<void(std::string_view)> on_command) {
    // 注意: 回调在 reader 线程上执行, 所以这里绝不能持锁去调 start_reader ——
    // 回调里可能反过来调 pipe_send, 那样就是自己等自己。
    auto& c = ctx();
    if (!c.pipe_connected) return false;

    return c.pipe.start_reader(
        [cb = std::move(on_command)](proto::Kind kind, std::string_view body) {
            switch (kind) {
                case proto::Kind::command:
                    if (cb) cb(body);
                    break;
                case proto::Kind::bye: {
                    std::function<void()> h;
                    {
                        std::lock_guard lk2(g_mu);
                        h = g_on_disconnect;
                    }
                    if (h) h();
                    break;
                }
                case proto::Kind::ping:
                    pipe_send(proto::Kind::status, "pong");
                    break;
                default:
                    break;
            }
        });
}

void pipe_set_disconnect_handler(std::function<void()> cb) {
    std::lock_guard lk(g_mu);
    g_on_disconnect = std::move(cb);
}

void pipe_close() {
    std::lock_guard lk(g_mu);
    auto& c = ctx();
    c.pipe_connected = false;
    c.pipe.close();
}

} // namespace mcd2::payload
