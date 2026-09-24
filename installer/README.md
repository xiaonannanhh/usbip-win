# USBRelay Windows 7 LAN printer relay

USBRelay is a custom user-mode LAN printer relay. The server publishes the
local Windows printer queues that it can enumerate. A client discovers the
server by UDP broadcast, downloads the published printer list, and creates a
local Windows port whose RAW data is sent to the server. The user then adds a
local printer in Windows and selects that existing `USBRELAY:` port.

This project does not use Windows printer sharing, SMB, UNC paths, NetBIOS
printer sharing, USB/IP, a VHCI device, a stub driver, or a kernel driver.
The server's original local printer queues remain installed and usable. Each
printer has its own serial relay queue: one TCP connection is one job, a TCP FIN
finishes it immediately, and five seconds without new data also finishes it.
Connections for the same printer are processed one at a time.

## Packages

Build one architecture at a time:

```powershell
.\build_installers.ps1 -Architecture x86
.\build_installers.ps1 -Architecture x64
```

The build writes:

- `USBRelay-Server-Setup-x86.exe`
- `USBRelay-Client-Setup-x86.exe`
- `USBRelay-Server-Setup-x64.exe`
- `USBRelay-Client-Setup-x64.exe`

After both architecture-specific pairs exist, build the 32-bit universal
launchers:

```powershell
.\build_universal_installers.ps1
```

The universal outputs are:

- `USBRelay-Server-Setup-Universal.exe`
- `USBRelay-Client-Setup-Universal.exe`

The universal launcher calls `GetNativeSystemInfo`, selects the native x86 or
x64 package, extracts only that package to a temporary directory, waits for it
to finish, and removes the temporary files. Therefore an x86 client can print
through an x64 server, because the network protocol is architecture-neutral.

## Network protocol

- UDP `3241`: server discovery broadcasts and client discovery receive.
- TCP `3242`: printer-list requests and RAW print jobs.

The discovery packet contains the server computer name, TCP port, and printer
count. The client UI displays both the computer name and the source IPv4
address. Selecting a discovered server fills the connection field and loads
its printer list. A computer name, IPv4 address, or resolvable DNS name can
also be entered manually.

Only these ports are added to Windows Firewall. The server opens TCP `3242`
and UDP `3241`; the client opens UDP `3241`. No SMB or Windows printer-sharing
firewall rules are added.

## Server installation

Run the server setup as administrator. It:

1. Ensures Print Spooler is automatic and running.
2. Copies the server UI to `%ProgramFiles%\USBRelay\Server`.
3. Starts the server UI, which launches its hidden background relay process.
4. Adds the two USBRelay firewall rules.
5. Creates Start Menu and desktop shortcuts.
6. Creates the optional logon startup task for the tray UI.

The server UI lists the local queues that will be published. Refresh the list
after adding or removing a local printer. Its hidden background mode publishes
the current list periodically; restarting is not required for normal queue
changes.

The installer removes the old `usbipd` service and the old
`C:\Program Files\USBIP-Win7` and `C:\Program Files\USBIP-Win7-Client`
directories only when the service binary path exactly matches the old project
path. It never removes `C:\Program Files\USBip`,
`C:\Program Files\usbipd-win`, or `C:\Program Files\USBRelayHost`.

## Client installation

Run the client setup as administrator. It installs the user-mode USBRelay
print port monitor for the native system architecture and registers it with
the Windows Print Spooler. It also creates shortcuts and the optional logon
startup task.

Select a discovered printer and click `创建本地端口`. The client creates only
the port; it does not create a printer queue and does not inspect, install, or
download printer drivers. Open the Windows Add Printer wizard, choose
`添加本地打印机`, select `使用现有端口`, and choose the listed USBRelay port.
Supply the matching x86/x64 vendor driver through the normal Windows workflow.
USBRelay does not include, download, or manufacture vendor printer drivers.
After the queue is created, any normal Windows application can print to it;
`设为默认` changes the default queue.

The local queue uses a port beginning with `USBRELAY:`. Client cleanup and
uninstall only remove queues and ports with that prefix. Existing local
printer queues and the server's physical USB printer remain untouched.

The client stores the last server and the latest successful printer metadata
under `HKCU\Software\USBRelay-Client`. On startup it displays that cached list
immediately and then refreshes it automatically. If the server is temporarily
offline, the cached list remains visible instead of being cleared.

## UI and startup

Both programs have a native Win32 UI, use the supplied application icon, and
place an icon in the notification area. Double-click the tray icon to reopen
the UI. Right-click it and choose `退出` to stop the UI and its hidden server
background process. The startup checkbox controls the logon task for the UI.

## Uninstall

Use the matching setup executable with `/uninstall`, or run the extracted
`uninstall-server.cmd` or `uninstall-client.cmd` as administrator. The client
uninstaller first removes only USBRelay-created queues and ports, then stops
the spooler, unregisters only the USBRelay monitor and DLL, restarts the
spooler, removes its firewall rule, tasks, shortcuts, and installation
directory.

The server uninstaller stops the UI and its hidden background relay, then
removes only the USBRelay firewall rules, tasks, shortcuts, and server
installation directory. It does not remove Windows printer queues.

## Requirements and troubleshooting

- Windows 7 SP1 or later, x86 or x64.
- Administrator rights for installation, queue creation, and uninstall.
- Print Spooler must be enabled on the server and client.
- The two computers must be on the same LAN or have routed access to TCP
  `3242` and UDP `3241`.
- If discovery is unavailable, enter the server computer name or IPv4 address
  manually and click `刷新打印机`.
- If a matching printer driver is not available in Windows, install the
  correct x86/x64 vendor driver through the normal Add Printer workflow.
- If the port is not listed by the Add Printer wizard, confirm the client status
  says `端口已创建`, refresh the wizard, and select `使用现有端口` rather than
  `创建新端口`.
- If printing fails, check that the server UI and its hidden background relay
  are running, TCP `3242` is reachable, and the server's local queue can print
  locally.

The build requires Visual Studio 2019 C/C++ tools, Windows SDK 10.0.19041.0,
and `makecab.exe`. Runtime installation uses only Windows `cmd.exe` and
`expand.exe`.
