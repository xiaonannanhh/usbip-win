#pragma once

#include <windows.h>

#define USBIPD_PRINT_PORT 3242

BOOL usbipd_print_relay_init(void);
void usbipd_print_relay_request_stop(void);
BOOL usbipd_print_relay_wait(DWORD timeout_ms);
void usbipd_print_relay_cleanup(void);
BOOL usbipd_print_relay_running(void);
