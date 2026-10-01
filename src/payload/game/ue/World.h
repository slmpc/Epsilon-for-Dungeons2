// ============================================================================
//  World.h — UWorld / ULevel / Actor 遍历
//
//  字段偏移: docs/offsets/world.md
// ============================================================================
#pragma once

#include "payload/game/ue/Engine.h"
#include "payload/game/Field.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epsilon::game::ue {

struct ActorInfo {
    int32_t     index = 0;
    uint64_t    address = 0;
    std::string name;
    std::string className;
    uint64_t    klass = 0;
};

struct LevelInfo {
    uint64_t    address = 0;
    std::string name;
    int32_t     actorCount = 0;
    int32_t     offsetActors = 0;
    std::string offsetSource;
};

struct WorldInfo {
    uint64_t    address = 0;
    std::string name;
    std::string className;
    uint64_t    persistentLevel = 0;
    int32_t     offsetPersistentLevel = 0;
    std::string offsetSource;
    std::string levelName;
};

class WorldView {
public:
    explicit WorldView(Engine& engine) : eng_(engine) {}

    [[nodiscard]] std::optional<WorldInfo> currentWorld() const;
    [[nodiscard]] std::optional<LevelInfo> persistentLevel() const;

    [[nodiscard]] std::vector<ActorInfo> actors(WorldInfo const& world, size_t limit = 0) const;
    [[nodiscard]] std::vector<ActorInfo> actors(size_t limit = 0) const;

    // 解析某类属性的偏移。返回 nullopt 表示反射与已知表都没给出答案。
    [[nodiscard]] std::optional<int32_t> resolveOffset(uint64_t klass, std::string_view prop) const;

private:
    [[nodiscard]] std::optional<int32_t> findOffset(uint64_t klass, std::string_view prop,
                                                    int32_t fallback, std::string& how) const;

    Engine& eng_;
};

} // namespace epsilon::game::ue
