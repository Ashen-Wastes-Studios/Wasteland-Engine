@echo off
set PATH=%PATH%;C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64
cd /d C:\dev\Wasteland\bin\Release-windows-x86_64\DemonCore-Editor
copy /Y "Renderer3D_NovaRenderer.cs.cso.bak" "Renderer3D_NovaRenderer.cs.cso" > nul
DemonCore-Editor.exe
