# 玩家角色判定

> **依据**：`src/payload/game/dungeons2/Player.h/.cpp`（判定的**唯一出处**）、
> `CommandServer::cmdPlayer` 的诊断输出、`MovementResolver` 的解析流程。
> 偏移见 [`../offsets/character-movement.md`](../offsets/character-movement.md) 与
> [`../offsets/world.md`](../offsets/world.md)。

---

## 实测类名

真机关卡 actor 列表里出现的相关类名：

| 类名 | 是什么 |
|---|---|
| **`BP_AlexCharacter_C`** | ★ **玩家角色本体** |
| `BP_GameplayPlayerController_C` | 控制器（**不是 pawn**） |
| `BP_WolfCharacter_C` | 同伴 / 召唤物 |
| `BasePlayerState` | 玩家状态（**不是 pawn**） |
| `Mock*` | 游戏的假玩家（教程 / 演示） |
| `Mob*` | 怪物 |

⚠️ 早期代码只匹配 `PlayerCharacter` / `PlayerPawn` ——
**这个构建两个都不存在**，所以恒不命中。正确做法是从实测数据里认这些具体名字。

---

## ★ 必须按优先级挑，不能取第一个命中的

实测 actor 列表里的索引：

```
[480] BP_GameplayPlayerController_C     ← 类名里有 "Player", 但它是控制器
[483] BP_AlexCharacter_C                ← 真正的玩家角色
```

控制器排在玩家角色**之前**。所以「遍历 actor，命中就返回」会挑到控制器，
后续 `findProperty(pawnClass, "CharacterMovement")` 自然问不到东西。

判据因此做成一张**有序优先级表**：

| 子串 | rank |
|---|---|
| `AlexCharacter` | 0 |
| `BP_SteveCharacter` | 0 |
| `DungeonsCharacter` | 1 |
| `PlayerCharacter` | 2 |
| `PlayerPawn` | 3 |

遍历全部候选，取 **rank 最小** 的那个；rank 相同时保留先遇到的。

---

## 排除项

以下子串直接判为「不是本地玩家」（返回 -1）：

```
Mock  Mob  Projectile  Controller  PlayerState  HUD  Wolf
```

其中 `Controller` / `PlayerState` / `HUD` 在 UE 里本来就不是 pawn，
但它们的**类名里可能带 "Player"**，必须显式排掉，否则会把控制器当成角色。

`Mock` 是游戏的假玩家（教程 / 演示场景会出现），`Wolf` 是同伴 / 召唤物 ——
两者都会通过「类名里有 Character」的宽泛判断。

---

## 判定规则的唯一出处

规则实现在 [`src/payload/game/dungeons2/Player.h`](../../src/payload/game/dungeons2/Player.h) /
`Player.cpp`：

```cpp
int  playerClassRank(std::string_view className) noexcept;   // -1 = 不是玩家
std::string_view playerClassRulesText() noexcept;            // 供命令输出
uint64_t findLocalPlayerPawn(ue::Engine& engine);            // 通关卡 Actor 挑
```

**命令层与功能模块都必须调用它，不允许各自复刻一份。**

> 历史教训：`CommandServer::cmdPlayer` 曾经自己复刻了一遍排除项与命中项。
> 两处规则一旦漂移，`player` 命令给出的结论就是**误导性**的 ——
> 而这条命令的存在意义恰恰是「把类名实情摊开」。
> 现在 `cmdPlayer` 直接调用 `playerClassRank()`，输出与模块行为保证一致。

---

## 找移动组件

拿到 pawn 之后，取它身上的 `CharacterMovement` 组件，**两条路，按优先级**：

1. **走反射**问 `UPawn::CharacterMovement`（对象指针属性）。
   比在对象表里按名字猜稳 —— 同一个 pawn 可能挂着多个 `MovementComponent` 子对象。
2. **退路**：在对象表里找 `class` 含 `CharacterMovementComponent` 且
   `Outer == pawn` 的实例。只在主路径失败时才走。

两条都失败时记录
`no CharacterMovement component on pawn <类名>`，并按失败退避重试。

---

## 找玩家的属性集

见 [`movement-attributes.md`](movement-attributes.md) 的「如何挑出属于玩家的那一个实例」：
沿 `Outer` 链上升匹配玩家 pawn，实测深度 1。

找不到**不算失败** —— 移动组件的偏移仍然可用，只是走不了「改属性」这条真正生效的路。
这种情况下会打一条警告，避免用户以为模块没生效却查不到原因。

---

## 诊断命令 `player`

```
epsilon> player
```

输出关卡里所有「长得像玩家」的 Actor（类名含 `Character` / `Pawn` / `Player`），
并标出模块会挑中哪一个：

```
=== player candidates ===
world          : 0x... World
PersistentLevel: 0x... (972 actors)
  [  480] 0x...  BP_GameplayPlayerController_C        ...
  [  483] 0x...  BP_AlexCharacter_C                   <== MODULE WILL USE THIS

pawn-like actors : 7
module matches   : 1
=> module would pick [483] BP_AlexCharacter_C (0x...)
```

匹配数为 0 时会打印规则说明并要求人工确认实际玩家类名 ——
**这条命令存在的意义就是把假设摊开**，而不是让人去猜为什么「开了没反应」。

若 actor 列表本身读不出来（`Actors` 数组读失败），会自动在同一趟里跑
`scanForActorArray` 把候选槽摊出来，免得还要人工搬地址再跑一次 `scanlevel`。

> 该命令的输出保持 ASCII：覆盖层字体只有 ASCII 字形，而结果很可能需要对着面板看。
