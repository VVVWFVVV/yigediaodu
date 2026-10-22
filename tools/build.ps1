# ============================================================
# 一个调度 · 双构建脚本
#   host   ：WSL2 Ubuntu g++ 构建 + 模拟设备树测试
#   device ：NDK 交叉编译 arm64-v8a，产物落到 libs/arm64-v8a/xindiaodud
# 用法：
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -HostOnly
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -DeviceOnly -SkipTests
# ============================================================
param(
    [switch]$HostOnly,
    [switch]$DeviceOnly,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot

function Info([string]$m) { Write-Host "[build] $m" -ForegroundColor Cyan }
function Ok([string]$m)   { Write-Host "[build] $m" -ForegroundColor Green }
function Fail([string]$m) { Write-Host "[build] $m" -ForegroundColor Red; exit 1 }

# ---------------- 定位构建工具 ----------------
# 依次尝试：PATH -> WinGet Links -> Android SDK 自带 cmake
function Find-Tool([string]$name, [string[]]$extraPaths) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in $extraPaths) {
        if ($p -and (Test-Path $p)) { return $p }
    }
    return $null
}

$links = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links'
$sdk = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
$cmake = Find-Tool 'cmake' @("$links\cmake.exe", "$sdk\cmake\bin\cmake.exe", "$sdk\cmdline-tools\latest\bin\cmake.exe")
$ninja = Find-Tool 'ninja' @("$links\ninja.exe")
if (-not $cmake) { Fail "未找到 cmake（PATH / WinGet Links / SDK 均无）" }
if (-not $ninja) { Fail "未找到 ninja（PATH / WinGet Links 均无）" }
Info "cmake: $cmake"
Info "ninja: $ninja"

# ---------------- 版本号同步（防二进制版本漂移） ----------------
# 从 module.prop 提取 version= 并重写 src/main.cpp 的 kVersion 行。
# 教训：v1.0.5~v1.0.14 六轮忘记同步 kVersion（停在 1.0.4），二进制 --status
# 输出陈旧版本号，覆盖了面板从 module.prop 读到的正确版本显示。
# 今后升版本只改 module.prop，构建时自动同步到二进制。
# 注意：module.prop 的 version 带 v 前缀（v1.0.15），二进制 kVersion 用
# 无前缀形式（1.0.15）——面板 refreshTopbar 显示时会自己补 'v'，
# 带前缀会出现「vv1.0.15」。
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$modVer = (Select-String -Path "$RepoRoot\module.prop" -Pattern '^version=(.+)$' |
           Select-Object -First 1).Matches.Groups[1].Value.Trim()
if ($modVer) {
    $binVer = $modVer -replace '^v', ''
    $mainCpp = "$RepoRoot\src\main.cpp"
    $content = [System.IO.File]::ReadAllText($mainCpp, $utf8NoBom)
    $newContent = [regex]::Replace($content,
        'const char\* kVersion = "[^"]*";',
        ('const char* kVersion = "' + $binVer + '";'))
    if ($newContent -ne $content) {
        [System.IO.File]::WriteAllText($mainCpp, $newContent, $utf8NoBom)
        Info "kVersion 已同步: $binVer（module.prop: $modVer）"
    }
} else {
    Info "module.prop 未找到 version=，跳过 kVersion 同步"
}

# ---------------- 定位 NDK ----------------
function Find-Ndk {
    if ($env:ANDROID_NDK_HOME -and (Test-Path $env:ANDROID_NDK_HOME)) { return $env:ANDROID_NDK_HOME }
    $ndkDir = Join-Path $env:LOCALAPPDATA 'Android\Sdk\ndk'
    if (Test-Path $ndkDir) {
        $latest = Get-ChildItem $ndkDir -Directory | Sort-Object Name | Select-Object -Last 1
        if ($latest) { return $latest.FullName }
    }
    return $null
}

# ---------------- host 构建与测试（WSL） ----------------
# host 侧不强制 cmake：WSL 里没有 cmake 时直接 g++，保证任何时候可测可跑；
# device 侧始终走 CMake + NDK（唯一权威构建）。
function Invoke-HostBuild {
    Info "host 构建（WSL2 Ubuntu）..."
    $wslPath = '/mnt/d/新调度'
    $haveCmake = & wsl -d Ubuntu -- bash -lc "command -v cmake >/dev/null 2>&1 && echo yes || echo no"
    if ("$haveCmake".Trim() -eq "yes") {
        Info "检测到 WSL cmake，走 CMake 构建"
        & wsl -d Ubuntu -- bash -lc "cd '$wslPath' && cmake -S . -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Debug"
        if ($LASTEXITCODE -ne 0) { Fail "host cmake 配置失败" }
        & wsl -d Ubuntu -- bash -lc "cd '$wslPath' && cmake --build build-host"
    } else {
        Info "WSL 无 cmake，直接 g++ 构建"
        & wsl -d Ubuntu -- bash -lc "cd '$wslPath' && mkdir -p build-host && g++ -std=c++17 -g -O0 -Wall -Wextra src/*.cpp -o build-host/xindiaodud"
    }
    if ($LASTEXITCODE -ne 0) { Fail "host 构建失败" }
    Ok "host 构建完成: build-host/xindiaodud"

    if (-not $SkipTests) {
        Info "运行 host 测试（模拟设备树）..."
        & wsl -d Ubuntu -- bash -lc "cd '$wslPath' && bash test/run_tests.sh"
        if ($LASTEXITCODE -ne 0) { Fail "host 测试失败（详见上方输出）" }
        Ok "host 测试全部通过"

        # 前端冒烟测试（Node mock DOM：加载/boot/四页渲染/数据填充）
        if (Get-Command node -ErrorAction SilentlyContinue) {
            Info "运行前端冒烟测试（Node）..."
            node "$RepoRoot\test\js_smoke.js"
            if ($LASTEXITCODE -ne 0) { Fail "前端冒烟测试失败" }
            Ok "前端冒烟测试通过"
        } else {
            Info "未找到 node，跳过前端冒烟测试（不影响后端）"
        }
    }
}

# ---------------- device 构建（NDK） ----------------
function Invoke-DeviceBuild {
    $ndk = Find-Ndk
    if (-not $ndk) { Fail "未找到 NDK：请设置 ANDROID_NDK_HOME，或安装到 %LOCALAPPDATA%\Android\Sdk\ndk" }
    Info "device 构建（NDK: $ndk）..."

    $toolchain = Join-Path $ndk 'build\cmake\android.toolchain.cmake'
    if (-not (Test-Path $toolchain)) { Fail "toolchain 不存在: $toolchain" }

    & $cmake -S $RepoRoot -B "$RepoRoot\build-device" -G Ninja `
        -DCMAKE_MAKE_PROGRAM="$ninja" `
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" `
        -DANDROID_ABI=arm64-v8a `
        -DANDROID_PLATFORM=android-26 `
        -DANDROID_STL=c++_static `
        -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { Fail "device cmake 配置失败" }

    & $cmake --build "$RepoRoot\build-device"
    if ($LASTEXITCODE -ne 0) { Fail "device 构建失败" }

    # strip 瘦身：调试符号对设备端无意义
    $strip = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'
    if (Test-Path $strip) {
        & $strip "$RepoRoot\build-device\xindiaodud"
        Info "已 strip：$([math]::Round((Get-Item "$RepoRoot\build-device\xindiaodud").Length/1024)) KB"
    }

    $out = Join-Path $RepoRoot 'libs\arm64-v8a'
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    Copy-Item "$RepoRoot\build-device\xindiaodud" "$out\xindiaodud" -Force
    Ok "device 构建完成: libs\arm64-v8a\xindiaodud"
}

if (-not $DeviceOnly) { Invoke-HostBuild }
if (-not $HostOnly)  { Invoke-DeviceBuild }
Ok "全部完成"
