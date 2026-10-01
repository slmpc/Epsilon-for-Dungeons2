// ============================================================================
//  Player.h — 本地玩家角色的判定与定位
//
//  这是玩家判定的**唯一出处**: 命令层与功能模块都必须调用它, 不允许各自
//  复刻一份类名规则。
//  实测类名与判定依据: docs/reverse/player-detection.md
// ============================================================================
#pragma once

#include "payload/game/ue/Engine.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace epsilon::game::dungeons2 {

// 类名命中时返回它的优先级(越小越优先); 不是本地玩家返回 -1。
// 必须比较优先级而不是取第一个命中的 —— 关卡 Actor 列表里控制器排在玩家角色之前。
[[nodiscard]] int playerClassRank(std::string_view className) noexcept;

// 判定规则的文字描述, 供诊断命令输出(纯 ASCII, 会被面板显示)。
[[nodiscard]] std::string_view playerClassRulesText() noexcept;

// 关卡 Actor 里挑优先级最高的本地玩家 pawn。找不到返回 0。
[[nodiscard]] uint64_t findLocalPlayerPawn(ue::Engine& engine);

// 取 pawn 身上的 CharacterMovement 组件: 先走反射, 失败再在对象表里按 Outer 匹配。
[[nodiscard]] std::optional<uint64_t> findMovementComponent(ue::Engine& engine, uint64_t pawn);

// 找出属于该 pawn 的 ATR_Movement 实例(沿 Outer 链上升匹配)。
[[nodiscard]] std::optional<uint64_t> findPlayerAttributeSet(ue::Engine& engine, uint64_t pawn);

} // namespace epsilon::game::dungeons2
