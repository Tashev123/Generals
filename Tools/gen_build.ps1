# Builds the editor target of this checkout (the main one or a worktree), waiting its turn if another build or
# test run is on (gen_lock.ps1). Prints the errors and warnings of the project's own files and the result.
# The editor must be closed for this checkout (Live Coding blocks outside builds).
#   powershell -File Tools\gen_build.ps1
$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\gen_lock.ps1"
$ue = Get-GenEngine
$proj = (Resolve-Path (Join-Path $PSScriptRoot '..\Generals1.uproject')).Path
$src = (Resolve-Path (Join-Path $PSScriptRoot '..\Source')).Path
Enter-GenLock
try {
    # -DisablePlugin=Fab: the launcher updated the engine's Fab plugin on 2026-10-28 with a module (FabToolset) that
    # the installed engine's precompiled build rules don't have, and every build stopped at "RulesError". The
    # project doesn't use Fab; the editor still loads its precompiled binaries.
    $out = & cmd /c "`"$ue\Engine\Build\BatchFiles\Build.bat`" Generals1Editor Win64 Development `"-Project=$proj`" -WaitMutex -DisablePlugin=Fab 2>&1"
    $code = $LASTEXITCODE
} finally { Exit-GenLock }
# (compiler and linker errors from anywhere, warnings only from this project's files)
$out | Where-Object { ($_ -match ': (fatal )?error ') -or ($_ -match ': warning ' -and $_.StartsWith($src, 'OrdinalIgnoreCase')) } |
    Select-Object -First 60 | ForEach-Object { $_ }
$result = $out | Select-String -Pattern 'Result: \w+' | Select-Object -Last 1
if ($result) { $result.Line.Trim() } else { $out | Select-Object -Last 15 }
exit $code
