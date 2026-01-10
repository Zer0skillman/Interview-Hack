# Build Script for AI Backend
Write-Host "Checking for Python..."
python --version
if ($?) {
    Write-Host "Python found."
} else {
    Write-Error "Python not found! Please install Python 3.10+ and add to PATH."
    exit 1
}

# Create Virtual Environment (Optional but recommended)
if (-not (Test-Path "venv")) {
    Write-Host "Creating venv..."
    python -m venv venv
}

# Activate venv
Write-Host "Activating venv..."
.\venv\Scripts\Activate.ps1

# Install requirements
Write-Host "Installing dependencies..."
pip install -r requirements.txt

# Run PyInstaller
Write-Host "Building Executable... (This will take longer due to full package collection)"
pyinstaller --noconsole --onefile --name ai_backend --clean `
    --collect-all langchain `
    --collect-all langchain_community `
    --collect-all langchain_core `
    --collect-all langchain_google_genai `
    --collect-all langchain_openai `
    main.py

# Move artifact
if (Test-Path "dist/ai_backend.exe") {
    Write-Host "Build Success! Moving to deployment folder..."
    if (-not (Test-Path "../app")) { mkdir "../app" }
    Move-Item -Force "dist/ai_backend.exe" "../app/ai_backend.exe"
    Write-Host "Done. Backend is at ../app/ai_backend.exe"
} else {
    Write-Error "Build Failed. No executable found in dist/."
}
