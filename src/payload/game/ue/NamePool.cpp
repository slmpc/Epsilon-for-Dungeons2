#include "payload/game/ue/NamePool.h"

#include "payload/game/Field.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace epsilon::game::ue {
namespace {

namespace np = offsets::namePool;

struct Entry {
    std::string text;
    uint32_t    consumed = 0;   // 含头部, 已按 2 字节对齐
};

std::optional<Entry> readEntry(uint64_t va) {
    auto header = readU16(va, 0);
    if (!header) return std::nullopt;

    const bool     wide = (*header & 1) != 0;
    const uint32_t len  = (*header >> 6) & 0x3FF;
    if (len == 0 || len > np::maxNameLength) return std::nullopt;

    Entry e;
    const uint64_t body = va + np::headerSize;

    if (wide) {
        std::wstring w(len, L'\0');
        if (!safeRead(w.data(), reinterpret_cast<const void*>(body),
                      static_cast<size_t>(len) * sizeof(wchar_t))) {
            return std::nullopt;
        }
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                            nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::nullopt;
        e.text.resize(static_cast<size_t>(n));
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                              e.text.data(), n, nullptr, nullptr);
    } else {
        e.text.resize(len);
        if (!safeRead(e.text.data(), reinterpret_cast<const void*>(body), len)) return std::nullopt;
        for (unsigned char c : e.text) {
            if (c < 0x20 || c == 0x7F) return std::nullopt;
        }
    }

    uint32_t consumed = np::headerSize + static_cast<uint32_t>(wide ? len * 2 : len);
    if (consumed & 1) ++consumed;
    e.consumed = consumed;
    return e;
}

uint64_t blockAt(const NamePool& pool, uint32_t blockIdx) {
    return readPtr(pool.blocksAddress(), blockIdx * 8u).value_or(0);
}

} // namespace

std::string NamePool::resolveUncached(int32_t index) const {
    if (!blocks_ || index < 0 || index > np::maxSanityIndex) return {};

    const uint32_t blockIdx = static_cast<uint32_t>(index) >> 16;
    const uint32_t offset   = static_cast<uint32_t>(index) & 0xFFFF;

    const uint64_t block = blockAt(*this, blockIdx);
    if (!block) return {};

    auto e = readEntry(block + (static_cast<uint64_t>(offset) << np::encodedShift));
    if (!e) return {};
    return e->text;
}

std::string NamePool::resolve(int32_t index) const {
    for (auto const& [cachedIdx, cachedName] : cache_) {
        if (cachedIdx == index) return cachedName;
    }
    std::string name = resolveUncached(index);
    if (cache_.size() < 4096) cache_.emplace_back(index, name);
    return name;
}

bool NamePool::probe(int32_t index) const {
    return !resolveUncached(index).empty();
}

int NamePool::score(int32_t maxEntries) const {
    if (!blocks_) return 0;

    auto block0 = readPtr(blocks_, 0);
    if (!block0) return 0;

    int ok = 0;
    uint64_t p = *block0;
    for (int32_t i = 0; i < maxEntries; ++i) {
        auto e = readEntry(p);
        if (!e || e->consumed == 0) break;
        ++ok;
        p += e->consumed;
        if (p - *block0 >= np::blockSize) break;
    }
    return ok;
}

std::optional<std::pair<int32_t, std::string>> NamePool::findByName(
    std::string_view name, int32_t maxEntries) const {
    if (!blocks_ || name.empty()) return std::nullopt;

    auto block0 = readPtr(blocks_, 0);
    if (!block0) return std::nullopt;

    uint64_t p = *block0;
    for (int32_t i = 0; i < maxEntries; ++i) {
        auto e = readEntry(p);
        if (!e || e->consumed == 0) break;
        if (e->text == name) {
            return std::make_pair(
                static_cast<int32_t>((p - *block0) >> np::encodedShift), e->text);
        }
        p += e->consumed;
        if (p - *block0 >= np::blockSize) break;
    }
    return std::nullopt;
}

} // namespace epsilon::game::ue
