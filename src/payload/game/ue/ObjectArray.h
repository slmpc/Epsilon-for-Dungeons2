#pragma once

#include "payload/game/ue/NamePool.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace epsilon::game::ue {

struct ObjectStat {
    int32_t     index = 0;
    uint64_t    address = 0;      // UObject*
    uint64_t    klass = 0;        // UClass*
    std::string name;             // UObject::GetName()
    std::string className;        // 类名
    std::string outerName;        // 外部对象名
    std::string fullName;         // ClassName Name
};

class ObjectArray {
public:
    // gObjectsVa = FUObjectArray 的运行时地址
    explicit ObjectArray(uint64_t gObjectsVa = 0) : gObjects_(gObjectsVa) {}

    [[nodiscard]] bool valid() const noexcept { return gObjects_ != 0 && numElements_ > 0; }
    [[nodiscard]] uint64_t address() const noexcept { return gObjects_; }
    [[nodiscard]] int32_t  numElements() const noexcept { return numElements_; }
    [[nodiscard]] int32_t  maxElements() const noexcept { return maxElements_; }

    // 重新从目标内存刷新计数与块表。
    bool refresh();
    // 结构自洽校验, 返回 0..3 的置信度, 0 = 不可信。
    int  validate() const;

    // 取第 index 个 FUObjectItem / UObject*。越界或空洞返回 0。
    [[nodiscard]] uint64_t objectAt(int32_t index) const;
    [[nodiscard]] uint64_t itemAt(int32_t index) const;

    [[nodiscard]] uint64_t    classOf(uint64_t obj) const;
    [[nodiscard]] uint64_t    outerOf(uint64_t obj) const;
    [[nodiscard]] int32_t     nameIndexOf(uint64_t obj) const;
    [[nodiscard]] std::string nameOf(uint64_t obj) const;
    [[nodiscard]] std::string classNameOf(uint64_t obj) const;
    [[nodiscard]] std::string outerNameOf(uint64_t obj) const;
    [[nodiscard]] std::string fullNameOf(uint64_t obj) const;

    [[nodiscard]] ObjectStat describe(int32_t index) const;

    // 遍历所有对象。回调返回 false 可提前终止, 空槽自动跳过。
    void for_each(std::function<bool(ObjectStat const&)> const& fn,
                  int32_t start = 0, int32_t maxCount = -1) const;

    // 全量线性扫描查对象(不区分大小写)。
    [[nodiscard]] uint64_t findObjectByName(std::string_view name) const;
    [[nodiscard]] uint64_t findObjectByFullName(std::string_view full) const;
    [[nodiscard]] uint64_t findClass(std::string_view className) const;

    void setNamePool(NamePool const* np) { names_ = np; }
    [[nodiscard]] NamePool const* namePool() const noexcept { return names_; }

private:
    [[nodiscard]] uint64_t chunkAt(int32_t chunkIdx) const;

    uint64_t          gObjects_ = 0;
    uint64_t          objects_ = 0;       // TUObjectArray*
    uint64_t          chunkTable_ = 0;
    int32_t           numElements_ = 0;
    int32_t           maxElements_ = 0;
    int32_t           numChunks_ = 0;
    int32_t           maxChunks_ = 0;
    NamePool const*   names_ = nullptr;
};

} // namespace epsilon::game::ue
