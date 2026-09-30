// ============================================================================
//  name_pool.h — FNamePool (GNames) 解析
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
//  这里的所有读取都经过 safe_read —— 目标内存里任何指针都不可信, 必须假设
//  它随时可能是垃圾值。宁可不解析, 也不能让游戏崩。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mcd2::ue {

class NamePool {
public:
    // blocks_va = GNames + 0x10 (FNameEntryAllocator::Blocks 的地址)
    explicit NamePool(uint64_t blocks_va = 0) : blocks_(blocks_va) {}

    [[nodiscard]] bool valid() const noexcept { return blocks_ != 0; }
    [[nodiscard]] uint64_t blocks_address() const noexcept { return blocks_; }
    void set_blocks(uint64_t va) noexcept { blocks_ = va; }

    // 把比较索引解析成名字。失败返回空串。
    [[nodiscard]] std::string resolve(int32_t index) const;

    // 解析但不带缓存, 用于校验候选 GNames 是否正确。
    [[nodiscard]] std::string resolve_uncached(int32_t index) const;

    // 判断某个 index 是否可以解析出合法名字。
    [[nodiscard]] bool probe(int32_t index) const;

    // 统计前 n 个索引里有多少能解析出来 —— 用来给候选 blocks 地址打分。
    [[nodiscard]] int score(int32_t max_index = 256) const;

private:
    mutable std::vector<std::pair<int32_t, std::string>> cache_;
    uint64_t blocks_ = 0;
};

// 从模块内存里搜索 FNamePool::Blocks 数组的地址。
//
//  思路(与 analysis/re/mcd2.py 的 find_gnames 同源, 但 C++ 侧更严格):
//    在 .data 里找 64 KiB 对齐、且指向一段合法 FNameEntry 链的指针。
//    命中之后再向前回退, 找到这个指针数组的起始处 —— 那就是 Blocks[]。
//
//  参数:
//    data_base / data_size : .data 节区的运行时地址与长度
//    max_candidates        : 最多检查多少个候选(限制耗时)
//  返回: Blocks 数组地址, 0 表示没找到。
uint64_t locate_name_pool_blocks(uint64_t data_base, size_t data_size,
                                 size_t max_candidates = 4096);

} // namespace mcd2::ue
