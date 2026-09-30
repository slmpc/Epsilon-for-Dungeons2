// ============================================================================
//  reflection.cpp
// ============================================================================
#include "payload/ue/reflection.h"

#include "common/pe_image.h"
#include "common/text.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace mcd2::ue {
namespace {

constexpr size_t kMaxChain = 4096;   // 属性链长度上限, 防环

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
        // (FFieldClass 的 NamePrivate 在 +0x18, 已在实测中确认)
        uint64_t klass = 0;
        if (safe_read(&klass, reinterpret_cast<const void*>(f + layout_.field_class), 8) && klass) {
            int32_t cls_name_idx = 0;
            if (safe_read(&cls_name_idx, reinterpret_cast<const void*>(klass + 0x18), 4)) {
                pf.type = names_->resolve(cls_name_idx);
            }
        }

        // StructProperty / ObjectProperty 的内层类型
        const bool is_struct = icontains(pf.type, "StructProperty");
        const bool is_object = icontains(pf.type, "ObjectProperty") ||
                               icontains(pf.type, "ClassProperty") ||
                               icontains(pf.type, "InterfaceProperty");
        if (is_struct || is_object) {
            for (uint32_t off : kObjectInnerCandidates) {
                uint64_t inner = 0;
                if (!safe_read(&inner, reinterpret_cast<const void*>(f + off), 8) || !inner) continue;
                int32_t idx = 0;
                if (!safe_read(&idx, reinterpret_cast<const void*>(inner + 0x18), 4)) continue;
                std::string in = names_->resolve(idx);
                if (in.empty() || in == "None") continue;
                if (is_struct) pf.struct_type = std::move(in);
                else           pf.object_type = std::move(in);
                break;
            }
        }

        out.push_back(std::move(pf));

        uint64_t next = 0;
        if (!safe_read(&next, reinterpret_cast<const void*>(f + layout_.field_next), 8)) break;
        if (next == f) break;              // 自环
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
