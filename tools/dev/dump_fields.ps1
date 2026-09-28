# 只读静态工具：从 eurotrucks2.exe 的 .rdata 字段描述表中导出「名称 → 偏移」。
# 该脚本只读取磁盘上的可执行文件，不接触游戏进程。
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string[]]$Ranges,
    [string]$Filter = ''
)

$bytes = [System.IO.File]::ReadAllBytes($Exe)
$imageBase = [uint64]0x140000000
$rdataRaw = [uint64]0x1DBB800
$rdataEnd = [uint64]0x26D9E00

function Get-StringFromVa([uint64]$va) {
    if ($va -lt $imageBase) { return $null }
    $rva = $va - $imageBase
    $file = $rdataRaw + ($rva - [uint64]0x1DBD000)
    if ($file -lt $rdataRaw -or $file -ge $rdataEnd) { return $null }
    $sb = [System.Text.StringBuilder]::new()
    for ($j = 0; $j -lt 72; $j++) {
        $index = [int64]$file + $j
        if ($index -ge $bytes.Length) { break }
        $c = $bytes[$index]
        if ($c -eq 0) { break }
        if ($c -lt 32 -or $c -gt 126) { return $null }
        [void]$sb.Append([char]$c)
    }
    if ($sb.Length -lt 2) { return $null }
    return $sb.ToString()
}

foreach ($range in $Ranges) {
    $parts = $range.Split('-')
    $start = [Convert]::ToInt64(($parts[0] -replace '^0[xX]', ''), 16)
    $end = [Convert]::ToInt64(($parts[1] -replace '^0[xX]', ''), 16)
    Write-Output ("=== 0x{0:X} .. 0x{1:X} ===" -f $start, $end)
    for ($p = $start; $p -le $end; $p += 8) {
        $q0 = [System.BitConverter]::ToUInt64($bytes, $p)
        if ($q0 -lt $imageBase) { continue }
        $name = Get-StringFromVa $q0
        if (-not $name) { continue }
        if ($Filter -and $name -notmatch $Filter) { continue }
        $q1 = [System.BitConverter]::ToUInt64($bytes, $p + 8)
        $q2 = [System.BitConverter]::ToUInt64($bytes, $p + 16)
        $q3 = [System.BitConverter]::ToUInt64($bytes, $p + 24)
        $q4 = [System.BitConverter]::ToUInt64($bytes, $p + 32)
        $typeName = Get-StringFromVa $q1
        $typeText = ''
        if ($typeName) { $typeText = " <$typeName>" }
        Write-Output ("  0x{0:X8}  name={1,-24} +08={2:X16}{3} +10={4:X16} +18={5:X16} +20={6:X16}" -f $p, $name, $q1, $typeText, $q2, $q3, $q4)
    }
}
