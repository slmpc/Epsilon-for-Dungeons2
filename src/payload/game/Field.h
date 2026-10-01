// ============================================================================
//  Field.h — 目标进程内存的字段级读写原语
//
//  这一层是 game/ 之下唯一的 safeRead/safeWrite 出口: 只接受 (基址, 偏移),
//  永远不做裸指针运算。目标进程里任何指针都可能是垃圾值。
// ============================================================================
#pragma once

#include "common/PeImage.h"
#include "payload/game/Offsets.h"

#include <cstdint>
#include <optional>
#include <type_traits>

namespace epsilon::game {

// 读 base+offset 处的 T。base 为 0 或读不到返回 nullopt。
template <typename T>
[[nodiscard]] std::optional<T> readField(uint64_t base, uint32_t offset) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "只支持 POD");
    if (!base) return std::nullopt;
    return safeReadPod<T>(base + offset);
}

// 读 base+offset 处的指针。空指针与读取失败都返回 nullopt。
[[nodiscard]] inline std::optional<uint64_t> readPtr(uint64_t base, uint32_t offset) noexcept {
    auto v = readField<uint64_t>(base, offset);
    if (!v || *v == 0) return std::nullopt;
    return v;
}

[[nodiscard]] inline std::optional<int32_t> readI32(uint64_t base, uint32_t offset) noexcept {
    return readField<int32_t>(base, offset);
}

[[nodiscard]] inline std::optional<uint16_t> readU16(uint64_t base, uint32_t offset) noexcept {
    return readField<uint16_t>(base, offset);
}

[[nodiscard]] inline std::optional<float> readF32(uint64_t base, uint32_t offset) noexcept {
    return readField<float>(base, offset);
}

// 写 base+offset 处的 T。走 safeWrite(必要时放宽页保护)。
template <typename T>
bool writeField(uint64_t base, uint32_t offset, T value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "只支持 POD");
    if (!base) return false;
    return safeWrite(reinterpret_cast<void*>(base + offset), &value, sizeof(T));
}

[[nodiscard]] inline bool writeF32(uint64_t base, uint32_t offset, float value) noexcept {
    return writeField<float>(base, offset, value);
}

// ---------------------------------------------------------------- TArray<T>
struct ArrayHeader {
    uint64_t data = 0;
    int32_t  num = 0;
    int32_t  max = 0;
};

// 读一个 TArray 头并做形态校验: num 必须落在合理范围, data 与 num 必须同生同灭。
[[nodiscard]] std::optional<ArrayHeader> readArray(uint64_t owner, uint32_t offset) noexcept;

// 取 TArray 的第 index 个 8 字节元素(元素为指针的数组)。
[[nodiscard]] std::optional<uint64_t> arrayElement(const ArrayHeader& arr, int32_t index) noexcept;

} // namespace epsilon::game
