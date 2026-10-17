<#
.SYNOPSIS
  Build and run the panel link's PC simulator: a virtual master or a virtual keypad.

.DESCRIPTION
  link_sim (host/link_sim, docs/features/11 LINK-013) runs one side of the panel link on this PC
  over UDP, with a console:

    master  the real alarm engine with a small house and the master's side of the link: open
            pairing, allow a keypad, list and remove keypads, open zones, arm and disarm; the
            house's devices for the keypads' Smart Home (devices, device, house) and a keypad's
            Screen and sound settings (prefs N, prefs N set KEY VALUE), as Home Assistant changes
            them.
    keypad  the keypad's side of the link and the master's status as the keypad rebuilds it:
            pair, arm, disarm, chime, panic; the master's devices as it mirrors them (devices)
            and acts on them (act REF ACTION [VALUE] [CODE]); its own settings (prefs).

  The two pair with each other, and with panels on the LAN: they use the OS's random numbers.
  There is no mDNS on the PC: give the keypad --master <ip>[:port], or it asks by broadcast to
  255.255.255.255:6056. The master listens on 0.0.0.0:6056 (Windows may ask to let it through
  its firewall); --bind 127.0.0.1 keeps both on this PC. Type help at the console.

  The records (the master's keypads, the keypad's master, with their pairing keys) are kept in
  build/link_sim/master.plink and build/link_sim/keypad.plink unless --state names another file.
  Delete a file to start that side afresh.

  -BuildOnly builds and stops. -Smoke runs link_smoke instead: the test CI runs, a master and a
  keypad in one process over 127.0.0.1 on a virtual clock - pairing, commands, Smart Home and
  settings, supervision, a master restart, removal (add -v after it for both logs).

.EXAMPLE
  pwsh tools/link_sim.ps1 master
  pwsh tools/link_sim.ps1 keypad --master 127.0.0.1
  pwsh tools/link_sim.ps1 master --bind 127.0.0.1 --name "Harbor House"
  pwsh tools/link_sim.ps1 keypad --master 192.0.2.10 --state build/link_sim/hall.plink
  pwsh tools/link_sim.ps1 -Smoke
  pwsh tools/link_sim.ps1 -Smoke -v
#>
param(
    [switch]$BuildOnly,
    [switch]$Smoke
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build/link_sim'
. (Join-Path $PSScriptRoot 'host_env.ps1')

cmake -S (Join-Path $root 'host/link_sim') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($BuildOnly) { exit 0 }

$ext = $(if ($env:OS -eq 'Windows_NT') { '.exe' } else { '' })
if ($Smoke) {
    & (Join-Path $build "link_smoke$ext") @args
    exit $LASTEXITCODE
}

# Each side keeps its record in the build folder unless --state says otherwise.
$rest = @($args)
$role = $(if ($rest.Count -gt 0) { $rest[0] } else { '' })
if (($role -eq 'master' -or $role -eq 'keypad') -and -not ($rest -contains '--state')) {
    $rest += @('--state', (Join-Path $build "$role.plink"))
}
& (Join-Path $build "link_sim$ext") @rest
exit $LASTEXITCODE
