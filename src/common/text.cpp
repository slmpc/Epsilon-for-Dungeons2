// ============================================================================
//  text.cpp
// ============================================================================
#include "common/text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mcd2 {
namespace {

HANDLE g_stdout = nullptr;
HANDLE g_stderr = nullptr;
bool   g_vt     = false;
bool   g_color  = true;
bool   g_inited = false;

// 一次性初始化句柄。注意: 注入体所在的游戏进程是 GUI 子系统, 标准句柄可能为 0,
// 此时 raw_write 会静默丢弃 —— 这正是我们要的行为(不能因为没控制台就崩)。
void ensure_init() noexcept {
    if (g_inited) return;
    g_inited = true;
    g_stdout = ::GetStdHandle(STD_OUTPUT_HANDLE);
    g_stderr = ::GetStdHandle(STD_ERROR_HANDLE);
    if (g_stdout == INVALID_HANDLE_VALUE) g_stdout = nullptr;
    if (g_stderr == INVALID_HANDLE_VALUE) g_stderr = nullptr;
}

const char* color_for(Level lv) noexcept {
    switch (lv) {
        case Level::trace: return ansi::gray.data();
        case Level::info:  return ansi::cyan.data();
        case Level::warn:  return ansi::yellow.data();
        case Level::error: return ansi::red.data();
        case Level::good:  return ansi::green.data();
    }
    return "";
}

const char* tag_for(Level lv) noexcept {
    switch (lv) {
        case Level::trace: return "[..]";
        case Level::info:  return "[*] ";
        case Level::warn:  return "[!] ";
        case Level::error: return "[x] ";
        case Level::good:  return "[+] ";
    }
    return "[?] ";
}

} // namespace

// ---------------------------------------------------------------------------
bool console_init(bool enable_vt) noexcept {
    ensure_init();

    // CRT 侧也切到 UTF-8, 免得 std::format 出来的中文被按 ANSI 码页截断。
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);

    g_vt = false;
    if (enable_vt && g_stdout) {
        DWORD mode = 0;
        if (::GetConsoleMode(g_stdout, &mode)) {
            if (::SetConsoleMode(g_stdout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                              ENABLE_PROCESSED_OUTPUT)) {
                g_vt = true;
            }
        }
    }
    g_color = g_color && g_vt;
    return g_vt;
}

bool console_attached() noexcept {
    ensure_init();
    return g_stdout != nullptr && ::GetConsoleWindow() != nullptr;
}

void set_color_enabled(bool on) noexcept { g_color = on; }
bool color_enabled() noexcept { return g_color && g_vt; }

// ---------------------------------------------------------------------------
void raw_write(void* handle, std::string_view s) noexcept {
    if (!handle || s.empty()) return;
    auto h = static_cast<HANDLE>(handle);
    const char* p = s.data();
    size_t left = s.size();
    while (left > 0) {
        DWORD written = 0;
        // 管道/重定向下 WriteFile 可能部分写入, 必须循环。
        if (!::WriteFile(h, p, static_cast<DWORD>(left), &written, nullptr)) {
            // 控制台被关掉会 ERROR_INVALID_HANDLE —— 直接放弃, 不抛异常。
            if (::GetLastError() == ERROR_INVALID_HANDLE) return;
            return;
        }
        if (written == 0) return;
        p += written;
        left -= written;
    }
}

void out(std::string_view s) noexcept { ensure_init(); raw_write(g_stdout, s); }
void err(std::string_view s) noexcept { ensure_init(); raw_write(g_stderr ? g_stderr : g_stdout, s); }

void out_colored(std::string_view color, std::string_view s) noexcept {
    if (!color_enabled() || color.empty()) { out(s); return; }
    out(color);
    out(s);
    out(ansi::reset);
}

void out_line(std::string_view s) noexcept { out(s); out("\n"); }
void err_line(std::string_view s) noexcept { err(s); err("\n"); }

void log(Level lv, std::string_view msg) noexcept {
    std::string line;
    line.reserve(msg.size() + 16);
    if (color_enabled()) {
        line += color_for(lv);
        line += tag_for(lv);
        line += msg;
        line += ansi::reset;
    } else {
        line += tag_for(lv);
        line += msg;
    }
    line += '\n';
    if (lv == Level::error) err(line); else out(line);
}

// ---------------------------------------------------------------------------
std::string hex(uint64_t v, int width) {
    std::string s = std::format("0x{:X}", v);
    if (width > 0 && static_cast<int>(s.size()) < width) {
        s.insert(2, static_cast<size_t>(width) - s.size(), '0');
    }
    return s;
}

std::string hex(void const* p) { return hex(reinterpret_cast<uint64_t>(p)); }

std::string thousands(uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

std::string human_bytes(uint64_t v) {
    constexpr const char* unit[] = {"B", "KB", "MB", "GB", "TB"};
    double d = static_cast<double>(v);
    int i = 0;
    while (d >= 1024.0 && i < 4) { d /= 1024.0; ++i; }
    return i == 0 ? std::format("{} B", v) : std::format("{:.2f} {}", d, unit[i]);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
std::string to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring to_utf16(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string to_lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

bool istarts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool icontains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != hay.end();
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    auto sp = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (b < e && sp(s[b])) ++b;
    while (e > b && sp(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

std::vector<std::string> split_ws(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) break;
        size_t b = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        out.emplace_back(s.substr(b, i - b));
    }
    return out;
}

std::string strip_ue_prefix(std::string_view name) {
    if (name.size() >= 2) {
        const char c0 = name[0];
        const char c1 = name[1];
        const bool prefix_like = (c0 == 'A' || c0 == 'U' || c0 == 'F' || c0 == 'E' ||
                                  c0 == 'I' || c0 == 'T');
        const bool rest_ok = (c1 >= 'A' && c1 <= 'Z');
        if (prefix_like && rest_ok) return std::string(name.substr(1));
    }
    return std::string(name);
}

std::string sanitize(std::string_view s, size_t max_len) {
    std::string r;
    r.reserve(std::min(s.size(), max_len));
    for (unsigned char c : s) {
        if (r.size() >= max_len) { r += "..."; break; }
        if (c >= 0x20 && c < 0x7F) r += static_cast<char>(c);
        else if (c == '\t') r += ' ';
        else r += std::format("\\x{:02X}", c);
    }
    return r;
}

// ---------------------------------------------------------------------------
std::string hexdump(const uint8_t* data, size_t len, uint64_t base_va, size_t max_bytes) {
    if (!data || len == 0) return "(空)\n";
    const size_t n = (max_bytes && len > max_bytes) ? max_bytes : len;
    std::string out;
    out.reserve((n / 16 + 1) * 80 + 64);

    for (size_t off = 0; off < n; off += 16) {
        out += std::format("{:012X}  ", base_va + off);
        const size_t line = std::min<size_t>(16, n - off);
        for (size_t i = 0; i < 16; ++i) {
            if (i < line) out += std::format("{:02X} ", data[off + i]);
            else          out += "   ";
            if (i == 7) out += ' ';
        }
        out += " |";
        for (size_t i = 0; i < line; ++i) {
            const unsigned char c = data[off + i];
            out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
        }
        out += "|\n";
    }
    if (n < len) out += std::format("... 还有 {} 字节未显示\n", len - n);
    return out;
}

} // namespace mcd2
