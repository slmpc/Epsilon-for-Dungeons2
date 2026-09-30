// ============================================================================
//  namePool.h — FNamePool (GNames) 解析
//
//  UE5 的名字不再放在 `TNameEntryArray` 里, 而是 `FNamePool`:
//      struct FNameEntryAllocator {
//          mutable FRWLock Lock;        // +0x00, 8 字节
//          uint32   CurrentBlock;       // +0x08
//          uint32   CurrentByteCursor;  // +0x0C
//          uint8*   Blocks[8192];       // +0x10   ← 我们只关心这个
//      };
//  每个 Block 固定 64 KiB。索引编码为 (Block << 16) | Offset, 所以一个块
//  最多容纳 65536 字节的名字数据。条目头部是紧凑位域:
//      bit0     : bIsWide (1 = UTF-16)
//      bit1..5  : 未使用
//      bit6..15 : Len (10 bit, 最大 1023)
//  随后是 Len 字节/宽字符的名字本体, 再跟一个 NUL, 并按 2 字节对齐。
//
//  这里的所有读取都经过 safeRead —— 目标内存里任何指针都不可信, 必须假设
//  它随时可能是垃圾值。宁可不解析, 也不能让游戏崩。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace epsilon::ue {

class NamePool {
public:
    // blocksVa = GNames + 0x10 (FNameEntryAllocator::Blocks 的地址)
    explicit NamePool(uint64_t blocksVa = 0) : blocks_(blocksVa) {}

    [[nodiscard]] bool valid() const noexcept { return blocks_ != 0; }
    [[nodiscard]] uint64_t blocksAddress() const noexcept { return blocks_; }
    void setBlocks(uint64_t va) noexcept { blocks_ = va; }

    // 把比较索引解析成名字。失败返回空串。
    [[nodiscard]] std::string resolve(int32_t index) const;

    // 解析但不带缓存, 用于校验候选 GNames 是否正确。
    [[nodiscard]] std::string resolveUncached(int32_t index) const;

    // 判断某个 index 是否可以解析出合法名字。
    [[nodiscard]] bool probe(int32_t index) const;

    // 从块 0 起始处顺序走条目链, 返回能连续解析出的条目数。
    // 这是判定"候选地址是不是真的 FNamePool"的手段。
    //
    // 注意: 不能改成"按索引 0..N 采样" —— 索引是 (Block<<16)|ByteOffset,
    // 相邻索引落在同一块里相差 1 字节的位置, 而不是相邻条目。
    [[nodiscard]] int score(int32_t maxEntries = 64) const;

private:
    mutable std::vector<std::pair<int32_t, std::string>> cache_;
    uint64_t blocks_ = 0;
};

} // namespace epsilon::ue
