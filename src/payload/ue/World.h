// ============================================================================
//  world.h — UWorld / ULevel / Actor 遍历
//
//  UWorld::PersistentLevel 和 ULevel::Actors 的偏移同样不硬编码:
//  先用运行时反射从类描述里把属性偏移问出来, 问不到才退回经验值。
//  这样游戏更新后优先级自动是"反射 > 经验值", 不需要改代码。
// ============================================================================
#pragma once

#include "payload/ue/Engine.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace epsilon::ue {

// TArray<T>(UE): +0x00 Data, +0x08 Num, +0x0C Max
struct ArrayHeader {
    uint64_t data = 0;
    int32_t  num = 0;
    int32_t  max = 0;
};

struct ActorInfo {
    int32_t     index = 0;
    uint64_t    address = 0;
    std::string name;
    std::string className;
    uint64_t    klass = 0;
    // 常用字段(问不到反射时为空)
    std::optional<double> x, y, z;
};

struct LevelInfo {
    uint64_t address = 0;
    std::string name;
    int32_t actorCount = 0;
    int32_t offsetActors = 0;
    std::string offsetSource;
};

struct WorldInfo {
    uint64_t address = 0;
    std::string name;
    std::string className;
    uint64_t persistentLevel = 0;
    int32_t offsetPersistentLevel = 0;
    std::string offsetSource;
    std::string levelName;
};

class WorldView {
public:
    WorldView(Engine& engine) : eng_(engine) {}

    // 取当前 UWorld。优先用定位到的 GWorld; 取不到则在对象表里找第一个 World。
    [[nodiscard]] std::optional<WorldInfo> currentWorld() const;

    [[nodiscard]] std::optional<LevelInfo> persistentLevel() const;

    // 枚举 PersistentLevel 里的全部 Actor。
    [[nodiscard]] std::vector<ActorInfo> actors(WorldInfo const& world, size_t limit = 0) const;

    // 便捷: 一步到位。
    [[nodiscard]] std::vector<ActorInfo> actors(size_t limit = 0) const;

    // 解析某类的属性偏移(供 dump/get 命令复用)。
    [[nodiscard]] std::optional<int32_t> resolveOffset(uint64_t klass, std::string_view prop) const;

private:
    [[nodiscard]] std::optional<int32_t> findOffset(uint64_t klass, std::string_view prop,
                                                     int32_t fallback, std::string& how) const;
    [[nodiscard]] std::optional<ArrayHeader> readArray(uint64_t owner, int32_t offset) const;

    Engine& eng_;
};

} // namespace epsilon::ue
