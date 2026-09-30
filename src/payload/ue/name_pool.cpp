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
struct Entry {
    std::string text;
    uint32_t    consumed = 0;
};

std::optional<Entry> read_entry(uint64_t va) {
    uint16_t header = 0;
    if (!safe_read(&header, reinterpret_cast<const void*>(va), sizeof(header))) return std::nullopt;

    // bit0 = bIsWide。奇数头部 = 宽字符条目。
    const bool     wide = (header & 1) != 0;
    const uint32_t len  = (header >> 6) & 0x3FF;
    if (len == 0 || len > kMaxNameLen) return std::nullopt;

    Entry e;
    if (wide) {
        std::wstring w(len, L'\0');
        if (!safe_read(w.data(), reinterpret_cast<const void*>(va + 2), len * sizeof(wchar_t)))
            return std::nullopt;
        wchar_t term = 0;
        if (!safe_read(&term, reinterpret_cast<const void*>(va + 2 + len * sizeof(wchar_t)), sizeof(term)))
            return std::nullopt;
        if (term != L'\0') return std::nullopt;

        // 转 UTF-8
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                            nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::nullopt;
        e.text.resize(static_cast<size_t>(n));
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                              e.text.data(), n, nullptr, nullptr);

        e.consumed = 2 + len * 2 + 2;   // 头 + 宽字符 + NUL(宽字符本身按 2 对齐)
    } else {
        std::string s(len + 1, '\0');
        if (!safe_read(s.data(), reinterpret_cast<const void*>(va + 2), len + 1)) return std::nullopt;
        if (s[len] != '\0') return std::nullopt;
        s.resize(len);
        e.text = std::move(s);
        e.consumed = 2 + len + 1;
        if (e.consumed & 1) ++e.consumed;   // 按 2 字节对齐
    }
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

    auto e = read_entry(block + offset);
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

int NamePool::score(int32_t max_index) const {
    int ok = 0;
    for (int32_t i = 0; i < max_index; ++i) {
        if (probe(i)) ++ok;
    }
    return ok;
}

} // namespace mcd2::ue
