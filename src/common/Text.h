// Text.h — UTF-8 控制台 / ANSI 着色 / 格式化与字符串辅助, 注入器与注入体共用。
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <format>

namespace epsilon {

// VT 未被确认支持时不输出颜色。
namespace ansi {
    inline constexpr std::string_view reset   = "\x1b[0m";
    inline constexpr std::string_view bold    = "\x1b[1m";
    inline constexpr std::string_view dim     = "\x1b[2m";
    inline constexpr std::string_view red     = "\x1b[31m";
    inline constexpr std::string_view green   = "\x1b[32m";
    inline constexpr std::string_view yellow  = "\x1b[33m";
    inline constexpr std::string_view blue    = "\x1b[34m";
    inline constexpr std::string_view magenta = "\x1b[35m";
    inline constexpr std::string_view cyan    = "\x1b[36m";
    inline constexpr std::string_view gray    = "\x1b[90m";
} // namespace ansi

// 把 stdout/stderr 切到 UTF-8 + VT。幂等; 返回是否启用 VT(决定上色)。
bool consoleInit(bool enableVt = true) noexcept;

bool consoleAttached() noexcept;

void setColorEnabled(bool on) noexcept;
bool colorEnabled() noexcept;

// 直接按字节写句柄, 不走 CRT 缓冲 —— 管道/重定向下不会丢内容。
void rawWrite(void* handle, std::string_view s) noexcept;

void out(std::string_view s) noexcept;
void err(std::string_view s) noexcept;

// 把 out()/err() 的内容同时镜像一份到文件: 提权运行时 shell 重定向在 UAC 之后
// 常常静默失效, 让程序自己写文件就没有这些边界。
// logOpen 收 utf8 路径; 已存在会被覆盖, 失败返回 false。
bool logOpen(std::string_view utf8Path) noexcept;
void logClose() noexcept;
[[nodiscard]] bool logIsOpen() noexcept;

void outColored(std::string_view color, std::string_view s) noexcept;

void outLine(std::string_view s = {}) noexcept;
void errLine(std::string_view s = {}) noexcept;

enum class Level { trace, info, warn, error, good };

void log(Level lv, std::string_view msg) noexcept;

template <typename... Args>
[[nodiscard]] std::string fmt(std::format_string<Args...> f, Args&&... args) {
    return std::format(f, std::forward<Args>(args)...);
}

// hex: 定宽(不足补零), 便于列对齐。
[[nodiscard]] std::string hex(uint64_t v, int width = 0);
[[nodiscard]] std::string hex(void const* p);
[[nodiscard]] std::string thousands(uint64_t v);
[[nodiscard]] std::string humanBytes(uint64_t v);

[[nodiscard]] std::string  toUtf8(std::wstring_view w);
[[nodiscard]] std::wstring toUtf16(std::string_view s);

[[nodiscard]] std::string toLower(std::string_view s);
[[nodiscard]] bool iequals(std::string_view a, std::string_view b);
[[nodiscard]] bool istartsWith(std::string_view s, std::string_view prefix);
[[nodiscard]] bool icontains(std::string_view hay, std::string_view needle);
[[nodiscard]] std::string trim(std::string_view s);

// 只按空白分词, 不处理引号(命令语法刻意保持简单可预测)。
[[nodiscard]] std::vector<std::string> splitWs(std::string_view s);

// 去掉 UE 的类名前缀: "APlayerCharacter" -> "PlayerCharacter"
[[nodiscard]] std::string stripUePrefix(std::string_view name);

[[nodiscard]] std::string sanitize(std::string_view s, size_t maxLen = 128);

[[nodiscard]] std::string hexdump(const uint8_t* data, size_t len,
                                 uint64_t baseVa = 0, size_t maxBytes = 0x200);

} // namespace epsilon
