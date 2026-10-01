# UWorld / ULevel / Actor / TArray

代码：`offsets::world` / `offsets::level` / `offsets::array`、
实现 `ue::WorldView`（[`World.cpp`](../../src/payload/game/ue/World.cpp)）

## TArray\<T\>

UE 的动态数组，布局固定为三个连续字段：

```
+0x00  T*      Data
+0x08  int32   Num
+0x0C  int32   Max
       ── 头部共 0x10 字节
```

读入时做的形态校验（`readArray`）：

- `0 <= Num <= 10'000'000`
- `Num > 0` 时 `Data != 0`（两者必须同生同灭）

## FString / FName

同样是三字段的连续布局，形态与 `TArray` 一致：

```
+0x00  TCHAR*  Data      （UTF-16）
+0x08  int32   Num       （字符数，含结尾 NUL）
+0x0C  int32   Max
```

代码里归 `offsets::stringData`。只被 `CommandServer::renderValue` 这类**只读诊断渲染**使用 ——
按类型把属性值打成可读文本。

## UWorld

| 字段 | 偏移 | 可信度 |
|---|---|---|
| `PersistentLevel` | `+0x30` | ⚠️ **已知值但未独立复核** —— 它是反射不可用时的退路，没有实测数值佐证 |

`WorldView::currentWorld()` 优先用已定位的 `GWorld`；取不到时退到对象表里找第一个类名为
`World` 的实例。

> ⚠️ 只采信**通过类名校验**（`GlobalSlot::verified`）的 `GWorld` 槽。
> 早期代码看的是 `value != 0`，那会让「读到了指针但类名对不上」的槽也被当成世界 ——
> 那正是游戏更新后最可能出现的情形。

## ULevel

| 字段 | 偏移 | 可信度 |
|---|---|---|
| `Actors` | **`+0xA0`** | **已验证** |

常见 UE5 版本把这个字段放在 `0x98` 一带（有的资料写 `0x40`），但**本构建都不是**。

实测判定过程：属性链反射对这个构建失效，拿不到反射值，于是直接扫 `ULevel` 对象体上符合
TArray 形态的槽，得到

```
+0xa0 -> { data, num = 972, max = 1364 }   首元素类名 WorldSettings
```

`WorldSettings` 在 UE 里**恒定**占据 `ULevel::Actors` 的索引 0，所以可以判定 `+0xa0` 就是
`Actors`。

`actors()` 会优先走反射问 `Actors`，失败才用这个已知值；用的是哪个来源会写进
`LevelInfo::offsetSource` 并由 `world` 命令打印出来。

## 候选但未验证

以下偏移在排查过程中出现过，**尚未验证**，因此没有进入代码：

| 字段 | 候选偏移 | 说明 |
|---|---|---|
| `AActor::RootComponent` | `+0x1B8` | 常见布局，未在本构建上核对 |
| `USceneComponent::RelativeLocation` | `+0x160` | 同上；UE5 的 `FVector` 是 double |

需要读 Actor 坐标时**先按 [README.md](README.md) 的四步法验证**，再把它加进 `Offsets.h`。

## 只读诊断窗口

`scanlevel <ULevel 地址>` 在 `0x20`..`0x300` 之间每 8 字节按 TArray 形态找候选，
用「首元素是不是合法 UObject」把碰巧长得像 TArray 的槽筛掉，把候选全列出来人工判定。
比继续猜常量可靠。
