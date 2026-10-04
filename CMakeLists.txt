cmake_minimum_required(VERSION 3.25)

# Epsilon For Dungeons II — Minecraft Dungeons II (UE 5.6): 热注入 + 运行时反射数据采集

project(epsilonForDungeons2
    VERSION 0.1.0
    DESCRIPTION "Epsilon For Dungeons II — hot-injection and runtime reflection framework"
    LANGUAGES CXX)

# clangd 用: .vscode/settings.json 指向 build/release/compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # 统一 UTF-8 源码解析: 所有源文件按 UTF-8 读入, 窄字符串字面量用 UTF-8 执行字符集
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /W4 /MP)
    add_compile_options(/wd4100)             # 未使用形参(接口占位常见)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX                 # 阻止 windows.h 定义 min/max 宏, 与 <algorithm> 冲突
        WIN32_LEAN_AND_MEAN
        # 全项目只用 W 系列 API: 少了这两个宏, IDC_ARROW 之类的 MAKEINTRESOURCE
        # 会解析成 ANSI 版本, 传给 LoadCursorW 报 LPCWSTR 类型不匹配。
        UNICODE
        _UNICODE
        # 不要定义 WIN32_NO_STATUS: 它会抹掉 STATUS_WAIT_0 / STATUS_PENDING 等宏,
        # 而 WaitForMultipleObjects 的返回值比较(WAIT_OBJECT_0)依赖它们。
    )
    # 静态 CRT: 产物不依赖 VC++ 运行库, 可直接拷进游戏目录
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# 产物统一落到 build/bin, 与 DLL 同目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# 依赖声明在 vcpkg.json (manifest 模式); 工具链文件由 CMakePresets.json 提供。
find_package(minhook CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

message(STATUS "MinHook: ${minhook_DIR}")
message(STATUS "ImGui  : ${imgui_DIR}")
message(STATUS "JSON   : ${nlohmann_json_DIR}")

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

add_library(epsilonPayload SHARED
    src/payload/DllMain.cpp
    src/payload/Runtime.cpp
    src/payload/Payload.cpp
    src/payload/PipeClient.cpp
    src/payload/CommandServer.cpp
    src/payload/Hooks.cpp
    src/payload/Overlay.cpp
    src/payload/game/Field.cpp
    src/payload/game/ue/Engine.cpp
    src/payload/game/ue/NamePool.cpp
    src/payload/game/ue/ObjectArray.cpp
    src/payload/game/ue/Reflection.cpp
    src/payload/game/ue/World.cpp
    src/payload/game/dungeons2/Player.cpp
    src/payload/game/dungeons2/Movement.cpp
    src/payload/game/dungeons2/MovementResolver.cpp
    src/payload/game/dungeons2/Currency.cpp
    src/payload/feature/settings/Setting.cpp
    src/payload/feature/module/Module.cpp
    src/payload/feature/module/ModuleManager.cpp
    src/payload/feature/config/ConfigManager.cpp
    src/payload/feature/GameContext.cpp
    src/payload/feature/Modules.cpp
    src/payload/feature/module/impl/Jump.cpp
    src/payload/feature/module/impl/Speed.cpp
    src/payload/feature/module/impl/Emerald.cpp
    src/payload/feature/ui/FeaturePanel.cpp
)
target_include_directories(epsilonPayload PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/payload")
# dxgi/d3d12 必须显式链接: 我们的代码走 GetProcAddress, 但 ImGui 的 DX12 后端直接调用
# CreateDXGIFactory1 等符号。shell32/ole32: SHGetKnownFolderPath 的返回值要 CoTaskMemFree 释放。
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

add_executable(epsilonInjector
    src/injector/Main.cpp
    src/injector/Cli.cpp
    src/injector/PipeServer.cpp
    # 应用清单: requireAdministrator(UAC)。游戏进程完整性级别更高,
    # 不提权 OpenProcess 直接返回 5 —— 清单让缺权限这件事在启动时就可见。
    src/injector/epsilonInjector.manifest
)
target_include_directories(epsilonInjector PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/injector")
target_link_libraries(epsilonInjector PRIVATE epsilonCommon)
if(MSVC)
    # 清单由上一条的 .manifest 提供; 关掉链接器自带的那一份, 否则两段
    # requestedExecutionLevel 会在 mt.exe 合并时报 c1010001(level 值不一致)。
    target_link_options(epsilonInjector PRIVATE "/MANIFESTUAC:NO")
endif()

# 最小 x64 控制台靶子: 不碰游戏验证整条注入链路。
add_executable(epsilonTestTarget EXCLUDE_FROM_ALL
    tests/testTarget/Main.cpp
)
target_link_libraries(epsilonTestTarget PRIVATE epsilonCommon)

# d3dProbe: 在普通进程里复现"临时交换链取 Present", 区分 D3D 调用问题与注入上下文问题。
add_executable(d3dProbe EXCLUDE_FROM_ALL
    tests/d3dProbe/Main.cpp
)

# d3d12Target: 真实出帧的 D3D12 窗口程序, 无游戏时验证 Present 钩子与覆盖层。
add_executable(d3d12Target EXCLUDE_FROM_ALL
    tests/d3d12Target/Main.cpp
)
target_link_libraries(d3d12Target PRIVATE epsilonCommon d3d12 dxgi dxguid)

# pipeTest: 单独验证命名管道双向收发, 定位"某方向收不到消息"。
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
