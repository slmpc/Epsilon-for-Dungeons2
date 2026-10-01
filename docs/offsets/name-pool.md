# FNamePool（GNames）

代码：`offsets::namePool`、实现 `ue::NamePool`
（[`NamePool.cpp`](../../src/payload/game/ue/NamePool.cpp)）

UE5 的名字不再放在 `TNameEntryArray` 里，而是 `FNamePool`。

## 布局

```
FNameEntryAllocator            （GNames 本体）
    +0x00  FRWLock  Lock        （8 字节，我们不用）
    +0x08  uint32   CurrentBlock
    +0x0C  uint32   CurrentByteCursor
    +0x10  uint8*   Blocks[8192]   ← 我们只关心这个

每个 Block 固定 64 KiB（0x10000）
```

因此代码里 `NamePool` 持有的不是 `GNames` 本体，而是 **`GNames + 0x10`**
（`FNameEntryAllocator::Blocks` 的地址）。

## 索引编码

```
index = (Block << 16) | ByteOffset
```

⚠️ **索引里的 ByteOffset 是「字节偏移右移一位」后的值**，读的时候要乘以 2 还原。
UE 这么做是因为条目按 2 字节对齐，右移一位能多换出一位寻址范围。

这一点必须实测确认。凭「索引就是字节偏移」的直觉写，会得到**完全错误但看起来像有数据**
的结果 —— 索引落在某个条目中间，解出的「名字」是别扭的 UTF-16 乱码，而地址、布局、
校验全都正常，极难定位。

实测依据（两个独立样本，都精确落在条目边界上）：

| 索引 | ×2 | 该处字节 | 解出的名字 |
|---|---|---|---|
| `0x6EB` | `0xDD6` | `F6 04` + … | `/Script/CoreUObject`（19 字符） |
| `0x20B` | `0x416` | `80 01` + … | `Object`（6 字符）← UObject 的 CDO |

## FNameEntry 布局

```
[uint16 header][name bytes]   然后按 2 字节对齐
```

头部是紧凑位域：

| 位 | 含义 |
|---|---|
| bit0 | `bIsWide`（1 = UTF-16） |
| bit1..5 | ProbeHash（这里不用） |
| bit6..15 | `Len`（10 bit，最大 1023） |

⚠️ **`FNameEntry` 不带 NUL 终止符**，长度完全由头部的 `Len` 位域给出。

如果把名字当 C 字符串读（要求 `str[len] == '\0'`），那么**每一条都会解析失败** ——
曾经因此把一个完全正确的 `FNamePool` 判成无效地址。

实测某块的头 32 字节：

| 字节 | 名字 | 占用 |
|---|---|---|
| `1E 01` | `None`（4 字符） | 2+4 = 6 |
| `10 03` | `ByteProperty`（12 字符） | 2+12 = 14 |
| `C0 02` | `IntProperty`（11 字符） | 2+11 = 13 → 对齐到 14 |

## 校验：`score()` 必须顺序走条目链

⚠️ **不能改成「按索引 0..N 采样」。**

索引是 `(Block << 16) | ByteOffset` —— 相邻索引不是相邻条目，而是同一个块里相差
1 字节的两个位置。按 `0,1,2,…,N` 采样的话，绝大多数会落在某个条目的中间，解析必然失败。

这个错误曾经让一个**完全正确**的 `FNamePool` 地址被判为无效：真机上前 64 个索引
「只命中 4/64」，看起来像地址偏了，实际上地址和布局都对（dump 出来是
`None` / `ByteProperty` / `IntProperty` …），只是校验方式错了。

所以 `score()` 从**块 0 的起始处逐条往前走**，走完一个 64 KiB 块为止。
定位 `GNames` 的门槛是：前 64 个条目至少 32 个能解出名字。

## 按名字反查索引

`NamePool::findByName()` 同样顺序走块 0 的条目链，命中后按
`index = (byteOffset >> 1)` 反推比较索引。`findprop` 命令靠它把属性名换成 FName 索引，
再回 `UClass` 内存里搜这个索引（见 [../reverse/reflection-limits.md](../reverse/reflection-limits.md)）。

名字池第一个块足够容纳我们关心的全部引擎属性名，所以只走块 0。
