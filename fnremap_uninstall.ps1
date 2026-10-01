#Requires -RunAsAdministrator
<#
  FnRemap uninstaller - removes WinUSB driver, restores original HID driver

  Run in elevated PowerShell:
    Set-ExecutionPolicy Bypass -Scope Process -Force
    & ".\fnremap_uninstall.ps1"
#>
$ErrorActionPreference = 'Stop'

Write-Host "`n=== FnRemap Uninstaller ===" -ForegroundColor Cyan

# --- Stop and remove service ---
Write-Host "`n=== Step 1: Remove service ===" -ForegroundColor Cyan
$svcName = 'FnRemap'

$svc = Get-Service -Name $svcName -ErrorAction SilentlyContinue
if ($svc) {
    Stop-Service -Name $svcName -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    sc.exe delete $svcName | Out-Null
    Write-Host "Service removed"
} else {
    Write-Host "Service not found (already removed)" -ForegroundColor Yellow
}

# Clean up old scheduled task if present
Unregister-ScheduledTask -TaskName $svcName -Confirm:$false -ErrorAction SilentlyContinue 2>$null

# Kill any running fnremap process
Get-Process -Name fnremap -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

# --- Remove install directory ---
Write-Host "`n=== Step 2: Remove program files ===" -ForegroundColor Cyan
$installDir = Join-Path $env:ProgramFiles 'FnRemap'
if (Test-Path $installDir) {
    Remove-Item $installDir -Recurse -Force
    Write-Host "Removed $installDir"
} else {
    Write-Host "Install directory not found (already removed)" -ForegroundColor Yellow
}

# --- Remove WinUSB driver ---
Write-Host "`n=== Step 3: Remove WinUSB driver ===" -ForegroundColor Cyan

# Find the installed driver
$drivers = pnputil /enum-drivers 2>&1 | Out-String
$lines = $drivers -split "`n"
$oemInf = $null

for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'fnremap\.inf') {
        # Look backwards for the Published Name
        for ($j = $i; $j -ge [Math]::Max(0, $i-5); $j--) {
            if ($lines[$j] -match 'Published Name\s*:\s*(oem\d+\.inf)') {
                $oemInf = $matches[1]
                break
            }
        }
        break
    }
}

if ($oemInf) {
    Write-Host "Found driver: $oemInf"
    pnputil /delete-driver $oemInf /uninstall /force
    Write-Host "Driver removed."
} else {
    Write-Host "FnRemap driver not found in driver store." -ForegroundColor Yellow
    Write-Host "It may already be uninstalled."
}

Write-Host "`n=== Done ===" -ForegroundColor Green
Write-Host "Unplug and replug the keyboard to restore normal operation."
Write-Host "If the keyboard doesn't work after replug, restart Windows."
