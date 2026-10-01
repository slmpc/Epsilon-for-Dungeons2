// Protocol.cpp — 协议层目前只有常量与 POD, 这里放编译期自检, 便于注入器启动时确认两端假设一致。
#include "common/Protocol.h"
#include "common/Text.h"

namespace epsilon::proto {

static_assert(sizeof(Hello) % 8 == 0, "Hello 需要 8 字节对齐以便直接按 POD 传输");
static_assert(versionValue == 1, "协议版本变更时记得同步两端语义");

} // namespace epsilon::proto
