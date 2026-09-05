@echo off
rem Locates the PlatformIO CLI and runs it with the given arguments.
rem Works whether pio is on PATH or only installed via the PlatformIO IDE extension.
setlocal

where pio >nul 2>nul
if %ERRORLEVEL%==0 (
    pio %*
    exit /b %ERRORLEVEL%
)

if exist "%USERPROFILE%\.platformio\penv\Scripts\pio.exe" (
    "%USERPROFILE%\.platformio\penv\Scripts\pio.exe" %*
    exit /b %ERRORLEVEL%
)

echo Error: could not find the PlatformIO "pio" executable. 1>&2
echo Install PlatformIO Core, or the PlatformIO IDE VS Code extension. 1>&2
exit /b 1
