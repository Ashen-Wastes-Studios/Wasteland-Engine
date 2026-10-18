@echo off
REM Build + run the diagnostic addendum (profile strings + DX12 alloc HRESULTs).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\dev\Wasteland\tools

echo === COMPILE ===
cl /nologo /EHsc /std:c++17 /W3 probe_dx_uav_diag.cpp ^
   d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib ^
   /Fe:probe_dx_uav_diag.exe
echo COMPILE_EXIT=%ERRORLEVEL%

if not "%ERRORLEVEL%"=="0" exit /b %ERRORLEVEL%

echo.
echo === RUN ===
probe_dx_uav_diag.exe
echo RUN_EXIT=%ERRORLEVEL%