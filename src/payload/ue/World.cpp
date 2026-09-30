// ============================================================================
//  world.cpp
// ============================================================================
#include "payload/ue/World.h"

#include "common/PeImage.h"
#include "common/Text.h"

#include <algorithm>

namespace epsilon::ue {
namespace {

// 偏移经验值(Dungeons2 实测基线, 仅作反射失败时的退路)
constexpr int32_t kFallbackPersistentLevel = 0x30;   // UWorld::PersistentLevel
constexpr int32_t kFallbackActors          = 0x40;   // ULevel::Actors

// 场景组件里相对位置字段(capsule/root)常见的偏移。取不到就跳过, 不影响其它功能。
constexpr uint32_t kOffActorRootComponent = 0x1B8;
constexpr uint32_t kOffComponentLocation  = 0x160;

} // namespace

std::optional<int32_t> WorldView::findOffset(uint64_t klass, std::string_view prop,
                                              int32_t fallback, std::string& how) const {
    if (klass) {
        if (auto pf = eng_.reflection().findProperty(klass, prop)) {
            how = fmt("反射 {}::{} → +{:#x}", eng_.objects().nameOf(klass), pf->name, pf->offset);
            return pf->offset;
        }
        // 本类没有就沿继承链找
        for (auto& pf : eng_.reflection().allPropertiesInherited(klass)) {
            if (iequals(pf.name, prop)) {
                how = fmt("反射(继承链) {} → +{:#x}", pf.name, pf.offset);
                return pf.offset;
            }
        }
    }
    if (fallback >= 0) {
        how = fmt("经验值 +{:#x}(反射不可用)", fallback);
        return fallback;
    }
    how = "未解析";
    return std::nullopt;
}

std::optional<ArrayHeader> WorldView::readArray(uint64_t owner, int32_t offset) const {
    if (!owner) return std::nullopt;
    ArrayHeader h{};
    if (!safeRead(&h, reinterpret_cast<const void*>(owner + offset), sizeof(h))) return std::nullopt;
    if (h.num < 0 || h.num > 10'000'000) return std::nullopt;
    if (h.data == 0 && h.num > 0) return std::nullopt;
    return h;
}

std::optional<WorldInfo> WorldView::currentWorld() const {
    WorldInfo wi;

    // 优先用已定位的 GWorld
    if (eng_.gworld().value) {
        wi.address = eng_.gworld().value;
    } else {
        // 退路: 在对象表里找第一个 World 实例
        eng_.objects().for_each([&](ObjectStat const& st) {
            if (iequals(st.className, "World")) {
                wi.address = st.address;
                return false;
            }
            return true;
        });
    }
    if (!wi.address) return std::nullopt;

    wi.className = eng_.objects().classNameOf(wi.address);
    wi.name = eng_.objects().nameOf(wi.address);

    // UWorld::PersistentLevel
    auto off = findOffset(eng_.objects().classOf(wi.address), "PersistentLevel",
                           kFallbackPersistentLevel, wi.offsetSource);
    if (!off) return wi;
    wi.offsetPersistentLevel = *off;

    uint64_t lvl = 0;
    if (safeRead(&lvl, reinterpret_cast<const void*>(wi.address + *off), 8)) {
        wi.persistentLevel = lvl;
        if (lvl) wi.levelName = eng_.objects().nameOf(lvl);
    }
    return wi;
}

std::optional<LevelInfo> WorldView::persistentLevel() const {
    auto wi = currentWorld();
    if (!wi || !wi->persistentLevel) return std::nullopt;

    LevelInfo li;
    li.address = wi->persistentLevel;
    li.name = eng_.objects().nameOf(li.address);

    auto off = findOffset(eng_.objects().classOf(li.address), "Actors",
                           kFallbackActors, li.offsetSource);
    if (off) {
        li.offsetActors = *off;
        if (auto arr = readArray(li.address, *off)) li.actorCount = arr->num;
    }
    return li;
}

std::vector<ActorInfo> WorldView::actors(WorldInfo const& world, size_t limit) const {
    std::vector<ActorInfo> out;
    if (!world.persistentLevel) return out;

    const uint64_t level = world.persistentLevel;
    const uint64_t levelClass = eng_.objects().classOf(level);

    std::string how;
    auto off = findOffset(levelClass, "Actors", kFallbackActors, how);
    if (!off) return out;

    auto arr = readArray(level, *off);
    if (!arr || !arr->data || arr->num <= 0) return out;

    const int32_t total = (limit && static_cast<size_t>(arr->num) > limit)
                              ? static_cast<int32_t>(limit) : arr->num;

    out.reserve(static_cast<size_t>(total));
    for (int32_t i = 0; i < total; ++i) {
        uint64_t ap = 0;
        if (!safeRead(&ap, reinterpret_cast<const void*>(arr->data + 8ull * i), 8) || !ap)
            continue;

        ActorInfo ai;
        ai.index = i;
        ai.address = ap;
        ai.klass = eng_.objects().classOf(ap);
        ai.name = eng_.objects().nameOf(ap);
        ai.className = eng_.objects().classNameOf(ap);
        out.push_back(std::move(ai));
    }
    return out;
}

std::vector<ActorInfo> WorldView::actors(size_t limit) const {
    auto wi = currentWorld();
    if (!wi) return {};
    return actors(*wi, limit);
}

std::optional<int32_t> WorldView::resolveOffset(uint64_t klass, std::string_view prop) const {
    std::string how;
    return findOffset(klass, prop, -1, how);
}

} // namespace epsilon::ue
