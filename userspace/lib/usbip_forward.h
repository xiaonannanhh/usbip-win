#pragma

#include <winsock2.h>
#include <windows.h>

typedef struct _usbip_forward_cancel {
	HANDLE	hEvent;
} usbip_forward_cancel_t;

BOOL usbip_forward_cancel_init(usbip_forward_cancel_t *cancel);
void usbip_forward_cancel_request(usbip_forward_cancel_t *cancel);
void usbip_forward_cancel_cleanup(usbip_forward_cancel_t *cancel);

void usbip_forward(HANDLE hdev_src, HANDLE hdev_dst, BOOL inbound,
	usbip_forward_cancel_t *cancel);
