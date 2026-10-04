#pragma once

#include <cstdint>

namespace epsilon::game::offsets {

// ---------------------------------------------------------------- 引擎全局 (RVA)
// 实测基线, 仅作快速路径; 失效时校验必然失败, 于是报告"未定位"。
// 2026-10-04 游戏更新后重新实测(旧值整体失效, 见 docs/offsets/engine-globals.md)。
namespace globals {
inline constexpr uint32_t gObjects = 0x0BF35A70;
inline constexpr uint32_t gNames   = 0x0BE51EC0;
inline constexpr uint32_t gEngine  = 0x0C0C70B0;
inline constexpr uint32_t gWorld   = 0x0C0C4970;
} // namespace globals

// ---------------------------------------------------------------- UObject
namespace object {
inline constexpr uint32_t flags         = 0x08;
inline constexpr uint32_t internalIndex = 0x0C;
inline constexpr uint32_t classPrivate  = 0x10;
inline constexpr uint32_t namePrivate   = 0x18;
inline constexpr uint32_t outerPrivate  = 0x20;

// EObjectFlags::RF_ClassDefaultObject。类默认对象(CDO)带这一位。
// 判定"这是不是 CDO"用它, 而不是看名字有没有 Default__ 前缀 ——
// 名字可能解析不出来, 标志位不会。
inline constexpr uint32_t classDefaultObjectFlag = 0x10;
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

// ---------------------------------------------------------------- ATR_Currency 属性集
// 绿宝石等货币的权威来源。属性是 GAS 的 FGameplayAttributeData:
// 声明偏移处 8 字节是一个共享描述指针, 真正的 {BaseValue, CurrentValue} 在声明值 + 8。
// 判定过程见 docs/offsets/currency.md(用 CDO 的 7 个属性槽与属性表一一对应敲定)。
namespace currencyAttribute {
inline constexpr uint32_t dataShift         = 0x08;   // 实测的数据位置 = 声明值 + 8
inline constexpr uint32_t currentValueDelta = 0x04;   // FGameplayAttributeData::CurrentValue

inline constexpr uint32_t emeralds                     = 0x90;   // CDO 默认 0
inline constexpr uint32_t emeraldsMax                  = 0xA0;   // CDO 默认 0
inline constexpr uint32_t emeraldsMin                  = 0xB0;   // CDO 默认 0
inline constexpr uint32_t emeraldIncreasePercentage    = 0xC0;   // CDO 默认 0
inline constexpr uint32_t emeraldDropChanceIncrease    = 0xD0;   // CDO 默认 0
inline constexpr uint32_t maxAdditionalEmeralds        = 0xE0;   // CDO 默认 2.0
inline constexpr uint32_t emeraldCapForDamageIncrease  = 0xF0;   // CDO 默认 0

// 属性块诊断窗口(第一个属性起, 覆盖到最后一个)。
inline constexpr uint32_t blockFirst = 0x80;
inline constexpr uint32_t blockLast  = 0x120;
inline constexpr uint32_t blockStep  = 0x10;
} // namespace currencyAttribute

// 承载货币的系统固有类名(精确匹配, 不是子串)。
inline constexpr char currencyOwnerClass[] = "ATR_Currency";

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

// 货币持有者对象前 0x140 字节的槽位, 供诊断命令摊开判读。
inline constexpr uint32_t currencyBodyFirst = 0x70;
inline constexpr uint32_t currencyBodyLast  = 0x140;
inline constexpr uint32_t currencyBodyStep  = 0x08;
} // namespace scan

} // namespace epsilon::game::offsets
