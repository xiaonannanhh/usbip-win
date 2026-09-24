#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <winspool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../../../include/usbrelay_protocol.h"

#define CORE_MUTEX_NAME L"Local\\USBRelay-Server-Core"
#define STOP_EVENT_NAME L"Local\\USBRelay-Server-Stop"
#define MAX_LINE 4096
#define MAX_CHUNK (16 * 1024 * 1024)
#define DISCOVERY_INTERVAL_MS 2000
#define SOCKET_TIMEOUT_MS 120000
#define MAX_PRINTERS 1024

typedef struct relay_client {
	struct relay_client *next;
	SOCKET socket_handle;
} relay_client;

static CRITICAL_SECTION g_client_lock;
static relay_client *g_clients;
static HANDLE g_done_event;
static HANDLE g_accept_thread;
static SOCKET g_listen_socket = INVALID_SOCKET;
static SOCKET g_discovery_socket = INVALID_SOCKET;
static HANDLE g_stop_event;
static HANDLE g_core_mutex;
static volatile LONG g_active;
static volatile LONG g_stopping;
static BOOL g_initialized;

static void active_changed(void)
{
	if (InterlockedCompareExchange(&g_active, 0, 0) == 0 &&
		g_done_event != NULL) {
		SetEvent(g_done_event);
	}
}

static void log_message(const char *prefix, const char *message)
{
	fprintf(stderr, "[%s] %s\n", prefix, message);
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

static void set_socket_timeout(SOCKET socket_handle, DWORD milliseconds)
{
	int timeout = (int)milliseconds;

	setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
		(const char *)&timeout, sizeof(timeout));
	setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
		(const char *)&timeout, sizeof(timeout));
}

static BOOL recv_exact(SOCKET socket_handle, void *data, int length)
{
	char *cursor = (char *)data;

	while (length > 0) {
		int received = recv(socket_handle, cursor, length, 0);

		if (received <= 0)
			return FALSE;
		cursor += received;
		length -= received;
	}
	return TRUE;
}

static int recv_line(SOCKET socket_handle, char *buffer, int buffer_size)
{
	int length = 0;

	if (buffer == NULL || buffer_size < 2)
		return -1;
	while (length < buffer_size - 1) {
		char character;
		int received = recv(socket_handle, &character, 1, 0);

		if (received <= 0)
			return received == 0 && length != 0 ? length : -1;
		if (character == '\n') {
			buffer[length] = '\0';
			return length;
		}
		if (character != '\r')
			buffer[length++] = character;
	}
	buffer[buffer_size - 1] = '\0';
	return -1;
}

static BOOL send_line(SOCKET socket_handle, const char *line)
{
	return send_all(socket_handle, line, (int)strlen(line)) &&
		send_all(socket_handle, "\r\n", 2);
}

static BOOL wide_to_utf8(const wchar_t *source, char *destination,
	int destination_size)
{
	int result;

	if (destination == NULL || destination_size <= 0)
		return FALSE;
	if (source == NULL)
		source = L"";
	result = WideCharToMultiByte(CP_UTF8, 0, source, -1, destination,
		destination_size, NULL, NULL);
	if (result <= 0) {
		destination[0] = '\0';
		return FALSE;
	}
	destination[destination_size - 1] = '\0';
	return TRUE;
}

static void sanitize_protocol_text(char *text)
{
	char *cursor;

	if (text == NULL)
		return;
	for (cursor = text; *cursor != '\0'; cursor++) {
		if (*cursor == '\r' || *cursor == '\n')
			*cursor = ' ';
	}
}

static DWORD printer_id(const wchar_t *printer_name)
{
	DWORD hash = 2166136261u;

	while (printer_name != NULL && *printer_name != L'\0') {
		wchar_t character = *printer_name++;

		if (character >= L'A' && character <= L'Z')
			character = (wchar_t)(character - L'A' + L'a');
		hash ^= (DWORD)(character & 0xff);
		hash *= 16777619u;
		hash ^= (DWORD)((character >> 8) & 0xff);
		hash *= 16777619u;
	}
	return hash;
}

static BOOL enum_local_printers(BYTE **buffer, DWORD *returned)
{
	DWORD needed = 0;
	DWORD count = 0;
	BYTE *data;

	if (buffer == NULL || returned == NULL)
		return FALSE;
	*buffer = NULL;
	*returned = 0;
	SetLastError(ERROR_SUCCESS);
	if (!EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, NULL, 0, &needed,
		&count) && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return FALSE;
	if (needed == 0)
		return TRUE;
	data = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
	if (data == NULL) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	if (!EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, data, needed,
		&needed, &count)) {
		DWORD error = GetLastError();
		HeapFree(GetProcessHeap(), 0, data);
		SetLastError(error);
		return FALSE;
	}
	*buffer = data;
	*returned = count;
	return TRUE;
}

static BOOL find_printer(DWORD id, wchar_t *name, size_t name_count)
{
	BYTE *buffer = NULL;
	DWORD count = 0;
	DWORD index;
	BOOL found = FALSE;

	if (name == NULL || name_count == 0)
		return FALSE;
	name[0] = L'\0';
	if (!enum_local_printers(&buffer, &count))
		return FALSE;
	for (index = 0; index < count; index++) {
		PRINTER_INFO_2W *info = &((PRINTER_INFO_2W *)buffer)[index];

		if (info->pPrinterName != NULL &&
			printer_id(info->pPrinterName) == id) {
			wcsncpy_s(name, name_count, info->pPrinterName, _TRUNCATE);
			found = TRUE;
			break;
		}
	}
	if (buffer != NULL)
		HeapFree(GetProcessHeap(), 0, buffer);
	return found;
}

static BOOL send_printer_list(SOCKET socket_handle)
{
	BYTE *buffer = NULL;
	DWORD count = 0;
	DWORD valid_count = 0;
	DWORD index;
	char line[MAX_LINE];
	char name[1024];
	char driver[1024];
	char port[1024];

	if (!enum_local_printers(&buffer, &count)) {
		_snprintf_s(line, sizeof(line), _TRUNCATE, "ERR spooler %lu",
			(unsigned long)GetLastError());
		send_line(socket_handle, line);
		return FALSE;
	}
	if (count > MAX_PRINTERS)
		count = MAX_PRINTERS;
	for (index = 0; index < count; index++) {
		PRINTER_INFO_2W *info = &((PRINTER_INFO_2W *)buffer)[index];

		if (info->pPrinterName != NULL && info->pPrinterName[0] != L'\0')
			valid_count++;
	}
	if (!send_line(socket_handle, USBRELAY_PRINT_MAGIC " LIST-OK"))
		goto fail;
	if (_snprintf_s(line, sizeof(line), _TRUNCATE, "count=%lu",
		(unsigned long)valid_count) < 0 || !send_line(socket_handle, line))
		goto fail;

	for (index = 0; index < count; index++) {
		PRINTER_INFO_2W *info = &((PRINTER_INFO_2W *)buffer)[index];

		if (info->pPrinterName == NULL || info->pPrinterName[0] == L'\0')
			continue;
		if (!wide_to_utf8(info->pPrinterName, name, sizeof(name)) ||
			!wide_to_utf8(info->pDriverName, driver, sizeof(driver)) ||
			!wide_to_utf8(info->pPortName, port, sizeof(port)))
			goto fail;
		sanitize_protocol_text(name);
		sanitize_protocol_text(driver);
		sanitize_protocol_text(port);
		if (_snprintf_s(line, sizeof(line), _TRUNCATE,
			"printer=%08lX", (unsigned long)printer_id(info->pPrinterName)) < 0 ||
			!send_line(socket_handle, line) ||
			_snprintf_s(line, sizeof(line), _TRUNCATE, "name=%s", name) < 0 ||
			!send_line(socket_handle, line) ||
			_snprintf_s(line, sizeof(line), _TRUNCATE, "driver=%s", driver) < 0 ||
			!send_line(socket_handle, line) ||
			_snprintf_s(line, sizeof(line), _TRUNCATE, "port=%s", port) < 0 ||
			!send_line(socket_handle, line) ||
			!send_line(socket_handle, "") )
			goto fail;
	}
	HeapFree(GetProcessHeap(), 0, buffer);
	return TRUE;

fail:
	if (buffer != NULL)
		HeapFree(GetProcessHeap(), 0, buffer);
	return FALSE;
}

static BOOL start_spooler_job(const wchar_t *name, HANDLE *printer)
{
	DOC_INFO_1W doc;

	*printer = NULL;
	if (!OpenPrinterW((LPWSTR)name, printer, NULL))
		return FALSE;
	ZeroMemory(&doc, sizeof(doc));
	doc.pDocName = L"USBRelay Remote Print Job";
	doc.pDatatype = L"RAW";
	if (!StartDocPrinterW(*printer, 1, (LPBYTE)&doc) ||
		!StartPagePrinter(*printer)) {
		DWORD error = GetLastError();
		ClosePrinter(*printer);
		*printer = NULL;
		SetLastError(error);
		return FALSE;
	}
	return TRUE;
}

static BOOL receive_job(SOCKET socket_handle, HANDLE printer, DWORD *error)
{
	BYTE *buffer = NULL;
	DWORD buffer_size = 0;
	BOOL complete = FALSE;

	if (error == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*error = ERROR_SUCCESS;

	for (;;) {
		BYTE length_bytes[4];
		DWORD length;
		DWORD offset = 0;

		if (!recv_exact(socket_handle, length_bytes, sizeof(length_bytes))) {
			int network_error = WSAGetLastError();
			*error = network_error == WSAETIMEDOUT ? ERROR_TIMEOUT :
				network_error == 0 ? ERROR_CONNECTION_ABORTED :
				ERROR_CONNECTION_ABORTED;
			break;
		}
		length = ((DWORD)length_bytes[0] << 24) |
			((DWORD)length_bytes[1] << 16) |
			((DWORD)length_bytes[2] << 8) |
			(DWORD)length_bytes[3];
		if (length == 0) {
			complete = TRUE;
			break;
		}
		if (length > MAX_CHUNK) {
			*error = ERROR_INVALID_DATA;
			break;
		}
		if (length > buffer_size) {
			BYTE *new_buffer;

			if (buffer == NULL)
				new_buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, length);
			else
				new_buffer = (BYTE *)HeapReAlloc(GetProcessHeap(), 0,
					buffer, length);
			if (new_buffer == NULL) {
				*error = ERROR_NOT_ENOUGH_MEMORY;
				break;
			}
			buffer = new_buffer;
			buffer_size = length;
		}
		if (!recv_exact(socket_handle, buffer, (int)length)) {
			*error = ERROR_CONNECTION_ABORTED;
			break;
		}
		while (offset < length) {
			DWORD written = 0;

			if (!WritePrinter(printer, buffer + offset, length - offset,
				&written) || written == 0) {
				*error = GetLastError();
				if (*error == ERROR_SUCCESS)
					*error = ERROR_WRITE_FAULT;
				goto done;
			}
			offset += written;
		}
	}

done:
	if (buffer != NULL)
		HeapFree(GetProcessHeap(), 0, buffer);
	return complete;
}

static void handle_print(SOCKET socket_handle, const char *command)
{
	unsigned long parsed_id;
	wchar_t printer_name[1024];
	HANDLE printer = NULL;
	DWORD error = ERROR_SUCCESS;
	char response[128];

	if (sscanf_s(command, USBRELAY_PRINT_MAGIC " PRINT %lx CHUNKED",
		&parsed_id) != 1) {
		send_line(socket_handle, "ERR protocol expected PRINT <id> CHUNKED");
		return;
	}
	if (!find_printer((DWORD)parsed_id, printer_name,
		_countof(printer_name))) {
		send_line(socket_handle, "ERR printer not found");
		return;
	}
	if (!start_spooler_job(printer_name, &printer)) {
		_snprintf_s(response, sizeof(response), _TRUNCATE,
			"ERR spooler %lu", (unsigned long)GetLastError());
		send_line(socket_handle, response);
		return;
	}
	if (receive_job(socket_handle, printer, &error)) {
		if (!EndPagePrinter(printer) || !EndDocPrinter(printer)) {
			error = GetLastError();
			AbortPrinter(printer);
		}
	}
	else {
		AbortPrinter(printer);
	}
	ClosePrinter(printer);
	if (error == ERROR_SUCCESS)
		send_line(socket_handle, "OK");
	else {
		_snprintf_s(response, sizeof(response), _TRUNCATE,
			"ERR spooler %lu", (unsigned long)error);
		send_line(socket_handle, response);
	}
}

static void remove_client(relay_client *client)
{
	relay_client **cursor;

	EnterCriticalSection(&g_client_lock);
	cursor = &g_clients;
	while (*cursor != NULL) {
		if (*cursor == client) {
			*cursor = client->next;
			InterlockedDecrement(&g_active);
			break;
		}
		cursor = &(*cursor)->next;
	}
	LeaveCriticalSection(&g_client_lock);
	active_changed();
}

static DWORD WINAPI client_thread(LPVOID parameter)
{
	relay_client *client = (relay_client *)parameter;
	char line[MAX_LINE];

	for (;;) {
		int length = recv_line(client->socket_handle, line, sizeof(line));

		if (length < 0)
			break;
		if (strcmp(line, USBRELAY_PRINT_MAGIC " LIST") == 0) {
			if (!send_printer_list(client->socket_handle))
				break;
			continue;
		}
		if (strncmp(line, USBRELAY_PRINT_MAGIC " PRINT ",
			strlen(USBRELAY_PRINT_MAGIC " PRINT ")) == 0) {
			handle_print(client->socket_handle, line);
			break;
		}
		send_line(client->socket_handle, "ERR protocol unsupported");
		break;
	}
	shutdown(client->socket_handle, SD_BOTH);
	closesocket(client->socket_handle);
	remove_client(client);
	free(client);
	return 0;
}

static DWORD WINAPI accept_thread(LPVOID parameter)
{
	UNREFERENCED_PARAMETER(parameter);

	while (!g_stopping) {
		SOCKET socket_handle = accept(g_listen_socket, NULL, NULL);
		relay_client *client;
		HANDLE thread;

		if (socket_handle == INVALID_SOCKET)
			break;
		set_socket_timeout(socket_handle, SOCKET_TIMEOUT_MS);
		if (g_stopping) {
			closesocket(socket_handle);
			break;
		}
		client = (relay_client *)calloc(1, sizeof(*client));
		if (client == NULL) {
			closesocket(socket_handle);
			continue;
		}
		client->socket_handle = socket_handle;
		EnterCriticalSection(&g_client_lock);
		if (g_stopping) {
			LeaveCriticalSection(&g_client_lock);
			closesocket(socket_handle);
			free(client);
			break;
		}
		client->next = g_clients;
		g_clients = client;
		InterlockedIncrement(&g_active);
		LeaveCriticalSection(&g_client_lock);
		thread = CreateThread(NULL, 0, client_thread, client, 0, NULL);
		if (thread == NULL) {
			shutdown(socket_handle, SD_BOTH);
			closesocket(socket_handle);
			remove_client(client);
			free(client);
			continue;
		}
		CloseHandle(thread);
	}
	InterlockedDecrement(&g_active);
	active_changed();
	return 0;
}

static BOOL make_utf8_computer_name(char *buffer, int buffer_size)
{
	wchar_t name[256];
	DWORD length = _countof(name);

	if (!GetComputerNameW(name, &length))
		return strcpy_s(buffer, buffer_size, "USBRelay-Server") == 0;
	return wide_to_utf8(name, buffer, buffer_size);
}

static BOOL send_discovery_packet(const char *packet, int length,
	const struct sockaddr_in *destination)
{
	int sent = sendto(g_discovery_socket, packet, length, 0,
		(const SOCKADDR *)destination, sizeof(*destination));

	return sent != SOCKET_ERROR;
}

static void send_discovery_to_interfaces(const char *packet, int length)
{
	ULONG buffer_size = 16 * 1024;
	PIP_ADAPTER_ADDRESSES addresses = NULL;
	PIP_ADAPTER_ADDRESSES adapter;
	ULONG result;
	int attempt;

	for (attempt = 0; attempt < 3; attempt++) {
		addresses = (PIP_ADAPTER_ADDRESSES)HeapAlloc(
			GetProcessHeap(), HEAP_ZERO_MEMORY, buffer_size);
		if (addresses == NULL)
			return;
		result = GetAdaptersAddresses(AF_INET,
			GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
			GAA_FLAG_SKIP_DNS_SERVER, NULL, addresses, &buffer_size);
		if (result == ERROR_SUCCESS)
			break;
		HeapFree(GetProcessHeap(), 0, addresses);
		addresses = NULL;
		if (result != ERROR_BUFFER_OVERFLOW)
			return;
	}
	if (addresses == NULL)
		return;

	for (adapter = addresses; adapter != NULL; adapter = adapter->Next) {
		PIP_ADAPTER_UNICAST_ADDRESS unicast;

		if (adapter->OperStatus != IfOperStatusUp ||
			adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		for (unicast = adapter->FirstUnicastAddress;
			unicast != NULL; unicast = unicast->Next) {
			const struct sockaddr_in *address;
			struct sockaddr_in destination;
			unsigned long host_address;
			unsigned long mask;
			DWORD prefix;

			if (unicast->Address.lpSockaddr == NULL ||
				unicast->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			address = (const struct sockaddr_in *)
				unicast->Address.lpSockaddr;
			host_address = ntohl(address->sin_addr.s_addr);
			if (host_address == INADDR_ANY ||
				(host_address >> 24) == 127)
				continue;
			prefix = unicast->OnLinkPrefixLength;
			if (prefix >= 31)
				continue;
			mask = prefix == 0 ? 0 :
				0xffffffffUL << (32 - prefix);
			ZeroMemory(&destination, sizeof(destination));
			destination.sin_family = AF_INET;
			destination.sin_port = htons(USBRELAY_DISCOVERY_PORT);
			destination.sin_addr.s_addr =
				htonl(host_address | ~mask);
			send_discovery_packet(packet, length, &destination);
		}
	}
	HeapFree(GetProcessHeap(), 0, addresses);
}

static void send_discovery(void)
{
	BYTE *printer_buffer = NULL;
	DWORD printer_count = 0;
	DWORD valid_count = 0;
	DWORD index;
	char name[256];
	char packet[1024];
	int length;
	struct sockaddr_in destination;

	if (g_discovery_socket == INVALID_SOCKET)
		return;
	if (enum_local_printers(&printer_buffer, &printer_count)) {
		for (index = 0; index < printer_count; index++) {
			PRINTER_INFO_2W *info =
				&((PRINTER_INFO_2W *)printer_buffer)[index];
			if (info->pPrinterName != NULL &&
				info->pPrinterName[0] != L'\0')
				valid_count++;
		}
	}
	make_utf8_computer_name(name, sizeof(name));
	length = _snprintf_s(packet, sizeof(packet), _TRUNCATE,
		USBRELAY_DISCOVERY_MAGIC "\r\n"
		"name=%s\r\n"
		"print_port=%d\r\n"
		"printers=%lu\r\n",
		name, USBRELAY_PRINT_PORT, (unsigned long)valid_count);
	if (printer_buffer != NULL)
		HeapFree(GetProcessHeap(), 0, printer_buffer);
	if (length < 0)
		return;
	ZeroMemory(&destination, sizeof(destination));
	destination.sin_family = AF_INET;
	destination.sin_port = htons(USBRELAY_DISCOVERY_PORT);
	destination.sin_addr.s_addr = INADDR_BROADCAST;
	send_discovery_to_interfaces(packet, length);
	send_discovery_packet(packet, length, &destination);
}

static BOOL open_server_sockets(void)
{
	struct sockaddr_in address;
	BOOL broadcast = TRUE;
	int reuse = 1;

	g_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (g_listen_socket == INVALID_SOCKET)
		return FALSE;
	setsockopt(g_listen_socket, SOL_SOCKET, SO_REUSEADDR,
		(const char *)&reuse, sizeof(reuse));
	ZeroMemory(&address, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(USBRELAY_PRINT_PORT);
	if (bind(g_listen_socket, (const SOCKADDR *)&address, sizeof(address)) ==
		SOCKET_ERROR || listen(g_listen_socket, SOMAXCONN) == SOCKET_ERROR) {
		closesocket(g_listen_socket);
		g_listen_socket = INVALID_SOCKET;
		return FALSE;
	}

	g_discovery_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (g_discovery_socket == INVALID_SOCKET) {
		closesocket(g_listen_socket);
		g_listen_socket = INVALID_SOCKET;
		return FALSE;
	}
	if (setsockopt(g_discovery_socket, SOL_SOCKET, SO_BROADCAST,
		(const char *)&broadcast, sizeof(broadcast)) == SOCKET_ERROR) {
		closesocket(g_discovery_socket);
		g_discovery_socket = INVALID_SOCKET;
		closesocket(g_listen_socket);
		g_listen_socket = INVALID_SOCKET;
		return FALSE;
	}
	return TRUE;
}

static void close_server_sockets(void)
{
	if (g_discovery_socket != INVALID_SOCKET) {
		closesocket(g_discovery_socket);
		g_discovery_socket = INVALID_SOCKET;
	}
	if (g_listen_socket != INVALID_SOCKET) {
		closesocket(g_listen_socket);
		g_listen_socket = INVALID_SOCKET;
	}
}

static BOOL start_relay(void)
{
	WSADATA wsa_data;

	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
		return FALSE;
	g_core_mutex = CreateMutexW(NULL, FALSE, CORE_MUTEX_NAME);
	if (g_core_mutex == NULL ||
		GetLastError() == ERROR_ALREADY_EXISTS) {
		if (g_core_mutex != NULL)
			CloseHandle(g_core_mutex);
		g_core_mutex = NULL;
		WSACleanup();
		return FALSE;
	}
	InitializeCriticalSection(&g_client_lock);
	g_done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	g_stop_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
		STOP_EVENT_NAME);
	if (g_stop_event == NULL)
		g_stop_event = CreateEventW(NULL, TRUE, FALSE, STOP_EVENT_NAME);
	if (g_done_event == NULL || g_stop_event == NULL ||
		!open_server_sockets()) {
		close_server_sockets();
		if (g_done_event != NULL)
			CloseHandle(g_done_event);
		if (g_stop_event != NULL)
			CloseHandle(g_stop_event);
		DeleteCriticalSection(&g_client_lock);
		WSACleanup();
		g_done_event = NULL;
		g_stop_event = NULL;
		CloseHandle(g_core_mutex);
		g_core_mutex = NULL;
		return FALSE;
	}
	ResetEvent(g_stop_event);
	g_active = 1;
	g_stopping = FALSE;
	g_accept_thread = CreateThread(NULL, 0, accept_thread, NULL, 0, NULL);
	if (g_accept_thread == NULL) {
		g_active = 0;
		close_server_sockets();
		CloseHandle(g_done_event);
		CloseHandle(g_stop_event);
		DeleteCriticalSection(&g_client_lock);
		WSACleanup();
		g_done_event = NULL;
		g_stop_event = NULL;
		CloseHandle(g_core_mutex);
		g_core_mutex = NULL;
		return FALSE;
	}
	g_initialized = TRUE;
	return TRUE;
}

static void request_stop(void)
{
	relay_client *client;

	InterlockedExchange(&g_stopping, TRUE);
	if (g_stop_event != NULL)
		SetEvent(g_stop_event);
	if (g_listen_socket != INVALID_SOCKET)
		closesocket(g_listen_socket);
	g_listen_socket = INVALID_SOCKET;
	EnterCriticalSection(&g_client_lock);
	for (client = g_clients; client != NULL; client = client->next)
		shutdown(client->socket_handle, SD_BOTH);
	LeaveCriticalSection(&g_client_lock);
}

static void stop_relay(void)
{
	if (!g_initialized)
		return;
	request_stop();
	if (g_accept_thread != NULL) {
		WaitForSingleObject(g_accept_thread, INFINITE);
		CloseHandle(g_accept_thread);
		g_accept_thread = NULL;
	}
	/* Client threads remove themselves from g_clients before they exit. Do not
	 * destroy the lock until every thread has completed that operation. */
	if (g_done_event != NULL)
		WaitForSingleObject(g_done_event, INFINITE);
	close_server_sockets();
	if (g_done_event != NULL)
		CloseHandle(g_done_event);
	if (g_stop_event != NULL)
		CloseHandle(g_stop_event);
	if (g_core_mutex != NULL)
		CloseHandle(g_core_mutex);
	DeleteCriticalSection(&g_client_lock);
	WSACleanup();
	g_done_event = NULL;
	g_stop_event = NULL;
	g_core_mutex = NULL;
	g_initialized = FALSE;
}

static int run_relay(void)
{
	DWORD last_discovery = 0;

	if (!start_relay()) {
		log_message("ERROR", "Unable to start USBRelay listener.");
		return 1;
	}
	log_message("INFO", "USBRelay printer relay is listening on TCP 3242.");
	for (;;) {
		DWORD now = GetTickCount();

		if (WaitForSingleObject(g_stop_event, 500) == WAIT_OBJECT_0)
			break;
		if (last_discovery == 0 ||
			(DWORD)(now - last_discovery) >= DISCOVERY_INTERVAL_MS) {
			last_discovery = now;
			send_discovery();
		}
	}
	stop_relay();
	return 0;
}

int usbrelay_engine_run(void)
{
	return run_relay();
}

#ifndef USBRELAY_ENGINE_NO_MAIN
int main(int argc, char **argv)
{
	int index;

	for (index = 1; index < argc; index++) {
		if (strcmp(argv[index], "--help") == 0) {
			printf("USBRelay printer relay\n");
			printf("Runs the printer relay in the current user session.\n");
			return 0;
		}
	}
	return usbrelay_engine_run();
}
#endif
