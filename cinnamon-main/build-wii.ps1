# Builds wiitale.dol with devkitPPC.
#
# This wrapper exists because powerpc-eabi-gcc.exe is a native Windows binary and reads
# the Windows TMP variable. Git Bash exports that as a POSIX path, which gcc cannot use,
# so it falls back to C:\Windows\ and fails on a permission error. Setting a writable
# Windows-shaped TMP here is the whole trick.
#
#   powershell -ExecutionPolicy Bypass -File build-wii.ps1          # build
#   powershell -ExecutionPolicy Bypass -File build-wii.ps1 clean    # wipe the tree

$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$tmp = Join-Path $env:LOCALAPPDATA "Temp\wiitale-build"
if (-not (Test-Path $tmp)) { New-Item -ItemType Directory -Force $tmp | Out-Null }
$env:TMP = $tmp; $env:TEMP = $tmp; $env:TMPDIR = $tmp

$env:DEVKITPRO = "C:/devkitPro"; $env:DEVKITPPC = "C:/devkitPro/devkitPPC"
$env:PATH = "C:\devkitPro\devkitPPC\bin;C:\devkitPro\tools\bin;C:\devkitPro\msys2\usr\bin;" + $env:PATH

Set-Location $root
& "C:\devkitPro\msys2\usr\bin\make.exe" -f Makefile.wii @args 2>&1 |
    Select-String -NotMatch "^/usr/bin/sed"
