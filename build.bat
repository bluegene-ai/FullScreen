@echo off
REM ============================================================
REM FullScreen Browser - Build Script (MSVC)
REM ============================================================
REM Prerequisites:
REM   - Visual Studio 2022 with Desktop C++ workload
REM   - WebView2 NuGet package
REM
REM Setup:
REM   nuget install Microsoft.Web.WebView2 -OutputDirectory packages
REM   (or download SDK from https://www.nuget.org/packages/Microsoft.Web.WebView2)
REM ============================================================

setlocal enabledelayedexpansion

REM --- Find MSVC environment ---
if not defined DevEnvDir (
    echo [ERROR] Please run from a Visual Studio Developer Command Prompt.
    echo         e.g. "x64 Native Tools Command Prompt for VS 2022"
    exit /b 1
)

set SRC_DIR=%~dp0src
set OUT_DIR=%~dp0build
set WEBVIEW2_DIR=%~dp0packages\Microsoft.Web.WebView2.1.0.2903.40

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

REM --- Check WebView2 SDK ---
set WEBVIEW2_INCLUDE=%WEBVIEW2_DIR%\build\native\include
set WEBVIEW2_LIB=%WEBVIEW2_DIR%\build\native\x64
if not exist "%WEBVIEW2_INCLUDE%" (
    echo [ERROR] WebView2 SDK not found at %WEBVIEW2_DIR%
    echo.
    echo   Run: nuget install Microsoft.Web.WebView2 -OutputDirectory packages
    echo   Or download from: https://www.nuget.org/packages/Microsoft.Web.WebView2
    exit /b 1
)

REM --- Compile ---
echo [BUILD] Compiling FullScreen Browser...

set CFLAGS=/nologo /W4 /EHsc /std:c++17 /MT /O2 /DNDEBUG /DUNICODE /D_UNICODE
set CFLAGS=%CFLAGS% /I"%SRC_DIR%" /I"%WEBVIEW2_INCLUDE%"

set LFLAGS=/link /NOLOGO /SUBSYSTEM:WINDOWS /MACHINE:X64
set LFLAGS=%LFLAGS% /LIBPATH:"%WEBVIEW2_LIB%"
set LFLAGS=%LFLAGS% user32.lib gdi32.lib winhttp.lib ole32.lib oleaut32.lib
set LFLAGS=%LFLAGS% shlwapi.lib advapi32.lib shell32.lib comctl32.lib
set LFLAGS=%LFLAGS% WebView2LoaderStatic.lib

REM Compile resource file
rc /nologo /fo "%OUT_DIR%\resource.res" "%SRC_DIR%\resource.rc"
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Resource compilation failed.
    exit /b 1
)

REM Compile all .cpp files
set OBJ_FILES=
for %%f in ("%SRC_DIR%\*.cpp") do (
    set OBJ="%OUT_DIR%\%%~nf.obj"
    set OBJ_FILES=!OBJ_FILES! !OBJ!
    echo   Compiling %%~nxf...
    cl %CFLAGS% /c /Fo"!OBJ!" "%%f"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Compilation failed for %%f
        exit /b 1
    )
)

REM Link
echo [LINK] Linking FullScreenBrowser.exe...
cl %CFLAGS% /Fe"%OUT_DIR%\FullScreenBrowser.exe" %OBJ_FILES% "%OUT_DIR%\resource.res" %LFLAGS%
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Linking failed.
    exit /b 1
)

echo.
echo [SUCCESS] Build complete!
echo   Output: %OUT_DIR%\FullScreenBrowser.exe
for %%A in ("%OUT_DIR%\FullScreenBrowser.exe") do echo   Size:   %%~zA bytes

endlocal
