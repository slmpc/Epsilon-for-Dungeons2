#include "payload/feature/GameContext.h"

namespace epsilon::feature {

using game::dungeons2::CurrencyResolver;
using game::dungeons2::MovementResolver;

namespace {

// 函数内静态, 不用全局对象: DllMain 线程里全局对象的构造析构顺序不可控。
GameContext      gContext{};
MovementResolver gMovement{};
CurrencyResolver gCurrency{};

} // namespace

GameContext& game() { return gContext; }

void bindEngine(game::ue::Engine* engine) {
    gContext.engine = engine;
    gContext.movement = &gMovement;
    // 换引擎实例即换了一批目标对象, 旧解析结果必须作废。
    gMovement.invalidate();
    gCurrency.invalidate();
}

void tickFrame() {
    ++gContext.frame;
    // 解析器自带节流, 每帧调用即可。
    if (gContext.engine) {
        gMovement.resolve(*gContext.engine, false);
        gCurrency.resolve(*gContext.engine, false);
    }
}

MovementResolver& movement() { return gMovement; }

CurrencyResolver& currency() { return gCurrency; }

} // namespace epsilon::feature
