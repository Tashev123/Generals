# Smoke test of the packaged game (E:\Games\My Generals): a server and two games of the package itself play a
# short network match on this computer; the logs (in the package's Saved folder) and both games' checksums tell
# whether it works as the editor's build does.
param(
    [string]$Root = "E:\Games\My Generals\Windows",
    [int]$Delay = 90,
    [int]$Port = 8288
)
$exe = Join-Path $Root "Generals1.exe"
$logs = Join-Path $Root "Generals1\Saved\Logs"
$replays = Join-Path $Root "Generals1\Saved\Replays"
New-Item -ItemType Directory -Force $replays | Out-Null
$server = Start-Process -FilePath $exe -PassThru -ArgumentList @("-GenServer", "-GenPort=$Port", "-GenNoUPnP", "-GenSeed=9", "-nullrhi", "-nosound", "-unattended", "-log=GenServer.log")
Start-Sleep -Seconds 2
$games = @()
foreach ($i in 1, 2) {
    $a = @("-windowed", "-ResX=1280", "-ResY=720", "-RenderOffscreen", "-unattended", "-DeterministicAudio", "-GenNoMusic", "-log=GenClient$i.log",
        "-GenJoin=127.0.0.1:$Port", "-GenName=Pkg$i", "-GenChecksumLog=`"$((Join-Path $replays "pkg-p$i.crc.txt").Replace("\", "/"))`"",
        "-GenShot=`"$((Join-Path $Root "Generals1\Saved\pkg-p$i.png").Replace("\", "/"))`"", "-GenShotDelay=$Delay")
    if ($i -eq 1) { $a += @("-GenNetStart=2", "-GenMap=`"Tournament Desert`"") }
    $games += Start-Process -FilePath $exe -PassThru -ArgumentList $a
    Start-Sleep -Seconds 8   # (the first to join is the host, who starts the match: game 1)
}
foreach ($g in $games) { if (-not $g.WaitForExit(900000)) { $g.Kill() } }
# (The package's Generals1.exe starts the real program in Binaries\Win64 and waits for it: the server is
# stopped by its own command line, not by the launcher's process.)
Get-CimInstance Win32_Process -Filter "Name = 'Generals1.exe'" | Where-Object { $_.CommandLine -like "*-GenPort=$Port*" } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
"== server"
Select-String -Path (Join-Path $logs "GenServer.log") -Pattern "Server:|Build id|Data hash" | ForEach-Object { $_.Line.Substring([Math]::Min(30, $_.Line.Length)) }
foreach ($i in 1, 2) {
    "== game $i"
    Select-String -Path (Join-Path $logs "GenClient$i.log") -Pattern "Net:|Skirmish started|Zero Hour|Fatal|Error: (?!.*Python)" | Select-Object -First 12 | ForEach-Object { $_.Line.Substring([Math]::Min(30, $_.Line.Length)) }
}
$a = Get-Content (Join-Path $replays "pkg-p1.crc.txt") -ErrorAction SilentlyContinue
$b = Get-Content (Join-Path $replays "pkg-p2.crc.txt") -ErrorAction SilentlyContinue
if ($a -and $b) {
    $n = [Math]::Min($a.Count, $b.Count); $first = -1
    for ($j = 0; $j -lt $n; $j++) { if ($a[$j] -ne $b[$j]) { $first = $j; break } }
    if ($first -ge 0) { "checksums differ from line $($first + 1)" } else { "checksums: the same for all $n frames" }
} else { "no checksums written" }
