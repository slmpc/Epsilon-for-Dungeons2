// ============================================================================
//  reflection.cpp
// ============================================================================
#include "payload/ue/reflection.h"
#include "payload/ue/object_array.h"

#include "common/pe_image.h"
#include "common/text.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace mcd2::ue {
namespace {

constexpr size_t kMaxChain = 4096;    // 属性链长度上限, 防环
constexpr int32_t kMaxSaneOffset = 0x100000;   // 1 MiB, 超过必然是垃圾

// FField 的类指针(FProperty 子类描述符)。FProperty 继承了 FField,
// 其 ClassPrivate 告诉我们这是 FloatProperty 还是 StructProperty 等。
constexpr uint32_t kOffFieldClass = 0x08;

// FStructProperty::Struct / FObjectProperty::PropertyClass 一般在 0x78 附近。
// 只在类型名匹配时才去读, 读不到就当没有。
constexpr std::array<uint32_t, 3> kObjectInnerCandidates = {0x78, 0x70, 0x80};

} // namespace

std::string Reflection::field_name(uint64_t field) const {
    if (!field || !ready()) return {};
    int32_t idx = 0;
    if (!safe_read(&idx, reinterpret_cast<const void*>(field + layout_.field_name), 4)) return {};
    return names_->resolve(idx);
}

int Reflection::score_chain(uint64_t first_field, ReflectionLayout const& l,
                            size_t probe) const {
    if (!ready() || !first_field) return 0;

    int score = 0;
    uint64_t f = first_field;
    size_t seen = 0;
    size_t good_names = 0;

    while (f && seen < probe) {
        ++seen;

        int32_t name_idx = 0;
        if (!safe_read(&name_idx, reinterpret_cast<const void*>(f + l.field_name), 4)) break;
        const std::string nm = names_->resolve(name_idx);
        if (nm.empty()) break;
        ++good_names;
        score += 10;

        // 偏移合理性: 属性偏移必须是小的非负数
        int32_t off = 0;
        if (safe_read(&off, reinterpret_cast<const void*>(f + l.prop_offset), 4)) {
            if (off >= 0 && off < kMaxSaneOffset) score += 4; else score -= 8;
        }

        // ArrayDim / ElementSize 也必须合理
        int32_t dim = 0, esize = 0;
        if (safe_read(&dim, reinterpret_cast<const void*>(f + l.prop_arraydim), 4) &&
            safe_read(&esize, reinterpret_cast<const void*>(f + l.prop_size), 4)) {
            if (dim >= 1 && dim <= 4096) score += 2; else score -= 4;
            if (esize > 0 && esize <= 0x10000) score += 2; else score -= 4;
        }

        uint64_t next = 0;
        if (!safe_read(&next, reinterpret_cast<const void*>(f + l.field_next), 8)) break;
        if (next == f) break;                    // 自环
        f = next;
    }

    if (seen == 0 || good_names == 0) return 0;
    // 命中越少越不可信
    if (seen < 3) score /= 2;
    return score;
}

int Reflection::calibrate(ObjectArray const& objects) {
    // 候选布局: Next 与 Name 是最容易变的两项, 其余按 FProperty 标准布局。
    struct Variant { uint32_t next; uint32_t name; };
    static constexpr std::array<Variant, 6> kVariants = {{
        {0x20, 0x28},   // UE5 标准(默认)
        {0x28, 0x30},   // 部分构建里 FField 前面多一个 vtable 槽
        {0x18, 0x20},
        {0x20, 0x30},
        {0x28, 0x20},
        {0x30, 0x28},
    }};

    // 挑几个属性多的样本类。用名字找, 找不到就跳过 —— 不硬依赖某个具体类。
    static constexpr std::array<std::string_view, 8> kSamples = {
        "Actor", "Pawn", "Character", "PlayerController",
        "GameInstance", "World", "Level", "SceneComponent",
    };

    std::vector<uint64_t> sample_classes;
    for (auto nm : kSamples) {
        if (const uint64_t c = objects.find_class(nm)) sample_classes.push_back(c);
        if (sample_classes.size() >= 5) break;
    }

    if (sample_classes.empty()) {
        layout_.source = "找不到样本类, 沿用默认布局";
        layout_.confidence = 0;
        return 0;
    }

    int best_score = -1;
    ReflectionLayout best = layout_;

    for (auto const& v : kVariants) {
        ReflectionLayout cand = layout_;
        cand.field_next = v.next;
        cand.field_name = v.name;

        int total = 0;
        int classes_with_chain = 0;
        for (uint64_t cls : sample_classes) {
            uint64_t first = 0;
            if (!safe_read(&first, reinterpret_cast<const void*>(cls + kOffStructChildProps), 8))
                continue;
            if (!first) continue;
            const int s = score_chain(first, cand, 12);
            if (s > 0) ++classes_with_chain;
            total += s;
        }
        // 多类别同时走通比单类高分更重要
        total += classes_with_chain * 50;

        if (total > best_score) {
            best_score = total;
            best = cand;
        }
    }

    layout_ = best;
    if (best_score <= 0) {
        layout_.confidence = 0;
        layout_.source = "所有候选布局都走不通(游戏可能仍在加载)";
    } else {
        layout_.confidence = std::min(3, 1 + best_score / 400);
        layout_.source = fmt("探测胜出: Next=+{:#x} Name=+{:#x} (score {})",
                             layout_.field_next, layout_.field_name, best_score);
    }
    return layout_.confidence;
}

std::vector<PropertyField> Reflection::walk(uint64_t first_field, size_t limit) const {
    std::vector<PropertyField> out;
    if (!ready() || !first_field) return out;

    uint64_t f = first_field;
    size_t seen = 0;
    while (f && seen < std::min(limit, kMaxChain)) {
        ++seen;

        int32_t name_idx = 0;
        if (!safe_read(&name_idx, reinterpret_cast<const void*>(f + layout_.field_name), 4)) break;
        const std::string nm = names_->resolve(name_idx);
        if (nm.empty()) break;

        PropertyField pf;
        pf.address = f;
        pf.name = nm;

        safe_read(&pf.array_dim, reinterpret_cast<const void*>(f + layout_.prop_arraydim), 4);
        safe_read(&pf.size,      reinterpret_cast<const void*>(f + layout_.prop_size), 4);
        safe_read(&pf.flags,     reinterpret_cast<const void*>(f + layout_.prop_flags), 8);
        safe_read(&pf.offset,    reinterpret_cast<const void*>(f + layout_.prop_offset), 4);

        if (pf.array_dim < 1) pf.array_dim = 1;

        // 类型名: FField::ClassPrivate -> FFieldClass 的 NamePrivate
        uint64_t klass = 0;
        if (safe_read(&klass, reinterpret_cast<const void*>(f + kOffFieldClass), 8) && klass) {
            int32_t cls_name_idx = 0;
            if (safe_read(&cls_name_idx, reinterpret_cast<const void*>(klass + 0x18), 4)) {
                pf.type = names_->resolve(cls_name_idx);
            }
        }

        // StructProperty / ObjectProperty 的内层类型
        if (icontains(pf.type, "StructProperty")) {
            for (uint32_t off : kObjectInnerCandidates) {
                uint64_t inner = 0;
                if (!safe_read(&inner, reinterpret_cast<const void*>(f + off), 8) || !inner) continue;
                int32_t idx = 0;
                if (!safe_read(&idx, reinterpret_cast<const void*>(inner + 0x18), 4)) continue;
                std::string in = names_->resolve(idx);
                if (!in.empty() && in != "None") { pf.struct_type = in; break; }
            }
        } else if (icontains(pf.type, "ObjectProperty") || icontains(pf.type, "ClassProperty") ||
                   icontains(pf.type, "InterfaceProperty")) {
            for (uint32_t off : kObjectInnerCandidates) {
                uint64_t inner = 0;
                if (!safe_read(&inner, reinterpret_cast<const void*>(f + off), 8) || !inner) continue;
                int32_t idx = 0;
                if (!safe_read(&idx, reinterpret_cast<const void*>(inner + 0x18), 4)) continue;
                std::string in = names_->resolve(idx);
                if (!in.empty() && in != "None") { pf.object_type = in; break; }
            }
        }

        out.push_back(std::move(pf));

        uint64_t next = 0;
        if (!safe_read(&next, reinterpret_cast<const void*>(f + layout_.field_next), 8)) break;
        if (next == f) break;
        f = next;
    }
    return out;
}

std::vector<PropertyField> Reflection::properties_of(uint64_t struct_obj) const {
    if (!ready() || !struct_obj) return {};
    uint64_t first = 0;
    if (!safe_read(&first, reinterpret_cast<const void*>(struct_obj + kOffStructChildProps), 8))
        return {};
    return walk(first, kMaxChain);
}

std::optional<PropertyField> Reflection::find_property(uint64_t struct_obj,
                                                       std::string_view name) const {
    for (auto& p : properties_of(struct_obj)) {
        if (iequals(p.name, name)) return p;
    }
    return std::nullopt;
}

std::vector<PropertyField> Reflection::all_properties_inherited(uint64_t struct_obj) const {
    std::vector<PropertyField> out;
    std::vector<std::string> seen;

    uint64_t cls = struct_obj;
    int depth = 0;
    while (cls && depth++ < 64) {
        for (auto& p : properties_of(cls)) {
            if (std::find(seen.begin(), seen.end(), p.name) != seen.end()) continue;
            seen.push_back(p.name);
            out.push_back(p);
        }
        cls = super_struct(cls);
    }
    return out;
}

int32_t Reflection::struct_size(uint64_t struct_obj) const {
    int32_t n = 0;
    if (!struct_obj || !safe_read(&n, reinterpret_cast<const void*>(struct_obj + kOffStructPropsSize), 4))
        return 0;
    return n;
}

uint64_t Reflection::super_struct(uint64_t struct_obj) const {
    uint64_t s = 0;
    if (!struct_obj || !safe_read(&s, reinterpret_cast<const void*>(struct_obj + kOffStructSuper), 8))
        return 0;
    return s;
}

std::string Reflection::class_name_of(uint64_t obj) const {
    if (!obj || !ready()) return {};
    uint64_t klass = 0;
    if (!safe_read(&klass, reinterpret_cast<const void*>(obj + 0x10), 8) || !klass) return {};
    int32_t idx = 0;
    if (!safe_read(&idx, reinterpret_cast<const void*>(klass + 0x18), 4)) return {};
    return names_->resolve(idx);
}

} // namespace mcd2::ue
