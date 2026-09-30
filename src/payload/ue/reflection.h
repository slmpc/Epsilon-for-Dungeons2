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
//  这些偏移可能随引擎版本变动, 所以**不盲信硬编码**: 用一组候选偏移组合去
//  遍历属性链, 谁能让链条干净走通就给谁投票, 最高分才采信。
// ============================================================================
#pragma once

#include "payload/ue/name_pool.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mcd2::ue {

class ObjectArray;   // 仅用于 calibrate 采样

// 已确认的 UStruct 偏移(UE5 各版本间很稳定)
inline constexpr uint32_t kOffStructSuper      = 0x40;
inline constexpr uint32_t kOffStructChildren   = 0x48;
inline constexpr uint32_t kOffStructChildProps = 0x50;
inline constexpr uint32_t kOffStructPropsSize  = 0x58;
inline constexpr uint32_t kOffFieldNext        = 0x28;   // UField::Next

struct PropertyField {
    uint64_t    address = 0;      // FProperty*
    std::string name;             // 属性名, 如 "Health"
    std::string type;             // 类型名, 如 "FloatProperty"
    std::string struct_type;      // FStructProperty 的 Struct 名(否则空)
    std::string object_type;      // FObjectProperty 的 PropertyClass 名(否则空)
    int32_t     offset = 0;       // 相对对象起始的字节偏移
    int32_t     size = 0;         // ElementSize
    int32_t     array_dim = 1;    // ArrayDim
    uint64_t    flags = 0;
};

// 属性链的偏移组合。默认值是 UE5.6 常见布局, 会被 calibrate() 校正。
struct ReflectionLayout {
    uint32_t field_next    = 0x20;   // FField::Next
    uint32_t field_name    = 0x28;   // FField::NamePrivate (FName)
    uint32_t prop_arraydim = 0x30;
    uint32_t prop_size     = 0x34;
    uint32_t prop_flags    = 0x38;
    uint32_t prop_offset   = 0x40;
    int      confidence    = 0;      // 0..3, 越高越可信
    std::string source;              // 定下来的依据
};

class Reflection {
public:
    explicit Reflection(NamePool const* names = nullptr) : names_(names) {}
    void bind(NamePool const* names) { names_ = names; }
    [[nodiscard]] bool ready() const noexcept { return names_ && names_->valid(); }

    // 遍历一个 UStruct/UClass 的属性链。
    [[nodiscard]] std::vector<PropertyField> properties_of(uint64_t struct_obj) const;

    // 按名字找属性(仅本类)。
    [[nodiscard]] std::optional<PropertyField> find_property(uint64_t struct_obj,
                                                             std::string_view name) const;

    // 沿继承链收集全部属性(子类覆盖父类同名)。
    [[nodiscard]] std::vector<PropertyField> all_properties_inherited(uint64_t struct_obj) const;

    [[nodiscard]] int32_t     struct_size(uint64_t struct_obj) const;
    [[nodiscard]] uint64_t    super_struct(uint64_t struct_obj) const;
    [[nodiscard]] std::string class_name_of(uint64_t obj) const;

    [[nodiscard]] ReflectionLayout const& layout() const noexcept { return layout_; }
    void set_layout(ReflectionLayout const& l) { layout_ = l; }

    // 用一组样本类探测并落定布局。返回 confidence。
    int calibrate(ObjectArray const& objects);

private:
    [[nodiscard]] std::string field_name(uint64_t field) const;
    [[nodiscard]] std::vector<PropertyField> walk(uint64_t first_field, size_t limit) const;
    [[nodiscard]] int score_chain(uint64_t first_field, ReflectionLayout const& l,
                                  size_t probe = 12) const;

    NamePool const*  names_ = nullptr;
    ReflectionLayout layout_{};
};

} // namespace mcd2::ue
