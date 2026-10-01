# 输出与诊断通道

本文讲注入体怎么把"它看到了什么、它走到哪一步了"送出来：`Sink` 的两种语义、
命令响应的缓冲机制、四种日志级别、文件日志的命名与共享模式坑，以及 `trace()` 为什么
要标记"未送出"。

主要来源：[`src/payload/Payload.h`](../../src/payload/Payload.h)、
[`src/payload/Payload.cpp`](../../src/payload/Payload.cpp)、
[`src/payload/PipeClient.cpp`](../../src/payload/PipeClient.cpp)。

---

## 1. `Sink`：只有两条路

```cpp
enum class Sink {
    pipe,   // 回传给注入器 —— 唯一正常路径
    none,   // 管道没连上: 只写文件日志, 静默驻留
};
```

| 取值 | 行为 | 何时出现 |
|---|---|---|
| `Sink::pipe` | `emit*` 的输出经命名管道回吐给注入器的控制台 | `connectInjectorPipe(8000)` 成功 |
| `Sink::none` | 输出丢弃，只留文件日志 | 管道连不上（注入器没起、或名字被别的实例占着） |

**设计原则**：**绝不因为没地方写就崩**。`emit()` 在 `Sink::none` 下直接返回。

> ⚠️ **陈旧注释**：`Payload.h` 文件头还写着"输出通道有两种, 运行时二选一: pipe / console"，
> `Payload.cpp` 文件头也提 `console`。**代码里没有 console 分支**（`Sink` 只有 `pipe`/`none`），
> 管道连不上时明确"不弹控制台(don't add fallback paths)"。以代码为准。

### 唯一的 `PipeClient` 实例（重要历史坑）

```cpp
struct Context {
    Sink        sink = Sink::none;
    // 唯一的管道实例。pipeClient.cpp 与这里的所有发送路径都必须用它 ——
    // 曾经因为存在第二个 PipeClient 导致"连上了但发不出去"。
    PipeClient  pipe;
    bool        pipeConnected = false;
    ...
};
```

**现象 → 原因 → 结论**（[`PipeClient.cpp`](../../src/payload/PipeClient.cpp#L1-L13)）：

| | |
|---|---|
| 现象 | 连接成功、日志显示"已连接注入器管道"，但**注入器一条消息都收不到**；反过来注入器→注入体的命令方向完全正常（命令能收到） |
| 原因 | 文件里原本有一个模块级静态 `PipeClient gPipe` 用来连接，而 `Payload.cpp` 的 `emitNow()` 往 `Context::pipe` 发送 —— **两个不同的对象**。每次 `send` 都打到那个从没连接过的实例上，返回 `false` |
| 结论 | 统一到 `ctx().pipe` 这一个实例，状态记在 `ctx().pipeConnected`。**任何新增的收发路径都必须走它** |

症状极具误导性：单向正常、反向全丢，很容易被误判成"命令没执行"而不是"输出没送出去"。

---

## 2. 输出 API 语义

| API | 行为 | 是否走响应缓冲 | 是否落盘 |
|---|---|---|---|
| `emit(text)` | 先镜像落盘，再按当前状态投递：缓冲中→追加；`pipe`→发 `Kind::data`；`none`→丢弃 | 视 `pendingActive` | **是**（`mirrorToLogFile`） |
| `emitLine(text = {})` | `emit(text)`（非空时）+ `emit("\n")` | 同上 | 是 |
| `emitFmt(fmt, args...)` | `emitLine(std::format(...))` | 同上 | 是 |
| `emitNow(kind, text)` | **绕过**响应缓冲直接发一条指定 `Kind` 的消息；返回是否真的送出去 | 否 | **否** |
| `logInfo/logWarn/logError` | `emitNow(status/error, "[*]/[!]/[x] " + s)` | 否 | **否** |
| `logVerbose` | `ctx().verbose` 为真时 `emitNow(status, "[.] " + s)` | 否 | **否** |
| `trace(msg)` | 先 `emitNow(status, msg)`，再带时间戳写文件日志 | 否 | **是**（每条都 flush） |

前缀约定（肉眼在注入器控制台里就能区分严重性）：

```
[*] 信息        (Kind::status)
[!] 警告        (Kind::status)
[x] 错误        (Kind::error)
[.] verbose     (Kind::status, 仅 ctx().verbose)
```

> **（未证实但值得知道的现状）**：`logInfo/logWarn/logError/logVerbose` 走的是
> `emitNow`，**不经过 `mirrorToLogFile`**。因此在 `Sink::none`（静默模式）下这几类日志
> 会彻底消失，文件里只剩 `trace()` 的行。关键步骤请用 `trace()` 而不是 `logXxx()`。

### 响应缓冲（命令输出攒成一条消息）

```cpp
std::string pending;        // 一条命令的所有输出
bool        pendingActive = false;
```

* `beginResponse()`：清空缓冲并置 `pendingActive = true`（命令服务端在**每条命令前**调用）。
* `endResponse()`：在锁内把 `pendingActive` 复位、`std::move` 出缓冲，**若缓冲非空且
  `sink == pipe`**，整体发一条 `Kind::data`。
* 目的：一条命令的输出攒成一个管道消息，**避免碎片化**（一次 `objects` 可能几百行）。

投递顺序在 `emit()` 里是固定的：**先落盘，再发管道**。

> 落盘不会失败到需要回滚，而管道可能对端已经没了。这样"命令有没有输出"这件事不再依赖
> 注入器是否还活着。

---

## 3. 文件日志

### 3.1 为什么必须有

注入体被塞进一个**没有控制台**的进程里。出问题没有任何可见输出 —— 目标进程直接消失，
你只知道"它崩了"，不知道崩在哪一步。所以关键路径一律落盘，
**部署到游戏里时，这份日志就是唯一的排查依据**。

### 3.2 文件日志命名

```
%TEMP%\epsilonPayload_<pid>_<模块基址>.log
```

* `<pid>`：`GetCurrentProcessId()`。
* `<模块基址>`：**大写十六进制、无 `0x` 前缀**（`fmt("{:X}", ...)`），取自
  `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | UNCHANGED_REFCOUNT,
  &makeLogPath, &self)` —— 即**注入体 DLL 自己**的基址。
* 目录取 `GetTempPathW()`，剥掉尾部的 `\`；取不到时用 `.`。

| 术语 | 指的是 |
|---|---|
| 日志名里的 `<模块基址>` | **注入体 DLL** 的加载基址（同一份 DLL 每次注入只有一个基址） |
| `Runtime::moduleBase()` | **宿主游戏 EXE** 的基址（偏移计算的基准，见 [lifecycle.md](lifecycle.md#3-交给运行时的是宿主-exe-的基址不是注入体自己的模块)） |

### 3.3 ⚠️ 为什么文件名必须带模块基址

注释原文的理由（[`Payload.cpp`](../../src/payload/Payload.cpp#L138-L157)）：

* **现象**："注入了但看不到任何日志"。
* **原因**：同一个 DLL 可以被注入多次（**换个文件名就行**），而每个副本都会尝试打开同一个
  日志文件。Windows 的共享冲突判定看的是**已存在的句柄允许了什么**，不是新打开者请求了什么
  —— 只要先来的那个副本用 `FILE_SHARE_READ` 打开过，后来者的 `CREATE_ALWAYS` 就**永远失败**。
* **结论**：文件名带上模块基址，每个副本各写各的，彻底绕开。

### 3.4 ⚠️ `CREATE_ALWAYS` 的共享模式必须给 READ + WRITE

```cpp
// CREATE_ALWAYS: 每次注入重新开始一份, 免得新旧日志混在一起看串。
HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
if (h == INVALID_HANDLE_VALUE) return;      // 静默失败!
```

* **现象**："第二次注入看不到任何日志"。
* **原因**：只给 `FILE_SHARE_READ` 的话，同一个进程里**已经加载过的旧注入体**还持着这个
  文件的句柄，新注入体的 `CREATE_ALWAYS` 会撞共享冲突而**静默失败**。同一个 DLL 可以被注
  多次（改个文件名就行），所以这个场景是**常态而非特例**。
* **结论**：共享模式同时给 `FILE_SHARE_READ | FILE_SHARE_WRITE`。
* 注意失败时**直接 `return`，不报错** —— 所以"没有日志文件"这件事本身就是一条需要
  主动怀疑的线索（见 [dev/diagnostics.md](../dev/diagnostics.md)）。

`CREATE_ALWAYS` 的另一个作用：每次注入重新开始一份，免得新旧日志混在一起看串。

### 3.5 打开时写什么

```cpp
const std::string banner = fmt("=== epsilonPayload pid={} 启动 {} ===\n",
                               ::GetCurrentProcessId(), timestamp());
::WriteFile(h, banner.data(), ...);
::FlushFileBuffers(h);
```

时间戳格式 `HH:MM:SS.mmm`（本地时间）。`<pid>` 与文件名里的 pid 一致，便于确认日志没串。

### 3.6 命令输出镜像落盘

```cpp
void mirrorToLogFile(std::string_view text);
```

**现象 → 原因 → 结论**：

| | |
|---|---|
| 现象 | "命令执行了但没有任何输出"，让人误以为是命令本身失败 |
| 原因 | 命令输出原本**只**走管道。注入器一旦退出（比如 `--exec` 跑完就关），还在路上的响应就彻底丢了 |
| 实测 | 被这个坑掉过一轮：`findprop` 的输出一个字都没留下，**分不清是没命中还是根本没跑** |
| 结论 | `emit()` 里先镜像落盘再发管道。无论注入器是否还在，命令结果都能从 `%TEMP%\epsilonPayload_<pid>_<base>.log` 读回来。这条路径**不依赖任何对端存活** |

> 排查时不要只看注入器的 stdout：注入器的 `--exec` 路径在每条命令之间只 `Sleep(400)`
> 就继续，可能先于响应到达就退出了。

### 3.7 `trace()` 与 `[未送出]` 标记

```cpp
void trace(std::string_view msg) {
    // 先尝试送出, 再把"送没送出去"一起写进文件。
    const bool sent = emitNow(proto::Kind::status, msg);
    ...
    std::string line = fmt("[{}] {}{}\n", timestamp(),
                           sent ? "" : "[未送出] ", msg);
    ::WriteFile(gLogFile, line.data(), ...);
    ::FlushFileBuffers(gLogFile);       // 每条都 flush
}
```

* **`[未送出]` 是决定性的信息**：它把"注入体没写"和"写了但没到"两种情况**直接分开**。
  排查"注入器收不到消息"这类问题时，这一位就能定性。
* **每条都 `FlushFileBuffers`**：如果下一步就崩了，日志必须**已经**落盘。
* 用途：关键步骤之间的"我走到这了"全部用它，便于**二分定位崩溃点**（日志最后一行就是
  崩溃点附近）。

日志行样例：

```
=== epsilonPayload pid=25764 启动 14:42:07.113 ===
[14:42:07.115] runtimeMain 进入 (pid 25764, 模块基址 0x7FF6..., 大小 3.2 MB)
[14:42:07.120] 正在连接注入器管道...
[14:42:07.124] 已连接注入器管道, 输出走管道
[14:42:07.130] 正在定位引擎全局(GObjects / GNames)...
[14:42:07.130] 引擎定位 第 1 次尝试...
[14:42:07.146] 引擎定位成功
[14:42:07.150] 功能线程已启动(模块 tick + Present 自动安装)
[14:42:08.902] [未送出] Present 钩子自动安装已启用(默认); 将在 10 秒后尝试, 如需关闭请设 EPSILON_NO_AUTO_HOOK=1
```

（上面是结构示例，行内容与顺序来自代码里的字面量；时间戳/地址是示意值。）

### 3.8 其他 API

| API | 说明 |
|---|---|
| `logFileOpen() noexcept` | 幂等（已打开直接返回）；失败**静默**返回，不抛异常 |
| `logFileClose() noexcept` | 关句柄并置 `INVALID_HANDLE_VALUE` |
| `logFilePath() noexcept` | 返回当前日志路径（持锁读），给 `runtimeMain` 在静默模式下打印用 |

所有文件写入都持 `gFileMu`；`gEmitMu` 保护 `pending` 与投递路径。两个锁的顺序是
`emit()` 先 `mirrorToLogFile`（取 `gFileMu`，内部自解锁）再取 `gEmitMu`，不嵌套持有。

---

## 4. 怎么验证这套通道

| 要验证的点 | 做法 |
|---|---|
| 管道双向可用 | 注入后执行 `status`：注入器控制台出现数据（`Kind::data`），同时文件日志里也有同样内容（镜像） |
| 输出是否真的送出去了 | `findstr /C:"[未送出]" %TEMP%\epsilonPayload_*.log` —— 命中说明管道那一侧没接上（或对端已退出），而不是注入体没写 |
| 命令输出是否落盘 | `--exec findprop <类> <属性>` 之后**注入器已退出**，仍能从日志里读到结果 |
| 日志文件名是否如预期 | `dir %TEMP%\epsilonPayload_*.log`：应看到 `<pid>_<大写十六进制基址>` 后缀 |
| 静默模式行为 | 不启注入器直接注入 DLL：日志里应有 `管道连接失败(...) —— 进入静默模式, 只有文件日志` 与 `日志文件: <路径>`（后者由 `logFilePath()` 提供） |
| 重复注入的日志是否互相覆盖 | 复制 DLL 改名后再次注入：两份 `epsilonPayload_<pid>_<不同基址>.log` 并存 |

回到索引：[../README.md](../README.md)
