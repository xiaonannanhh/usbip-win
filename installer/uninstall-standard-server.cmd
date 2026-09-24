@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

set "PRODUCT=打印机内网共享服务端"
set "INSTALL_DIR=%ProgramFiles%\USBRelay\StandardServer"
set "UI_NAME=usbrelay-standard-server-ui.exe"
set "SCRIPT_DIR=%~dp0"

call :require_admin
if errorlevel 1 exit /b 1

rem Deleting the installation directory would also delete this batch file
rem while cmd.exe is still reading it. Let a detached process finish cleanup.
set "USBRELAY_DEFER_DELETE="
if /i "!SCRIPT_DIR!"=="!INSTALL_DIR!\" set "USBRELAY_DEFER_DELETE=1"

echo Removing %PRODUCT%.
if exist "!INSTALL_DIR!\%UI_NAME%" (
    taskkill /f /t /im "%UI_NAME%" >nul 2>&1
    start "" /wait "!INSTALL_DIR!\%UI_NAME%" /remove-autostart >nul 2>&1
    start "" /wait "!INSTALL_DIR!\%UI_NAME%" /remove-shortcuts >nul 2>&1
)

timeout /t 1 /nobreak >nul 2>&1

netsh advfirewall firewall delete rule name="USBRelay Standard TCP/IP Server TCP 9100-9199" >nul 2>&1
netsh advfirewall firewall delete rule name="USBRelay Standard TCP/IP Server UDP 3251" >nul 2>&1
netsh advfirewall firewall delete rule name="打印机内网共享服务端 TCP 9100-9199" >nul 2>&1
netsh advfirewall firewall delete rule name="打印机内网共享服务端 UDP 3251" >nul 2>&1
reg.exe delete "HKLM\SOFTWARE\USBRelay\StandardServer" /f >nul 2>&1

if defined USBRELAY_DEFER_DELETE (
    set "DELETE_SCRIPT=%TEMP%\USBRelay-Standard-Server-Delete-%RANDOM%-%RANDOM%.cmd"
    >"!DELETE_SCRIPT!" echo @echo off
    >>"!DELETE_SCRIPT!" echo ping 127.0.0.1 -n 3 ^>nul
    >>"!DELETE_SCRIPT!" echo rmdir /s /q "!INSTALL_DIR!"
    >>"!DELETE_SCRIPT!" echo del /q "%%~f0" ^>nul 2^>^&1
    >>"!DELETE_SCRIPT!" echo exit /b 0
    start "" /b "%ComSpec%" /d /c call "!DELETE_SCRIPT!"
    echo %PRODUCT% uninstallation completed.
    exit /b 0
)

if exist "!INSTALL_DIR!" rmdir /s /q "!INSTALL_DIR!"
if exist "!INSTALL_DIR!" (
    echo Failed to remove "!INSTALL_DIR!".
    exit /b 1
)

echo %PRODUCT% uninstallation completed.
exit /b 0

:require_admin
 fltmc >nul 2>&1
if errorlevel 1 (
    echo Administrator privileges are required.
    exit /b 1
)
exit /b 0
