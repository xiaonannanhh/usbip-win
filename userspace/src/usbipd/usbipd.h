#pragma once

#include <winsock2.h>
#include <windows.h>

#include "usbip_common.h"

extern int recv_request_import(SOCKET sockfd);
extern int recv_request_devlist(SOCKET connfd);

BOOL usbipd_forwarders_init(void);
void usbipd_forwarders_cleanup(void);
BOOL usbipd_forwarder_start(HANDLE hdev, SOCKET sockfd);
void usbipd_forwarders_request_stop(void);
BOOL usbipd_forwarders_wait(DWORD timeout_ms);
BOOL usbipd_is_stopping(void);

#include "usbipd_print.h"
