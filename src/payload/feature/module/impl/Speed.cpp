// ============================================================================
//  Speed.cpp
// ============================================================================
#include "payload/feature/module/impl/Speed.h"

#include "common/Text.h"
#include "payload/feature/GameContext.h"
#include "payload/Payload.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>     // GetTickCount64

#include <cmath>

namespace epsilon::feature {
namespace {

using epsilon::payload::logInfo;
using epsilon::payload::logWarn;

// 倍率上限。10 倍速在多数游戏里已经会让角色直接穿过碰撞体 —— 那不是"加速"
// 而是"送死", 所以卡在这个量级。
constexpr double maxMultiplier = 10.0;

// 三个目标的落盘名(与 EnumSetting 的候选名一致)。
constexpr char sinkGameMultiplier[] = "SpeedMultiplier";
constexpr char sinkMaxWalkSpeed[]   = "MaxWalkSpeed";
// ★ 真正生效的那个: ATR_Movement 属性集里的 MovementSpeed 属性。
constexpr char sinkAttribute[]      = "MovementAttribute";

constexpr int32_t warningIntervalMs = 4000;
uint64_t gLastWarnMs = 0;
uint64_t gLastRewriteNoteMs = 0;

} // namespace

// 目标 → 落盘/显示名。写成成员函数而不是散落的三元表达式 —— 之前目标只有两个时
// 三处各写了一遍 `a ? x : y`, 加第三个目标时那种写法必然漏掉其中一处。
const char* SpeedModule::sinkLabel(Sink s) {
    switch (s) {
        case Sink::movementAttribute: return sinkAttribute;
        case Sink::maxWalkSpeed:      return sinkMaxWalkSpeed;
        case Sink::gameMultiplier:
        default:                      return sinkGameMultiplier;
    }
}


SpeedModule::SpeedModule()
    : Module("Speed", Category::player, "Movement speed multiplier") {
    // 说明文案统一纯 ASCII, 理由同 JumpModule 的构造函数注释。
    //
    // ★ 默认(也是唯一真正有效的)目标是 **GAS 属性集里的 MovementSpeed 属性**。
    //
    //   走过的弯路, 记下来免得再犯:
    //     * 写组件的 MaxWalkSpeed(+0x234): 每帧被游戏从属性重算, 必然被覆盖
    //     * 写属性集 +0x90: 能写能保持, 但那是**空槽位**(原本是 0), 无效
    //     * 写属性集 +0x98/+0x9c: 原本是 700.0, 改成 3000 后**游戏内明显变快**
    //
    //   属性表读出的偏移整体差 8 字节(表里标 +0x90, 实际在 +0x98) —— 靠相邻校验
    //   定下来的: 表里 GravityScale 标 +0xf0, 而 +0xf8 处正是 1.2 这样一个合理的
    //   重力倍率。另两个选项保留下来只为对照排查, 不要作为默认。
    auto& s = addEnum("ApplyTo",
                      {sinkAttribute, sinkMaxWalkSpeed, sinkGameMultiplier},
                      sinkAttribute,
                      "Which field to scale. MovementAttribute is the one that "
                      "actually works on this build");
    sink_ = &s;

    auto& m = addDouble("Multiplier", 1.5, 0.1, maxMultiplier, 0.1, "Speed multiplier");
    multiplier_ = &m;

    auto& n = addBool("Notify", false, "Print resolution details once on enable");
    notify_ = &n;

    // setDefaultHidden, 不是 setHidden —— 理由同 Jump.cpp: ConfigManager 加载时
    // 会 reset(), 把 hidden 恢复成 defaultHidden_(true), 模块会从面板上消失。
    setDefaultHidden(false);
}

SpeedModule::Sink SpeedModule::currentSink() const {
    if (sink_ && sink_->is(sinkAttribute))      return Sink::movementAttribute;
    if (sink_ && sink_->is(sinkMaxWalkSpeed))   return Sink::maxWalkSpeed;
    return Sink::gameMultiplier;
}

int32_t SpeedModule::sinkOffset(Sink sink) const {
    auto const& offs = movement().offsets();
    if (sink == Sink::gameMultiplier) return offs.speedMultiplier;
    if (sink == Sink::movementAttribute) return offs.speedAttributeBase;
    return offs.maxWalkSpeed;
}

bool SpeedModule::readSink(Sink sink, float& out) const {
    // 属性集那条路要走自己的读写接口(目标是 ATR_Movement, 不是移动组件)。
    if (sink == Sink::movementAttribute) {
        auto v = movement().readMovementSpeed();
        if (!v) return false;
        out = *v;
        return true;
    }
    const int32_t off = sinkOffset(sink);
    if (!off) return false;
    auto v = movement().readFloat(off);
    if (!v) return false;
    out = *v;
    return true;
}

bool SpeedModule::writeSink(Sink sink, float value) const {
    // 属性集那条路: 必须走 writeMovementSpeed —— 它会把 Base 与 Current 一起写,
    // 只写一个的话 GAS 聚合时可能被另一个盖回去。
    if (sink == Sink::movementAttribute) {
        return movement().writeMovementSpeed(value);
    }
    const int32_t off = sinkOffset(sink);
    if (!off) return false;
    return movement().writeFloat(off, value);
}

void SpeedModule::captureBaseline(Sink sink) {
    float v = 0.0f;
    if (readSink(sink, v)) {
        base_ = v;
        hasBase_ = true;
        lastApplied_ = 0.0f;
        // 基线为 0 说明这个字段没被游戏初始化(或偏移问错了)。用 1.0 兜底,
        // 否则乘出来恒为 0, 角色会被钉在原地 —— 那比"没生效"糟糕得多。
        if (std::fabs(base_) < 0.0001f) base_ = 1.0f;
    } else {
        hasBase_ = false;
    }
}

void SpeedModule::onEnable() {
    hasBase_ = false;
    haveActiveSink_ = false;
    explainedFallback_ = false;

    auto& mv = movement();
    if (auto* eng = game().engine; eng != nullptr && eng->ready()) {
        mv.resolve(*eng, true);     // 忽略节流, 玩家可能刚进关
    }

    if (!mv.ready()) {
        logWarn(fmt("[Speed] 启用但尚未解析到玩家移动组件: {}", mv.lastError()));
        logWarn("[Speed] 会在解析成功后自动生效");
        return;
    }

    Sink sink = currentSink();

    // 首选目标不可用时退回备用目标, 并且**明确告知** —— 静默退回会让用户
    // 以为改的是 SpeedMultiplier, 实际动的是 MaxWalkSpeed, 排查时会很困惑。
    if (sink == Sink::gameMultiplier && mv.offsets().speedMultiplier == 0) {
        logWarn("[Speed] 未在该移动组件上找到 MovementSpeedMultiplier, 退回 MaxWalkSpeed");
        sink = Sink::maxWalkSpeed;
        explainedFallback_ = true;
    }

    if (sinkOffset(sink) == 0) {
        logWarn("[Speed] 两个目标属性都不可用(反射偏移解析失败)");
        return;
    }

    activeSink_ = sink;
    haveActiveSink_ = true;
    captureBaseline(sink);

    if (notify_ && notify_->value()) {
        logInfo(fmt("[Speed] 目标 {} / {} ; 施加于 {} ; 基线 = {:.3f}",
                    mv.target().pawnClass, mv.target().movementClassName,
                    sinkLabel(sink),
                    base_));
        logInfo(fmt("[Speed] 属性偏移 {}", mv.offsets().describe()));
    }
}

void SpeedModule::onDisable() {
    if (haveActiveSink_ && hasBase_) {
        if (writeSink(activeSink_, base_)) {
            logInfo(fmt("[Speed] 已还原 = {:.3f}", base_));
        } else {
            logWarn("[Speed] 还原失败 —— 目标可能已销毁(换关卡/死亡)");
        }
    }
    hasBase_ = false;
    haveActiveSink_ = false;
    lastApplied_ = 0.0f;
}

void SpeedModule::onFrame() {
    applyIfNeeded(false);
}

void SpeedModule::applyIfNeeded(bool force) {
    auto& mv = movement();
    if (!mv.ready()) return;

    const Sink want = currentSink();

    // 目标切换 / 首次生效 / 换关卡(偏移重新解析)都要重取基线。
    if (!haveActiveSink_ || want != activeSink_ || !hasBase_) {
        activeSink_ = want;
        haveActiveSink_ = true;
        captureBaseline(activeSink_);
        if (!hasBase_) return;
    }

    const float target = static_cast<float>(base_ * multiplier_->value());
    if (std::isnan(target) || target <= 0.0f) return;

    float current = 0.0f;
    if (!readSink(activeSink_, current)) return;

    // 容差比较, 理由同 Jump: 引擎可能做一次 float 往返, 严格相等会导致
    // 每帧都判定"被改了"从而一直写。
    const float diff = std::fabs(current - lastApplied_);
    const bool unchanged = (diff <= 0.001f) && (std::fabs(current - target) <= 0.001f) && !force;
    if (unchanged) return;

    if (writeSink(activeSink_, target)) {
        lastApplied_ = target;
        // 节流记录: 只在"每次都在重写"时提示 —— 那说明游戏自己在覆盖这个字段,
        // 靠每帧补写是压不住的(实测表现: 面板上的数值在闪, 角色却没变快)。
        // 这种时候正确的做法是挂钩游戏自己的读取/计算路径, 而不是加倍努力地写。
        const uint64_t now = ::GetTickCount64();
        if (now - gLastRewriteNoteMs > 3000) {
            gLastRewriteNoteMs = now;
            logWarn(fmt("[Speed] 持续重写 {} (读到 {:.3f} -> 写入 {:.3f}) —— "
                        "该字段疑似每帧被游戏重置, 每帧补写无法稳定生效",
                        sinkLabel(activeSink_),
                        current, target));
        }
    } else {
        const uint64_t now = ::GetTickCount64();
        if (now - gLastWarnMs > static_cast<uint64_t>(warningIntervalMs)) {
            gLastWarnMs = now;
            logWarn("[Speed] 写入失败(目标内存不可写?)");
        }
    }
}

std::string SpeedModule::info() const {
    auto& mv = movement();
    if (!mv.ready()) return "waiting for player";

    const Sink s = haveActiveSink_ ? activeSink_ : currentSink();
    float cur = 0.0f;
    const bool ok = readSink(s, cur);
    const char* name = sinkLabel(s);
    return ok ? fmt("{}={:.2f} x{:.1f}", name, cur,
                    multiplier_ ? multiplier_->value() : 0.0)
              : fmt("{} unavailable", name);
}

} // namespace epsilon::feature
