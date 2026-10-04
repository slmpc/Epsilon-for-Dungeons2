<#
.SYNOPSIS
    配置并编译 Epsilon For Dungeons II (Ninja + MSVC + vcpkg)。

.DESCRIPTION
    为什么需要这个脚本:
      本机 PATH 里 D:\Programs\LLVM\bin 排在 MSVC 前面, 直接用 cmake 配 Ninja
      生成器会让 CMake 抓到 clang++ 而不是 cl.exe。而 Ninja + MSVC 不仅需要
      cl.exe 可见, 还需要 INCLUDE / LIB / LIBPATH 等环境变量 —— 这些只有
      vcvars64.bat 才会设置。

    所以这里先把 vcvars64 的环境导入当前 pwsh 进程, 再调 cmake。

.PARAMETER Preset
    CMake preset 名。默认 release。

.PARAMETER Target
    只编译指定 target (例如 epsilonTestTarget)。留空编译全部。

.PARAMETER Fresh
    删掉现有 build 目录重新配置。

.PARAMETER ConfigureOnly
    只配置不编译。

.EXAMPLE
    .\scripts\build.ps1
    .\scripts\build.ps1 -Preset debug -Fresh
    .\scripts\build.ps1 -Target epsilonTestTarget
#>
[CmdletBinding()]
param(
    [string]$Preset = 'release',
    [string]$Target = '',
    [switch]$Fresh,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- 定位 VS
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "找不到 vswhere.exe: $vswhere`n请确认已安装 Visual Studio (含 C++ 工作负载)。"
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) {
    throw "vswhere 找不到带 C++ 工具链的 VS 安装。"
}

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "找不到 vcvars64.bat: $vcvars"
}

Write-Host "[*] VS      : $vsPath" -ForegroundColor Cyan
Write-Host "[*] vcvars  : $vcvars" -ForegroundColor Cyan

# ---------------------------------------------------------------- 导入 vcvars64
# 用 cmd 跑一次 vcvars64 再把它的 "set" 输出回灌到本进程。
# 这是让 Ninja 找到 MSVC 的标准做法, 比手写 INCLUDE/LIB 可靠得多。
$dump = & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0 -or -not $dump) {
    throw "执行 vcvars64.bat 失败 (exit=$LASTEXITCODE)"
}

$applied = 0
foreach ($line in $dump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
        $applied++
    }
}
Write-Host "[*] 已导入 $applied 个环境变量" -ForegroundColor Cyan

# ---------------------------------------------------------------- 控制台代码页
# ★ 必须在配置之前切成 UTF-8, 否则**头文件依赖会被静默丢掉**。
#
#   Ninja 靠 cl.exe 的 /showIncludes 输出来建头文件依赖图, 前缀由 CMake 在配置期
#   探测 cl.exe 的实际输出得到, 写进 CMakeFiles\rules.ninja 的 msvc_deps_prefix。
#   中文区域下 cl.exe 输出 GBK, CMake 却按 UTF-8 解码 —— 于是写进去的是双编码
#   乱码(实测 `娉ㄦ剰: 鍖呭惈鏂囦欢:`), Ninja 拿它去匹配 cl.exe 的 GBK 输出永远
#   匹配不上。
#
#   后果不是报错, 而是**构建"成功"但产物是旧的**: 只改 .h 的改动不会触发任何
#   重编译。踩过一次 —— 改了 Offsets.h 里的四个引擎 RVA, DLL 里仍是旧值。
#
#   切到 UTF-8 代码页后 configure 与 build 两侧都是 UTF-8, 前缀能对上。
#   注意 VSLANG=1033 对 /showIncludes **无效**(实测), 别指望它。
& chcp.com 65001 | Out-Null
$env:PYTHONIOENCODING = 'utf-8'
Write-Host "[*] 代码页  : 65001 (UTF-8, 保证 Ninja 能解析 /showIncludes 前缀)" -ForegroundColor Cyan

# 确认现在抓到的确实是 MSVC 而不是 clang
$cl = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
if (-not $cl) {
    throw "导入 vcvars64 后 PATH 里仍然没有 cl.exe。"
}
Write-Host "[*] 编译器: $cl" -ForegroundColor Green

if ($cl -match 'LLVM|clang') {
    throw "PATH 里优先命中的是 clang ($cl), 不是 MSVC。请检查 LLVM 是否被放在 MSVC 之前。"
}

# ---------------------------------------------------------------- vcpkg
# 注意: vcvars64.bat 会把 VCPKG_ROOT 设成 VS 自带的 vcpkg
#       (…\Microsoft Visual Studio\18\Community\VC\vcpkg), 而我们的
#       vcpkg.json baseline 与已装好的 minhook 都在用户自己的 vcpkg 里。
#       所以这里必须**强制覆盖**, 不能只在未设置时才兜底。
$preferred = 'D:\Programs\vcpkg'
if (Test-Path (Join-Path $preferred 'vcpkg.exe')) {
    if ($env:VCPKG_ROOT -and $env:VCPKG_ROOT -ne $preferred) {
        Write-Host "[!] vcvars 把 VCPKG_ROOT 设成了 $env:VCPKG_ROOT" -ForegroundColor Yellow
        Write-Host "[!] 强制改回 $preferred (MinHook 与 baseline 在这里)" -ForegroundColor Yellow
    }
    $env:VCPKG_ROOT = $preferred
} elseif (-not $env:VCPKG_ROOT) {
    throw "VCPKG_ROOT 未设置, 且 $preferred 下找不到 vcpkg.exe。"
}
Write-Host "[*] vcpkg   : $env:VCPKG_ROOT" -ForegroundColor Cyan

# ---------------------------------------------------------------- 清场
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build\$Preset"

if ($Fresh -and (Test-Path $buildDir)) {
    Write-Host "[*] -Fresh: 删除 $buildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

Push-Location $repoRoot
try {
    # ------------------------------------------------------------ 配置
    Write-Host "`n=== cmake --preset $Preset ===" -ForegroundColor Cyan
    & cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败 (exit=$LASTEXITCODE)" }

    if ($ConfigureOnly) {
        Write-Host "[+] 配置完成 (未编译)" -ForegroundColor Green
        return
    }

    # ------------------------------------------------------------ 编译
    Write-Host "`n=== cmake --build --preset $Preset ===" -ForegroundColor Cyan
    if ($Target) {
        & cmake --build $buildDir --target $Target
    } else {
        & cmake --build --preset $Preset
    }
    if ($LASTEXITCODE -ne 0) { throw "编译失败 (exit=$LASTEXITCODE)" }

    # ------------------------------------------------------------ 产物
    $bin = Join-Path $buildDir 'bin'
    Write-Host "`n=== 产物 ($bin) ===" -ForegroundColor Green
    if (Test-Path $bin) {
        Get-ChildItem $bin -File |
            Where-Object { $_.Extension -in '.exe', '.dll' } |
            Sort-Object Name |
            Format-Table @{ n = '文件'; e = { $_.Name } },
                         @{ n = '大小'; e = { '{0:N0} B' -f $_.Length } },
                         @{ n = '时间'; e = { $_.LastWriteTime.ToString('HH:mm:ss') } } -AutoSize
    } else {
        Write-Host "[!] 没有 $bin 目录" -ForegroundColor Yellow
    }
}
finally {
    Pop-Location
}
