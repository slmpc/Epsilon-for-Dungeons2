#pragma once

#include "payload/game/dungeons2/Movement.h"
#include "payload/game/ue/Engine.h"

#include <cstdint>
#include <optional>
#include <string>

namespace epsilon::game::dungeons2 {

// 当前解析到的玩家移动目标。
struct PlayerTarget {
    uint64_t    pawn = 0;
    uint64_t    movement = 0;
    uint64_t    movementClass = 0;
    uint64_t    attributeSet = 0;
    std::string pawnClass;
    std::string movementClassName;
    std::string attributeSetClassName;
    bool        resolved = false;
};

class MovementResolver {
public:
    // force = true 时忽略节流立即重扫(玩家换关卡/重生后需要)。
    bool resolve(ue::Engine& engine, bool force = false);
    void invalidate() noexcept { nextResolveAtMs_ = 0; }

    [[nodiscard]] bool ready() const noexcept { return target_.resolved && component_.layout().hasCore(); }

    [[nodiscard]] PlayerTarget const& target() const noexcept { return target_; }
    [[nodiscard]] MovementComponent const& component() const noexcept { return component_; }
    [[nodiscard]] MovementAttributeSet const& attributes() const noexcept { return attributes_; }
    [[nodiscard]] MovementLayout const& layout() const noexcept { return component_.layout(); }

    [[nodiscard]] std::optional<float> read(MovementField field) const {
        return component_.read(field);
    }
    bool write(MovementField field, float value) const {
        return component_.write(field, value);
    }
    [[nodiscard]] std::optional<float> readAttribute(MovementAttribute attribute) const {
        return attributes_.read(attribute);
    }
    bool writeAttribute(MovementAttribute attribute, float value) const {
        return attributes_.write(attribute, value);
    }

    // 诊断: 上一次解析失败的原因(纯 ASCII, 会被面板显示)。
    [[nodiscard]] std::string const& lastError() const noexcept { return lastError_; }
    // 偏移来源("reflection" / "table (N reflected)")。
    [[nodiscard]] std::string const& offsetSource() const noexcept { return offsetSource_; }

private:
    bool resolveTarget(ue::Engine& engine);
    bool resolveLayout(ue::Engine& engine);
    void clearTarget();

    PlayerTarget          target_{};
    MovementComponent     component_{};
    MovementAttributeSet  attributes_{};
    std::string           lastError_;
    std::string           offsetSource_;
    uint64_t              nextResolveAtMs_ = 0;
};

} // namespace epsilon::game::dungeons2
