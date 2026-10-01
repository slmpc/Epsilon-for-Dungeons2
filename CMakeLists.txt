cmake_minimum_required(VERSION 3.25)

# ============================================================================
#  Epsilon For Dungeons II
#  Minecraft Dungeons II (UE 5.6) — 热注入 + 运行时反射数据采集
#
#  目标:
#    epsilonInjector.exe  控制台前端: 定位进程 / RemoteThread 注入 / 管道 UI
#    epsilonPayload.dll   注入体: 运行时反射内省 + MinHook 帧回调 + 命令服务
# ============================================================================

project(epsilonForDungeons2
    VERSION 0.1.0
    DESCRIPTION "Epsilon For Dungeons II — hot-injection and runtime reflection framework"
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

# ---------------------------------------------------------------- 依赖: MinHook + ImGui + JSON (vcpkg)
# 依赖声明在 vcpkg.json 里(manifest 模式)。
# 配置时必须带上工具链文件, 本仓库的 CMakePresets.json 已内置:
#     cmake --preset release
# 或直接跑 .\scripts\build.ps1
find_package(minhook CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

message(STATUS "MinHook: ${minhook_DIR}")
message(STATUS "ImGui  : ${imgui_DIR}")
message(STATUS "JSON   : ${nlohmann_json_DIR}")

# ---------------------------------------------------------------- 公共层
add_library(epsilonCommon STATIC
    src/common/PeImage.cpp
    src/common/ProcUtil.cpp
    src/common/PipeChannel.cpp
    src/common/Protocol.cpp
    src/common/Text.cpp
)
target_include_directories(epsilonCommon PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(epsilonCommon PUBLIC EPSILON_VERSION="${PROJECT_VERSION}")
if(MSVC)
    target_compile_options(epsilonCommon PRIVATE /wd4996)
endif()
# psapi: GetModuleInformation(注入体在 DllMain 里量自己所在模块的大小)
target_link_libraries(epsilonCommon PUBLIC psapi)

# ---------------------------------------------------------------- 注入体 DLL
add_library(epsilonPayload SHARED
    src/payload/DllMain.cpp
    src/payload/Runtime.cpp
    src/payload/Payload.cpp
    src/payload/PipeClient.cpp
    src/payload/CommandServer.cpp
    src/payload/Hooks.cpp
    src/payload/Overlay.cpp
    src/payload/ue/Engine.cpp
    src/payload/ue/NamePool.cpp
    src/payload/ue/ObjectArray.cpp
    src/payload/ue/Reflection.cpp
    src/payload/ue/World.cpp
    src/payload/feature/settings/Setting.cpp
    src/payload/feature/module/Module.cpp
    src/payload/feature/module/ModuleManager.cpp
    src/payload/feature/config/ConfigManager.cpp
)
target_include_directories(epsilonPayload PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/payload")
# dxgi / d3d12 是必须的: 我们自己的代码走 GetProcAddress, 但 ImGui 的 DX12
# 后端(imgui_impl_dx12.cpp)直接调用了 CreateDXGIFactory1 等符号。
# shell32/ole32: ConfigManager 用 SHGetKnownFolderPath 取用户目录, 该函数
#   需要 ole32 的 CoTaskMemFree 来释放它返回的字符串。
target_link_libraries(epsilonPayload PRIVATE
    epsilonCommon
    minhook::minhook
    imgui::imgui
    nlohmann_json::nlohmann_json
    shell32
    ole32
    dxgi
    d3d12
)
set_target_properties(epsilonPayload PROPERTIES
    OUTPUT_NAME "epsilonPayload"
    PREFIX ""
    DEBUG_POSTFIX "")
if(MSVC)
    target_compile_options(epsilonPayload PRIVATE /Zi)
    target_link_options(epsilonPayload PRIVATE /DEBUG /INCREMENTAL:NO)
endif()

# ---------------------------------------------------------------- 注入器 EXE
add_executable(epsilonInjector
    src/injector/Main.cpp
    src/injector/Cli.cpp
    src/injector/PipeServer.cpp
)
target_include_directories(epsilonInjector PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/injector")
target_link_libraries(epsilonInjector PRIVATE epsilonCommon)

# ---------------------------------------------------------------- 自测靶子
# 一个最小 x64 控制台进程, 用来在不碰游戏的前提下验证整条注入链路:
#   起靶子 → 注入 → DLL 自报加载 → 采集 → 卸载
# 它故意什么都不做, 只等输入; 这样注入测试不会牵扯游戏。
add_executable(epsilonTestTarget EXCLUDE_FROM_ALL
    tests/testTarget/Main.cpp
)
target_link_libraries(epsilonTestTarget PRIVATE epsilonCommon)

# ---------------------------------------------------------------- 一次性诊断工具
# d3dProbe: 在普通进程里复现 payload 的"临时交换链取 Present"流程,
# 用来区分"D3D 调用本身的问题"和"注入上下文的问题"。
add_executable(d3dProbe EXCLUDE_FROM_ALL
    tests/d3dProbe/Main.cpp
)

# d3d12Target: 真实出帧的 D3D12 窗口程序。
# 没有游戏在跑时, 用它验证 Present 钩子与 ImGui 覆盖层。
add_executable(d3d12Target EXCLUDE_FROM_ALL
    tests/d3d12Target/Main.cpp
)
target_link_libraries(d3d12Target PRIVATE epsilonCommon d3d12 dxgi dxguid)

# pipeTest: 把命名管道双向收发单独拎出来验证。
# 注入链路里排查"某方向收不到消息"成本很高, 这个能直接定位是通道代码的
# 问题还是注入环境的问题。
add_executable(pipeTest EXCLUDE_FROM_ALL
    tests/pipeTest/Main.cpp
)
target_link_libraries(pipeTest PRIVATE epsilonCommon)

# DLL 与 EXE 同目录, 注入器默认就在这里找 payload
add_custom_command(TARGET epsilonInjector POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "== 产物 =="
    COMMAND ${CMAKE_COMMAND} -E echo "  $<TARGET_FILE:epsilonInjector>"
    COMMAND ${CMAKE_COMMAND} -E echo "  $<TARGET_FILE:epsilonPayload>"
    VERBATIM)

message(STATUS "EpsilonForDungeons2: C++${CMAKE_CXX_STANDARD} / toolchain ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
