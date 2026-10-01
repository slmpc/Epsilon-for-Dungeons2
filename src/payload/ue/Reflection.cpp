// ============================================================================
//  reflection.cpp
// ============================================================================
#include "payload/ue/Reflection.h"

#include "common/PeImage.h"
#include "common/Text.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace epsilon::ue {
namespace {

constexpr size_t kMaxChain = 4096;   // 属性链长度上限, 防环

// FStructProperty::Struct / FObjectProperty::PropertyClass 一般在 0x78 附近。
// 只在类型名匹配时才去读, 读不到就当没有。
constexpr std::array<uint32_t, 3> kObjectInnerCandidates = {0x78, 0x70, 0x80};

} // namespace

std::string Reflection::fieldName(uint64_t field) const {
    if (!field || !ready()) return {};
    int32_t idx = 0;
    if (!safeRead(&idx, reinterpret_cast<const void*>(field + layout_.fieldName), 4)) return {};
    return names_->resolve(idx);
}

std::vector<PropertyField> Reflection::walk(uint64_t firstField, size_t limit) const {
    std::vector<PropertyField> out;
    if (!ready() || !firstField) return out;

    uint64_t f = firstField;
    size_t seen = 0;
    while (f && seen < std::min(limit, kMaxChain)) {
        ++seen;

        int32_t nameIdx = 0;
        if (!safeRead(&nameIdx, reinterpret_cast<const void*>(f + layout_.fieldName), 4)) break;
        const std::string nm = names_->resolve(nameIdx);
        if (nm.empty()) break;

        PropertyField pf;
        pf.address = f;
        pf.name = nm;

        safeRead(&pf.arrayDim, reinterpret_cast<const void*>(f + layout_.propArrayDim), 4);
        safeRead(&pf.size,      reinterpret_cast<const void*>(f + layout_.propSize), 4);
        safeRead(&pf.flags,     reinterpret_cast<const void*>(f + layout_.propFlags), 8);
        safeRead(&pf.offset,    reinterpret_cast<const void*>(f + layout_.propOffset), 4);

        if (pf.arrayDim < 1) pf.arrayDim = 1;

        // 类型名: FField::ClassPrivate -> FFieldClass 的 NamePrivate
        // (FFieldClass 的 NamePrivate 在 +0x18, 已在实测中确认)
        uint64_t klass = 0;
        if (safeRead(&klass, reinterpret_cast<const void*>(f + layout_.fieldClass), 8) && klass) {
            int32_t clsNameIdx = 0;
            if (safeRead(&clsNameIdx, reinterpret_cast<const void*>(klass + 0x18), 4)) {
                pf.type = names_->resolve(clsNameIdx);
            }
        }

        // StructProperty / ObjectProperty 的内层类型
        const bool isStruct = icontains(pf.type, "StructProperty");
        const bool isObject = icontains(pf.type, "ObjectProperty") ||
                               icontains(pf.type, "ClassProperty") ||
                               icontains(pf.type, "InterfaceProperty");
        if (isStruct || isObject) {
            for (uint32_t off : kObjectInnerCandidates) {
                uint64_t inner = 0;
                if (!safeRead(&inner, reinterpret_cast<const void*>(f + off), 8) || !inner) continue;
                int32_t idx = 0;
                if (!safeRead(&idx, reinterpret_cast<const void*>(inner + 0x18), 4)) continue;
                std::string in = names_->resolve(idx);
                if (in.empty() || in == "None") continue;
                if (isStruct) pf.structType = std::move(in);
                else           pf.objectType = std::move(in);
                break;
            }
        }

        out.push_back(std::move(pf));

        uint64_t next = 0;
        if (!safeRead(&next, reinterpret_cast<const void*>(f + layout_.fieldNext), 8)) break;
        if (next == f) break;              // 自环
        f = next;
    }
    return out;
}

std::vector<PropertyField> Reflection::propertiesOf(uint64_t structObj) const {
    if (!ready() || !structObj) return {};

    // 已经探到过真实偏移就直接用。这一步很重要: 自愈扫描要试 13 个候选槽,
    // 每次 propertiesOf 都跑一遍的话, 枚举 Actor(几十次调用)会明显变慢。
    if (lastChildPropsOffset_ != 0) {
        uint64_t cached = 0;
        if (safeRead(&cached, reinterpret_cast<const void*>(structObj + lastChildPropsOffset_), 8)) {
            auto got = walk(cached, kMaxChain);
            if (!got.empty()) return got;
        }
        // 缓存偏移读不出来就退回重新探测(下次仍会缓存)。
    }

    uint64_t first = 0;
    if (safeRead(&first, reinterpret_cast<const void*>(structObj + offStructChildProps), 8)) {
        auto got = walk(first, kMaxChain);
        if (!got.empty()) return got;
    }

    // ---- 自愈: 静态偏移失效时现场找回属性链 ----
    //
    // 为什么需要这一步: UStruct 各字段的偏移是**每个引擎版本都可能变**的。
    // 实测这个构建上报的类大小/继承链都对, 但属性数恒为 0 —— 也就是
    // SuperStruct 偏移是对的, 但属性链起点偏移不对(注意本构建里
    // SuperStruct / ChildProperties 这两个反射名都不在字符串表里,
    // 说明 UStruct 的布局与常见 UE5 版本不同)。
    //
    // 属性链起点是 UStruct 里某个 8 字节指针, 它指向的 FField 必然有一个能
    // 解析出非空名字的 NamePrivate。于是可以按"指针 + 该处能读出合法名字 +
    // 走出来的链里有 *Property 类型"来确认候选偏移 —— 比继续猜常量可靠,
    // 而且下次游戏更新后也能自己找回。
    for (uint32_t probe = 0x20; probe <= 0x98; probe += 8) {
        if (probe == offStructChildProps) continue;      // 上面试过了
        uint64_t cand = 0;
        if (!safeRead(&cand, reinterpret_cast<const void*>(structObj + probe), 8)) continue;
        if (!cand) continue;
        // 先只看第一个字段能不能解出名字, 便宜且足够筛掉绝大多数候选。
        int32_t nameIdx = 0;
        if (!safeRead(&nameIdx, reinterpret_cast<const void*>(cand + layout_.fieldName), 4)) continue;
        if (names_->resolve(nameIdx).empty()) continue;

        auto probeChain = walk(cand, kMaxChain);
        if (probeChain.empty()) continue;
        // 再按类型名筛一层: FProperty 链里应当出现 *Property 类型名。
        size_t propertyLike = 0;
        for (auto const& p : probeChain) {
            if (icontains(p.type, "Property")) ++propertyLike;
        }
        if (propertyLike == 0) continue;

        lastChildPropsOffset_ = probe;
        return probeChain;
    }
    return {};
}

// 上一次自愈找出的属性链偏移。0 表示仍在用静态偏移。
uint32_t Reflection::discoveredChildPropsOffset() const noexcept {
    return lastChildPropsOffset_;
}

std::optional<PropertyField> Reflection::findProperty(uint64_t structObj,
                                                       std::string_view name) const {
    for (auto& p : propertiesOf(structObj)) {
        if (iequals(p.name, name)) return p;
    }
    return std::nullopt;
}

std::vector<PropertyField> Reflection::allPropertiesInherited(uint64_t structObj) const {
    std::vector<PropertyField> out;
    std::vector<std::string> seen;

    uint64_t cls = structObj;
    int depth = 0;
    while (cls && depth++ < 64) {
        for (auto& p : propertiesOf(cls)) {
            if (std::find(seen.begin(), seen.end(), p.name) != seen.end()) continue;
            seen.push_back(p.name);
            out.push_back(p);
        }
        cls = superStruct(cls);
    }
    return out;
}

int32_t Reflection::structSize(uint64_t structObj) const {
    int32_t n = 0;
    if (!structObj || !safeRead(&n, reinterpret_cast<const void*>(structObj + offStructPropsSize), 4))
        return 0;
    return n;
}

uint64_t Reflection::superStruct(uint64_t structObj) const {
    uint64_t s = 0;
    if (!structObj || !safeRead(&s, reinterpret_cast<const void*>(structObj + offStructSuper), 8))
        return 0;
    return s;
}

std::string Reflection::classNameOf(uint64_t obj) const {
    if (!obj || !ready()) return {};
    uint64_t klass = 0;
    if (!safeRead(&klass, reinterpret_cast<const void*>(obj + 0x10), 8) || !klass) return {};
    int32_t idx = 0;
    if (!safeRead(&idx, reinterpret_cast<const void*>(klass + 0x18), 4)) return {};
    return names_->resolve(idx);
}

} // namespace epsilon::ue
