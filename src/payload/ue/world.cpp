// ============================================================================
//  world.cpp
// ============================================================================
#include "payload/ue/world.h"

#include "common/pe_image.h"
#include "common/text.h"

#include <algorithm>

namespace mcd2::ue {
namespace {

// 偏移经验值(MCD2 实测基线, 仅作反射失败时的退路)
constexpr int32_t kFallbackPersistentLevel = 0x30;   // UWorld::PersistentLevel
constexpr int32_t kFallbackActors          = 0x40;   // ULevel::Actors

// 场景组件里相对位置字段(capsule/root)常见的偏移。取不到就跳过, 不影响其它功能。
constexpr uint32_t kOffActorRootComponent = 0x1B8;
constexpr uint32_t kOffComponentLocation  = 0x160;

} // namespace

std::optional<int32_t> WorldView::find_offset(uint64_t klass, std::string_view prop,
                                              int32_t fallback, std::string& how) const {
    if (klass) {
        if (auto pf = eng_.reflection().find_property(klass, prop)) {
            how = fmt("反射 {}::{} → +{:#x}", eng_.objects().name_of(klass), pf->name, pf->offset);
            return pf->offset;
        }
        // 本类没有就沿继承链找
        for (auto& pf : eng_.reflection().all_properties_inherited(klass)) {
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

std::optional<ArrayHeader> WorldView::read_array(uint64_t owner, int32_t offset) const {
    if (!owner) return std::nullopt;
    ArrayHeader h{};
    if (!safe_read(&h, reinterpret_cast<const void*>(owner + offset), sizeof(h))) return std::nullopt;
    if (h.num < 0 || h.num > 10'000'000) return std::nullopt;
    if (h.data == 0 && h.num > 0) return std::nullopt;
    return h;
}

std::optional<WorldInfo> WorldView::current_world() const {
    WorldInfo wi;

    // 优先用已定位的 GWorld
    if (eng_.gworld().value) {
        wi.address = eng_.gworld().value;
    } else {
        // 退路: 在对象表里找第一个 World 实例
        eng_.objects().for_each([&](ObjectStat const& st) {
            if (iequals(st.class_name, "World")) {
                wi.address = st.address;
                return false;
            }
            return true;
        });
    }
    if (!wi.address) return std::nullopt;

    wi.class_name = eng_.objects().class_name_of(wi.address);
    wi.name = eng_.objects().name_of(wi.address);

    // UWorld::PersistentLevel
    auto off = find_offset(eng_.objects().class_of(wi.address), "PersistentLevel",
                           kFallbackPersistentLevel, wi.offset_source);
    if (!off) return wi;
    wi.offset_persistent_level = *off;

    uint64_t lvl = 0;
    if (safe_read(&lvl, reinterpret_cast<const void*>(wi.address + *off), 8)) {
        wi.persistent_level = lvl;
        if (lvl) wi.level_name = eng_.objects().name_of(lvl);
    }
    return wi;
}

std::optional<LevelInfo> WorldView::persistent_level() const {
    auto wi = current_world();
    if (!wi || !wi->persistent_level) return std::nullopt;

    LevelInfo li;
    li.address = wi->persistent_level;
    li.name = eng_.objects().name_of(li.address);

    auto off = find_offset(eng_.objects().class_of(li.address), "Actors",
                           kFallbackActors, li.offset_source);
    if (off) {
        li.offset_actors = *off;
        if (auto arr = read_array(li.address, *off)) li.actor_count = arr->num;
    }
    return li;
}

std::vector<ActorInfo> WorldView::actors(WorldInfo const& world, size_t limit) const {
    std::vector<ActorInfo> out;
    if (!world.persistent_level) return out;

    const uint64_t level = world.persistent_level;
    const uint64_t level_class = eng_.objects().class_of(level);

    std::string how;
    auto off = find_offset(level_class, "Actors", kFallbackActors, how);
    if (!off) return out;

    auto arr = read_array(level, *off);
    if (!arr || !arr->data || arr->num <= 0) return out;

    const int32_t total = (limit && static_cast<size_t>(arr->num) > limit)
                              ? static_cast<int32_t>(limit) : arr->num;

    out.reserve(static_cast<size_t>(total));
    for (int32_t i = 0; i < total; ++i) {
        uint64_t ap = 0;
        if (!safe_read(&ap, reinterpret_cast<const void*>(arr->data + 8ull * i), 8) || !ap)
            continue;

        ActorInfo ai;
        ai.index = i;
        ai.address = ap;
        ai.klass = eng_.objects().class_of(ap);
        ai.name = eng_.objects().name_of(ap);
        ai.class_name = eng_.objects().class_name_of(ap);
        out.push_back(std::move(ai));
    }
    return out;
}

std::vector<ActorInfo> WorldView::actors(size_t limit) const {
    auto wi = current_world();
    if (!wi) return {};
    return actors(*wi, limit);
}

std::optional<int32_t> WorldView::resolve_offset(uint64_t klass, std::string_view prop) const {
    std::string how;
    return find_offset(klass, prop, -1, how);
}

} // namespace mcd2::ue
