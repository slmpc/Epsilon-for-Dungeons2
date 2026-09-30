// ============================================================================
//  reflection.h — 运行时属性偏移重建
//
//  这是整个框架最有价值的一块, 直接对应分析结论 §6.1 的第 5 步:
//
//    "UE 的 UClass 在 UObject::StaticClass() 注册时会把属性偏移写进
//     FProperty::Offset_Internal。定位这个注册过程, 就能在运行时把全部偏移
//     dump 出来 —— 这比静态猜偏移可靠得多。"
//
//  为什么必须要它: Binds.Cache 里有 158k 条"名字→类型"记录, 但**不含任何
//  偏移数字**(实测那些 u32 是字符串长度, 不是 offset)。所以血量/伤害/坐标
//  这些字段的偏移只能现场重建 —— 就是这里做的事。
//
//  UStruct / UField / FField 布局(UE5):
//      UStruct : UField
//          +0x40  UStruct*   SuperStruct
//          +0x48  UField*    Children          (函数等子字段链)
//          +0x50  FField*    ChildProperties   (属性链的第一个)
//          +0x58  int32      PropertiesSize
//      UField : UObject
//          +0x28  UField*    Next
//      FField
//          +0x20  FField*    Next
//          +0x28  FName      NamePrivate
//      FProperty : FField
//          +0x30  int32      ArrayDim
//          +0x34  int32      ElementSize
//          +0x38  uint64     PropertyFlags
//          +0x40  int32      Offset_Internal  ← 我们要的东西
//
//  这些偏移**只认实测基线, 不做多方案探测**。游戏更新后如果布局变了,
//  `props` 命令输出的偏移会明显不合常理(值荒谬或属性名读不出来),
//  那时改这里的常量即可 —— 比维护一套自动探测更可预测。
// ============================================================================
#pragma once

#include "payload/ue/NamePool.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace epsilon::ue {

// UStruct 层偏移(UE5 各版本间很稳定, 实测于 5.6.1)
inline constexpr uint32_t offStructSuper      = 0x40;
inline constexpr uint32_t offStructChildren   = 0x48;
inline constexpr uint32_t offStructChildProps = 0x50;
inline constexpr uint32_t offStructPropsSize  = 0x58;

// FField / FProperty 层偏移。集中在结构体里, 改一处即可。
struct ReflectionLayout {
    uint32_t fieldNext    = 0x20;   // FField::Next
    uint32_t fieldName    = 0x28;   // FField::NamePrivate (FName)
    uint32_t fieldClass   = 0x08;   // FField::ClassPrivate(用来判属性类型)
    uint32_t propArrayDim = 0x30;
    uint32_t propSize     = 0x34;
    uint32_t propFlags    = 0x38;
    uint32_t propOffset   = 0x40;   // ← Offset_Internal
};

struct PropertyField {
    uint64_t    address = 0;      // FProperty*
    std::string name;             // 属性名, 如 "Health"
    std::string type;             // 类型名, 如 "FloatProperty"
    std::string structType;      // FStructProperty 的 Struct 名(否则空)
    std::string objectType;      // FObjectProperty 的 PropertyClass 名(否则空)
    int32_t     offset = 0;       // 相对对象起始的字节偏移
    int32_t     size = 0;         // ElementSize
    int32_t     arrayDim = 1;    // ArrayDim
    uint64_t    flags = 0;
};

class Reflection {
public:
    explicit Reflection(NamePool const* names = nullptr) : names_(names) {}
    void bind(NamePool const* names) { names_ = names; }
    [[nodiscard]] bool ready() const noexcept { return names_ && names_->valid(); }

    // 遍历一个 UStruct/UClass 的属性链。
    [[nodiscard]] std::vector<PropertyField> propertiesOf(uint64_t structObj) const;

    // 按名字找属性(仅本类)。
    [[nodiscard]] std::optional<PropertyField> findProperty(uint64_t structObj,
                                                             std::string_view name) const;

    // 沿继承链收集全部属性(子类覆盖父类同名)。
    [[nodiscard]] std::vector<PropertyField> allPropertiesInherited(uint64_t structObj) const;

    [[nodiscard]] int32_t     structSize(uint64_t structObj) const;
    [[nodiscard]] uint64_t    superStruct(uint64_t structObj) const;
    [[nodiscard]] std::string classNameOf(uint64_t obj) const;

    [[nodiscard]] ReflectionLayout const& layout() const noexcept { return layout_; }
    // 需要适配别的引擎版本时, 在这里整体替换布局常量。
    void setLayout(ReflectionLayout const& l) { layout_ = l; }

private:
    [[nodiscard]] std::string fieldName(uint64_t field) const;
    [[nodiscard]] std::vector<PropertyField> walk(uint64_t firstField, size_t limit) const;

    NamePool const*  names_ = nullptr;
    ReflectionLayout layout_{};
};

} // namespace epsilon::ue
