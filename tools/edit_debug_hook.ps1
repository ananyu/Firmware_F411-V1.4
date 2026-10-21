$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$path = Join-Path $root "COMMON\src\freeRTOSdebug.c"

$bytes = [System.IO.File]::ReadAllBytes($path)
$enc = [System.Text.Encoding]::GetEncoding(28591)
$s = $enc.GetString($bytes)

# 1) Compile the hook for both method 1 and method 2.
$old1 = "#if (configCHECK_FOR_STACK_OVERFLOW == 1)"
$new1 = "#if (configCHECK_FOR_STACK_OVERFLOW > 0)"
if ($s.Contains($old1)) { $s = $s.Replace($old1, $new1); Write-Host "OK: guard" }
elseif ($s.Contains($new1)) { Write-Host "SKIP: guard" }
else { throw "pattern1" }

# 2) Align signature with the tasks.c extern (TaskHandle_t, char*).
$old2 = "void vApplicationStackOverflowHook(xTaskHandle *pxTask, signed portCHAR *pcTaskName)"
$new2 = "void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)"
if ($s.Contains($old2)) { $s = $s.Replace($old2, $new2); Write-Host "OK: signature" }
elseif ($s.Contains($new2)) { Write-Host "SKIP: signature" }
else { throw "pattern2" }

# 3) Report which task overflowed.
$old3 = 'printf("\nStack overflow!\n");'
$new3 = 'printf("\nStack overflow in task: %s\n", pcTaskName);'
if ($s.Contains($old3)) { $s = $s.Replace($old3, $new3); Write-Host "OK: printf" }
elseif ($s.Contains($new3)) { Write-Host "SKIP: printf" }
else { throw "pattern3" }

$outBytes = $enc.GetBytes($s)
[System.IO.File]::WriteAllBytes($path, $outBytes)
Write-Host "WROTE ($($outBytes.Length) bytes)"
