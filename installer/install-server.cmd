@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "PRODUCT=USBRelay server"
set "INSTALL_DIR=%ProgramFiles%\USBRelay\Server"
set "OLD_SERVER_DIR=C:\Program Files\USBIP-Win7"
set "OLD_CLIENT_DIR=C:\Program Files\USBIP-Win7-Client"

call :require_admin
if errorlevel 1 exit /b 1

echo Installing %PRODUCT%.
call :remove_legacy_project
if errorlevel 1 exit /b 1
call :remove_legacy_shortcuts
if errorlevel 1 exit /b 1

taskkill /f /t /im usbrelay-server-ui.exe >nul 2>&1
timeout /t 1 /nobreak >nul 2>&1

if not exist "%INSTALL_DIR%" md "%INSTALL_DIR%" >nul 2>&1
if not exist "%INSTALL_DIR%" (
    echo Failed to create "%INSTALL_DIR%".
    exit /b 1
)

call :copy_payload usbrelay-server-ui.exe
if errorlevel 1 exit /b 1
call :copy_payload install-server.cmd
if errorlevel 1 exit /b 1
call :copy_payload uninstall-server.cmd
if errorlevel 1 exit /b 1
call :copy_payload README_CN.txt
if errorlevel 1 exit /b 1
call :copy_payload LICENSE
if errorlevel 1 exit /b 1
call :copy_payload COPYING
if errorlevel 1 exit /b 1

sc.exe config spooler start= auto >nul 2>&1
net start spooler >nul 2>&1

netsh advfirewall firewall delete rule name="USBRelay Server TCP 3242" >nul 2>&1
netsh advfirewall firewall delete rule name="USBRelay Server UDP 3241" >nul 2>&1
netsh advfirewall firewall add rule name="USBRelay Server TCP 3242" dir=in action=allow protocol=TCP localport=3242 profile=any >nul
if errorlevel 1 goto :firewall_failed
netsh advfirewall firewall add rule name="USBRelay Server UDP 3241" dir=in action=allow protocol=UDP localport=3241 profile=any >nul
if errorlevel 1 goto :firewall_failed

start "" /wait "%INSTALL_DIR%\usbrelay-server-ui.exe" /install-shortcuts
if errorlevel 1 goto :shortcut_failed
start "" /wait "%INSTALL_DIR%\usbrelay-server-ui.exe" /install-autostart
if errorlevel 1 goto :shortcut_failed

echo USBRelay server installation completed.
pushd "%INSTALL_DIR%"
start "USBRelay server" "%INSTALL_DIR%\usbrelay-server-ui.exe"
popd
exit /b 0

:copy_payload
if not exist "%~dp0%~1" (
    echo Missing payload file: "%~dp0%~1".
    exit /b 1
)
copy /y "%~dp0%~1" "%INSTALL_DIR%\%~1" >nul
if errorlevel 1 (
    echo Failed to copy "%~1".
    exit /b 1
)
exit /b 0

:require_admin
net session >nul 2>&1
if errorlevel 1 (
    echo Administrator privileges are required.
    exit /b 1
)
exit /b 0

:remove_legacy_project
set "LEGACY_SAFE=0"
set "LEGACY_BINARY="
sc.exe query usbipd >nul 2>&1
if errorlevel 1 (
    set "LEGACY_SAFE=1"
) else (
    for /f "tokens=1,* delims=:" %%A in ('sc.exe qc usbipd ^| findstr /i "BINARY_PATH_NAME"') do if not defined LEGACY_BINARY set "LEGACY_BINARY=%%B"
    for /f "tokens=*" %%A in ("!LEGACY_BINARY!") do set "LEGACY_BINARY=%%A"
    set "LEGACY_NORMALIZED=!LEGACY_BINARY:"=!"
    if /i "!LEGACY_NORMALIZED!"=="C:\Program Files\USBIP-Win7\usbipd.exe --service" set "LEGACY_SAFE=1"
    if /i "!LEGACY_NORMALIZED!"=="C:\Program Files\USBIP-Win7\usbipd.exe" set "LEGACY_SAFE=1"
    if "!LEGACY_SAFE!"=="1" (
        echo Removing the old USBIP-Win7 service.
        sc.exe stop usbipd >nul 2>&1
        timeout /t 2 /nobreak >nul 2>&1
        sc.exe delete usbipd >nul 2>&1
        timeout /t 2 /nobreak >nul 2>&1
    ) else (
        echo The existing usbipd service does not match the old USBIP-Win7 path.
        echo Its files were left untouched for safety.
        exit /b 1
    )
)

if "!LEGACY_SAFE!"=="1" (
    if exist "%OLD_SERVER_DIR%" rmdir /s /q "%OLD_SERVER_DIR%"
    if exist "%OLD_CLIENT_DIR%" rmdir /s /q "%OLD_CLIENT_DIR%"
    if exist "%OLD_SERVER_DIR%" (
        echo Failed to remove "%OLD_SERVER_DIR%".
        exit /b 1
    )
    if exist "%OLD_CLIENT_DIR%" (
        echo Failed to remove "%OLD_CLIENT_DIR%".
        exit /b 1
    )
)
exit /b 0

:remove_legacy_shortcuts
rem Remove only shortcuts owned by the old USBIP-Win7 package.
del /f /q "%PUBLIC%\Desktop\USBIP Client.lnk" >nul 2>&1
del /f /q "%PUBLIC%\Desktop\USBIP Server.lnk" >nul 2>&1
del /f /q "%ProgramData%\Microsoft\Windows\Start Menu\Programs\USBIP Client.lnk" >nul 2>&1
del /f /q "%ProgramData%\Microsoft\Windows\Start Menu\Programs\USBIP Server.lnk" >nul 2>&1
exit /b 0

:firewall_failed
echo Failed to configure the Windows Firewall.
exit /b 1

:shortcut_failed
echo Failed to create USBRelay shortcuts or startup task.
exit /b 1
