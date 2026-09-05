@echo off
REM SyncAssets.bat — copies the source assets over the stale copies next to the built exes.
REM Source of truth: DemonCore-Editor\assets\  ->  bin\<Config>\DemonCore-Editor\assets\
REM Run this after editing scenes/textures/shaders/scripts in the source tree
REM instead of doing a full rebuild. Safe to run while the editor is open (assets
REM aren't locked; only the .exe relink needs the editor closed).
setlocal

set "SRC=%~dp0DemonCore-Editor\assets"
if not exist "%SRC%" (
    echo ERROR: source assets not found at "%SRC%"
    exit /b 1
)

set SYNCED=0
for %%C in (Release-windows-x86_64 Debug-windows-x86_64) do (
    if exist "%~dp0bin\%%C\DemonCore-Editor" (
        echo Syncing assets -^> bin\%%C\DemonCore-Editor\assets ...
        xcopy /Y /E /I /D "%SRC%" "%~dp0bin\%%C\DemonCore-Editor\assets\"
        if errorlevel 1 (
            echo WARNING: xcopy to bin\%%C reported an issue.
        ) else (
            set SYNCED=1
        )
    )
)

if "%SYNCED%"=="0" (
    echo No bin output dirs found — build once first so bin\^<Config^>\DemonCore-Editor exists.
    exit /b 1
)

echo Done.
