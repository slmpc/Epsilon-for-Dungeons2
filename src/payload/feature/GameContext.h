// ============================================================================
//  GameContext.h — 功能模块访问游戏运行时的统一入口。
//  线程约定: 全部只在游戏线程(功能线程)里使用。
//  细节见 docs/features/module-framework.md
// ============================================================================
#pragma once

#include "payload/game/dungeons2/MovementResolver.h"
#include "payload/game/ue/Engine.h"

#include <cstdint>

namespace epsilon::feature {

struct GameContext {
    game::ue::Engine* engine = nullptr;
    // 进程内唯一的移动解析器(定义在 GameContext.cpp), 所有权不在这里。
    game::dungeons2::MovementResolver* movement = nullptr;
    // 自注入以来累计的帧号。供模块做"每 N 帧做一次"的节流。
    uint64_t frame = 0;

    [[nodiscard]] bool engineReady() const noexcept {
        return engine != nullptr && engine->ready();
    }
    // 要写游戏数据的模块都该先检查这个。
    [[nodiscard]] bool movementReady() const noexcept {
        return engineReady() && movement != nullptr && movement->ready();
    }
};

// 当前进程的上下文。注入体初始化时由 Runtime 填入引擎指针。
[[nodiscard]] GameContext& game();

// 由 Runtime 在引擎就绪后调用一次, 绑定引擎实例。
void bindEngine(game::ue::Engine* engine);

// 每帧由 Runtime 推进(累加帧号 + 带节流地重新解析玩家移动目标)。
void tickFrame();

// 进程内唯一的移动解析器。
[[nodiscard]] game::dungeons2::MovementResolver& movement();

} // namespace epsilon::feature
