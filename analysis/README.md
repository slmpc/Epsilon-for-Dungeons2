# analysis — Minecraft Dungeons II 逆向分析工作区

本目录由原先仓库根部的 `re/` 与 `docs/` 合并而来（见首个 commit），内容按"代码"与"文档"两类归位。
针对 `EpsilonForDungeons2` 的静态取证 + 活体进程只读校验工作，全部工具为纯标准库实现（`ctypes` + `struct`），
**无注入、无写入、无调试器**。

```
analysis/
├── docs/                     文档与早期工具
│   ├── MCD2_基址与运行时结构_实测.md          基址/RVA 速查 + 结构自洽校验报告
│   ├── MCD2_逆向分析_外挂开发切入点.md        目标画像、可行性边界与切入点分析
│   └── _re_tools/            早期一次性脚本（彼此独立，无互相 import）
└── re/                       当前可复用的定位器工具链
    ├── pe.py                 ← 被依赖：最小 PE 解析器（节区 / VA 换算）
    ├── live.py               ← 被依赖：活体进程只读访问库（Toolhelp32 + RPM）
    ├── mcd2.py               主入口：四项定位（GObjects / GNames / GEngine / GWorld）
    ├── find_globals.py       枚举 UObject 并反查 .data 中指向它们的全局指针
    ├── find_engine2.py       发现 UEngine 子类实例并确认 GWorld
    ├── gworld_check.py       对候选全局做代码写引用分析，判定哪个是 GWorld
    └── da.py                 llvm-objdump 封装：按 RVA/VA 区间反汇编
```

## 目标环境

| 项目 | 值 |
|---|---|
| 可执行体 | `E:\SteamLibrary\steamapps\common\Minecraft Dungeons II\Dungeons\Binaries\Win64\Dungeons-Win64-Shipping.exe` |
| 版本 | 1.1.1.0 / Unreal Engine 5.6.1 |
| SHA256 | `7C83AFBF0AD34A40B853CDB25A22FFFB605D08E2A1E2D431974D7C7C1EE0BA54` |

## 依赖关系

- `analysis/re/`：`find_globals.py` / `find_engine2.py` 依赖 `live.py`；`gworld_check.py` 依赖 `pe.py`。
  三者均用 `sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))` 自行定位，**可在任意工作目录下调用**。
- `analysis/docs/_re_tools/`：各自独立，仅依赖标准库；`bindmine.py` / `gvas.py` / `jwt.py` / `rawd.py`
  是存档（GVAS）取证链，`utoc.py` / `utoc2.py` 是 IoStore 容器解析（`utoc2.py` 为修正版，优先用它）。
- 外部工具：`analysis/re/da.py` 需要 `llvm-objdump.exe`（当前指向 `D:\Programs\LLVM\bin\`）。

## 用法

```powershell
# 完整报告（需要游戏进程在运行）
python analysis\re\mcd2.py
python analysis\re\mcd2.py --json        # 机器可读
python analysis\re\mcd2.py --pid 1234    # 指定进程

# 全局变量定位链
python analysis\re\find_globals.py       # 落盘 analysis\re\globals_log.txt
python analysis\re\find_engine2.py       # 落盘 analysis\re\engine2_log.txt
python analysis\re\gworld_check.py

# 存档 / 容器取证
python analysis\docs\_re_tools\rawd.py    <savefile>
python analysis\docs\_re_tools\gvas.py    <savefile>
python analysis\docs\_re_tools\jwt.py     <savefile>
python analysis\docs\_re_tools\utoc2.py   <container.utoc>
```

## 本次整合所做的路径修正

合并到 `analysis/` 后，原先写死旧 `re\` 路径的三处已改为**基于脚本自身位置**解析，避免再次迁移时失效：

| 文件 | 修改 |
|---|---|
| `re/find_globals.py` | `LOG` 由绝对路径 `...\re\globals_log.txt` 改为 `os.path.join(os.path.dirname(__file__), 'globals_log.txt')` |
| `re/find_engine2.py` | `LOG` 同上，改为脚本同级 `engine2_log.txt` |
| `re/gworld_check.py` | `sys.path` 由硬编码绝对路径改为自身目录，并补齐缺失的 `import os` |

其余 14 个文件的字节内容与迁移前完全一致（SHA256 逐一核对）。脚本落盘的 `*_log.txt` 已加入 `.gitignore`。
