# Shared by gen_build.ps1 and gen_test.ps1: where the engine is, and one build or test run at a time on this PC
# (several checkouts - git worktrees - may be worked on at once; the machine has room for one compile).
#   . "$PSScriptRoot\gen_lock.ps1"; Enter-GenLock; ...; Exit-GenLock   (the lock also goes when the script ends)

function Get-GenEngine {
    $reg = Get-ItemProperty 'HKCU:\Software\Epic Games\Unreal Engine\Builds' -ErrorAction SilentlyContinue
    if ($reg -and $reg.'5.8' -and (Test-Path $reg.'5.8')) { return ($reg.'5.8' -replace '/', '\') }
    foreach ($p in 'C:\UE\UE_5.8', 'C:\Program Files\Epic Games\UE_5.8') { if (Test-Path $p) { return $p } }
    throw 'Unreal Engine 5.8 not found'
}

$script:GenMutex = $null
function Enter-GenLock {
    $script:GenMutex = New-Object System.Threading.Mutex($false, 'Global\GeneralsBuildAndTest')
    try {
        if (-not $script:GenMutex.WaitOne(0)) {
            Write-Host 'Another build or test run is going on: waiting for it...'
            [void]$script:GenMutex.WaitOne()
        }
    } catch [System.Threading.AbandonedMutexException] { } # (its holder ended without letting go: it is ours now)
}
function Exit-GenLock {
    if ($script:GenMutex) { try { $script:GenMutex.ReleaseMutex() } catch { }; $script:GenMutex.Dispose(); $script:GenMutex = $null }
}
