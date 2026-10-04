# Emerald — 绿宝石获取倍率

代码：[`module/impl/Emerald.h`](../../src/payload/feature/module/impl/Emerald.h) ·
[`module/impl/Emerald.cpp`](../../src/payload/feature/module/impl/Emerald.cpp)
数据模型：[`game/dungeons2/Currency.h`](../../src/payload/game/dungeons2/Currency.h)

> ✅ **偏移已实测确认**：`ATR_Currency::Emeralds` 声明 `0x90`、数据 `0x98`，
> 实测绿宝石 61 → 读出 `61.0000`。
> ⚠️ **「拾取时按倍率放大」这一步尚未在真机上跑通验证**，所以默认 `enabled: false`，
> 与 `Speed` / `Jump` 的处理一致。

---

## 绿宝石在哪

绿宝石**不是**某个整数成员，而是 GAS 属性 `ATR_Currency::Emeralds`
（声明偏移 `0x90`，`BaseValue` 在 `0x98`，`CurrentValue` 在 `0x9C`）。
完整推导、以及「属性表里 `Offset_Internal` 在记录 `+0x34`」这个关键结论，
见 [../offsets/currency.md](../offsets/currency.md)。

这条路上先踩了两个坑，都留在那里：

1. **`FCurrencyBundle` 是嵌套结构体，不是对象字段。** 属性表里确实有一组
   `EmeraldAmount=0x00 / SpringStoneAmount=0x04 / EnchantmentBooksAmount=0x08 /
   UpgradeLevelMerchant=0x0C …` 的连续 4 字节整数，一度被当成「对象首字节起排布」。
   实际那是 `FCurrencyBundle`（`Get/Set/Has/Remove` 那套）**结构体内部**的布局，
   直接拿它当对象偏移读出来全是垃圾。
2. **真正的持有者是 `ATR_Currency`。** 运行时对象表里按类名一列就看出来了 ——
   同族的 `ATR_Movement` / `ATR_Soul` / `ATR_Health` 等 30 个属性集都在。

---

## 施加方式：放大「增量」，不是维持「基线 × 倍率」

这是本模块与 `Speed` **刻意不同**的地方，也是它比 `Speed` 简单的地方。

`Speed` 要维持一个恒定倍率，所以必须记住基线、每帧写 `基线 × 倍率`，
并为此处理「重注入后字段里还是我们上次写的值」这类污染（见
[speed-jump.md](speed-jump.md) 那段长记录）。

绿宝石不需要：**它是只增不减的计数，游戏每次拾取/结算都把原始值写进去**。
于是模块每帧只看两件事：

```
读到 current
  current == lastApplied_          -> 这帧游戏没写，什么都不做
  current != lastApplied_          -> 游戏写了新原始值 raw，按增量放大：
                                      want = raw + (raw - lastRaw_) * (M - 1)
```

`lastRaw_` 是「扣掉我们加成后的游戏原始值」，随 `lastApplied_` 一起更新。

这样做的好处：

- **不需要基线，也就不存在基线污染。** 重新注入 / 关掉再打开，最坏情况是多放大一笔，
  不会像 `Speed` 那样滚雪球
- **不与游戏争写。** 我们只在检测到游戏写入的那一帧补一笔，其余帧只读不写
- **天然幂等。** 读回等于我们写的值就什么都不做 —— 不会每帧重复乘

## 几个刻意的取舍

- **花钱默认不放大**（`ScaleLoss=false`）。绿宝石在城镇里要花掉，把扣款也乘上是另一回事，
  会影响商店价格，属于用户没要求的行为。想连扣款一起放大时再打开。
- **单次增量上限 `1e7`**。防止偏移万一落到一个巨大的数上时把值写坏。
- **`MaxAdditionalEmeralds` / `EmeraldDropChanceIncrease` 也在选项里**，但它们不是「当前持有量」，
  是玩法参数 —— 放进枚举是为了排查时能直接读，不代表缩放它们有明确语义。
  `ScaleLoss` 对这两个没意义。
- **`SetAmount` 手动覆盖写一次就复位成 `-1`**，避免变成「每帧强制写成某个数」。
- **目标属性集换地址（换关卡/重生）时重取基线**，且**不去写旧地址** ——
  那个地址可能已被释放并复用。

## 怎么用

模块默认关闭。开启后（面板 `Emerald`，或配置
`~/.epsilon/ext/dungeons2/configs/<配置名>/modules/Emerald.json`）：

```json
{
  "enabled": true,
  "settings": {
    "Attribute": "Emeralds",
    "Multiplier": 5.0,
    "ScaleLoss": false,
    "Notify": true
  }
}
```

**必须进关卡或城镇**再启用 —— 菜单里 `ATR_Currency` 只有 CDO，
`Default__` 前缀的实例会被解析器跳过。

排查用命令：

```
currency                      # 7 条属性 + 整块属性区
currency set=1000             # 直写 Emeralds 看是否保持
currency class=ATR_          # 列出所有 ATR_ 对象，偏移失效时用来重判
```

## 生效判据

绿宝石是整数显示，属性是 float。放大后的值落在游戏显示上应当是
`原值 × 倍率`（小数部分按游戏的取整方式处理）。

日志里每条放大都会打一行（`Notify` 打开时）：

```
[Emerald] Emeralds 61.0 -> 65.0  (原始 62.0, x5.00)
```

判读方式：`原始` 是游戏写进去的值，`->` 后面是放大后我们写回去的值。
若只看到「原始」不变而 `->` 没出现，说明游戏那一帧没写值（正常，拾取时才写）。

## ★ 挑实例时必须排掉 CDO

玩家身上那个实例的 Outer 是 `BP_AlexCharacter_C`；另有一个 `Default__ATR_Currency`
类默认对象，**写它会影响之后所有新实例**。

判据用 `RF_ClassDefaultObject` 标志位而不是 `Default__` 名字前缀 ——
实测菜单里 CDO 的名字解析出来是空串，名字过滤兜不住。详见
[../offsets/currency.md](../offsets/currency.md)。

## 相关文档

- 偏移本体与推导： [../offsets/currency.md](../offsets/currency.md)
- 引擎全局失效与重找： [../offsets/engine-globals.md](../offsets/engine-globals.md)
- 同类模块的坑： [speed-jump.md](speed-jump.md)
- 模块框架： [module-framework.md](module-framework.md)
