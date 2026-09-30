// ============================================================================
//  text.h — UTF-8 控制台 / ANSI 着色 / 格式化辅助
//  注入器与注入体共用。两边都是"往控制台吐文本", 所以格式约定必须一致。
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <format>

namespace epsilon {

// ------------------------------------------------------------------ ANSI 调色
// 仅在确认终端支持 VT 序列后才会输出; 否则退化为无色文本。
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

// 初始化当前进程的标准输出/错误为 UTF-8 + VT 序列。幂等。
// 返回: 是否成功启用 VT(决定是否上色)。
bool consoleInit(bool enableVt = true) noexcept;

// 当前进程是否具备可用控制台(注入体在游戏进程里通常没有)。
bool consoleAttached() noexcept;

// 着色开关(全局), 注入器在 --no-color 时关掉。
void setColorEnabled(bool on) noexcept;
bool colorEnabled() noexcept;

// ------------------------------------------------------------------ 输出原语
// 直接字节写入句柄, 不走 CRT 缓冲, 避免管道/重定向下丢内容。
void rawWrite(void* handle, std::string_view s) noexcept;

// 写 stdout / stderr。
void out(std::string_view s) noexcept;
void err(std::string_view s) noexcept;

// ---------------------------------------------------------------- 输出镜像到文件
// 把 out()/err() 的内容同时写一份到文件。
//
// 为什么需要: 注入器常常要提权运行(游戏进程完整性级别更高), 而提权启动时的
// 输出重定向非常不可靠 —— 各种 shell 包装(cmd /c、PowerShell -File)在 UAC
// 之后都会因为引号与重定向语义而静默失效。让程序自己写文件就没有这些边界。
//
// logOpen: utf8 路径; 已存在会被覆盖。返回是否成功。
bool logOpen(std::string_view utf8Path) noexcept;
void logClose() noexcept;
[[nodiscard]] bool logIsOpen() noexcept;

// 上色输出(自动判断 VT 支持)。
void outColored(std::string_view color, std::string_view s) noexcept;

// 行输出 + 便捷包装。
void outLine(std::string_view s = {}) noexcept;
void errLine(std::string_view s = {}) noexcept;

// ------------------------------------------------------------------ 语义化日志
// 统一前缀, 方便在滚动输出里肉眼过滤。
enum class Level { trace, info, warn, error, good };

void log(Level lv, std::string_view msg) noexcept;

// ============================================================================
//  format 辅助: 全部返回 std::string
// ============================================================================
template <typename... Args>
[[nodiscard]] std::string fmt(std::format_string<Args...> f, Args&&... args) {
    return std::format(f, std::forward<Args>(args)...);
}

// ------------------------------------------------------------- 数值/地址美化
// 0x00007FF637FE0000 这种定宽, 便于列对齐。
[[nodiscard]] std::string hex(uint64_t v, int width = 0);
[[nodiscard]] std::string hex(void const* p);
// 1234567 -> "1,234,567"
[[nodiscard]] std::string thousands(uint64_t v);
// 字节数 -> "12.34 MB"
[[nodiscard]] std::string humanBytes(uint64_t v);

// ------------------------------------------------------------- 字符串工具
// UTF-16 <-> UTF-8。项目里到处都在做这个转换(窗口标题、管道名、进程路径),
// 所以统一放这里, 不再各写一份。
[[nodiscard]] std::string  toUtf8(std::wstring_view w);
[[nodiscard]] std::wstring toUtf16(std::string_view s);

[[nodiscard]] std::string toLower(std::string_view s);
[[nodiscard]] bool iequals(std::string_view a, std::string_view b);
[[nodiscard]] bool istartsWith(std::string_view s, std::string_view prefix);
[[nodiscard]] bool icontains(std::string_view hay, std::string_view needle);
[[nodiscard]] std::string trim(std::string_view s);

// 空格分词(不处理引号, 命令语法故意保持简单可预测)。
[[nodiscard]] std::vector<std::string> splitWs(std::string_view s);

// 去掉 C++ 名字里的 UE 前缀: "APlayerCharacter" -> "PlayerCharacter"
[[nodiscard]] std::string stripUePrefix(std::string_view name);

// 转义不可打印字节, 用于把游戏里的字符串安全打到控制台。
[[nodiscard]] std::string sanitize(std::string_view s, size_t maxLen = 128);

// ------------------------------------------------------------- 十六进制转储
// 经典 xxd 布局: 偏移  hex...  |ascii|
[[nodiscard]] std::string hexdump(const uint8_t* data, size_t len,
                                 uint64_t baseVa = 0, size_t maxBytes = 0x200);

} // namespace epsilon
