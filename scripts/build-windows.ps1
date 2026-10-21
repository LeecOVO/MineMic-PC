param(
    [Parameter(Mandatory = $true)][string]$QtPrefix,
    [switch]$SingleFile,
    [string]$BuildDirectory = "$PSScriptRoot/../build/windows",
    [string]$OutputDirectory = "$PSScriptRoot/../dist"
)
$ErrorActionPreference = "Stop"
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "Use an x64 VS 2022 developer environment."
}
$project = (Resolve-Path "$PSScriptRoot/..").Path
$qt = (Resolve-Path $QtPrefix).Path
$env:PATH = "$qt/bin;$env:PATH"
$static = if ($SingleFile) { "ON" } else { "OFF" }
& cmake -S $project -B $BuildDirectory -G Ninja "-DCMAKE_PREFIX_PATH=$qt" -DCMAKE_BUILD_TYPE=Release "-DMINEMIC_SINGLE_FILE=$static" -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw "Configure failed" }
& cmake --build $BuildDirectory --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
& ctest --test-dir $BuildDirectory --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Tests failed" }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$artifactDirectory = Join-Path $OutputDirectory $(if ($SingleFile) { "single-file" } else { "portable" })
if (Test-Path $artifactDirectory) { throw "Output already exists: $artifactDirectory. Choose a fresh OutputDirectory." }
New-Item -ItemType Directory $artifactDirectory | Out-Null
Copy-Item "$BuildDirectory/MineMic.exe" $artifactDirectory
if ($SingleFile) {
    $dependencies = & dumpbin /DEPENDENTS "$artifactDirectory/MineMic.exe"
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect executable imports" }
    $dependencies | Set-Content (Join-Path $OutputDirectory "MineMic-imports.txt")
    if ($dependencies -match '(?i)(Qt\d.*\.dll|vcruntime\d.*\.dll|msvcp\d.*\.dll|libgcc.*\.dll|libstdc\+\+.*\.dll)') {
        throw "Executable still imports a Qt/compiler runtime DLL; single-file verification failed."
    }
    Write-Host "Single EXE: $artifactDirectory/MineMic.exe"
} else {
    & "$qt/bin/windeployqt.exe" --release --no-translations --no-opengl-sw --no-compiler-runtime "$artifactDirectory/MineMic.exe"
    if ($LASTEXITCODE -ne 0) { throw "Qt deployment failed" }
    Write-Host "Development portable directory: $artifactDirectory (requires the official MSVC Redistributable)"
}
