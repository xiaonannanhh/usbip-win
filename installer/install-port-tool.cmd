@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

set "PRODUCT=打印机内网共享配置工具端"
set "INSTALL_DIR=%ProgramFiles%\USBRelay\PortTool"
set "UI_NAME=usbrelay-port-tool-ui.exe"

call :require_admin
if errorlevel 1 exit /b 1

echo Installing %PRODUCT%.
taskkill /f /t /im "%UI_NAME%" >nul 2>&1
timeout /t 1 /nobreak >nul 2>&1

if not exist "!INSTALL_DIR!" md "!INSTALL_DIR!" >nul 2>&1
if not exist "!INSTALL_DIR!" (
    echo Failed to create "!INSTALL_DIR!".
    exit /b 1
)

call :copy_payload "%UI_NAME%"
if errorlevel 1 exit /b 1
call :copy_payload install-port-tool.cmd
if errorlevel 1 exit /b 1
call :copy_payload uninstall-port-tool.cmd
if errorlevel 1 exit /b 1
call :copy_payload README_PORT_TOOL_CN.txt
if errorlevel 1 exit /b 1
call :copy_payload README_PORT_TOOL_MANUAL_CN.md
if errorlevel 1 exit /b 1
call :copy_payload LICENSE
if errorlevel 1 exit /b 1
call :copy_payload COPYING
if errorlevel 1 exit /b 1

sc.exe config spooler start= auto >nul 2>&1
net start spooler >nul 2>&1

netsh advfirewall firewall delete rule name="USBRelay Standard TCP/IP Port Tool UDP 3251" >nul 2>&1
netsh advfirewall firewall delete rule name="打印机内网共享配置工具端 UDP 3251" >nul 2>&1
netsh advfirewall firewall add rule name="打印机内网共享配置工具端 UDP 3251" dir=in action=allow protocol=UDP localport=3251 profile=any >nul
if errorlevel 1 goto :firewall_failed

start "" /wait "!INSTALL_DIR!\%UI_NAME%" /install-shortcuts
if errorlevel 1 goto :shortcut_failed
start "" /wait "!INSTALL_DIR!\%UI_NAME%" /install-autostart
if errorlevel 1 goto :shortcut_failed

echo %PRODUCT% installation completed.
pushd "!INSTALL_DIR!"
start "打印机内网共享配置工具端" "!INSTALL_DIR!\%UI_NAME%"
popd
exit /b 0

:copy_payload
if not exist "%~dp0%~1" (
    echo Missing payload file: "%~dp0%~1".
    exit /b 1
)
copy /y "%~dp0%~1" "!INSTALL_DIR!\%~1" >nul
if errorlevel 1 (
    echo Failed to copy "%~1".
    exit /b 1
)
exit /b 0

:require_admin
 fltmc >nul 2>&1
if errorlevel 1 (
    echo Administrator privileges are required.
    exit /b 1
)
exit /b 0

:firewall_failed
echo Failed to configure Windows Firewall for UDP 3251.
exit /b 1

:shortcut_failed
echo Failed to create shortcuts or the logon startup task.
exit /b 1
