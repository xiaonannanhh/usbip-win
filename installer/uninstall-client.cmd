@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "PRODUCT=USBRelay client"
set "INSTALL_DIR=%ProgramFiles%\USBRelay\Client"
set "SYSTEM_DIR=%SystemRoot%\System32"
set "MONITOR_KEY=HKLM\SYSTEM\CurrentControlSet\Control\Print\Monitors\USBRelay Port Monitor"

call :require_admin
if errorlevel 1 exit /b 1

rem Deleting the installation directory would also delete this batch file
rem while cmd.exe is still reading it. Let a detached process do that after
rem this script has exited.
set "USBRELAY_DEFER_DELETE="
if /i "%~dp0"=="%INSTALL_DIR%\" set "USBRELAY_DEFER_DELETE=1"

echo Removing %PRODUCT%.
taskkill /f /t /im usbrelay-client-ui.exe >nul 2>&1
if exist "%INSTALL_DIR%\usbrelay-client-ui.exe" (
    "%INSTALL_DIR%\usbrelay-client-ui.exe" /cleanup >nul 2>&1
    start "" /wait "%INSTALL_DIR%\usbrelay-client-ui.exe" /remove-autostart >nul 2>&1
    start "" /wait "%INSTALL_DIR%\usbrelay-client-ui.exe" /remove-shortcuts >nul 2>&1
)

net stop spooler >nul 2>&1
timeout /t 2 /nobreak >nul 2>&1
reg delete "%MONITOR_KEY%\Ports" /f >nul 2>&1
reg delete "%MONITOR_KEY%" /f >nul 2>&1
del /f /q "%SYSTEM_DIR%\usbrelay_portmon.dll" >nul 2>&1

net start spooler >nul 2>&1
if errorlevel 1 (
    echo Failed to restart the Print Spooler service.
    exit /b 1
)

netsh advfirewall firewall delete rule name="USBRelay Client UDP 3241" >nul 2>&1
if defined USBRELAY_DEFER_DELETE (
    start "" /b "%ComSpec%" /d /c "ping 127.0.0.1 -n 3 >nul & rmdir /s /q "%INSTALL_DIR%""
    echo USBRelay client uninstallation completed.
    exit /b 0
)

if exist "%INSTALL_DIR%" rmdir /s /q "%INSTALL_DIR%"
if exist "%INSTALL_DIR%" (
    echo Failed to remove "%INSTALL_DIR%".
    exit /b 1
)

echo USBRelay client uninstallation completed.
exit /b 0

:require_admin
net session >nul 2>&1
if errorlevel 1 (
    echo Administrator privileges are required.
    exit /b 1
)
exit /b 0
