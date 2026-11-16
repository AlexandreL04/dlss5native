@echo off
rem Builds hid.dll. Run from a "x64 Native Tools Command Prompt for VS", or set D5_VCVARS to a script that
rem sets up an x64 MSVC environment. The paths of the dependencies can be overridden with D5_NGX and D5_DETOURS.
setlocal
if defined D5_VCVARS call "%D5_VCVARS%" >nul
cd /d "%~dp0"
if not defined D5_NGX set D5_NGX=..\deps\Streamline\external\ngx-sdk\include
if not defined D5_DETOURS set D5_DETOURS=..\deps\Detours\src
if not exist obj mkdir obj
cl /nologo /utf-8 /std:c++17 /O2 /MT /EHsc /W3 /LD /DWIN32_LEAN_AND_MEAN /I"%D5_NGX%" /I"%D5_DETOURS%" /Foobj\ ^
  main.cpp settings.cpp ngx.cpp gate.cpp colorstage.cpp menu.cpp present.cpp ^
  "%D5_DETOURS%\detours.cpp" "%D5_DETOURS%\modules.cpp" "%D5_DETOURS%\disasm.cpp" "%D5_DETOURS%\image.cpp" "%D5_DETOURS%\creatwth.cpp" ^
  /Fe:hid.dll /link /DLL user32.lib gdi32.lib d3d12.lib dxgi.lib d3dcompiler.lib
endlocal
