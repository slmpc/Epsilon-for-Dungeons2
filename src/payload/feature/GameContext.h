// ============================================================================
//  GameContext.h — 功能模块访问游戏运行时的统一入口
//
//  功能模块不该各自去 include Engine / World / Runtime 那一堆头文件, 也不该
//  各自持有一份解析状态 —— 那会导致每个模块各扫一遍对象表。
//
//  这里给出一层薄封装:
//    * game()      当前注入体的运行时(引擎实例、模块注册表、配置)
//    * movement()  进程内唯一的玩家移动解析器(带节流缓存)
//    * requireReady()  统一的"能不能干活"判断
//
//  线程约定: 全部只在游戏线程(帧钩子回调)里使用。
// ============================================================================
#pragma once

#include "payload/feature/movement/MovementAccess.h"
// GameContext 的 engineReady() 会调用 Engine::ready(), 所以这里必须是完整
// 类型, 不能只前置声明 —— 前置声明会让内联函数编译不过(C2027)。
#include "payload/ue/Engine.h"

namespace epsilon::ue {
class Engine;
}

namespace epsilon::feature {

// 运行时的三个引用打包在一起, 便于模块一次拿到需要的全部上下文。
struct GameContext {
    ue::Engine* engine = nullptr;
    // 进程内唯一的移动解析器(定义在 GameContext.cpp)。
    PlayerMovement* movement = nullptr;
    // 自注入以来累计的帧号。供模块做"每 N 帧做一次"的节流。
    uint64_t frame = 0;

    [[nodiscard]] bool engineReady() const noexcept {
        return engine != nullptr && engine->ready();
    }
    // 引擎就绪 + 玩家移动组件已解析。要写游戏数据的模块都该检查这个。
    [[nodiscard]] bool movementReady() const noexcept {
        return engineReady() && movement != nullptr && movement->ready();
    }
};

// 当前进程的上下文。注入体初始化时由 Runtime 填入引擎指针。
[[nodiscard]] GameContext& game();

// 由 Runtime 在引擎就绪后调用一次, 绑定引擎实例。
void bindEngine(ue::Engine* engine);

// 每帧由 Runtime 推进(累加帧号)。
void tickFrame();

// 进程内唯一的移动解析器。
[[nodiscard]] PlayerMovement& movement();

} // namespace epsilon::feature
