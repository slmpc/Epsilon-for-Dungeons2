// ============================================================================
//  Offsets.h — 目标构建上全部原始偏移的唯一出处
//
//  规则: 除本文件外, 任何地方都不得出现裸偏移字面量。
//  业务代码一律通过 game/ 下的类型化接口访问字段。
//  每个常量的实测依据与验证方法见 docs/offsets/。
// ============================================================================
#pragma once

#include <cstdint>

namespace epsilon::game::offsets {

// ---------------------------------------------------------------- 引擎全局 (RVA)
// 实测基线, 仅作快速路径; 失效时校验必然失败, 于是报告"未定位"。
namespace globals {
inline constexpr uint32_t gObjects = 0x0BEA8BF0;
inline constexpr uint32_t gNames   = 0x0BDC5040;
inline constexpr uint32_t gEngine  = 0x0C03A1C0;
inline constexpr uint32_t gWorld   = 0x0C037A80;
} // namespace globals

// ---------------------------------------------------------------- UObject
namespace object {
inline constexpr uint32_t flags         = 0x08;
inline constexpr uint32_t internalIndex = 0x0C;
inline constexpr uint32_t classPrivate  = 0x10;
inline constexpr uint32_t namePrivate   = 0x18;
inline constexpr uint32_t outerPrivate  = 0x20;
} // namespace object

// ---------------------------------------------------------------- FUObjectArray / TUObjectArray
namespace objectArray {
inline constexpr uint32_t chunkTable  = 0x10;   // GObjects 内嵌的 TUObjectArray 起始
inline constexpr uint32_t maxElements = 0x20;
inline constexpr uint32_t numElements = 0x24;
inline constexpr uint32_t maxChunks   = 0x28;
inline constexpr uint32_t numChunks   = 0x2C;
inline constexpr uint32_t itemStride  = 0x18;   // FUObjectItem 步长
inline constexpr uint32_t itemsPerChunk = 0x10000;
} // namespace objectArray

// ---------------------------------------------------------------- FNamePool
namespace namePool {
inline constexpr uint32_t blocks        = 0x10;      // GNames + 0x10 = FNameEntryAllocator::Blocks
inline constexpr uint32_t blockSize     = 0x10000;   // 每个块 64 KiB
inline constexpr uint32_t headerSize    = 2;         // FNameEntry 头部 uint16
inline constexpr uint32_t maxNameLength = 1023;      // Len 位域 10 bit
inline constexpr uint32_t encodedShift  = 1;         // 索引里的字节偏移是右移一位存的
inline constexpr int32_t  maxSanityIndex = 1 << 22;
inline constexpr uint32_t scoreEntries  = 64;        // 校验 FNamePool 时顺序走的条目数
} // namespace namePool

// ---------------------------------------------------------------- UStruct / UField
namespace uStruct {
inline constexpr uint32_t superStruct   = 0x40;
inline constexpr uint32_t children      = 0x48;
inline constexpr uint32_t childProps    = 0x50;
inline constexpr uint32_t propertiesSize = 0x58;
inline constexpr uint32_t fieldNext     = 0x28;      // UField::Next
} // namespace uStruct

// ---------------------------------------------------------------- FField / FProperty / FFieldClass
namespace field {
inline constexpr uint32_t classPrivate = 0x08;
inline constexpr uint32_t next         = 0x20;
inline constexpr uint32_t namePrivate  = 0x28;
} // namespace field

namespace fieldClass {
inline constexpr uint32_t namePrivate = 0x18;
} // namespace fieldClass

namespace property {
inline constexpr uint32_t arrayDim = 0x30;
inline constexpr uint32_t size     = 0x34;
inline constexpr uint32_t flags    = 0x38;
inline constexpr uint32_t offset   = 0x40;   // Offset_Internal
} // namespace property

// FStructProperty::Struct / FObjectProperty::PropertyClass 的候选槽, 按可信度排列。
namespace propertyInner {
inline constexpr uint32_t slots[] = {0x78, 0x70, 0x80};
} // namespace propertyInner

// ---------------------------------------------------------------- UWorld / ULevel / AActor
namespace world {
inline constexpr uint32_t persistentLevel = 0x30;
} // namespace world

namespace level {
inline constexpr uint32_t actors = 0xA0;
} // namespace level

// AActor::RootComponent / USceneComponent::RelativeLocation 的候选值 (+0x1B8 / +0x160)
// 尚未验证, 因此不进入代码; 记录见 docs/offsets/world.md。

// ---------------------------------------------------------------- TArray<T>
namespace array {
inline constexpr uint32_t data = 0x00;
inline constexpr uint32_t num  = 0x08;
inline constexpr uint32_t max  = 0x0C;
inline constexpr uint32_t headerSize = 0x10;
} // namespace array

// ---------------------------------------------------------------- FString / FName
// { TCHAR* Data; int32 Num; int32 Max }
namespace stringData {
inline constexpr uint32_t data = 0x00;
inline constexpr uint32_t num  = 0x08;
inline constexpr uint32_t max  = 0x0C;
} // namespace stringData

// ---------------------------------------------------------------- D3D12 COM vtable 索引
namespace d3d12 {
// IDXGISwapChain::Present = IUnknown(3) + IDXGIObject(4) + IDXGIDeviceSubObject(1)
inline constexpr uint32_t present = 8;
// ID3D12CommandQueue::ExecuteCommandLists
inline constexpr uint32_t executeCommandLists = 10;
} // namespace d3d12

// ---------------------------------------------------------------- UCharacterMovementComponent
// 从二进制里的代码生成属性表读出的偏移。基类字段在子类里偏移不变。
// 逐项可信度见 docs/offsets/character-movement.md。
namespace characterMovement {
inline constexpr uint32_t maxStepHeight              = 0x1A0;
inline constexpr uint32_t jumpZVelocity              = 0x1A4;
inline constexpr uint32_t walkableFloorAngle         = 0x1AC;
inline constexpr uint32_t gravityScale               = 0x1C0;
inline constexpr uint32_t gravityDirection           = 0x1D0;
inline constexpr uint32_t maxWalkSpeed               = 0x234;
inline constexpr uint32_t maxWalkSpeedCrouched       = 0x278;
inline constexpr uint32_t maxSwimSpeed               = 0x27C;
inline constexpr uint32_t maxFlySpeed                = 0x280;
inline constexpr uint32_t maxAcceleration            = 0x288;
inline constexpr uint32_t brakingDecelerationWalking = 0x29C;
inline constexpr uint32_t airControl                 = 0x2AC;
inline constexpr uint32_t mass                       = 0x2FC;
} // namespace characterMovement

// ---------------------------------------------------------------- ATR_Movement 属性集
// 代码生成属性表里**声明**的位置(实测 0x14a069800 区段, 每项 0x40, 偏移在 +0x24)。
// 实际数据位于 declared + dataShift。见 docs/offsets/movement-attributes.md。
namespace movementAttribute {
inline constexpr uint32_t dataShift = 0x08;
inline constexpr uint32_t currentValueDelta = 0x04;   // FGameplayAttributeData::CurrentValue

inline constexpr uint32_t movementSpeedMultiplier      = 0x90;
inline constexpr uint32_t movementFriction              = 0xA0;
inline constexpr uint32_t movementFrictionMultiplier    = 0xB0;
inline constexpr uint32_t movementRotation              = 0xC0;
inline constexpr uint32_t movementRotationMultiplier    = 0xD0;
inline constexpr uint32_t movementGravity               = 0xE0;
inline constexpr uint32_t gravityScale                  = 0xF0;
inline constexpr uint32_t airControl                    = 0x100;
inline constexpr uint32_t rollCooldown                  = 0x110;
inline constexpr uint32_t rollCharges                   = 0x130;
inline constexpr uint32_t mass                          = 0x160;
inline constexpr uint32_t interactionRange              = 0x170;

// 属性块诊断的打印窗口(起始偏移与 float 个数)。
inline constexpr uint32_t blockDumpStart = 0x70;
inline constexpr uint32_t blockDumpFloats = 80;
} // namespace movementAttribute

// ---------------------------------------------------------------- 取证扫描窗口
// 只被只读诊断命令使用: 静态偏移失效时, 靠这些窗口把候选槽摊出来人工判定。
namespace scan {
inline constexpr uint32_t structPointerSlotFirst = 0x20;
inline constexpr uint32_t structPointerSlotLast  = 0x98;
inline constexpr uint32_t structPointerSlotStep  = 0x08;

inline constexpr uint32_t fieldNameProbeFirst = 0x18;
inline constexpr uint32_t fieldNameProbeLast  = 0x48;
inline constexpr uint32_t fieldNameProbeStep  = 0x04;

inline constexpr uint32_t classBodyFirst = 0x100;
inline constexpr uint32_t classBodyLast  = 0x2000;
inline constexpr uint32_t propertyLikeOffsetFirst = 0x30;
inline constexpr uint32_t propertyLikeOffsetLast  = 0x50;

inline constexpr uint32_t levelBodyFirst = 0x20;
inline constexpr uint32_t levelBodyLast  = 0x300;

// 移动组件上用于人工对照的 float 邻域窗口: 跳跃区 / 速度区 / 游戏自己的速度源区。
inline constexpr uint32_t movementNeighbourhoodBases[] = {0x1A0, 0x22C, 0x1010};
inline constexpr int      movementNeighbourhoodFloats = 8;
} // namespace scan

} // namespace epsilon::game::offsets
