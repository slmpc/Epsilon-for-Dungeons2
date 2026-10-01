// ============================================================================
//  Speed.h — 加速模块。
//  默认目标是 movementAttribute, 目标不可用时等待而不是换成别的字段。
//  成因见 docs/features/speed-jump.md
// ============================================================================
#pragma once

#include "payload/feature/module/Module.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace epsilon::feature {

class SpeedModule final : public Module {
public:
    SpeedModule();

    void onEnable() override;
    void onDisable() override;
    void onFrame() override;

    [[nodiscard]] std::string info() const override;

    // 未还原的写入要落盘: 重新注入后字段仍持有我们写下的一对值。
    [[nodiscard]] nlohmann::json saveCustomState() const override;
    void loadCustomState(nlohmann::json const& state) override;

private:
    // 施加目标。落盘名见 sinkLabel()。
    enum class Sink { gameMultiplier = 0, maxWalkSpeed, movementAttribute };
    static constexpr size_t sinkCount = 3;

    // 我们写进去的那一笔: 原始值 + 写进去的值。
    struct OwnWrite {
        Sink  sink = Sink::movementAttribute;
        float pristine = 0.0f;
        float applied = 0.0f;
        bool  valid = false;
    };

    [[nodiscard]] Sink currentSink() const;
    [[nodiscard]] bool sinkAvailable(Sink sink) const;
    [[nodiscard]] bool readSink(Sink sink, float& out) const;
    bool writeSink(Sink sink, float value) const;
    // 该目标当前绑定对象的地址(属性集 / 移动组件)。0 表示拿不到。
    [[nodiscard]] uint64_t sinkAddress(Sink sink) const;

    void applyIfNeeded();
    // 取基线。读不到值时返回 false, 并把 hasBase_ 置回 false。
    bool captureBaseline(Sink sink);
    // 把 activeSink_ 还原成 pristine, sink 必须就是 activeSink_。
    // 返回 true 表示目标已不再持有我们的写入; 写失败时记录保留。
    bool restoreSink(Sink sink, float pristine);

    [[nodiscard]] OwnWrite&       ownOf(Sink sink) { return own_[static_cast<size_t>(sink)]; }
    [[nodiscard]] OwnWrite const& ownOf(Sink sink) const { return own_[static_cast<size_t>(sink)]; }

    // 目标 → 落盘/显示名。
    [[nodiscard]] static char const* sinkLabel(Sink sink);
    // 名字 → 目标。未知名字返回 false。
    [[nodiscard]] static bool sinkFromLabel(std::string_view label, Sink& out);

    Sink  activeSink_ = Sink::movementAttribute;

    float base_ = 0.0f;         // 基线(原始值)
    bool  hasBase_ = false;
    float lastApplied_ = 0.0f;  // 我们上一次写入的值
    uint64_t baseAddress_ = 0;  // 取基线时目标对象的地址(换关卡/重生会变)

    std::array<OwnWrite, sinkCount> own_{};

    EnumSetting*   sink_ = nullptr;
    DoubleSetting* multiplier_ = nullptr;
    BoolSetting*   notify_ = nullptr;
};

} // namespace epsilon::feature
