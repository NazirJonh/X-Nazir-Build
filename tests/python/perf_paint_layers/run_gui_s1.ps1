# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Runs gui_s1.py in a GUI Blender window (one window opens on screen), then parses the stdout log
# between PERF_MARK start / end and prints the FAST_PATH_OK / FAST_PATH_NOT_REACHED verdict.
#
# Example:
#   .\run_gui_s1.ps1 -Mode S1 -Rows 6 -Ticks 50

param(
    [string]$Exe = 'D:\Blender_VERSE\build_windows_Full_x64_vc17_Release\bin\Release\X-NazirSculpt.exe',
    [string]$Python = 'C:\Users\User\AppData\Local\Programs\Python\Python314\python.exe',
    [int]$Rows = 6,
    [int]$Ticks = 50,
    [ValidateSet('S1', 'S8', 'S8F', 'S17', 'S8C', 'S12', 'S13', 'S14', 'S15', 'S16')][string]$Mode = 'S1',
    [int]$Stops = 2,
    [string]$Out = '',
    [string]$Edit = '',
    [int]$TimeoutSec = 300,
    [string]$GpuBackend = '',
    [switch]$FactoryStartup,
    [switch]$NoShots,
    [switch]$Shots,
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$script = Join-Path $here 'gui_s1.py'
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
if (-not $Out) {
    $Out = Join-Path $here ("gui_s1_{0}_{1}.json" -f $Mode, $stamp)
}
$log = Join-Path (Split-Path -Parent $Out) ("gui_s1_{0}_{1}.log" -f $Mode, $stamp)
$errLog = "$log.err"

# Equivalent of `X-NazirSculpt.exe ... > log 2>&1`, with a wall-clock guard in case Blender hangs
# before the in-script 120 s watchdog starts.
$blenderArgs = @()
if ($FactoryStartup) { $blenderArgs += '--factory-startup' }
if ($GpuBackend) { $blenderArgs += @('--gpu-backend', $GpuBackend) }
$blenderArgs += @('--debug-gpu', '--python', $script, '--',
    '--rows', "$Rows", '--ticks', "$Ticks", '--mode', $Mode, '--stops', "$Stops", '--out', $Out)
if ($Edit) { $blenderArgs += @('--edit', $Edit) }
if ($Verify) { $blenderArgs += '--verify' }
if ($NoShots) { $blenderArgs += '--no-shots' }
if ($Shots) { $blenderArgs += '--shots' }
$proc = Start-Process -FilePath $Exe -ArgumentList $blenderArgs -PassThru -NoNewWindow `
    -RedirectStandardOutput $log -RedirectStandardError $errLog
if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
    Write-Host "Blender did not exit in $TimeoutSec s, killing it."
    $proc.Kill()
    $timedOut = $true
} else {
    $timedOut = $false
}
Write-Host "Blender exit code: $($proc.ExitCode) timed_out=$timedOut"

if (Test-Path $errLog) {
    Add-Content -Path $log -Value "`n--- stderr ---"
    Get-Content -Path $errLog | Add-Content -Path $log
}

# Parse with plain Python in isolated mode (no bpy, no script-dir imports).
$env:PYTHONIOENCODING = 'utf-8'
& $Python -I $script --parse --log $log --out $Out --ticks $Ticks --mode $Mode
$parseCode = $LASTEXITCODE
Write-Host "log: $log"
Write-Host "json: $Out"
exit $parseCode
