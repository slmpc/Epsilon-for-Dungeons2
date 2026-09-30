// ============================================================================
//  pipeClient.h — 注入体的输出通道
//
//  默认走这里: 注入器在命令行开着管道服务端, 注入体把采集到的数据回吐过去,
//  由注入器统一显示。这样"控制台"就是注入器自己的终端, 不需要在游戏进程里
//  额外开窗口。
//
//  若注入器没开管道(比如你直接用别的注入器把 DLL 塞进去), 注入体会自动
//  退回 AllocConsole(), 在游戏进程里开一个独立控制台 —— 见 runtime.cpp。
// ============================================================================
#pragma once

#include "common/PipeChannel.h"

#include <functional>
#include <string>

namespace epsilon::payload {

// 连接注入器的管道。名字从环境变量 EPSILON_PIPE_NAME 读。
//   retryMs: 注入器可能还在创建管道, 允许重试
// 成功返回 true。
bool connectInjectorPipe(uint32_t retryMs = 3000, std::string* error = nullptr);

// 当前管道是否已连上。
bool pipeConnected();

// 通过管道发一条消息。未连接返回 false。
bool pipeSend(proto::Kind kind, std::string_view payload);

// 注册命令接收回调并启动后台读线程。
bool pipeStartCommandReader(std::function<void(std::string_view)> onCommand);

// 断开的回调(注入器退出时触发)。
void pipeSetDisconnectHandler(std::function<void()> cb);

void pipeClose();

} // namespace epsilon::payload
