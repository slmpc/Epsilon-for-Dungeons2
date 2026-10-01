# FUObjectArray（GObjects）与 UObject

代码：`offsets::objectArray` / `offsets::object` / `offsets::array`、
遍历实现 `ue::ObjectArray`（[`ObjectArray.cpp`](../../src/payload/game/ue/ObjectArray.cpp)）

实测于活体进程（[`analysis/re/epsilon.py`](../../analysis/re/epsilon.py) 亦验证过同一套布局）。

## 布局

```
GObjects  (FUObjectArray)
    +0x10  TUObjectArray  ← **内嵌**，不是再一层指针
           +0x10  FUObjectItem**  ChunkTable
           +0x20  int32           MaxElements
           +0x24  int32           NumElements
           +0x28  int32           MaxChunks
           +0x2C  int32           NumChunks

FUObjectItem（步长 0x18）
    +0x00  UObject*
    +0x08  EObjectFlags
    +0x0C  int32          ClusterRootIndex
    +0x10  int32          SerialNumber

UObject
    +0x08  EObjectFlags
    +0x0C  int32          InternalIndex   ← 对象自己的索引
    +0x10  UClass*        ClassPrivate
    +0x18  FName          NamePrivate
    +0x20  UObject*       OuterPrivate
```

> ⚠️ `TUObjectArray` 是**内嵌**在 `FUObjectArray` 里的（`+0x10`），不是「指针再解一层」。
> 块表和计数都在 `GObjects` 上直接读。先解一层引用再 `+0x10` 会读到完全无关的地方，
> 现象是「读不到 FUObjectArray」。

## 取值链

```
T  = *(u64*)(GObjects + 0x10)              // TUObjectArray
Ch = *(u64*)(T + 8 * (index >> 16))        // ChunkTable[index / 65536]
Obj= *(u64*)(Ch + 0x18 * (index & 0xFFFF)) // FUObjectItem.Object
```

每个 chunk 固定 `0x10000`（65536）个 item。

## 真机实测字节（判定该布局正确的依据）

UE 5.6.1 上 `GObjects` 处的实际内存：

| 偏移 | 值 |
|---|---|
| `+0x10` | 块表指针 |
| `+0x20` `MaxElements` | `2162688` |
| `+0x24` `NumElements` | `149034` |
| `+0x28` `MaxChunks` | `33` |
| `+0x2C` `NumChunks` | `3` |

自洽推算：

```
149034 / 65536 向上取整 = 3     == NumChunks
2162688 / 65536         = 33    == MaxChunks
```

两组数字完全自洽，这就是判定「这个地址确实是 `FUObjectArray`」的依据。

## 其它

- 取类名：`UClass` 本身也是 `UObject`，所以类名 = `*(FName*)(ClassPrivate + 0x18)`
- `fullNameOf()` 的格式是 `ClassName Name`
- 全表扫描约 15 万个槽位；`for_each` 会自动跳过空槽
- 按名字查对象的三个入口（`findObjectByName` / `findObjectByFullName` / `findClass`）都是
  **全量线性扫描，不区分大小写**
