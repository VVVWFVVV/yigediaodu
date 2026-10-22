# 给指定 .ps1 文件补 UTF-8 BOM（PowerShell 5.1 无 BOM 时按 GBK 解码会乱码/解析失败）
param([string[]]$Paths)
foreach ($p in $Paths) {
    if (-not (Test-Path $p)) { Write-Host "missing: $p"; continue }
    $bytes = [IO.File]::ReadAllBytes($p)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        Write-Host "already has BOM: $p"
        continue
    }
    $text = [Text.UTF8Encoding]::new($false).GetString($bytes)
    [IO.File]::WriteAllText($p, $text, [Text.UTF8Encoding]::new($true))
    Write-Host "BOM added: $p"
}
