# Speed / Jump 两个模块的现状

代码：[`module/impl/Speed.h`](../../src/payload/feature/module/impl/Speed.h) ·
[`module/impl/Jump.h`](../../src/payload/feature/module/impl/Jump.h)

> ⚠️ **现状：加速已实测生效，大跳尚未在真机上验证成功。**
> 默认配置里两个模块都是 `enabled: false` —— 在找到游戏真正读取速度的路径之前，
> 不要再默认启用它们。

---

## Speed

### 三个可选目标

`ApplyTo` 枚举（落盘名与设置候选名一致）：

| 目标 | 改的是 | 结果 |
|---|---|---|
| **`MovementAttribute`**（默认） | GAS 属性集 `ATR_Movement` 的 `MovementSpeedMultiplier` | ★ **实测生效**，写入持久 |
| `MaxWalkSpeed` | `UCharacterMovementComponent` 上的 `+0x234` | 每帧被游戏从属性重算，必然被覆盖 |
| `SpeedMultiplier` | 组件上的 `MovementSpeedMultiplier`（游戏自己的复制属性） | **本构建上拿不到偏移**（反射不可用，且不在属性表里） |

后两个**保留下来只为对照排查**，不要作为默认。

目标不可用时会**退回 `MaxWalkSpeed` 并且明确告知** —— 静默退回会让用户以为改的是别的字段，
排查时会很困惑。

### 走过的弯路

| 尝试 | 现象 | 结论 |
|---|---|---|
| 写组件 `MaxWalkSpeed`（`+0x234`） | 写入 130 后连续 8 次采样全是 100 | 每帧被重算，每帧补写压不住 |
| 写属性集 `+0x90` | 能写能保持 | 那是**空槽位**（原本是 0），无效 |
| 写属性集 `+0x98` / `+0x9c` | 原本 700.0，改成 3000 后**游戏内明显变快** | ★ 真正生效的一对 |

（另一个对照证据：同样方式写 `MaxAcceleration` `+0x288` 却完全保持 —— 说明写路径本身没问题，
是 `MaxWalkSpeed` 这个字段被重算。）

属性表读出的偏移**整体差 8 字节**（表里标 `+0x90`，实际在 `+0x98`），靠相邻校验定下来 ——
详见 [../offsets/movement-attributes.md](../offsets/movement-attributes.md)。

### 实现要点

- 基线在**启用瞬间**读取，只读一次。之后每帧用 `基线 × 倍率` 作为目标值，
  避免「改了设置后在已放大值上再乘一次」的累积放大
- 倍率上限 `10.0`。10 倍速在多数游戏里已经会让角色直接穿过碰撞体 ——
  那不是「加速」而是「送死」
- 基线为 0 时用 `1.0` 兜底。否则乘出来恒为 0，角色会被钉在原地 ——
  那比「没生效」糟糕得多
- 目标切换 / 换关卡（偏移重新解析）都会重取基线
- 写入时**同时写 Base 与 Current**，只写一个会被 GAS 聚合时的另一个值盖回去
- 浮点比较留容差（`0.001`）：引擎可能做一次 float 往返，严格相等会导致每帧都判定「被改了」
  从而一直写
- 连续重写超过 3 秒会打一条警告 —— 那说明游戏自己在覆盖这个字段，**该去挂钩游戏自己的
  读取/计算路径**，而不是加倍努力地写

---

## Jump

改 `UCharacterMovementComponent::JumpZVelocity`。

UE 的跳跃初速度由 `JumpZVelocity` 决定（调用链 `DoJump -> CharacterMovement->JumpZVelocity`）。
跳跃高度与初速度的**平方**成正比，所以把 `JumpZVelocity` 乘 N 倍，高度大约是 N² 倍。

- 实测二进制里存在 `JumpZVelocity` 反射名，以及 `GetMaxJumpHeight` /
  `GetMaxJumpHeightWithJumpTime`（后者是 UE 5.6 `UCharacterMovementComponent` 自带的接口）
- 偏移 `+0x1a4` **已验证**（读出 110，与 UE 默认 420 不同，说明读的确实是本游戏调过的值）

### 为什么「每帧写」而不是「写一次」

跳跃过程里引擎会按曲线调整 Z 速度，而 `JumpZVelocity` 也可能被游戏自己的逻辑
（或从服务端下发的复制属性）覆盖。一次性写入很容易在某次状态更新后失效，
表现为「有时生效有时不生效」。

改成每帧**仅在值被改掉时**补写：大部分帧只是一次读取比对，不产生写入，
对帧时间与内存写痕迹都很轻。

### 关闭时还原

不还原的话，关掉模块之后跳跃高度会停在放大后的值上 —— 用户会以为「关了没用」。

---

## 为什么默认关闭

`Speed` / `Jump` 两个模块在真机上**尚未成功生效**：

- `MaxWalkSpeed` 写入后被游戏每帧重算覆盖（实测写入 130 后连续 8 次采样全是 100，
  每帧补写压不住）
- 反汇编发现一个每帧同步函数把 `+0x1018` 复制进 `+0x234`，但实测 `+0x1018` **恒为 0**，
  写入它也不影响 `MaxWalkSpeed` —— 该假设**已被证伪**，真实数据流仍未找到
- 两次空指针崩溃（`0xC0000005` 读，`fault address = 0x0`）都发生在 Speed 模块持续写该字段
  期间。**因果关系未证实，但不能排除**

所以默认配置里两个模块都保持 `enabled: false`
（`~/.epsilon/ext/dungeons2/configs/*/modules/*.json`）。

**在找到游戏真正读取速度的路径之前，不要再默认启用它们。**
更可能正确的方向是**挂钩**游戏自己的读取/计算路径，而不是反复写被重算的字段。

---

## 相关文档

- 偏移本体：[../offsets/movement-attributes.md](../offsets/movement-attributes.md)、
  [../offsets/character-movement.md](../offsets/character-movement.md)
- 实测过程与未决问题：[../reverse/movement-attributes.md](../reverse/movement-attributes.md)
- 模块框架本身：[module-framework.md](module-framework.md)
