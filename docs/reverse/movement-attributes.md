# 移动/速度的真正来源：GAS 属性集 `ATR_Movement`

> **依据**：`src/payload/game/dungeons2/Movement.h/.cpp` 与
> `MovementResolver.cpp` 的注释、`CommandServer` 的 `attrmv` / `poke` / `mvset` 命令实现，
> 以及 `analysis/docs/*` 里已有的逆向笔记。
> **偏移数字本身在 [`../offsets/movement-attributes.md`](../offsets/movement-attributes.md)，
> 本文只讲结论、过程与未决问题。**

---

## 一句话结论

**速度不能在 `UCharacterMovementComponent` 上改。** 真正生效的是玩家 pawn 身上那个
GAS 属性集 `ATR_Movement` 里的 `MovementSpeedMultiplier`（Base / Current 一对）。

---

## 为什么不能写 `MaxWalkSpeed`

| 观测 | 数据 |
|---|---|
| 写入 `MaxWalkSpeed`（`+0x234`）为 130 | 之后连续 8 次采样**全是 100**（40 ms 间隔） |
| 同样方式写 `MaxAcceleration`（`+0x288`）为某值 | 8 次采样**全部保持** |

第二行是关键：它说明**写路径本身没有问题**（不是地址错、不是页保护、不是写被丢弃），
而是 `MaxWalkSpeed` 这个字段被游戏自己每帧重算。

每帧补写压不住 —— 写进去的瞬间就被覆盖，表现为「面板上的数值在闪，角色却没变快」。

反汇编能看到一个每帧同步函数把 `+0x1018` 复制进 `+0x234`，
但**实测 `+0x1018` 恒为 0**，写入它也不影响 `MaxWalkSpeed` ——
「游戏自己的速度源在 `+0x1018`」这个假设**已被证伪**，真实数据流仍未找到。

---

## 发现 `ATR_Movement` 的线索

反汇编里出现了 `OnRep_MovementSpeedMultiplier`，而它的 `Outer` 是 `Class :: ATR_Movement`。

`A` 前缀 = Actor，`TR_` = 属性集（Attribute Set）的命名习惯 —— 也就是说游戏把这个倍率
定义在**一个 GAS 属性集**上，而不是移动组件上。属性集实例挂在
`AbilitySystemComponent` 上，ASC 又挂在玩家 pawn 上。

---

## 偏移来源与「表偏移整体差 8 字节」

属性偏移来自**代码生成参数表**：`0x14a069800` 区段，每项 `0x40` 字节，
偏移字段在 **`+0x24`**。

⚠️ 这张表读出的偏移**整体比实际数据位置小 8 字节**。

判定依据是**相邻校验**（两个独立样本指向同一个偏移差）：

| 属性 | 表里声明 | 实际观测 |
|---|---|---|
| `MovementSpeedMultiplier` | `+0x90` | 实际值在 `+0x98`，原本是 700.0 |
| `GravityScale` | `+0xf0` | `+0xf8` 处正是 **1.2** —— 一个合理的重力倍率 |

单个样本可能是巧合，两个不同位置的样本都差 8 字节就不是了。

另外注意：**这张表的项布局与 `UCharacterMovementComponent` 那张不同**
（后者偏移字段在 `+0x2c`）。不要混用同一套解析代码。

---

## 三个候选目标的实测对照 ★

这是整条线索里最有价值的一组对照实验：

| 写哪里 | 结果 |
|---|---|
| 移动组件 `MaxWalkSpeed`（`+0x234`） | 写入后被游戏每帧重算覆盖 —— **无效** |
| 属性集 `+0x90` | 能写能保持，但那是**空槽位**（原本值是 0）—— **无效** |
| 属性集 `+0x98` / `+0x9c` | 原本 700.0，改成 3000 后**游戏内移动明显变快** —— ★ **生效** |

第三行还验证了两件事：

- 写入**持久**：之后 8 次采样全部保持
- **跨注入保留**：重新注入后值仍在

### 必须同时写 Base 与 Current

`FGameplayAttributeData` 是 `{BaseValue, CurrentValue}` 一对，相邻 4 字节。
只写其中一个的话，GAS 在下次聚合时可能用另一个值把它盖回去 ——
实测两个一起写才能稳定生效。`MovementAttributeSet::write()` 就是这么做的。

---

## 如何挑出属于玩家的那一个实例

场景里 `ATR_Movement` 实例有**几十个**（玩家 + 各种敌人 / NPC），必须挑对。

判别方式：**沿 `Outer` 链往上走**，看哪一级正好是玩家 pawn。

```
玩家 ATR_Movement → ASC → 玩家 pawn        ← 实测深度 1
敌人 ATR_Movement → ASC → 敌人 AIController → …  （走不到玩家 pawn）
```

`ATR_Movement` 挂在 `AbilitySystemComponent` 上，ASC 又挂在 pawn（或 `PlayerState`）上，
所以链上一定能撞到玩家 pawn —— 实测**深度 1**，即 ASC 直接挂在 pawn 上。

全表扫描有成本（十万级对象），所以只在解析目标时做一次，结果缓存在
`PlayerTarget::attributeSet` 里。

---

## 仍未解决

**`MovementSpeedMultiplier` 的当前值是 0**，写入 5.0 后**游戏内没有可感知的效果反馈**，
所以还不能确认它是否就是驱动移动速度的那个属性。

下一步方向：在 `ATR_Movement` 里找**值呈现单位量级（1.0）**或**随移动变化**的属性，
用 `floats` 把整个属性块打出来逐个对照。

可用命令：

```
epsilon> attrmv                # 找出玩家的 ATR_Movement 并打印属性块
                               # (+0x70 起 80 个 float, 每个属性声明偏移处标名字)
epsilon> floats <地址> 48      # 按 float 解读一段内存(个数是第二个位置参数)
epsilon> poke <地址> <值>      # 往任意地址写一个 float, 带回读与 8 次采样
epsilon> mvset <字段> <值>     # 写组件字段(支持裸偏移 +0x230)
```

`attrmv` 末尾还会自动做一次「写 `+0x90` 并采样 8 次」的对照实验，
用来判断某个槽位是否被 GAS 在聚合时重算。

---

## 对功能模块的影响

`Speed` 模块因此把默认目标设为**属性集**，另两个目标只作对照排查用，
并且在目标不可用时**明确告知**（不静默退回）。

`Jump` 改的是 `JumpZVelocity`（组件上的 `+0x1a4`，**已验证**读出 110）——
跳跃是另一条通路，不受上述重算问题影响。

详见 [`../features/speed-jump.md`](../features/speed-jump.md)。
