@echo off
cd /d C:\dev\Wasteland
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
msbuild.exe "DemonCore-Editor\DemonCore-Editor.vcxproj" /m:1 /nodeReuse:false /p:Configuration=Release /p:Platform=x64
