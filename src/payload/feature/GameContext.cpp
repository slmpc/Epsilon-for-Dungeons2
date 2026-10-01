// ============================================================================
//  GameContext.cpp
// ============================================================================
#include "payload/feature/GameContext.h"

namespace epsilon::feature {
namespace {

// 函数内静态, 不用全局对象: 注入体的加载/卸载时机不可控, DllMain 线程里
// 全局对象的构造析构顺序很容易出问题(与 ModuleManager 的取舍一致)。
GameContext  gContext{};
PlayerMovement gMovement{};

} // namespace

GameContext& game() { return gContext; }

void bindEngine(ue::Engine* engine) {
    gContext.engine = engine;
    gContext.movement = &gMovement;
    // 换引擎实例意味着目标换了一批对象, 旧解析结果必须作废。
    gMovement.invalidate();
}

void tickFrame() {
    ++gContext.frame;
    // 解析器自带节流, 这里每帧调用即可 —— 需要重扫时它才会真扫。
    if (gContext.engine) {
        gMovement.resolve(*gContext.engine, false);
    }
}

PlayerMovement& movement() { return gMovement; }

} // namespace epsilon::feature
