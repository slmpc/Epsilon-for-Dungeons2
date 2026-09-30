// ============================================================================
//  engine.cpp — 引擎全局定位
//
//  定位策略: **只认实测 RVA**。每一项都必须通过结构自洽校验才算数,
//  校验不过就报"未定位" —— 不做多路探测、不做特征码扫描。
//
//    1. GNames   : 实测 RVA → 前 64 个索引必须能解出名字(名字池的铁证)
//    2. GObjects : 实测 RVA → FUObjectArray 结构自洽校验
//    3. GEngine  : 实测 RVA → 指向的对象类名必须是 GameEngine
//    4. GWorld   : 实测 RVA → 指向的对象类名必须是 World
//
//  为什么保留"实测 RVA + 校验"而不是纯 RVA: 校验器就是安全网。游戏更新后
//  RVA 失效时, 校验必然失败, 于是报告"未定位"而不是拿着一堆垃圾偏移去读内存。
//  那时把下面 baseline 里的四个常量更新一下即可 —— 这是可预测的维护动作,
//  比维护一套自动探测简单得多。
//
//  实测基线(analysis/docs/Dungeons2_基址与运行时结构_实测.md):
//    GObjects 0x0BEA8BF0 | GNames 0x0BDC5040 | GEngine 0x0C03A1C0 | GWorld 0x0C037A80
// ============================================================================
#include "payload/ue/Engine.h"

#include "common/PeImage.h"
#include "common/Text.h"

#include <string>

namespace epsilon::ue {

void Engine::setModule(uint64_t base, uint64_t size, std::wstring path) {
    moduleBase_ = base;
    moduleSize_ = size;
    modulePath_ = std::move(path);
}

bool Engine::init() {
    // 先把标签钉上 —— 定位失败时也要能正确显示是哪一项没找到。
    gengine_.label = "GEngine";
    gworld_.label = "GWorld";

    if (!moduleBase_) return false;

    // GNames 必须先立起来 —— GObjects 的校验要靠它验名字。
    if (!locateGNames()) return false;
    names_.setBlocks(names_.blocksAddress());

    if (!locateGObjects()) return false;
    objects_.setNamePool(&names_);

    reflection_.bind(&names_);

    const bool ok = objects_.valid() && names_.valid();
    if (ok) locateEngineAndWorld();
    return ok;
}

bool Engine::rescan() { return init(); }

// ---------------------------------------------------------------------------
bool Engine::locateGNames() {
    const uint64_t candidate = moduleBase_ + baseline::gNames;
    // FNameEntryAllocator::Blocks = GNames + 0x10
    NamePool np(candidate + 0x10);

    // 名字池正确的铁证: 头几十个索引必须能解出引擎自己的名字。
    const int sc = np.score(64);
    if (sc < 32) {
        gNamesHow_ = fmt("实测 RVA {:#x} 校验失败(前 64 个索引只命中 {}/64, 需要 ≥32)",
                          baseline::gNames, sc);
        return false;
    }

    names_.setBlocks(candidate + 0x10);
    gNamesRva_ = baseline::gNames;
    gNamesHow_ = fmt("实测 RVA {:#x} → 块数组 {} (前 64 个索引命中 {}/64)",
                      baseline::gNames, hex(names_.blocksAddress(), 16), sc);
    return true;
}

bool Engine::locateGObjects() {
    const uint64_t va = moduleBase_ + baseline::gObjects;

    ObjectArray oa(va);
    oa.setNamePool(&names_);
    if (!oa.refresh()) {
        gObjectsHow_ = fmt("实测 RVA {:#x} 读不到 FUObjectArray", baseline::gObjects);
        return false;
    }

    const int conf = oa.validate();
    if (conf == 0) {
        gObjectsHow_ = fmt("实测 RVA {:#x} 结构自洽校验失败", baseline::gObjects);
        return false;
    }

    objects_ = oa;
    gObjectsRva_ = baseline::gObjects;
    gObjectsHow_ = fmt("实测 RVA {:#x} → {} 个对象 (置信度 {}/3)",
                        baseline::gObjects,
                        thousands(static_cast<uint64_t>(oa.numElements())), conf);
    return true;
}

bool Engine::locateEngineAndWorld() {
    auto trySlot = [&](GlobalSlot& slot, std::string const& label, uint32_t rva,
                        std::string_view expectClass) -> bool {
        slot.label = label;
        slot.address = moduleBase_ + rva;
        slot.value = 0;
        slot.verified = false;

        uint64_t v = 0;
        if (!safeRead(&v, reinterpret_cast<const void*>(slot.address), 8) || !v) {
            slot.how = fmt("实测 RVA {:#x} 读不到指针", rva);
            return false;
        }
        slot.value = v;
        slot.objectClass = objects_.classNameOf(v);
        slot.objectName = objects_.nameOf(v);

        if (slot.objectClass.empty() || !iequals(slot.objectClass, expectClass)) {
            slot.how = fmt("实测 RVA {:#x} → {} {} (期望类 {}, 校验失败)",
                           rva, slot.objectClass.empty() ? "?" : slot.objectClass,
                           slot.objectName, expectClass);
            return false;
        }

        slot.verified = true;
        slot.how = fmt("实测 RVA {:#x} → {} {}", rva, slot.objectClass, slot.objectName);
        return true;
    };

    trySlot(gengine_, "GEngine", baseline::gEngine, "GameEngine");
    trySlot(gworld_,  "GWorld",  baseline::gWorld,  "World");
    return gengine_.value != 0 || gworld_.value != 0;
}

// ---------------------------------------------------------------------------
std::string Engine::report() const {
    std::string s;
    s += fmt("模块基址        : {}\n", hex(moduleBase_, 16));
    s += fmt("模块大小        : {}\n", humanBytes(moduleSize_));
    s += fmt("GObjects        : {}  (RVA {:#x})\n",
             objects_.valid() ? hex(objects_.address(), 16) : "未定位", gObjectsRva_);
    if (!gObjectsHow_.empty()) s += fmt("  定位方式      : {}\n", gObjectsHow_);
    if (objects_.valid()) {
        s += fmt("  NumElements   : {}\n", thousands(static_cast<uint64_t>(objects_.numElements())));
        s += fmt("  MaxElements   : {}\n", thousands(static_cast<uint64_t>(objects_.maxElements())));
        s += fmt("  结构校验      : {}/3\n", objects_.validate());
    }
    s += fmt("GNames          : {}  (RVA {:#x})\n",
             names_.valid() ? hex(names_.blocksAddress(), 16) : "未定位", gNamesRva_);
    if (!gNamesHow_.empty()) s += fmt("  定位方式      : {}\n", gNamesHow_);
    s += fmt("属性链布局      : Next=+{:#x} Name=+{:#x} Offset=+{:#x}\n",
             reflection_.layout().fieldNext, reflection_.layout().fieldName,
             reflection_.layout().propOffset);

    auto slotLine = [&](GlobalSlot const& g) {
        if (!g.value) return fmt("{}: 未定位   [{}]\n", g.label,
                                 g.how.empty() ? "-" : g.how);
        return fmt("{}: {} → {} {}   [{}]\n", g.label, hex(g.address, 16),
                   g.objectClass, g.objectName, g.how);
    };
    s += slotLine(gengine_);
    s += slotLine(gworld_);
    return s;
}

} // namespace epsilon::ue
