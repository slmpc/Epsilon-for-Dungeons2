# D3D12 / DXGI COM vtable 索引

代码：`offsets::d3d12`、实现 [`Hooks.cpp`](../../src/payload/Hooks.cpp)

这不是「游戏内存偏移」，而是 **COM 接口的 vtable 索引**。列在这里是因为它同样是
「一个错了就会让目标进程崩掉」的魔法数字，且同样由 `Offsets.h` 统一持有。

⚠️ **永远不要凭记忆写 vtable 索引。** 错一次就是把 `jmp` 写到别的函数头上。

## `IDXGISwapChain::Present` = vtable\[8\]

索引 = 各父接口的虚函数个数之和：

| 接口 | 方法 | 个数 |
|---|---|---|
| `IUnknown` | `QueryInterface`, `AddRef`, `Release` | 3 |
| `IDXGIObject` | `SetPrivateData`, `SetPrivateDataInterface`, `GetPrivateData`, `GetParent` | 4 |
| `IDXGIDeviceSubObject` | `GetDevice` | 1 |
| **小计** | | **8** |
| `IDXGISwapChain` | `Present` 是接口自身第一个方法 → 索引 **8** | |

## `ID3D12CommandQueue::ExecuteCommandLists` = vtable\[10\]

| 接口 | 方法 | 累计 |
|---|---|---|
| `IUnknown` | 3 个 | 3 |
| `ID3D12Object` | `GetPrivateData`, `SetPrivateData`, `SetPrivateDataInterface`, `SetName` | 7 |
| `ID3D12DeviceChild` | `GetDevice` | 8 |
| `ID3D12Pageable` | （无新增） | 8 |
| `ID3D12CommandQueue` | `UpdateTileMappings`, `CopyTileMappings` → `ExecuteCommandLists` 排第三 | **10** |

## 定位方式（唯一路径，不做 fallback）

在目标进程里建一个**临时 D3D12 设备 + 命令队列 + 交换链**，从临时对象的 vtable 取地址：

```
D3D12CreateDevice → CreateCommandQueue → CreateSwapChainForHwnd
```

同一进程里 `dxgi` 的 `IDXGISwapChain` 实现共享 vtable，所以临时交换链的 `Present`
就是游戏在用的那个。

用 D3D12 而不是 D3D11：目标是 UE5 + D3D12 游戏，直接用它少引入一个 API 面。

## 两个必须注意的点

1. **`dxgi.dll` / `d3d12.dll` 加载后故意不 `FreeLibrary`。** 返回的 `Present` 地址就在
   `dxgi` 的代码段里，放掉引用计数会让模块卸载，地址立刻变野指针。
2. **取到的地址必须落在可执行模块内才采信**（`inExecutableModule`：`MEM_IMAGE` +
   `MEM_COMMIT` + 不是 `PAGE_GUARD`/`PAGE_NOACCESS` + 可执行保护位）。
   这是用于挡住「vtable 索引数错」与「取到未映射地址」的自检 ——
   有这道检查，索引写错时我们会「定位失败」而不是「把游戏钩崩」。

## 验证记录

`tests/d3dProbe` 在**普通进程**里逐字节验证过：

```
sizeof(DXGI_SWAP_CHAIN_DESC) = 72
vtbl[8] = Present
所在段属性 = EXECUTE_READ
```

真机注入验证（对 `Dungeons-Win64-Shipping` 执行 `hook`）输出：

```
[hook] D3D12 探测 hr=0x0 device=... queue=... swapchain=...
[hook] Present = 0x... (临时 D3D12 交换链 vtable[8])
[overlay] 就绪 (后缓冲 3 个, 格式 24, 2560x1600)
[overlay] 提交 bb=2 围栏完成=true
Present 钩子已生效, 游戏正在出帧
```

## 相关头文件

`IDXGIFactory2` / `IDXGISwapChain1` / `DXGI_SWAP_CHAIN_DESC1` 定义在 `dxgi1_2.h` 之后，
只 `#include <dxgi.h>` 是不够的 —— 项目里用的是 `#include <dxgi1_6.h>`（会把 1_1..1_6 全拉进来）。
