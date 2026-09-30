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
//  实测基线(analysis/docs/MCD2_基址与运行时结构_实测.md):
//    GObjects 0x0BEA8BF0 | GNames 0x0BDC5040 | GEngine 0x0C03A1C0 | GWorld 0x0C037A80
// ============================================================================
#include "payload/ue/engine.h"

#include "common/pe_image.h"
#include "common/text.h"

#include <string>

namespace mcd2::ue {

void Engine::set_module(uint64_t base, uint64_t size, std::wstring path) {
    module_base_ = base;
    module_size_ = size;
    module_path_ = std::move(path);
}

bool Engine::init() {
    // 先把标签钉上 —— 定位失败时也要能正确显示是哪一项没找到。
    gengine_.label = "GEngine";
    gworld_.label = "GWorld";

    if (!module_base_) return false;

    // GNames 必须先立起来 —— GObjects 的校验要靠它验名字。
    if (!locate_gnames()) return false;
    names_.set_blocks(names_.blocks_address());

    if (!locate_gobjects()) return false;
    objects_.set_name_pool(&names_);

    reflection_.bind(&names_);

    const bool ok = objects_.valid() && names_.valid();
    if (ok) locate_engine_and_world();
    return ok;
}

bool Engine::rescan() { return init(); }

// ---------------------------------------------------------------------------
bool Engine::locate_gnames() {
    const uint64_t candidate = module_base_ + baseline::kGNames;
    // FNameEntryAllocator::Blocks = GNames + 0x10
    NamePool np(candidate + 0x10);

    // 名字池正确的铁证: 头几十个索引必须能解出引擎自己的名字。
    const int sc = np.score(64);
    if (sc < 32) {
        gnames_how_ = fmt("实测 RVA {:#x} 校验失败(前 64 个索引只命中 {}/64, 需要 ≥32)",
                          baseline::kGNames, sc);
        return false;
    }

    names_.set_blocks(candidate + 0x10);
    gnames_rva_ = baseline::kGNames;
    gnames_how_ = fmt("实测 RVA {:#x} → 块数组 {} (前 64 个索引命中 {}/64)",
                      baseline::kGNames, hex(names_.blocks_address(), 16), sc);
    return true;
}

bool Engine::locate_gobjects() {
    const uint64_t va = module_base_ + baseline::kGObjects;

    ObjectArray oa(va);
    oa.set_name_pool(&names_);
    if (!oa.refresh()) {
        gobjects_how_ = fmt("实测 RVA {:#x} 读不到 FUObjectArray", baseline::kGObjects);
        return false;
    }

    const int conf = oa.validate();
    if (conf == 0) {
        gobjects_how_ = fmt("实测 RVA {:#x} 结构自洽校验失败", baseline::kGObjects);
        return false;
    }

    objects_ = oa;
    gobjects_rva_ = baseline::kGObjects;
    gobjects_how_ = fmt("实测 RVA {:#x} → {} 个对象 (置信度 {}/3)",
                        baseline::kGObjects,
                        thousands(static_cast<uint64_t>(oa.num_elements())), conf);
    return true;
}

bool Engine::locate_engine_and_world() {
    auto try_slot = [&](GlobalSlot& slot, std::string const& label, uint32_t rva,
                        std::string_view expect_class) -> bool {
        slot.label = label;
        slot.address = module_base_ + rva;
        slot.value = 0;
        slot.verified = false;

        uint64_t v = 0;
        if (!safe_read(&v, reinterpret_cast<const void*>(slot.address), 8) || !v) {
            slot.how = fmt("实测 RVA {:#x} 读不到指针", rva);
            return false;
        }
        slot.value = v;
        slot.object_class = objects_.class_name_of(v);
        slot.object_name = objects_.name_of(v);

        if (slot.object_class.empty() || !iequals(slot.object_class, expect_class)) {
            slot.how = fmt("实测 RVA {:#x} → {} {} (期望类 {}, 校验失败)",
                           rva, slot.object_class.empty() ? "?" : slot.object_class,
                           slot.object_name, expect_class);
            return false;
        }

        slot.verified = true;
        slot.how = fmt("实测 RVA {:#x} → {} {}", rva, slot.object_class, slot.object_name);
        return true;
    };

    try_slot(gengine_, "GEngine", baseline::kGEngine, "GameEngine");
    try_slot(gworld_,  "GWorld",  baseline::kGWorld,  "World");
    return gengine_.value != 0 || gworld_.value != 0;
}

// ---------------------------------------------------------------------------
std::string Engine::report() const {
    std::string s;
    s += fmt("模块基址        : {}\n", hex(module_base_, 16));
    s += fmt("模块大小        : {}\n", human_bytes(module_size_));
    s += fmt("GObjects        : {}  (RVA {:#x})\n",
             objects_.valid() ? hex(objects_.address(), 16) : "未定位", gobjects_rva_);
    if (!gobjects_how_.empty()) s += fmt("  定位方式      : {}\n", gobjects_how_);
    if (objects_.valid()) {
        s += fmt("  NumElements   : {}\n", thousands(static_cast<uint64_t>(objects_.num_elements())));
        s += fmt("  MaxElements   : {}\n", thousands(static_cast<uint64_t>(objects_.max_elements())));
        s += fmt("  结构校验      : {}/3\n", objects_.validate());
    }
    s += fmt("GNames          : {}  (RVA {:#x})\n",
             names_.valid() ? hex(names_.blocks_address(), 16) : "未定位", gnames_rva_);
    if (!gnames_how_.empty()) s += fmt("  定位方式      : {}\n", gnames_how_);
    s += fmt("属性链布局      : Next=+{:#x} Name=+{:#x} Offset=+{:#x}\n",
             reflection_.layout().field_next, reflection_.layout().field_name,
             reflection_.layout().prop_offset);

    auto slot_line = [&](GlobalSlot const& g) {
        if (!g.value) return fmt("{}: 未定位   [{}]\n", g.label,
                                 g.how.empty() ? "-" : g.how);
        return fmt("{}: {} → {} {}   [{}]\n", g.label, hex(g.address, 16),
                   g.object_class, g.object_name, g.how);
    };
    s += slot_line(gengine_);
    s += slot_line(gworld_);
    return s;
}

} // namespace mcd2::ue
