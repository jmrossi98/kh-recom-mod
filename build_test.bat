@echo off
rem Builds the two overlay test programs, which render the menu to a BMP
rem without the game: overlay_test.exe (D3D11) and overlay_test12.exe (D3D12).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%~dp0build\test" mkdir "%~dp0build\test"
set LIBS=user32.lib gdi32.lib d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib
cl /nologo /O2 /W4 /MT /EHsc /std:c++17 /Fo"%~dp0build\test\\" /Fe"%~dp0build\overlay_test.exe" "%~dp0tools_dev\overlay_test.cpp" "%~dp0src\overlay.cpp" /link /NOLOGO %LIBS%
cl /nologo /O2 /W4 /MT /EHsc /std:c++17 /Fo"%~dp0build\test\\" /Fe"%~dp0build\overlay_test12.exe" "%~dp0tools_dev\overlay_test12.cpp" "%~dp0src\overlay.cpp" /link /NOLOGO %LIBS%
