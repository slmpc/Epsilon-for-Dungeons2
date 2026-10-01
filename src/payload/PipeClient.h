// PipeClient.h — 注入体的输出通道(命名管道客户端)
// 细节见 docs/payload/output.md
#pragma once

#include "common/PipeChannel.h"

#include <functional>
#include <string>

namespace epsilon::payload {

// 连接注入器的管道(名字由 PID + 本模块文件名推导, 两端零传递)。
//   retryMs: 注入器可能还在创建管道, 允许重试
// 成功返回 true; 失败时把诊断信息写进 error(可为 nullptr)。
bool connectInjectorPipe(uint32_t retryMs = 3000, std::string* error = nullptr);

// 当前管道是否已连上。
bool pipeConnected();

// 从**本模块**的文件名推导槽位(去掉 .dll、只留字母数字), 管道名与单实例
// 互斥体名都用它 —— 不同文件名的注入体因此可以并存。
// 取不到模块名时返回空串, 调用方退化为"只有 PID"的名字。
std::wstring ownModuleSlot();

// 通过管道发一条消息。未连接返回 false。
bool pipeSend(proto::Kind kind, std::string_view payload);

// 注册命令接收回调并启动后台读线程。回调在 reader 线程上执行。
bool pipeStartCommandReader(std::function<void(std::string_view)> onCommand);

// 断开的回调(收到 proto::Kind::bye 时触发)。
void pipeSetDisconnectHandler(std::function<void()> cb);

void pipeClose();

} // namespace epsilon::payload
