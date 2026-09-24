# 版本日志

版本号采用 `主版本.次版本.修订号`。每次 Git 提交会自动递增修订号，把提交说明写入版本摘要，并列出提交涉及的暂存文件。

## 0.1.3 - 2026-09-24

- 提交说明：fix: ensure changelog is committed before sync
- 涉及文件：`.githooks/post-commit.ps1`, `CHANGELOG.md`, VERSION, CHANGELOG.md.

## 0.1.2 - 2026-09-24

- 提交说明：feat: add Windows 7 LAN printer relay and versioned packages
- 涉及文件：`.githooks/commit-msg`, `.githooks/commit-msg.ps1`, `.githooks/post-commit`, `.githooks/post-commit.ps1`, `.githooks/pre-commit`, `.githooks/pre-commit.ps1`, `BUILD_WIN7_CN.md`, `CHANGELOG.md`, `README.md`, `VERSION_SYNC.md`, `driver/lib/libdrv.vcxproj`, `driver/stub/usbip_stub.inx`, `driver/stub/usbip_stub.vcxproj`, `driver/vhci/gencat.bat`, `driver/vhci/usbip_vhci.inf`, `driver/vhci/usbip_vhci.vcxproj`, `include/usbrelay_protocol.h`, `include/usbrelay_standard_protocol.h`, `installer/README.md`, `installer/README_CN.txt`.

## 0.1.1 - 2026-09-24

- 修复服务端磁盘写入较慢时可能忽略 TCP 已接收缓冲数据、导致打印任务被提前截断的问题。
- 保持一个 TCP 连接对应一个任务；收到 FIN 时结束任务，收到最后数据后连续 50 ms 无新数据时结束任务。
- 保留服务端任务阶段日志，记录接入、接收失败、已入队以及打印完成或失败，便于定位“客户端提示未打印”。
- 发布 x86/x64 通用服务端安装包；两个架构均完成仅提取验证，未执行安装。