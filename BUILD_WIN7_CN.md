# USBRelay Windows 7 构建与发布说明

USBRelay 是重新架构的局域网打印转发软件，目标为 Windows 7 SP1 及更高
版本，支持 x86 和 x64。它是用户态打印队列转发，不是 USB/IP 实现。

## 设计边界

- 服务端枚举本机 Windows 打印队列并继续使用本机 Print Spooler。
- 客户端只创建 USBRelay 自定义本地端口；用户在 Windows“添加打印机”向导
  中选择该现有端口，客户端不自动创建打印队列，也不提供或安装打印机驱动。
- 不使用 Windows 打印机共享、SMB、UNC、NetBIOS、USB/IP、VHCI、stub
  驱动或其他内核驱动。
- 服务端不解绑 USB 打印机，不改动原有打印机端口，本机打印不受影响。
- 多个客户端可以同时连接同一服务端；作业由服务端 Print Spooler 排队。
- 客户端在当前用户注册表中保存上次服务端和最近一次成功获取的打印机
  列表；重启后先立即显示缓存，再自动刷新。服务端暂时离线时保留缓存，
  不会把列表清空。
- x86/x64 只影响本机程序和端口监视器位数，网络协议兼容。

## 构建环境

推荐使用 64 位 Windows 构建主机、Visual Studio 2019 Build Tools 和
Windows SDK 10.0.19041.0。脚本默认路径为：

```text
C:\BuildTools2019\VC\Tools\MSVC\14.29.30133\bin\Hostx64\x86\cl.exe
C:\BuildTools2019\VC\Tools\MSVC\14.29.30133\bin\Hostx64\x64\cl.exe
C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x86\rc.exe
C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\rc.exe
```

如果工具安装位置不同，使用 `-ClPath`、`-RcPath` 覆盖参数。

## 构建命令

在源码根目录执行：

```powershell
& .\installer\build_installers.ps1 -Architecture x86
& .\installer\build_installers.ps1 -Architecture x64
& .\installer\build_universal_installers.ps1
```

输出目录默认为源码根目录上方的 `outputs`。每次架构构建会重新生成：

```text
USBRelay-x86-server
USBRelay-x86-client
USBRelay-x64-server
USBRelay-x64-client
```

Universal 构建需要 x86 和 x64 安装器先存在，然后生成两个自动判断系统
原生位数的安装器。Universal 外层本身是 x86 程序，因此可以在 Windows 7
x86 和 x64 上启动；在 x64 系统会释放并运行 x64 内层安装器，在 x86 系统
会运行 x86 内层安装器。可用 `/arch x86`、`/arch x64` 或 `/arch auto` 覆盖
自动判断。

## 发布物

最终发布目录应只保留以下 USBRelay 文件：

```text
USBRelay-Server-Setup-x86.exe
USBRelay-Client-Setup-x86.exe
USBRelay-Server-Setup-x64.exe
USBRelay-Client-Setup-x64.exe
USBRelay-Server-Setup-Universal.exe
USBRelay-Client-Setup-Universal.exe
USBRelay-x86-server\
USBRelay-x86-client\
USBRelay-x64-server\
USBRelay-x64-client\
```

客户端发布目录只需要 `usbrelay-client-ui.exe`、
`usbrelay_portmon.dll`、批处理、说明和许可证；`.lib`、`.exp`、`.obj`、
`.pdb` 等构建中间文件不得进入发布目录。

## 协议与防火墙

- UDP `3241`：服务端广播 `USBRELAY-DISCOVERY/1`。
- TCP `3242`：获取打印机列表和发送打印数据。

安装脚本只添加 USBRelay 自己的防火墙规则，不开启 135、139、445，也不
配置 Windows 打印机共享。

## 实机验收

必须在真实 Windows 7 SP1 x86 与 x64 环境各验收一次：

1. 安装服务端，确认 USB 打印机仍出现在本机打印机列表中并可本地打印。
2. 安装客户端，确认无需输入地址即可收到服务端广播。
3. 确认可以按计算机名、IPv4 或 DNS 名称连接。
4. 等待打印机列表成功后退出并重开客户端，确认缓存列表立即出现并自动刷新；
   临时断开服务端后确认列表仍保留。
5. 在客户端创建 USBRelay 本地端口，确认过程不要求安装驱动。
6. 通过 Windows 添加打印机向导选择该端口，并由 Windows 流程选择或安装
   与服务端匹配的 x86/x64 厂商驱动后建立队列。
7. 打印 Windows 测试页和实际文档。
8. 两台客户端同时提交作业，确认服务端队列正常排队。
9. 测试托盘唤起/退出、桌面快捷方式、开机启动和卸载。

端口监视器由 Print Spooler 加载，静态构建成功不能替代这组实机测试。
