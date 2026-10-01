#include "payload/game/ue/Reflection.h"

#include "common/Text.h"
#include "payload/game/Field.h"

#include <algorithm>

namespace epsilon::game::ue {
namespace {

// 属性链长度上限, 防环。
constexpr size_t maxChain = 4096;

// 自愈的强判据: 链里出现这些已知的引擎属性名, 就基本不可能是垃圾数据凑出来的。
constexpr std::string_view strongPropertyNames[] = {
    "MaxWalkSpeed", "JumpZVelocity", "GravityScale", "AirControl",
    "MaxAcceleration", "RelativeLocation", "RootComponent",
};

bool isStrongPropertyName(std::string_view name) {
    for (auto const& n : strongPropertyNames) {
        if (n == name) return true;
    }
    return false;
}

} // namespace

std::string Reflection::fieldNameAt(uint64_t field) const {
    if (!field || !ready()) return {};
    auto idx = readI32(field, layout_.fieldName);
    if (!idx) return {};
    return names_->resolve(*idx);
}

uint64_t Reflection::fieldNextOf(uint64_t field) const {
    return readPtr(field, layout_.fieldNext).value_or(0);
}

std::vector<PropertyField> Reflection::walk(uint64_t firstField, size_t limit) const {
    std::vector<PropertyField> out;
    if (!ready() || !firstField) return out;

    uint64_t f = firstField;
    size_t seen = 0;
    while (f && seen < std::min(limit, maxChain)) {
        ++seen;

        const std::string nm = fieldNameAt(f);
        if (nm.empty()) break;

        PropertyField pf;
        pf.address = f;
        pf.name = nm;

        pf.arrayDim = readI32(f, layout_.propArrayDim).value_or(0);
        pf.size     = readI32(f, layout_.propSize).value_or(0);
        pf.flags    = readField<uint64_t>(f, layout_.propFlags).value_or(0);
        pf.offset   = readI32(f, layout_.propOffset).value_or(0);
        if (pf.arrayDim < 1) pf.arrayDim = 1;

        // 类型名: FField::ClassPrivate -> FFieldClass::NamePrivate
        if (auto klass = readPtr(f, layout_.fieldClass)) {
            if (auto clsNameIdx = readI32(*klass, offsets::fieldClass::namePrivate)) {
                pf.type = names_->resolve(*clsNameIdx);
            }
        }

        const bool isStruct = icontains(pf.type, "StructProperty");
        const bool isObject = icontains(pf.type, "ObjectProperty") ||
                              icontains(pf.type, "ClassProperty") ||
                              icontains(pf.type, "InterfaceProperty");
        if (isStruct || isObject) {
            for (uint32_t off : offsets::propertyInner::slots) {
                auto inner = readPtr(f, off);
                if (!inner) continue;
                auto idx = readI32(*inner, offsets::fieldClass::namePrivate);
                if (!idx) continue;
                std::string in = names_->resolve(*idx);
                if (in.empty() || in == "None") continue;
                if (isStruct) pf.structType = std::move(in);
                else          pf.objectType = std::move(in);
                break;
            }
        }

        out.push_back(std::move(pf));

        const uint64_t next = fieldNextOf(f);
        if (!next || next == f) break;
        f = next;
    }
    return out;
}

std::vector<PropertyField> Reflection::propertiesOf(uint64_t structObj) const {
    if (!ready() || !structObj) return {};

    // 自愈要试十几个候选槽, 缓存命中时不必再扫。
    if (lastChildPropsOffset_ != 0) {
        if (auto cached = readPtr(structObj, lastChildPropsOffset_)) {
            auto got = walk(*cached, maxChain);
            if (!got.empty()) return got;
        }
    }

    if (auto first = readPtr(structObj, offsets::uStruct::childProps)) {
        auto got = walk(*first, maxChain);
        if (!got.empty()) return got;
    }

    // ---- 自愈: 静态偏移失效时现场找回属性链起点 ----
    // 判据演进与两次踩坑记录见 docs/reverse/reflection-limits.md。
    struct Offer {
        uint32_t offset = 0;
        std::vector<PropertyField> chain;
        bool     strong = false;
        size_t   propertyLike = 0;
    };
    std::vector<Offer> offers;

    namespace sc = offsets::scan;
    for (uint32_t probe = sc::structPointerSlotFirst; probe <= sc::structPointerSlotLast;
         probe += sc::structPointerSlotStep) {
        if (probe == offsets::uStruct::childProps) continue;

        auto cand = readPtr(structObj, probe);
        if (!cand) continue;
        if (fieldNameAt(*cand).empty()) continue;

        auto chain = walk(*cand, maxChain);
        if (chain.empty()) continue;

        Offer o;
        o.offset = probe;
        for (auto const& p : chain) {
            if (isStrongPropertyName(p.name)) o.strong = true;
            if (icontains(p.type, "Property")) ++o.propertyLike;
        }
        o.chain = std::move(chain);
        offers.push_back(std::move(o));
    }

    if (offers.empty()) return {};

    // 优先采信"链里含已知属性名"的候选; 否则退到属性最多的那个。
    Offer const* best = nullptr;
    for (auto const& o : offers) {
        if (!o.strong) continue;
        if (!best || o.chain.size() > best->chain.size()) best = &o;
    }
    if (!best) {
        for (auto const& o : offers) {
            if (o.propertyLike == 0) continue;
            if (!best || o.chain.size() > best->chain.size()) best = &o;
        }
    }
    if (!best) return {};

    lastChildPropsOffset_ = best->offset;
    return best->chain;
}

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
    return readI32(structObj, offsets::uStruct::propertiesSize).value_or(0);
}

uint64_t Reflection::superStruct(uint64_t structObj) const {
    return readPtr(structObj, offsets::uStruct::superStruct).value_or(0);
}

std::string Reflection::classNameOf(uint64_t obj) const {
    if (!obj || !ready()) return {};
    // UClass 本身也是 UObject, 类名就是那个 UClass 对象的 NamePrivate。
    auto klass = readPtr(obj, offsets::object::classPrivate);
    if (!klass) return {};
    auto idx = readI32(*klass, offsets::object::namePrivate);
    if (!idx) return {};
    return names_->resolve(*idx);
}

} // namespace epsilon::game::ue
