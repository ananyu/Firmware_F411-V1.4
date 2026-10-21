$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$path = Join-Path $root "CONFIG\interface\FreeRTOSConfig.h"

# Read raw bytes, decode as Latin-1 (byte-preserving, 0..255 -> char) so GBK
# comment bytes are never reinterpreted or lost.
$bytes = [System.IO.File]::ReadAllBytes($path)
$enc = [System.Text.Encoding]::GetEncoding(28591)
$s = $enc.GetString($bytes)

# 1) Enable run-time stats: the value digit right after the macro name.
$old1 = "#define configGENERATE_RUN_TIME_STATS	        0"
$new1 = "#define configGENERATE_RUN_TIME_STATS	        1"
if ($s.Contains($old1)) {
    $s = $s.Replace($old1, $new1)
    Write-Host "OK: configGENERATE_RUN_TIME_STATS 0->1"
} elseif ($s.Contains($new1)) {
    Write-Host "SKIP: already 1"
} else {
    throw "pattern1 not found"
}

# 2) Enable stack overflow method 2.
$old2 = "#define configCHECK_FOR_STACK_OVERFLOW			0"
$new2 = "#define configCHECK_FOR_STACK_OVERFLOW			2"
if ($s.Contains($old2)) {
    $s = $s.Replace($old2, $new2)
    Write-Host "OK: configCHECK_FOR_STACK_OVERFLOW 0->2"
} elseif ($s.Contains($new2)) {
    Write-Host "SKIP: already 2"
} else {
    throw "pattern2 not found"
}

# 3) Insert the DWT-cycle counter macros after the stats-formatting define line.
$anchor = "#define configUSE_STATS_FORMATTING_FUNCTIONS	1"
$macros = @"

/* ---- run-time stats clock source: DWT cycle counter (CPU clock, 96MHz).
 * Enabled once from main before the scheduler starts (fsStatsInit).
 * configCPU_CLOCK_HZ must equal the core clock for % math to be correct. ---- */
#define configRUN_TIME_COUNTER_TYPE              unsigned long
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()  fsStatsInit()
#define portGET_RUN_TIME_COUNTER_VALUE()          fsStatsCycles()
"@
if ($s.Contains("fsStatsInit")) {
    Write-Host "SKIP: macros already present"
} else {
    $idx = $s.IndexOf($anchor)
    if ($idx -lt 0) { throw "anchor not found" }
    $insertAt = $idx + $anchor.Length
    $s = $s.Substring(0, $insertAt) + $macros + $s.Substring($insertAt)
    Write-Host "OK: inserted DWT macros"
}

# Write back as raw bytes (Latin-1 round-trip preserves every byte).
$outBytes = $enc.GetBytes($s)
[System.IO.File]::WriteAllBytes($path, $outBytes)
Write-Host "WROTE $path ($($outBytes.Length) bytes)"
