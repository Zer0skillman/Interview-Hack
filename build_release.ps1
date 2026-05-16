# Master Build Script for AI Overlay (single-process C++)

$ErrorActionPreference = "Stop"

Write-Host "==================================" -ForegroundColor Cyan
Write-Host "   BUILDING AI OVERLAY RELEASE" -ForegroundColor Cyan
Write-Host "==================================" -ForegroundColor Cyan

# 1. Build C++ Frontend
Write-Host "`n[1/2] Building C++ Frontend..." -ForegroundColor Yellow

# Ensure g++ is in PATH (common MSYS2 location)
$env:Path = "C:\msys64\mingw64\bin;" + $env:Path

$cppOutput = "overlay.exe"

# Compile the Win32 resource (icon + version info) into an object file
if (Test-Path "app.rc") {
    Write-Host "Compiling app.rc (icon + version info)..."
    & windres app.rc -O coff -o app.res.o
    if (-not (Test-Path "app.res.o")) {
        Write-Error "windres failed to produce app.res.o"
        exit 1
    }
}

$resObj = ""
if (Test-Path "app.res.o") { $resObj = "app.res.o" }
$gppCommand = "g++ main.cpp OverlayWindow.cpp Overlay_Rendering.cpp ConfigLoader.cpp LLMClient.cpp ConfigDialog.cpp AudioCapture.cpp Logger.cpp $resObj -o $cppOutput -mwindows -static -DUNICODE -D_UNICODE -lwinhttp -lcomctl32 -lgdiplus -lcrypt32 -lole32 -lshell32"

Write-Host "Running: $gppCommand"
Invoke-Expression $gppCommand

if (-not (Test-Path $cppOutput)) {
    Write-Error "C++ Compilation failed. $cppOutput not created."
    exit 1
}

# Also build the test runner (separate console exe). Failure here is a soft warning.
Write-Host "Building tests.exe..."
$testCmd = "g++ tests.cpp -o tests.exe -std=c++17 -DUNICODE -D_UNICODE -static"
Invoke-Expression $testCmd
if (Test-Path "tests.exe") {
    Write-Host "  ok - run .\tests.exe to execute"
} else {
    Write-Warning "  tests.exe did not build; continuing"
}

# 2. Package — write into project_app/ without nuking the directory, so a
# transient Defender lock on the previous overlay.exe doesn't kill the build.
Write-Host "`n[2/2] Packaging Release to 'project_app'..." -ForegroundColor Yellow
$distDir = "project_app"
if (-not (Test-Path $distDir)) {
    New-Item -ItemType Directory -Path $distDir | Out-Null
}

# Retry overwrite a few times — Defender briefly holds a handle on freshly-built exes.
$attempt = 0
$ok = $false
while ($attempt -lt 5 -and -not $ok) {
    try {
        Move-Item -Path $cppOutput -Destination "$distDir\overlay.exe" -Force
        $ok = $true
    } catch {
        $attempt++
        Start-Sleep -Milliseconds 600
    }
}
if (-not $ok) {
    Write-Error "Could not move $cppOutput into $distDir (file locked). Try closing any running overlay.exe."
    exit 1
}

if (Test-Path "models_list.txt") {
    Copy-Item "models_list.txt" -Destination "$distDir\" -Force
}
if (Test-Path "README.md") {
    Copy-Item "README.md" -Destination "$distDir\" -Force
}

Write-Host "`n==================================" -ForegroundColor Green
Write-Host "   BUILD SUCCESSFUL" -ForegroundColor Green
Write-Host "==================================" -ForegroundColor Green
Write-Host "Output Folder: $PWD\$distDir"
Write-Host "Run '$distDir\overlay.exe' to test."
