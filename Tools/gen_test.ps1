# Builds the editor target (optional) and runs the game offscreen with test switches,
# saving a screenshot. Requires the Unreal Editor to be closed when -Build is used.
#
#   .\gen_test.ps1 -Build -Shot out.png -Map "Tournament Desert" -Cam "220,2080,0,300" -Objects "AmericaTankCrusader"
param(
    [switch]$Build,
    [string]$Shot = "",
    [string]$Map = "Tournament Desert",
    [string]$Cam = "",
    [string]$Objects = "",
    [string]$Model = "",
    [string]$Anim = "",
    [int]$Delay = 12,
    [int]$ResX = 2880,
    [int]$ResY = 1620,
    [string]$TestMove = "",
    [string]$ObjectsAt = "",
    [string]$Enemies = "",
    [string]$EnemyAt = "",
    [switch]$TestAttack,
    [string]$TestFX = "",
    [string]$TestBuild = "",
    [string]$TestQueue = "",
    [string]$TestSelect = "",
    [int]$TestGather = 0,
    [string]$TestJets = "",
    [string]$TestPowers = "",
    [double]$Speed = 0,
    [string]$Enemy = "",
    [string]$Difficulty = "",
    [switch]$NoAI,
    [switch]$Trace,
    [int]$Seed = 0,
    [int]$PauseFrame = 0,
    [string]$Faction = "",
    [string]$Extra = ""
)

$ErrorActionPreference = "Stop"
$ue = "C:\Program Files\Epic Games\UE_5.8"
$proj = Join-Path $PSScriptRoot "..\Generals1.uproject" | Resolve-Path
$log = Join-Path $PSScriptRoot "..\Saved\Logs\GenTest.log"

if ($Build) {
    $out = & "$ue\Engine\Build\BatchFiles\Build.bat" Generals1Editor Win64 Development "-Project=$proj" -WaitMutex 2>&1
    $errors = $out | Select-String -Pattern "error|Result:"
    $errors | ForEach-Object { $_.Line }
    if (-not ($out | Select-String -Pattern "Result: Succeeded")) { exit 1 }
}

$args = @("`"$proj`"", "/Game/Generals/Maps/Skirmish", "-game", "-windowed", "-ResX=$ResX", "-ResY=$ResY",
          "-RenderOffscreen", "-GenMap=`"$Map`"", "-log=GenTest.log", "-unattended", "-nosplash")
if ($Cam) { $args += "-GenCam=$Cam" }
if ($Objects) { $args += "-GenObject=$Objects" }
if ($Model) { $args += "-GenModel=$Model" }
if ($Anim) { $args += "-GenAnim=$Anim" }
if ($TestMove) { $args += "-GenTestMove=$TestMove" }
if ($ObjectsAt) { $args += "-GenObjectAt=$ObjectsAt" }
if ($Enemies) { $args += "-GenEnemyObject=$Enemies" }
if ($EnemyAt) { $args += "-GenEnemyAt=$EnemyAt" }
if ($TestAttack) { $args += "-GenTestAttack" }
if ($TestFX) { $args += "-GenTestFX=$TestFX" }
if ($TestBuild) { $args += "-GenTestBuild=$TestBuild" }
if ($TestQueue) { $args += "-GenTestQueue=$TestQueue" }
if ($TestSelect) { $args += "-GenTestSelect=$TestSelect" }
if ($TestGather) { $args += "-GenTestGather=$TestGather" }
if ($TestJets) { $args += "-GenTestJets=`"$TestJets`"" }
if ($TestPowers) { $args += "-GenTestPowers=`"$TestPowers`"" }
if ($Speed) { $args += "-GenSpeed=$Speed" }
if ($Enemy) { $args += "-GenEnemy=$Enemy" }
if ($Difficulty) { $args += "-GenDifficulty=$Difficulty" }
if ($NoAI) { $args += "-GenNoAI" }
if ($Trace) { $args += "-GenTrace" }
if ($Seed) { $args += "-GenSeed=$Seed" }
if ($PauseFrame) { $args += "-GenPauseFrame=$PauseFrame" }
if ($Faction) { $args += "-GenFaction=$Faction" }
if ($Shot) {
    # Relative paths are taken from the project folder; quoted, since that path contains a space.
    if (-not [IO.Path]::IsPathRooted($Shot)) { $Shot = Join-Path (Split-Path $proj) $Shot }
    $shotPath = [IO.Path]::GetFullPath($Shot).Replace("\", "/")
    $args += "-GenShot=`"$shotPath`""
    $args += "-GenShotDelay=$Delay"
}
if ($Extra) { $args += $Extra }

$p = Start-Process -FilePath "$ue\Engine\Binaries\Win64\UnrealEditor.exe" -ArgumentList $args -PassThru
if (-not $p.WaitForExit(900000)) { $p.Kill(); "timeout" }
"exit: $($p.ExitCode)"
Select-String -Path $log -Pattern "LogGenerals|Error: (?!.*(AgentSkill|PythonTestRunner|Traceback|File |\^|from |class |tests\.))" |
    Select-Object -Last 30 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(220, $_.Line.Length)) }

