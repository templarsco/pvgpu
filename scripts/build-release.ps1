# PVGPU Release Build Script
# This script builds all components and creates a release package

param(
    [Parameter()]
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    
    [Parameter()]
    [string]$OutputDir = ".\release",
    
    [Parameter()]
    [switch]$SkipBackend,
    
    [Parameter()]
    [switch]$SkipDrivers,
    
    [Parameter()]
    [switch]$Package
)

$ErrorActionPreference = "Stop"

# Script location
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RootDir = Split-Path -Parent $ScriptDir

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "PVGPU Release Build" -ForegroundColor Cyan
Write-Host "Configuration: $Configuration" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Create output directory
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir | Out-Null
}

$OutputDir = Resolve-Path $OutputDir

# ============================================================================
# Build Backend (Rust)
# ============================================================================
if (-not $SkipBackend) {
    Write-Host "[1/4] Building Backend (Rust)..." -ForegroundColor Yellow
    
    Push-Location "$RootDir\backend"
    try {
        if ($Configuration -eq "Release") {
            cargo build --release
            if ($LASTEXITCODE -ne 0) { throw "Backend build failed" }
            $backendExe = "target\release\pvgpu-backend.exe"
        } else {
            cargo build
            if ($LASTEXITCODE -ne 0) { throw "Backend build failed" }
            $backendExe = "target\debug\pvgpu-backend.exe"
        }
        
        if (Test-Path $backendExe) {
            Copy-Item $backendExe "$OutputDir\pvgpu-backend.exe"
            Write-Host "  Backend built: pvgpu-backend.exe" -ForegroundColor Green
        } else {
            Write-Host "  WARNING: Backend executable not found" -ForegroundColor Yellow
        }
    }
    finally {
        Pop-Location
    }
} else {
    Write-Host "[1/4] Skipping Backend build" -ForegroundColor Gray
}

# ============================================================================
# Build Drivers (Windows - requires WDK)
# ============================================================================
if (-not $SkipDrivers) {
    Write-Host "[2/4] Building Drivers (WDK)..." -ForegroundColor Yellow
    
    # Check for MSBuild
    $msbuild = $null
    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    
    if (Test-Path $vsWhere) {
        $vsPath = & $vsWhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
        if ($vsPath) {
            $msbuild = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe"
        }
    }
    
    if (-not $msbuild -or -not (Test-Path $msbuild)) {
        Write-Host "  WARNING: MSBuild not found. Skipping driver build." -ForegroundColor Yellow
        Write-Host "  Install Visual Studio 2022 with WDK to build drivers." -ForegroundColor Yellow
    } else {
        Push-Location "$RootDir\driver"
        try {
            & $msbuild pvgpu.sln /p:Configuration=$Configuration /p:Platform=x64 /t:Rebuild
            if ($LASTEXITCODE -ne 0) { 
                Write-Host "  WARNING: Driver build failed" -ForegroundColor Yellow
            } else {
                # Create driver output directory
                $driverOut = "$OutputDir\driver"
                if (-not (Test-Path $driverOut)) {
                    New-Item -ItemType Directory -Path $driverOut | Out-Null
                }
                
                # Copy driver files
                $driverBin = "bin\$Configuration\x64"
                if (Test-Path "$driverBin\pvgpu.sys") {
                    Copy-Item "$driverBin\pvgpu.sys" "$driverOut\"
                }
                if (Test-Path "$driverBin\pvgpu_umd.dll") {
                    Copy-Item "$driverBin\pvgpu_umd.dll" "$driverOut\"
                }
                Copy-Item "kmd\pvgpu.inf" "$driverOut\"
                
                Write-Host "  Drivers built: pvgpu.sys, pvgpu_umd.dll" -ForegroundColor Green
            }
        }
        finally {
            Pop-Location
        }
    }
} else {
    Write-Host "[2/4] Skipping Driver build" -ForegroundColor Gray
}

# ============================================================================
# Copy Protocol Header
# ============================================================================
Write-Host "[3/4] Copying protocol files..." -ForegroundColor Yellow

$protoOut = "$OutputDir\protocol"
if (-not (Test-Path $protoOut)) {
    New-Item -ItemType Directory -Path $protoOut | Out-Null
}

Copy-Item "$RootDir\protocol\pvgpu_protocol.h" "$protoOut\"
Write-Host "  Protocol header copied" -ForegroundColor Green

# ============================================================================
# Copy Documentation
# ============================================================================
Write-Host "[4/4] Copying documentation..." -ForegroundColor Yellow

Copy-Item "$RootDir\README.md" "$OutputDir\"
Copy-Item "$RootDir\LICENSE-MIT" "$OutputDir\" -ErrorAction SilentlyContinue
Copy-Item "$RootDir\LICENSE-APACHE" "$OutputDir\" -ErrorAction SilentlyContinue

# Copy docs folder
$docsOut = "$OutputDir\docs"
if (-not (Test-Path $docsOut)) {
    New-Item -ItemType Directory -Path $docsOut | Out-Null
}
if (Test-Path "$RootDir\docs") {
    Copy-Item "$RootDir\docs\*" "$docsOut\" -Recurse -ErrorAction SilentlyContinue
}
Copy-Item "$RootDir\driver\README.md" "$docsOut\DRIVER-INSTALL.md" -ErrorAction SilentlyContinue
Copy-Item "$RootDir\backend\README.md" "$docsOut\BACKEND-CONFIG.md" -ErrorAction SilentlyContinue

Write-Host "  Documentation copied" -ForegroundColor Green

# ============================================================================
# Create Installation Script
# ============================================================================
Write-Host "Creating installation script..." -ForegroundColor Yellow

$installScript = @'
@echo off
setlocal

echo ========================================
echo PVGPU Driver Installation
echo ========================================
echo.

:: Check for admin rights
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: Please run as Administrator
    echo Right-click this script and select "Run as administrator"
    pause
    exit /b 1
)

:: Check for test signing
bcdedit | findstr /i "testsigning.*Yes" >nul 2>&1
if %errorlevel% neq 0 (
    echo WARNING: Test signing may not be enabled.
    echo.
    echo To enable test signing, run:
    echo   bcdedit /set testsigning on
    echo   bcdedit /set nointegritychecks on
    echo Then reboot.
    echo.
    choice /M "Continue anyway"
    if errorlevel 2 exit /b 1
)

:: Install driver
echo Installing PVGPU driver...
cd /d "%~dp0driver"

pnputil /add-driver pvgpu.inf /install
if %errorlevel% neq 0 (
    echo.
    echo WARNING: Driver installation returned an error.
    echo This may be normal if the device is not present.
    echo The driver has been added to the driver store.
)

:: Copy UMD DLLs
echo.
echo Copying user-mode driver DLLs...
copy /Y pvgpu_umd.dll "%SystemRoot%\System32\" >nul 2>&1
if exist pvgpu_umd32.dll (
    copy /Y pvgpu_umd32.dll "%SystemRoot%\SysWOW64\" >nul 2>&1
)

echo.
echo ========================================
echo Installation complete!
echo ========================================
echo.
echo Next steps:
echo 1. Start QEMU with -device pvgpu
echo 2. The driver will auto-install when device is detected
echo 3. Check Device Manager for "PVGPU Paravirtualized GPU Adapter"
echo.
pause
'@

Set-Content -Path "$OutputDir\install-driver.bat" -Value $installScript
Write-Host "  Installation script created" -ForegroundColor Green

# ============================================================================
# Create Package (ZIP)
# ============================================================================
if ($Package) {
    Write-Host ""
    Write-Host "Creating release package..." -ForegroundColor Yellow
    
    $version = "1.0.0"  # TODO: Read from version file
    $packageName = "pvgpu-$version-win-x64.zip"
    
    Compress-Archive -Path "$OutputDir\*" -DestinationPath ".\$packageName" -Force
    Write-Host "  Package created: $packageName" -ForegroundColor Green
}

# ============================================================================
# Summary
# ============================================================================
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Build Complete!" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Output directory: $OutputDir"
Write-Host ""
Write-Host "Contents:"
Get-ChildItem $OutputDir -Recurse | ForEach-Object {
    $indent = "  " * ($_.FullName.Replace($OutputDir, "").Split("\").Count - 1)
    Write-Host "$indent$($_.Name)"
}
