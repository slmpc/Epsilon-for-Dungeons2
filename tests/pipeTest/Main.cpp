// ============================================================================
//  pipe_test — 命名管道双向收发的最小验证
//
//  背景: 注入链路里出现了一个奇怪现象 ——
//      注入器 → 注入体 方向正常(注入体收到了 quit 命令)
//      注入体 → 注入器 方向完全收不到(注入器一条消息都没打印)
//  在注入链路里排查这件事成本很高(要起靶子、注入、读落盘日志), 所以这里
//  把 PipeServer / PipeClient 单独拎出来跑一遍, 直接定位是通道代码的问题
//  还是注入环境的问题。
//
//  用法(两个进程):
//      pipe_test.exe server            打印 pid 后等客户端
//      pipe_test.exe client <serverpid> 连过去, 收发若干条
// ============================================================================
#include "common/PipeChannel.h"
#include "common/Text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>

using namespace epsilon;

namespace {

const char* kindName(proto::Kind k) {
    switch (k) {
        case proto::Kind::hello:   return "hello";
        case proto::Kind::ready:   return "ready";
        case proto::Kind::status:  return "status";
        case proto::Kind::data:    return "data";
        case proto::Kind::error:   return "error";
        case proto::Kind::bye:     return "bye";
        case proto::Kind::frame:   return "frame";
        case proto::Kind::command: return "command";
        case proto::Kind::ping:    return "ping";
    }
    return "?";
}

int runServer() {
    consoleInit(true);
    const uint32_t pid = ::GetCurrentProcessId();
    outLine(fmt("SERVER pid={}", pid));

    const std::wstring name = defaultPipeName(pid);
    outLine(fmt("管道名: {}", toUtf8(name)));

    std::atomic<int> got{0};
    PipeServer srv;
    std::string err;
    if (!srv.start(name, [&](proto::Kind k, std::string_view body) {
            ++got;
            outColored(ansi::green,
                        fmt("  [server 收到] kind={} len={} body=\"{}\"\n",
                            kindName(k), body.size(), sanitize(body, 80)));
        }, [&] { outColored(ansi::yellow, "  [server] 客户端断开\n"); }, &err)) {
        errLine(fmt("start 失败: {}", err));
        return 1;
    }
    outLine("等待客户端连接...");

    if (!srv.waitForClient(20000)) {
        errLine("等客户端超时");
        return 2;
    }
    outColored(ansi::green, "[server] 客户端已连接\n");

    // 给客户端一点时间把消息写完
    for (int i = 0; i < 20; ++i) {
        ::Sleep(100);
        if (got.load() >= 3) break;
    }
    outLine(fmt("[server] 共收到 {} 条消息(期望 3: hello/ready/status)", got.load()));

    outLine("[server] 下发 command ...");
    srv.sendCommand("status");
    ::Sleep(300);
    srv.sendCommand("quit");
    ::Sleep(300);

    outLine(fmt("[server] 最终收到 {} 条", got.load()));
    srv.stop();

    const bool ok = got.load() >= 3;
    outColored(ok ? ansi::green : ansi::red,
                ok ? "[server] 结论: 客户端→服务端方向正常\n"
                   : "[server] 结论: 客户端→服务端方向**不通**\n");
    return ok ? 0 : 3;
}

int runClient(uint32_t serverPid) {
    consoleInit(true);
    outLine(fmt("CLIENT pid={} 目标 server={}", ::GetCurrentProcessId(), serverPid));

    const std::wstring name = defaultPipeName(serverPid);
    PipeClient cli;
    std::string err;
    if (!cli.connect(name, 5000, &err)) {
        errLine(fmt("连接失败: {}", err));
        return 1;
    }
    outColored(ansi::green, "[client] 已连接\n");

    // 复刻注入体的发送序列
    struct Hello { uint32_t pid; uint64_t base; char tag[8]; };
    Hello h{::GetCurrentProcessId(), 0xDEADBEEF, {'t','e','s','t'}};
    outLine(fmt("[client] send hello  -> {}", cli.sendPod(proto::Kind::hello, h)));
    outLine(fmt("[client] send ready  -> {}", cli.send(proto::Kind::ready, "ready")));
    outLine(fmt("[client] send status -> {}", cli.send(proto::Kind::status, "定位中...")));

    // 收服务端下发的命令
    outLine("[client] 等待 command ...");
    for (int i = 0; i < 10; ++i) {
        proto::Kind k{};
        std::string body;
        if (cli.recv(k, body, 1000)) {
            outColored(ansi::cyan, fmt("  [client 收到] kind={} body=\"{}\"\n",
                                        kindName(k), sanitize(body, 60)));
            if (body == "quit") break;
        } else {
            outLine("  (recv 超时)");
        }
    }

    cli.close();
    outLine("[client] 结束");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    consoleInit(true);
    if (argc >= 2 && std::strcmp(argv[1], "server") == 0) return runServer();
    if (argc >= 3 && std::strcmp(argv[1], "client") == 0) {
        return runClient(static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10)));
    }
    outLine("用法: pipe_test server | pipe_test client <serverpid>");
    return 2;
}
