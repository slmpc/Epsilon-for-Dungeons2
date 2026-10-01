#include "payload/game/Field.h"

namespace epsilon::game {

std::optional<ArrayHeader> readArray(uint64_t owner, uint32_t offset) noexcept {
    if (!owner) return std::nullopt;

    ArrayHeader h{};
    if (!safeRead(&h, reinterpret_cast<const void*>(owner + offset), sizeof(h))) return std::nullopt;
    if (h.num < 0 || h.num > 10'000'000) return std::nullopt;
    if (h.data == 0 && h.num > 0) return std::nullopt;
    return h;
}

std::optional<uint64_t> arrayElement(const ArrayHeader& arr, int32_t index) noexcept {
    if (!arr.data || index < 0 || index >= arr.num) return std::nullopt;
    return readPtr(arr.data, static_cast<uint32_t>(index) * 8u);
}

} // namespace epsilon::game
