@echo off
setlocal
set PATH=C:\msys64\mingw64\bin;%PATH%
g++ -std=c++17 -O2 -static -I src -I src\ed25519 src\main.cpp src\lora.cpp src\meshcore.cpp src\rtl_sdr.cpp src\ed25519\fe.c src\ed25519\ge.c src\ed25519\sc.c src\ed25519\sha512.c src\ed25519\verify.c -o MeshCoreSniffer.exe
if errorlevel 1 exit /b 1
echo built MeshCoreSniffer.exe
