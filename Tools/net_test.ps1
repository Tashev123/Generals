# Network game test on this computer: starts a server program and N games that join it, lets the first one
# (the host) set the lobby up and start the match, plays for a while and collects what happened.
#
#   .\net_test.ps1 -Players 2 -AI "Hard" -Map "Tournament Desert" -Delay 120
#   .\net_test.ps1 -Players 2 -Teams "1,1,2" -AI "Hard" -Powers "60,build,AmericaBarracks,1170,560;;"
#
# -Powers: each game's -GenTestPowers, separated by ";;" (the first game's first). -Kill "2@60": the second game
# is killed 60 seconds after it started and started again with -GenRejoin 10 seconds later (a crash and a
# rejoin). Every game records the match (Saved\Replays\<Name>-p<N>.genrep) with its checksums, and the
# script compares those of all games frame by frame at the end - besides the server, which compares what the
# games report while they play.
param(
    [string]$Name = "net",
    [int]$Players = 2,
    [string]$Map = "Tournament Desert",
    [string]$AI = "",
    [string]$Teams = "",
    [string]$Sides = "",
    [string]$Powers = "",
    [int]$Delay = 120,
    [int]$Seed = 5,
    [int]$Port = 8188,
    [string]$Kill = "",
    [string]$Extra = "",
    [int]$ResX = 1280,
    [int]$ResY = 720,
    [switch]$Window,
    [switch]$NoStart
)

$ErrorActionPreference = "Stop"
$ue = "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
$root = Split-Path $PSScriptRoot
$proj = Join-Path $root "Generals1.uproject"
$logs = Join-Path $root "Saved\Logs"
$replays = Join-Path $root "Saved\Replays"
New-Item -ItemType Directory -Force $replays | Out-Null

$server = Start-Process -FilePath $ue -PassThru -ArgumentList @("`"$proj`"", "/Game/Generals/Maps/Skirmish", "-game", "-GenServer", "-GenPort=$Port",
    "-GenSeed=$Seed", "-GenNoUPnP", "-nullrhi", "-nosound", "-unattended", "-nosplash", "-log=GenServer.log")
"server: pid $($server.Id), port $Port"

$powerList = if ($Powers) { $Powers -split ";;" } else { @() }
$teamList = if ($Teams) { $Teams -split "," } else { @() }
$sideList = if ($Sides) { $Sides -split "," } else { @() }

function Start-Game([int]$i, [bool]$rejoin) {
    $rec = (Join-Path $replays "$Name-p$i.genrep").Replace("\", "/")
    $crc = (Join-Path $replays "$Name-p$i.crc.txt").Replace("\", "/")
    $a = @("`"$proj`"", "/Game/Generals/Maps/Skirmish", "-game", "-windowed", "-ResX=$ResX", "-ResY=$ResY", "-unattended", "-nosplash",
        "-log=GenClient$i.log", "-DeterministicAudio", "-GenNoMusic", "-ini:Engine:[Audio]:UnfocusedVolumeMultiplier=1.0",
        "-GenName=Player$i", "-GenRecord=`"$rec`"", "-GenChecksumLog=`"$crc`"",
        "-GenShot=`"$((Join-Path $root "Saved\Screenshots\$Name-p$i.png").Replace("\", "/"))`"", "-GenShotDelay=$Delay")
    if (-not $Window) { $a += "-RenderOffscreen" }
    if ($rejoin) { $a += "-GenRejoin" } else { $a += "-GenJoin=127.0.0.1:$Port" }
    if ($i -eq 1 -and -not $NoStart) {
        $a += "-GenNetStart=$Players"
        $a += "-GenMap=`"$Map`""
        if ($AI) { $a += "-GenNetAI=$AI" }
    }
    if ($teamList.Count -ge $i -and $teamList[$i - 1]) { $a += "-GenNetTeam=$($teamList[$i - 1])" }
    if ($sideList.Count -ge $i -and $sideList[$i - 1]) { $a += "-GenNetSide=$($sideList[$i - 1])" }
    if ($powerList.Count -ge $i -and $powerList[$i - 1]) { $a += "-GenTestPowers=`"$($powerList[$i - 1])`"" }
    if ($Extra) { $a += $Extra }
    return Start-Process -FilePath $ue -PassThru -ArgumentList $a
}

Start-Sleep -Seconds 3
$games = @{}
for ($i = 1; $i -le $Players; $i++) {
    $games[$i] = Start-Game $i $false
    "game $i`: pid $($games[$i].Id)"
    Start-Sleep -Seconds 5   # (the first to join is the host, who starts the match: game 1)
}

if ($Kill) {
    # (Counted from the moment the server's clock starts - the match itself - not from the games' start.)
    $who, $when = $Kill -split "@"
    $serverLog = Join-Path $logs "GenServer.log"
    $waited = 0
    while ($waited -lt 300 -and -not (Select-String -Path $serverLog -Pattern "the clock runs" -Quiet -ErrorAction SilentlyContinue)) {
        Start-Sleep -Seconds 2
        $waited += 2
    }
    Start-Sleep -Seconds ([int]$when)
    "killing game $who (pid $($games[[int]$who].Id))"
    Stop-Process -Id $games[[int]$who].Id -Force
    Start-Sleep -Seconds 10
    Copy-Item (Join-Path $logs "GenClient$who.log") (Join-Path $logs "GenClient$who-killed.log") -Force
    $games[[int]$who] = Start-Game ([int]$who) $true
    "game $who started again to rejoin: pid $($games[[int]$who].Id)"
}

foreach ($g in $games.Values) { if (-not $g.WaitForExit(1800000)) { $g.Kill() } }
"games done: " + (($games.Keys | Sort-Object | ForEach-Object { "$_ exit $($games[$_].ExitCode)" }) -join ", ")
Start-Sleep -Seconds 2
if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }

"== server"
Select-String -Path (Join-Path $logs "GenServer.log") -Pattern "Server:" | ForEach-Object { $_.Line.Substring([Math]::Min(30, $_.Line.Length)) } | Select-Object -Last 25
for ($i = 1; $i -le $Players; $i++) {
    "== game $i"
    Select-String -Path (Join-Path $logs "GenClient$i.log") -Pattern "Net:|MISMATCH|Session f|Catching up|Test net|Replay:|Assertion failed|Fatal error" |
        ForEach-Object { $_.Line.Substring([Math]::Min(30, $_.Line.Length)) } | Select-Object -Last 14
}

# Every game's checksums, frame by frame: they must be the same wherever two games both worked a frame out.
"== checksums"
$sums = @{}
for ($i = 1; $i -le $Players; $i++) {
    $f = Join-Path $replays "$Name-p$i.crc.txt"
    if (Test-Path $f) { $sums[$i] = Get-Content $f } else { "game $i wrote no checksums" }
}
$keys = @($sums.Keys | Sort-Object)
for ($k = 1; $k -lt $keys.Count; $k++) {
    $a = $sums[$keys[0]]; $b = $sums[$keys[$k]]
    $n = [Math]::Min($a.Count, $b.Count)
    $first = -1
    for ($j = 0; $j -lt $n; $j++) { if ($a[$j] -ne $b[$j]) { $first = $j; break } }
    if ($first -ge 0) { "game $($keys[0]) and game $($keys[$k]) differ from line $($first + 1): $($a[$first]) / $($b[$first])" }
    else { "game $($keys[0]) and game $($keys[$k]): the same for all $n frames both have" }
}
