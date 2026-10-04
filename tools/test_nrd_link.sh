#!/usr/bin/env bash
# Build and run tools/nrd_link_test.cpp: proves NRD.lib links and that its
# embedded shader bytecode is present.
#
# A build-only check cannot catch the failure this guards against -- NRD.lib
# links cleanly even when it contains no shaders at all, because the blobs are
# generated at NRD build time. So actually create an instance and read back the
# pipeline descriptors.
set -euo pipefail

cd "$(dirname "$0")/.."

OUT=".hermes"

if [ ! -f "tools/nrd_link_test.cpp" ]; then
  echo "tools/nrd_link_test.cpp missing" >&2
  exit 1
fi
if [ ! -f "Wasteland/vendor/NRD/_Bin/Release/NRD.lib" ]; then
  echo "NRD.lib not built -- run: python tools/build_nrd.py" >&2
  exit 1
fi

mkdir -p "$OUT"

# cl.exe needs the MSVC + Windows SDK environment that only vcvars sets up; the
# CRT import libs (MSVCRT.lib) are not discoverable by hand.
# Written as a .bat rather than passed to `cmd /c` because the vcvars path
# contains spaces and nested quoting is unreliable through MSYS.
BAT="$OUT/build_nrd_link_test.bat"
cat > "$BAT" << 'EOF'
@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0\.."
cl /nologo /EHsc /std:c++17 /MD /I Wasteland\vendor\NRD\Include tools\nrd_link_test.cpp /Fe:.hermes\nrd_link_test.exe /Fo:.hermes\ /link /LIBPATH:Wasteland\vendor\NRD\_Bin\Release NRD.lib
EOF

cmd.exe /c "$(cygpath -w "$BAT")"

cp Wasteland/vendor/NRD/_Bin/Release/NRD.dll "$OUT/"
"$OUT/nrd_link_test.exe"
