@echo off
REM Build + run the DX11/DX12 compute-UAV-slot probe.
REM All output stays under tools\ ; no engine file is touched.
REM
REM The DX12 side needs a cs_6_0 DXIL declaring 64 UAVs, because SM5.1 DXBC caps
REM at 8 UAV declarations -- compiling the DX12 probe from DXBC would silently
REM report 8 and make the side-by-side comparison meaningless.
REM nova_uav64.hlsl is generated first (python one-liner), then compiled to DXIL.

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d C:\dev\Wasteland\tools

echo === COMPILE ===
cl /nologo /EHsc /std:c++17 /W3 probe_dx_uav_slots.cpp ^
   d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib ^
   /Fe:probe_dx_uav_slots.exe
echo COMPILE_EXIT=%ERRORLEVEL%

if not "%ERRORLEVEL%"=="0" exit /b %ERRORLEVEL%

echo.
echo === DXC: 64-UAV cs_6_0 to DXIL ===
"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe" /T cs_6_0 /E main /Fh nova_uav64.dxil nova_uav64.hlsl
echo DXC_EXIT=%ERRORLEVEL%

echo.
echo === RUN ===
probe_dx_uav_slots.exe nova_uav64.dxil
echo RUN_EXIT=%ERRORLEVEL%