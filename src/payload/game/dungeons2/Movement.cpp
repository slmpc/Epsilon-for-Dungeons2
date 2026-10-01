#include "payload/game/dungeons2/Movement.h"

#include "common/Text.h"
#include "payload/game/Field.h"

namespace epsilon::game::dungeons2 {
namespace {

namespace cm = offsets::characterMovement;
namespace ma = offsets::movementAttribute;

struct FieldDescriptor {
    std::string_view name;
    uint32_t         builtinOffset;   // 0 = 代码里没有可信的固有偏移
};

constexpr FieldDescriptor fieldTable[] = {
    {"MaxStepHeight",              cm::maxStepHeight},
    {"JumpZVelocity",              cm::jumpZVelocity},
    {"WalkableFloorAngle",         cm::walkableFloorAngle},
    {"GravityScale",               cm::gravityScale},
    {"GravityDirection",           cm::gravityDirection},
    {"MaxWalkSpeed",               cm::maxWalkSpeed},
    {"MaxWalkSpeedCrouched",       cm::maxWalkSpeedCrouched},
    {"MaxSwimSpeed",               cm::maxSwimSpeed},
    {"MaxFlySpeed",                cm::maxFlySpeed},
    {"MaxAcceleration",            cm::maxAcceleration},
    {"BrakingDecelerationWalking", cm::brakingDecelerationWalking},
    {"AirControl",                 cm::airControl},
    {"Mass",                       cm::mass},
    {"MovementSpeedMultiplier",    0},
};
static_assert(std::size(fieldTable) == movementFieldCount);

struct AttributeDescriptor {
    std::string_view name;
    uint32_t         declaredOffset;
};

constexpr AttributeDescriptor attributeTable[] = {
    {"MovementSpeedMultiplier",   ma::movementSpeedMultiplier},
    {"MovementFriction",          ma::movementFriction},
    {"MovementFrictionMultiplier", ma::movementFrictionMultiplier},
    {"MovementRotation",          ma::movementRotation},
    {"MovementRotationMultiplier", ma::movementRotationMultiplier},
    {"MovementGravity",           ma::movementGravity},
    {"GravityScale",              ma::gravityScale},
    {"AirControl",                ma::airControl},
    {"RollCooldown",              ma::rollCooldown},
    {"RollCharges",               ma::rollCharges},
    {"Mass",                      ma::mass},
    {"InteractionRange",          ma::interactionRange},
};
static_assert(std::size(attributeTable) == movementAttributeCount);

constexpr size_t indexOf(MovementField f) noexcept { return static_cast<size_t>(f); }
constexpr size_t indexOf(MovementAttribute a) noexcept { return static_cast<size_t>(a); }

} // namespace

// ===========================================================================
//  移动组件字段
// ===========================================================================
std::string_view movementFieldName(MovementField field) noexcept {
    const size_t i = indexOf(field);
    return i < movementFieldCount ? fieldTable[i].name : std::string_view{};
}

uint32_t builtinMovementOffset(MovementField field) noexcept {
    const size_t i = indexOf(field);
    return i < movementFieldCount ? fieldTable[i].builtinOffset : 0;
}

void MovementLayout::set(MovementField field, uint32_t offset) noexcept {
    const size_t i = indexOf(field);
    if (i >= movementFieldCount || offset == 0) return;
    if (offsets_[i] == 0) ++filled_;
    offsets_[i] = offset;
}

uint32_t MovementLayout::offsetOf(MovementField field) const noexcept {
    const size_t i = indexOf(field);
    return i < movementFieldCount ? offsets_[i] : 0;
}

bool MovementLayout::has(MovementField field) const noexcept {
    return offsetOf(field) != 0;
}

bool MovementLayout::hasCore() const noexcept {
    return has(MovementField::maxWalkSpeed) && has(MovementField::jumpZVelocity);
}

std::string MovementLayout::describe() const {
    std::string s;
    auto add = [&](MovementField f) {
        if (!s.empty()) s += "  ";
        s += movementFieldName(f);
        s += '=';
        const uint32_t off = offsetOf(f);
        s += off ? fmt("+{:#x}", off) : std::string("?");
    };
    add(MovementField::maxWalkSpeed);
    add(MovementField::jumpZVelocity);
    add(MovementField::gravityScale);
    add(MovementField::airControl);
    add(MovementField::maxAcceleration);
    add(MovementField::movementSpeedMultiplier);
    return s;
}

std::optional<float> MovementComponent::read(MovementField field) const {
    return readAt(layout_.offsetOf(field));
}

bool MovementComponent::write(MovementField field, float value) const {
    return writeAt(layout_.offsetOf(field), value);
}

std::optional<float> MovementComponent::readAt(uint32_t offset) const {
    if (!address_ || offset == 0) return std::nullopt;
    return readF32(address_, offset);
}

bool MovementComponent::writeAt(uint32_t offset, float value) const {
    if (!address_ || offset == 0) return false;
    return writeF32(address_, offset, value);
}

// ===========================================================================
//  GAS 属性集
// ===========================================================================
std::string_view movementAttributeName(MovementAttribute attribute) noexcept {
    const size_t i = indexOf(attribute);
    return i < movementAttributeCount ? attributeTable[i].name : std::string_view{};
}

uint32_t declaredAttributeOffset(MovementAttribute attribute) noexcept {
    const size_t i = indexOf(attribute);
    return i < movementAttributeCount ? attributeTable[i].declaredOffset : 0;
}

uint32_t attributeDataOffset(MovementAttribute attribute) noexcept {
    const uint32_t declared = declaredAttributeOffset(attribute);
    return declared ? declared + ma::dataShift : 0;
}

std::optional<float> MovementAttributeSet::read(MovementAttribute attribute) const {
    const uint32_t base = attributeDataOffset(attribute);
    if (!base) return std::nullopt;
    return readAt(base + ma::currentValueDelta);
}

bool MovementAttributeSet::write(MovementAttribute attribute, float value) const {
    const uint32_t base = attributeDataOffset(attribute);
    if (!base) return false;
    // Base 与 Current 都写: GAS 聚合时若只改了一个, 另一个可能把它盖回去。
    bool ok = writeAt(base, value);
    ok |= writeAt(base + ma::currentValueDelta, value);
    return ok;
}

std::optional<float> MovementAttributeSet::readAt(uint32_t offset) const {
    if (!address_ || offset == 0) return std::nullopt;
    return readF32(address_, offset);
}

bool MovementAttributeSet::writeAt(uint32_t offset, float value) const {
    if (!address_ || offset == 0) return false;
    return writeF32(address_, offset, value);
}

} // namespace epsilon::game::dungeons2
