# ============================================================
# 一个调度 · 打包脚本（Windows 主力）
# 校验：必需文件齐全 / .sh 无 CRLF / 二进制为 arm64 ELF
# 产物：dist/<模块id>-v<版本>.zip（条目均在 zip 根，可直接刷入）
# ============================================================
param(
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot

function Info([string]$m) { Write-Host "[pack] $m" -ForegroundColor Cyan }
function Ok([string]$m)   { Write-Host "[pack] $m" -ForegroundColor Green }
function Fail([string]$m) { Write-Host "[pack] $m" -ForegroundColor Red; exit 1 }

# ---------------- 版本 ----------------
$ver = (Select-String -Path "$RepoRoot\module.prop" -Pattern '^version=(.+)$').Matches.Groups[1].Value.Trim()
if (-not $ver) { $ver = "v1.0" }
# zip 名前缀取模块 id（v1.0.16 起 id=WY925976742，不再硬编码）
$modId = (Select-String -Path "$RepoRoot\module.prop" -Pattern '^id=(.+)$' | Select-Object -First 1).Matches.Groups[1].Value.Trim()
if (-not $modId) { $modId = "unknown" }

# ---------------- 必需文件清单 ----------------
$required = @(
    "module.prop",
    "customize.sh",
    "post-fs-data.sh",
    "service.sh",
    "action.sh",
    "uninstall.sh",
    "system.prop",
    "META-INF/com/google/android/update-binary",
    "script/pathinfo.sh",
    "script/setup.sh",
    "script/fcctl.sh",
    "script/diag.sh",
    "config/settings.conf",
    "config/profiles/daily.conf",
    "config/profiles/daily-plus.conf",
    "config/profiles/game.conf",
    "config/profiles/bg.conf",
    "config/profiles/off.conf",
    "config/perapp/com.miHoYo.Yuanshen.conf",
    "config/perapp/com.tencent.tmgp.sgame.conf",
    "config/perapp/com.tencent.tmgp.pubgmhd.conf",
    "webroot/index.html",
    "webroot/assets/app.css",
    "webroot/assets/bridge.js",
    "webroot/assets/api.js",
    "webroot/assets/app.js",
    "webroot/assets/pages.js",
    "webroot/assets/page-settings.js",
    "webroot/assets/page-diag.js",
    "libs/arm64-v8a/xindiaodud"
)

Info "校验必需文件..."
$missing = @()
foreach ($f in $required) {
    if (-not (Test-Path (Join-Path $RepoRoot $f))) { $missing += $f }
}
if ($missing.Count -gt 0) {
    Fail "缺少必需文件：`n  $($missing -join "`n  ")"
}
Ok "必需文件齐全（$($required.Count) 项）"

# ---------------- CRLF 检查 ----------------
# 覆盖所有会进 zip 的文件（与下方打包枚举同一套排除规则），含无扩展名的
# 安装脚本 META-INF/com/google/android/update-binary——教训：v1.0.15 先用
# 扩展名白名单检查，漏掉了它（code-explorer 复核实锤），CRLF 的刷入脚本是
# 「刷入失败」的常见诱因（shebang 带 \r）。仅跳过 libs/ 下的 ELF 二进制
# （其内容可能合法包含 0x0D 0x0A 序列）。
Info "检查 CRLF..."
$excludeTop = @('build-device', 'build-host', 'dist', 'test', 'tools')
$crlf = @()
Get-ChildItem $RepoRoot -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($RepoRoot.Length + 1)
    $top = ($rel -split '[\\/]')[0]
    if ($excludeTop -contains $top) { return }
    if ($top -eq '.codebuddy') { return }
    if ($top -eq 'libs') { return }  # ELF 二进制：内容可能合法含 0D 0A
    $bytes = [IO.File]::ReadAllBytes($_.FullName)
    for ($i = 0; $i -lt $bytes.Length - 1; $i++) {
        if ($bytes[$i] -eq 13 -and $bytes[$i+1] -eq 10) { $crlf += $rel; break }
    }
}
if ($crlf.Count -gt 0) { Fail "以下文件含 CRLF：`n  $($crlf -join "`n  ")" }
Ok "全部打包文件为 LF 行尾"

# ---------------- 二进制架构检查 ----------------
Info "检查守护二进制架构..."
$bin = Join-Path $RepoRoot "libs\arm64-v8a\xindiaodud"
$bytes = [IO.File]::ReadAllBytes($bin)
if ($bytes.Length -lt 20 -or $bytes[0] -ne 0x7F -or $bytes[1] -ne 0x45 -or $bytes[2] -ne 0x4C -or $bytes[3] -ne 0x46) {
    Fail "libs/arm64-v8a/xindiaodud 不是 ELF 文件"
}
# e_machine 位于偏移 18（小端）：0xB7 0x00 = AArch64(183)
if (-not ($bytes[18] -eq 0xB7 -and $bytes[19] -eq 0x00)) {
    Fail "二进制不是 AArch64 架构（e_machine=$($bytes[18]),$($bytes[19])）"
}
Ok "二进制为 AArch64 ELF（$([math]::Round($bytes.Length/1024)) KB）"

# ---------------- 组装 zip 条目 ----------------
# 直接从仓库根打包（不用临时目录：$env:TEMP 可能是 8.3 短名，
# 与 Get-ChildItem 返回的长名长度不一致，字符串裁剪会出错）
Info "组装打包内容..."
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "dist" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path $OutDir "$modId-$ver.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }

# $excludeTop 已在 CRLF 检查段定义（两处枚举规则必须一致）

# zip 条目的 Unix 权限位（部分安装器不 chmod，自己带上最稳）：
# 0x81ED = 普通文件 + 0755，0x81A4 = 普通文件 + 0644（存入 external attributes 高 16 位）
function Set-ZipUnixMode($entry, [uint32]$mode) {
    try {
        $fi = $entry.GetType().GetField('_externalFileAttr',
            [System.Reflection.BindingFlags]'NonPublic,Instance')
        if ($fi) { $fi.SetValue($entry, $mode) }
    } catch {
        # 反射失败就交给安装器 chmod（customize.sh 里也会再设一次）
    }
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$fs = [System.IO.File]::Open($zip, [System.IO.FileMode]::Create)
$archive = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
$count = 0
try {
    Get-ChildItem $RepoRoot -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($RepoRoot.Length + 1)
        $top = ($rel -split '[\\/]')[0]
        if ($excludeTop -contains $top) { return }
        if ($_.Name -like '._*') { return }
        # 条目名强制正斜杠（ZipFile.CreateFromDirectory 在 .NET Framework 下会写反斜杠）
        $entry = $archive.CreateEntry($rel.Replace('\', '/'), [System.IO.Compression.CompressionLevel]::Optimal)
        if ($_.Extension -eq '.sh' -or $_.Name -eq 'update-binary' -or $_.Name -eq 'xindiaodud' -or $_.Name -eq 'fas-rs') {
            Set-ZipUnixMode $entry ([uint32]([uint32]33261 * [uint32]65536))   # 0755
        } else {
            Set-ZipUnixMode $entry ([uint32]([uint32]33188 * [uint32]65536))   # 0644
        }
        $es = $entry.Open()
        $bytes = [System.IO.File]::ReadAllBytes($_.FullName)
        $es.Write($bytes, 0, $bytes.Length)
        $es.Close()
        $count++
    }
} finally {
    $archive.Dispose()
    $fs.Close()
}

$size = [math]::Round((Get-Item $zip).Length / 1024)
Ok "打包完成: dist\$modId-$ver.zip（$count 个文件，$size KB）"
Info "刷入：KernelSU 管理器 -> 模块 -> 从本地安装zip；或 Magisk -> 模块 -> 安装"
