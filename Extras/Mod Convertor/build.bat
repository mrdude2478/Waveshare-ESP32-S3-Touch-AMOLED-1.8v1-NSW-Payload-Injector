@echo off
REM Build mod2header.exe with the MSVC compiler.
REM Run this from a "Developer Command Prompt for VS" (or after calling
REM vcvarsall.bat) so that cl.exe is on your PATH.

cl /EHsc /std:c++17 /O2 /nologo mod2header.cpp /Fe:mod2header.exe

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Build failed. Make sure you're running this from a
    echo "Developer Command Prompt for VS" so cl.exe is available.
    exit /b 1
)

echo.
echo Build succeeded: mod2header.exe
echo You can now drag and drop a .mod file onto mod2header.exe
