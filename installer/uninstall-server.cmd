@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "PRODUCT=USBRelay server"
set "INSTALL_DIR=%ProgramFiles%\USBRelay\Server"

call :require_admin
if errorlevel 1 exit /b 1

rem Deleting the installation directory would also delete this batch file
rem while cmd.exe is still reading it. Let a detached process do that after
rem this script has exited.
set "USBRELAY_DEFER_DELETE="
if /i "%~dp0"=="%INSTALL_DIR%\" set "USBRELAY_DEFER_DELETE=1"

echo Removing %PRODUCT%.
if exist "%INSTALL_DIR%\usbrelay-server-ui.exe" (
    taskkill /f /t /im usbrelay-server-ui.exe >nul 2>&1
    start "" /wait "%INSTALL_DIR%\usbrelay-server-ui.exe" /remove-autostart >nul 2>&1
    start "" /wait "%INSTALL_DIR%\usbrelay-server-ui.exe" /remove-shortcuts >nul 2>&1
)

timeout /t 1 /nobreak >nul 2>&1

netsh advfirewall firewall delete rule name="USBRelay Server TCP 3242" >nul 2>&1
netsh advfirewall firewall delete rule name="USBRelay Server UDP 3241" >nul 2>&1

if defined USBRELAY_DEFER_DELETE (
    start "" /b "%ComSpec%" /d /c "ping 127.0.0.1 -n 3 >nul & rmdir /s /q "%INSTALL_DIR%""
    echo USBRelay server uninstallation completed.
    exit /b 0
)

if exist "%INSTALL_DIR%" rmdir /s /q "%INSTALL_DIR%"
if exist "%INSTALL_DIR%" (
    echo Failed to remove "%INSTALL_DIR%".
    exit /b 1
)

echo USBRelay server uninstallation completed.
exit /b 0

:require_admin
net session >nul 2>&1
if errorlevel 1 (
    echo Administrator privileges are required.
    exit /b 1
)
exit /b 0
