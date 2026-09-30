// ============================================================================
//  name_pool.cpp
// ============================================================================
#include "payload/ue/name_pool.h"

#include "common/pe_image.h"
#include "common/text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>     // WideCharToMultiByte / CP_UTF8

#include <cstring>
#include <optional>

namespace mcd2::ue {
namespace {

constexpr uint64_t kBlockSize    = 0x10000;      // 64 KiB
constexpr uint32_t kMaxNameLen   = 1023;         // 10 bit
constexpr int      kMaxSanityIdx = 1 << 22;      // 只接受合理范围内的索引

// 解析一个 FNameEntry 头。返回 (名字文本, 消耗字节数)。
//
// ⚠️ UE5 的 FNameEntry **不带 NUL 终止符**：
//      [uint16 header][name bytes]   然后按 2 字节对齐
//    长度完全由头部的 Len 位域给出。
//
//    这一点必须从内存实测反推, 不能凭直觉。实测某块的头 32 字节:
//        1E 01 "None"          4 字符 → 占 2+4=6
//        10 03 "ByteProperty" 12 字符 → 占 2+12=14
//        C0 02 "IntProperty"  11 字符 → 占 2+11=13 → 对齐到 14
//    如果把名字当 C 字符串读(要求 str[len]=='\0'), 那么**每一条都会解析失败**
//    —— 曾经因此把一个完全正确的 FNamePool 判成无效地址。
struct Entry {
    std::string text;
    uint32_t    consumed = 0;   // 含头部, 已按 2 字节对齐
};

std::optional<Entry> read_entry(uint64_t va) {
    uint16_t header = 0;
    if (!safe_read(&header, reinterpret_cast<const void*>(va), sizeof(header))) return std::nullopt;

    // bit0 = bIsWide; bit1..5 = ProbeHash; bit6..15 = Len
    const bool     wide = (header & 1) != 0;
    const uint32_t len  = (header >> 6) & 0x3FF;
    if (len == 0 || len > kMaxNameLen) return std::nullopt;

    Entry e;
    if (wide) {
        std::wstring w(len, L'\0');
        if (!safe_read(w.data(), reinterpret_cast<const void*>(va + 2),
                       static_cast<size_t>(len) * sizeof(wchar_t)))
            return std::nullopt;

        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                            nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::nullopt;
        e.text.resize(static_cast<size_t>(n));
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                              e.text.data(), n, nullptr, nullptr);
    } else {
        e.text.resize(len);
        if (!safe_read(e.text.data(), reinterpret_cast<const void*>(va + 2), len))
            return std::nullopt;
        // 轻量合法性检查: 名字里不该出现控制字符。
        // 不要求 NUL —— 见上面的说明。
        for (unsigned char c : e.text) {
            if (c < 0x20 || c == 0x7F) return std::nullopt;
        }
    }

    uint32_t consumed = 2 + static_cast<uint32_t>(wide ? len * 2 : len);
    if (consumed & 1) ++consumed;         // 2 字节对齐
    e.consumed = consumed;
    return e;
}

// 判断某个 64 KiB 块起始处是否像一串合法的 FNameEntry。
// 抽 want 个连续条目, 全部合法才算通过。
bool chain_looks_valid(uint64_t block_va, int want = 4) {
    uint64_t p = block_va;
    for (int i = 0; i < want; ++i) {
        auto e = read_entry(p);
        if (!e) return false;
        if (e->consumed == 0) return false;
        p += e->consumed;
        if (p - block_va > kBlockSize) return false;
    }
    return true;
}

} // namespace

std::string NamePool::resolve_uncached(int32_t index) const {
    if (!blocks_ || index < 0 || index > kMaxSanityIdx) return {};

    const uint32_t block_idx = static_cast<uint32_t>(index) >> 16;
    const uint32_t offset    = static_cast<uint32_t>(index) & 0xFFFF;

    uint64_t block = 0;
    if (!safe_read(&block, reinterpret_cast<const void*>(blocks_ + 8ull * block_idx), sizeof(block)))
        return {};
    if (block == 0) return {};

    // ⚠️ 偏移量是**除以 2** 存的, 不是字节偏移。
    //
    // 条目按 2 字节对齐, 所以 UE 把字节偏移右移一位塞进 16 位字段, 换出一位
    // 额外的寻址范围。读的时候要乘以 2 还原。
    //
    // 这一点必须实测确认, 凭"索引就是字节偏移"的直觉写会得到**完全错误但
    // 看起来像有数据**的结果 —— 索引会落在某个条目中间, 解出的"名字"是
    // 别扭的 UTF-16 乱码, 而地址、布局、校验全都正常, 极难定位。
    //
    // 实测依据(两个独立样本, 都精确落在条目边界上):
    //   索引 0x6EB × 2 = 0xDD6 → F6 04 + "/Script/CoreUObject"(19 字符)
    //   索引 0x20B × 2 = 0x416 → 80 01 + "Object"(6 字符)   ← UObject 的 CDO
    auto e = read_entry(block + 2ull * offset);
    if (!e) return {};
    return e->text;
}

std::string NamePool::resolve(int32_t index) const {
    for (auto const& [cached_idx, cached_name] : cache_) {
        if (cached_idx == index) return cached_name;
    }
    std::string name = resolve_uncached(index);
    if (cache_.size() < 4096) cache_.emplace_back(index, name);
    return name;
}

bool NamePool::probe(int32_t index) const {
    return !resolve_uncached(index).empty();
}

int NamePool::score(int32_t max_entries) const {
    // ⚠️ 必须**顺序走条目链**, 不能按索引采样。
    //
    // FName 的索引是 (Block << 16) | ByteOffset —— 相邻索引不是相邻条目,
    // 而是同一个块里相差 1 字节的两个位置。按 0,1,2,...,N 采样的话, 绝大
    // 多数会落在某个条目的中间, 解析必然失败。
    //
    // 这个错误曾经让一个**完全正确**的 FNamePool 地址被判为无效:
    // 真机上前 64 个索引"只命中 4/64", 看起来像地址偏了, 实际上地址和布局
    // 都对(dump 出来是 None / ByteProperty / IntProperty ...), 只是校验
    // 方式错了。所以这里改成从块 0 的起始处逐条往前走。
    if (!blocks_) return 0;

    uint64_t block0 = 0;
    if (!safe_read(&block0, reinterpret_cast<const void*>(blocks_), sizeof(block0)))
        return 0;
    if (!block0) return 0;

    int ok = 0;
    uint64_t p = block0;
    for (int32_t i = 0; i < max_entries; ++i) {
        auto e = read_entry(p);
        if (!e || e->consumed == 0) break;
        ++ok;
        p += e->consumed;
        if (p - block0 >= kBlockSize) break;   // 走完一个 64 KiB 块
    }
    return ok;
}

} // namespace mcd2::ue
