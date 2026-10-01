# 运行时反射定位：四个引擎全局与各自的校验

> **依据**：`src/payload/game/ue/Engine.h/.cpp`、`ObjectArray.h/.cpp`、`NamePool.h/.cpp`、
> `World.h/.cpp` 里的注释与实现；活体实测记录见
> [analysis/docs/Dungeons2_基址与运行时结构_实测.md](../../analysis/docs/Dungeons2_基址与运行时结构_实测.md)；
> 运行时输出取自 `status` / `rescan` / `mem` / `props` 命令的实测文案。
> **基线**：`Dungeons-Win64-Shipping.exe` 1.1.1.0 / UE 5.6.1，静态 image base `0x140000000`。

---

## 0. 一页速览

| 全局 | 基线 RVA | 类型 | 定位方式 | 自洽校验（不过就报"未定位"） |
|---|---|---|---|---|
| `GNames` | `0x0BDC5040` | `FNamePool` | 实测 RVA，`Blocks[] = GNames + 0x10` | `NamePool::score(64) ≥ 32`：从块 0 起始**顺序**走条目链 |
| `GObjects` | `0x0BEA8BF0` | `FUObjectArray` | 实测 RVA | `refresh()` 读得到块表 + `validate() != 0`（置信度 1..3） |
| `GEngine` | `0x0C03A1C0` | `UEngine*` | 实测 RVA | 指向对象的**类名必须是 `GameEngine`** |
| `GWorld` | `0x0C037A80` | `UWorld*` | 实测 RVA | 指向对象的**类名必须是 `World`** |

定位顺序在 `Engine::init()` 里是固定的：**GNames 先立起来**（GObjects 的校验要靠名字池验名字），
再 GObjects，再 `reflection_.bind(&names_)`，最后 GEngine/GWorld。GNames 失败即整体失败（提前 return），
所以 `status` 上出现"GNames 未定位"时，GObjects 那一行也不会有值。

维护动作只有一件事：游戏更新后把 `Engine.h` 里 `baseline::` 的四个常量改成新值。
这是**刻意的取舍**（见 [PLAN.md](../../PLAN.md) §3：删掉了 GObjects 三路 / GNames 两路 /
GEngine·GWorld 两路 / `patternScan` 整个模块），换来"不会有一条 fallback 悄悄走错路"。

---

## 1. 为什么必须校验：失败就是"未定位"，不是拿垃圾偏移去读

`Engine.cpp` 的头注释把理由写得很直白：

> 校验器就是安全网。游戏更新后 RVA 失效时, 校验必然失败, 于是报告"未定位"而不是拿着一堆
> 垃圾偏移去读内存。那时把下面 baseline 里的四个常量更新一下即可 —— 这是可预测的维护动作。

具体的失败文案（照抄实现里的格式串，排查时按这些字符串定位分支）：

| 场景 | 输出文案 |
|---|---|
| GNames 校验不过 | `实测 RVA 0xbdc5040 校验失败(前 64 个索引只命中 {n}/64, 需要 ≥32)` |
| GObjects 读不到块表 | `实测 RVA 0xbea8bf0 读不到 FUObjectArray` |
| GObjects 结构不自洽 | `实测 RVA 0xbea8bf0 结构自洽校验失败` |
| GEngine/GWorld 类名不符 | `实测 RVA 0xc03a1c0 → {类名} {对象名} (期望类 GameEngine, 校验失败)` |
| 整体失败（`rescan`） | `定位失败 —— 游戏可能还没进到关卡, 稍后再试。` |

**为什么不能"读不到就换一个候选"**：`ObjectArray` 的读取全部走 `safeRead`，指针是垃圾值时
读到的要么是 0、要么是随机字节——而随机字节同样能"读出"一个看似合理的对象数。
只有把它和 count/chunk 的自洽关系、vtable 归属、名字可解析性三条对上，才能把"看起来有数据"
和"真的是它"区分开。本项目已经因为跳过这类校验吃过一次亏（见 §5.3 的 `score` 反例）。

### 已知的注释坑（别信头注释）

| 位置 | 注释说 | 实际代码 |
|---|---|---|
| `Engine.h` 顶部 | "GObjects：先用实测 RVA，再用特征码兜底"、"GNames：先验 RVA，再扫 `.data` 找" | **只有实测 RVA**。`Engine.cpp` 的注释是修正后的口径，`PLAN.md` §3 也记录了 fallback 已删。**以代码为准** |
| `Engine.cpp` 的 GNames 失败文案 | "前 64 个**索引**只命中 n/64" | `score()` 实际是**顺序走条目链**（原因见 §5.3）。文案是历史措辞，含义应为"前 64 个**条目**" |

---

## 2. GNames —— FNamePool

### 2.1 布局（实测）

```text
FNamePool / FNameEntryAllocator          (GNames = moduleBase + 0x0BDC5040)
  +0x00  FRWLock Lock            8 字节
  +0x08  uint32 CurrentBlock
  +0x0C  uint32 CurrentByteCursor
  +0x10  uint8* Blocks[8192]     ← 我们只关心这个（NamePool 的 blocks_ 就是这个地址）
每块固定 64 KiB（0x10000）。

FNameEntry 头（紧凑位域，2 字节）
  bit0     bIsWide (1 = UTF-16)
  bit1..5  ProbeHash
  bit6..15 Len（10 bit，最大 1023）
随后是 Len 字节/宽字符的名字本体
```

### 2.2 定位与校验（`Engine::locateGNames`）

```cpp
NamePool np(candidate + 0x10);        // 局部对象, 只用于试
const int sc = np.score(64);
if (sc < 32) { /* 报"校验失败"并 return false */ }
names_.setBlocks(candidate + 0x10);   // 过了才落到成员上
```

阈值 **32/64**：名字池第一个块里前几十条必然是引擎自己的名字（`None` / `ByteProperty` /
`IntProperty` / `/Script/CoreUObject` …），命中率低于一半说明地址或布局错了。

### 2.3 索引 → 名字 的解码

```text
index      = (Block << 16) | (ByteOffset >> 1)     ← 注意右移一位, 见 §5.2
Block      = index >> 16
Offset     = index & 0xFFFF                        （"偏移/2"）
Entry      = *(u64*)(Blocks + 8*Block) + 2*Offset
Header     = *(u16*)Entry                          （bit0 宽字符, bit6..15 长度）
Text       = Entry + 2 起 Len 字节 / Len 个 UTF-16
```

`safeRead` 全程保护；`index` 超出 `1<<22` 直接判无效（`kMaxSanityIdx`），对应块指针为 0 也判无效。
`resolve()` 带一层线性缓存（上限 4096 条），`resolveUncached()` 用于校验场景。

---

## 3. GObjects —— FUObjectArray

### 3.1 布局要点：`TUObjectArray` 是**内嵌**的，不是再解一层引用 ★

实测布局（`ObjectArray.h` 顶部注释，已在活体进程验证）：

```text
FUObjectArray                              (GObjects = moduleBase + 0x0BEA8BF0)
  +0x10  TUObjectArray Objects             ← 内嵌! 块表指针就在这里, 直接读
  +0x20  int32 MaxElements                 （内嵌 TUObjectArray 的字段）
  +0x24  int32 NumElements
  +0x28  int32 MaxChunks
  +0x2C  int32 NumChunks

FUObjectItem   步长 0x18
  +0x00  UObject* Object
  +0x08  EObjectFlags Flags
  +0x0C  int32 ClusterRootIndex
  +0x10  int32 SerialNumber

UObject
  +0x00  vtable
  +0x08  EObjectFlags Flags
  +0x0C  int32 InternalIndex        ← 对象自己的索引
  +0x10  UClass* ClassPrivate
  +0x18  FName NamePrivate          { int32 ComparisonIndex; int32 Number }
  +0x20  UObject* OuterPrivate
```

**踩坑点**：`TUObjectArray` 内嵌在 `FUObjectArray + 0x10`。如果按"`GObjects+0x10` 是个
`TUObjectArray*`，再解一层引用、再 +0x10 拿块表"的直觉写，读到的会是完全无关的地方，
现象就是"读不到 FUObjectArray"（`refresh()` 直接返回 false）。代码里 `objects_` 成员只是给
上层显示用的"TUObjectArray 起始地址"（= `gObjects_ + 0x10`），**不参与取值**。

### 3.2 真机字节与自洽推算（为什么敢确认这个布局是对的）

两次独立活体样本，`NumElements / MaxElements / NumChunks / MaxChunks` 全部满足
`NumChunks == ceil(NumElements / 65536)` 且 `MaxChunks == MaxElements / 65536`：

| 采样 | MaxElements `+0x20` | NumElements `+0x24` | MaxChunks `+0x28` | NumChunks `+0x2C` | 推算 |
|---|---|---|---|---|---|
| 关卡内（`refresh()` 注释记录） | **2162688** | **149034** | **33** | **3** | `2162688/65536 = 33`；`ceil(149034/65536) = 3` |
| 主菜单（`Epsilon.py` 只读校验记录） | 2162688 | **77524** | 33 | **2** | `ceil(77524/65536) = 2`；前 200 槽 vtable **200/200** 落在 `.rdata` |

两条样本的 `NumElements` 不同（主菜单 77524 → 关卡内 149034），说明**这个值会随加载增长，
不要缓存**；但两个计数关系始终成立 —— 垃圾地址不可能两次都撞上这种巧合。

### 3.3 取值链

```text
T   = *(u64*)(GObjects + 0x10)            // 块表 (== 内嵌 TUObjectArray::Objects)
Ch  = *(u64*)(T + 8*(index >> 16))
Obj = *(u64*)(Ch + 0x18*(index & 0xFFFF)) // FUObjectItem 的第一个字段
```

`chunkAt()` 有 `chunkIdx < maxChunks_` 的上界检查（防越界读到别的内存）；
`itemAt()` / `objectAt()` 全部走 `safeRead`，空洞返回 0，`for_each()` 跳过空槽。

### 3.4 `validate()` 的 3 条证据（置信度 0..3）

| # | 证据 | 判据（原文数值） | 不过就 |
|---|---|---|---|
| 1 | **计数字段互相自洽** | `1 ≤ NumElements ≤ 8'000'000`；`NumElements ≤ MaxElements ≤ 32'000'000`；`NumChunks == ceil(NumElements/65536)`；`1 ≤ MaxChunks ≤ 8192` 且 `MaxChunks ≥ NumChunks` | 置信度 0 |
| 2 | **块表与第 0 块指向真实可读内存** | `probeReadable(chunkTable, 8)`；`chunkAt(0) != 0` 且 `probeReadable(chunk0, 0x18)` | 置信度 0 |
| 3 | **前 8 个对象的 vtable 落在某个已映射模块里** | `safeRead(obj)` 取 vtable → `VirtualQuery` 的 `mbi.Type == MEM_IMAGE`；要求 `ok != 0 && ok >= bad` | 置信度 0 |
| 4 | （加分项）**名字可解析** | 已 `setNamePool(&names_)` 时，前 8 个对象里 `≥4` 个能解出非空名字 | → 置信度 **3**；否则 `ok ≥ 6` → **2**，其余 → **1** |

第 3 条是"最强的一条证据 —— 垃圾指针几乎不可能连续满足"（注释原话）。`status` 里显示的
`结构校验 : {n}/3` 就是这个 `validate()` 的返回值；**0 会在 `locateGObjects()` 里被直接
判为定位失败**，根本不会进入 `objects_`。

---

## 4. UObject / UClass / FName 的取值链

| 想拿什么 | 怎么拿 | 代码位置 |
|---|---|---|
| 对象名 | `NamePool::resolve(*(i32*)(obj + 0x18))` | `ObjectArray::nameOf` |
| 对象的类 (`UClass*`) | `*(u64*)(obj + 0x10)` | `ObjectArray::classOf` |
| **类名** | `UClass` 自己也是 `UObject` → `resolve(*(i32*)(klass + 0x18))` | `ObjectArray::classNameOf` |
| 外部对象 (`Outer`) | `*(u64*)(obj + 0x20)` | `ObjectArray::outerOf` |
| "Class Outer:Name" 形态 | `classNameOf` + `nameOf`（**不含 Outer**，与 UE 的 `GetFullName()` 不同） | `ObjectArray::fullNameOf` |
| 属性的类型名 | `*(u64*)(field + 0x08)`（`FField::ClassPrivate`）→ `resolve(*(i32*)(klass + 0x18))` | `Reflection::walk` |
| 找类对象 | 名字等于类名 **且** 它的类名是 `"Class"` | `ObjectArray::findClass` |

> ⚠️ `fullNameOf()` 的注释写的是 `ClassName Outer:Name`，但实现只拼 `ClassName Name`
> （注释与实现不符）。要判"这个对象挂在哪"必须自己走 `outerOf()`。

---

## 5. NamePool 的两个致命坑 ★

这两个坑都不是"崩溃型"，而是**把正确的地址判成错的**（或反过来读出"看起来有数据"的垃圾），
排查成本极高，因此单列。

### 5.1 坑一：UE5 的 `FNameEntry` **不带 NUL 终止符**

布局是 `[uint16 header][name bytes]` 然后按 2 字节对齐；长度**完全由头部的 Len 位域给出**。
如果把名字当 C 字符串读（要求 `str[len] == '\0'`），**每一条都会解析失败** ——
曾经因此把一个**完全正确**的 FNamePool 判成无效地址。

实测样本（某块的头 32 字节，`NamePool.cpp` 注释记录）：

| 头字节 | header | bit0 `bIsWide` | `(header>>6)&0x3FF` | 名字 | 占位 |
|---|---|---|---|---|---|
| `1E 01` | `0x011E` | 0 | `4` | `None` | `2+4 = 6` |
| `10 03` | `0x0310` | 0 | `0xC = 12` | `ByteProperty` | `2+12 = 14` |
| `C0 02` | `0x02C0` | 0 | `0xB = 11` | `IntProperty` | `2+11 = 13` → 对齐到 `14` |

验算：`0x011E >> 6 = 4`（"None" 4 字符）、`0x0310 >> 6 = 12`、`0x02C0 >> 6 = 11` —— 三条全对，
所以"长度只看位域、不看终止符"是**实测反推**出来的，不是凭直觉。

现在的合法性检查只剩一条轻量规则：名字里**不该出现控制字符**（`< 0x20` 或 `0x7F`），
仍**不要求** NUL。

### 5.2 坑二：索引里的字节偏移是**右移一位**存的

条目按 2 字节对齐，所以 UE 把字节偏移 `>> 1` 塞进 16 位字段，换出一位额外寻址范围。
读的时候必须 `× 2` 还原：`Entry = Block + 2 * (index & 0xFFFF)`。

实测依据（两个独立样本，都精确落在条目边界上）：

| 索引 | `index × 2` | 该处 header | `Len` | 解出的名字 |
|---|---|---|---|---|
| `0x6EB` | `0xDD6` | `F6 04` (`0x04F6`) | `0x13 = 19` | `/Script/CoreUObject` |
| `0x20B` | `0x416` | `80 01` (`0x0180`) | `6` | `Object`（UObject 的 CDO） |

**直觉写法的后果**（注释原话）：*"索引会落在某个条目中间, 解出的'名字'是别扭的 UTF-16 乱码,
而地址、布局、校验全都正常, 极难定位。"* —— 注意它**不会**让你看到空串或崩溃，
只会让你看到"另一种乱码"，所以现象上非常像"这个地址差不多是对的，就是偏了一点"。

### 5.3 为什么 `score()` 必须顺序走条目链，不能按索引采样 ★

`FName` 的索引是 `(Block << 16) | ByteOffset`，**相邻索引不是相邻条目**，而是同一个块里
相差 1 字节的两个位置。按 `0,1,2,…,N` 采样的话，绝大多数会落在某个条目的中间，解析必然失败。

这个错误曾经让一个**完全正确**的 FNamePool 地址被判为无效（`NamePool::score` 注释记录）：

```text
真机上前 64 个索引"只命中 4/64", 看起来像地址偏了;
实际上地址和布局都对(dump 出来是 None / ByteProperty / IntProperty ...), 只是校验方式错了。
```

修正后的判据：**从块 0 的起始处逐条往前走**，用每条自己的 `consumed` 前移，走完一个 64 KiB 块
或达到 `maxEntries` 为止，返回连续可解析的条目数。

```
p = block0
loop maxEntries 次:
    e = readEntry(p)         // 头部 + 名字 + 2 字节对齐
    失败就 break              // 返回已 OK 的条数
    ok++; p += e.consumed
    若 p - block0 >= 0x10000: break
```

⚠️ 同一个坑还有第二处体现：`probe()` / `resolve(index)` 只能验单个索引，
**不能**用来做"这个池对不对"的整体判断。判断池只能用 `score()`。

---

## 6. GEngine / GWorld

### 6.1 校验方式

`Engine::locateEngineAndWorld()` 用同一个 `trySlot` lambda 处理两个槽：

1. 读 `*(u64*)(moduleBase + RVA)`，读不到或为 0 → 失败（`读不到指针`）；
2. 用对象表解出该指针指向对象的 `classNameOf` / `nameOf`；
3. **类名必须等于期望值**（`GameEngine` / `World`），否则失败并把实际类名/对象名打出来；
4. 通过则 `verified = true`。

实测基线（`analysis/docs` 记录）：`GEngine` → 唯一非 CDO 的 `GameEngine` 实例；
`GWorld` → `Menu_Spicewood`，类名 `World`。

> 为什么 `GWorld` 的判别不能只看"指针非空"：`.data` 里有 **49514** 个槽指向 UObject，
> 其中一个（`0x0BA35810`）当时也指向同一个 `Menu_Spicewood` 对象，但它在 `.text` 里
> **零引用**——是结构体字段（如 `FWorldContext::World`），不是全局变量。类名校验能挡住
> "读到了别人的字段"这种情况，因为它要求"这里确实放着一个 `World` 实例"，
> 而**真正区分 GWorld 的是静态引用分析**（`0x0C037A80` 有 4 处读取，其中一处是
> `UObject::GetWorld()` 的兜底分支）。

### 6.2 坑：`value` ≠ `verified`

`trySlot` 的写法是**先填 `slot.value`，再做类名校验**。因此类名校验失败时
`slot.value` 仍是非 0，只有 `slot.verified == false`。这带来两个后果：

- `Engine::hasEngineWorld()` 只看 `gengine_.value && gworld_.value`，
  **不能**当作"两个全局都验过了"；
- `WorldView::currentWorld()` 也只用 `gworld().value`（没看 `verified`）。

**判据以 `verified` 为准** —— `globals` 命令把它显示成 `校验=通过/未通过`，
`report()` 里的槽位行也会把 `[how]` 一起打出来（失败原因就在 `how` 里）。

---

## 7. UWorld / ULevel：反射优先，经验值兜底

`WorldView` 的取偏移策略（`World.cpp::findOffset`）：

```text
1. 反射问本类属性        → how = "反射 UWorld::PersistentLevel → +0x30"
2. 反射沿继承链找同名     → how = "反射(继承链) PersistentLevel → +0x30"
3. 都拿不到 → 经验值      → how = "经验值 +0x30(反射不可用)"
4. 经验值也没有 → nullopt → how = "未解析"
```

**本构建实际生效的是第 3 条**（反射不可用，见 [reflection-limits.md](reflection-limits.md)）。
所以 `world` / `actors` 命令输出的 `偏移来源` 一栏必然写着"经验值 …(反射不可用)"——
如果哪天它变成"反射 …"，说明反射修好了，那是个重要信号。

### 7.1 `ULevel::Actors` = `+0xa0`（实测确定）

常见 UE5 版本把这个字段放在 `0x98` 一带（有的资料写 `0x40`），**本构建都不是**。
定它的办法是"扫 ULevel 对象体上符合 TArray 形态的槽"（`scanlevel` 命令）：

```text
+a0 -> { data, num = 972, max = 1364 }, 首元素类名 WorldSettings
```

`WorldSettings` 在 UE 里**恒定占据 `ULevel::Actors` 的索引 0**，所以可以判定 `+0xa0` 就是 `Actors`。
`readArray()` 还有一层形态校验：`0 ≤ num ≤ 10'000'000`，且不允许 `data == 0 && num > 0`。

偏移本体与可信度见 [../offsets/world.md](../offsets/world.md)（本文不重复数字）。

### 7.2 两个退路（不是 fallback，是必要的可用性设计）

- `currentWorld()`：`GWorld` 没定位到 → 在对象表里找**第一个**类名为 `World` 的实例。
  ⚠️ 这是全表线性扫描，且 CDO 也叫 `World`，所以它给出的不保证是"当前世界"；
  只有在 `GWorld` 缺失时才会走到这里。
- `PersistentLevel` 经验值 `+0x30`、`Actors` 经验值 `+0xa0`。
  `+0xa0` 有实测依据（上一节）；**`+0x30` 这次整理没有重新独立验证**，标为待复核：
  复核办法是 `ptr <UWorld 地址>` 看哪个槽指向 `ULevel`，或 `mem va=` 直接看。

---

## 8. 游戏更新后怎么复现这套定位

```text
1. status                     → 看四个全局里哪个"未定位", 失败原因就在同一行
2. mem <rva> len=0x40         → 直接看原始字节, 分清"读数不对"和"地址不对"
3. 更新 Engine.h 的 baseline:: 四个常量
4. 复核结构:  GObjects → refresh/validate 的自洽关系(§3.2 的算式)
             GNames   → score(64) 的命中数
             UStruct  → props <类名> + scanlevel <ULevel 地址> + findprop(见 reflection-limits.md)
5. 复核偏移:  按 property-verification.md 的四步法
```

固定基线（本构建，供比对）：

```text
GObjects 0x0BEA8BF0 | GNames 0x0BDC5040 | GEngine 0x0C03A1C0 | GWorld 0x0C037A80
.text RVA 0x1000 大小 0x08DC367B   (.rdata 0x08DC5000 / .data 0x0B9EC000)
GNames 实测样本:  CurrentBlock = 0x63(99), CurrentByteCursor = 0xA25A
```

---

## 9. 未决与存疑

| # | 事项 | 状态 |
|---|---|---|
| 1 | `UWorld::PersistentLevel = +0x30` | 经验值，**本次整理未独立复核**（`+0xa0` 有实测依据）。 |
| 2 | `currentWorld()` 的退路可能返回 CDO `World` | 代码事实，未观察到实际误用；`GWorld` 正常时不会走到。 |
| 3 | ~~`value` 与 `verified` 的混用~~ | **已修**：`currentWorld()` 改为只采信 `verified` 的 `GWorld` 槽，校验不过时退到对象表扫描。 |
| 4 | ~~`Engine.h` 头注释描述的特征码兜底~~ | **已删**：该兜底路径从未实现，现以「实测 RVA + 校验」为唯一口径。 |
| 5 | ~~`score()` 的"索引/条目"措辞~~ | **已修**：失败文案与日志统一改说"条目"。 |
| 6 | `fullNameOf()` 不含 Outer | 返回的是 `ClassName Name`；函数名略有误导（注释已改正，行为未变，`findObjectByFullName` 与它自洽）。 |
