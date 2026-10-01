#pragma once

#include "payload/game/ue/NamePool.h"
#include "payload/game/Offsets.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epsilon::game::ue {

// FField / FProperty 层的布局。默认取 Offsets.h 的实测常量, 整体可替换。
struct ReflectionLayout {
    uint32_t fieldNext    = offsets::field::next;
    uint32_t fieldName    = offsets::field::namePrivate;
    uint32_t fieldClass   = offsets::field::classPrivate;
    uint32_t propArrayDim = offsets::property::arrayDim;
    uint32_t propSize     = offsets::property::size;
    uint32_t propFlags    = offsets::property::flags;
    uint32_t propOffset   = offsets::property::offset;
};

struct PropertyField {
    uint64_t    address = 0;      // FProperty*
    std::string name;
    std::string type;
    std::string structType;       // FStructProperty 的 Struct 名(否则空)
    std::string objectType;       // FObjectProperty 的 PropertyClass 名(否则空)
    int32_t     offset = 0;       // 相对对象起始的字节偏移
    int32_t     size = 0;         // ElementSize
    int32_t     arrayDim = 1;
    uint64_t    flags = 0;
};

class Reflection {
public:
    explicit Reflection(NamePool const* names = nullptr) : names_(names) {}
    void bind(NamePool const* names) { names_ = names; }
    [[nodiscard]] bool ready() const noexcept { return names_ && names_->valid(); }

    [[nodiscard]] std::vector<PropertyField> propertiesOf(uint64_t structObj) const;
    [[nodiscard]] std::optional<PropertyField> findProperty(uint64_t structObj,
                                                            std::string_view name) const;
    [[nodiscard]] std::vector<PropertyField> allPropertiesInherited(uint64_t structObj) const;

    [[nodiscard]] int32_t     structSize(uint64_t structObj) const;
    [[nodiscard]] uint64_t    superStruct(uint64_t structObj) const;
    [[nodiscard]] std::string classNameOf(uint64_t obj) const;

    // ---- FField 层原语: 供只读诊断使用, 让调用方不必自己算偏移 ----
    [[nodiscard]] std::string fieldNameAt(uint64_t field) const;
    [[nodiscard]] uint64_t    fieldNextOf(uint64_t field) const;

    [[nodiscard]] ReflectionLayout const& layout() const noexcept { return layout_; }
    void setLayout(ReflectionLayout const& l) { layout_ = l; }

    // 自愈探到的"属性链起点"偏移。0 = 仍在用 Offsets::uStruct::childProps。
    [[nodiscard]] uint32_t discoveredChildPropsOffset() const noexcept;

private:
    [[nodiscard]] std::vector<PropertyField> walk(uint64_t firstField, size_t limit) const;

    NamePool const*  names_ = nullptr;
    ReflectionLayout layout_{};
    // propertiesOf 是 const 的, 但自愈过程要在里面记录探到的偏移。
    mutable uint32_t lastChildPropsOffset_ = 0;
};

} // namespace epsilon::game::ue
