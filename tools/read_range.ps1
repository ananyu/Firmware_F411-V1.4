$root = Split-Path $PSScriptRoot -Parent
$gbk = [Text.Encoding]::GetEncoding(936)
$p = Join-Path $root 'FreeRTOS\tasks.c'
$lines = [IO.File]::ReadAllLines($p, $gbk)
[Console]::OutputEncoding = [Text.Encoding]::UTF8
for ($i = 2760; $i -le 2810; $i++) { Write-Host ($i.ToString() + '| ' + $lines[$i-1]) }
