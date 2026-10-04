# 货币系统的代码侧（`ATR_Currency` / `GetPlayerCurrency`）

> 本文记录**代码路径**上的实测结论，补充 [../offsets/currency.md](../offsets/currency.md)
> 的偏移表。会话日期 2026-10-04，样本 `Dungeons-Win64-Shipping.unpacked.exe`
> （节区布局与原版一致，ImageBase `0x140000000`，所有 RVA 通用）。

---

## 一、二进制里保留着全部 UFunction 名字 ★

Shipping 构建虽然剥了符号，但 **UFunction 名字全在 `.rdata`**，而且有现成的
「名字 → 实现」记录。实测本构建有**两种**记录，**别搞混**：

| 表 | 形状 | `+0x08` 是什么 |
|---|---|---|
| A | `{ const char* Name; UFunction* (*Getter)(); }`，步长 `0x10` | **惰性取 UFunction 的小函数**，不是实现 |
| B | `{ const char* Name; FNativeFuncPtr Exec; void* Impl; }`，步长 `0x48` | `exec` 跳板；**真实实现在 `+0x10`** |

A 型长这样（典型反编译）：

```c
__int64 sub_X() {                       // 只是个缓存取用器
  result = qword_14C176310;
  if (!result) { sub_1416A2980(&qword_14C176310, &off_14BC0C318); return qword_14C176310; }
  return result;
}
```

**拿 A 型的 `+0x08` 当实现去反编译，会得到一堆一模一样的取用器** —— 本次先踩了这个坑。

B 型的 `exec` 跳板是标准签名 `(UObject* this, FFrame& stack)`，内部用
`sub_1416891F0(stack, ...)` 取参后转发给 `+0x10` 的真实实现：

```c
__int64 __fastcall exec_X(__int64 a1, _QWORD *a2) {
  ... sub_1416891F0(a2, a2[3], &v14) ...   // 取参
  return impl(v7, v5);
}
```

### 工具

[`analysis/re/nativefuncs.py`](../../analysis/re/nativefuncs.py) 按 8 字节对齐扫
`.rdata`/`.data`，把所有满足「`+0x00` 指向合法标识符、`+0x08` 与 `+0x10` 都是代码」的记录
全量捞出来 —— 不需要先知道表在哪。本构建捞出 **10419 条**。

```
python nativefuncs.py --dump map.txt      # 导出全部
python nativefuncs.py Currency Emerald    # 按关键词筛
```

**这个技巧对本项目是通用的**：以后要找任何游戏侧 API 的实现，先在这里筛名字，
比从字符串交叉引用摸过去快得多（`SW_*` 那种 GameplayTag 字符串的引用点全是
包级类注册函数，反编译出来没用）。

---

## 二、绿宝石的读路径

`GetPlayerCurrency` 的真实实现 `0x146179740`（IDA 里已重命名）：

```c
__int64 __fastcall SW_GetPlayerCurrency_Impl(__int64 a1, unsigned __int8 idx) {
  ...
  v8 = GetNumericAttributeValue(v4, v7);
  return (unsigned int)((int)(float)((float)(v8 + v8) + 0.5) >> 1);   // 四舍五入
}
```

其中按 `idx` 选属性：

| `idx` | `ECurrency` | 属性取用器 |
|---|---|---|
| 0 | `Emeralds` | `0x145B1AB50` |
| 1 | `SpringStone` | `0x145B1E3A0` |
| 2 | `EnchantmentPoint` | `0x145B1AC00` |

**两条可直接用的结论**：

1. 游戏自己就是**按属性读**的，不是读某个整数成员 —— 与
   [../offsets/currency.md](../offsets/currency.md) 的结论一致。
2. 显示的个数 = **`round(属性值)`**（`(int)(v*2+0.5)>>1` 就是四舍五入取整）。
   所以倍率放大后，画面上看到的是 `round(原值 × 倍率)`。

---

## 三、写路径：偏移在代码层被印证

`ATR_Currency::OnRep_Emeralds` 的真实实现 `0x145B2EC70`：

```c
__int64 __fastcall ATR_Currency_OnRep_Emeralds_Impl(__int64 a1, __int64 a2) {
  ...
  v4 = sub_144B564B0(a1);
  sub_144B6A170(v4 + 2192, qword_14C1637B0, a1 + 144, a2);   // 广播 (旧值, 新值)
  sub_1423F0BE0(a1 + 144);
  sub_1423F0BE0(a2);
  return sub_145680350(a1, qword_14C1637B0);
}
```

- **`a1 + 144` 就是 `a1 + 0x90`** —— 代码层直接写出 `Emeralds` 的声明偏移，
  和属性表读出来的一致。这是独立于属性表的**第二条证据**。
- `a2` 是**旧值**的 `FGameplayAttributeData*`，`a1 + 0x90` 是新的。
- `sub_144B6A170(..., old, new)` 是委托广播 —— 属性变更事件。

**这是什么、不是什么**：`OnRep_*` 是**复制回调**，值从「服务端」同步过来时才触发。
单机时本地就是权威端，能不能稳定触发取决于该属性在本地是否走复制路径 ——
**没有实测过，不要当成可靠的钩子点**。

---

## 四、为什么功能模块仍然用「轮询 + 增量放大」

找写路径的目的是「挂钩游戏自己的授予逻辑」，比反复写一个可能被重算的字段稳。
本次**没有找到**一个独立的「授予绿宝石」原生函数：

- 名字里带 `AddCurrency` / `GrantCurrency` / `ModifyEmeralds` 的函数**不存在**
  （`nativefuncs.py Currency Emerald` 全量筛过）
- 授予是走 **GameplayEffect**（`SW_GameplayEffect_ModifyEmeraldsDelta`）的，
  而 GE 是资源（在 pak 里），不是代码
- GAS 的属性写入走 `FProperty*` 间接寻址，代码里不会出现 `0x98` 这种字面量 ——
  想靠「搜偏移立即数」找写点是徒劳的

所以当前实现（[../features/emerald.md](../features/emerald.md)）改为：
**只读轮询那个字段，检测到游戏写入新原始值的那一帧，按增量放大后写回**。
它不改写游戏的授予逻辑，也不存在 Speed 那种「基线被自己污染」的问题。

若将来要做真正的挂钩，候选点是 GAS 的 `SetNumericAttributeBase`
（`SW_SetNumericAttributeBase_Impl` @ `0x145697B50`）—— 但它对所有属性通用，
要按属性过滤；本次未实现。

---

## 五、IDA 里已做的标注

`Dungeons-Win64-Shipping.unpacked.exe.i64` 中已重命名/加注释：

| 地址 | 名字 |
|---|---|
| `0x146179740` | `SW_GetPlayerCurrency_Impl` |
| `0x145B2EC70` | `ATR_Currency_OnRep_Emeralds_Impl` |
| `0x145B1AB50` | `SW_GetEmeraldsAttribute_Static` |
| `0x145B1E3A0` | `SW_GetSpringStoneAttribute_Static` |
| `0x145B1AC00` | `SW_GetEnchantmentPointAttribute_Static` |
| `0x145668420` | `exec_SetNumericAttributeBase` |
| `0x145697B50` | `SW_SetNumericAttributeBase_Impl` |
| `0x144AE7260` | `GetNumericAttributeValue` |
| `0x145684920` | `GetAbilitySystemComponent_ForCurrency` |

另在 `0x14a0d00c0`（起 7 条 `ATR_Currency` 属性记录）与两张 UFunction 表
（`0x149f84258` / `0x149f8e8b0`）处加了结构说明注释。

## 六、游戏更新后怎么重来

1. `python analysis/re/nativefuncs.py --dump map.txt` —— 重建名字 → 实现映射
2. 在 map.txt 里筛 `Currency` / `Emerald`，拿到新的 `GetPlayerCurrency` 实现地址
3. 反编译它，核对三个属性取用器与取整方式有没有变
4. 反编译 `OnRep_Emeralds`，核对 `a1 + 0x90` 是否仍是新值槽
5. 偏移值本身仍以 [../offsets/currency.md](../offsets/currency.md) 的属性表法为准
