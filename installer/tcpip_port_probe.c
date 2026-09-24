#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winspool.h>
#include <tcpxcv.h>

#include <stdio.h>
#include <wchar.h>

static void print_win32_error(const wchar_t *operation, DWORD error)
{
	wchar_t message[1024];
	DWORD length = FormatMessageW(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, error, 0, message, (DWORD)(sizeof(message) / sizeof(message[0])),
		NULL);

	while (length > 0 &&
		(message[length - 1] == L'\r' || message[length - 1] == L'\n' ||
			message[length - 1] == L' ')) {
		message[--length] = L'\0';
	}
	wprintf(L"%ls: %lu %ls\n", operation, error,
		length > 0 ? message : L"");
}

static int test_add_port(void)
{
	const wchar_t *monitor_name = L",XcvMonitor Standard TCP/IP Port";
	const wchar_t *port_name = L"USBRELAY_PROBE_127_9100";
	HANDLE monitor = NULL;
	PRINTER_DEFAULTS defaults;
	PORT_DATA_1 data;
	DWORD needed = 0;
	DWORD status = 0;
	BOOL result;

	ZeroMemory(&data, sizeof(data));
	wcsncpy_s(data.sztPortName, MAX_PORTNAME_LEN, port_name, _TRUNCATE);
	data.dwVersion = 1;
	data.dwProtocol = RAWTCP;
	data.cbSize = sizeof(data);
	wcsncpy_s(data.sztHostAddress, MAX_NETWORKNAME_LEN,
		L"127.0.0.1", _TRUNCATE);
	data.dwPortNumber = 9100;
	data.dwSNMPEnabled = FALSE;
	data.dwDoubleSpool = FALSE;

	ZeroMemory(&defaults, sizeof(defaults));
	defaults.DesiredAccess = SERVER_ACCESS_ADMINISTER;

	if (!OpenPrinterW((LPWSTR)monitor_name, &monitor, &defaults)) {
		print_win32_error(L"OpenPrinter", GetLastError());
		return 1;
	}

	result = XcvDataW(monitor, L"AddPort", (PBYTE)&data, sizeof(data),
		NULL, 0, &needed, &status);
	wprintf(L"AddPort returned %d, status %lu, needed %lu, last error %lu\n",
		result, status, needed, GetLastError());
	if (!result || status != ERROR_SUCCESS) {
		ClosePrinter(monitor);
		return 2;
	}

	if (!DeletePortW(NULL, (LPWSTR)L"127.0.0.1", (LPWSTR)port_name)) {
		print_win32_error(L"DeletePort", GetLastError());
		ClosePrinter(monitor);
		return 3;
	}

	ClosePrinter(monitor);
	wprintf(L"Probe succeeded and the test port was deleted.\n");
	return 0;
}

int wmain(void)
{
	return test_add_port();
}
