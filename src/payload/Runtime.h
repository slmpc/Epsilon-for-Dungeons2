// Runtime.h — 注入体的运行时总控
// 生命周期: DllMain 只建线程 → runtimeMain 决策输出通道/定位引擎/进命令循环。
// 细节见 docs/payload/lifecycle.md
#pragma once

#include "payload/CommandServer.h"
#include "payload/game/ue/Engine.h"

#include <cstdint>
#include <string>

namespace epsilon::payload {

// 全局引擎实例; 未初始化时返回一个空壳(成员函数可安全调用, 一律未定位)。
game::ue::Engine& engine();

bool runtimeReady();

// 显式安装 Present 帧钩子(供 `hook` 命令调用)。幂等, 返回是否已安装。
bool installFrameHook();

// 线程入口, 由 DllMain 创建。返回 0; 单实例守卫失败时立刻返回。
unsigned long __stdcall runtimeMain(void* param);

// 主动结束: 置停止标志并断管道。不摘钩子、不 FreeLibrary。
void shutdownRuntime();

void setModuleInfo(uint64_t base, uint64_t size, std::wstring path);
// 宿主进程主模块(游戏 EXE)的基址/大小/路径, 不是注入体自己的。
uint64_t moduleBase();
uint64_t moduleSize();
std::wstring const& modulePath();

} // namespace epsilon::payload
