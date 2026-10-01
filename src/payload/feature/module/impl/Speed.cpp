#include "payload/feature/module/impl/Speed.h"

#include "common/Text.h"
#include "payload/Payload.h"
#include "payload/feature/GameContext.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <optional>

namespace epsilon::feature {
namespace {

using epsilon::payload::logInfo;
using epsilon::payload::logWarn;
using namespace epsilon::game::dungeons2;

// 荒谬的倍率会让角色穿过碰撞体, 限制在 10 倍以内。
constexpr double maxMultiplier = 10.0;

constexpr char sinkGameMultiplier[] = "SpeedMultiplier";
constexpr char sinkMaxWalkSpeed[]   = "MaxWalkSpeed";
constexpr char sinkAttribute[]      = "MovementAttribute";

constexpr uint64_t rewriteNoteIntervalMs = 3000;
constexpr uint64_t warningIntervalMs = 4000;
uint64_t gLastWarnMs = 0;
uint64_t gLastRewriteNoteMs = 0;

// 判断"字段里的值就是我们上次写进去的那一笔"。值在几百的量级上, 用相对容差。
bool nearlyEqual(float a, float b) {
    const float scale = std::fmax(1.0f, std::fmax(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= 1e-3f * scale;
}

} // namespace

char const* SpeedModule::sinkLabel(Sink sink) {
    switch (sink) {
        case Sink::movementAttribute: return sinkAttribute;
        case Sink::maxWalkSpeed:      return sinkMaxWalkSpeed;
        case Sink::gameMultiplier:
        default:                      return sinkGameMultiplier;
    }
}

bool SpeedModule::sinkFromLabel(std::string_view label, Sink& out) {
    for (size_t i = 0; i < sinkCount; ++i) {
        const auto sink = static_cast<Sink>(i);
        if (label == sinkLabel(sink)) {
            out = sink;
            return true;
        }
    }
    return false;
}

SpeedModule::SpeedModule()
    : Module("Speed", Category::player, "Movement speed multiplier") {
    // 默认 movementAttribute; 另两个只作对照排查用。
    auto& s = addEnum("ApplyTo",
                      {sinkAttribute, sinkMaxWalkSpeed, sinkGameMultiplier},
                      sinkAttribute,
                      "Which field to scale. MovementAttribute is the default "
                      "and the only one that works on this build");
    sink_ = &s;

    auto& m = addDouble("Multiplier", 1.5, 0.1, maxMultiplier, 0.1, "Speed multiplier");
    multiplier_ = &m;

    auto& n = addBool("Notify", false, "Print resolution details once on enable");
    notify_ = &n;

    setDefaultHidden(false);
}

SpeedModule::Sink SpeedModule::currentSink() const {
    if (sink_ && sink_->is(sinkAttribute))    return Sink::movementAttribute;
    if (sink_ && sink_->is(sinkMaxWalkSpeed)) return Sink::maxWalkSpeed;
    return Sink::gameMultiplier;
}

bool SpeedModule::sinkAvailable(Sink sink) const {
    auto const& mv = movement();
    switch (sink) {
        case Sink::movementAttribute: return mv.attributes().valid();
        case Sink::maxWalkSpeed:      return mv.layout().has(MovementField::maxWalkSpeed);
        case Sink::gameMultiplier:
        default:                      return mv.layout().has(MovementField::movementSpeedMultiplier);
    }
}

uint64_t SpeedModule::sinkAddress(Sink sink) const {
    auto const& mv = movement();
    switch (sink) {
        case Sink::movementAttribute: return mv.attributes().address();
        case Sink::maxWalkSpeed:
        case Sink::gameMultiplier:
        default:                      return mv.component().address();
    }
}

bool SpeedModule::readSink(Sink sink, float& out) const {
    auto const& mv = movement();

    std::optional<float> v;
    switch (sink) {
        case Sink::movementAttribute:
            v = mv.readAttribute(MovementAttribute::movementSpeedMultiplier);
            break;
        case Sink::maxWalkSpeed:
            v = mv.read(MovementField::maxWalkSpeed);
            break;
        case Sink::gameMultiplier:
        default:
            v = mv.read(MovementField::movementSpeedMultiplier);
            break;
    }

    if (!v) return false;
    out = *v;
    return true;
}

bool SpeedModule::writeSink(Sink sink, float value) const {
    auto const& mv = movement();
    switch (sink) {
        case Sink::movementAttribute:
            return mv.writeAttribute(MovementAttribute::movementSpeedMultiplier, value);
        case Sink::maxWalkSpeed:
            return mv.write(MovementField::maxWalkSpeed, value);
        case Sink::gameMultiplier:
        default:
            return mv.write(MovementField::movementSpeedMultiplier, value);
    }
}

bool SpeedModule::captureBaseline(Sink sink) {
    float current = 0.0f;
    if (!readSink(sink, current)) {
        hasBase_ = false;
        return false;
    }

    OwnWrite& own = ownOf(sink);
    if (own.valid && nearlyEqual(current, own.applied)) {
        // 字段里是我们的写入: 沿用记录里的原始值, 不能拿它当基线再乘一次。
        base_ = own.pristine;
        lastApplied_ = current;
    } else {
        // 字段是游戏自己的值: 那一笔不再成立。
        base_ = current;
        own.valid = false;
        lastApplied_ = 0.0f;
    }

    // 基线为 0 说明字段没被游戏初始化(或偏移不对)。用 1.0 兜底, 否则乘出来恒为 0,
    // 角色会被钉在原地。
    if (std::fabs(base_) < 0.0001f) base_ = 1.0f;

    hasBase_ = true;
    baseAddress_ = sinkAddress(sink);
    return true;
}

bool SpeedModule::restoreSink(Sink sink, float pristine) {
    if (sink != activeSink_) return false;

    const uint64_t now = sinkAddress(sink);
    // 解析不到目标: 记录保留。
    if (now == 0) return false;

    if (now != baseAddress_) {
        // 目标对象已换掉(换关卡/重生): 不写旧地址(可能已被释放并复用), 记录作废。
        ownOf(sink) = OwnWrite{};
        markDirty();
        return true;
    }

    if (!writeSink(sink, pristine)) return false;

    ownOf(sink) = OwnWrite{};
    markDirty();
    return true;
}

void SpeedModule::onEnable() {
    hasBase_ = false;
    baseAddress_ = 0;
    lastApplied_ = 0.0f;
    // 记录不清: 字段可能还持有我们上次写进去的值。
    activeSink_ = currentSink();

    auto& mv = movement();
    if (auto* eng = game().engine; eng != nullptr && eng->ready()) {
        mv.resolve(*eng, true);     // 强制重扫: 玩家可能刚进关卡
    }

    if (!mv.ready()) {
        logWarn(fmt("[Speed] 启用但尚未解析到玩家移动组件: {}", mv.lastError()));
        logWarn("[Speed] 会在解析成功后自动生效");
        return;
    }

    // 目标不可用就等下一帧(applyIfNeeded), 不退回别的字段。
    if (!sinkAvailable(activeSink_)) {
        logWarn(fmt("[Speed] 目标 {} 在当前构建上拿不到 —— 它可用后才会生效",
                    sinkLabel(activeSink_)));
        return;
    }

    if (notify_ && notify_->value()) {
        logInfo(fmt("[Speed] 目标 {} / {} ; 施加于 {}",
                    mv.target().pawnClass, mv.target().movementClassName,
                    sinkLabel(activeSink_)));
        logInfo(fmt("[Speed] 属性偏移 {}", mv.layout().describe()));
    }
}

void SpeedModule::onDisable() {
    if (hasBase_) {
        if (restoreSink(activeSink_, base_)) {
            logInfo(fmt("[Speed] 已还原 {} = {:.3f}", sinkLabel(activeSink_), base_));
        } else {
            logWarn(fmt("[Speed] 还原 {} 失败 —— 目标可能已销毁(换关卡/死亡)",
                        sinkLabel(activeSink_)));
            logWarn("[Speed] 原始值已记下, 下次启用不会在放大后的值上再乘一遍");
        }
    }
    hasBase_ = false;
    baseAddress_ = 0;
    lastApplied_ = 0.0f;
}

void SpeedModule::onFrame() {
    applyIfNeeded();
}

void SpeedModule::applyIfNeeded() {
    auto& mv = movement();
    if (!mv.ready()) return;

    const Sink want = currentSink();
    const bool sinkChanged = want != activeSink_;

    // 目标对象换掉(换关卡/重生)时基线失效。
    const uint64_t wantAddress = sinkAddress(want);
    const bool objectChanged = hasBase_ && wantAddress != 0 && wantAddress != baseAddress_;

    // 换目标 / 换对象 / 还没有基线, 都要重取基线。
    if (sinkChanged || objectChanged || !hasBase_) {
        // 换目标前先还原旧字段。
        if (sinkChanged && hasBase_) {
            const Sink old = activeSink_;
            if (restoreSink(old, base_)) {
                logInfo(fmt("[Speed] 目标改为 {} ; {} 已还原", sinkLabel(want), sinkLabel(old)));
            } else {
                logWarn(fmt("[Speed] 目标改为 {} ; 但 {} 还原失败, 它可能停在放大后的值上",
                            sinkLabel(want), sinkLabel(old)));
            }
        }
        activeSink_ = want;
        if (!captureBaseline(want)) return;
    }

    const double multiplier = multiplier_ ? multiplier_->value() : 1.0;
    const float target = static_cast<float>(static_cast<double>(base_) * multiplier);
    if (std::isnan(target) || target <= 0.0f) return;

    float current = 0.0f;
    if (!readSink(activeSink_, current)) return;

    // 浮点比较留容差: 引擎内部可能做一次 float 往返, 严格相等会导致每帧都判定
    // "被改了"从而一直写。
    const float diff = std::fabs(current - lastApplied_);
    const bool unchanged = (diff <= 0.001f) && (std::fabs(current - target) <= 0.001f);
    if (unchanged) return;

    // 上次写的就是这个值、现在读到别的值 -> 游戏在覆盖该字段。
    const bool overwritten = nearlyEqual(lastApplied_, target);

    if (writeSink(activeSink_, target)) {
        lastApplied_ = target;

        OwnWrite& own = ownOf(activeSink_);
        const bool changed = !own.valid || !nearlyEqual(own.applied, target) ||
                             !nearlyEqual(own.pristine, base_);
        own.sink = activeSink_;
        own.pristine = base_;
        own.applied = target;
        own.valid = true;
        if (changed) markDirty();

        // 连续重写说明游戏在覆盖这个字段, 每帧补写压不住 —— 该去挂钩游戏自己
        // 的读取/计算路径, 而不是加倍努力地写。
        const uint64_t now = ::GetTickCount64();
        if (overwritten && now - gLastRewriteNoteMs > rewriteNoteIntervalMs) {
            gLastRewriteNoteMs = now;
            logWarn(fmt("[Speed] 持续重写 {} (读到 {:.3f} -> 写入 {:.3f}) —— "
                        "该字段疑似每帧被游戏重置, 每帧补写无法稳定生效",
                        sinkLabel(activeSink_), current, target));
        }
    } else {
        const uint64_t now = ::GetTickCount64();
        if (now - gLastWarnMs > warningIntervalMs) {
            gLastWarnMs = now;
            logWarn("[Speed] 写入失败(目标内存不可写?)");
        }
    }
}

nlohmann::json SpeedModule::saveCustomState() const {
    nlohmann::json modified = nlohmann::json::array();
    for (size_t i = 0; i < sinkCount; ++i) {
        const OwnWrite& own = own_[i];
        if (!own.valid) continue;

        nlohmann::json entry = nlohmann::json::object();
        entry["sink"] = sinkLabel(own.sink);
        entry["pristine"] = own.pristine;
        entry["applied"] = own.applied;
        modified.push_back(std::move(entry));
    }

    // 没有未还原的写入就不留 state 键。
    if (modified.empty()) return nullptr;

    nlohmann::json out = nlohmann::json::object();
    out["modified"] = std::move(modified);
    return out;
}

void SpeedModule::loadCustomState(nlohmann::json const& state) {
    // null(Module::reset 调用)时不清记录: 只有还原成功才清。
    if (!state.is_object()) return;

    const auto it = state.find("modified");
    if (it == state.end() || !it->is_array()) return;

    for (auto const& entry : *it) {
        if (!entry.is_object()) continue;

        const auto sinkIt = entry.find("sink");
        const auto pristineIt = entry.find("pristine");
        const auto appliedIt = entry.find("applied");
        if (sinkIt == entry.end() || !sinkIt->is_string()) continue;
        if (pristineIt == entry.end() || !pristineIt->is_number()) continue;
        if (appliedIt == entry.end() || !appliedIt->is_number()) continue;

        Sink sink = Sink::movementAttribute;
        if (!sinkFromLabel(sinkIt->get<std::string>(), sink)) continue;

        const double pristine = pristineIt->get<double>();
        const double applied = appliedIt->get<double>();
        // 坏值按"没有记录"处理(配置是用户可手改的明文)。
        if (!std::isfinite(pristine) || !std::isfinite(applied)) continue;
        if (pristine <= 0.0 || applied <= 0.0) continue;

        // 合并而不是替换: 内存里的记录可能比磁盘上的新。
        OwnWrite& own = ownOf(sink);
        own.sink = sink;
        own.pristine = static_cast<float>(pristine);
        own.applied = static_cast<float>(applied);
        own.valid = true;
    }
}

std::string SpeedModule::info() const {
    auto const& mv = movement();
    if (!mv.ready()) return "waiting for player";

    const Sink sink = hasBase_ ? activeSink_ : currentSink();
    float current = 0.0f;
    const bool ok = readSink(sink, current);
    char const* name = sinkLabel(sink);
    return ok ? fmt("{}={:.2f} x{:.1f}", name, current,
                    multiplier_ ? multiplier_->value() : 0.0)
              : fmt("{} unavailable", name);
}

} // namespace epsilon::feature
