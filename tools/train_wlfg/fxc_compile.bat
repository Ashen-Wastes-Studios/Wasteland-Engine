@echo off
setlocal
set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
set SHADER="C:\dev\Wasteland\DemonCore-Editor\assets\shaders\Renderer3D_NovaRenderer.hlsl"
set OUT="C:\dev\Wasteland\bin\Release-windows-x86_64\DemonCore-Editor\assets\shaders\Renderer3D_NovaRenderer.cs.cso"
%FXC% /T cs_5_1 /E main /D WASTELAND_NOVA_TERRAIN_GRID=1 /Fo %OUT% %SHADER% 2>"C:\Users\rtoue\AppData\Local\hermes\cache\scratch\fxc_compile_err.txt"
echo Exit code: %ERRORLEVEL%
echo --- Errors (X4) ---
type "C:\Users\rtoue\AppData\Local\hermes\cache\scratch\fxc_compile_err.txt" 2>nul | findstr /C:"X4" /C:"error" | head -20
