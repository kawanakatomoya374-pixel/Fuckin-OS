@echo off
REM C-OS 4.0.5 Demo Legacy 32-bit Windows Build Script (ARCHIVED)
REM This script is archived for historical reference only
REM Use build_windows.bat for current 64-bit builds

setlocal enabledelayedexpansion

echo WARNING: This is a legacy 64-bit build script
echo Use build_windows.bat for current 64-bit builds
echo.

REM Legacy 32-bit Configuration
set CC=C:\msys64\mingw64\bin\gcc.exe
set AS=C:\msys64\mingw64\bin\nasm.exe
set LD=C:\msys64\mingw64\bin\ld.exe
set OBJCOPY=C:\msys64\mingw64\bin\objcopy.exe
set BUILD_DIR=build_legacy
set SRC_DIR=src
set KERNEL_BIN=%BUILD_DIR%\kernel.bin
set ISO_IMAGE=%BUILD_DIR%\cos.iso

REM Legacy 64-bit Compiler flags
set CFLAGS=-Wall -Wextra -std=c99 -ffreestanding -nostdlib -fno-stack-protector -fno-pic -fno-pie -m32 -O2 -s -Wno-unused-variable -Wno-unused-function -Wno-unused-parameter
set INCLUDES=-I.\src\include -I.\src\kernel -I.\src\kernel\arch -I.\src\drivers\video -I.\src\drivers\input -I.\src\drivers\disk -I.\src\fs -I.\src\gui -I.\src\bios -I.\src\drivers

echo This 64-bit build system is deprecated.
echo See src/archive/README_32BIT_CLEANUP.md for migration notes.
pause
