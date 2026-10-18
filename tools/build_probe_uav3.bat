@echo off
REM Isolate the DX12 CreateCommittedResource E_INVALIDARG.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\dev\Wasteland\tools

echo === COMPILE ===
cl /nologo /EHsc /std:c++17 /W3 probe_dx12_alloc.cpp ^
   d3d12.lib dxgi.lib dxguid.lib ^
   /Fe:probe_dx12_alloc.exe
echo COMPILE_EXIT=%ERRORLEVEL%

if not "%ERRORLEVEL%"=="0" exit /b %ERRORLEVEL%

echo.
echo === RUN ===
probe_dx12_alloc.exe
echo RUN_EXIT=%ERRORLEVEL%