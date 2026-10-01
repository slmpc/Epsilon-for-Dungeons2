// Protocol.h — 注入器 <-> 注入体的管道线协议: 12 字节定长头 + 变长负载, 头是纯 POD。
#pragma once

#include <cstdint>
#include <string_view>

namespace epsilon::proto {

// 版本不一致时注入器拒绝通信。常量名不能叫 version/magic —— 与 Header 成员同名会
// 自引用, 静默生成未初始化的头(表现为"两端都连上了但收不到任何消息")。
inline constexpr uint32_t versionValue = 1;

inline constexpr uint32_t magicValue = 0x3244434Du;

// 单条负载上限, 防对端撒谎导致巨额分配。
inline constexpr uint32_t maxPayload = 1u << 20;

// 方向: 1..7 由注入体发往注入器, command / ping 反向。
enum class Kind : uint16_t {
    hello      = 1,
    ready      = 2,
    status     = 3,
    data       = 4,
    error      = 5,
    bye        = 6,
    frame      = 7,
    command    = 8,
    ping       = 9,
};

struct Header {
    uint32_t magic   = magicValue;
    uint16_t version = static_cast<uint16_t>(versionValue);
    uint16_t kind    = 0;
    uint32_t length  = 0;
};
static_assert(sizeof(Header) == 12, "协议头必须是紧凑 12 字节");

// 握手负载: 定长 POD, 两端共用同一份定义。
struct Hello {
    uint32_t pid        = 0;
    uint64_t moduleBase = 0;
    uint64_t moduleSize = 0;
    uint64_t imageBase  = 0;
    uint32_t protocol   = versionValue;
    char     tag[32]     = {};
};

} // namespace epsilon::proto
