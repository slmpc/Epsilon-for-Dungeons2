#include "payload/game/ue/Engine.h"

#include "common/Text.h"
#include "payload/game/Field.h"

#include <string>

namespace epsilon::game::ue {
namespace {

// 名字池校验的门槛: 顺序走 scoreEntries 条, 至少一半能解出名字才算这块地址可信。
constexpr int minNamePoolScore = 32;

} // namespace

void Engine::setModule(uint64_t base, uint64_t size, std::wstring path) {
    moduleBase_ = base;
    moduleSize_ = size;
    modulePath_ = std::move(path);
}

bool Engine::init() {
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

bool Engine::locateGNames() {
    const uint64_t candidate = moduleBase_ + offsets::globals::gNames;
    NamePool np(candidate + offsets::namePool::blocks);

    const int sc = np.score();
    if (sc < minNamePoolScore) {
        gNamesHow_ = fmt("实测 RVA {:#x} 校验失败(前 {} 个条目只命中 {}, 需要 ≥{})",
                         offsets::globals::gNames,
                         static_cast<uint64_t>(offsets::namePool::scoreEntries), sc,
                         minNamePoolScore);
        return false;
    }

    names_.setBlocks(candidate + offsets::namePool::blocks);
    gNamesRva_ = offsets::globals::gNames;
    gNamesHow_ = fmt("实测 RVA {:#x} → 块数组 {} (前 {} 个条目命中 {})",
                     offsets::globals::gNames, hex(names_.blocksAddress(), 16),
                     static_cast<uint64_t>(offsets::namePool::scoreEntries), sc);
    return true;
}

bool Engine::locateGObjects() {
    const uint64_t va = moduleBase_ + offsets::globals::gObjects;

    ObjectArray oa(va);
    oa.setNamePool(&names_);
    if (!oa.refresh()) {
        gObjectsHow_ = fmt("实测 RVA {:#x} 读不到 FUObjectArray", offsets::globals::gObjects);
        return false;
    }

    const int conf = oa.validate();
    if (conf == 0) {
        gObjectsHow_ = fmt("实测 RVA {:#x} 结构自洽校验失败", offsets::globals::gObjects);
        return false;
    }

    objects_ = oa;
    gObjectsRva_ = offsets::globals::gObjects;
    gObjectsHow_ = fmt("实测 RVA {:#x} → {} 个对象 (置信度 {}/3)",
                       offsets::globals::gObjects,
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

        auto v = readPtr(slot.address, 0);
        if (!v) {
            slot.how = fmt("实测 RVA {:#x} 读不到指针", rva);
            return false;
        }
        slot.value = *v;
        slot.objectClass = objects_.classNameOf(*v);
        slot.objectName = objects_.nameOf(*v);

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

    trySlot(gengine_, "GEngine", offsets::globals::gEngine, "GameEngine");
    trySlot(gworld_,  "GWorld",  offsets::globals::gWorld,  "World");
    return gengine_.value != 0 || gworld_.value != 0;
}

std::string Engine::report() const {
    std::string s;
    s += fmt("模块基址        : {}\n", hex(moduleBase_, 16));
    s += fmt("模块大小        : {}\n", humanBytes(moduleSize_));
    s += fmt("GObjects        : {}  (RVA {:#x})\n",
             objects_.valid() ? hex(objects_.address(), 16) : "未定位", gObjectsRva_);
    if (!gObjectsHow_.empty()) s += fmt("  定位方式      : {}\n", gObjectsHow_);
    if (objects_.valid()) {
        s += fmt("  NumElements   : {}\n",
                 thousands(static_cast<uint64_t>(objects_.numElements())));
        s += fmt("  MaxElements   : {}\n",
                 thousands(static_cast<uint64_t>(objects_.maxElements())));
        s += fmt("  结构校验      : {}/3\n", objects_.validate());
    }
    s += fmt("GNames          : {}  (RVA {:#x})\n",
             names_.valid() ? hex(names_.blocksAddress(), 16) : "未定位", gNamesRva_);
    if (!gNamesHow_.empty()) s += fmt("  定位方式      : {}\n", gNamesHow_);
    s += fmt("字段布局        : Next=+{:#x} Name=+{:#x} Offset=+{:#x}\n",
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

} // namespace epsilon::game::ue
