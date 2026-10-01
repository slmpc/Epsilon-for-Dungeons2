# UCharacterMovementComponent — 移动组件字段

代码：`offsets::characterMovement`、接口 `dungeons2::MovementComponent`
（[`Movement.cpp`](../../src/payload/game/dungeons2/Movement.cpp)）

> ⚠️ **这些字段不是速度的真正来源。** 实测 `MaxWalkSpeed` 每帧被游戏从 GAS 属性重算，
> 写它必然被覆盖。真正生效的是 [movement-attributes.md](movement-attributes.md) 里的
> `ATR_Movement` 属性集。本文档记录的偏移仍然有效（读取用），但**不要**拿它们当写入目标。

## 偏移来源

不是猜的，是从二进制的**代码生成属性表**（`FPropertyParams`）里读出来的。
UE 的 UHT 会为每个类的属性生成这份表，其中 `STRUCT_OFFSET(Class, Property)` 是编译期常量。
Shipping 构建里这张表在 `.rdata`，每项形如：

```
[flags][ArrayDim][Offset][NameUTF8 指针]
```

`UCharacterMovementComponent` 的属性表在 `0x14973xxxx` 区段。

⚠️ 这张表的**项布局与 `ATR_Movement` 那张不同**：这里偏移在 `+0x2c`，
`ATR_Movement` 那张在 `+0x24`。不要混用。

## 类继承链

```
CharacterMovementComponent              （UE 基类，实测类大小 4048 字节）
  └─ SWMovementComponent
      └─ GASCharacterMovementComponent
          └─ UPlayerCharacterMovementComponent   ← 游戏自己的子类（RTTI 实测存在）
```

基类字段在子类里的偏移**不会变**，所以下面这些父类偏移在玩家实例上直接可用。
（实测最大的偏移 `0x2fc` 远小于基类大小 4048，自洽。）

RTTI 里还能看到游戏自己加的复制属性：

```
.?AVUPlayerCharacterMovementComponent@@
MovementSpeedMultiplier
OnRep_MovementSpeedMultiplier
```

## 偏移表

| 字段 | 偏移 | 可信度 |
|---|---|---|
| `MaxStepHeight` | `+0x1a0` | **已验证** |
| `JumpZVelocity` | `+0x1a4` | **已验证**（读出 110） |
| `WalkableFloorAngle` | `+0x1ac` | 属性表 |
| `GravityScale` | `+0x1c0` | ⚠️ **存疑** |
| `GravityDirection` | `+0x1d0` | 属性表 |
| `MaxWalkSpeed` | `+0x234` | **已验证**（读出 100） |
| `MaxWalkSpeedCrouched` | `+0x278` | 属性表 |
| `MaxSwimSpeed` | `+0x27c` | 属性表 |
| `MaxFlySpeed` | `+0x280` | 属性表 |
| `MaxAcceleration` | `+0x288` | **已验证**（读出 600） |
| `BrakingDecelerationWalking` | `+0x29c` | 属性表 |
| `AirControl` | `+0x2ac` | 属性表（俯视角游戏为 0，合理） |
| `Mass` | `+0x2fc` | 属性表 |
| `MovementSpeedMultiplier` | — | ❌ **不可用**（见下） |

### 为什么 `GravityScale +0x1c0` 存疑

读出来是 **0**，而且它在属性表里的那一项落在**另一个地址区段**，可能属于别的类。
真身在 `ATR_Movement` 里（`+0xf0` 声明 / `+0xf8` 实际，实测约 1.2）。

### 为什么 `MovementSpeedMultiplier` 不在表里

它的属性表项算出的偏移**小于基类大小**，因此判定为其它类的属性，不可采信。
这个字段只可能来自运行时反射，而本构建反射不可用 —— 所以 Speed 模块拿它当目标时会退回
`MaxWalkSpeed`（并且会明确告知，不静默退回）。

## 邻域窗口（只读诊断）

`mv` 命令会把三个区段按 float 打出来，用来人工判断「哪个槽是值、哪个是它的倒数」：

| 窗口 | 含义 |
|---|---|
| `+0x1a0` | 跳跃相关 |
| `+0x22c` | 速度相关（与属性表相邻） |
| `+0x1010` | 游戏自己的速度源 |

两个已经用过的线索：

- 有一个 **37 字节的 setter** 同时写 `+0x230` 与 `1/(+0x230)` 到 `+0x234`
  → 这两个槽是一对，只改其中一个会破坏不变量

- 反汇编发现一个 **56 字节的函数**每帧做字段复制：

  ```
  a1[141] = a1[1030]   →  +0x234 = +0x1018
  a1[158] = a1[1031]   →  +0x278 = +0x101C
  a1[163] = a1[1032]   →  +0x28C = +0x1020
  ```

  即 UE 字段是从游戏自己的速度值「复制」过来的。
  **但这条假设已被证伪**：实测 `+0x1018` 恒为 0，写入它也不影响 `MaxWalkSpeed`。
  真实数据流仍未找到 —— 见 [../reverse/movement-attributes.md](../reverse/movement-attributes.md)。

## 维护提示

游戏更新后 `mv` 读出 0 或荒谬值，就是这张表失效了。重新按
[README.md](README.md) 的四步法实测，然后只改 `Offsets.h` 里的 `characterMovement` 命名空间。
