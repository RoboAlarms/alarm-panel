<#
.SYNOPSIS
  Run idf.py for a firmware project with ESP-IDF v6.1 activated.

.DESCRIPTION
  Finds the ESP-IDF v6.1 installation made by EIM (ESP-IDF Installation Manager) through
  C:\Espressif\tools\eim_idf.json (or $env:IDF_EIM_JSON), activates it for this process and
  runs idf.py in the chosen firmware project.

  Images are signed (docs/features/13). The signing key is secrets/ota_signing_key.pem (never
  committed); it is created on the first build when missing. Keep a copy somewhere safe: panels
  only accept updates signed with the key of the firmware they run, so a lost key means
  reflashing every panel over USB.

.EXAMPLE
  pwsh tools/idf.ps1 build                              # crowpanel_p4, P4 silicon v1.x (default)
  pwsh tools/idf.ps1 -ChipRev v3 build                  # P4 silicon v3.x build
  pwsh tools/idf.ps1 -p COM4 flash monitor              # flash + serial monitor
  pwsh tools/idf.ps1 -Version 0.6.1-test build          # test image (build_v1_0.6.1-test)
  pwsh tools/idf.ps1 -Project e32r40t build             # another board (build, build_dev, ...)
  pwsh tools/idf.ps1 -Project crowpanel_10 build     # the 10.1in, built on crowpanel_p4
  pwsh tools/idf.ps1 -Project radios/h2_rcp build       # the ESP32-H2's own firmware

  Build folders: crowpanel_p4 and the boards built on it (ALARM_BOARD_BASE crowpanel_p4 in their
  CMakeLists.txt) build one image per P4 silicon family (build_v1, build_v3 and
  sdkconfig.v1/.v3); every other board has one (build, sdkconfig). A version, profile or dark
  build adds its suffix to either: build_v1_dev, build_dev, build_0.6.1-test. A board built on
  another project reads each sdkconfig.defaults fragment from it first (cmake/alarm_firmware.cmake).
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Project = 'crowpanel_p4',
    [Alias('p')][string]$Port, # serial port for flash / monitor (-p COM4)
    [ValidateSet('v1', 'v3')][string]$ChipRev = 'v1',
    [Alias('Profile')][ValidateSet('legacy', 'dev', 'release')][string]$BuildProfile = 'legacy',
    [switch]$Dark,
    [ValidatePattern('^\d+\.\d+\.\d+([-+][0-9A-Za-z.+-]+)?$')][string]$Version,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$IdfArgs
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$projDir = Join-Path $root "firmware/$Project"
if (-not (Test-Path $projDir)) { throw "No firmware project at $projDir" }

$eimJson = if ($env:IDF_EIM_JSON) { $env:IDF_EIM_JSON } else { 'C:\Espressif\tools\eim_idf.json' }
if (-not (Test-Path $eimJson)) { throw "ESP-IDF not found ($eimJson). Install ESP-IDF v6.1 with EIM first." }
$eim = Get-Content $eimJson -Raw | ConvertFrom-Json
$idf = $eim.idfInstalled | Where-Object { $_.name -eq 'v6.1' } | Select-Object -First 1
if (-not $idf) { throw "ESP-IDF v6.1 is not in $eimJson (found: $($eim.idfInstalled.name -join ', '))" }
. $idf.activationScript *> $null

# Windows MAX_PATH: esp_hosted ships very deep example paths that overflow 260 characters
# under the default cache in %LOCALAPPDATA%. A short cache path avoids that without needing
# the system-wide LongPathsEnabled setting.
if (-not $env:IDF_COMPONENT_CACHE_PATH) { $env:IDF_COMPONENT_CACHE_PATH = 'C:\Espressif\cm' }

# The OTA signing key (sdkconfig.defaults: CONFIG_SECURE_BOOT_SIGNING_KEY).
$keyFile = Join-Path $root 'secrets/ota_signing_key.pem'
if (-not (Test-Path $keyFile)) {
    New-Item -ItemType Directory -Force (Split-Path $keyFile) | Out-Null
    Write-Host "Creating the firmware signing key $keyFile (keep a backup of it)"
    espsecure generate-signing-key --version 2 --scheme rsa3072 $keyFile
    if ($LASTEXITCODE -ne 0) { throw 'Creating the signing key failed' }
}

# A board built on another firmware project (set(ALARM_BOARD_BASE <board>) in its CMakeLists.txt)
# takes its fragments from there too, and its silicon families.
$base = $null
$baseDir = $null
$baseMatch = Select-String -Path (Join-Path $projDir 'CMakeLists.txt') `
    -Pattern '^\s*set\(ALARM_BOARD_BASE\s+([a-z0-9_]+)\s*\)' | Select-Object -First 1
if ($baseMatch) {
    $base = $baseMatch.Matches[0].Groups[1].Value
    $baseDir = Join-Path $root "firmware/$base"
    if (-not (Test-Path $baseDir)) { throw "firmware/$Project is built on firmware/$base, which is missing" }
}

# Each P4 silicon family needs its own image (ESP-IDF 6.x can't build one for both); the other
# boards have one.
$defaults = @('sdkconfig.defaults')
if ($Project -eq 'crowpanel_p4' -or $base -eq 'crowpanel_p4') {
    if ($ChipRev -eq 'v1') { $defaults += 'sdkconfig.defaults.p4rev1' }
    $buildDir = "build_$ChipRev"
    $sdkconfig = "sdkconfig.$ChipRev"
} else {
    if ($PSBoundParameters.ContainsKey('ChipRev')) { throw "-ChipRev is for crowpanel_p4 and the boards built on it only" }
    $buildDir = 'build'
    $sdkconfig = 'sdkconfig'
}

# A version other than the VERSION file (test images) builds in its own folder.
$env:ALARM_FW_VERSION = ''
if ($Version) {
    $env:ALARM_FW_VERSION = $Version
    $buildDir += "_$Version"
}

if ($BuildProfile -ne 'legacy') {
    $defaults += "sdkconfig.defaults.$BuildProfile"
    $buildDir += "_$BuildProfile"
    $sdkconfig += ".$BuildProfile"
}
if ($Dark) {
    # A dark acceptance image keeps the signed profile identity for remote-only panels.
    # The packaging checker still refuses to ship a forced-dark release artifact.
    $defaults += 'sdkconfig.defaults.dark'
    $buildDir += '_dark'
    $sdkconfig += '.dark'
}
$env:ALARM_BUILD_PROFILE = $BuildProfile
foreach ($d in $defaults) {
    $here = Test-Path (Join-Path $projDir $d)
    $inBase = $baseDir -and (Test-Path (Join-Path $baseDir $d))
    if (-not ($here -or $inBase)) { throw "firmware/$Project has no $d" }
}

if ($Port) { $IdfArgs = @('-p', $Port) + $IdfArgs }

Push-Location $projDir
try {
    idf.py -B $buildDir -D SDKCONFIG=$sdkconfig -D "SDKCONFIG_DEFAULTS=$($defaults -join ';')" @IdfArgs
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
