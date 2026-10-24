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
    [string]$Menu = "",
    [double]$Speed = 0,
    [string]$Enemy = "",
    [string]$Difficulty = "",
    [switch]$NoAI,
    [switch]$Fog,
    [switch]$Window,
    [switch]$Music,
    [string]$Record = "",
    [switch]$AudioSelfTest,
    [string]$DumpWav = "",
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
          "-GenMap=`"$Map`"", "-log=GenTest.log", "-unattended", "-nosplash")
# -Window: in a real window on the desktop instead of offscreen (what needs the platform itself: the mouse cursors).
if (-not $Window) { $args += "-RenderOffscreen" }
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
# -Menu "sec,click,Id;...": the skirmish menu is shown and worked (ids: Source/Generals1/Game/GenShell.cpp)
if ($Menu) { $args += "-GenTestMenu=`"$Menu`"" }
if ($Speed) { $args += "-GenSpeed=$Speed" }
if ($Enemy) { $args += "-GenEnemy=$Enemy" }
if ($Difficulty) { $args += "-GenDifficulty=$Difficulty" }
if ($NoAI) { $args += "-GenNoAI" }
# Test runs see the whole map (most of them look at the enemy); -Fog plays with the fog of war, as the game does.
if (-not $Fog) { $args += "-GenNoShroud" }
# Sound: rendered by the engine's non-realtime audio device, so a test run is silent on the speakers but
# everything is mixed (and can be recorded with -Record file.wav). No music unless -Music.
$args += "-DeterministicAudio"
# (An unfocused Unreal window mutes its sound sources; nothing reaches the speakers here anyway.)
$args += "-ini:Engine:[Audio]:UnfocusedVolumeMultiplier=1.0"
if (-not $Music) { $args += "-GenNoMusic" }
if ($Record) {
    # The non-realtime device renders 1/30 s of sound per tick whatever the frame rate: a fixed 30 Hz step
    # makes game time agree with it (the run then goes as fast as the machine allows).
    $args += "-UseFixedTimeStep"
    $args += "-FPS=30"
    if (-not [IO.Path]::IsPathRooted($Record)) { $Record = Join-Path (Split-Path $proj) $Record }
    $args += "-GenRecordAudio=`"$([IO.Path]::GetFullPath($Record).Replace("\", "/"))`""
}
if ($AudioSelfTest) { $args += "-GenAudioSelfTest" }
if ($DumpWav) { $args += "-GenDumpWav=$DumpWav" }
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

