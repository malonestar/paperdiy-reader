# Windows helper (ESP-IDF 5.5.x installed with the Espressif installer).
#   .\tools\build.ps1                 build firmware (+ fetch components, fonts on first run)
#   .\tools\build.ps1 flash COM6      flash firmware + fonts
#   .\tools\build.ps1 release         build and package dist\
param([string]$Action = "build", [string]$Port = "")
$ErrorActionPreference = "Stop"
$root   = Split-Path -Parent $PSScriptRoot
$envbat = if ($env:IDF_ENV_BAT) { $env:IDF_ENV_BAT } else { "$env:USERPROFILE\esp\idfenv.bat" }
$py     = if ($env:IDF_PYTHON) { $env:IDF_PYTHON } else { "$env:USERPROFILE\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe" }
function Idf([string]$a) { & cmd /c "call `"$envbat`" >nul && cd /d `"$root`" && idf.py $a"; if ($LASTEXITCODE) { exit $LASTEXITCODE } }

if (-not (Test-Path "$root\components\M5GFX")) { & $py "$root\fetch_repos.py" }
if (-not (Test-Path "$root\fonts\fonts.bin")) {
    & $py -m pip install --quiet freetype-py fonttools
    & $py "$root\tools\make_fonts.py"
}
switch ($Action) {
    "build"   { Idf "build" }
    "flash"   {
        Idf "build"
        if (-not $Port) { $Port = [System.IO.Ports.SerialPort]::GetPortNames() | Select-Object -Last 1 }
        Idf "-p $Port flash"
        & $py -m esptool --chip esp32s3 -p $Port -b 921600 write-flash 0x410000 "$root\fonts\fonts.bin"
    }
    "release" { Idf "build"; & cmd /c "call `"$envbat`" >nul && python `"$root\tools\make_release.py`"" }
    default   { Write-Error "unknown action $Action" }
}
