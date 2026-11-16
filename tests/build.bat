@echo off
rem Builds and runs the off-line tests with AddressSanitizer. Same environment as src\build.bat.
setlocal
if defined D5_VCVARS call "%D5_VCVARS%" >nul
cd /d "%~dp0"
if not defined D5_NGX set D5_NGX=..\deps\Streamline\external\ngx-sdk\include
if not defined D5_DETOURS set D5_DETOURS=..\deps\Detours\src
if not exist obj mkdir obj
cl /nologo /utf-8 /std:c++17 /Od /Zi /MD /EHsc /W3 /fsanitize=address /DWIN32_LEAN_AND_MEAN /I"%D5_NGX%" /I"%D5_DETOURS%" /Foobj\ /Fdobj\ ^
  stage_test.cpp ..\src\colorstage.cpp ..\src\menu.cpp ..\src\settings.cpp ^
  /Fe:stage_test.exe /link /DEBUG user32.lib gdi32.lib d3d12.lib dxgi.lib d3dcompiler.lib dxguid.lib
if errorlevel 1 exit /b 1
"%~dp0stage_test.exe" %*
endlocal
