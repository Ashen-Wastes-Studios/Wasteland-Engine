@echo off
setlocal
set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
set SHADER="C:\dev\Wasteland\DemonCore-Editor\assets\shaders\Renderer3D_NovaRenderer.hlsl"
%FXC% /T cs_5_1 /E main /D WASTELAND_NOVA_TERRAIN_GRID=1 /Fl "C:\Users\rtoue\AppData\Local\hermes\cache\scratch\fxc_out.txt" %SHADER% 2>"C:\Users\rtoue\AppData\Local\hermes\cache\scratch\fxc_err.txt"
echo Exit code: %ERRORLEVEL%
echo --- stdout ---
type "C:\Users\rtoue\AppData\Local\hermes\cache\scratch\fxc_out.txt" 2>nul | findstr /C:"error" /C:"warning" /C:"X4" | head -30
echo --- stderr ---
type "C:\Users\rtoue\AppData\Local\hermes\cache\scratch\fxc_err.txt" 2>nul | head -30
