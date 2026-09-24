@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "PRODUCT=USBRelay client"
set "INSTALL_DIR=%ProgramFiles%\USBRelay\Client"
set "SYSTEM_DIR=%SystemRoot%\System32"
set "MONITOR_KEY=HKLM\SYSTEM\CurrentControlSet\Control\Print\Monitors\USBRelay Port Monitor"

call :require_admin
if errorlevel 1 exit /b 1

echo Installing %PRODUCT%.
if not exist "%INSTALL_DIR%" md "%INSTALL_DIR%" >nul 2>&1
if not exist "%INSTALL_DIR%" (
    echo Failed to create "%INSTALL_DIR%".
    exit /b 1
)

rem Remove only shortcuts owned by the old USBIP-Win7 package.
del /f /q "%PUBLIC%\Desktop\USBIP Client.lnk" >nul 2>&1
del /f /q "%PUBLIC%\Desktop\USBIP Server.lnk" >nul 2>&1
del /f /q "%ProgramData%\Microsoft\Windows\Start Menu\Programs\USBIP Client.lnk" >nul 2>&1
del /f /q "%ProgramData%\Microsoft\Windows\Start Menu\Programs\USBIP Server.lnk" >nul 2>&1

taskkill /f /t /im usbrelay-client-ui.exe >nul 2>&1
if exist "%INSTALL_DIR%\usbrelay-client-ui.exe" (
    "%INSTALL_DIR%\usbrelay-client-ui.exe" /cleanup >nul 2>&1
)

call :copy_payload usbrelay-client-ui.exe
if errorlevel 1 exit /b 1
call :copy_payload usbrelay_portmon.dll
if errorlevel 1 exit /b 1
call :copy_payload install-client.cmd
if errorlevel 1 exit /b 1
call :copy_payload uninstall-client.cmd
if errorlevel 1 exit /b 1
call :copy_payload README_CN.txt
if errorlevel 1 exit /b 1
call :copy_payload LICENSE
if errorlevel 1 exit /b 1
call :copy_payload COPYING
if errorlevel 1 exit /b 1

net stop spooler >nul 2>&1
timeout /t 2 /nobreak >nul 2>&1
copy /y "%INSTALL_DIR%\usbrelay_portmon.dll" "%SYSTEM_DIR%\usbrelay_portmon.dll" >nul
if errorlevel 1 (
    echo Failed to install the USBRelay print port monitor DLL.
    goto :failed
)

reg add "%MONITOR_KEY%" /v Driver /t REG_SZ /d usbrelay_portmon.dll /f >nul
if errorlevel 1 (
    echo Failed to register the USBRelay print port monitor.
    goto :failed
)
rem The Ports key is created by AddPort. Do not create an empty default
rem value here: the print spooler would enumerate it as a blank port.
reg delete "%MONITOR_KEY%\Ports" /ve /f >nul 2>&1

sc.exe config spooler start= auto >nul 2>&1
net start spooler >nul 2>&1
for /l %%I in (1,1,15) do (
    sc.exe query spooler | findstr /i /c:"RUNNING" >nul 2>&1
    if not errorlevel 1 goto :spooler_ready
    timeout /t 1 /nobreak >nul 2>&1
)
echo Failed to start the Print Spooler service.
goto :failed

:spooler_ready
if not exist "%SYSTEM_DIR%\spool\PRINTERS" (
    rem The spooler may report RUNNING just before its queue directory is
    rem ready on older Windows versions. The directory is normally present;
    rem this check only avoids treating a transient start result as success.
    timeout /t 1 /nobreak >nul 2>&1
)
if errorlevel 1 (
    echo Failed to start the Print Spooler service.
    goto :failed
)

netsh advfirewall firewall delete rule name="USBRelay Client UDP 3241" >nul 2>&1
netsh advfirewall firewall add rule name="USBRelay Client UDP 3241" dir=in action=allow protocol=UDP localport=3241 profile=any >nul
if errorlevel 1 (
    echo Failed to configure the Windows Firewall.
    goto :failed
)

start "" /wait "%INSTALL_DIR%\usbrelay-client-ui.exe" /install-shortcuts
if errorlevel 1 goto :shortcut_failed
start "" /wait "%INSTALL_DIR%\usbrelay-client-ui.exe" /install-autostart
if errorlevel 1 goto :shortcut_failed

echo USBRelay client installation completed.
pushd "%INSTALL_DIR%"
start "USBRelay client" "%INSTALL_DIR%\usbrelay-client-ui.exe"
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

:shortcut_failed
echo Failed to create USBRelay shortcuts or startup task.
goto :failed

:failed
net start spooler >nul 2>&1
exit /b 1
