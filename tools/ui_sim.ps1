<#
.SYNOPSIS
  Build and run the interactive panel simulator.

.DESCRIPTION
  Opens a window with the panel's 1024x600 screen on the left and buttons on the right that
  put the panel into the states worth looking at: arming, a door opening, mains power lost,
  the router going away. Click the screen as you would touch it.

  It runs the real alarm engine, the real event log and the same UI code the panel builds, so
  a change can be tried here instead of flashing a panel that is protecting a house.

  It also plays the panel's sounder out of the PC's speakers - the same patterns the board
  renders, at half its level so a full-scale alarm is bearable through headphones. "Mute sound"
  stops the speakers while the panel goes on sounding underneath, and the status line under the
  screen names the pattern the engine is asking for. It speaks too, as the panel does: the
  announcements (Settings > Screen and sound), a zone's spoken name after its chime, Preview.

  Windows only: it uses LVGL's own Win32 backend, so there is nothing to install. Elsewhere,
  use the scripted preview (tools/ui_preview.ps1) for now.

  "Save screenshot" writes the panel's screen to build/ui_sim/shots.

  -Board picks the board profile (firmware/<board>/sdkconfig.defaults, docs/features/33): that
  board's layout, screen size, pages and LVGL memory. The default is the 7" panel.

  -ZWave adds docs/features/36 Z6's Z-Wave world and its button group (a secure S2 door sensor,
  an insecure motion sensor, against the real library and a fake ZAC93 on the simulator's own
  clock). Off by default: without it the simulator is exactly as before.

  -Locale en-XA or ar-XB shows the screen in a test language (docs/features/37 I18N-024): the
  catalogs from tools/i18n.py pseudo --build, which the script runs first. Its own build folder.
  -Locale es-US starts in Spanish, -Locale en-GB in British English (the committed catalogs,
  drafts until they are reviewed). Like a
  development build, the simulator offers the draft languages in Settings > Language and region.

  -VoicePack <file> uses another voice: an unsigned pack made with tools/voice/voice.py pack
  --unsigned (host/README.md "The voice"). The default is the tracked en-US voice, packed at build
  time.

.EXAMPLE
  pwsh tools/ui_sim.ps1
  pwsh tools/ui_sim.ps1 -Board crowpanel_p4
  pwsh tools/ui_sim.ps1 -ZWave
  pwsh tools/ui_sim.ps1 -Locale en-XA
  pwsh tools/ui_sim.ps1 -Locale es-US
#>
param(
    [string]$Shots,
    [switch]$BuildOnly,
    [string]$Board = 'crowpanel_p4',
    [switch]$ZWave,
    [ValidateSet('', 'en-XA', 'ar-XB', 'es-US', 'en-GB')][string]$Locale = '',
    [string]$VoicePack = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $root "firmware/$Board/sdkconfig.defaults"))) {
    throw "No board profile firmware/$Board/sdkconfig.defaults"
}
# The 7" panel keeps the folder it always had; another board, or -ZWave, gets its own so the
# two configurations' object files never mix.
$suffix = $(if ($Board -eq 'crowpanel_p4') { '' } else { "-$Board" }) + $(if ($ZWave) { '-zwave' } else { '' }) +
    $(if ($Locale) { "-$Locale" } else { '' })
$build = Join-Path $root "build/ui_sim$suffix"
if (-not $Shots) { $Shots = Join-Path $build 'shots' }
. (Join-Path $PSScriptRoot 'host_env.ps1')

$zwaveOpt = if ($ZWave) { 'ON' } else { 'OFF' }
# A real Python 3 (the Store's python.exe is a stub): the test languages' catalogs, and the
# voice pack the build makes from the tracked takes (host/ui_preview/voice_pack.cmake).
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python -or $python -like '*WindowsApps*') {
    $python = 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe'
}
$pythonOpt = @(if (Test-Path $python) { "-DPython3_EXECUTABLE=$python" }) # none: an empty list, no argument
if ($Locale -match '-X[A-Z]$') {
    # The test languages' catalogs, from the current template (build/i18n/pseudo); a real
    # language (es-US) is in the committed catalogs.
    & $python (Join-Path $root 'tools/i18n.py') pseudo --build
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
cmake -S (Join-Path $root 'host/ui_sim') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc "-DALARM_BOARD=$Board" "-DALARM_SIM_ZWAVE=$zwaveOpt" "-DALARM_PREVIEW_LOCALE=$Locale" $pythonOpt
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($BuildOnly) { exit 0 }

# -VoicePack: another voice for the run (an unsigned pack; tools/voice/voice.py pack --unsigned).
# host_panel.c reads ALARM_VOICE_PACK; without it the tracked en-US voice packed at build time.
if ($VoicePack) {
    if (-not (Test-Path $VoicePack)) { throw "No voice pack at $VoicePack" }
    $env:ALARM_VOICE_PACK = (Resolve-Path $VoicePack).Path
}
& (Join-Path $build 'ui_sim.exe') $Shots
exit $LASTEXITCODE
