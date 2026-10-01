// ============================================================================
//  Protocol.cpp
// ============================================================================
#include "common/Protocol.h"
#include "common/Text.h"

namespace epsilon::proto {

// 目前协议层只有常量与 POD, 逻辑都在两端的通道实现里。
// 这里放一个自检, 便于注入器启动时确认两端编译期假设一致。
static_assert(sizeof(Hello) % 8 == 0, "Hello 需要 8 字节对齐以便直接按 POD 传输");
static_assert(versionValue == 1, "协议版本变更时记得同步两端语义");

} // namespace epsilon::proto
