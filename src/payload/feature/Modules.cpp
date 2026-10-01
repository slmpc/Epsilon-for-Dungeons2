// ============================================================================
//  Modules.cpp
// ============================================================================
#include "payload/feature/Modules.h"

#include "payload/Payload.h"
#include "payload/feature/module/ModuleManager.h"
#include "payload/feature/module/impl/Jump.h"
#include "payload/feature/module/impl/Speed.h"

namespace epsilon::feature {

using epsilon::payload::logInfo;

void initModules() {
    auto& mgr = ModuleManager::instance();
    if (!mgr.empty()) return;      // 已注册过

    // ---- player ----
    // 顺序即 UI 里的显示顺序。
    mgr.add<JumpModule>();
    mgr.add<SpeedModule>();

    logInfo(fmt("[Feature] 已注册 {} 个功能模块", mgr.moduleCount()));
}

} // namespace epsilon::feature
