# MeshCoreSniffer GUI - build (MSYS2 MinGW-w64)
#   run from the project root:  .\build_gui.ps1
$ErrorActionPreference = "Stop"
$mingw = "C:\msys64\mingw64"
$env:PATH = "$mingw\bin;$env:PATH"

cmake -S . -B build_gui -G Ninja -DCMAKE_BUILD_TYPE=Release -DMESHCORE_BUILD_GUI=ON
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
cmake --build build_gui
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$exe = Join-Path $PSScriptRoot "MeshCoreSniffer-gui.exe"
Copy-Item "build_gui\MeshCoreSniffer-gui.exe" $exe -Force

# Runtime DLLs for double-clicking / running outside the MSYS2 shell.
$dlls = @(
    "glfw3.dll",
    "libsqlite3-0.dll",
    "librtlsdr.dll",
    "libusb-1.0.dll"
)
foreach ($d in $dlls) {
    $src = Join-Path $mingw "bin\$d"
    if (Test-Path -LiteralPath $src) {
        Copy-Item $src (Join-Path $PSScriptRoot $d) -Force
    }
}

Write-Host "built MeshCoreSniffer-gui.exe"
