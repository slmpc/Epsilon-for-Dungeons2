// ============================================================================
//  pattern_scan.cpp
// ============================================================================
#include "common/pattern_scan.h"
#include "common/text.h"

#include <algorithm>
#include <cstring>

namespace mcd2 {
namespace {

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::optional<Pattern> Pattern::parse(std::string_view text) {
    Pattern p;

    // 先剥掉所有分隔符, 得到两种可能: 带 ? 的成对形式, 或纯紧凑十六进制。
    std::string compact;
    compact.reserve(text.size());
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        compact += c;
    }
    if (compact.empty()) return std::nullopt;

    // 逐 "token" 解析: 分隔符已被去掉, 所以按 2 字符步进。
    size_t i = 0;
    while (i < compact.size()) {
        // 通配符: '?' 或 '??' 或 'x'
        if (compact[i] == '?' || compact[i] == 'x' || compact[i] == 'X') {
            p.bytes_.push_back(0);
            p.mask_.push_back(0);
            ++i;
            if (i < compact.size() && (compact[i] == '?' || compact[i] == 'x' || compact[i] == 'X')) ++i;
            continue;
        }

        const int hi = hex_nibble(compact[i]);
        if (hi < 0) return std::nullopt;
        if (i + 1 >= compact.size()) return std::nullopt;
        const int lo = hex_nibble(compact[i + 1]);
        if (lo < 0) return std::nullopt;

        p.bytes_.push_back(static_cast<uint8_t>((hi << 4) | lo));
        p.mask_.push_back(1);
        i += 2;
    }

    if (p.bytes_.empty()) return std::nullopt;
    return p;
}

bool Pattern::matches(const uint8_t* buf, size_t buf_len, size_t off) const noexcept {
    if (off + bytes_.size() > buf_len) return false;
    for (size_t i = 0; i < bytes_.size(); ++i) {
        if (mask_[i] && buf[off + i] != bytes_[i]) return false;
    }
    return true;
}

std::string Pattern::text() const {
    std::string s;
    s.reserve(bytes_.size() * 3);
    for (size_t i = 0; i < bytes_.size(); ++i) {
        if (i) s += ' ';
        if (!mask_[i]) s += "??";
        else s += fmt("{:02X}", bytes_[i]);
    }
    return s;
}

// ---------------------------------------------------------------------------
std::vector<ScanHit> scan_section(const PeImage& img, const void* base,
                                  std::string_view section, const Pattern& pat,
                                  size_t max_hits) {
    std::vector<ScanHit> hits;
    if (pat.empty() || !base) return hits;

    auto const* sec = img.section(section);
    if (!sec) return hits;

    const uint8_t* mod = static_cast<const uint8_t*>(base);
    const size_t   len = sec->span();
    const size_t   n   = pat.size();
    if (len == 0 || len < n) return hits;

    // .text 有 148 MB, 逐字节 SEH 探测是不可接受的。
    // MemoryWalker 保证"窗口内任意地址可访问", 于是窗口内可以放心直接 memcmp。
    MemoryWalker walker(mod + sec->vaddr, len);

    bool done = false;
    do {
        const uint8_t* p   = walker.begin();
        const uint8_t* end = walker.window_end();
        if (end > p + n) {
            const uint8_t* last = end - n;   // 最后一个可能完整匹配的起点
            while (p <= last) {
                if (pat.matches(p, n, 0)) {
                    ScanHit h;
                    h.address = reinterpret_cast<uint64_t>(p);
                    h.rva = static_cast<uint32_t>(h.address - reinterpret_cast<uint64_t>(mod));
                    hits.push_back(h);
                    if (hits.size() >= max_hits) { done = true; break; }
                }
                ++p;
            }
        }
    } while (!done && walker.next_window());

    return hits;
}

std::vector<ScanHit> scan_module(const PeImage& img, const void* base,
                                 const Pattern& pat, const ScanOptions& opt) {
    std::vector<ScanHit> hits;
    if (!img.valid() || !base || pat.empty()) return hits;

    auto collect = [&](std::string_view sec_name, size_t budget) {
        std::vector<ScanHit> h = scan_section(img, base, sec_name, pat, opt.max_hits - hits.size());
        if (budget) h.resize(std::min(h.size(), budget));
        hits.insert(hits.end(), h.begin(), h.end());
    };

    if (!opt.section.empty()) {
        collect(opt.section, 0);
        return hits;
    }

    for (auto& s : img.sections()) {
        if (!s.executable()) continue;
        collect(s.name, 0);
        if (hits.size() >= opt.max_hits) break;
    }
    return hits;
}

std::optional<ScanHit> scan_module_first(const PeImage& img, const void* base,
                                         const Pattern& pat, const ScanOptions& opt) {
    ScanOptions o = opt;
    o.max_hits = 1;
    auto v = scan_module(img, base, pat, o);
    if (v.empty()) return std::nullopt;
    return v.front();
}

// ---------------------------------------------------------------------------
std::optional<uint64_t> rip_target(uint64_t insn_addr, uint32_t insn_len, uint32_t disp_off) {
    if (disp_off + 4 > insn_len) return std::nullopt;
    int32_t disp = 0;
    if (!safe_read(&disp, reinterpret_cast<const void*>(insn_addr + disp_off), 4)) return std::nullopt;
    return static_cast<uint64_t>(static_cast<int64_t>(insn_addr) +
                                 static_cast<int64_t>(insn_len) + disp);
}

std::optional<uint64_t> rip_target_from_hit(const ScanHit& hit, uint32_t insn_len,
                                            uint32_t disp_off_from_hit) {
    return rip_target(hit.address, insn_len, disp_off_from_hit);
}

} // namespace mcd2
