// ============================================================================
//  pattern_scan.h — 特征码扫描 + RIP 相对寻址解析
//
//  存在的理由(来自分析结论 §8.3): 游戏每次更新都会让所有硬编码偏移失效, 没有
//  PDB 就无法做符号级 diff。唯一能让项目活下去的方式是把"扫描 + 运行时反射"
//  做成自动流水线。所以: 已知 RVA 只当"快速路径", 永远保留特征码兜底。
// ============================================================================
#pragma once

#include "common/pe_image.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mcd2 {

// IDA 风格特征码: "48 8B 05 ? ? ? ?" 或紧凑 "488B05????????"
class Pattern {
public:
    static std::optional<Pattern> parse(std::string_view text);

    // 是否在 buf 的 off 处匹配。
    [[nodiscard]] bool matches(const uint8_t* buf, size_t buf_len, size_t off) const noexcept;

    [[nodiscard]] size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool   empty() const noexcept { return bytes_.empty(); }
    [[nodiscard]] std::string text() const;

private:
    std::vector<uint8_t> bytes_;
    std::vector<uint8_t> mask_;   // 1 = 参与比较, 0 = 通配
};

struct ScanHit {
    uint64_t address = 0;   // 命中处的运行时绝对地址
    uint32_t rva     = 0;   // 相对模块基址
};

struct ScanOptions {
    std::string section = ".text";  // 空 = 扫所有可执行节区
    size_t      max_hits = 64;
    size_t      max_scan_bytes = 0; // 0 = 不限(节区有多大扫多大)
};

// 在模块内存映像里扫描。base 必须是模块运行时基址(注入体内即 HMODULE)。
std::vector<ScanHit> scan_module(const PeImage& img, const void* base,
                                 const Pattern& pat, const ScanOptions& opt = {});

// 只取第一个命中, 便于"找一个就够"的场景。
std::optional<ScanHit> scan_module_first(const PeImage& img, const void* base,
                                         const Pattern& pat, const ScanOptions& opt = {});

// 在一个已解析好的节区范围内扫描。
std::vector<ScanHit> scan_section(const PeImage& img, const void* base,
                                  std::string_view section, const Pattern& pat,
                                  size_t max_hits = 64);

// --------------------------------------------------------------------------
//  RIP 相对寻址: 形如 48 8B 05 <disp32> / 48 8D 0D <disp32> / 89 05 <disp32>
//  返回目标绝对地址。
//    insn_addr : 指令起始绝对地址
//    insn_len  : 指令总长度(含 disp32)
//    disp_off  : disp32 字段相对指令起始的偏移(通常 = insn_len - 4)
// --------------------------------------------------------------------------
std::optional<uint64_t> rip_target(uint64_t insn_addr, uint32_t insn_len, uint32_t disp_off);

// 便捷: 从命中的便利位置直接算。offset_to_disp 为 disp32 相对命中地址的偏移。
std::optional<uint64_t> rip_target_from_hit(const ScanHit& hit, uint32_t insn_len,
                                            uint32_t disp_off_from_hit);

// --------------------------------------------------------------------------
//  UE5 常用特征码(集中在这里, 游戏更新后只改这一处)
// --------------------------------------------------------------------------
namespace sigs {
    // GObjects: "shr r8, 0x10" 之后的 "mov rax, [rip+disp]" —— FUObjectArray 访问器
    inline constexpr std::string_view kGObjectsAnchor = "49 C1 E8 10";
    inline constexpr std::string_view kMovRaxRip      = "48 8B 05";

    // FNamePool 访问器常见形态: mov rax, [rip+disp] / mov ..., gs:[...] 组合
    inline constexpr std::string_view kMovRaxRipCandidates =
        "48 8B 05 ? ? ? ? 48 8B 0C C8";
} // namespace sigs

} // namespace mcd2
