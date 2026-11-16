@echo off
rem Compiles every shader of the colour stage off-line. Same environment as src\build.bat.
setlocal
if defined D5_VCVARS call "%D5_VCVARS%" >nul
cd /d "%~dp0"
cl /nologo /utf-8 /std:c++17 /O1 /EHsc shadercheck.cpp /Fe:shadercheck.exe /link d3dcompiler.lib >build.log 2>&1
if errorlevel 1 (type build.log & exit /b 1)
"%~dp0shadercheck.exe"
endlocal
