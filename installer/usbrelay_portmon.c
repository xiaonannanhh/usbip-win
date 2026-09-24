#define WIN32_LEAN_AND_MEAN

#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x06010000
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winspool.h>
#include <winsplp.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "../include/usbrelay_protocol.h"

#define PORT_PREFIX_LENGTH 9
#define PORT_MAX_LENGTH 512
#define HOST_MAX_LENGTH 253
#define MAX_CHUNK (16 * 1024 * 1024)
#define SOCKET_TIMEOUT_MS 120000
#define PORT_REGISTRY_KEY L"SYSTEM\\CurrentControlSet\\Control\\Print\\Monitors\\USBRelay Port Monitor\\Ports"

typedef struct relay_port {
	wchar_t host[HOST_MAX_LENGTH + 1];
	DWORD printer_id;
	SOCKET socket_handle;
	BOOL document_started;
	BOOL failed;
} relay_port;

static LONG g_winsock_users;
static MONITOR2 g_monitor;

#ifdef _WIN64
#pragma comment(linker, "/export:InitializePrintMonitor2")
#else
#pragma comment(linker, "/export:InitializePrintMonitor2=_InitializePrintMonitor2@8")
#endif

static void set_socket_timeout(SOCKET socket_handle, DWORD milliseconds)
{
	int timeout = (int)milliseconds;

	setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
		(const char *)&timeout, sizeof(timeout));
	setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
		(const char *)&timeout, sizeof(timeout));
}

static BOOL send_all(SOCKET socket_handle, const void *data, int length)
{
	const char *cursor = (const char *)data;

	while (length > 0) {
		int sent = send(socket_handle, cursor, length, 0);

		if (sent <= 0)
			return FALSE;
		cursor += sent;
		length -= sent;
	}
	return TRUE;
}

static BOOL recv_line(SOCKET socket_handle, char *buffer, int buffer_size)
{
	int length = 0;

	if (buffer == NULL || buffer_size < 2)
		return FALSE;
	while (length < buffer_size - 1) {
		char character;
		int received = recv(socket_handle, &character, 1, 0);

		if (received <= 0)
			return FALSE;
		if (character == '\n') {
			buffer[length] = '\0';
			return TRUE;
		}
		if (character != '\r')
			buffer[length++] = character;
	}
	WSASetLastError(WSAEMSGSIZE);
	return FALSE;
}

static BOOL parse_port_name(LPCWSTR port_name, relay_port *port)
{
	const wchar_t *host_start;
	const wchar_t *separator;
	wchar_t *end;
	unsigned long id;
	size_t host_length;

	if (port_name == NULL || port == NULL ||
		_wcsnicmp(port_name, USBRELAY_PORT_PREFIX, PORT_PREFIX_LENGTH) != 0) {
		SetLastError(ERROR_INVALID_NAME);
		return FALSE;
	}
	host_start = port_name + PORT_PREFIX_LENGTH;
	separator = wcsrchr(host_start, L':');
	if (separator == NULL || separator == host_start || separator[1] == L'\0') {
		SetLastError(ERROR_INVALID_NAME);
		return FALSE;
	}
	host_length = (size_t)(separator - host_start);
	if (host_length > HOST_MAX_LENGTH) {
		SetLastError(ERROR_INVALID_NAME);
		return FALSE;
	}
	ZeroMemory(port, sizeof(*port));
	if (wcsncpy_s(port->host, sizeof(port->host) / sizeof(port->host[0]), host_start,
		host_length) != 0) {
		SetLastError(ERROR_INVALID_NAME);
		return FALSE;
	}
	id = wcstoul(separator + 1, &end, 16);
	if (end == separator + 1 || *end != L'\0' || id > 0xffffffffUL) {
		SetLastError(ERROR_INVALID_NAME);
		return FALSE;
	}
	port->printer_id = (DWORD)id;
	port->socket_handle = INVALID_SOCKET;
	return TRUE;
}

static BOOL connect_to_server(relay_port *port)
{
	struct addrinfoW hints;
	struct addrinfoW *addresses = NULL;
	struct addrinfoW *address;
	int result;

	ZeroMemory(&hints, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	result = GetAddrInfoW(port->host, L"3242", &hints, &addresses);
	if (result != 0) {
		SetLastError(ERROR_HOST_UNREACHABLE);
		return FALSE;
	}
	for (address = addresses; address != NULL; address = address->ai_next) {
		SOCKET socket_handle = socket(address->ai_family,
			address->ai_socktype, address->ai_protocol);

		if (socket_handle == INVALID_SOCKET)
			continue;
		set_socket_timeout(socket_handle, SOCKET_TIMEOUT_MS);
		if (connect(socket_handle, address->ai_addr,
			(int)address->ai_addrlen) == 0) {
			port->socket_handle = socket_handle;
			break;
		}
		closesocket(socket_handle);
	}
	FreeAddrInfoW(addresses);
	if (port->socket_handle == INVALID_SOCKET) {
		SetLastError(ERROR_HOST_UNREACHABLE);
		return FALSE;
	}
	return TRUE;
}

static BOOL add_port_to_registry(LPCWSTR port_name)
{
	HKEY key;
	LONG result;
	const wchar_t description[] = L"USBRelay network printer";

	result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, PORT_REGISTRY_KEY, 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL);
	if (result != ERROR_SUCCESS) {
		SetLastError((DWORD)result);
		return FALSE;
	}
	result = RegSetValueExW(key, port_name, 0, REG_SZ,
		(const BYTE *)description, (DWORD)(sizeof(description)));
	RegCloseKey(key);
	if (result != ERROR_SUCCESS) {
		SetLastError((DWORD)result);
		return FALSE;
	}
	return TRUE;
}

static BOOL remove_port_from_registry(LPCWSTR port_name)
{
	HKEY key;
	LONG result;

	result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, PORT_REGISTRY_KEY, 0,
		KEY_SET_VALUE, &key);
	if (result == ERROR_FILE_NOT_FOUND)
		return TRUE;
	if (result != ERROR_SUCCESS) {
		SetLastError((DWORD)result);
		return FALSE;
	}
	result = RegDeleteValueW(key, port_name);
	RegCloseKey(key);
	if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
		SetLastError((DWORD)result);
		return FALSE;
	}
	return TRUE;
}

static DWORD get_registered_ports(wchar_t names[][PORT_MAX_LENGTH],
	DWORD maximum)
{
	HKEY key;
	DWORD count = 0;
	DWORD index;
	LONG result;

	result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, PORT_REGISTRY_KEY, 0,
		KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE, &key);
	if (result != ERROR_SUCCESS)
		return 0;
	for (index = 0; index < maximum; index++) {
		DWORD name_length = PORT_MAX_LENGTH - 1;

		result = RegEnumValueW(key, index, names[count], &name_length,
			NULL, NULL, NULL, NULL);
		if (result == ERROR_NO_MORE_ITEMS)
			break;
		if (result != ERROR_SUCCESS)
			continue;
		if (count < maximum)
			count++;
	}
	RegCloseKey(key);
	return count;
}

static BOOL WINAPI monitor_enum_ports(HANDLE monitor, LPWSTR name,
	DWORD level, LPBYTE ports, DWORD buffer_size, LPDWORD needed,
	LPDWORD returned)
{
	wchar_t names[128][PORT_MAX_LENGTH];
	DWORD count;
	DWORD required;
	DWORD index;
	BYTE *string_cursor;
	DWORD string_bytes;
	PORT_INFO_1W *ports1;
	PORT_INFO_2W *ports2;

	UNREFERENCED_PARAMETER(monitor);
	UNREFERENCED_PARAMETER(name);
	if (needed == NULL || returned == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*needed = 0;
	*returned = 0;
	if (level != 1 && level != 2) {
		SetLastError(ERROR_INVALID_LEVEL);
		return FALSE;
	}
	count = get_registered_ports(names, sizeof(names) / sizeof(names[0]));
	required = count * (level == 1 ? sizeof(PORT_INFO_1W) :
		sizeof(PORT_INFO_2W));
	for (index = 0; index < count; index++) {
		required += (DWORD)((wcslen(names[index]) + 1) * sizeof(wchar_t));
		if (level == 2) {
			required += (DWORD)((wcslen(USBRELAY_PORT_MONITOR_NAME) + 1) *
				sizeof(wchar_t));
			required += (DWORD)((wcslen(L"USBRelay network printer") + 1) *
				sizeof(wchar_t));
		}
	}
	*needed = required;
	if (count == 0) {
		SetLastError(ERROR_SUCCESS);
		return TRUE;
	}
	if (ports == NULL) {
		*returned = 0;
		SetLastError(ERROR_SUCCESS);
		return TRUE;
	}
	if (buffer_size < required) {
		*returned = 0;
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}

	string_bytes = required - count * (level == 1 ? sizeof(PORT_INFO_1W) :
		sizeof(PORT_INFO_2W));
	/*
	 * localspl passes the complete EnumPorts buffer to each monitor and
	 * expects the string block at the end of that buffer.  Packing strings
	 * directly after this monitor's structures makes localspl overwrite the
	 * first entry while it merges the other port monitors.
	 */
	string_cursor = ports + buffer_size - string_bytes;
	ports1 = (PORT_INFO_1W *)ports;
	ports2 = (PORT_INFO_2W *)ports;
	for (index = 0; index < count; index++) {
		DWORD bytes = (DWORD)((wcslen(names[index]) + 1) * sizeof(wchar_t));

		if (level == 1) {
			ports1[index].pName = (LPWSTR)string_cursor;
			CopyMemory(string_cursor, names[index], bytes);
			string_cursor += bytes;
		}
		else {
			ports2[index].pPortName = (LPWSTR)string_cursor;
			CopyMemory(string_cursor, names[index], bytes);
			string_cursor += bytes;
			bytes = (DWORD)((wcslen(USBRELAY_PORT_MONITOR_NAME) + 1) *
				sizeof(wchar_t));
			ports2[index].pMonitorName = (LPWSTR)string_cursor;
			CopyMemory(string_cursor, USBRELAY_PORT_MONITOR_NAME, bytes);
			string_cursor += bytes;
			bytes = (DWORD)((wcslen(L"USBRelay network printer") + 1) *
				sizeof(wchar_t));
			ports2[index].pDescription = (LPWSTR)string_cursor;
			CopyMemory(string_cursor, L"USBRelay network printer", bytes);
			string_cursor += bytes;
			ports2[index].fPortType = PORT_TYPE_WRITE | PORT_TYPE_READ;
			ports2[index].Reserved = 0;
		}
	}
	*returned = count;
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

static BOOL WINAPI monitor_open_port_ex(HANDLE monitor, HANDLE monitor_port,
	LPWSTR port_name, LPWSTR printer_name, PHANDLE handle,
	PMONITOR2 monitor2)
{
	relay_port *port;

	UNREFERENCED_PARAMETER(monitor);
	UNREFERENCED_PARAMETER(monitor_port);
	UNREFERENCED_PARAMETER(printer_name);
	UNREFERENCED_PARAMETER(monitor2);
	if (handle == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	port = (relay_port *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		sizeof(*port));
	if (port == NULL) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	if (!parse_port_name(port_name, port)) {
		HeapFree(GetProcessHeap(), 0, port);
		return FALSE;
	}
	*handle = (HANDLE)port;
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

static BOOL WINAPI monitor_open_port(HANDLE monitor, LPWSTR name,
	PHANDLE handle)
{
	return monitor_open_port_ex(monitor, NULL, name, NULL, handle, NULL);
}

static BOOL WINAPI monitor_start_doc(HANDLE handle, LPWSTR printer_name,
	DWORD job_id, DWORD level, LPBYTE doc_info)
{
	relay_port *port = (relay_port *)handle;
	char command[128];

	UNREFERENCED_PARAMETER(printer_name);
	UNREFERENCED_PARAMETER(job_id);
	UNREFERENCED_PARAMETER(level);
	UNREFERENCED_PARAMETER(doc_info);
	if (port == NULL || port->socket_handle != INVALID_SOCKET) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!connect_to_server(port))
		return FALSE;
	_snprintf_s(command, sizeof(command), _TRUNCATE,
		USBRELAY_PRINT_MAGIC " PRINT %08lX CHUNKED\r\n",
		(unsigned long)port->printer_id);
	if (!send_all(port->socket_handle, command, (int)strlen(command))) {
		closesocket(port->socket_handle);
		port->socket_handle = INVALID_SOCKET;
		SetLastError(ERROR_NETNAME_DELETED);
		return FALSE;
	}
	port->document_started = TRUE;
	port->failed = FALSE;
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

static BOOL WINAPI monitor_write_port(HANDLE handle, LPBYTE buffer,
	DWORD buffer_size, LPDWORD written)
{
	relay_port *port = (relay_port *)handle;
	DWORD sent = 0;

	if (written != NULL)
		*written = 0;
	if (port == NULL || !port->document_started ||
		port->socket_handle == INVALID_SOCKET) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	while (sent < buffer_size) {
		BYTE length[4];
		DWORD chunk = buffer_size - sent;

		if (chunk > MAX_CHUNK)
			chunk = MAX_CHUNK;
		length[0] = (BYTE)(chunk >> 24);
		length[1] = (BYTE)(chunk >> 16);
		length[2] = (BYTE)(chunk >> 8);
		length[3] = (BYTE)chunk;
		if (!send_all(port->socket_handle, length, sizeof(length)) ||
			!send_all(port->socket_handle, buffer + sent, (int)chunk)) {
			port->failed = TRUE;
			SetLastError(ERROR_NETNAME_DELETED);
			return FALSE;
		}
		sent += chunk;
	}
	if (written != NULL)
		*written = sent;
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

static BOOL WINAPI monitor_end_doc(HANDLE handle)
{
	relay_port *port = (relay_port *)handle;
	BYTE length[4] = { 0, 0, 0, 0 };
	char response[256];
	BOOL success = TRUE;

	if (port == NULL || !port->document_started ||
		port->socket_handle == INVALID_SOCKET) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!port->failed &&
		(!send_all(port->socket_handle, length, sizeof(length)) ||
			!recv_line(port->socket_handle, response, sizeof(response)))) {
		port->failed = TRUE;
		SetLastError(ERROR_NETNAME_DELETED);
	}
	if (!port->failed && strcmp(response, "OK") != 0) {
		port->failed = TRUE;
		SetLastError(ERROR_PRINT_CANCELLED);
	}
	if (port->failed)
		success = FALSE;
	closesocket(port->socket_handle);
	port->socket_handle = INVALID_SOCKET;
	port->document_started = FALSE;
	if (!success && GetLastError() == ERROR_SUCCESS)
		SetLastError(ERROR_NETNAME_DELETED);
	else if (success)
		SetLastError(ERROR_SUCCESS);
	return success;
}

static BOOL WINAPI monitor_close_port(HANDLE handle)
{
	relay_port *port = (relay_port *)handle;

	if (port == NULL) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (port->socket_handle != INVALID_SOCKET) {
		shutdown(port->socket_handle, SD_BOTH);
		closesocket(port->socket_handle);
	}
	HeapFree(GetProcessHeap(), 0, port);
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

static BOOL WINAPI monitor_add_port(HANDLE monitor, LPWSTR name, HWND window,
	LPWSTR monitor_name)
{
	UNREFERENCED_PARAMETER(monitor);
	UNREFERENCED_PARAMETER(window);
	UNREFERENCED_PARAMETER(monitor_name);
	if (!add_port_to_registry(name))
		return FALSE;
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

static BOOL WINAPI monitor_add_port_ex(HANDLE monitor, LPWSTR name,
	DWORD level, LPBYTE buffer, LPWSTR monitor_name)
{
	PORT_INFO_1W *info;

	UNREFERENCED_PARAMETER(monitor);
	UNREFERENCED_PARAMETER(name);
	UNREFERENCED_PARAMETER(monitor_name);
	if (level != 1 || buffer == NULL) {
		SetLastError(ERROR_INVALID_LEVEL);
		return FALSE;
	}
	info = (PORT_INFO_1W *)buffer;
	if (info->pName == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!add_port_to_registry(info->pName))
		return FALSE;
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

static BOOL WINAPI monitor_configure_port(HANDLE monitor, LPWSTR name,
	HWND window, LPWSTR port_name)
{
	UNREFERENCED_PARAMETER(monitor);
	UNREFERENCED_PARAMETER(name);
	UNREFERENCED_PARAMETER(window);
	UNREFERENCED_PARAMETER(port_name);
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

static BOOL WINAPI monitor_delete_port(HANDLE monitor, LPWSTR name,
	HWND window, LPWSTR port_name)
{
	UNREFERENCED_PARAMETER(monitor);
	UNREFERENCED_PARAMETER(name);
	UNREFERENCED_PARAMETER(window);
	if (!remove_port_from_registry(port_name))
		return FALSE;
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

static BOOL WINAPI monitor_read_port(HANDLE handle, LPBYTE buffer,
	DWORD buffer_size, LPDWORD read)
{
	UNREFERENCED_PARAMETER(handle);
	UNREFERENCED_PARAMETER(buffer);
	UNREFERENCED_PARAMETER(buffer_size);
	if (read != NULL)
		*read = 0;
	SetLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
}

static BOOL WINAPI monitor_set_port_timeouts(HANDLE handle,
	LPCOMMTIMEOUTS timeouts, DWORD reserved)
{
	UNREFERENCED_PARAMETER(handle);
	UNREFERENCED_PARAMETER(timeouts);
	UNREFERENCED_PARAMETER(reserved);
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

static BOOL WINAPI monitor_get_printer_data_from_port(HANDLE handle,
	DWORD control_id, LPWSTR value_name, LPWSTR input, DWORD input_size,
	LPWSTR output, DWORD output_size, LPDWORD returned)
{
	UNREFERENCED_PARAMETER(handle);
	UNREFERENCED_PARAMETER(control_id);
	UNREFERENCED_PARAMETER(value_name);
	UNREFERENCED_PARAMETER(input);
	UNREFERENCED_PARAMETER(input_size);
	UNREFERENCED_PARAMETER(output);
	UNREFERENCED_PARAMETER(output_size);
	if (returned != NULL)
		*returned = 0;
	SetLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
}

static BOOL WINAPI monitor_xcv_open(HANDLE monitor, LPCWSTR object,
	ACCESS_MASK access, PHANDLE xcv)
{
	UNREFERENCED_PARAMETER(monitor);
	UNREFERENCED_PARAMETER(object);
	UNREFERENCED_PARAMETER(access);
	if (xcv == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*xcv = (HANDLE)(ULONG_PTR)1;
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

static DWORD WINAPI monitor_xcv_data(HANDLE xcv, LPCWSTR data_name,
	PBYTE input, DWORD input_size, PBYTE output, DWORD output_size,
	PDWORD output_needed)
{
	BOOL success = FALSE;
	DWORD status = ERROR_INVALID_PARAMETER;

	UNREFERENCED_PARAMETER(xcv);
	if (output_needed != NULL)
		*output_needed = sizeof(status);
	if (input == NULL || input_size < sizeof(wchar_t) ||
		input_size % sizeof(wchar_t) != 0 || data_name == NULL)
		return status;
	if (!_wcsicmp(data_name, L"AddPort"))
		success = add_port_to_registry((LPCWSTR)input);
	else if (!_wcsicmp(data_name, L"DeletePort"))
		success = remove_port_from_registry((LPCWSTR)input);
	else
		status = ERROR_NOT_SUPPORTED;
	if (success)
		status = ERROR_SUCCESS;
	else if (status == ERROR_INVALID_PARAMETER)
		status = GetLastError();
	if (output != NULL && output_size >= sizeof(status))
		CopyMemory(output, &status, sizeof(status));
	return status;
}

static BOOL WINAPI monitor_xcv_close(HANDLE xcv)
{
	UNREFERENCED_PARAMETER(xcv);
	SetLastError(ERROR_SUCCESS);
	return TRUE;
}

static VOID WINAPI monitor_shutdown(HANDLE monitor)
{
	UNREFERENCED_PARAMETER(monitor);
	if (InterlockedDecrement(&g_winsock_users) == 0)
		WSACleanup();
}

static void initialize_monitor_table(void)
{
	ZeroMemory(&g_monitor, sizeof(g_monitor));
	g_monitor.cbSize = sizeof(g_monitor);
	g_monitor.pfnEnumPorts = monitor_enum_ports;
	g_monitor.pfnOpenPort = monitor_open_port;
	g_monitor.pfnOpenPortEx = monitor_open_port_ex;
	g_monitor.pfnStartDocPort = monitor_start_doc;
	g_monitor.pfnWritePort = monitor_write_port;
	g_monitor.pfnReadPort = monitor_read_port;
	g_monitor.pfnEndDocPort = monitor_end_doc;
	g_monitor.pfnClosePort = monitor_close_port;
	g_monitor.pfnAddPort = monitor_add_port;
	g_monitor.pfnAddPortEx = monitor_add_port_ex;
	g_monitor.pfnConfigurePort = monitor_configure_port;
	g_monitor.pfnDeletePort = monitor_delete_port;
	g_monitor.pfnGetPrinterDataFromPort = monitor_get_printer_data_from_port;
	g_monitor.pfnSetPortTimeOuts = monitor_set_port_timeouts;
	g_monitor.pfnXcvOpenPort = monitor_xcv_open;
	g_monitor.pfnXcvDataPort = monitor_xcv_data;
	g_monitor.pfnXcvClosePort = monitor_xcv_close;
	g_monitor.pfnShutdown = monitor_shutdown;
}

LPMONITOR2 WINAPI InitializePrintMonitor2(PMONITORINIT monitor_init,
	PHANDLE monitor)
{
	WSADATA data;
	LONG users;

	UNREFERENCED_PARAMETER(monitor_init);
	if (monitor == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	users = InterlockedIncrement(&g_winsock_users);
	if (users == 1 && WSAStartup(MAKEWORD(2, 2), &data) != 0) {
		InterlockedDecrement(&g_winsock_users);
		SetLastError(WSANOTINITIALISED);
		return NULL;
	}
	initialize_monitor_table();
	*monitor = (HANDLE)(ULONG_PTR)1;
	return &g_monitor;
}
