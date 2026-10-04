# 资源容器与存档的加密状况（实测）

> 回答「能不能逆 pak 里的资源」。结论：**能** —— 索引已解开、6910 个文件路径已列出；
> 数据体是 **Oodle 压缩 + AES 加密**，解密密钥有效，解压需要调用游戏自己的 Oodle。
> 测于 2026-10-04。

---

## 一、容器清单

`Dungeons\Content\Paks\` 下：

| 文件 | 大小 | 格式 |
|---|---|---|
| `Dungeons-Windows.utoc` | 17 MB | UE5 IoStore 索引，TOC version **8** |
| `Dungeons-Windows.ucas` | 9.1 GB | IoStore 数据体（主资源） |
| `global.utoc` / `global.ucas` | 890 B / 3.8 MB | 同上 |
| `Dungeons-Windows.pak` | 253 MB | 传统 UE pak，version **11**，6910 个松散资源 |

IoStore TOC 头实测（`global.utoc` 只有 1 个 entry，最容易对上）：

```
0x00  magic  "-==--==--==--==-"
0x10  u8     version            = 8
0x14  u32    TocHeaderSize      = 0x90
0x18  u32    TocEntryCount
0x1C  u32    TocCompressedBlockEntryCount
0x20  u32    TocCompressedBlockEntrySize  = 12
0x24  u32    CompressionMethodNameCount
0x28  u32    CompressionMethodNameLength  = 32
0x2C  u32    CompressionBlockSize         = 0x10000
0x30  u32    DirectoryIndexSize
末尾 20 字节为该 TOC 的 SHA-1
```

**`.utoc` 本身不加密**（熵 5.0~5.6，远低于随机），但里面的**目录索引是压缩的** ——
全文找不到任何资源路径串。

## 二、`.pak` 索引：已完全解开 ★

### 密钥

```
1f81803353095534b0544c9e26088a3d4db069c9727d38de649cb18e4fd4a3ec
```

UE 的 `FAES` 用 **AES-256-ECB**（无 IV、逐块独立），所以直接解就行。

**验证依据**（不是"解出来没报错"，而是解出来就是合法结构）：

```
明文索引头:
  0A 00 00 00              int32  挂载点长度 = 10
  "../../../\x00"          char[] 挂载点（经典 UE pak 挂载点）
  FE 1A 00 00              int32  文件数 = 6910
  E3 EC 45 19 00 00 00 00  uint64 PathHashSeed
  01 00 00 00              int32  有 PathHashIndex
  8A FC 1B 0F 00 00 00 00  int64  PathHashIndexOffset
  50 E9 02 00 00 00 00 00  int64  PathHashIndexSize
```

### FPakInfo 尾部布局（本构建 v11 实测）

```
+0x00 u32 magic 0x5A6F12E1
+0x04 i32 version = 11
+0x08 i64 IndexOffset
+0x10 i64 IndexSize
+0x18 u8  IndexHash[20]              (SHA-1)
+0x2C char CompressionMethodName[]   -> "Oodle"
```

⚠️ 尾部之后还有 ~128 字节全零，且**魔数不在文件最末**（在 EOF 前 0xCC 处），
按"从末尾倒着读固定长度"的常见写法会解析错。

### 索引结构（PathHashIndex 及以上版本）

```
int32   MountPointLength
char    MountPoint[]
int32   NumEntries
uint64  PathHashSeed
int32   bReaderHasPathHashIndex
        int64 PathHashIndexOffset, int64 PathHashIndexSize, uint8 Hash[20]
int32   bReaderHasFullDirectoryIndex
        int64 FullDirectoryIndexOffset, int64 FullDirectoryIndexSize, uint8 Hash[20]
int32   EncodedPakEntriesSize
uint8   EncodedPakEntries[]
```

`FullDirectoryIndex` 用 `(目录名, 文件数, [(文件名, 编码条目下标)])` 的形式给出全部路径，
本身也被加密 → 同样用上面的密钥解。

### 结果

**6910 个文件**，扩展名分布：

| 扩展名 | 个数 |
|---|---|
| `.res` | 3438 |
| `.png` | 1538 |
| `.json` | 687 |
| `.uplugin` | 325 |
| `.svg` | 313 |
| `.ini` | 128 |
| `.csv` | 108 |
| `.locres` | 87 |
| `.ufont` / `.tga` / `.ttf` / `.brk` / `.locmeta` / `.mtl` | 其余 |

里面包括 `Dungeons/Content/data/lovika/levels/*.json`（关卡数据）、
`Dungeons/Config/*.ini`（含 `DefaultEngine.ini` / `DefaultGame.ini` /
`DefaultGameplayTags.ini`）等。

> 注意：**主资源不在这里**。这个 pak 是松散内容包（图标/字体/本地化/关卡 JSON），
> 烘焙过的 `.uasset` 在 9.1 GB 的 IoStore `.ucas` 里。

## 三、`EncodedPakEntries` 的两种变体

`FullDirectoryIndex` 给的是 `(目录, 文件数, [(文件名, loc)])`，`loc` 是
**EncodedPakEntries 里的字节偏移**（不是文件偏移、也不是数组下标）。
长度实测**只有 12 与 20 两种**：

| 长度 | 形态 | 含义 |
|---|---|---|
| `12` | `{u32 flags, u32 pakOffset, u32 size}` | 未压缩（csize == usize，不必存两个） |
| `20` | `{u32 flags, u32 pakOffset, u32 usize, u32 csize, u32 extra}` | 压缩（两个大小都要存） |

6910 条里：**2591 条是 20 字节变体，4313 条是 12 字节变体**，另有 6 条读不出来。

`pakOffset` 处内联着一个 `FPakEntry`，**内容是明文**。判据不是「看着像」，而是
把读出来的 `Size` 与编码条目给的 `csize` 对账 —— **2591 条全部吻合**，
猜出来的结构不会这么齐：

```
int64  Offset          (内联时为 0)
int64  Size            压缩后大小
int64  UncompressedSize
u32    CompressionMethodIndex   (0 = 未压缩, 1 = Oodle)
u8     Hash[20]        SHA-1
[若压缩] int32 NumBlocks + NumBlocks*(int64 start, int64 end)
...  Flags / CompressionBlockSize
```

压缩条目示例（`Dungeons/Config/DefaultActorStateGimmicks.ini` @ `0x396800`）：

```
00000000 00000000   Offset = 0
A0050000 00000000   Size = 1440
BA360000 00000000   UncompressedSize = 13978
01000000            CompressionMethodIndex = 1 (Oodle)
CD7CE13E…           SHA-1 (20 字节)
01000000            NumBlocks = 1
49000000 00000000   block[0].start
E6050000 00000000   block[0].end
```

## 四、数据体：头部明文，载荷全高熵

- **`FPakEntry` 头部是明文**（见上，对账通过）。
- **载荷全部高熵**：整段解密后逐 MiB 检查没有一处可读；
  明文标志 `\r\n` 在 253 MB 里命中 3898 次，随机分布的期望值是 ≈3866 —— 贴着期望值。

那 2591 条（20 字节变体）的头部实测**全是 `method=1`（Oodle）**。

⚠️ **未解决**：那 4313 条 12 字节变体的头部写着 `method=0`，
**但载荷同样不是明文**。两种解释都没排除：

1. 它们其实也是 Oodle 压缩，只是 `method` 字段在 12 字节变体下读法不同；
2. 这些条目被**逐条加密**（`Flags` 字节实测是 `01`，与「加密」吻合），
   逐条加密的 ECB 对齐基点与整段解密不同，所以整段解密拿不到明文。

**判定它必须先打通 Oodle 通道** —— 解压成功的条目会立刻说明是压缩还是加密。
在此之前 **pak 载荷取不出来**；能拿到的是：完整文件清单 + 每个条目的大小/方法/哈希。

## 五、Oodle 是静态链接在 exe 里的 ★

游戏目录**没有** `oo2core*.dll`，但 exe 里 Oodle 是全的：

```
0x14a9a7d10  压缩级别名: HyperFast1..4 / SuperFast / VeryFast / Fast / Optimal1..5
0x14a9a7dd8  解码器名:  LZHLW / LZNIB / LZB16 / LZBLW / LZNA / Kraken / Mermaid
                       / BitKnit / Selkie / Hydra / Leviathan
0x14a9a86d8  "oo2::OodleLZ_Decompress"    <- 定位靠它
```

### 怎么定位到的（这个手法可复用）

Oodle 的日志宏把**函数名当字符串字面量嵌进函数体**。所以
**去找引用 `"oo2::OodleLZ_Decompress"` 的那个函数**，就是它本体：

| 函数 | 地址 | 判定 |
|---|---|---|
| `OodleLZ_Decompress` | `0x147CA8CE0` | 14 个形参，第 10 个是回调函数指针 —— 与 Oodle 公开签名逐项吻合 |
| `OodleLZ_Compress` | `0x147CA8400` | 同法 |
| `OodleLZ_Compressor_to_DecodeType` | `0x147CA6FC0` | 同法 |

> ⚠️ 别从 `.rdata` 的**纯名字串**找：`OodleLZ_Decompress`（`0x14a9a86dd`）
> **没有任何代码交叉引用**。要找的是**带 `oo2::` 前缀的那一份**（`0x14a9a86d8`）。
> 本次先在这个坑上白跑了一轮。

### 怎么用

注入体里已经加了 `oodle` 命令（[`CommandServer.cpp`](../../src/payload/CommandServer.cpp)），
把游戏自己的 `OodleLZ_Decompress` 当解压器使：

```
epsilon> oodle <输入文件> <输出文件> <原始长度>
```

输入应当是**已解密**的 Oodle 压缩数据。RVA 记在 `offsets::oodle`
（`OodleLZ_Decompress` 的 RVA = `0x07CA8CE0`）。

## 六、存档

`%LOCALAPPDATA%\Dungeons2\Saved\SaveGames\`

| 文件 | 状态 |
|---|---|
| `GlobalSaveDataDefault.sav` | ✅ **每字节 +1 即明文 JSON** |
| `entitlements.jwt.bin.sav` | ✅ 标准 UE `GVAS` |
| `auth_dynamic_ent.jwt.bin.sav` | ✅ 同上 |
| `OnlineDataTables.sav` | ❌ 与 pak 同级密文（非 zlib/gzip/deflate） |
| `Guid.bin.sav` | 二进制 GUID |

`GlobalSaveDataDefault.sav` 魔数 `z!aknar!`、全文 100% 可打印、熵 4.69：

```
z!aknar!   +1   {"blobs"
```

解出来是 7 个设置 blob：`Audio` / `General` / `Gfx` / `Controls` /
`KeyBindings` / `Interface` / `Accessibility`。
**只有全局设置，没有角色进度或货币** —— 所以
[../features/emerald.md](../features/emerald.md) 仍然走运行时改内存。

## 六、工具

| 脚本 | 作用 |
|---|---|
| [`probe_iostore.py`](../../analysis/re/probe_iostore.py) | IoStore 头部与压缩方法探测 |
| [`entropy_scan.py`](../../analysis/re/entropy_scan.py) | 分块熵/可打印率，判断明文/压缩/加密（换游戏通用） |
| [`unpak.py`](../../analysis/re/unpak.py) | `.pak` 索引解密 + 文件清单（`--key` 传密钥） |
| [`test_aes_key.py`](../../analysis/re/test_aes_key.py) | 验证一个候选密钥是否有效 |
| [`probe_pak_data.py`](../../analysis/re/probe_pak_data.py) | 判断数据区是明文还是压缩/加密 |
| [`decode_save.py`](../../analysis/re/decode_save.py) | 解开 `GlobalSaveDataDefault.sav` |

```powershell
python analysis/re/unpak.py --key <hex> --stat          # 列 6910 个文件 + 扩展名分布
```

## 七、下一步

1. 在 `0x147ca6xxx` 区里确认 `OodleLZ_Decompress` 的确切入口（可用签名匹配，
   或从 `FOodleDataCompression::Decompress` 的调用点反推）
2. 解析 `EncodedPakEntries` 得到每个文件的 offset/size/压缩标志
3. 在注入体里加一个 `oodle` 命令：把解密后的块交给游戏自己的 `OodleLZ_Decompress`
4. IoStore 那 9.1 GB **是否用同一把密钥尚未验证** —— 它的数据同样是高熵，
   Oodle 与加密哪个在先、密钥是否相同，都要等 Oodle 通道打通后才能判定
