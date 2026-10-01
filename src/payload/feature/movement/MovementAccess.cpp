// ============================================================================
//  MovementAccess.cpp
// ============================================================================
#include "payload/feature/movement/MovementAccess.h"

#include "common/PeImage.h"
#include "common/Text.h"
#include "payload/Payload.h"
#include "payload/ue/Engine.h"
#include "payload/ue/ObjectArray.h"
#include "payload/ue/Reflection.h"
#include "payload/ue/World.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace epsilon::feature {

using namespace epsilon::ue;

namespace {

// 解析节流: 失败时多久重试一次。玩家读图/重生期间解析必然失败, 每帧重试会让
// 帧时间抖, 但退避太久又会出现"回到关卡后好几秒才生效"。
constexpr uint64_t retryIntervalMs = 1500;
// 成功之后的复核间隔。换关卡会换掉 pawn / movement, 定期复核能自动跟上。
constexpr uint64_t recheckIntervalMs = 5000;

uint64_t nowMs() { return ::GetTickCount64(); }

// 玩家 pawn 的类名判定 —— 按优先级排序候选。
//
// 实测这个游戏的角色类名(真机关卡 actor 列表):
//     BP_GameplayPlayerController_C   控制器(不是 pawn)
//     BP_AlexCharacter_C              ★ 玩家角色本体
//     BP_WolfCharacter_C              同伴/召唤物
//     BasePlayerState                 玩家状态(不是 pawn)
//
// 原先只匹配 "PlayerCharacter"/"PlayerPawn" —— 这个构建两个都不存在, 所以
// 恒不命中。正确的做法是**从实测数据**里认这些具体名字, 并把判据做成有优先级
// 的列表, 而不是靠一个宽泛的子串。
//
// 为何用有序列表而不是"哪个先出现用哪个": actor 列表里控制器(索引 480)排在
// 玩家角色(索引 483)之前, 单纯取第一个命中会选错对象。
struct PlayerClassRule {
    std::string_view needle;   // 类名里必须含有的子串
    int              rank;     // 越小越优先
};

constexpr PlayerClassRule playerClassRules[] = {
    {"AlexCharacter", 0},        // 实测的玩家角色
    {"BP_SteveCharacter", 0},    // 初代/可能的另一套主角
    {"DungeonsCharacter", 1},
    {"PlayerCharacter", 2},
    {"PlayerPawn", 3},
};

int playerRankFor(std::string_view className) {
    // 先排除明确不是本地玩家的东西。
    // Mock = 游戏的假玩家(教程/演示); Mob = 怪物; Wolf = 同伴;
    // Controller / State / HUD 在 UE 里不是 pawn, 但它们**可能**含有
    // "Player" 字样, 必须显式排掉, 否则会把控制器当成角色。
    if (icontains(className, "Mock")) return -1;
    if (icontains(className, "Mob")) return -1;
    if (icontains(className, "Projectile")) return -1;
    if (icontains(className, "Controller")) return -1;
    if (icontains(className, "PlayerState")) return -1;
    if (icontains(className, "HUD")) return -1;
    if (icontains(className, "Wolf")) return -1;

    for (auto const& r : playerClassRules) {
        if (icontains(className, r.needle)) return r.rank;
    }
    return -1;
}

// ---- 诊断文案: 一律纯 ASCII ----
//
// ⚠️ 这些字符串会被 Overlay 面板直接显示, 而游戏进程内的 ImGui 只有内置
// ASCII 位图字形 —— 中文会渲染成一串 '?', 等于没有信息(实测踩过)。
// 需要中文说明的地方放在 trace/logWarn 里, 那边是文件与管道, 不受字体限制。
constexpr char errEngineNotReady[] =
    "engine not ready (GObjects/GNames not located)";
constexpr char errNoPlayer[] =
    "no local player pawn in level actors (filter expects PlayerCharacter/PlayerPawn, "
    "excluding Mock/Mob) - run the 'player' command to list candidates";

} // namespace

// ===========================================================================
//  MovementOffsets
// ===========================================================================
std::string MovementOffsets::describe() const {
    std::string s;
    auto add = [&](char const* name, int32_t off) {
        if (!s.empty()) s += "  ";
        s += name;
        s += '=';
        s += (off > 0) ? fmt("+{:#x}", off) : std::string("?");
    };
    add("MaxWalkSpeed", maxWalkSpeed);
    add("JumpZVelocity", jumpZVelocity);
    add("GravityScale", gravityScale);
    add("AirControl", airControl);
    add("MaxAcceleration", maxAcceleration);
    add("SpeedMultiplier", speedMultiplier);
    return s;
}

// ===========================================================================
//  解析
// ===========================================================================
bool PlayerMovement::resolve(Engine& engine, bool force) {
    const uint64_t now = nowMs();
    if (!force && now < nextResolveAtMs_) return ready();

    lastError_.clear();

    if (!engine.ready()) {
        lastError_ = errEngineNotReady;
        target_.resolved = false;
        nextResolveAtMs_ = now + retryIntervalMs;
        return false;
    }

    // 只在需要时重扫目标。反射遍历是几百次跨进程读, 不能每帧都做。
    if (!target_.resolved || force) {
        offsets_ = {};              // 目标换了, 偏移必须跟着重问
        if (!resolveTarget(engine)) {
            nextResolveAtMs_ = now + retryIntervalMs;
            return false;
        }
    }

    if (!offsets_.hasCore()) {
        if (!resolveOffsets(engine)) {
            nextResolveAtMs_ = now + retryIntervalMs;
            return false;
        }
    }

    nextResolveAtMs_ = now + recheckIntervalMs;
    return true;
}

// 找到本地玩家的 pawn, 再问出它身上的 CharacterMovement 组件指针。
bool PlayerMovement::resolveTarget(Engine& engine) {
    const uint64_t pawn = findLocalPlayerPawn(engine);
    if (!pawn) {
        lastError_ = errNoPlayer;
        target_ = {};
        return false;
    }

    target_.pawn = pawn;
    target_.pawnClass = engine.objects().classNameOf(pawn);
    const uint64_t pawnClass = engine.objects().classOf(pawn);

    // ---- 拿移动组件 ----
    // 优先走反射问 "CharacterMovement" 属性(UPawn 上的对象指针)。比在对象表里
    // 按名字猜稳 —— 同一个 pawn 可能挂着多个 MovementComponent 子对象。
    uint64_t movement = 0;
    if (auto off = engine.reflection().findProperty(pawnClass, "CharacterMovement");
        off.has_value() && off->offset > 0) {
        uint64_t ptr = 0;
        if (safeRead(&ptr, reinterpret_cast<const void*>(
                              pawn + static_cast<uint32_t>(off->offset)), 8)) {
            movement = ptr;
        }
    }

    // 退路: 反射没问到时, 在对象表里找 Outer == pawn 的移动组件。
    // 只在主路径失败时才走这里。
    if (!movement) {
        engine.objects().for_each([&](ObjectStat const& st) {
            if (icontains(st.className, "CharacterMovementComponent") &&
                engine.objects().outerOf(st.address) == pawn) {
                movement = st.address;
                return false;
            }
            return true;
        });
    }

    if (!movement) {
        lastError_ = fmt("no CharacterMovement component on pawn {}", target_.pawnClass);
        target_.resolved = false;
        return false;
    }

    target_.movement = movement;
    target_.movementClass = engine.objects().classOf(movement);
    target_.movementClassName = engine.objects().classNameOf(movement);
    target_.resolved = true;
    return true;
}

// 用反射把移动组件上那几个属性的偏移问出来。
//
// 注意用 allPropertiesInherited: MaxWalkSpeed / JumpZVelocity / GravityScale 都
// 定义在 UCharacterMovementComponent(基类)上, 而运行时拿到的是游戏自己的子类
// (实测有 UPlayerCharacterMovementComponent)。只问本类会一个都找不到。
bool PlayerMovement::resolveOffsets(Engine& engine) {
    if (!target_.movementClass) {
        lastError_ = "movement component UClass is null";
        return false;
    }

    const auto props = engine.reflection().allPropertiesInherited(target_.movementClass);
    if (props.empty()) {
        lastError_ = fmt("reflection returned no properties for {} "
                         "(UStruct layout offsets are stale - run 'props')",
                         target_.movementClassName);
        return false;
    }

    // 按名字取偏移。同名属性在继承链上可能重复(FloatProperty 才是我们要的),
    // 所以只接受 FloatProperty, 避免撞上同名的 bool/int 属性。
    auto pickFloat = [&](std::string_view name) -> int32_t {
        for (auto const& p : props) {
            if (p.name == name && icontains(p.type, "FloatProperty") && p.offset > 0) {
                return p.offset;
            }
        }
        return 0;
    };

    MovementOffsets o;
    o.maxWalkSpeed    = pickFloat("MaxWalkSpeed");
    o.jumpZVelocity   = pickFloat("JumpZVelocity");
    o.gravityScale    = pickFloat("GravityScale");
    o.airControl      = pickFloat("AirControl");
    o.maxAcceleration = pickFloat("MaxAcceleration");
    // 游戏自己的速度倍率。实测该组件上有 MovementSpeedMultiplier +
    // OnRep_MovementSpeedMultiplier, 说明它是被复制的权威字段。有就优先用。
    o.speedMultiplier = pickFloat("MovementSpeedMultiplier");
    if (!o.speedMultiplier) o.speedMultiplier = pickFloat("SpeedMultiplier");

    offsets_ = o;

    if (!offsets_.hasCore()) {
        lastError_ = fmt("{}: no MaxWalkSpeed/JumpZVelocity among {} properties",
                         target_.movementClassName, props.size());
        return false;
    }
    return true;
}

// 在关卡 Actor 里找本地玩家。
//
// 走 WorldView::actors() 而不是全量扫 GObjects: 后者有几十万个对象, 而且会
// 命中 CDO / 已销毁实例。关卡 Actor 列表才是"当前真实存在的对象"。
uint64_t PlayerMovement::findLocalPlayerPawn(Engine& engine) const {
    WorldView view(engine);
    const auto world = view.currentWorld();
    if (!world) return 0;

    const auto list = view.actors(*world);

    // 取优先级最高(rank 最小)的那个候选, 而不是第一个出现的。
    // 实测 actor 列表里控制器排在玩家角色之前, 取第一个会选错对象。
    uint64_t best = 0;
    int bestRank = 9999;
    for (auto const& a : list) {
        if (!a.address) continue;
        const int rank = playerRankFor(a.className);
        if (rank < 0 || rank >= bestRank) continue;
        bestRank = rank;
        best = a.address;
    }
    return best;
}

// ===========================================================================
//  读写
// ===========================================================================
std::optional<float> PlayerMovement::readFloat(int32_t offset) const {
    if (!offset || !target_.movement) return std::nullopt;
    float v = 0.0f;
    if (!safeRead(&v, reinterpret_cast<const void*>(
                            target_.movement + static_cast<uint32_t>(offset)), sizeof(v))) {
        return std::nullopt;
    }
    return v;
}

bool PlayerMovement::writeFloat(int32_t offset, float value) const {
    if (!offset || !target_.movement) return false;
    // safeWrite 内部先试直写, 失败才放宽页保护 —— 组件对象一般在可写堆上,
    // 正常走快路径。
    return safeWrite(reinterpret_cast<void*>(
                         target_.movement + static_cast<uint32_t>(offset)),
                     &value, sizeof(value));
}

std::optional<float> PlayerMovement::maxWalkSpeed() const {
    return readFloat(offsets_.maxWalkSpeed);
}

std::optional<float> PlayerMovement::jumpZVelocity() const {
    return readFloat(offsets_.jumpZVelocity);
}

std::optional<float> PlayerMovement::speedMultiplier() const {
    return readFloat(offsets_.speedMultiplier);
}

bool PlayerMovement::setMaxWalkSpeed(float v) const {
    return writeFloat(offsets_.maxWalkSpeed, v);
}

bool PlayerMovement::setJumpZVelocity(float v) const {
    return writeFloat(offsets_.jumpZVelocity, v);
}

} // namespace epsilon::feature
