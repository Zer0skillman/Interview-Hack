# Master Build Script for AI Overlay

$ErrorActionPreference = "Stop"

Write-Host "==================================" -ForegroundColor Cyan
Write-Host "   BUILDING AI OVERLAY RELEASE" -ForegroundColor Cyan
Write-Host "==================================" -ForegroundColor Cyan

# 1. Build Python Backend
Write-Host "`n[1/3] Building Python Backend..." -ForegroundColor Yellow
if (Test-Path "python_backend") {
    Push-Location "python_backend"
    try {
        # Call the backend build script (which installs reqs and runs pyinstaller)
        # We assume it creates ../app/ai_backend.exe as currently programmed
        & .\build.ps1
    } catch {
        Write-Error "Failed to build Python Backend. Check errors above."
        Pop-Location
        exit 1
    }
    Pop-Location
} else {
    Write-Error "Directory 'python_backend' not found!"
    exit 1
}

# 2. Build C++ Frontend
Write-Host "`n[2/3] Building C++ Frontend..." -ForegroundColor Yellow

# Ensure g++ is in PATH (common MSYS2 location)
$env:Path = "C:\msys64\mingw64\bin;" + $env:Path

$cppOutput = "overlay.exe"
$gppCommand = "g++ main.cpp OverlayWindow.cpp ConfigLoader.cpp LLMClient.cpp ConfigDialog.cpp -o $cppOutput -mwindows -static -DUNICODE -D_UNICODE -lwinhttp -lcomctl32"

Write-Host "Running: $gppCommand"
Invoke-Expression $gppCommand

if (-not (Test-Path $cppOutput)) {
    Write-Error "C++ Compilation failed. $cppOutput not created."
    exit 1
}

# 3. Create Distribution Folder 'project_app'
Write-Host "`n[3/3] Packaging Relase to 'project_app'..." -ForegroundColor Yellow
$distDir = "project_app"

# Clean old dist
if (Test-Path $distDir) {
    Remove-Item $distDir -Recurse -Force
}
New-Item -ItemType Directory -Path $distDir | Out-Null

# Move/Copy Files
# A. Overlay Executable
Move-Item -Path $cppOutput -Destination "$distDir\"

# B. AI Backend Executable (Created by python_backend/build.ps1 in 'app' folder)
if (Test-Path "app/ai_backend.exe") {
    Move-Item -Path "app/ai_backend.exe" -Destination "$distDir\"
    Remove-Item "app" -Recurse -Force # Cleanup temporary build folder
} elseif (Test-Path "python_backend/dist/ai_backend.exe") {
    # Fallback if build.ps1 didn't move it
    Copy-Item "python_backend/dist/ai_backend.exe" -Destination "$distDir\"
} else {
    Write-Warning "Could not find ai_backend.exe! The app needs this to function."
}

# C. Supporting Files
if (Test-Path "models_list.txt") {
    Copy-Item "models_list.txt" -Destination "$distDir\"
}
if (Test-Path "README.md") {
    Copy-Item "README.md" -Destination "$distDir\"
}

# Cleanup
# Remove config from dist if it accidentally got copied? No, we only copied specific files.
# We explicitly desire a fresh install (no llm_config.txt)

Write-Host "`n==================================" -ForegroundColor Green
Write-Host "   BUILD SUCCESSFUL" -ForegroundColor Green
Write-Host "==================================" -ForegroundColor Green
Write-Host "Output Folder: $PWD\$distDir"
Write-Host "Run '$distDir\overlay.exe' to package or test."
