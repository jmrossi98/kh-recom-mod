@echo off
rem Builds build\recom_combat.dll. The game locks the dll while it is running.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%~dp0build" mkdir "%~dp0build"
cl /nologo /LD /O2 /W4 /MT /std:c++17 /Fo"%~dp0build\\" /Fe"%~dp0build\recom_combat.dll" "%~dp0src\combat.cpp" /link /NOLOGO user32.lib
