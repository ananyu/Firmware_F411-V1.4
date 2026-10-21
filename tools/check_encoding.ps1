$root = Split-Path $PSScriptRoot -Parent
$files = @('COMMUNICATE/interface/atkp.h','COMMUNICATE/src/atkp.c','FLIGHT/src/stabilizer.c','USER/main.c','FLIGHT/src/blackbox.c','FLIGHT/interface/blackbox.h')
foreach ($f in $files) {
    $p = Join-Path $root $f
    $b = [IO.File]::ReadAllBytes($p)
    $q = 0; $hi = 0
    foreach ($x in $b) { if ($x -eq 0x3F) { $q++ }; if ($x -ge 0x80) { $hi++ } }
    Write-Host ("{0}  qmark={1}  highbytes={2}" -f $f, $q, $hi)
}
