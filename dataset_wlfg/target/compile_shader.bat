@echo off
"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe" -T cs_5_0 -E main -D WASTELAND_NOVA_TERRAIN_GRID=1 -Fo "C:\dev\Wasteland\dataset_wlfg\target\nova_shader_test.cs.cso" "C:\dev\Wasteland\bin\Release-windows-x86_64\DemonCore-Editor\assets\shaders\Renderer3D_NovaRenderer.hlsl"
echo EXIT_CODE: %ERRORLEVEL%
