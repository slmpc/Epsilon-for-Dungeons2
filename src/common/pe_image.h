// ============================================================================
//  pe_image.h — 轻量 PE 解析(容器无关)
//  两端共用: 注入器用它算静态 RVA, 注入体用它拿模块节区表做内存扫描。
//  刻意不依赖任何 Windows 头 —— 同一份代码可以拿磁盘文件或进程内存喂进来。
// ============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcd2 {

struct PeSection {
    std::string name;       // 最多 8 字符, 已去掉尾部 NUL
    uint32_t    vsize = 0;  // VirtualSize
    uint32_t    vaddr = 0;  // RVA
    uint32_t    rawsize = 0;
    uint32_t    rawptr = 0;
    uint32_t    characteristics = 0;

    [[nodiscard]] bool executable() const { return (characteristics & 0x20000000u) != 0; }
    [[nodiscard]] bool writable()   const { return (characteristics & 0x80000000u) != 0; }
    [[nodiscard]] bool readable()   const { return (characteristics & 0x40000000u) != 0; }
    [[nodiscard]] uint64_t span() const { return vsize > rawsize ? vsize : rawsize; }
};

class PeImage {
public:
    // 从磁盘加载(注入器用它读 Shipping.exe 的静态布局)。
    [[nodiscard]] static std::optional<PeImage> from_file(std::wstring_view path);
    // 从已映射的内存加载(注入体用它读自身所在模块)。
    [[nodiscard]] static std::optional<PeImage> from_memory(const void* base);

    [[nodiscard]] bool     valid()     const { return valid_; }
    [[nodiscard]] uint16_t machine()   const { return machine_; }
    [[nodiscard]] uint64_t image_base()const { return image_base_; }
    [[nodiscard]] uint32_t size_image()const { return size_image_; }
    [[nodiscard]] uint32_t timestamp() const { return timestamp_; }
    [[nodiscard]] size_t   size_of_headers() const { return size_of_headers_; }
    [[nodiscard]] bool     is_dll()    const { return (characteristics_ & 0x2000) != 0; }

    [[nodiscard]] std::vector<PeSection> const& sections() const { return sections_; }
    [[nodiscard]] PeSection const* section(std::string_view name) const;

    // RVA → 映射内存里的实际指针。base 为模块运行时基址。
    [[nodiscard]] void const* rva_ptr(const void* base, uint32_t rva) const;
    // 该 RVA 落在哪个节区(找不到返回 nullptr)。
    [[nodiscard]] PeSection const* section_at(uint32_t rva) const;
    // 某个节的 RVA 边界。
    [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>> section_range(std::string_view name) const;

    [[nodiscard]] std::string const& path() const { return path_; }
    [[nodiscard]] std::string describe() const;

private:
    bool parse(const uint8_t* data, size_t size, const void* mapped_base, bool from_file);

    bool                      valid_ = false;
    uint16_t                  machine_ = 0;
    uint64_t                  image_base_ = 0;
    uint32_t                  size_image_ = 0;
    uint32_t                  timestamp_ = 0;
    size_t                    size_of_headers_ = 0;
    uint16_t                  characteristics_ = 0;
    std::vector<PeSection>    sections_;
    std::vector<uint8_t>      file_data_;   // from_file 时持有
    std::string               path_;
};

// 用 __try/__except 安全地探测一段内存是否可读(任意地址, 不会崩)。
// 这是整套框架在目标进程里"乱指指针不崩"的基础。
bool probe_readable(const void* addr, size_t size) noexcept;

// 安全读: 先探测再拷贝。失败返回 false, 不动 out。
bool safe_read(void* dst, const void* src, size_t size) noexcept;

// 顺序遍历一段内存的辅助器。语义 = 逐字节探测:
//   walker.good_at(p) 为真 ⟹ p 处访问安全; 为假 ⟹ 调用方跳过该片区域。
// 实现上用"窗口探测 + 窗口内任意位置可访问"来避免每个字节一次 SEH。
// 特征码扫描是热路径(200 MB 映像), 这个优化不是可选项。
class MemoryWalker {
public:
    MemoryWalker(const void* begin, size_t len) noexcept;
    [[nodiscard]] const uint8_t* begin() const noexcept { return begin_; }
    [[nodiscard]] const uint8_t* end() const noexcept { return end_; }
    [[nodiscard]] const uint8_t* window_end() const noexcept { return win_end_; }
    [[nodiscard]] bool good_at(const uint8_t* p) const noexcept { return p < win_end_; }
    // 跳到下一个窗口, 返回是否还有内容。
    bool next_window() noexcept;
    [[nodiscard]] size_t windows() const noexcept { return windows_; }

private:
    void probe_window() noexcept;
    const uint8_t* begin_ = nullptr;
    const uint8_t* end_ = nullptr;
    const uint8_t* win_end_ = nullptr;
    size_t windows_ = 0;
};

// 模板包装: 读一个 POD,T 必须是平凡可拷贝类型。
template <typename T>
[[nodiscard]] std::optional<T> safe_read_pod(uint64_t addr) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "只支持 POD");
    T v{};
    if (!addr) return std::nullopt;
    if (!safe_read(&v, reinterpret_cast<const void*>(addr), sizeof(T))) return std::nullopt;
    return v;
}

} // namespace mcd2
