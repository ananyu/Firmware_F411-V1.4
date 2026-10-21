$root = Split-Path $PSScriptRoot -Parent
$name = [char]0x5F00 + [char]0x53D1 + [char]0x603B + [char]0x7ED3 + '_' + [char]0x9762 + [char]0x8BD5 + [char]0x5B66 + [char]0x4E60 + [char]0x6587 + [char]0x6863 + '.md'
$p = Join-Path $root $name
$gbk = [Text.Encoding]::GetEncoding(936)
$lines = [IO.File]::ReadAllLines($p, $gbk)
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$start = [int]$args[0]; $end = [int]$args[1]
for ($i = $start; $i -le $end -and $i -le $lines.Count; $i++) {
    Write-Host ($i.ToString() + '| ' + $lines[$i-1])
}
