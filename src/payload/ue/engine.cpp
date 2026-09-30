// ============================================================================
//  engine.cpp
// ============================================================================
#include "payload/ue/engine.h"

#include "common/pattern_scan.h"
#include "common/pe_image.h"
#include "common/text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace mcd2::ue {
namespace {

// 判断一个地址是否落在某个已加载模块(映像)里。
bool in_image(uint64_t va) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(reinterpret_cast<LPCVOID>(va), &mbi, sizeof(mbi)) == 0) return false;
    return mbi.Type == MEM_IMAGE;
}

// 一次扫描 .data 里所有像指针的值, 逐个回调。返回处理了多少个。
template <typename Fn>
size_t scan_data_pointers(uint64_t data_base, size_t data_size, Fn&& fn) {
    if (!data_base || data_size < 8) return 0;
    size_t n = 0;
    MemoryWalker walker(reinterpret_cast<const void*>(data_base), data_size);
    do {
        const uint8_t* p = walker.begin();
        const uint8_t* e = walker.window_end();
        for (; p + 8 <= e; p += 8) {
            uint64_t v = 0;
            std::memcpy(&v, p, 8);
            if (v < 0x10000 || v > 0x7FFFFFFFFFFFull) continue;
            if ((v & 0x7) != 0) continue;
            if (!fn(reinterpret_cast<uint64_t>(p), v)) return n;
            ++n;
        }
    } while (walker.next_window());
    return n;
}

} // namespace

void Engine::set_module(uint64_t base, uint64_t size, std::wstring path) {
    module_base_ = base;
    module_size_ = size;
    module_path_ = std::move(path);
}

bool Engine::init() {
    // 先把标签钉上 —— 定位失败时也要能正确显示是哪一项没找到。
    // (之前这两个标签只在 locate_engine_and_world 内部设置, 而那个函数在
    //  引擎整体定位失败时根本不会被调用, 于是报告里出现空的 ": 未定位"。)
    gengine_.label = "GEngine";
    gworld_.label = "GWorld";

    if (!module_base_) return false;

    // GNames 必须先立起来 —— GObjects 的校验要靠它验名字。
    if (!locate_gnames()) return false;
    names_.set_blocks(names_.blocks_address());

    if (!locate_gobjects()) return false;
    objects_.set_name_pool(&names_);

    reflection_.bind(&names_);

    // 用真实类样本校正属性链偏移。
    reflection_.calibrate(objects_);

    const bool ok = objects_.valid() && names_.valid();
    if (ok) locate_engine_and_world();
    return ok;
}

bool Engine::rescan() { return init(); }

// ---------------------------------------------------------------------------
bool Engine::locate_gnames() {
    auto image = PeImage::from_memory(reinterpret_cast<const void*>(module_base_));
    if (!image || !image->valid()) return false;

    // --- 快路径: 实测 RVA ---
    {
        const uint64_t candidate = module_base_ + baseline::kGNames;
        NamePool np(candidate + 0x10);          // FNameEntryAllocator::Blocks = GNames + 0x10
        // 名字池正确的铁证: 头几个索引必须是引擎自己的名字。
        const int sc = np.score(64);
        if (sc >= 32) {
            names_.set_blocks(candidate + 0x10);
            gnames_rva_ = baseline::kGNames;
            gnames_how_ = fmt("实测 RVA {:#x} → 块数组 {} (前 64 个索引命中 {}/64)",
                              baseline::kGNames, hex(names_.blocks_address(), 16), sc);
            return true;
        }
    }

    // --- 兜底: 扫描 .data 找 FNamePool::Blocks 数组 ---
    if (auto const* data_sec = image->section(".data")) {
        const uint64_t dbase = module_base_ + data_sec->vaddr;
        const uint64_t found = locate_name_pool_blocks(dbase, data_sec->span());
        if (found) {
            NamePool np(found);
            const int sc = np.score(64);
            if (sc >= 16) {
                names_.set_blocks(found);
                gnames_rva_ = static_cast<uint32_t>(found - module_base_);
                gnames_how_ = fmt("扫描 .data 命中块数组 {} (前 64 个索引命中 {}/64)",
                                  hex(found, 16), sc);
                return true;
            }
        }
    }
    gnames_how_ = "未找到 FNamePool";
    return false;
}

bool Engine::locate_gobjects() {
    auto image = PeImage::from_memory(reinterpret_cast<const void*>(module_base_));
    if (!image || !image->valid()) return false;

    auto try_candidate = [&](uint64_t va, std::string const& how) -> bool {
        ObjectArray oa(va);
        oa.set_name_pool(&names_);
        if (!oa.refresh()) return false;
        const int conf = oa.validate();
        if (conf == 0) return false;

        objects_ = oa;
        gobjects_rva_ = static_cast<uint32_t>(va - module_base_);
        gobjects_how_ = fmt("{} → {} 个对象 (置信度 {}/3)", how,
                            thousands(static_cast<uint64_t>(oa.num_elements())), conf);
        return true;
    };

    // --- 快路径 1: 实测 RVA 直接当 FUObjectArray 地址 ---
    if (try_candidate(module_base_ + baseline::kGObjects, fmt("实测 RVA {:#x}", baseline::kGObjects)))
        return true;

    // --- 快路径 2: 实测 RVA 当 TUObjectArray, 回推 FUObjectArray ---
    //   因为 GObjects + 0x10 == Objects 指针字段, 且访问器里就是 "mov rax,[rip+0x10]",
    //   所以实测值也可能已经是被 +0x10 之后的地址。
    if (baseline::kGObjects >= 0x10 &&
        try_candidate(module_base_ + baseline::kGObjects - 0x10,
                      fmt("实测 RVA {:#x} - 0x10", baseline::kGObjects)))
        return true;

    // --- 兜底: 特征码扫描 "shr r8,0x10" 之后紧跟 "mov rax,[rip+disp]" ---
    auto anchor = Pattern::parse(sigs::kGObjectsAnchor);
    if (anchor) {
        ScanOptions so;
        so.section = ".text";
        so.max_hits = 512;
        const auto hits = scan_module(*image, reinterpret_cast<const void*>(module_base_),
                                     *anchor, so);
        for (auto const& h : hits) {
            // 在锚点后 48 字节内找 mov rax, [rip+disp]
            for (uint32_t off = 0; off < 48; ++off) {
                const uint64_t at = h.address + off;
                uint8_t b[3]{};
                if (!safe_read(b, reinterpret_cast<const void*>(at), 3)) break;
                if (b[0] != 0x48 || b[1] != 0x8B || b[2] != 0x05) continue;

                // rip 目标 = 指令地址 + 7 + disp32
                auto const target = rip_target(at, 7, 3);
                if (!target) break;

                // 访问器里拿的是 TUObjectArray*(即 GObjects+0x10), 所以减回去。
                if (*target < 0x10) break;
                const uint64_t gobjects = *target - 0x10;
                if (gobjects < module_base_ || gobjects >= module_base_ + module_size_) break;

                if (try_candidate(gobjects, fmt("特征码命中 {:#x}", at))) return true;
                break;
            }
        }
    }

    gobjects_how_ = "未找到 FUObjectArray";
    return false;
}

bool Engine::locate_engine_and_world() {
    auto try_slot = [&](GlobalSlot& slot, std::string const& label, uint32_t rva,
                        std::string_view expect_class, std::string& how) -> bool {
        slot.label = label;
        slot.address = module_base_ + rva;
        slot.value = 0;
        slot.verified = false;

        uint64_t v = 0;
        if (!safe_read(&v, reinterpret_cast<const void*>(slot.address), 8) || !v) return false;
        slot.value = v;

        const std::string cls = objects_.class_name_of(v);
        slot.object_class = cls;
        slot.object_name = objects_.name_of(v);

        if (!cls.empty() && iequals(cls, expect_class)) {
            slot.verified = true;
            how = fmt("实测 RVA {:#x} → {} {}", rva, slot.object_class, slot.object_name);
            slot.how = how;
            return true;
        }
        return false;
    };

    try_slot(gengine_, "GEngine", baseline::kGEngine, "GameEngine", gengine_.how);
    try_slot(gworld_,  "GWorld",  baseline::kGWorld,  "World",      gworld_.how);

    // --- 兜底: 不知道 RVA 时, 扫 .data 找一个指向 "GameEngine"/"World" 实例的指针 ---
    //   代价是遍历整个 .data(实测几十 MB), 所以只在快路径失败时才启用。
    auto image = PeImage::from_memory(reinterpret_cast<const void*>(module_base_));
    auto const* data_sec = image ? image->section(".data") : nullptr;
    if (!data_sec) return gengine_.value || gworld_.value;

    const uint64_t dbase = module_base_ + data_sec->vaddr;
    const size_t   dsize = data_sec->span();

    std::vector<std::pair<uint64_t, uint64_t>> candidates;   // (全局变量地址, 指向的对象)
    candidates.reserve(4096);
    if (gworld_.value == 0 || gengine_.value == 0) {
        // UObject 本体在堆上, 但它的 vtable 一定在某个模块映像里 —— 用这条判据筛,
        // 能把 .data 里绝大多数随机指针挡掉。
        scan_data_pointers(dbase, dsize, [&](uint64_t slot_va, uint64_t obj) {
            uint64_t vtbl = 0;
            if (!safe_read(&vtbl, reinterpret_cast<const void*>(obj), 8)) return true;
            if (!in_image(vtbl)) return true;
            candidates.emplace_back(slot_va, obj);
            return candidates.size() < 200000;
            });
    }

    for (auto const& [slot_va, obj] : candidates) {
        const std::string cls = objects_.class_name_of(obj);
        if (cls.empty()) continue;

        if (gengine_.value == 0 && iequals(cls, "GameEngine")) {
            gengine_.label = "GEngine";
            gengine_.address = slot_va;
            gengine_.value = obj;
            gengine_.object_class = cls;
            gengine_.object_name = objects_.name_of(obj);
            gengine_.verified = true;
            gengine_.how = fmt("扫描 .data 命中 {}", hex(slot_va, 16));
        } else if (gworld_.value == 0 && iequals(cls, "World")) {
            gworld_.label = "GWorld";
            gworld_.address = slot_va;
            gworld_.value = obj;
            gworld_.object_class = cls;
            gworld_.object_name = objects_.name_of(obj);
            gworld_.verified = true;
            gworld_.how = fmt("扫描 .data 命中 {}", hex(slot_va, 16));
        }

        if (gengine_.value && gworld_.value) break;
    }
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
    s += fmt("属性链布局      : Next=+{:#x} Name=+{:#x} (置信度 {}/3)\n",
             reflection_.layout().field_next, reflection_.layout().field_name,
             reflection_.layout().confidence);
    if (!reflection_.layout().source.empty())
        s += fmt("  依据          : {}\n", reflection_.layout().source);

    auto slot_line = [&](GlobalSlot const& g) {
        if (!g.value) return fmt("{}: 未定位\n", g.label);
        return fmt("{}: {} → {} {}   [{}]\n", g.label, hex(g.address, 16),
                   g.object_class, g.object_name, g.how.empty() ? "-" : g.how);
    };
    s += slot_line(gengine_);
    s += slot_line(gworld_);
    return s;
}

} // namespace mcd2::ue
