#include "payload/game/dungeons2/Player.h"

#include "common/Text.h"
#include "payload/game/Field.h"
#include "payload/game/ue/World.h"

namespace epsilon::game::dungeons2 {
namespace {

struct ClassRule {
    std::string_view needle;
    int              rank;   // 越小越优先
};

constexpr ClassRule playerClassRules[] = {
    {"AlexCharacter",      0},   // 实测的玩家角色: BP_AlexCharacter_C
    {"BP_SteveCharacter",  0},
    {"DungeonsCharacter",  1},
    {"PlayerCharacter",    2},
    {"PlayerPawn",         3},
};

// 明确不是本地玩家的东西。Controller / PlayerState / HUD 不是 pawn, 但类名里
// 可能带 "Player", 必须显式排掉。
constexpr std::string_view excludedNeedles[] = {
    "Mock", "Mob", "Projectile", "Controller", "PlayerState", "HUD", "Wolf",
};

// ATR_Movement 实例挂在 ASC 上, ASC 挂在 pawn 上; 实测深度 1。
constexpr int maxOuterDepth = 8;

} // namespace

int playerClassRank(std::string_view className) noexcept {
    for (auto const& needle : excludedNeedles) {
        if (icontains(className, needle)) return -1;
    }
    for (auto const& rule : playerClassRules) {
        if (icontains(className, rule.needle)) return rule.rank;
    }
    return -1;
}

std::string_view playerClassRulesText() noexcept {
    return "AlexCharacter / SteveCharacter / DungeonsCharacter / PlayerCharacter / "
           "PlayerPawn; excluding Controller / PlayerState / HUD / Mock / Mob / Wolf";
}

uint64_t findLocalPlayerPawn(ue::Engine& engine) {
    ue::WorldView view(engine);

    const auto world = view.currentWorld();
    if (!world) return 0;

    uint64_t best = 0;
    int bestRank = 9999;
    for (auto const& actor : view.actors(*world)) {
        if (!actor.address) continue;
        const int rank = playerClassRank(actor.className);
        if (rank < 0 || rank >= bestRank) continue;
        bestRank = rank;
        best = actor.address;
    }
    return best;
}

std::optional<uint64_t> findMovementComponent(ue::Engine& engine, uint64_t pawn) {
    if (!pawn) return std::nullopt;

    const uint64_t pawnClass = engine.objects().classOf(pawn);
    if (auto off = engine.reflection().findProperty(pawnClass, "CharacterMovement");
        off && off->offset > 0) {
        if (auto movement = readPtr(pawn, static_cast<uint32_t>(off->offset))) {
            return movement;
        }
    }

    // 退路: 在对象表里找 Outer == pawn 的移动组件。
    uint64_t found = 0;
    engine.objects().for_each([&](ue::ObjectStat const& st) {
        if (icontains(st.className, "CharacterMovementComponent") &&
            engine.objects().outerOf(st.address) == pawn) {
            found = st.address;
            return false;
        }
        return true;
    });
    if (!found) return std::nullopt;
    return found;
}

std::optional<uint64_t> findPlayerAttributeSet(ue::Engine& engine, uint64_t pawn) {
    if (!pawn) return std::nullopt;

    uint64_t found = 0;
    engine.objects().for_each([&](ue::ObjectStat const& st) {
        if (st.className != "ATR_Movement") return true;

        uint64_t outer = st.address;
        for (int depth = 1; depth <= maxOuterDepth; ++depth) {
            outer = engine.objects().outerOf(outer);
            if (!outer) break;
            if (outer == pawn) { found = st.address; return false; }
        }
        return true;
    });

    if (!found) return std::nullopt;
    return found;
}

} // namespace epsilon::game::dungeons2
