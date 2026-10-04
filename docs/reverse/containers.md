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

## 三、数据体：Oodle 压缩 + 加密

对数据区 `[0, IndexOffset)` 取样，分别按「原样」与「AES 解密」两种读法统计：

| 读法 | 可打印率 |
|---|---|
| 原样 | 0.342 ~ 0.373 |
| AES-256-ECB 解密 | 0.368 ~ 0.374 |

均匀随机字节的可打印率期望 = `95/256 ≈ 0.371`。**两种读法都贴着它**，
且明文字节特征 `\r\n` 在 253 MB 里命中 3812 次 —— 恰好等于随机分布下的期望值（≈3862）。

**结论：数据区没有任何明文。** 索引能解而数据解不出可读内容，唯一解释是
**先 Oodle 压缩再加密**（压缩后的流本来就像随机数，再加密仍是随机数）。

## 四、Oodle 是静态链接在 exe 里的 ★

游戏目录**没有** `oo2core*.dll`，但 exe 里有完整的 Oodle：

```
0x14a9a7c58  'v:\devel\projects\oodle2\core\oodlecoreplugins_gen.inc'
0x14a9a7d10  压缩级别名: HyperFast1..4 / SuperFast / VeryFast / Fast / Optimal1..5
0x14a9a7dd8  解码器名:  LZHLW / LZNIB / LZB16 / LZBLW / LZNA / Kraken / Mermaid
                       / BitKnit / Selkie / Hydra / Leviathan
0x14a9a7ed0  'OODLE ERROR : Legacy LZ VTable not installed'
```

代码集中在 **`0x147ca6xxx`** 一带：

| 地址 | 大小 | 备注 |
|---|---|---|
| `0x147ca6cc0` | `0xc2` | 引用 Oodle 源文件路径串 |
| `0x147ca6ea0` | `0x112` | 同上 |
| `0x147ca6fc0` | `0x158` | 同上 |
| `0x147ca7240` | `0x27c` | 同上 |
| `0x147ca75c0` | `0xddd` | 大函数，`OodleLZ_Decompress` 候选 |
| `0x147ca83a0` | `0x5e` | `OodleLZDecoder_MemorySizeNeeded` 相关 |

> `OodleLZ_Decompress` 之类**函数名字符串**在 `.rdata` 里的名字表（`0x14a9a86dd`），
> 没有代码交叉引用 —— 别指望靠名字串找到函数体，要靠签名或从调用者上溯。

**这意味着解压是可行的**：不需要第三方 Oodle SDK，直接调用游戏自己的
`OodleLZ_Decompress` 即可 —— 而注入通路本来就是通的（
见 [../payload/lifecycle.md](../payload/lifecycle.md)）。

`OodleLZ_Decompress` 的签名（Oodle 公开头文件）：

```c
OO_SINTa OodleLZ_Decompress(const void* compBuf, OO_SINTa compBufSize,
                            void* rawBuf, OO_SINTa rawLen,
                            OodleLZ_FuzzSafe, OodleLZ_CheckCRC, OodleLZ_Verbosity,
                            void* decBufBase, OO_SINTa decBufSize,
                            OodleLZ_DecompressCallback*, void* userData,
                            void* decoderMemory, OO_SINTa decoderMemorySize,
                            OodleLZ_Decode_ThreadPhase);
```

## 五、存档

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
