// ============================================================================
//  object_array.h — FUObjectArray (GObjects) 遍历
//
//  实测布局(analysis/re/mcd2.py 已在活体进程上验证):
//      GObjects               +0x10  TUObjectArray* Objects
//      FUObjectArray::Objects +0x10  FUObjectItem** ChunkTable
//                             +0x20  int32 MaxElements
//                             +0x24  int32 NumElements
//                             +0x28  int32 MaxChunks
//                             +0x2C  int32 NumChunks
//      FUObjectItem 步长 0x18:
//                             +0x00  UObject* Object
//                             +0x08  EObjectFlags Flags
//                             +0x0C  int32 ClusterRootIndex
//                             +0x10  int32 SerialNumber
//      UObject:
//                             +0x08  EObjectFlags Flags
//                             +0x0C  int32 InternalIndex   ← 对象自己的索引
//                             +0x10  UClass* ClassPrivate
//                             +0x18  FName NamePrivate
//                             +0x20  UObject* OuterPrivate
//
//  取值链: T=*(u64*)(G+0x10); Ch=*(u64*)(T+8*(I>>16));
//          Obj=*(u64*)(Ch+0x18*(I&0xFFFF))
// ============================================================================
#pragma once

#include "payload/ue/name_pool.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mcd2::ue {

inline constexpr uint32_t kItemStride  = 0x18;
inline constexpr uint32_t kChunkItems  = 0x10000;

// UObject 字段偏移
inline constexpr uint32_t kOffObjFlags = 0x08;
inline constexpr uint32_t kOffObjIndex = 0x0C;
inline constexpr uint32_t kOffObjClass = 0x10;
inline constexpr uint32_t kOffObjName  = 0x18;
inline constexpr uint32_t kOffObjOuter = 0x20;

// FUObjectArray 字段偏移
inline constexpr uint32_t kOffChunkTable  = 0x10;
inline constexpr uint32_t kOffMaxElements = 0x20;
inline constexpr uint32_t kOffNumElements = 0x24;
inline constexpr uint32_t kOffMaxChunks   = 0x28;
inline constexpr uint32_t kOffNumChunks   = 0x2C;
inline constexpr uint32_t kOffObjectsPtr  = 0x10;   // GObjects -> TUObjectArray*

inline constexpr uint64_t kObjsToArray = 0x10;      // GObjects -> Objects 的偏移

struct ObjectStat {
    int32_t     index = 0;
    uint64_t    address = 0;      // UObject*
    uint64_t    klass = 0;        // UClass*
    std::string name;             // UObject::GetName()
    std::string class_name;       // 类名
    std::string outer_name;       // 外部对象名
    std::string full_name;        // ClassName Outer:Name
};

class ObjectArray {
public:
    // gobjects_va = FUObjectArray 的运行时地址
    explicit ObjectArray(uint64_t gobjects_va = 0) : gobjects_(gobjects_va) {}

    [[nodiscard]] bool valid() const noexcept { return gobjects_ != 0 && num_elements_ > 0; }
    [[nodiscard]] uint64_t address() const noexcept { return gobjects_; }
    [[nodiscard]] int32_t  num_elements() const noexcept { return num_elements_; }
    [[nodiscard]] int32_t  max_elements() const noexcept { return max_elements_; }

    // 重新从目标内存刷新计数与块表。
    bool refresh();
    // 结构自洽校验 —— 决定"这个候选 GObjects 地址是否可信"。
    // 返回 0..3 的置信度, 0 = 不可信。
    int  validate() const;

    // 取第 index 个 FUObjectItem 的 UObject*。越界或空洞返回 0。
    [[nodiscard]] uint64_t object_at(int32_t index) const;
    [[nodiscard]] uint64_t item_at(int32_t index) const;

    // 取 UObject 的各个字段。
    [[nodiscard]] uint64_t    class_of(uint64_t obj) const;
    [[nodiscard]] uint64_t    outer_of(uint64_t obj) const;
    [[nodiscard]] int32_t     name_index_of(uint64_t obj) const;
    [[nodiscard]] std::string name_of(uint64_t obj) const;
    [[nodiscard]] std::string class_name_of(uint64_t obj) const;
    [[nodiscard]] std::string outer_name_of(uint64_t obj) const;
    [[nodiscard]] std::string full_name_of(uint64_t obj) const;

    [[nodiscard]] ObjectStat describe(int32_t index) const;

    // 遍历所有对象。回调返回 false 可提前终止。
    // 遍历过程中会跳过空槽; 每步都做 safe_read, 索引越界即停。
    void for_each(std::function<bool(ObjectStat const&)> const& fn,
                  int32_t start = 0, int32_t max_count = -1) const;

    // 按名字查对象(全量线性扫描, 名字不区分大小写)。找到一个就返回。
    [[nodiscard]] uint64_t find_object_by_name(std::string_view name) const;
    // 按完整名(形如 "ClassName /Path:Name")查。
    [[nodiscard]] uint64_t find_object_by_full_name(std::string_view full) const;
    // 找某个类的 CDO / 类对象本身。
    [[nodiscard]] uint64_t find_class(std::string_view class_name) const;

    void set_name_pool(NamePool const* np) { names_ = np; }
    [[nodiscard]] NamePool const* name_pool() const noexcept { return names_; }

private:
    [[nodiscard]] uint64_t chunk_at(int32_t chunk_idx) const;

    uint64_t          gobjects_ = 0;
    uint64_t          objects_ = 0;       // TUObjectArray*
    uint64_t          chunk_table_ = 0;
    int32_t           num_elements_ = 0;
    int32_t           max_elements_ = 0;
    int32_t           num_chunks_ = 0;
    int32_t           max_chunks_ = 0;
    NamePool const*   names_ = nullptr;
};

} // namespace mcd2::ue
