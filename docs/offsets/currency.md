# 货币属性 `ATR_Currency`

代码：`offsets::currencyAttribute`（[`Offsets.h`](../../src/payload/game/Offsets.h)）、
模型 [`dungeons2/Currency.h`](../../src/payload/game/dungeons2/Currency.h)

> **可信度：已验证**（属性表 + CDO 结构 + 运行时数值三者互相印证，
> 实测绿宝石 61 → 读出 `61.0000`）。

---

## 结论

绿宝石不是某个整数成员，而是 GAS 属性集 **`ATR_Currency`** 上的属性。

| 属性 | 声明偏移 | 数据偏移(Base) | CDO 默认值 |
|---|---|---|---|
| `Emeralds` | `0x90` | `0x98` | 0 |
| `EmeraldsMax` | `0xA0` | `0xA8` | 0 |
| `EmeraldsMin` | `0xB0` | `0xB8` | 0 |
| `EmeraldIncreasePercentage` | `0xC0` | `0xC8` | 0 |
| `EmeraldDropChanceIncrease` | `0xD0` | `0xD8` | 0 |
| `MaxAdditionalEmeralds` | `0xE0` | `0xE8` | **2.0** |
| `EmeraldCapForDamageIncrease` | `0xF0` | `0xF8` | 0 |

「数据偏移 = 声明偏移 + 8」是**本属性集在 CDO 上直接看出来的**（下节），不是从
`ATR_Movement` 推的。

## 每条属性占 0x10 字节

本构建的属性槽在 CDO 上长这样：

```
+0x00  uint64  共享描述指针（CDO 上所有属性的这个值完全相同）
+0x08  float   FGameplayAttributeData::BaseValue
+0x0C  float   FGameplayAttributeData::CurrentValue
```

`Default__ATR_Currency` 的实测形状（`0x80` 之前是 `UObject` / `UAttributeSet` 头）：

```
+0x0090  PTR      +0x0098  (0.0, 0.0)
+0x00a0  PTR      +0x00a8  (0.0, 0.0)
+0x00b0  PTR      +0x00b8  (0.0, 0.0)
+0x00c0  PTR      +0x00c8  (0.0, 0.0)
+0x00d0  PTR      +0x00d8  (0.0, 0.0)
+0x00e0  PTR      +0x00e8  (2.0, 2.0)     <- MaxAdditionalEmeralds
+0x00f0  PTR      +0x00f8  (0.0, 0.0)
```

**恰好 7 个槽**（`0x90`..`0xF0`），与属性表里 `ATR_Currency` 的 7 条属性**一一对应**。
这是把偏移钉下来的关键证据 —— 槽数与属性数对上，且 `0xE0` 的 2.0 落在
`MaxAdditionalEmeralds` 上（"最多额外几颗绿宝石"= 2 很自然）。

`0x80` 处两个 4 字节都是 0，没有指针，所以第一个属性不可能是 `0x80`。

---

## 属性表里 `Offset_Internal` 到底在记录的哪一格 ★

这是本次最容易搞错、也最值得记下来的一点。

属性表每条记录 **0x40 字节**，本构建的形状是：

```
记录 +0x00  const char* 属性名
记录 +0x08  const char* OnRep_函数名
记录 +0x10  uint32 0x34 / 标志
记录 +0x18  uint32 0x19 / 0x45
记录 +0x30  uint32 ArrayDim (1)
记录 +0x34  uint32 Offset_Internal      <-- 要的就是它
记录 +0x38  uint32 类型哈希
```

`ATR_Currency` 的 `Emeralds` 记录落在 `0x14a0d00c0`，`+0x34` 读出 **`0x90`**：

```
0x14a0d00c0  ptr "Emeralds"            <- 记录 +0x00
0x14a0d00c8  ptr "OnRep_Emeralds"      <- 记录 +0x08
0x14a0d00d0  0x34 | 0x00100001 | 0x19 | 0x45
0x14a0d0f00  ...
0x14a0d00f0  1 | 0x90 | hash | 1       <- 记录 +0x34 = 偏移
0x14a0d0100  ptr "EmeraldsMax"         <- 下一条记录
```

### 为什么不能按 `名字槽 − 0x0C` 读

仓库里 `ATR_Movement` 的既有偏移是用 `名字槽 − 0x0C` 读出来的
（见 [movement-attributes.md](movement-attributes.md)）。这两种读法**相差整整一条记录**，
必须搞清楚谁对。

用 `ATR_Movement` 的 CDO 当标尺（真值来自运行时浮点数）：

| 属性 | `−0x0C` 规则 | `+0x34` 规则 | CDO 那个槽的实际值 | 语义判断 |
|---|---|---|---|---|
| `MovementSpeed` | `0x00` | `0x90` | 700.0 | 700 是**速度量级** |
| `MovementSpeedMultiplier` | `0x90` | `0xA0` | 1.0 | 倍率必须是 1.0 量级 |
| `GravityScale` | `0xF0` | `0x100` | 1.2 | 1.2 是重力倍率的典型默认 |

- `−0x0C` 会给 `MovementSpeed` 算出 **`0x00`** —— 属性不可能落在对象首字节
  （那里是 vtable），这一条直接判死。
- `−0x0C` 还会把 `0x90`（CDO 值是 **700**）派给 `MovementSpeedMultiplier`，
  一个「倍率」的默认值是 700 说不过去。

**结论：`Offset_Internal` 在记录 `+0x34`，名字在 `+0x00`。**

补充一条**不该从中读出的结论**：这条规则只解决「表里名字与偏移怎么配对」，
**不代表既有 `ATR_Movement` 的常数写错了**。该表的实测记录是「写 `+0x98`
（声明 `0x90`）后游戏内移动明显变快」，物理槽位是对的；按 `+0x34` 规则它的名字
应当是 `MovementSpeed` 而不是 `MovementSpeedMultiplier` —— 只是**标名**不同，
`Speed` 模块写的地址没变，所以**不动它**（改了反而会让已验证生效的功能写到别处去）。
详见 [../reverse/movement-attributes.md](../reverse/movement-attributes.md) 末尾。

---

## 验证状态：**已验证**

四步法（见 [README.md](README.md)）逐条对照：

| 步骤 | 结果 |
|---|---|
| 1. 属性表读出偏移 | ✅ `0x14a0d00c0` 记录 `+0x34` = `0x90` |
| 2. 指令特征计数 | ⬜ 未做（其余三步已足够） |
| 3. 相邻性 | ✅ CDO 上 7 个属性槽（`0x90`..`0xF0`）与表里 7 条属性一一对应 |
| 4. 运行时数值合理 | ✅ **实测绿宝石 61，读出来就是 `61.0000`** |

第 4 步的实测输出（注入体 `currency` 命令）：

```
属性集      : 0x0001A7427FED40  (ATR_Currency)
外层对象    : BP_AlexCharacter_C        <- 玩家 pawn, 与 Player.h 的判定规则一致

   声明      数据         属性                            值
   +0x0090  +0x0098    Emeralds                       61.0000
   +0x00a0  +0x00a8    EmeraldsMax                    99999.0000
   +0x00b0  +0x00b8    EmeraldsMin                    0.0000
   +0x00c0  +0x00c8    EmeraldIncreasePercentage      0.0000
   +0x00d0  +0x00d8    EmeraldDropChanceIncrease      0.0000
   +0x00e0  +0x00e8    MaxAdditionalEmeralds          1.0000
   +0x00f0  +0x00f8    EmeraldCapForDamageIncrease    0.0000
```

`EmeraldsMax = 99999` / `EmeraldsMin = 0` 语义也对得上，进一步排除「偏移落在别的字段上」。

## ★ 挑实例时必须按标志位排掉 CDO

`ATR_Currency` 在对象表里有**两个**实例：`Default__ATR_Currency`（类默认对象）
和玩家身上那个。挑错会出事 —— **往 CDO 写会影响之后所有新实例**。

判据用 `EObjectFlags::RF_ClassDefaultObject`（`UObject::Flags` 的 `0x10` 位，
见 `offsets::object::classDefaultObjectFlag`），**不要用名字前缀 `Default__`**：

> 实测踩过：同一个 CDO，菜单里跑 `currency` 时名字解析出来是空串，
> 于是 `starts_with("Default__")` 这个过滤没兜住，模块选中了 CDO
> （日志里 `玩家归属=Package`、`MaxAdditionalEmeralds=2.0` 就是它的默认值）。
> 标志位不会解析失败，名字会。

另外解析器优先选 **Outer 链能走到本地玩家 pawn** 的那个实例 ——
玩家身上那个的 Outer 实测就是 `BP_AlexCharacter_C`。

## 游戏更新后怎么修

1. 找到 `ATR_Currency` 的属性名串（`Emeralds` / `EmeraldsMax` / …）
2. 定位属性表记录（名字在记录 `+0x00`），读 `+0x34`
3. 与上一节表格逐项对照 —— 只要首项还是 `0x90` 且步长仍是 `0x10`，改 `Offsets.h` 即可
