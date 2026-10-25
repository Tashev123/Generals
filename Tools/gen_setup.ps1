# Runs Tools\ue_setup.py in the editor's Python (materials, the lighting collection, the Skirmish level).
# The Unreal Editor must be closed.
#
#   .\gen_setup.ps1                 everything (minutes: all materials recompile)
#   .\gen_setup.ps1 -Only water     just the water materials (also: roads, shroud, w3d, decal, sky, terrain)
param([string]$Only = "")

$ErrorActionPreference = "Stop"
. "$PSScriptRoot\gen_lock.ps1"
$ue = Get-GenEngine
$proj = Join-Path $PSScriptRoot "..\Generals1.uproject" | Resolve-Path
$script = (Join-Path $PSScriptRoot "ue_setup.py" | Resolve-Path).Path.Replace("\", "/")   # (Unreal eats "\u")

$env:GEN_SETUP_ONLY = $Only
try {
    $out = & "$ue\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$proj" -run=pythonscript "-script=$script" -unattended -nosplash 2>&1
}
finally {
    $env:GEN_SETUP_ONLY = ""
}
# What matters: Python errors, materials that failed, and the commandlet's own verdict.
$out | Select-String -Pattern "Traceback|LogPython: Error|Failed to compile|Generals setup complete|Success - |Failure - " |
    Where-Object { $_ -notmatch "AgentSkill|PythonTestRunner|editor_toolset|ToolsetRegistry|init_unreal" } |
    ForEach-Object { $_.Line.Substring(0, [Math]::Min(240, $_.Line.Length)) }
