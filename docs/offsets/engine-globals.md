# 引擎全局（GObjects / GNames / GEngine / GWorld）

代码：`offsets::globals`（[`Offsets.h`](../../src/payload/game/Offsets.h)）、
定位逻辑 `ue::Engine`（[`Engine.cpp`](../../src/payload/game/ue/Engine.cpp)）

## 实测基线（模块 RVA）

以 `Dungeons-Win64-Shipping.exe` 的**运行时模块基址**为基准：

| 全局 | RVA |
|---|---|
| `GObjects` | `0x0BEA8BF0` |
| `GNames` | `0x0BDC5040` |
| `GEngine` | `0x0C03A1C0` |
| `GWorld` | `0x0C037A80` |

这些值同时也记录在 [analysis/docs/Dungeons2_基址与运行时结构_实测.md](../../analysis/docs/Dungeons2_基址与运行时结构_实测.md)。

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

## 覆盖方式

```powershell
.\build\release\bin\epsilonInjector.exe --pid <PID> -v -i
epsilon> status          # 引擎定位总览（各全局的定位方式与校验结果）
epsilon> rescan          # 重新定位（游戏加载完成后 GObjects 才会稳定）
```

`Engine::init()` 在启动时最多重试 3 次、每次间隔 1 秒 —— 游戏可能仍在加载。
