@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
python3 tools/compile_nova_slang.py --deploy
msbuild.exe "DemonCore-Editor/DemonCore-Editor.vcxproj" /m:1 /nodeReuse:false /p:Configuration=Release /p:Platform=x64
