cmake_minimum_required(VERSION 3.25)

# ============================================================================
#  MCD2 Hot-Injection Framework
#  Minecraft Dungeons II (UE 5.6) — 热注入 + 运行时反射数据采集
#
#  目标:
#    mcd2_injector.exe  控制台前端: 定位进程 / RemoteThread 注入 / 管道 UI
#    mcd2_payload.dll   注入体: 运行时反射内省 + MinHook 帧回调 + 命令服务
# ============================================================================

project(mcd2_hotinject
    VERSION 0.1.0
    DESCRIPTION "MCD2 hot-injection and runtime reflection framework"
    LANGUAGES CXX)

# ---------------------------------------------------------------- 编译数据库
# 给 clangd 用。.vscode/settings.json 已经把 clangd 指到
#   build/release/compile_commands.json
# 也就是下面这个 preset 的构建目录。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------- 全局语言标准
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------- MSVC 专用
if(MSVC)
    # 统一 UTF-8 源码解析: 所有源文件按 UTF-8 读入, 窄字符串字面量用 UTF-8 执行字符集
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /W4 /MP)
    add_compile_options(/wd4100)             # 未使用形参(接口占位常见)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX                 # 阻止 windows.h 定义 min/max 宏, 与 <algorithm> 冲突
        WIN32_LEAN_AND_MEAN
        # 全项目只用 W 系列 API, 所以显式声明 Unicode。
        # 少了这两个宏, IDC_ARROW 之类的 MAKEINTRESOURCE 会解析成 ANSI 版本,
        # 传给 LoadCursorW 就会报 LPCWSTR 类型不匹配。
        UNICODE
        _UNICODE
        # 注意: 不要定义 WIN32_NO_STATUS —— 那会从 winnt.h 里抹掉
        # STATUS_WAIT_0 / STATUS_PENDING 等宏, 而 WaitForMultipleObjects 的
        # 返回值比较(WAIT_OBJECT_0)依赖它们。
    )
    # 静态 CRT: 产物不依赖 VC++ 运行库, 可直接拷进游戏目录
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# 产物统一落到 build/bin, 便于与 DLL 同目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# ---------------------------------------------------------------- 依赖: MinHook + ImGui (vcpkg)
# 依赖声明在 vcpkg.json 里(manifest 模式)。
# 配置时必须带上工具链文件, 本仓库的 CMakePresets.json 已内置:
#     cmake --preset release
# 或直接跑 .\scripts\build.ps1
find_package(minhook CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

message(STATUS "MinHook: ${minhook_DIR}")
message(STATUS "ImGui  : ${imgui_DIR}")

# ---------------------------------------------------------------- 公共层
add_library(mcd2_common STATIC
    src/common/pe_image.cpp
    src/common/proc_util.cpp
    src/common/pipe_channel.cpp
    src/common/protocol.cpp
    src/common/text.cpp
)
target_include_directories(mcd2_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(mcd2_common PUBLIC MCD2_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(mcd2_common PRIVATE /wd4996)
endif()
# psapi: GetModuleInformation(注入体在 DllMain 里量自己所在模块的大小)
target_link_libraries(mcd2_common PUBLIC psapi)

# ---------------------------------------------------------------- 注入体 DLL
add_library(mcd2_payload SHARED
    src/payload/dllmain.cpp
    src/payload/runtime.cpp
    src/payload/payload.cpp
    src/payload/pipe_client.cpp
    src/payload/command_server.cpp
    src/payload/hooks.cpp
    src/payload/overlay.cpp
    src/payload/ue/engine.cpp
    src/payload/ue/name_pool.cpp
    src/payload/ue/object_array.cpp
    src/payload/ue/reflection.cpp
    src/payload/ue/world.cpp
)
target_include_directories(mcd2_payload PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/payload")
# dxgi / d3d12 是必须的: 我们自己的代码走 GetProcAddress, 但 ImGui 的 DX12
# 后端(imgui_impl_dx12.cpp)直接调用了 CreateDXGIFactory1 等符号。
target_link_libraries(mcd2_payload PRIVATE
    mcd2_common
    minhook::minhook
    imgui::imgui
    dxgi
    d3d12
)
set_target_properties(mcd2_payload PROPERTIES
    OUTPUT_NAME "mcd2_payload"
    PREFIX ""
    DEBUG_POSTFIX "")
if(MSVC)
    target_compile_options(mcd2_payload PRIVATE /Zi)
    target_link_options(mcd2_payload PRIVATE /DEBUG /INCREMENTAL:NO)
endif()

# ---------------------------------------------------------------- 注入器 EXE
add_executable(mcd2_injector
    src/injector/main.cpp
    src/injector/cli.cpp
    src/injector/pipe_server.cpp
)
target_include_directories(mcd2_injector PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/injector")
target_link_libraries(mcd2_injector PRIVATE mcd2_common)

# ---------------------------------------------------------------- 自测靶子
# 一个最小 x64 控制台进程, 用来在不碰游戏的前提下验证整条注入链路:
#   起靶子 → 注入 → DLL 自报加载 → 采集 → 卸载
# 它故意什么都不做, 只等输入; 这样注入测试不会牵扯游戏。
add_executable(mcd2_testtarget EXCLUDE_FROM_ALL
    tests/test_target/main.cpp
)
target_link_libraries(mcd2_testtarget PRIVATE mcd2_common)

# ---------------------------------------------------------------- 一次性诊断工具
# d3d_probe: 在普通进程里复现 payload 的"临时交换链取 Present"流程,
# 用来区分"D3D 调用本身的问题"和"注入上下文的问题"。
add_executable(d3d_probe EXCLUDE_FROM_ALL
    tests/d3d_probe/main.cpp
)

# d3d12_target: 真实出帧的 D3D12 窗口程序。
# 没有游戏在跑时, 用它验证 Present 钩子与 ImGui 覆盖层。
add_executable(d3d12_target EXCLUDE_FROM_ALL
    tests/d3d12_target/main.cpp
)
target_link_libraries(d3d12_target PRIVATE mcd2_common d3d12 dxgi dxguid)

# pipe_test: 把命名管道双向收发单独拎出来验证。
# 注入链路里排查"某方向收不到消息"成本很高, 这个能直接定位是通道代码的
# 问题还是注入环境的问题。
add_executable(pipe_test EXCLUDE_FROM_ALL
    tests/pipe_test/main.cpp
)
target_link_libraries(pipe_test PRIVATE mcd2_common)

# DLL 与 EXE 同目录, 注入器默认就在这里找 payload
add_custom_command(TARGET mcd2_injector POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "== 产物 =="
    COMMAND ${CMAKE_COMMAND} -E echo "  $<TARGET_FILE:mcd2_injector>"
    COMMAND ${CMAKE_COMMAND} -E echo "  $<TARGET_FILE:mcd2_payload>"
    VERBATIM)

message(STATUS "MCD2 hot-inject: C++${CMAKE_CXX_STANDARD} / toolchain ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
