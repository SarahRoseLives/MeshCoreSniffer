# MeshCoreSniffer - build (MSYS2 MinGW-w64)
#   run from the project root:  .\build.ps1
$ErrorActionPreference = "Stop"
$mingw = "C:\msys64\mingw64"
$env:PATH = "$mingw\bin;$env:PATH"
$gxx = "$mingw\bin\g++.exe"

& $gxx -std=c++17 -O2 -static -I src -I src/ed25519 `
    src/main.cpp src/lora.cpp src/meshcore.cpp src/rtl_sdr.cpp `
    src/ed25519/fe.c src/ed25519/ge.c src/ed25519/sc.c `
    src/ed25519/sha512.c src/ed25519/verify.c `
    -o MeshCoreSniffer.exe
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Write-Host "built MeshCoreSniffer.exe"
