@echo off
REM Undo-CL-Setup - reverts the changes made by CL Setup v1.2.1
REM Elevates to administrator if needed, then runs Undo-CL-Setup.ps1
setlocal
REM Admin check: reading the LOCAL SERVICE hive needs admin rights and does
REM not depend on the Server service (unlike "net session")
reg query HKU\S-1-5-19 >nul 2>&1
if %errorLevel%==0 goto run
echo Requesting administrator privileges...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
exit /b
:run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Undo-CL-Setup.ps1"
echo.
pause
