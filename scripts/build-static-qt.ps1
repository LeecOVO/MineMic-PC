param(
    [Parameter(Mandatory = $true)][string]$QtBaseSource,
    [string]$InstallDirectory = "$PSScriptRoot/../out/qt-static",
    [string]$BuildDirectory = "$PSScriptRoot/../out/qt-static-build",
    [ValidateSet("opensource", "commercial")][string]$QtLicense = "opensource"
)
$ErrorActionPreference = "Stop"
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "Run in an x64 Native Tools Command Prompt / Developer PowerShell for VS 2022."
}
$source = (Resolve-Path $QtBaseSource).Path
if (-not (Test-Path "$source/configure.bat")) { throw "QtBaseSource must point at the qtbase source root." }
New-Item -ItemType Directory -Force $BuildDirectory, $InstallDirectory | Out-Null
$build = (Resolve-Path $BuildDirectory).Path
$prefix = (Resolve-Path $InstallDirectory).Path
Push-Location $build
try {
    # Widgets uses QPainter. No QML, browser, multimedia codecs, or OpenGL runtime.
    & "$source/configure.bat" -prefix $prefix -static -static-runtime -release "-$QtLicense" -confirm-license -nomake examples -nomake tests -no-opengl -no-icu -no-openssl
    if ($LASTEXITCODE -ne 0) { throw "Qt configure failed" }
    & cmake --build . --parallel
    if ($LASTEXITCODE -ne 0) { throw "Qt build failed" }
    & cmake --install .
    if ($LASTEXITCODE -ne 0) { throw "Qt install failed" }
} finally { Pop-Location }
Write-Host "Static Qt installed at $prefix"
