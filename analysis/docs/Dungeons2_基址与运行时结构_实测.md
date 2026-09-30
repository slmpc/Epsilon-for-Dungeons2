# Minecraft Dungeons II — 基址与运行时结构（实测报告）

> 目标：`E:\SteamLibrary\steamapps\common\Minecraft Dungeons II\Dungeons\Binaries\Win64\Dungeons-Win64-Shipping.exe`
> 版本：1.1.1.0 / Unreal Engine 5.6.1　SHA256 `7C83AFBF0AD34A40B853CDB25A22FFFB605D08E2A1E2D431974D7C7C1EE0BA54`
> 方法：静态特征码挖掘 + 活体进程只读校验（`OpenProcess` + `ReadProcessMemory`，**无注入、无写入、无调试器**）
> 所有数字均来自实测，并在活体进程上通过了结构自洽校验。

---

## 一、结论速查表

| 名称 | 模块相对偏移（RVA） | 类型 | 验证方式 |
|---|---|---|---|
| 静态 ImageBase | `0x140000000` | PE 头 | 文件解析 |
| **运行时模块基址** | 每次启动随机（实测 `0x7FF637FE0000`） | — | Toolhelp32 |
| **`GObjects`**（FUObjectArray） | **`0x0BEA8BF0`** | 结构体 | 特征码 + 结构自洽 |
| **`GNames`**（FNamePool） | **`0x0BDC5040`** | 结构体 | 块数组扫描 + 名字反解 |
| **`GEngine`** | **`0x0C03A1C0`** | `UEngine*` | 指向唯一 `GameEngine` 实例 |
| **`GWorld`** | **`0x0C037A80`** | `UWorld*` | 指向 `Menu_Spicewood`（`World`） |
| `0x0BA35810` | 同上对象 | 结构体字段 | **不是**全局变量（无代码引用） |

用法：

```
G  = module_base + 0x0BEA8BF0        # GObjects
N  = module_base + 0x0BDC5040        # GNames
GE = module_base + 0x0C03A1C0        # GEngine
GW = module_base + 0x0C037A80        # GWorld
```

实测环境下的绝对值（进程 PID 41716，基址 `0x7FF637FE0000`）：

```
GObjects = 0x7FF643E88BF0      GNames = 0x7FF643DA5040
GEngine  = 0x7FF64401A1C0      GWorld = 0x7FF644017A80
```

---

## 二、运行时模块基址

映像带 `DYNAMIC_BASE | HIGH_ENTROPY_VA`，**基址每次启动不同，必须运行时读取**，不能写死。

`Dungeons.exe` 只是 UE5 的 `BootstrapPackagedGame`，真正的游戏进程是 `Dungeons-Win64-Shipping.exe`。

节区布局（RVA 固定，VA = 基址 + RVA）：

| 节区 | RVA | 大小 |
|---|---|---|
| `.text` | `0x00001000` | `0x08DC367B` |
| `.rdata` | `0x08DC5000` | `0x02C26780` |
| `.data` | `0x0B9EC000` | `0x00802944` |
| `.pdata` | `0x0C1EF000` | `0x0076B2F4` |
| `_RDATA` | `0x0C95C000` | `0x0005D910` |
| `.reloc` | `0x0C9BE000` | `0x0038E870` |

取基址：`CreateToolhelp32Snapshot(TH32CS_SNAPMODULE)` → 取名为 `Dungeons-Win64-Shipping.exe` 的 `modBaseAddr`。

> ⚠️ **权限坑**：Steam 拉起的实例可能以更高完整性级别运行，此时 `OpenProcess` 返回 `ERROR_ACCESS_DENIED(5)`，而 `PROCESS_QUERY_LIMITED_INFORMATION` 仍可成功——这是权限差异的典型指纹。同一个 exe 可以同时存在两个实例，取基址前应逐个试开，选能读的那个。

---

## 三、GObjects（FUObjectArray）

### 3.1 定位：特征码，不写死偏移

UE5 的对象索引解码代码唯一，且在 `.text` 中只命中一次：

```asm
shr   r8, 0x10                      ; 49 C1 E8 10   ChunkIndex = Index >> 16
lea   r9, [rax + 2*rax]             ; Index & 0xFFFF, 再 *3（即 *0x18）
mov   rax, [rip + GObjects+0x10]    ; 48 8B 05 <disp32>   ← 直接给出 GObjects
mov   r8,  [rax + 8*r8]             ; chunk = ObjObjects.Objects[ChunkIndex]
lea   rax, [r8 + 8*r9]              ; &chunk[i]
```

特征码（35 字节，`??` 为通配）：

```
44 8B C0 0F B7 C0 49 C1 E8 10 4C 8D 0C 40 48 8B 05 ?? ?? ?? ?? 4E 8B 04 C0 4B 8D 04 C8
```

实测命中点：RVA `0x1665F02`（VA `0x144F604D1`）。解析 RIP 位移得到 `GObjects+0x10`，减 `0x10` 即得 **`GObjects = RVA 0x0BEA8BF0`**。

**独立交叉验证**：同一代码块前 4 字节是 `cmp eax, [rip+0x14BEA8C14]`——`0x14BEA8C14` 正好等于 `GObjects + 0x24`，即 `NumElements`。两条互不相关的指令指向同一结论。

### 3.2 活体校验结果

| 项 | 实测值 |
|---|---|
| GObjects VA | `0x7FF643E88BF0` |
| chunk 表指针（`+0x10`） | `0x1E76B4E5860` |
| `chunk[0]` | `0x1E7667A0008` |
| `NumElements`（`+0x24`） | **77524** |
| `MaxElements`（`+0x20`） | 2162688 |
| `NumChunks / MaxChunks` | 2 / 33 |
| 前 200 个槽位有效对象 | **200 / 200**（vtable 全部落在本模块 `.rdata`） |

自洽关系全部成立：`ceil(77524 / 65536) = 2 = NumChunks`；`NumElements ≤ MaxElements`；`NumChunks ≤ MaxChunks`。

### 3.3 结构布局（实测，与 UE4 不同，勿照搬旧资料）

```
FUObjectArray                                  偏移
  +0x00  ObjFirstGCIndex            int32
  +0x04  ObjLastNonGCIndex          int32
  +0x08  MaxObjectsNotConsideredByGC int32
  +0x0C  OpenForDisregardForGC      bool
  +0x10  ObjObjects : FChunkedFixedUObjectArray
           +0x00  Objects   (FUObjectItem** 块表)   ← GObjects+0x10
           +0x08  PreAllocatedObjects
           +0x10  MaxElements   int32               ← GObjects+0x20
           +0x14  NumElements   int32               ← GObjects+0x24
           +0x18  MaxChunks     int32
           +0x1C  NumChunks     int32

FUObjectItem   步长 0x18
  +0x00  UObject* Object
  +0x08  int32    Flags
  +0x0C  int32    ClusterRootIndex
  +0x10  int32    SerialNumber
```

**索引 → 对象**：

```
ChunkIndex = Index >> 16
InChunk    = Index & 0xFFFF
Chunk      = *(u64*)(GObjects + 0x10 + 8*ChunkIndex)
ItemPtr    = Chunk + 0x18*InChunk
Object     = *(u64*)ItemPtr
```

> 与 UE4 的关键差异：UE4 的 `TUObjectArray` 里 `Num`/`Max` 在 `+0x18`/`+0x1C`，`FUObjectItem` 是 `0x10` 字节；**UE5 是 chunk 表 + `0x18` 步长**。用旧偏移会全部读错。

`UObject` 头部（实测）：

```
  +0x00  vtable
  +0x08  ObjectFlags   int32
  +0x0C  InternalIndex int32
  +0x10  ClassPrivate  UClass*
  +0x18  NamePrivate   FName { int32 ComparisonIndex; int32 Number; }
  +0x20  OuterPrivate  UObject*
```

---

## 四、GNames（FNamePool）

### 4.1 定位方法

UE5 的 `FNameEntryAllocator` 结构：

```
FNamePool
  +0x00  Lock                (8 字节)
  +0x08  CurrentBlock        uint32     ← 实测 0x63 (99)
  +0x0C  CurrentByteCursor   uint32     ← 实测 0xA25A
  +0x10  Blocks[8192]        uint8*     ← 每个块固定 64 KiB
```

`Blocks[]` 是**一组 64 KiB 对齐的堆指针**——这个形态在 `.data` 里极易识别（`.data` 大小为 8 MB，全扫一遍约 3 秒）。定位后从最低命中槽向前回溯，直到遇到不是「已提交的 64 KiB 对齐指针」也不是 0 的值，即为数组首元素。

实测：`.data` 中 5 个槽通过「64 KiB 对齐 + 块头是合法 `FNameEntry` 链」双重校验，回溯 20 槽得到 `Blocks[] = 0x7FF643DA5050`，因此 **`GNames = 0x7FF643DA5040` → RVA `0x0BDC5040`**。

### 4.2 名字解析

```
Block = Index >> 16 ;  Off = Index & 0xFFFF
Entry = *(u64*)(GNames + 0x10 + 8*Block) + Off*2

uint16 Header = *(u16*)Entry
  bit0      bIsWide
  bit1-5    ProbeHash
  bit6-15   Len
字符串紧跟在 Entry+2，长度为 Len（bIsWide 时是 UTF-16）
```

### 4.3 验证：反解出真实对象

用找到的池解析 `GObjects` 里的对象：

| 索引 | 对象名 | 类名 |
|---|---|---|
| 0 | `/Script/CoreUObject` | `Package` |
| 1 | `Object` | `Class` |
| 2 | `/Script/Engine` | `Package` |
| 11 | `Actor` | `Class` |
| 20000 | `AddVector2DParameterKey` | `Function` |
| 70000 | `SW_Mob_Bonker_SoulStated_Ability29` | `AbilityDefinitionExtended` |

77524 个对象中解析出 **63382 个有效名字**（其余为名字未初始化/已回收的槽位）。名字语义完全正确，确认无误。

---

## 五、GEngine 与 GWorld

枚举全部 77524 个对象并建立「对象地址 → 名字/类名」索引后，反查 `.data` 中指向对象的全局槽：

| 全局 | RVA | 指向 | 类型 | 结论 |
|---|---|---|---|---|
| `GEngine` | `0x0C03A1C0` | 唯一非 CDO 的 `GameEngine` 实例 | `UGameEngine` | ✅ |
| `GWorld` | `0x0C037A80` | `Menu_Spicewood` | `World` | ✅ |
| — | `0x0BA35810` | 同一个 `Menu_Spicewood` | — | ❌ 结构体字段 |

**怎么区分 `GWorld` 和 `0x0BA35810`（两者当前指向同一对象）**：
对两个地址做静态引用分析，`0x0BA35810` 在 `.text` 中**零引用**——说明它是某个结构体的字段（如 `FWorldContext::World`），不是全局变量。而 `0x0C037A80` 有 4 处读取，其中一处正是 `UObject::GetWorld()` 的兜底分支：

```asm
mov  rax, [rcx+0x18]      ; 先试 outer 链
test rax, rax
je   .fallback
mov  rax, [rax+0x30]
test rax, rax
jne  .ret
.fallback:
mov  rax, [rip+0x14C037A80]   ; ← GWorld
.ret:
ret
```

另一个读取点是一个单指令 getter（`mov rax,[GWorld]; ret`）。**`0x0C037A80` = `GWorld`**，确定。

---

## 六、更新后如何重新定位

硬编码偏移在每次游戏更新后都会失效。本仓库提供 **`re/Epsilon.py`**（纯标准库、只读、自验证）：

```bash
python re/Epsilon.py            # 完整报告
python re/Epsilon.py --json     # 机器可读，便于接进流水线
```

它会：
1. 枚举进程取模块基址（自动跳过打不开的高权限实例）；
2. 用 `.text` 特征码定位 `GObjects`，再跑结构自洽校验（`NumChunks == ceil(Num/65536)`、vtable 归属等），**校验不过就报 FAIL 而不是给错值**；
3. 扫描 `.data` 定位 `GNames`，并用名字反解做验证；
4. 读取 `GEngine`/`GWorld` 槽位并解析目标对象的类名来确认。

上次实测输出（全部 OK）：

```
结论: GObjects OK | GNames OK | GEngine OK | GWorld OK
```

`GEngine`/`GWorld` 目前仍用 RVA 常量 + 运行时验证。若某次更新后这两项 FAIL，重新跑一次「枚举对象 → 反查 `.data` 中指向 `GameEngine`/`World` 实例的槽位」即可（`re/find_engine2.py` 就是这个流程）。

---

## 七、工程注意事项

1. **不要写内存做持久化验证**：本项目此前已确认存档走服务端权威 + JWT，本地数值改动的收益极低（见 `docs/Dungeons2_逆向分析_外挂开发切入点.md`）。读取没问题，写入前先想清楚这条链路。
2. **`.data` 里指向对象的全局远不止这几个**：本次实测 `.data` 中有 **49514** 个槽指向 UObject（大多数是 `UPackage` 和 `UClass` 单例）。要什么全局，用「目标对象 → 反查槽位」的方法就能拿到，不需要猜。
3. **进程基址异常稳定**：本机多次重启游戏，`Dungeons-Win64-Shipping.exe` 基址都落在 `0x7FF637FE0000`。这是环境特性（可能是系统 ASLR 策略或启动器行为），**不应依赖**——工具里始终从进程读基址。
4. **`GObjects` 的 `NumElements` 会随加载增长**：实测主菜单时 77524。进入关卡后会更多，不要缓存该值。

---

## 八、验证记录

```
object      = E:\...\Dungeons-Win64-Shipping.exe
route       = REVERSE
baseline    = SHA256 7C83AFBF0AD34A40B853CDB25A22FFFB605D08E2A1E2D431974D7C7C1EE0BA54
action      = python re\Epsilon.py  (只读)
exit_status = 0
verification_1 = GObjects RVA 0x0BEA8BF0, NumElements=77524, NumChunks=2,
                 NumElements=ceil -> 自洽; 前 200 槽 vtable 200/200 落在 .rdata
verification_2 = GNames RVA 0x0BDC5040; 反解 [0]='/Script/CoreUObject':Package,
                 [1]='Object':Class, [70000]='SW_Mob_Bonker_SoulStated_Ability29'
verification_3 = GEngine RVA 0x0C03A1C0 -> 类名 'GameEngine'（唯一非 CDO 实例）
                 GWorld  RVA 0x0C037A80 -> 'Menu_Spicewood' 类名 'World'
rollback    = 无（本次全程只读，未修改目标文件或进程内存）
```
