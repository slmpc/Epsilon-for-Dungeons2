// ============================================================================
//  Modules.h — 功能模块注册入口
//
//  ModuleManager 本身不认识任何具体模块(见其文件头)。所有实际模块在
//  module/impl/ 下实现, 并在这里汇总注册 —— 加新模块只需要在 initModules
//  里加一行(与 Open-Epsilon 用 modules/impl/ 放实现、ModuleManager 里统一
//  登记的布局一致)。
// ============================================================================
#pragma once

namespace epsilon::feature {

// 把全部内置功能模块注册进 ModuleManager。幂等: 重复调用不会重复注册
// (ModuleManager::registerModule 会拒绝同名模块)。
void initModules();

} // namespace epsilon::feature
