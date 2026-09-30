// ============================================================================
//  engine.h — 引擎全局定位总入口
//
//  定位策略(与 analysis/re/epsilon.py 一致, 但全部在目标进程内完成):
//    1. GObjects : 先用实测 RVA, 再用特征码兜底, 两者都过"结构自洽校验"
//    2. GNames   : 先用实测 RVA 验块数组, 再扫 .data 找
//    3. GEngine  : 实测 RVA + 校验指向的对象类名是 GameEngine
//    4. GWorld   : 实测 RVA + 校验指向的对象类名是 World
//
//  为什么保留"已知 RVA"这一条快路径: 校验器本身就是安全网。RVA 失效时校验
//  必然失败, 于是自动落到特征码/扫描路径 —— 游戏更新后不需要改代码就能活。
//
//  实测基线(analysis/docs/Dungeons2_基址与运行时结构_实测.md):
//    GObjects 0x0BEA8BF0 | GNames 0x0BDC5040 | GEngine 0x0C03A1C0 | GWorld 0x0C037A80
// ============================================================================
#pragma once

#include "payload/ue/NamePool.h"
#include "payload/ue/ObjectArray.h"
#include "payload/ue/Reflection.h"

#include <cstdint>
#include <string>

namespace epsilon::ue {

// 实测基线 RVA, 仅作快速路径
namespace baseline {
    inline constexpr uint32_t gObjects = 0x0BEA8BF0;
    inline constexpr uint32_t gNames   = 0x0BDC5040;
    inline constexpr uint32_t gEngine  = 0x0C03A1C0;
    inline constexpr uint32_t gWorld   = 0x0C037A80;
} // namespace baseline

struct GlobalSlot {
    std::string label;
    uint64_t    address = 0;     // 全局变量本身的地址
    uint64_t    value = 0;       // 它指向的对象
    std::string objectName;
    std::string objectClass;
    bool        verified = false;
    std::string how;             // 怎么定位到的
};

class Engine {
public:
    Engine() = default;

    // 绑定自身所在模块。必须在 init() 之前调用。
    void setModule(uint64_t base, uint64_t size, std::wstring path);

    // 全流程定位。返回是否拿到可用的 GObjects + GNames。
    bool init();

    [[nodiscard]] bool ready() const noexcept { return objects_.valid() && names_.valid(); }
    [[nodiscard]] bool hasEngineWorld() const noexcept { return gengine_.value && gworld_.value; }

    [[nodiscard]] ObjectArray&       objects()       noexcept { return objects_; }
    [[nodiscard]] ObjectArray const& objects() const noexcept { return objects_; }
    [[nodiscard]] NamePool&          names()         noexcept { return names_; }
    [[nodiscard]] NamePool const&    names()   const noexcept { return names_; }
    [[nodiscard]] Reflection&        reflection()    noexcept { return reflection_; }
    [[nodiscard]] Reflection const&  reflection() const noexcept { return reflection_; }

    [[nodiscard]] GlobalSlot const& gengine() const noexcept { return gengine_; }
    [[nodiscard]] GlobalSlot const& gworld()  const noexcept { return gworld_; }
    [[nodiscard]] uint64_t moduleBase() const noexcept { return moduleBase_; }
    [[nodiscard]] uint64_t moduleSize() const noexcept { return moduleSize_; }

    // 人类可读的定位报告。
    [[nodiscard]] std::string report() const;

    // 单独重扫(游戏加载完成后 GObjects 才会稳定)。
    bool rescan();

private:
    bool locateGObjects();
    bool locateGNames();
    bool locateEngineAndWorld();

    uint64_t moduleBase_ = 0;
    uint64_t moduleSize_ = 0;
    std::wstring modulePath_;

    NamePool    names_;
    ObjectArray objects_;
    Reflection  reflection_;

    GlobalSlot  gengine_;
    GlobalSlot  gworld_;

    uint32_t gObjectsRva_ = 0;
    uint32_t gNamesRva_ = 0;
    std::string gObjectsHow_;
    std::string gNamesHow_;
};

} // namespace epsilon::ue
