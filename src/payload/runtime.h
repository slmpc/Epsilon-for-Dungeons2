// ============================================================================
//  runtime.h — 注入体的运行时总控
//
//  生命周期:
//    DllMain(PROCESS_ATTACH)  → 只创建一个线程, 立刻返回(绝不在加载锁里干活)
//    runtime_main()           → 决定输出通道 → 初始化引擎 → 装钩子 → 进入命令循环
//    DllMain(PROCESS_DETACH)  → 摘钩子 / 断管道 / 关控制台
// ============================================================================
#pragma once

#include "payload/command_server.h"
#include "payload/ue/engine.h"

#include <cstdint>
#include <string>

namespace mcd2::payload {

// 运行时的全局引擎实例。init_runtime 之后才有效。
ue::Engine& engine();

// 是否已经初始化完成。
bool runtime_ready();

// 显式安装 Present 帧钩子(供 `hook` 命令调用)。返回是否成功。
// 不在启动路径上自动调用 —— 数据采集不需要帧回调, 而这个钩子在被注入的
// DLL 上下文里有已知崩溃风险(见 hooks.h 的说明)。
bool install_frame_hook();

// 线程入口。由 DllMain 创建。
unsigned long __stdcall runtime_main(void* param);

// 主动结束: 停止命令循环、摘钩子、断通道。
void shutdown_runtime();

// 模块信息
void set_module_info(uint64_t base, uint64_t size, std::wstring path);
uint64_t module_base();
uint64_t module_size();
std::wstring const& module_path();

} // namespace mcd2::payload
