# 本构建的反射为什么不可用（以及绕行方案与它的局限）

> **依据**：`src/payload/game/ue/Reflection.h/.cpp` 的注释与实现；
> `src/payload/CommandServer.cpp` 里 `dumpStructPointerSlots` / `probeFFieldNameOffset` /
> `findPropertyDirect` 的注释与判据演进记录；[AGENTS.md](../../AGENTS.md)「这个构建的 UE5 反射布局被改过」；
> 以及本文 §1 **新做的二进制字符串扫描**（可复现，方法写在表下）。
> **对象**：`Dungeons-Win64-Shipping.exe` 1.1.1.0 / UE 5.6.1。

---

## 0. 结论速览

| 结论 | 依据 |
|---|---|
| `SuperStruct` 与 `ChildProperties` **这两个反射名字符串在本构建里根本不存在**（只有 `Children`） | §1 全 exe 字符串扫描：`SuperStruct` = 0 命中，`ChildProperties` = 0 命中，`Children` = 78 命中 |
| 走 `ChildProperties -> Next -> NamePrivate` 的属性链遍历**在本构建上不可用** | `Reflection::propertiesOf()` 对绝大多数类返回空（`props <类名>` 通常一条属性都没有） |
| 三种判据（值在模块映像外 / 能解出合法 FName / 沿 `Next` 连续自洽）**全部失败** | §3；含"`MaterialLayersFunctionsTree` 噪声"与"没有候选能连续自洽"两次记录 |
| **未决结论**：`fieldNext(0x20)` 与 `fieldName(0x28)` **至少有一个是错的** | §4 与 `dumpStructPointerSlots` 的自动探测输出 |
| `UStruct` 里的 `SuperStruct` 与 `PropertiesSize` **是对的** | `class` 命令能报出正确的类大小与父类，但"本类属性数"恒为 0 —— 说明错的是**属性链起点**那一个字段 |
| 后果：属性偏移**不能**靠反射拿，只能从二进制的代码生成参数表读 | [property-verification.md](property-verification.md)（方法）、[movement-attributes.md](movement-attributes.md)（实例） |
| 设计口径：这些偏移常量**只认实测基线，不做多方案探测** | `Reflection.h` 顶部注释；游戏更新后布局若变，`props` 的输出会明显不合常理，改常量即可 |

---

## 1. 证据一：字符串表里没有这两个反射名（全 exe 扫描）

**方法**（可复现）：按 8 MiB 分块 + 64 字节重叠，对整个 exe（**210,557,248 字节**）做 ASCII 子串计数，
统计出现次数与首次文件偏移。脚本是纯标准库，一次几秒。

| 字符串 | 命中次数 | 首次文件偏移 |
|---|---|---|
| `SuperStruct` | **0** | — |
| `ChildProperties` | **0** | — |
| `Children` | 78 | `0x8e71cfd` |
| `NamePrivate` | 1 | `0x992cf3e` |
| `Offset_Internal` | 0 | — |
| `MaxWalkSpeed` | 2 | `0x973cbc8` |
| `JumpZVelocity` | 2 | `0x973cae0` |
| `GetMaxJumpHeight` | 2 | `0x9739038` |
| `MovementSpeedMultiplier` | 3 | `0x9ef7076` |
| `OnRep_MovementSpeedMultiplier` | 1 | `0xa068d58` |
| `GameplayAttributeData` | 1 | `0x9b999b8` |
| `AttributeSet` | 12 | `0x955a3f6` |
| `UPlayerCharacterMovementComponent` | 1 | `0xbd0a474` |
| `SWMovementComponent` | 1 | `0x9ef1698` |
| `GASCharacterMovementComponent` | 0 | — |
| `BP_AlexCharacter` | 1 | `0xa3b6a94` |
| `PlayerCharacter` | 19 | `0x97f7503` |
| `PlayerPawn` | 9 | `0x93cea79` |
| `PersistentLevel` | 2 | `0x8e71b50` |
| `WorldSettings` | 6 | `0x956f398` |
| `ATR_Movement` | **0（exe）** | 但在 `Content\Paks\global.ucas` 命中 **2** 处 |

**怎么读这张表**：

- `SuperStruct` / `ChildProperties` 是 UHT 生成的、属于**引擎侧 C++ 反射**的名字。
  如果这个构建用的是公开的 UE5 `UStruct` 布局，它们应当像 `MaxWalkSpeed` 一样出现在 exe 里。
  两个都 0 命中、而 `Children` 有 78 命中 —— 这**直接支持**"它用的是改过的 `UStruct` 布局（只有 `Children`）"。
- **反向警戒**：`ATR_Movement` 在 exe 里 0 命中，却在 `global.ucas` 里命中 2 处 →
  "exe 里没有这个名字"**不等于**"运行时不存在这个名字"（资产侧定义的名字会在运行时进名字池）。
  所以这条证据对"引擎反射名"有效，对"游戏自定义类名"要另找依据（见 [movement-attributes.md](movement-attributes.md)）。
- `PlayerCharacter` / `PlayerPawn` 各有 19 / 9 处命中，但**这两个类的实例在关卡 actor 列表里不存在**——
  "名字在二进制里"与"关卡里有这个类的对象"是两件事（见 [player-detection.md](player-detection.md)）。

---

## 2. 预期布局 vs 实测现象

代码里钉死的布局（`Reflection.h`，实测基线 UE 5.6.1）：

```text
UStruct : UField
    +0x40  UStruct*   SuperStruct
    +0x48  UField*    Children          (函数等子字段链)
    +0x50  FField*    ChildProperties   (属性链的第一个)
    +0x58  int32      PropertiesSize
UField : UObject
    +0x28  UField*    Next
FField
    +0x20  FField*    Next
    +0x28  FName      NamePrivate
FProperty : FField
    +0x30  int32      ArrayDim
    +0x34  int32      ElementSize
    +0x38  uint64     PropertyFlags
    +0x40  int32      Offset_Internal   ← 我们要的东西
```

实测现象（`class <类名>` 命令的输出对照）：

| 字段 | 命令输出 | 判断 |
|---|---|---|
| `PropertiesSize` `+0x58` | 类大小能报出来且合理（如 `CharacterMovementComponent` = 4048 字节） | **对** |
| `SuperStruct` `+0x40` | 父类能报出来且合理 | **对** |
| `ChildProperties` `+0x50` | `本类属性数 : 0`（几乎总是 0） | **错 / 不可用** |

也就是说：`UStruct` **头部**的几个字段偏移是对的，错的只可能是**属性链起点**这个字段，
或者 `FField` 一侧的 `Next` / `NamePrivate`。`props` 命令在遇到"属性数为 0"时会自动
调 `dumpStructPointerSlots` 取证——那不是调试残留，是这条结论的现场记录方式。

**设计口径**（`Reflection.h` 原话）：*"这些偏移**只认实测基线, 不做多方案探测**。游戏更新后
如果布局变了, `props` 命令输出的偏移会明显不合常理(值荒谬或属性名读不出来), 那时改这里的
常量即可 —— 比维护一套自动探测更可预测。"* 因此**不要**再加"六选一投票"之类的机制
（`PLAN.md` §3 记录过它被删掉的原因）。

---

## 3. 三种判据的失败记录

`dumpStructPointerSlots` 的注释里写着判据演进史（**两次踩坑**）：

| # | 判据 | 做法 | 实测结果 | 为什么不够 |
|---|---|---|---|---|
| 1 | **值落在模块映像范围外** | 认为"指向堆的指针就是候选" | 包名字符串指针**同样满足** | 太弱：任何堆指针都过 |
| 2 | **某个偏移能解出一个像标识符的 FName** | 对候选指针按 `FField*` 解 `NamePrivate` | 给出了 `MaterialLayersFunctionsTree`，**与 `CharacterMovementComponent` 毫无关系，是纯噪声** | 仍然太弱：垃圾值落在 FNamePool 范围内是**可能**的，解出的字符串看上去还挺"像名字" |
| 3 | **沿 `Next` 连续自洽** | 见下 `chainScore` | **没有任何候选能连续自洽**（最好只有 1 跳） | 这是可靠的不变量，但它失败了 —— 于是问题被归到"`Next` 或 `NamePrivate` 的偏移错了" |

判据 3 的实现（`chainScore`，最多 6 跳）：

```text
对每个候选槽 (0x20..0x98 step 8, 排除模块映像内的值):
  对每个候选名字偏移 (0x18..0x48 step 4):
      沿链最多走 6 跳, 每跳要求:
        idx = *(i32*)(f + nameOff);  idx > 0
        resolve(idx) 非空
        名字里不含 '/'        (含 '/' 的是包名/路径, 不是字段名)
        next = *(u64*)(f + layout.fieldNext); 防自环
      记分 = 连续成功的跳数
取最高分; > 1 才输出, 否则打印"没有候选能连续自洽(最好只有 N 跳) —— 换个大类再试"
```

注释原话：*"真正可靠的不变量是**链的自洽性**：`FProperty` 是一条 `Next` 链，若偏移 X 是真正的
`NamePrivate`，那么沿链每个节点在 X 处都应解出合法名字。垃圾指针凑不出这种'连续多跳都自洽'的性质。"*
判据 3 是对的，结论是"这个构建上确实没找到"。

> 附带记录：`dumpStructPointerSlots` 的自动探测有**次数预算**（`fieldProbeBudget_ = 1`）——
> 一次 `props` 会遍历整条继承链（9 个类），每个类都探一遍既慢又刷屏，只对第一个类做一次。
> 这是为什么"换个大类再试"是有效操作：想看另一个类，得先单独 `props <那个类>`。

---

## 4. 未决结论：`fieldNext(0x20)` 与 `fieldName(0x28)` 至少有一个是错的

这是本构建反射问题的**核心未决点**，原文记录在 `CommandServer.h` 的 `probeFFieldNameOffset` 注释里：

> 属性链之所以读不出来, 只剩一个可能 —— `FField::NamePrivate` 的静态偏移(+0x28)对这个构建不对。

以及 `findPropertyDirect` 的注释：

> 本构建的 `UStruct` 布局被改过, 传统的 `ChildProperties`/`Next` 链式遍历三种判据都失败了…
> **`fieldNext(0x20)` 与 `fieldName(0x28)` 至少有一个是错的。**

**为什么不能进一步定位**：两个未知量互相耦合。要定 `NamePrivate` 的偏移，需要一个
"确实是一个 `FField`"的指针；而"确实"这个判断本身又依赖沿 `Next` 链走 —— `Next` 的偏移
同样可疑。所以只能靠**外部锚点**。

**目前唯一的旁证**：`FField::ClassPrivate (+0x08)` → `FFieldClass::NamePrivate (+0x18)`
这条链"已在实测中确认"（`Reflection.cpp::walk` 注释原话）。它**不依赖** `UStruct` 布局，
所以属性类型名（`FloatProperty` / `StructProperty` …）是能读出来的 —— 这也是为什么
`findPropertyDirect` 的启发式还能用得上一点类型信息。

**下一步该怎么做**：`probeff <FField 地址>` 命令就是为这件事准备的：

```text
probeff <一个已知是 FField 的指针>
  从 +0x18 到 +0x48 每 4 字节试一次, 打印 idx 与 resolve(idx)
  判据: 哪个偏移解出的是**有意义的名字**(而不是空/"None") —— 通常真偏移只有一两个候选,
        且解出的名字是 "MaxWalkSpeed" 这种一眼能认的
  ⚠️ 判据要严: 垃圾值恰好落在 FNamePool 范围内并解出字符串是可能的, 所以命令会把**所有**
     候选都打出来, 由人按上下文判断
```

已知是 `FField` 的指针从哪来：`props <类名>` 在属性数为 0 时输出的槽扫描里，
"能解出名字候选"的那些槽指向的地址（`dumpStructPointerSlots` 末尾会提示
"用 `mem va=<该槽指向的地址>` 进一步核对"）。

---

## 5. 自愈扫描（`Reflection::propertiesOf`）

静态偏移失效时的现场找回逻辑：

| 步骤 | 内容 |
|---|---|
| 触发条件 | ① 已缓存的 `lastChildPropsOffset_` 读空 或 ② 静态 `+0x50` 走出的链为空 |
| 候选槽 | `0x20 .. 0x98`，步长 8，**跳过 `+0x50`**（那一个已经在上面试过） |
| 单槽前三道闸 | 值是像样的指针（非 0）→ `cand + layout.fieldName` 处能解出**非空 FName** → `walk()` 出非空链 |
| 强判据 | 链里出现**已知引擎属性名**之一：`MaxWalkSpeed`、`JumpZVelocity`、`GravityScale`、`AirControl`、`MaxAcceleration`、`RelativeLocation`、`RootComponent` → `strong = true`。注释原话："这是最强的判据, 因为它不可能由垃圾数据偶然凑出" |
| 选优 | 有 `strong` 的候选里取**链最长**的；没有 `strong` 就退到"链里有 `*Property` 类型名且链最长"；都没有 → 返回空 |
| 缓存 | 命中后写 `lastChildPropsOffset_`，之后所有 `propertiesOf` 直接用这个偏移（否则每次调用都要试十几个候选，枚举 Actor 会被拖慢） |
| 对外可见 | `props` 输出 `属性链起点: 自愈探到 +0x?? (静态常量是 +0x50) -> 建议固化` |

**口径不一致（以代码为准）**：`Reflection.cpp` 的注释写"自愈扫描要试 **13** 个候选槽"，
但循环 `for (probe = 0x20; probe <= 0x98; probe += 8)` 覆盖 **16** 个槽位、跳过 `+0x50`，
即最多 **15** 个候选。数字以代码为准。

**现状**：维护记录里**没有**一次"自愈成功并固化了新偏移"的实例 —— 目前所有在用的属性偏移
都来自代码生成参数表（[property-verification.md](property-verification.md)）。
因此这条自愈逻辑应视为**未证实生效**（未证实：它可能在某个类上成功过，但没有留下记录）。

---

## 6. `findPropertyDirect`：绕开 `FField` 链的思路与它的局限

命令：`findprop <类名> <属性名>`（别名 `fp`）。思路的核心是一条**物理事实**：

> `UClass` 对象内部**必然**存着它每个属性的 `FName` 索引，因为引擎自己也要按名字查属性。

于是反过来做，完全不依赖 `ChildProperties` 的位置、也不需要猜 `FField::Next` 的布局：

| 步骤 | 做什么 | 关键代码事实 |
|---|---|---|
| 1 | 在名字池里**顺序**找属性名，拿到它的 `FName` 索引 | 走块 0 的条目链，最多 20000 条；索引 = `byteOffset / 2`（因为索引里存的是右移一位后的偏移）；拿到后立刻**反查一次**自检 |
| 2 | 扫 `UClass` 对象体，找哪个位置存着这个索引 | 范围 `0x100..0x2000` 步长 4，最多 12 个命中；对每个命中，在 `±32` 字节内找一个**指向堆**的指针（排除模块映像内的值）当 `FProperty` 候选 |
| 3 | 在那个候选对象上扫"像结构体偏移的小整数" | `+0x30..0x50` 步长 4，取第一个 `0x40 < v < 0x2000` 的值作为 `Offset_Internal` 候选 |

输出列：`off / candidate / nearest-heap-ptr / looks-like-offset`，
末尾提示"命中处即该属性的 FName 存放位置; 旁边的堆对象应为它的 FProperty,
其中形如 `+0xNNN` 的小整数就是 `Offset_Internal` 的候选值"。

### 局限（每一条都会导致"看起来有结果但其实是噪声"）

| # | 局限 | 后果 |
|---|---|---|
| 1 | 只在名字池**第一个 64 KiB 块**的前 20000 条里找属性名 | 引擎自带的常见属性名（`MaxWalkSpeed` 之类）能命中；游戏自己的、排到后面块里的属性名**找不到**（命令会直接说"换一个更常见的属性名试试"） |
| 2 | 只扫 `UClass` 体 `0x100..0x2000`，且最多 12 个命中 | 属性表区在更后面、或命中被前面的噪声吃满时会漏 |
| 3 | "附近的堆指针"是启发式 | 相邻的别的指针（Outer、包指针、别的属性）都可能被当成 `FProperty` 候选 |
| 4 | "像偏移的小整数"也是启发式（`0x40 < v < 0x2000`） | 属性偏移落在这个区间之外、或中间夹着别的整数时会选错 |
| 5 | 步骤 3 得到的是**候选**，没有和代码生成参数表交叉验证 | **不能单独采信**。它现在的实际用途是"当参数表没覆盖某个属性时，提供一个待验证的候选"，最终仍要走 [property-verification.md](property-verification.md) 的四步法 |
| 6 | 前提"`UClass` 体内存着属性的 `FName` 索引"合理但**未经独立证实** | 若某次命中数为 0，命令会给出两种解释（属性名不在对象体内 / 索引推导有偏），需要人工排除 |

---

## 7. 这条结论牵连到哪些文档

| 文档 | 受影响的结论 |
|---|---|
| [ue-runtime.md](ue-runtime.md) §7 | `world` / `actors` 的偏移走"经验值"兜底，反射那一层永远拿不到值 |
| [movement-attributes.md](movement-attributes.md) | 移动属性的偏移来源**不是**反射，是代码生成参数表（`0x14a069800` 区段） |
| [property-verification.md](property-verification.md) | 四步验证法的第 1 步就是"从属性表读出偏移"，反射这条路已被排除 |
| [player-detection.md](player-detection.md) | 从 pawn 找 `CharacterMovement` 组件的"反射优先"路径在本构建基本拿不到，真正生效的是"对象表里找 `Outer == pawn`"的退路 |

**还能用的反射能力**（不要因为"反射不可用"就全部放弃）：

- `structSize()`（`+0x58`）、`superStruct()`（`+0x40`）：实测正确，`class` / `props` 的继承链遍历靠它；
- 类的名字 / `Outer` 链 / `is-a` 关系：走对象表，与 `UStruct` 布局无关；
- `FField::ClassPrivate(+0x08) → FFieldClass::NamePrivate(+0x18)`：实测确认，能读属性类型名；
- 对象表本身（`GObjects`）与名字池（`GNames`）：与 `UStruct` 布局**完全无关**，一切照旧可用。
