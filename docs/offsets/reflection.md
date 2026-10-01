# UStruct / UField / FField / FProperty

代码：`offsets::uStruct` / `offsets::field` / `offsets::fieldClass` / `offsets::property`、
实现 `ue::Reflection`（[`Reflection.cpp`](../../src/payload/game/ue/Reflection.cpp)）

这一块是「运行时重建属性偏移」的基础设施。设计上它是**首选**的偏移来源（能自动适配游戏
更新），但**本构建上不可用** —— 原因与取证过程见
[../reverse/reflection-limits.md](../reverse/reflection-limits.md)。实际生效的是从二进制属性表
读出的静态偏移（见 [character-movement.md](character-movement.md)）。

## 布局（公开的 UE5 布局）

```
UField : UObject
    +0x28  UField*  Next

UStruct : UField
    +0x40  UStruct* SuperStruct
    +0x48  UField*  Children          （函数等子字段链）
    +0x50  FField*  ChildProperties   （属性链的第一个）
    +0x58  int32   PropertiesSize

FField
    +0x08  FFieldClass* ClassPrivate
    +0x20  FField*      Next
    +0x28  FName        NamePrivate

FFieldClass
    +0x18  FName        NamePrivate

FProperty : FField
    +0x30  int32   ArrayDim
    +0x34  int32   ElementSize
    +0x38  uint64  PropertyFlags
    +0x40  int32   Offset_Internal   ← 我们要的东西
```

## 内层类型（`FStructProperty` / `FObjectProperty`）

`FStructProperty::Struct` 与 `FObjectProperty::PropertyClass` 在 `0x78` 附近。
代码按 **`0x78` → `0x70` → `0x80`** 的顺序试，第一个能解出非空且非 `None` 的 `FName`
就算命中。只在类型名匹配（含 `StructProperty` / `ObjectProperty` / `ClassProperty` /
`InterfaceProperty`）时才会去读。

## 可信度与维护

| 项 | 状态 |
|---|---|
| `UStruct::SuperStruct` `+0x40` | ✅ 实测可用（类大小与继承链都读对了） |
| `UStruct::PropertiesSize` `+0x58` | ✅ 实测可用 |
| `UStruct::ChildProperties` `+0x50` | ❌ **本构建上不成立** |
| `FField::Next` `+0x20` | ⚠️ **存疑** |
| `FField::NamePrivate` `+0x28` | ⚠️ **存疑** |
| `FProperty::Offset_Internal` `+0x40` | ✅ 实测可用（`props` 能读出合理偏移） |

⚠️ `fieldNext(0x20)` 与 `fieldName(0x28)` **至少有一个是错的** —— 这是尚未解决的结论。
三种判据（值落在模块映像外 / 能解出合法 FName / 沿 `Next` 连续自洽）全部无法在这个构建上
同时成立。见 [../reverse/reflection-limits.md](../reverse/reflection-limits.md)。

## 自愈扫描

`propertiesOf()` 在静态 `ChildProperties` 偏移失败时会现场找回属性链起点：

1. 在 `0x20`..`0x98` 之间每 8 字节试一个槽
2. 槽值必须是指针，且该指针在 `FField::NamePrivate` 偏移处能解出**非空** FName
3. 走出来的链必须非空
4. 在所有候选里优先采信「链里含**已知引擎属性名**」的那个 —— 这是最强判据，垃圾数据不可能
   偶然凑出 `MaxWalkSpeed` / `JumpZVelocity` / `GravityScale` / `AirControl` /
   `MaxAcceleration` / `RelativeLocation` / `RootComponent`
5. 没有强候选时，退到「含 `*Property` 类型名的条目最多」的那个

探到的偏移缓存在 `lastChildPropsOffset_`。缓存偏移读不出来时退回重新探测。
枚举 Actor 时 `propertiesOf` 会被调用几十次，缓存这一步是必需的。

> ⚠️ 判据演进史里有两次踩坑（判据太弱导致「`MaterialLayersFunctionsTree`」这种纯噪声），
> 详见 [../reverse/reflection-limits.md](../reverse/reflection-limits.md)。

## 只读诊断原语

`Reflection` 对外暴露两个 FField 层的原语，避免命令层自己算偏移：

| 方法 | 作用 |
|---|---|
| `fieldNameAt(field)` | 读 `FField::NamePrivate` 并解析成字符串 |
| `fieldNextOf(field)` | 读 `FField::Next` |

属性链长度上限 `4096`，并且遇到自环立即停止。
