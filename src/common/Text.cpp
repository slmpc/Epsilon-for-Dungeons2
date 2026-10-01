// Text.cpp — 控制台/日志输出实现; 所有写入直接走句柄, 不经 CRT 缓冲。
#include "common/Text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace epsilon {
namespace {

HANDLE gStdout = nullptr;
HANDLE gStderr = nullptr;
HANDLE gLog    = INVALID_HANDLE_VALUE;
bool   gVt     = false;
bool   gColor  = true;
bool   gInited = false;

// 游戏进程是 GUI 子系统, 标准句柄可能为 0; 此时 rawWrite 静默丢弃而不是崩。
void ensureInit() noexcept {
    if (gInited) return;
    gInited = true;
    gStdout = ::GetStdHandle(STD_OUTPUT_HANDLE);
    gStderr = ::GetStdHandle(STD_ERROR_HANDLE);
    if (gStdout == INVALID_HANDLE_VALUE) gStdout = nullptr;
    if (gStderr == INVALID_HANDLE_VALUE) gStderr = nullptr;
}

const char* colorFor(Level lv) noexcept {
    switch (lv) {
        case Level::trace: return ansi::gray.data();
        case Level::info:  return ansi::cyan.data();
        case Level::warn:  return ansi::yellow.data();
        case Level::error: return ansi::red.data();
        case Level::good:  return ansi::green.data();
    }
    return "";
}

const char* tagFor(Level lv) noexcept {
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

bool consoleInit(bool enableVt) noexcept {
    ensureInit();

    // CRT 侧也切 UTF-8, 否则中文会被按 ANSI 码页截断。
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);

    gVt = false;
    if (enableVt && gStdout) {
        DWORD mode = 0;
        if (::GetConsoleMode(gStdout, &mode)) {
            if (::SetConsoleMode(gStdout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                              ENABLE_PROCESSED_OUTPUT)) {
                gVt = true;
            }
        }
    }
    gColor = gColor && gVt;
    return gVt;
}

bool consoleAttached() noexcept {
    ensureInit();
    return gStdout != nullptr && ::GetConsoleWindow() != nullptr;
}

void setColorEnabled(bool on) noexcept { gColor = on; }
bool colorEnabled() noexcept { return gColor && gVt; }

void rawWrite(void* handle, std::string_view s) noexcept {
    if (!handle || s.empty()) return;
    auto h = static_cast<HANDLE>(handle);
    const char* p = s.data();
    size_t left = s.size();
    while (left > 0) {
        DWORD written = 0;
        // 管道/重定向下 WriteFile 可能部分写入, 必须循环。
        if (!::WriteFile(h, p, static_cast<DWORD>(left), &written, nullptr)) {
            if (::GetLastError() == ERROR_INVALID_HANDLE) return;
            return;
        }
        if (written == 0) return;
        p += written;
        left -= written;
    }
}

void out(std::string_view s) noexcept {
    ensureInit();
    rawWrite(gStdout, s);
    if (gLog != INVALID_HANDLE_VALUE) rawWrite(gLog, s);
}

void err(std::string_view s) noexcept {
    ensureInit();
    rawWrite(gStderr ? gStderr : gStdout, s);
    if (gLog != INVALID_HANDLE_VALUE) rawWrite(gLog, s);
}

bool logOpen(std::string_view utf8Path) noexcept {
    logClose();
    if (utf8Path.empty()) return false;
    const std::wstring w = toUtf16(utf8Path);
    if (w.empty()) return false;
    gLog = ::CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return gLog != INVALID_HANDLE_VALUE;
}

void logClose() noexcept {
    if (gLog != INVALID_HANDLE_VALUE) {
        ::CloseHandle(gLog);
        gLog = INVALID_HANDLE_VALUE;
    }
}

bool logIsOpen() noexcept { return gLog != INVALID_HANDLE_VALUE; }

void outColored(std::string_view color, std::string_view s) noexcept {
    if (!colorEnabled() || color.empty()) { out(s); return; }
    out(color);
    out(s);
    out(ansi::reset);
}

void outLine(std::string_view s) noexcept { out(s); out("\n"); }
void errLine(std::string_view s) noexcept { err(s); err("\n"); }

void log(Level lv, std::string_view msg) noexcept {
    std::string line;
    line.reserve(msg.size() + 16);
    if (colorEnabled()) {
        line += colorFor(lv);
        line += tagFor(lv);
        line += msg;
        line += ansi::reset;
    } else {
        line += tagFor(lv);
        line += msg;
    }
    line += '\n';
    if (lv == Level::error) err(line); else out(line);
}

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

std::string humanBytes(uint64_t v) {
    constexpr const char* unit[] = {"B", "KB", "MB", "GB", "TB"};
    double d = static_cast<double>(v);
    int i = 0;
    while (d >= 1024.0 && i < 4) { d /= 1024.0; ++i; }
    return i == 0 ? std::format("{} B", v) : std::format("{:.2f} {}", d, unit[i]);
}

std::string toUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring toUtf16(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string toLower(std::string_view s) {
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

bool istartsWith(std::string_view s, std::string_view prefix) {
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

std::vector<std::string> splitWs(std::string_view s) {
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

std::string stripUePrefix(std::string_view name) {
    if (name.size() >= 2) {
        const char c0 = name[0];
        const char c1 = name[1];
        const bool prefixLike = (c0 == 'A' || c0 == 'U' || c0 == 'F' || c0 == 'E' ||
                                  c0 == 'I' || c0 == 'T');
        const bool restOk = (c1 >= 'A' && c1 <= 'Z');
        if (prefixLike && restOk) return std::string(name.substr(1));
    }
    return std::string(name);
}

std::string sanitize(std::string_view s, size_t maxLen) {
    std::string r;
    r.reserve(std::min(s.size(), maxLen));
    for (unsigned char c : s) {
        if (r.size() >= maxLen) { r += "..."; break; }
        if (c >= 0x20 && c < 0x7F) r += static_cast<char>(c);
        else if (c == '\t') r += ' ';
        else r += std::format("\\x{:02X}", c);
    }
    return r;
}

std::string hexdump(const uint8_t* data, size_t len, uint64_t baseVa, size_t maxBytes) {
    if (!data || len == 0) return "(空)\n";
    const size_t n = (maxBytes && len > maxBytes) ? maxBytes : len;
    std::string out;
    out.reserve((n / 16 + 1) * 80 + 64);

    for (size_t off = 0; off < n; off += 16) {
        out += std::format("{:012X}  ", baseVa + off);
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

} // namespace epsilon
