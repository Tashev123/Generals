# Determinism check: plays a game that records itself, then plays the recording back and reports whether every
# frame came out the same (the replay compares its checksum with the recorded one after every logic frame).
#
#   .\replay_test.ps1 -Map "Twilight Flame" -Bots 7 -Seed 3 -Speed 8 -Delay 120
#   .\replay_test.ps1 -Name t2 -ReplayExtra "-GenNoSimThread"       # play it back another way
#   .\replay_test.ps1 -Name t2 -SkipRecord -ReplayExtra "-GenReplayViewer=2"
#
# On a mismatch the replay writes Saved\Logs\StateDump_replay_f<N>.txt; run the recording side again with
# -RecordExtra "-GenStateDump=<N> -GenDumpTag=orig" and compare the two dumps.
param(
    [string]$Name = "t",
    [string]$Map = "Tournament Desert",
    [int]$Bots = 1,
    [int]$Seed = 7,
    [double]$Speed = 8,
    [int]$Delay = 45,
    [string]$Difficulty = "Hard",
    [string]$Faction = "",
    [string]$TestPowers = "",
    [string]$RecordExtra = "",
    [string]$ReplayExtra = "",
    [double]$ReplaySpeed = 0,
    [int]$ReplayDelay = 0,
    [switch]$SkipRecord,
    [switch]$Fog,
    [int]$ResX = 1280,
    [int]$ResY = 720
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot
$replays = Join-Path $root "Saved\Replays"
New-Item -ItemType Directory -Force $replays | Out-Null
$rec = (Join-Path $replays "$Name.genrep").Replace("\", "/")
$log = Join-Path $root "Saved\Logs\GenTest.log"
$pattern = "Replay|MISMATCH|State dump|Assertion failed|Fatal error|Match:"

if (-not $SkipRecord) {
    $common = @{ Map = $Map; Seed = $Seed; Speed = $Speed; Delay = $Delay; Difficulty = $Difficulty; Shot = "Saved\Screenshots\$Name-record.png"; ResX = $ResX; ResY = $ResY }
    if ($Faction) { $common.Faction = $Faction }
    if ($TestPowers) { $common.TestPowers = $TestPowers }
    if ($Fog) { $common.Fog = $true }
    $common.Extra = "-GenBots=$Bots -GenTestUnits -GenRecord=`"$rec`" $RecordExtra"
    "== recording $Name"
    & (Join-Path $PSScriptRoot "gen_test.ps1") @common | Select-String -Pattern "^exit:" | ForEach-Object { $_.Line }
    Select-String -Path $log -Pattern $pattern | Select-Object -Last 4 | ForEach-Object { $_.Line.Substring([Math]::Min(30, $_.Line.Length)) }
}

"== playing back $Name"
$rs = if ($ReplaySpeed) { $ReplaySpeed } else { $Speed }
$rd = if ($ReplayDelay) { $ReplayDelay } else { $Delay + 15 }
& (Join-Path $PSScriptRoot "gen_test.ps1") -Shot "Saved\Screenshots\$Name-replay.png" -Speed $rs -Delay $rd -ResX $ResX -ResY $ResY -Extra "-GenReplay=`"$rec`" -GenDumpTag=replay $ReplayExtra" |
    Select-String -Pattern "^exit:" | ForEach-Object { $_.Line }
Select-String -Path $log -Pattern $pattern | Select-Object -Last 6 | ForEach-Object { $_.Line.Substring([Math]::Min(30, $_.Line.Length)) }
