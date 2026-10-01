# ATR_Movement — GAS 移动属性集

代码：`offsets::movementAttribute`、接口 `dungeons2::MovementAttributeSet`
（[`Movement.cpp`](../../src/payload/game/dungeons2/Movement.cpp)）

> ★ **这里才是移动参数的真身。** 属性是**持久**的：写入后连续 8 次采样全部保持，
> 且跨注入仍然保留 —— 与组件上的 `MaxWalkSpeed`（每帧被重算）形成鲜明对比。

## 属性集的形态

`ATR_Movement` 是游戏的 GAS（GameplayAbilitySystem）属性集，即一个 `AttributeSet` 子类的实例。
场景里同时存在**几十个**实例（玩家 + 各种敌人 / NPC），必须挑出属于玩家的那一个 —— 判别方法见下。

属性是 `FGameplayAttributeData`：

```
struct FGameplayAttributeData {
    float BaseValue;
    float CurrentValue;
};
```

两个 float 相邻（差 `0x04`），属性之间间隔 `0x10`（含对齐）。

## 偏移来源与「差 8 字节」

偏移来自**代码生成属性表**：`0x14a069800` 区段，每项 `0x40` 字节，偏移字段在 **`+0x24`**。

⚠️ 这张表读出的偏移**整体比实际数据位置小 8 字节**。

判定过程（相邻校验）：

| 属性 | 表里声明 | 实际观测 |
|---|---|---|
| `MovementSpeedMultiplier` | `+0x90` | 实际值在 **`+0x98`**（原本 700.0） |
| `GravityScale` | `+0xf0` | `+0xf8` 处正是 1.2 —— 一个合理的重力倍率 |

两个独立样本都指向同一偏移差（`+8`），因此代码里统一用
`attributeDataOffset(a) = declaredAttributeOffset(a) + offsets::movementAttribute::dataShift`。

## 声明偏移表

| 属性 | 表里声明 | 实际数据（声明 + 8） | 说明 |
|---|---|---|---|
| `MovementSpeedMultiplier` | `+0x90` | `+0x98` | ★ **唯一经写入验证的属性** |
| `MovementFriction` | `+0xa0` | `+0xa8` | |
| `MovementFrictionMultiplier` | `+0xb0` | `+0xb8` | |
| `MovementRotation` | `+0xc0` | `+0xc8` | |
| `MovementRotationMultiplier` | `+0xd0` | `+0xd8` | |
| `MovementGravity` | `+0xe0` | `+0xe8` | |
| `GravityScale` | `+0xf0` | `+0xf8` | 实测约 1.2，用于反推偏移差 |
| `AirControl` | `+0x100` | `+0x108` | |
| `RollCooldown` | `+0x110` | `+0x118` | |
| `RollCharges` | `+0x130` | `+0x138` | |
| `Mass` | `+0x160` | `+0x168` | |
| `InteractionRange` | `+0x170` | `+0x178` | |

**只有 `MovementSpeedMultiplier` 的数据位置经过写入实验验证**（`+0x98` / `+0x9c`）。
其余各项的 `+8` 是由两个样本外推的，标记为未证实。

## 写入实验（`MovementSpeedMultiplier`）

| 尝试 | 结果 |
|---|---|
| 写组件 `MaxWalkSpeed`（`+0x234`） | 每帧被游戏从属性重算，**必然被覆盖** |
| 写属性集 `+0x90` | 能写、能保持，但那是**空槽位**（原本是 0）→ 无效果 |
| 写属性集 `+0x98` / `+0x9c` | 原本 700.0，改成 3000 后**游戏内移动明显变快**，且写入持久 ★ |

因此 `Speed` 模块的默认目标固定为 `MovementAttribute`，另两个选项只作对照排查用，
且目标暂不可用时**等待而不是换字段**（换字段＝每帧在游戏刚重算出的值上再覆写一次）。
「写入持久」还有一层后果：重新注入后字段仍停在上次写下的值上，模块必须记住
`原始值 / 写进去的值` 这一对，否则会把已放大的值当成原始基线再乘一遍 ——
见 [`../features/speed-jump.md`](../features/speed-jump.md)。

**必须同时写 Base 与 Current**：GAS 在下次聚合时可能用另一个值把改动盖回去。

## 如何找到属于玩家的那一个实例

属性集挂在 `AbilitySystemComponent` 上，ASC 又挂在 pawn（或 `PlayerState`）上。
所以沿 **Outer 链往上走**，哪一级正好是玩家 pawn，那个实例就是玩家的：

```
for depth in 1..8:
    outer = OuterPrivate(outer)
    if outer == playerPawn: 命中
```

实测**深度 1**（ASC 直接挂在 pawn 上）。全部扫描有成本（十万级对象），所以只在解析目标时做一次，
结果缓存在 `PlayerTarget::attributeSet` 里。

## 仍未解决

`MovementSpeedMultiplier` 的**当前值是 0**，写入 5.0 后游戏内**没有可感知的效果反馈**，
所以还不能确认它是否就是驱动移动速度的那个属性。

下一步应当在 `ATR_Movement` 里找**值呈现单位量级（1.0）**或随移动变化的属性，
用 `floats` 命令把整个属性块打出来逐个对照：

```
epsilon> attrmv              # 找出玩家的 ATR_Movement 并打印属性块
epsilon> floats <地址> 48    # 按 float 解读一段内存(个数是第二个位置参数)
epsilon> poke <绝对地址> <值> # 往任意地址写一个 float, 带回读与 8 次采样
```

`attrmv` 会自动打印 `+0x70` 起 80 个 float，并在每个属性声明偏移处标注属性名。
