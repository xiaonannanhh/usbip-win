# USBRelay Standard TCP/IP 打印服务端

USBRelay Standard TCP/IP 是一个最低支持 Windows 7 SP1 的局域网打印
转发服务。它把服务端计算机上已经安装好的 Windows 打印队列发布为
RAW TCP 端口，客户端直接使用 Windows 自带的
`Standard TCP/IP Port` 连接，不需要安装客户端程序。

这个分支采用全新架构，不依赖下列旧方案：

- Windows 打印机共享、SMB、UNC、NetBIOS 打印机共享
- USB/IP、VHCI、USB/IP Stub
- 自定义打印端口监视器
- 内核驱动

服务端只向现有的 Windows 打印队列写入 RAW 数据，不会解绑 USB
打印机，不会修改原打印端口，本机仍然可以正常打印。

## 工作方式

- 服务端每 10 秒刷新本机打印队列。
- 每台打印队列分配一个固定的 RAW 端口，范围为 `9100-9199`。
- 端口映射保存在
  `HKLM\SOFTWARE\USBRelay\StandardServer\Ports`。
- 服务端在 `UDP 3251` 广播发现信息和查询应答。
- 服务端在 `TCP 9100-9199` 接收 RAW 打印数据。
- 客户端端口地址使用服务端计算机名，例如
  `DESKTOP-GZEJK10:9102`。

## 架构与兼容性

- 最低目标系统为 Windows 7 SP1，子系统版本为 `6.01`。
- 提供 x86、x64 和 Universal 三种安装包。
- x86 服务端和 x64 服务端使用相同协议。
- x86 客户端可以连接 x64 服务端。
- x64 客户端可以连接 x86 服务端。
- Universal 安装包根据当前 Windows 架构自动释放并运行对应的
  x86 或 x64 安装程序。
- 可执行文件使用静态 MSVC 运行库，不要求目标机器安装
  Visual C++ Redistributable。

## 主要文件

- `include/usbrelay_standard_protocol.h`
  Standard TCP/IP 服务协议常量。
- `userspace/src/usbrelay/usbrelay_standard_server.c`
  打印队列枚举、端口分配、发现广播和 RAW 转发引擎。
- `userspace/src/usbrelay/usbrelay_standard_server.h`
  服务端引擎接口。
- `installer/usbrelay_standard_server_ui.c`
  服务端托盘界面和打印机端点列表。
- `installer/install-standard-server.cmd`
  安装脚本。
- `installer/uninstall-standard-server.cmd`
  卸载脚本。
- `installer/README_STANDARD_SERVER_CN.txt`
  中文使用说明。
- `installer/build_standard_server_installers.ps1`
  x86/x64 安装包构建脚本。
- `installer/build_universal_standard_server_installer.ps1`
  Universal 安装包构建脚本。

## 构建

在源码根目录运行：

```powershell
& .\installer\build_standard_server_installers.ps1 -Architecture x86
& .\installer\build_standard_server_installers.ps1 -Architecture x64
& .\installer\build_universal_standard_server_installer.ps1
```

如果工具链不在默认位置，可以传入 `-ClPath`、`-RcPath` 或
`-OutputDir`。默认发布目录是仓库外层的：

```text
outputs\standard-tcpip-server
```

## 安装后的行为

- 默认安装目录为
  `%ProgramFiles%\USBRelay\StandardServer`。
- 创建桌面和开始菜单快捷方式。
- 创建登录启动任务 `USBRelay-Standard-Server-Autostart`。
- 启动后在系统右下角显示托盘图标。
- 托盘图标支持打开界面和退出。
- 添加 `TCP 9100-9199` 与 `UDP 3251` 入站防火墙规则。
- 卸载时只删除本程序、启动任务、快捷方式、防火墙规则和端口映射，
  不删除用户原有打印机和驱动。

## 客户端连接

客户端不需要安装 USBRelay。使用 Windows 自带的添加打印机流程：

1. 打开“控制面板”中的“设备和打印机”。
2. 选择“添加打印机”。
3. 选择“添加本地打印机”。
4. 选择“创建新端口”。
5. 端口类型选择 `Standard TCP/IP Port`。
6. 主机名填写服务端 UI 显示的计算机名，通常不需要固定 IP。
7. 设备类型选择 `TCP/IP Device`。
8. 协议选择 `RAW`，端口号填写该打印机对应的 `9100-9199`
   端口。
9. 安装与服务端打印机匹配的驱动。
10. 打印测试页。

不要选择“网络、无线或 Bluetooth 打印机”，也不要把服务端打印机
作为 Windows 共享打印机添加。这里创建的是客户端本地
Standard TCP/IP Port。

## Windows 7 验收

编译目标使用 Windows 7 API 和静态运行库，但最终打印机兼容性仍应在
真实的 Windows 7 SP1 x86 和 x64 环境中验证：

1. 安装服务端，确认本机原打印机仍可打印。
2. 在客户端确认可以发现服务端计算机名。
3. 创建 Standard TCP/IP Port，并确认客户端不需要安装 USBRelay。
4. 使用匹配的打印驱动打印测试页和实际作业。
5. 使用两个客户端同时提交作业。
6. 验证托盘图标、开机启动、退出和卸载。

## 网络与安全

- 服务端和客户端必须能够通过计算机名互相解析，可先用
  `ping 服务端计算机名` 检查。
- 协议没有身份验证和加密，只应部署在可信局域网。
- 不应把 `TCP 9100-9199` 暴露到互联网。
