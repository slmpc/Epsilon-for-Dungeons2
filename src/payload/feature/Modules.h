#pragma once

namespace epsilon::feature {

// 把全部内置功能模块注册进 ModuleManager。幂等: 重复调用不会重复注册
// (ModuleManager::registerModule 会拒绝同名模块)。
void initModules();

} // namespace epsilon::feature
