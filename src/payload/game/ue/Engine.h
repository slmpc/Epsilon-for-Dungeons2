#pragma once

#include "payload/game/ue/NamePool.h"
#include "payload/game/ue/ObjectArray.h"
#include "payload/game/ue/Reflection.h"

#include <cstdint>
#include <string>

namespace epsilon::game::ue {

struct GlobalSlot {
    std::string label;
    uint64_t    address = 0;     // 全局变量本身的地址
    uint64_t    value = 0;       // 它指向的对象
    std::string objectName;
    std::string objectClass;
    bool        verified = false;
    std::string how;
};

class Engine {
public:
    Engine() = default;

    // 绑定自身所在模块。必须在 init() 之前调用。
    void setModule(uint64_t base, uint64_t size, std::wstring path);

    // 全流程定位。返回是否拿到可用的 GObjects + GNames。
    bool init();
    bool rescan();

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

    [[nodiscard]] std::string report() const;

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

} // namespace epsilon::game::ue
