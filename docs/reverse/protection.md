# 原版 exe 的自我保护机制（静态实测）

> 用户怀疑「游戏被反作弊检测后退出」。本文记录**静态分析原版**
> （`Dungeons-Win64-Shipping.exe`，带厂商代码加密）得到的实证。
> 结论：**确实存在自保护机制，但它不是反作弊产品，而是游戏自带的
> 「绕开 IAT 解析 API + 自接管退出路径」**。
> 测于 2026-10-04。

---

## 一、先排除的几件事

| 检查 | 结果 |
|---|---|
| 游戏目录里的反作弊 DLL（EAC / BattlEye / Denuvo / VMProtect…） | **无** |
| `AntiCheatExpert Protection` 服务 | 已安装但 **Stopped**（`C:\Program Files\AntiCheatExpert\ACE-Service64.exe`，多半来自别的游戏） |
| UE 崩溃转储（`Saved\Crashes`） | 今天 **0 个**（最近一次是 10-01） |
| Windows 应用程序/系统日志（错误/警告） | 今天 **0 条** |
| Steam 侧 | `GameOverlayRenderer.dll detaching` + `state changed : Fully Installed,` —— **正常退出流程** |

所以：**不是崩溃，是干净终止。**

## 二、代码加密是厂商构建期就有的，与"被检测"无关

| | 原版（磁盘） | 脱壳版（内存转储） |
|---|---|---|
| `.text` 前 4 MiB 熵 | **8.000**（完全随机） | 6.763（正常代码） |
| 逐字节相同率 | — | **0.49%** |
| 节区布局 | — | 与原版**完全一致**（说明是内存转储） |

原版 Authenticode **Valid**、`CN=Microsoft Corporation`（见
[`build/REPORT_dungeons2_shipping_protection.md`](../../build/REPORT_dungeons2_shipping_protection.md)）。
**这是微软签发的原始形态**，属于厂商代码保护，不等于反作弊。

⚠️ 推论：**`.text` 有 73.7% 只在内存里才是明文**，所以「静态分析原版找反作弊」
这条路**天然有盲区** —— 逻辑若存在于加密区，磁盘上根本看不到。

## 三、入口点与 TLS 都是标准 CRT（没有壳的 stub）

| 位置 | 内容 |
|---|---|
| EntryPoint `0x148bfc8c0` | `sub rsp,28h / call __security_init_cookie / jmp __scrt_common_main_seh` —— 标准 MSVC |
| TLS 回调 `0x148bfbc8c` | `_initterm`：遍历 `0x148E9E850..0x148E9E8E8` 的函数指针表（19 项）调用 |
| TLS 回调 `0x148bfbd04` | 遍历动态初始化器 |

**没有传统脱壳 stub 可跟** —— 这是该保护的特点。

## 四、★ 真正的发现：游戏自己接管了 API 解析与退出路径

### 4.1 用哈希隐藏 API 名，绕开 IAT

`sub_148AD6250` 遍历已加载模块的**导出表**，对每个导出名做
**FNV-1a 变体哈希**（`2166136261` / `16777619`，名字先转小写），与硬编码常量比对；
命中后还会沿**转发器字符串**（`"KERNELBASE.CreateFileA"` 这种）继续链式解析。

反查三个常量（工具 [`analysis/re/fnv_find_export.py`](../../analysis/re/fnv_find_export.py)）：

| 常量 | 对应导出 |
|---|---|
| `0x66F841C0F75DB66E` | **`CreateFileA`** |
| `0x66F841C0E95DA064` | **`CreateFileW`** |
| `0x3F2BCA4589EABB08` | **`Sleep`** |

**为什么这么做**：直接从 IAT 拿 `CreateFileA` 的话，任何在 IAT 上挂钩子的人都能看到
并篡改；**绕开 IAT、每次现场哈希查导出**，拿到的一定是系统 DLL 里的真函数。
这是**反挂钩**手法。

> 复现：`python analysis/re/fnv_find_export.py` —— 对系统 DLL 的导出名做同款哈希。

### 4.2 替换自己的 `ExitProcess`

`sub_148AD6250` 结尾：

```c
sub_148C8F34C(&ExitProcess, 0x2000, 4, &v44);      // VirtualProtect(IAT槽, PAGE_EXECUTE_READWRITE)
ExitProcess = sub_148BDCF70;                       // 把 IAT 槽指向自己的函数
sub_148C8F34C(&ExitProcess, 0x2000, v44, v45);     // 恢复页保护
```

替换后的逻辑（`sub_148AFB37C`）：

```c
void __noreturn 替身(unsigned int code) {
    sub_148ACBB30();                       // 一大坨清理
    if (++qword_14C224518 == 1) {
        if (sub_148B0EC34(1) == 0)
            originalExitProcess(code);     // 判定通过 -> 正常退出
    }
    while (1) TerminateProcess(GetCurrentProcess(), code);   // 否则强制终止
}
```

`sub_148C8D6D8` 反编译出来就是 `TerminateProcess(GetCurrentProcess(), code)`。

**这一条直接解释了观测到的所有现象**：

| 观测 | 与 `TerminateProcess` 是否吻合 |
|---|---|
| 无 UE 崩溃转储 | ✅ 不经过异常路径 |
| 无 Windows 错误事件 | ✅ |
| Steam 记为正常退出 | ✅ |
| 进程"干净消失" | ✅ |

**即：游戏自己的退出路径就是 `TerminateProcess`**，
所以"看起来像被静默杀掉"其实是它的正常收尾方式。

## 五、这对我们意味着什么

1. **`exitwatch`（注入体里钩 `ExitProcess`/`TerminateProcess`）可能被游戏覆盖** ——
   它在启动时就改写了自己的 `ExitProcess` IAT 槽。挂钩顺序会影响结果。
2. **游戏有反挂钩意识**（绕开 IAT 解析文件 API + `Sleep`），
   `Sleep` 被单独解析通常是**反调试/反延时检测**的配套。我们的注入体用 MinHook
   做**内联补丁**（改函数头），这类改动**不在 IAT 上**，所以上述机制**看不到**，
   但任何做代码完整性校验的组件会看到。
3. **仍然没有证据表明今天的退出与注入有关**：
   - 18:37 / 18:40 两次会话**在完全没有注入的情况下**也只活了 2.5 分钟
   - 加密区意味着静态分析**无法证否**加密区里有别的检查

## 六、还没做的（能定性的那一步）

**对照实验**：启动游戏 → **什么都不做**（不注入、不读内存）→ 计时。
若仍 2~3 分钟退出，则与注入无关；若能稳定开着，则是注入触发。

**以及**：注入后跑 `exitwatch`，把「谁调用了退出」的调用方 RVA 落盘 ——
不过要先确认游戏的 IAT 替换不会覆盖我们的钩子（见 5.1）。

## 七、工具

| 脚本 | 作用 |
|---|---|
| [`protect_probe.py`](../../analysis/re/protect_probe.py) | 节区熵 + 敏感 API/字符串扫描 |
| [`disas.py`](../../analysis/re/disas.py) | 反汇编明文区（需 capstone） |
| [`fnv_find_export.py`](../../analysis/re/fnv_find_export.py) | 反查被 FNV 哈希隐藏的系统 API 名 |
| [`fnv_lookup.py`](../../analysis/re/fnv_lookup.py) | 在映像自身的标识符里反查哈希 |
