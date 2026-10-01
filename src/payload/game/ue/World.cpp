#include "payload/game/ue/World.h"

#include "common/Text.h"

namespace epsilon::game::ue {

std::optional<int32_t> WorldView::findOffset(uint64_t klass, std::string_view prop,
                                             int32_t fallback, std::string& how) const {
    if (klass) {
        if (auto pf = eng_.reflection().findProperty(klass, prop)) {
            how = fmt("反射 {}::{} → +{:#x}", eng_.objects().nameOf(klass), pf->name, pf->offset);
            return pf->offset;
        }
        for (auto& pf : eng_.reflection().allPropertiesInherited(klass)) {
            if (iequals(pf.name, prop)) {
                how = fmt("反射(继承链) {} → +{:#x}", pf.name, pf.offset);
                return pf.offset;
            }
        }
    }
    if (fallback >= 0) {
        how = fmt("已知表 +{:#x}(反射不可用)", fallback);
        return fallback;
    }
    how = "未解析";
    return std::nullopt;
}

std::optional<WorldInfo> WorldView::currentWorld() const {
    WorldInfo wi;

    // 只采信通过类名校验的 GWorld 槽 —— 未校验的指针可能是"读到了但不知道是什么"。
    if (eng_.gworld().verified) {
        wi.address = eng_.gworld().value;
    } else {
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

    auto off = findOffset(eng_.objects().classOf(wi.address), "PersistentLevel",
                          static_cast<int32_t>(offsets::world::persistentLevel), wi.offsetSource);
    if (!off) return wi;
    wi.offsetPersistentLevel = *off;

    if (auto lvl = readPtr(wi.address, static_cast<uint32_t>(*off))) {
        wi.persistentLevel = *lvl;
        wi.levelName = eng_.objects().nameOf(*lvl);
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
                          static_cast<int32_t>(offsets::level::actors), li.offsetSource);
    if (off) {
        li.offsetActors = *off;
        if (auto arr = readArray(li.address, static_cast<uint32_t>(*off))) li.actorCount = arr->num;
    }
    return li;
}

std::vector<ActorInfo> WorldView::actors(WorldInfo const& world, size_t limit) const {
    std::vector<ActorInfo> out;
    if (!world.persistentLevel) return out;

    const uint64_t level = world.persistentLevel;

    std::string how;
    auto off = findOffset(eng_.objects().classOf(level), "Actors",
                          static_cast<int32_t>(offsets::level::actors), how);
    if (!off) return out;

    auto arr = readArray(level, static_cast<uint32_t>(*off));
    if (!arr || !arr->data || arr->num <= 0) return out;

    const int32_t total = (limit && static_cast<size_t>(arr->num) > limit)
                              ? static_cast<int32_t>(limit) : arr->num;

    out.reserve(static_cast<size_t>(total));
    for (int32_t i = 0; i < total; ++i) {
        auto ap = arrayElement(*arr, i);
        if (!ap) continue;

        ActorInfo ai;
        ai.index = i;
        ai.address = *ap;
        ai.klass = eng_.objects().classOf(*ap);
        ai.name = eng_.objects().nameOf(*ap);
        ai.className = eng_.objects().classNameOf(*ap);
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

} // namespace epsilon::game::ue
