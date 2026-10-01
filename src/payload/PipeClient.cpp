// PipeClient.cpp — 注入体输出通道实现
// ⚠️ 所有收发路径都必须走 ctx().pipe 这一个实例(曾因存在第二个 PipeClient
//    导致"连上了但发不出去"), 状态记在 ctx().pipeConnected。
// 细节见 docs/payload/output.md
#include "payload/PipeClient.h"

#include "common/Text.h"
#include "payload/Payload.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mutex>

namespace epsilon::payload {
namespace {

std::mutex            gMu;
std::function<void()> gOnDisconnect;

} // namespace

// 用"本模块内某个函数的地址"反查模块句柄, 免得把 hModule 一路传下来。
std::wstring ownModuleSlot() {
    HMODULE h = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&connectInjectorPipe), &h) ||
        h == nullptr) {
        return {};
    }
    wchar_t path[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(h, path, static_cast<DWORD>(std::size(path)));
    if (n == 0) return {};
    return moduleSlotFromPath(std::wstring_view(path, n));
}

bool connectInjectorPipe(uint32_t retryMs, std::string* error) {
    std::lock_guard lk(gMu);

    auto& c = ctx();

    // 管道名 = 本进程 PID + 本模块文件名; 注入器按同样规则算出同一个字符串。
    const std::wstring name = defaultPipeName(::GetCurrentProcessId(), ownModuleSlot());

    std::string err;
    if (!c.pipe.connect(name, retryMs, &err)) {
        // 失败时把管道子系统的状态一并报出来: 光一句 GetLastError 分不清是
        // "管道不存在/被占用", 还是"本进程被禁止创建/打开管道"。
        const std::wstring full = pipeFullPath(name);
        std::string diag = fmt("管道连接失败: {}; 目标={}", err, toUtf8(full));

        // 本进程能不能创建管道? 能就说明不是全局被禁, 问题在打开那一侧。
        HANDLE srv = ::CreateNamedPipeW(
            pipeFullPath(name + L".probe").c_str(),
            PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 512, 512, 0, nullptr);
        if (srv != INVALID_HANDLE_VALUE) {
            diag += "; 本进程可创建管道=是";
            ::CloseHandle(srv);
        } else {
            diag += fmt("; 本进程可创建管道=否(err={})", ::GetLastError());
        }

        // 直接再打开一次, 单独记录错误码, 避免与重试逻辑的判定混淆。
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
    c.pipeConnected = true;
    return true;
}

bool pipeConnected() {
    std::lock_guard lk(gMu);
    auto& c = ctx();
    return c.pipeConnected && c.pipe.connected();
}

bool pipeSend(proto::Kind kind, std::string_view payload) {
    std::lock_guard lk(gMu);
    auto& c = ctx();
    if (!c.pipeConnected || !c.pipe.connected()) return false;
    return c.pipe.send(kind, payload);
}

bool pipeStartCommandReader(std::function<void(std::string_view)> onCommand) {
    // ⚠️ 回调在 reader 线程上执行, 所以这里绝不能持锁调 startReader ——
    //    回调里可能反过来调 pipeSend, 那就是自己等自己。
    auto& c = ctx();
    if (!c.pipeConnected) return false;

    return c.pipe.startReader(
        [cb = std::move(onCommand)](proto::Kind kind, std::string_view body) {
            switch (kind) {
                case proto::Kind::command:
                    if (cb) cb(body);
                    break;
                case proto::Kind::bye: {
                    std::function<void()> h;
                    {
                        std::lock_guard lk2(gMu);
                        h = gOnDisconnect;
                    }
                    if (h) h();
                    break;
                }
                case proto::Kind::ping:
                    pipeSend(proto::Kind::status, "pong");
                    break;
                default:
                    break;
            }
        });
}

void pipeSetDisconnectHandler(std::function<void()> cb) {
    std::lock_guard lk(gMu);
    gOnDisconnect = std::move(cb);
}

void pipeClose() {
    std::lock_guard lk(gMu);
    auto& c = ctx();
    c.pipeConnected = false;
    c.pipe.close();
}

} // namespace epsilon::payload
