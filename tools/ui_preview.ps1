<#
.SYNOPSIS
  Build the PC preview of the touch UI and render every screen state to PNG.

.DESCRIPTION
  Builds host/ui_preview (real alarm engine + LVGL Pro UI + ui_app.c on a headless LVGL
  display) with the host toolchain and runs it. Screenshots land in build/ui_preview/shots.
  LVGL comes from firmware/crowpanel_p4/managed_components (run a firmware build once) or is
  cloned from GitHub.

  -Board picks the board profile (firmware/<board>/sdkconfig.defaults, docs/features/33); the
  scripted walk is written for the 7" panel's profile, the default.

  -ZWave adds docs/features/36's Z-Wave world and pages, in a build folder of its own: the walk
  takes the same shots first (the world still off), then the Z-Wave shots after them.

  -Locale en-XA or ar-XB walks the screens in a test language (docs/features/37 I18N-024): the
  catalogs from tools/i18n.py pseudo --build, texts found through them, the English walk's checks
  only reported, and every label whose text doesn't fit listed (I18N-022). Its own build folder.
  -Locale es-US walks them the same way in Spanish, and -Locale en-GB in British English, from
  the committed catalogs (drafts until they are reviewed). The English walk builds without draft languages, as a released panel does
  (I18N-005).

  -Live (with -Locale) starts in English and changes to the language while running, as the
  Panel Manager or Settings does (I18N-014), then walks; it fails unless every picture is the same
  as the walk that started in the language (run that one first, it is the reference).

  The walk also runs the voice (host/ui_preview/voice_walk.c): what the panel would say is
  rendered to build/ui_preview/voice/<n>-<kind>.wav, with a trace line per utterance.

  -VoicePack <file> uses another voice: an unsigned pack made with tools/voice/voice.py pack
  --unsigned (host/README.md "The voice"). The default is the tracked en-US voice, packed at build
  time.

.EXAMPLE
  pwsh tools/ui_preview.ps1
  pwsh tools/ui_preview.ps1 -Board crowpanel_p4
  pwsh tools/ui_preview.ps1 -ZWave
  pwsh tools/ui_preview.ps1 -Locale en-XA
  pwsh tools/ui_preview.ps1 -Locale es-US
  pwsh tools/ui_preview.ps1 -Locale en-XA -Live
#>
param(
    [string]$Out,
    [string]$Board = 'crowpanel_p4',
    [switch]$ZWave,
    [ValidateSet('', 'en-XA', 'ar-XB', 'es-US', 'en-GB')][string]$Locale = '',
    [switch]$Live,
    [string]$VoicePack = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $root "firmware/$Board/sdkconfig.defaults"))) {
    throw "No board profile firmware/$Board/sdkconfig.defaults"
}
$suffix = $(if ($Board -eq 'crowpanel_p4') { '' } else { "-$Board" }) + $(if ($ZWave) { '-zwave' } else { '' }) +
    $(if ($Locale) { "-$Locale" } else { '' }) + $(if ($Live) { '-live' } else { '' })
if ($Live -and -not $Locale) { throw '-Live needs -Locale' }
$build = Join-Path $root "build/ui_preview$suffix"
if (-not $Out) { $Out = Join-Path $build 'shots' }
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
cmake -S (Join-Path $root 'host/ui_preview') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc "-DALARM_BOARD=$Board" "-DALARM_SIM_ZWAVE=$zwaveOpt" "-DALARM_PREVIEW_LOCALE=$Locale" "-DALARM_PREVIEW_LIVE=$(if ($Live) { 'ON' } else { 'OFF' })" $pythonOpt
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# -VoicePack: another voice for the run (an unsigned pack; tools/voice/voice.py pack --unsigned).
# host_panel.c reads ALARM_VOICE_PACK; without it the tracked en-US voice packed at build time.
if ($VoicePack) {
    if (-not (Test-Path $VoicePack)) { throw "No voice pack at $VoicePack" }
    $env:ALARM_VOICE_PACK = (Resolve-Path $VoicePack).Path
}
if (Test-Path $Out) { Remove-Item (Join-Path $Out '*.png') -ErrorAction SilentlyContinue }
& (Join-Path $build 'ui_preview.exe') $Out
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($Live) {
    # I18N-014: after a change while running, every screen as if the panel had started in it.
    $reference = Join-Path $root ("build/ui_preview" + ($suffix -replace '-live$', '') + '/shots')
    if (-not (Test-Path $reference)) { throw "No reference walk: run tools/ui_preview.ps1 -Locale $Locale first" }
    $differ = @(Get-ChildItem $Out -Filter *.png | Where-Object {
        $ref = Join-Path $reference $_.Name
        -not (Test-Path $ref) -or (Get-FileHash $_.FullName).Hash -ne (Get-FileHash $ref).Hash
    } | ForEach-Object { $_.Name })
    if ($differ.Count -gt 0) {
        Write-Host "Not the same as a start in ${Locale}: $($differ -join ', ')"
        exit 1
    }
    Write-Host "Every picture is the same as a start in $Locale"
}
# The UI without its optional pages (docs/features/33 step 6).
& (Join-Path $build 'ui_smoke_minimal.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# A setting changed from elsewhere goes through the screen (docs/features/07 UI-046).
& (Join-Path $build 'prefs_smoke.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# A keypad's calls to its master never hold up the screen (docs/features/11 LINK-054).
& (Join-Path $build 'calls_smoke.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# The words not drawn in a picture: ui_text.c and Home's headline, detail and note (BL-066). They
# are checked in English, so a walk in another language skips them.
if (-not $Locale) {
    & (Join-Path $build 'ui_words_test.exe')
    exit $LASTEXITCODE
}
exit 0
