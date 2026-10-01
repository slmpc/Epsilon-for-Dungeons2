// ============================================================================
//  NamePool.h — FNamePool (GNames) 解析
//
//  布局与校验依据: docs/offsets/name-pool.md
// ============================================================================
#pragma once

#include "payload/game/Offsets.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epsilon::game::ue {

// blocksVa = GNames + FNameEntryAllocator::Blocks
class NamePool {
public:
    explicit NamePool(uint64_t blocksVa = 0) : blocks_(blocksVa) {}

    [[nodiscard]] bool valid() const noexcept { return blocks_ != 0; }
    [[nodiscard]] uint64_t blocksAddress() const noexcept { return blocks_; }
    void setBlocks(uint64_t va) noexcept { blocks_ = va; }

    // 把比较索引解析成名字。失败返回空串。
    [[nodiscard]] std::string resolve(int32_t index) const;
    // 不走缓存, 用于校验候选 GNames。
    [[nodiscard]] std::string resolveUncached(int32_t index) const;
    [[nodiscard]] bool probe(int32_t index) const;

    // 顺序走块 0 的条目链, 返回能连续解析出的条目数。
    [[nodiscard]] int score(
        int32_t maxEntries = static_cast<int32_t>(offsets::namePool::scoreEntries)) const;

    // 顺序走块 0 的条目链按名字反查比较索引。返回 (索引, 名字)。
    [[nodiscard]] std::optional<std::pair<int32_t, std::string>> findByName(
        std::string_view name, int32_t maxEntries) const;

private:
    mutable std::vector<std::pair<int32_t, std::string>> cache_;
    uint64_t blocks_ = 0;
};

} // namespace epsilon::game::ue
