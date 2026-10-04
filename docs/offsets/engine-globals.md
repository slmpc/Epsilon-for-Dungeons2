# 引擎全局（GObjects / GNames / GEngine / GWorld）

代码：`offsets::globals`（[`Offsets.h`](../../src/payload/game/Offsets.h)）、
定位逻辑 `ue::Engine`（[`Engine.cpp`](../../src/payload/game/ue/Engine.cpp)）

## 实测基线（模块 RVA）

以 `Dungeons-Win64-Shipping.exe` 的**运行时模块基址**为基准：

| 全局 | 2026-10-04 更新后（现行） | 更新前（已失效） |
|---|---|---|
| `GObjects` | `0x0BF35A70` | `0x0BEA8BF0` |
| `GNames` | `0x0BE51EC0` | `0x0BDC5040` |
| `GEngine` | `0x0C0C70B0` | `0x0C03A1C0` |
| `GWorld` | `0x0C0C4970` | `0x0C037A80` |

> **四个 RVA 是版本绑定的。** 2026-10-04 游戏更新（映像 `LastWriteTime` 变、静态
> `ImageBase` 仍是 `0x140000000`）后全部失效 —— 症状是 `status` 报
> `GObjects 未定位 (RVA 0x0)`、注入体降级为元命令模式。失效时**校验必然失败**，
> 所以不会拿垃圾去读，只会报「未定位」，这是设计好的安全网。

## 定位策略：只认实测 RVA + 结构自洽校验

**不做特征码扫描，不做多路探测。** 每一项拿到候选地址后都必须通过各自的校验才算数；
校验不过就报告「未定位」，而不是拿着一堆垃圾偏移继续读内存。

| 顺序 | 全局 | 校验判据 | 失败时的表现 |
|---|---|---|---|
| 1 | `GNames` | 从块 0 顺序走前 64 个条目，**至少 32 个**能解出合法名字 | `实测 RVA ... 校验失败(前 64 个条目只命中 N, 需要 ≥32)` |
| 2 | `GObjects` | `FUObjectArray::validate()` 返回非 0（见下） | `实测 RVA ... 结构自洽校验失败` |
| 3 | `GEngine` | 指向对象的**类名必须是 `GameEngine`** | `(期望类 GameEngine, 校验失败)` |
| 4 | `GWorld` | 指向对象的**类名必须是 `World`** | `(期望类 World, 校验失败)` |

顺序是硬性的：`GNames` 必须先立起来，因为 `GObjects` 的校验要靠它验名字。

`GObjects` 的 `validate()` 返回 0..3 的置信度，`0` 即不可信：

1. 计数字段互相自洽
2. 块表与第 0 块指向真实可读内存
3. 抽样对象的 vtable 落在模块映像（`MEM_IMAGE`）内
4. 名字池就绪后，抽样对象的名字能解析

## 为什么保留「实测 RVA + 校验」而不是纯 RVA

校验器本身就是安全网。游戏更新后 RVA 失效时，校验**必然**失败，于是报告「未定位」而不是
拿垃圾去读。那时只需要更新上面四个常量 —— 这是一个可预测的维护动作，比维护一套自动探测简单得多。

> 早期文档里写过「先用实测 RVA，再用特征码兜底」，但代码里从未实现兜底路径。
> 现以实现为准：**只有 RVA + 校验这一条路**。

## 怎么重新找这四个值 ★

游戏更新后按这个顺序做，全程**只读**。先决条件是 `GObjects` / `GNames` 里至少有一个
还能用 —— 都没有时先用静态特征码把 `GObjects` 找出来。

### 1. `GObjects`

静态：在 `.text` 里找 `49 C1 E8 10`（`shr r8, 0x10`），紧跟其后 48 字节内找
`48 8B 05`（`mov rax, [rip+disp]`），解出 `rip+7+disp` 再减 `0x10` 得到候选，
要求落在 `.data`。

运行时校验（自洽性，比静态特征码可靠）：

```
maxElements = u32(cand + 0x20)
numElements = u32(cand + 0x24)
maxChunks   = u32(cand + 0x28)
numChunks   = u32(cand + 0x2C)
要求 numChunks == (numElements + 0xFFFF) / 0x10000
chunkTable  = u64(cand + 0x10)
chunk0      = u64(chunkTable)，且 obj0 = u64(chunk0) 的类名能解析
```

### 2. `GNames`

扫 `.data`，找 **64 KiB 对齐**、且块头能连续走通 4 个合法 `FNameEntry` 的指针数组；
数组首元素地址即 `FNameEntryAllocator::Blocks`，**`GNames` = 它 − `0x10`**。

⚠️ 本构建的 `FNameEntry` 头部是 2 字节，索引里的字节偏移要**左移一位**再取 ——
少这一步会解出乱码（本次踩过，`name[1]` 读成一长串拼接）。

### 3. `GEngine` / `GWorld`

不要再去猜 `.data` 里哪个槽是它 —— **从对象表反查最省事**：

1. 用已确认的 `GObjects` + `GNames` 遍历对象表
2. 按**精确类名**找实例：`GameEngine`（会同时命中 `Default__GameEngine`，跳过它）、
   `World`
3. 在 `.data` 里扫 8 字节槽，找哪个槽恰好装着该实例地址 → 该槽的 RVA 就是全局的 RVA

### 4. 交叉校验（强烈建议做）

`GEngine` 与 `GWorld` 两个槽在 `.data` 里的**间距是版本无关的**：

```
更新前：0x0C03A1C0 − 0x0C037A80 = 0x2740
更新后：0x0C0C70B0 − 0x0C0C4970 = 0x2740   ← 一致
```

本次就是靠这个不变量把两个候选槽（`0xBAC1810` / `0xC0C4970`）里选对了后者。
**只找到一个时，用另一个减去这个间距去验证候选。**

## 覆盖方式

```powershell
.\build\release\bin\epsilonInjector.exe            # 无参数即注入并进交互
epsilon> status          # 引擎定位总览（各全局的定位方式与校验结果）
epsilon> rescan          # 重新定位（游戏加载完成后 GObjects 才会稳定）
```

`Engine::init()` 在启动时最多重试 3 次、每次间隔 1 秒 —— 游戏可能仍在加载。
