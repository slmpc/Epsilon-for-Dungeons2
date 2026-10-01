// PeImage.h — 轻量 PE 解析(容器无关): 同一份代码可喂磁盘文件或进程内存。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epsilon {

struct PeSection {
    std::string name;
    uint32_t    vsize = 0;
    uint32_t    vaddr = 0;
    uint32_t    rawSize = 0;
    uint32_t    rawPtr = 0;
    uint32_t    characteristics = 0;

    [[nodiscard]] bool executable() const { return (characteristics & 0x20000000u) != 0; }
    [[nodiscard]] bool writable()   const { return (characteristics & 0x80000000u) != 0; }
    [[nodiscard]] bool readable()   const { return (characteristics & 0x40000000u) != 0; }
    [[nodiscard]] uint64_t span() const { return vsize > rawSize ? vsize : rawSize; }
};

class PeImage {
public:
    // fromFile 读磁盘映像, fromMemory 读已映射映像; 解析失败返回 nullopt。
    [[nodiscard]] static std::optional<PeImage> fromFile(std::wstring_view path);
    [[nodiscard]] static std::optional<PeImage> fromMemory(const void* base);

    [[nodiscard]] bool     valid()     const { return valid_; }
    [[nodiscard]] uint16_t machine()   const { return machine_; }
    [[nodiscard]] uint64_t imageBase()const { return imageBase_; }
    [[nodiscard]] uint32_t sizeImage()const { return sizeImage_; }
    [[nodiscard]] uint32_t timestamp() const { return timestamp_; }
    [[nodiscard]] size_t   sizeOfHeaders() const { return sizeOfHeaders_; }
    [[nodiscard]] bool     isDll()    const { return (characteristics_ & 0x2000) != 0; }

    [[nodiscard]] std::vector<PeSection> const& sections() const { return sections_; }
    [[nodiscard]] PeSection const* section(std::string_view name) const;

    // base 必须是模块的运行时基址; sectionAt / section 找不到时返回 nullptr。
    [[nodiscard]] void const* rvaPtr(const void* base, uint32_t rva) const;
    [[nodiscard]] PeSection const* sectionAt(uint32_t rva) const;
    [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>> sectionRange(std::string_view name) const;

    [[nodiscard]] std::string const& path() const { return path_; }
    [[nodiscard]] std::string describe() const;

private:
    bool parse(const uint8_t* data, size_t size, const void* mappedBase, bool fromFile);

    bool                      valid_ = false;
    uint16_t                  machine_ = 0;
    uint64_t                  imageBase_ = 0;
    uint32_t                  sizeImage_ = 0;
    uint32_t                  timestamp_ = 0;
    size_t                    sizeOfHeaders_ = 0;
    uint16_t                  characteristics_ = 0;
    std::vector<PeSection>    sections_;
    std::vector<uint8_t>      fileData_;
    std::string               path_;
};

// 用 __try/__except 探测可读性 —— 目标进程里乱指指针不崩的基础。
bool probeReadable(const void* addr, size_t size) noexcept;

// 读目标进程内存一律走这两个入口(不要直接解引用): 失败返回 false 且不改动 dst,
// safeWrite 先直写, 失败才放宽页保护, 写完恢复。
bool safeRead(void* dst, const void* src, size_t size) noexcept;
bool safeWrite(void* dst, const void* src, size_t size) noexcept;

// 模板包装: 读一个 POD,T 必须是平凡可拷贝类型。
template <typename T>
[[nodiscard]] std::optional<T> safeReadPod(uint64_t addr) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "只支持 POD");
    T v{};
    if (!addr) return std::nullopt;
    if (!safeRead(&v, reinterpret_cast<const void*>(addr), sizeof(T))) return std::nullopt;
    return v;
}

} // namespace epsilon
