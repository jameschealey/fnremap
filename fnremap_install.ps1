#Requires -RunAsAdministrator
<#
  FnRemap installer - signs INF catalog and installs WinUSB driver

  Run in elevated PowerShell:
    Set-ExecutionPolicy Bypass -Scope Process -Force
    & ".\fnremap_install.ps1"
#>
$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$inf        = Join-Path $projectDir 'fnremap.inf'

# Auto-detect WDK version
$wdkBinRoot = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
if (-not (Test-Path $wdkBinRoot)) {
    throw "Windows Driver Kit (WDK) not found at $wdkBinRoot"
}
$wdkVersion = Get-ChildItem $wdkBinRoot -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1 -ExpandProperty Name
if (-not $wdkVersion) {
    throw "No WDK version found under $wdkBinRoot"
}
Write-Host "Using WDK version: $wdkVersion"

$wdkBin64   = "$wdkBinRoot\$wdkVersion\x64"
$wdkBin86   = "$wdkBinRoot\$wdkVersion\x86"
$signtool   = if (Test-Path "$wdkBin64\signtool.exe") { "$wdkBin64\signtool.exe" } else { "$wdkBin86\signtool.exe" }
$inf2cat    = if (Test-Path "$wdkBin64\inf2cat.exe")  { "$wdkBin64\inf2cat.exe" }  else { "$wdkBin86\Inf2Cat.exe" }

if (-not (Test-Path $inf)) { throw "fnremap.inf not found at $inf" }

Write-Host "`n=== FnRemap WinUSB Installer ===" -ForegroundColor Cyan

# --- 1. Self-signed certificate ---
Write-Host "`n=== Step 1: Certificate ===" -ForegroundColor Cyan
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object Subject -eq 'CN=FnCtrl Driver' |
        Select-Object -First 1

if (-not $cert) {
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject 'CN=FnCtrl Driver' `
        -CertStoreLocation Cert:\CurrentUser\My `
        -NotAfter (Get-Date).AddYears(10)
    Write-Host "Created certificate: $($cert.Thumbprint)"
} else {
    Write-Host "Using existing certificate: $($cert.Thumbprint)"
}

$cerFile = Join-Path $projectDir 'fnremap.cer'
Export-Certificate -Cert $cert -FilePath $cerFile -Type CERT | Out-Null

# --- 2. Generate and sign catalog ---
Write-Host "`n=== Step 2: Catalog ===" -ForegroundColor Cyan
$tmpCat = Join-Path $env:TEMP 'fnremap_cat'
if (Test-Path $tmpCat) { Remove-Item $tmpCat -Recurse -Force }
New-Item -ItemType Directory $tmpCat -Force | Out-Null
Copy-Item $inf $tmpCat

& $inf2cat /driver:$tmpCat /os:10_x64 /uselocaltime
if ($LASTEXITCODE -ne 0) {
    Write-Host "WARNING: inf2cat failed" -ForegroundColor Yellow
}

$catTmp = Join-Path $tmpCat 'fnremap.cat'
$cat    = Join-Path $projectDir 'fnremap.cat'
if (Test-Path $catTmp) {
    Copy-Item $catTmp $cat -Force
    & $signtool sign /sha1 $cert.Thumbprint /fd SHA256 $cat
    if ($LASTEXITCODE -ne 0) { Write-Host "WARNING: catalog signing failed" -ForegroundColor Yellow }
} else {
    Write-Host "WARNING: no catalog generated" -ForegroundColor Yellow
}
Remove-Item $tmpCat -Recurse -Force

# --- 3. Install cert into machine trust stores ---
Write-Host "`n=== Step 3: Trust certificate ===" -ForegroundColor Cyan
Import-Certificate -FilePath $cerFile -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Write-Host "Installed to Trusted Root CA"
Import-Certificate -FilePath $cerFile -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null
Write-Host "Installed to Trusted Publisher"

# --- 4. Install driver ---
Write-Host "`n=== Step 4: Install driver ===" -ForegroundColor Cyan
pnputil /add-driver $inf /install
if ($LASTEXITCODE -ne 0) {
    Write-Host "pnputil returned $LASTEXITCODE - check output above" -ForegroundColor Yellow
}

# --- 5. Stop existing service/processes ---
Write-Host "`n=== Step 5: Stop existing ===" -ForegroundColor Cyan
$svcName    = 'FnRemap'
$installDir = Join-Path $env:ProgramFiles 'FnRemap'
$exeSrc     = Join-Path $projectDir 'fnremap.exe'

Unregister-ScheduledTask -TaskName $svcName -Confirm:$false -ErrorAction SilentlyContinue 2>$null
Stop-Service -Name $svcName -Force -ErrorAction SilentlyContinue 2>$null
Start-Sleep -Seconds 1
Get-Process -Name fnremap -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
sc.exe delete $svcName 2>$null | Out-Null
Write-Host "Stopped"

# --- 6. Install fnremap.exe ---
Write-Host "`n=== Step 6: Install fnremap.exe ===" -ForegroundColor Cyan

if (-not (Test-Path $exeSrc)) {
    throw "fnremap.exe not found at $exeSrc - compile it first"
}

if (-not (Test-Path $installDir)) {
    New-Item -ItemType Directory $installDir -Force | Out-Null
}
Copy-Item $exeSrc $installDir -Force
Write-Host "Installed to $installDir\fnremap.exe"

# --- 7. Windows service ---
Write-Host "`n=== Step 7: Windows service ===" -ForegroundColor Cyan
$exePath = Join-Path $installDir 'fnremap.exe'

$binPath = "`"$exePath`" --service"
New-Service -Name $svcName `
    -BinaryPathName $binPath `
    -DisplayName 'FnRemap - Keyboard Remapper' `
    -Description 'Apple Magic Keyboard Fn-to-Ctrl remapper' `
    -StartupType Automatic | Out-Null

sc.exe failure $svcName reset= 86400 actions= restart/5000/restart/10000/restart/30000 | Out-Null

Start-Service -Name $svcName
Write-Host "Service '$svcName' created and started"

Write-Host "`n=== Done ===" -ForegroundColor Green
Write-Host "FnRemap service is running and will auto-start at boot."
Write-Host "Unplug and replug the keyboard if it's not working yet."
Write-Host ""
Write-Host "To uninstall: run fnremap_uninstall.ps1"
