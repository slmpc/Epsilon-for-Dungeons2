// ============================================================================
//  pe_image.cpp
// ============================================================================
#include "common/pe_image.h"
#include "common/text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <type_traits>

namespace mcd2 {

// ---------------------------------------------------------------------------
//  安全内存探测
//  __try/__except 不能与需要析构的对象同处一个函数(MSVC C2712), 所以把危险
//  部分隔离在最小函数里, 只做一次 memcpy。
// ---------------------------------------------------------------------------
namespace {

bool probe_impl(const void* addr, size_t size) noexcept {
    __try {
        volatile uint8_t sink = 0;
        const auto* p = static_cast<const volatile uint8_t*>(addr);
        // 头尾各摸一下: 空指针/未映射/跨页边界都会在这里炸, 由 __except 收走。
        sink = p[0];
        sink = p[size - 1];
        (void)sink;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_impl(void* dst, const void* src, size_t size) noexcept {
    __try {
        std::memcpy(dst, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

bool probe_readable(const void* addr, size_t size) noexcept {
    if (!addr || size == 0) return false;
    if (!probe_impl(addr, size)) return false;
    return true;
}

bool safe_read(void* dst, const void* src, size_t size) noexcept {
    if (!dst || !src || size == 0) return false;
    return read_impl(dst, src, size);
}


// ---------------------------------------------------------------------------
namespace {

// 跨页读取时首字节可读不代表整段可读。用 VirtualQuery 做粗筛(便宜且不触发异常),
// 再用 SEH 兜底(贵但绝对准)。
bool range_ok(const void* addr, size_t size) noexcept {
    auto a = reinterpret_cast<uint64_t>(addr);
    uint64_t left = size;
    while (left > 0) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        const uint64_t region_end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (a >= region_end) return false;
        const uint64_t avail = region_end - a;
        if (avail >= left) return true;
        left -= avail;
        a = region_end;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
std::optional<PeImage> PeImage::from_file(std::wstring_view path) {
    HANDLE f = ::CreateFileW(std::wstring(path).c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::nullopt;

    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(f, &li) || li.QuadPart <= 0 || li.QuadPart > (1ll << 32)) {
        ::CloseHandle(f);
        return std::nullopt;
    }

    PeImage img;
    img.file_data_.resize(static_cast<size_t>(li.QuadPart));
    size_t done = 0;
    while (done < img.file_data_.size()) {
        DWORD got = 0;
        if (!::ReadFile(f, img.file_data_.data() + done,
                        static_cast<DWORD>(img.file_data_.size() - done), &got, nullptr) || got == 0)
            break;
        done += got;
    }
    ::CloseHandle(f);
    img.file_data_.resize(done);

    const int n = ::WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n > 0) {
        img.path_.resize(static_cast<size_t>(n));
        ::WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()),
                              img.path_.data(), n, nullptr, nullptr);
    }

    if (!img.parse(img.file_data_.data(), img.file_data_.size(), nullptr, true)) return std::nullopt;
    return img;
}

std::optional<PeImage> PeImage::from_memory(const void* base) {
    if (!base) return std::nullopt;
    PeImage img;
    // 内存映像: 直接用映射地址作为数据源, 不拷贝。
    if (!img.parse(static_cast<const uint8_t*>(base), 0x1000, base, false)) return std::nullopt;
    return img;
}

bool PeImage::parse(const uint8_t* data, size_t size, const void* mapped_base, bool from_file) {
    if (!data || size < 0x40) return false;

    // 磁盘文件直接验 MZ; 内存映像 base 处就是 MZ。
    if (data[0] != 'M' || data[1] != 'Z') return false;

    uint32_t e_lfanew = 0;
    std::memcpy(&e_lfanew, data + 0x3C, 4);
    if (e_lfanew == 0 || e_lfanew + 0x18 > size) return false;
    if (std::memcmp(data + e_lfanew, "PE\0\0", 4) != 0) return false;

    const uint8_t* coff = data + e_lfanew + 4;
    uint16_t nsections = 0, opt_size = 0;
    std::memcpy(&machine_, coff + 0, 2);
    std::memcpy(&timestamp_, coff + 4, 4);
    std::memcpy(&nsections, coff + 2, 2);
    std::memcpy(&opt_size, coff + 16, 2);
    std::memcpy(&characteristics_, coff + 18, 2);

    const uint8_t* opt = coff + 20;
    uint16_t magic = 0;
    std::memcpy(&magic, opt + 0, 2);
    const bool pe32plus = (magic == 0x20B);
    if (!pe32plus && magic != 0x10B) return false;

    if (pe32plus) {
        std::memcpy(&image_base_, opt + 24, 8);
        std::memcpy(&size_image_, opt + 56, 4);
        std::memcpy(&size_of_headers_, opt + 60, 4);
    } else {
        uint32_t ib = 0;
        std::memcpy(&ib, opt + 28, 4);
        image_base_ = ib;
        std::memcpy(&size_image_, opt + 56, 4);
        uint32_t soh = 0;
        std::memcpy(&soh, opt + 60, 4);
        size_of_headers_ = soh;
    }

    const uint8_t* sec = opt + opt_size;
    if (from_file) {
        // 注意: 指针相减得到 ptrdiff_t, 不能 reinterpret_cast 到 size_t。
        if (static_cast<size_t>(sec - data) + static_cast<size_t>(nsections) * 40 > size)
            return false;
    } else {
        // 内存映像: 节表在映像头部之后, 用 safe_read 兜底(不假设 0x1000 够用)
        if (!probe_readable(sec, static_cast<size_t>(nsections) * 40)) return false;
    }

    sections_.clear();
    sections_.reserve(nsections);
    for (uint16_t i = 0; i < nsections; ++i) {
        const uint8_t* s = sec + static_cast<size_t>(i) * 40;
        PeSection ps;
        char nm[9]{};
        if (!safe_read(nm, s, 8)) return false;
        nm[8] = '\0';
        ps.name = nm;
        std::memcpy(&ps.vsize, s + 8, 4);
        std::memcpy(&ps.vaddr, s + 12, 4);
        std::memcpy(&ps.rawsize, s + 16, 4);
        std::memcpy(&ps.rawptr, s + 20, 4);
        std::memcpy(&ps.characteristics, s + 36, 4);
        sections_.push_back(std::move(ps));
    }

    (void)mapped_base;
    valid_ = true;
    return true;
}

PeSection const* PeImage::section(std::string_view name) const {
    for (auto& s : sections_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

PeSection const* PeImage::section_at(uint32_t rva) const {
    for (auto& s : sections_) {
        const uint64_t end = static_cast<uint64_t>(s.vaddr) + s.span();
        if (rva >= s.vaddr && rva < end) return &s;
    }
    return nullptr;
}

std::optional<std::pair<uint32_t, uint32_t>> PeImage::section_range(std::string_view name) const {
    if (auto const* s = section(name)) {
        return std::make_pair(s->vaddr, static_cast<uint32_t>(s->vaddr + s->span()));
    }
    return std::nullopt;
}

void const* PeImage::rva_ptr(const void* base, uint32_t rva) const {
    return static_cast<const uint8_t*>(base) + rva;
}

std::string PeImage::describe() const {
    if (!valid_) return "PE: 无效";
    return fmt("PE {} | machine {:#06x} | ImageBase {} | SizeOfImage {} | {} 个节区 | {}",
               is_dll() ? "DLL" : "EXE", machine_, hex(image_base_, 16),
               human_bytes(size_image_), sections_.size(),
               path_.empty() ? "(内存映像)" : path_);
}

void const* pe_rva_ptr(const PeImage& img, const void* base, uint32_t rva) {
    return img.rva_ptr(base, rva);
}

} // namespace mcd2
