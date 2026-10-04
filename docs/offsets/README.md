# 偏移总览

本目录记录**这个构建**（`Dungeons-Win64-Shipping.exe` / UE 5.6.1）上所有已实测的内存
偏移，以及每一条的**来源**与**可信度**。

## 代码里的唯一出处

所有裸偏移常量集中在 [`src/payload/game/Offsets.h`](../../src/payload/game/Offsets.h)。
除该文件外，代码里不允许出现偏移字面量：其余位置一律通过
[`game/`](../../src/payload/game/) 下的类型化接口访问字段。

```
src/payload/game/
  Offsets.h          全部原始偏移常量（本目录所有表格的代码出处）
  Field.h/.cpp       base+offset 的类型化读写原语、TArray 头
  ue/                UE 通用运行时结构
    NamePool.*         FNamePool (GNames)
    ObjectArray.*      FUObjectArray (GObjects) / UObject
    Reflection.*       UStruct / FField / FProperty
    Engine.*           四个引擎全局的定位
    World.*            UWorld / ULevel / Actor
  dungeons2/         本游戏特有的模型
    Movement.*         UCharacterMovementComponent 字段 + ATR_Movement 属性集
    Player.*           玩家类名判定与目标定位
    MovementResolver.* 带节流的解析、缓存与读写入口
```

## 文档索引

| 文档 | 内容 | 相关代码 |
|---|---|---|
| [engine-globals.md](engine-globals.md) | `GObjects` / `GNames` / `GEngine` / `GWorld` 的 RVA 与校验 | `offsets::globals`, `ue::Engine` |
| [object-array.md](object-array.md) | `FUObjectArray` / `FUObjectItem` / `UObject` | `offsets::objectArray`, `offsets::object`, `ue::ObjectArray` |
| [name-pool.md](name-pool.md) | `FNamePool` / `FNameEntryAllocator` / `FNameEntry` | `offsets::namePool`, `ue::NamePool` |
| [reflection.md](reflection.md) | `UStruct` / `UField` / `FField` / `FProperty` | `offsets::uStruct`, `offsets::field`, `offsets::property`, `ue::Reflection` |
| [world.md](world.md) | `UWorld` / `ULevel` / `TArray` / `FString` | `offsets::world`, `offsets::level`, `offsets::array`, `offsets::stringData`, `ue::WorldView` |
| [character-movement.md](character-movement.md) | `UCharacterMovementComponent` 的 13 个 float 字段 | `offsets::characterMovement`, `dungeons2::MovementComponent` |
| [movement-attributes.md](movement-attributes.md) | GAS 属性集 `ATR_Movement` 的 12 个属性 | `offsets::movementAttribute`, `dungeons2::MovementAttributeSet` |
| [currency.md](currency.md) | GAS 属性集 `ATR_Currency` 的 7 个货币属性；**属性表里 `Offset_Internal` 在记录 `+0x34`** 的判定过程 | `offsets::currencyAttribute`, `dungeons2::CurrencyAttributeSet` |
| [d3d12-vtable.md](d3d12-vtable.md) | `Present` / `ExecuteCommandLists` 的 COM vtable 索引 | `offsets::d3d12` |

## 偏移的两种来源

**1. 代码生成属性表（首选，本项目的默认路径）**

UE 的 UHT 会为每个类的属性生成一份参数表（`FPropertyParams`），其中
`STRUCT_OFFSET(Class, Property)` 是**编译期常量**。Shipping 构建里这张表在 `.rdata`，
每项形如：

```
[flags][ArrayDim][Offset][NameUTF8 指针]
```

所以偏移可以直接从二进制里读出来，不需要在运行时猜 `UStruct` 的布局。

⚠️ 两张表的项布局**不一致**，读偏移的位置不同：

| 属性表 | 区段 | 偏移字段位置 |
|---|---|---|
| `ATR_Movement` | `0x14a0d68xx` | 记录 `+0x34`（名字在记录 `+0x00`） |
| `ATR_Currency` | `0x14a0d00xx` | 记录 `+0x34`（同左） |
| `UCharacterMovementComponent` | `0x14973xxxx` | `+0x2c` |

**「记录 `+0x34`」的判定依据**（用 CDO 上的真实浮点值当标尺）：`ATR_Movement` 的记录
`MovementSpeed` 在 `+0x34` 读出 `0x90`，而 CDO 上 `0x98` 的值是 **700.0** —— 速度量级；
`MovementSpeedMultiplier` 随之落在 `0xA0`，CDO 值是 **1.0** —— 倍率量级。
另一种读法（名字槽 `−0x0C`）会把 `MovementSpeed` 算到 `0x00`（对象首字节，不可能）。
完整对照见 [currency.md](currency.md)。

> ℹ️ **`ATR_Movement` 的名字与偏移是按另一种读法配的**（`名字槽 − 0x0C`），
> 按上面这条规则它的名字会整体后移一条（`0x90` 那个槽其实是 `MovementSpeed`）。
> 但**物理槽位没变** —— 实测「写 `+0x98` 让移动变快」用的就是同一格，
> 所以 `Speed` 模块与 `Offsets.h` 里的常数**保持原样**，只把这条差异记在这里。

**2. 运行时反射（`ue::Reflection`）**

设计上应当优先，能自动适配游戏更新。但**本构建上不可用** —— `UStruct` 布局被改过，
`ChildProperties` 链遍历不出来，详见 [reflection.md](reflection.md) 与
[../reverse/reflection-limits.md](../reverse/reflection-limits.md)。因此实际生效的是第 1 条。

## 验证一个偏移的四步法

出处与实例见 [../reverse/property-verification.md](../reverse/property-verification.md)。

1. **属性表**读出偏移
2. **指令特征**：数一遍全 `.text` 里 `movss xmm, [reg+offset]` 的出现次数是否合理
3. **相邻性**：UE 中连续声明的几个 float 应当被同一个函数一起读
4. **运行时数值**：读出来必须是**合理的游戏数值**，而不是 0 或垃圾

## 可信度标记

下表中的「可信度」列含义：

| 标记 | 含义 |
|---|---|
| **已验证** | 已按四步法全部走过，且运行时读出合理数值 |
| **属性表** | 从代码生成属性表读出，但未做运行时数值核对 |
| **存疑** | 读出的值不合理，或表项归属可疑，**不要依赖** |
| **未证实** | 来自经验/常识，尚未实测；已从代码中移除，仅作记录 |

## 游戏更新后怎么修

这些偏移是**版本绑定**的。游戏更新后典型症状：

- `status` 报「未定位」→ 多半是 [engine-globals.md](engine-globals.md) 的四个 RVA 失效
- `props` 输出为空或值荒谬 → [reflection.md](reflection.md) 的布局常量失效
- `world` / `actors` 读出 0 个 Actor → [world.md](world.md) 的 `Actors` 偏移失效
- `mv` 读出 0 或荒谬值 → [character-movement.md](character-movement.md) 的表失效

修法一律是：**重新按四步法实测，然后只改 `Offsets.h` 里对应的常量**。
