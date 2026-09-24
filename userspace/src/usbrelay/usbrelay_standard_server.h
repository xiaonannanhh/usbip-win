#ifndef USBRELAY_STANDARD_SERVER_H
#define USBRELAY_STANDARD_SERVER_H

#include <windows.h>

#define USBRELAY_STANDARD_NAME_CHARS 256
#define USBRELAY_STANDARD_DRIVER_CHARS 256
#define USBRELAY_STANDARD_PORT_NAME_CHARS 128
#define USBRELAY_STANDARD_ENDPOINT_CHARS 320
#define USBRELAY_STANDARD_DEVICE_KEY_CHARS 512

typedef struct UsbRelayStandardPrinterInfo {
	wchar_t device_key[USBRELAY_STANDARD_DEVICE_KEY_CHARS];
	wchar_t name[USBRELAY_STANDARD_NAME_CHARS];
	wchar_t driver[USBRELAY_STANDARD_DRIVER_CHARS];
	wchar_t original_port[USBRELAY_STANDARD_PORT_NAME_CHARS];
	wchar_t endpoint[USBRELAY_STANDARD_ENDPOINT_CHARS];
	unsigned short raw_port;
	BOOL published;
	BOOL persistent;
	DWORD error;
} UsbRelayStandardPrinterInfo;

int usbrelay_standard_engine_run(void);
BOOL usbrelay_standard_engine_is_running(void);
BOOL usbrelay_standard_list_printers(UsbRelayStandardPrinterInfo *printers,
	DWORD printer_capacity, DWORD *printer_count);
BOOL usbrelay_standard_get_computer_name(wchar_t *name, size_t name_chars);

#endif
