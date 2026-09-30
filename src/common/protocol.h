// ============================================================================
//  protocol.h — 注入器 <-> 注入体 的命名管道线协议
//
//  设计要点:
//    * 一条消息 = 固定 8 字节头 + 变长负载。PIPE_TYPE_MESSAGE 下每次 WriteFile
//      就是一个完整消息, 但仍带长度以便接收端校验和预分配。
//    * 双向: 注入体回报数据, 注入器下发命令行。
//    * 纯 POD 头, 两端各编译一次也能对齐(显式指定宽度, 无 padding 依赖)。
// ============================================================================
#pragma once

#include <cstdint>
#include <string_view>

namespace mcd2::proto {

// 协议版本。两端不一致时注入器直接拒绝通信, 免得用错位的结构解析垃圾。
inline constexpr uint32_t kVersion = 1;

inline constexpr uint32_t kMagic = 0x3244434Du;  // 'M''C''D''2' 小端摆法

// 单条负载上限, 防对端撒谎导致巨额分配。
inline constexpr uint32_t kMaxPayload = 1u << 20;   // 1 MiB

enum class Kind : uint16_t {
    hello      = 1,   // payload -> injector: 握手(版本/模块基址/pid)
    ready      = 2,   // payload -> injector: 命令服务已就绪
    status     = 3,   // payload -> injector: 一行人类可读状态
    data       = 4,   // payload -> injector: 数据块(直接打到控制台)
    error      = 5,   // payload -> injector: 出错
    bye        = 6,   // 双向: 我要走了
    frame      = 7,   // payload -> injector: 帧心跳(低频, 供 FPS 显示)
    command    = 8,   // injector -> payload: 命令行
    ping       = 9,   // injector -> payload: 探活
};

struct Header {
    uint32_t magic   = kMagic;
    uint16_t version = static_cast<uint16_t>(kVersion);
    uint16_t kind    = 0;
    uint32_t length  = 0;   // 负载字节数(不含本头)
};
static_assert(sizeof(Header) == 12, "协议头必须是紧凑 12 字节");

// 握手负载: 定长 POD, 两端共用同一份定义。
struct Hello {
    uint32_t pid         = 0;
    uint64_t module_base = 0;
    uint64_t module_size = 0;
    uint64_t image_base  = 0;
    uint32_t protocol    = kVersion;
    char     tag[32]     = {};   // 注入体标识串, 便于人眼确认
};

} // namespace mcd2::proto
