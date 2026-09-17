# C-OS 4.0.7 Demo Legacy 32-bit Build Script (ARCHIVED)
# This script is archived for historical reference only
# Use build.ps1 for current 64-bit builds

param(
    [switch]$Clean,
    [switch]$Debug,
    [switch]$Help
)

# Show help
if ($Help) {
    Write-Host "C-OS 4.0.7 Demo Legacy 32-bit Build Script (ARCHIVED)"
    Write-Host "This script is deprecated and kept for archive purposes"
    Write-Host "Use build.ps1 for current 64-bit builds"
    Write-Host ""
    Write-Host "This script builds 32-bit C-OS 4.0.7 Demo kernel (LEGACY)"
    exit 0
}

Write-Host "WARNING: This is a legacy 32-bit build script"
Write-Host "Use build.ps1 for current 64-bit builds"
Write-Host ""

# Legacy 32-bit Configuration
$CC = "gcc"
$LD = "ld"
$OBJCOPY = "objcopy"
$BUILD_DIR = "build_legacy"
$SRC_DIR = "src"
$KERNEL_BIN = "$BUILD_DIR\kernel.bin"
$ISO_IMAGE = "$BUILD_DIR\cos.iso"

# Legacy 32-bit Compiler flags
$CFLAGS = "-m32 -ffreestanding -nostdlib -nodefaultlibs -fno-builtin -fno-pic -Wall -Wextra"
$LDFLAGS = "-m32 -nostdlib -nodefaultlibs -T src/boot/linker.ld"
$INCLUDES = "-I src/include"

# 32-bit specific defines
$CFLAGS += "-D__i386__ -D__ILP32__"

Write-Host "This 32-bit build system is deprecated."
Write-Host "See src/archive/README_32BIT_CLEANUP.md for migration notes."
