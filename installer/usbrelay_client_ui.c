#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winspool.h>
#include <iphlpapi.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "../include/usbrelay_protocol.h"
#include "ui_common.h"
#include "ui_resource.h"

#define CLIENT_WINDOW_CLASS L"USBRelay-Client-UI"
#define CLIENT_WINDOW_TITLE L"USBRelay 客户端"
#define CLIENT_REGISTRY_KEY L"Software\\USBRelay-Client"
#define CLIENT_REGISTRY_KEY_MAX 160
#define CLIENT_PORT_REGISTRY_KEY L"SYSTEM\\CurrentControlSet\\Control\\Print\\Monitors\\USBRelay Port Monitor\\Ports"
#define CLIENT_TIMER 1
#define CLIENT_DISCOVERY_TIMEOUT_MS 45000
#define CLIENT_ACTIVE_PROBE_INTERVAL_MS 15000
#define CLIENT_ACTIVE_PROBE_FULL_INTERVAL_MS 90000
#define CLIENT_ACTIVE_PROBE_START_DELAY_MS 1500
#define CLIENT_ACTIVE_PROBE_CONNECT_TIMEOUT_MS 350
#define CLIENT_ACTIVE_PROBE_DATA_TIMEOUT_MS 1500
#define CLIENT_ACTIVE_PROBE_BATCH 48
#define CLIENT_MAX_PROBE_ADDRESSES 1024
#define CLIENT_PROBE_RESULT_MESSAGE (WM_APP + 1)
#define CLIENT_MAX_SERVERS 32
#define CLIENT_MAX_PRINTERS 128
#define CLIENT_MAX_LINE 8192
#define CLIENT_MAX_TCP 8192
#define CLIENT_SOCKET_TIMEOUT_MS 8000
#define CLIENT_PORT_HOST_MAX 220
#define CLIENT_QUEUE_MAX 220

#define IDC_STATUS 2001
#define IDC_DISCOVERED_LABEL 2002
#define IDC_DISCOVERED 2003
#define IDC_SERVER_LABEL 2004
#define IDC_SERVER 2005
#define IDC_REFRESH 2006
#define IDC_AUTOSTART 2007
#define IDC_PRINTERS 2008
#define IDC_ADD 2009
#define IDC_DEFAULT 2010
#define IDC_REMOVE 2011
#define IDC_LOG 2012
#define IDC_OPEN_PRINTER 2014

typedef struct DiscoveredServer {
	wchar_t name[128];
	wchar_t address[64];
	DWORD printer_count;
	DWORD last_seen;
} DiscoveredServer;

typedef struct RemotePrinter {
	DWORD id;
	wchar_t name[512];
	wchar_t driver[512];
	wchar_t original_port[512];
	wchar_t host[CLIENT_PORT_HOST_MAX + 1];
	wchar_t queue_name[CLIENT_QUEUE_MAX + 1];
	BOOL port_exists;
	BOOL queue_exists;
} RemotePrinter;

typedef struct ProbeResult {
	wchar_t name[128];
	wchar_t address[64];
	DWORD printer_count;
} ProbeResult;

typedef struct ActiveProbeContext {
	HWND window;
	volatile LONG *stop;
	volatile LONG *running;
	BOOL scan_subnet;
	wchar_t preferred[CLIENT_PORT_HOST_MAX + 1];
	DWORD preferred_addresses[64];
	int preferred_count;
	DWORD addresses[CLIENT_MAX_PROBE_ADDRESSES];
	int count;
	DWORD local_addresses[64];
	int local_count;
} ActiveProbeContext;

typedef struct ClientState {
	HWND window;
	HWND status;
	HWND discovered_label;
	HWND discovered;
	HWND server_label;
	HWND server;
	HWND refresh;
	HWND autostart;
	HWND printer_list;
	HWND add;
	HWND open_printer;
	HWND default_printer;
	HWND remove;
	HWND log;
	HFONT font;
	HICON tray_icon;
	wchar_t executable[MAX_PATH];
	SOCKET discovery_socket;
	BOOL winsock_started;
	BOOL tray_added;
	BOOL exiting;
	BOOL autostart_mode;
	BOOL updating_discovered;
	int discovered_count;
	DiscoveredServer discovered_servers[CLIENT_MAX_SERVERS];
	int printer_count;
	RemotePrinter remote_printers[CLIENT_MAX_PRINTERS];
	wchar_t last_refresh[64];
	int startup_refresh_countdown;
	HANDLE active_probe_thread;
	LONG active_probe_running;
	LONG active_probe_stop;
	DWORD next_active_probe;
	DWORD next_full_probe;
} ClientState;

static ClientState *state_from_window(HWND window)
{
	return (ClientState *)GetWindowLongPtrW(window, GWLP_USERDATA);
}

static BOOL has_argument(const wchar_t *wanted)
{
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	int index;
	BOOL found = FALSE;

	if (argv == NULL)
		return FALSE;
	for (index = 1; index < argc; index++) {
		if (!_wcsicmp(argv[index], wanted)) {
			found = TRUE;
			break;
		}
	}
	LocalFree(argv);
	return found;
}

static HWND make_control(ClientState *state, const wchar_t *class_name,
	const wchar_t *text, DWORD style, DWORD ex_style, int x, int y,
	int width, int height, int id)
{
	HWND control = CreateWindowExW(ex_style, class_name, text,
		WS_CHILD | WS_VISIBLE | style, x, y, width, height,
		state->window, (HMENU)(INT_PTR)id, NULL, NULL);
	if (control != NULL)
		SendMessageW(control, WM_SETFONT, (WPARAM)state->font, TRUE);
	return control;
}

static void show_window(HWND window)
{
	ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
	SetForegroundWindow(window);
}

static void set_status(ClientState *state, const wchar_t *text)
{
	if (state->status != NULL)
		SetWindowTextW(state->status, text);
}

static void log_message(ClientState *state, const wchar_t *format, ...)
{
	wchar_t message[2048];
	va_list arguments;

	va_start(arguments, format);
	_vsnwprintf_s(message, UI_ARRAY_COUNT(message), _TRUNCATE,
		format, arguments);
	va_end(arguments);
	UiAppendLog(state->log, L"%ls", message);
}

static void set_socket_timeout_ms(SOCKET socket_handle, DWORD timeout)
{
	setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
		(const char *)&timeout, sizeof(timeout));
	setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
		(const char *)&timeout, sizeof(timeout));
}

static void set_socket_timeout(SOCKET socket_handle)
{
	set_socket_timeout_ms(socket_handle, CLIENT_SOCKET_TIMEOUT_MS);
}

static BOOL send_all(SOCKET socket_handle, const char *data, int length)
{
	while (length > 0) {
		int sent = send(socket_handle, data, length, 0);
		if (sent <= 0)
			return FALSE;
		data += sent;
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

static BOOL utf8_to_wide(const char *source, wchar_t *destination,
	size_t destination_count)
{
	int length;

	if (destination == NULL || destination_count == 0)
		return FALSE;
	destination[0] = L'\0';
	if (source == NULL)
		source = "";
	length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source, -1,
		destination, (int)destination_count);
	if (length <= 0) {
		length = MultiByteToWideChar(CP_ACP, 0, source, -1,
			destination, (int)destination_count);
	}
	return length > 0;
}

static BOOL wide_to_utf8(const wchar_t *source, char *destination,
	size_t destination_count)
{
	int length;

	if (destination == NULL || destination_count == 0)
		return FALSE;
	destination[0] = '\0';
	if (source == NULL)
		source = L"";
	length = WideCharToMultiByte(CP_UTF8, 0, source, -1, destination,
		(int)destination_count, NULL, NULL);
	return length > 0;
}

static BOOL parse_key_value(const char *line, const char *key,
	char *value, size_t value_count)
{
	size_t key_length = strlen(key);
	if (strncmp(line, key, key_length) != 0 || line[key_length] != '=')
		return FALSE;
	if (value_count == 0)
		return FALSE;
	strncpy_s(value, value_count, line + key_length + 1, _TRUNCATE);
	return TRUE;
}

static DWORD parse_hex_id(const char *value)
{
	char *end = NULL;
	unsigned long parsed;
	if (value == NULL || *value == '\0')
		return 0;
	parsed = strtoul(value, &end, 16);
	if (end == value || *end != '\0')
		return 0;
	return (DWORD)parsed;
}

static BOOL make_discovery_socket(ClientState *state)
{
	struct sockaddr_in address;
	BOOL reuse = TRUE;
	u_long nonblocking = 1;

	state->discovery_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (state->discovery_socket == INVALID_SOCKET)
		return FALSE;
	setsockopt(state->discovery_socket, SOL_SOCKET, SO_REUSEADDR,
		(const char *)&reuse, sizeof(reuse));
	ZeroMemory(&address, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(USBRELAY_DISCOVERY_PORT);
	if (bind(state->discovery_socket, (const SOCKADDR *)&address,
		sizeof(address)) == SOCKET_ERROR) {
		closesocket(state->discovery_socket);
		state->discovery_socket = INVALID_SOCKET;
		return FALSE;
	}
	if (ioctlsocket(state->discovery_socket, FIONBIO, &nonblocking) != 0) {
		closesocket(state->discovery_socket);
		state->discovery_socket = INVALID_SOCKET;
		return FALSE;
	}
	return TRUE;
}

static int find_server(ClientState *state, const wchar_t *name,
	const wchar_t *address)
{
	int index;
	for (index = 0; index < state->discovered_count; index++) {
		if (!_wcsicmp(state->discovered_servers[index].address, address) ||
			(!_wcsicmp(state->discovered_servers[index].name, name) &&
				!wcscmp(state->discovered_servers[index].address, address)))
			return index;
	}
	return -1;
}

static int find_server_host(ClientState *state, const wchar_t *host)
{
	int index;

	if (host == NULL || host[0] == L'\0')
		return -1;
	for (index = 0; index < state->discovered_count; index++) {
		if (!_wcsicmp(state->discovered_servers[index].name, host) ||
			!_wcsicmp(state->discovered_servers[index].address, host))
			return index;
	}
	return -1;
}

static void refresh_discovered_combo(ClientState *state)
{
	int previous_item;
	int previous_index = -1;
	int selected_index = -1;
	int index;
	wchar_t current_host[CLIENT_PORT_HOST_MAX + 1];

	previous_item = (int)SendMessageW(state->discovered, CB_GETCURSEL, 0, 0);
	if (previous_item >= 0)
		previous_index = (int)SendMessageW(state->discovered,
			CB_GETITEMDATA, previous_item, 0);
	GetWindowTextW(state->server, current_host, UI_ARRAY_COUNT(current_host));
	UiTrimWhitespace(current_host);

	state->updating_discovered = TRUE;
	SendMessageW(state->discovered, CB_RESETCONTENT, 0, 0);
	for (index = 0; index < state->discovered_count; index++) {
		wchar_t text[256];
		int item;
		_snwprintf_s(text, UI_ARRAY_COUNT(text), _TRUNCATE,
			L"%ls  (%ls)  - %lu 个打印机",
			state->discovered_servers[index].name,
			state->discovered_servers[index].address,
			(unsigned long)state->discovered_servers[index].printer_count);
		item = (int)SendMessageW(state->discovered, CB_ADDSTRING, 0,
			(LPARAM)text);
		if (item >= 0)
			SendMessageW(state->discovered, CB_SETITEMDATA, item, index);
		if (index == previous_index ||
			(previous_index < 0 && find_server_host(state, current_host) == index))
			selected_index = item;
	}
	if (selected_index >= 0)
		SendMessageW(state->discovered, CB_SETCURSEL, selected_index, 0);
	state->updating_discovered = FALSE;
}

static void select_server(ClientState *state, int index, BOOL refresh)
{
	if (index < 0 || index >= state->discovered_count)
		return;
	SetWindowTextW(state->server, state->discovered_servers[index].name);
	if (refresh)
		SendMessageW(state->window, WM_COMMAND,
			MAKEWPARAM(IDC_REFRESH, BN_CLICKED), 0);
}

static void pump_discovery(ClientState *state)
{
	char packet[2048];
	struct sockaddr_in source;
	int source_length;

	if (state->discovery_socket == INVALID_SOCKET)
		return;
	for (;;) {
		char name_utf8[512] = { 0 };
		char port_utf8[64] = { 0 };
		char count_utf8[64] = { 0 };
		char address_text[64];
		wchar_t name[128];
		wchar_t address[64];
		char *cursor;
	char *line_end;
		DWORD printer_count = 0;
		int index;
		int received;
		BOOL changed = FALSE;

		source_length = sizeof(source);
		received = recvfrom(state->discovery_socket, packet,
			(int)sizeof(packet) - 1, 0, (SOCKADDR *)&source, &source_length);
		if (received == SOCKET_ERROR) {
			if (WSAGetLastError() == WSAEWOULDBLOCK)
				break;
			break;
		}
		packet[received] = '\0';
		if (strncmp(packet, USBRELAY_DISCOVERY_MAGIC, strlen(USBRELAY_DISCOVERY_MAGIC)) != 0)
			continue;
		cursor = strchr(packet, '\n');
		if (cursor == NULL)
			continue;
		cursor++;
		while (cursor != NULL && *cursor != '\0') {
			line_end = strchr(cursor, '\n');
			if (line_end != NULL)
				*line_end = '\0';
			if (*cursor == '\r') {
				cursor = line_end != NULL ? line_end + 1 : NULL;
				continue;
			}
			if (parse_key_value(cursor, "name", name_utf8,
				UI_ARRAY_COUNT(name_utf8))) {
			}
			else if (parse_key_value(cursor, "print_port", port_utf8,
				UI_ARRAY_COUNT(port_utf8))) {
			}
			else if (parse_key_value(cursor, "printers", count_utf8,
				UI_ARRAY_COUNT(count_utf8))) {
			}
			cursor = line_end != NULL ? line_end + 1 : NULL;
		}
		if (name_utf8[0] == '\0' || port_utf8[0] == '\0')
			continue;
		if (atoi(port_utf8) != USBRELAY_PRINT_PORT)
			continue;
		printer_count = (DWORD)strtoul(count_utf8, NULL, 10);
		if (inet_ntop(AF_INET, &source.sin_addr, address_text,
			sizeof(address_text)) == NULL)
			continue;
		utf8_to_wide(name_utf8, name, UI_ARRAY_COUNT(name));
		MultiByteToWideChar(CP_ACP, 0, address_text, -1, address,
			(int)UI_ARRAY_COUNT(address));
		index = find_server(state, name, address);
		if (index < 0) {
			if (state->discovered_count >= CLIENT_MAX_SERVERS)
				continue;
			index = state->discovered_count++;
			changed = TRUE;
		}
		if (wcscmp(state->discovered_servers[index].name, name) != 0 ||
			wcscmp(state->discovered_servers[index].address, address) != 0 ||
			state->discovered_servers[index].printer_count != printer_count)
			changed = TRUE;
		wcsncpy_s(state->discovered_servers[index].name,
			UI_ARRAY_COUNT(state->discovered_servers[index].name), name,
			_TRUNCATE);
		wcsncpy_s(state->discovered_servers[index].address,
			UI_ARRAY_COUNT(state->discovered_servers[index].address), address,
			_TRUNCATE);
		state->discovered_servers[index].printer_count = printer_count;
		state->discovered_servers[index].last_seen = GetTickCount();
		if (changed)
			refresh_discovered_combo(state);
	}
}

static BOOL probe_address_is_local(const ActiveProbeContext *context,
	DWORD address)
{
	int index;

	for (index = 0; index < context->local_count; index++) {
		if (context->local_addresses[index] == address)
			return TRUE;
	}
	return FALSE;
}

static void add_probe_address(ActiveProbeContext *context, DWORD address)
{
	int index;

	if (context->count >= CLIENT_MAX_PROBE_ADDRESSES ||
		address == 0 || address == 0xffffffffUL ||
		(address >> 24) == 127 || probe_address_is_local(context, address))
		return;
	for (index = 0; index < context->count; index++) {
		if (context->addresses[index] == address)
			return;
	}
	context->addresses[context->count++] = address;
}

static void add_neighbor_probe_addresses(ActiveProbeContext *context)
{
	DWORD buffer_size = 16 * 1024;
	MIB_IPNETTABLE *table = NULL;
	DWORD result;
	DWORD index;
	int attempt;

	for (attempt = 0; attempt < 3; attempt++) {
		table = (MIB_IPNETTABLE *)HeapAlloc(GetProcessHeap(),
			HEAP_ZERO_MEMORY, buffer_size);
		if (table == NULL)
			return;
		result = GetIpNetTable(table, &buffer_size, TRUE);
		if (result == ERROR_SUCCESS)
			break;
		HeapFree(GetProcessHeap(), 0, table);
		table = NULL;
		if (result != ERROR_INSUFFICIENT_BUFFER)
			return;
	}
	if (table == NULL)
		return;
	for (index = 0; index < table->dwNumEntries; index++) {
		DWORD address = ntohl(table->table[index].dwAddr);

		if (table->table[index].dwType == MIB_IPNET_TYPE_INVALID)
			continue;
		add_probe_address(context, address);
	}
	HeapFree(GetProcessHeap(), 0, table);
}

static void add_interface_probe_addresses(ActiveProbeContext *context)
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
			DWORD host_address;
			int index;

			if (unicast->Address.lpSockaddr == NULL ||
				unicast->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			address = (const struct sockaddr_in *)
				unicast->Address.lpSockaddr;
			host_address = ntohl(address->sin_addr.s_addr);
			if (host_address == 0 || (host_address >> 24) == 127 ||
				(host_address & 0xffff0000UL) == 0xa9fe0000UL)
				continue;
			for (index = 0; index < context->local_count; index++) {
				if (context->local_addresses[index] == host_address)
					break;
			}
			if (index == context->local_count &&
				context->local_count < (int)UI_ARRAY_COUNT(
					context->local_addresses))
				context->local_addresses[context->local_count++] =
					host_address;
		}
	}

	for (adapter = addresses; adapter != NULL; adapter = adapter->Next) {
		PIP_ADAPTER_UNICAST_ADDRESS unicast;

		if (adapter->OperStatus != IfOperStatusUp ||
			adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		for (unicast = adapter->FirstUnicastAddress;
			unicast != NULL; unicast = unicast->Next) {
			const struct sockaddr_in *address;
			DWORD host_address;
			DWORD prefix;
			DWORD mask;
			DWORD base;
			DWORD offset;

			if (unicast->Address.lpSockaddr == NULL ||
				unicast->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			address = (const struct sockaddr_in *)
				unicast->Address.lpSockaddr;
			host_address = ntohl(address->sin_addr.s_addr);
			prefix = unicast->OnLinkPrefixLength;
			if (prefix == 0 || prefix >= 32 ||
				(host_address & 0xffff0000UL) == 0xa9fe0000UL)
				continue;
			mask = 0xffffffffUL << (32 - prefix);
			base = host_address & mask;

			if (prefix >= 24) {
				DWORD first = base + 1;
				DWORD last = base | ~mask;

				if (last <= first)
					continue;
				for (offset = first; offset < last; offset++)
					add_probe_address(context, offset);
			}
			else {
				DWORD base24 = host_address & 0xffffff00UL;
				DWORD subnet;
				int neighbor;

				for (neighbor = -3; neighbor <= 3; neighbor++) {
					DWORD candidate_base = base24 +
						(DWORD)(neighbor * 256);
					if ((candidate_base & 0xff000000UL) == 0 ||
						(candidate_base & 0xff000000UL) ==
							0xff000000UL)
						continue;
					subnet = candidate_base;
					for (offset = 1; offset < 255; offset++)
						add_probe_address(context, subnet + offset);
				}
			}
		}
	}
	HeapFree(GetProcessHeap(), 0, addresses);
}

static void resolve_preferred_probe_addresses(ActiveProbeContext *context)
{
	struct addrinfoW hints;
	struct addrinfoW *addresses = NULL;
	struct addrinfoW *address;
	int result;

	if (context->preferred[0] == L'\0')
		return;
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	result = GetAddrInfoW(context->preferred, L"3242", &hints, &addresses);
	if (result != 0)
		return;
	for (address = addresses; address != NULL; address = address->ai_next) {
		const struct sockaddr_in *socket_address;
		DWORD host_address;
		int index;

		if (address->ai_addr == NULL ||
			address->ai_addrlen < sizeof(struct sockaddr_in) ||
			address->ai_family != AF_INET)
			continue;
		socket_address = (const struct sockaddr_in *)address->ai_addr;
		host_address = ntohl(socket_address->sin_addr.s_addr);
		if (host_address == 0 || (host_address >> 24) == 127 ||
			probe_address_is_local(context, host_address))
			continue;
		for (index = 0; index < context->preferred_count; index++) {
			if (context->preferred_addresses[index] == host_address)
				break;
		}
		if (index == context->preferred_count &&
			context->preferred_count < (int)UI_ARRAY_COUNT(
				context->preferred_addresses))
			context->preferred_addresses[context->preferred_count++] =
				host_address;
	}
	FreeAddrInfoW(addresses);
}

static BOOL connect_probe_socket(SOCKET socket_handle,
	const struct sockaddr_in *destination)
{
	u_long nonblocking = 1;
	u_long blocking = 0;
	DWORD deadline;
	int result;
	int socket_error;

	if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) != 0)
		return FALSE;
	result = connect(socket_handle, (const SOCKADDR *)destination,
		sizeof(*destination));
	if (result == 0) {
		ioctlsocket(socket_handle, FIONBIO, &blocking);
		return TRUE;
	}
	socket_error = WSAGetLastError();
	if (socket_error != WSAEWOULDBLOCK &&
		socket_error != WSAEINPROGRESS)
		return FALSE;
	deadline = GetTickCount() + CLIENT_ACTIVE_PROBE_CONNECT_TIMEOUT_MS;
	while ((LONG)(GetTickCount() - deadline) < 0) {
		fd_set write_set;
		fd_set error_set;
		struct timeval timeout;
		int error = 0;
		int error_size = sizeof(error);

		FD_ZERO(&write_set);
		FD_ZERO(&error_set);
		FD_SET(socket_handle, &write_set);
		FD_SET(socket_handle, &error_set);
		timeout.tv_sec = 0;
		timeout.tv_usec = 20000;
		result = select(0, NULL, &write_set, &error_set, &timeout);
		if (result == SOCKET_ERROR)
			return FALSE;
		if (result == 0)
			continue;
		if (!FD_ISSET(socket_handle, &write_set) &&
			!FD_ISSET(socket_handle, &error_set))
			continue;
		if (getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
			(char *)&error, &error_size) != 0 || error != 0)
			return FALSE;
		ioctlsocket(socket_handle, FIONBIO, &blocking);
		return TRUE;
	}
	WSASetLastError(WSAETIMEDOUT);
	return FALSE;
}

static BOOL query_probe_server(SOCKET socket_handle, DWORD *printer_count)
{
	char line[CLIENT_MAX_LINE];
	char value[128];

	set_socket_timeout_ms(socket_handle,
		CLIENT_ACTIVE_PROBE_DATA_TIMEOUT_MS);
	if (!send_all(socket_handle, USBRELAY_PRINT_MAGIC " LIST\r\n",
		(int)strlen(USBRELAY_PRINT_MAGIC " LIST\r\n")) ||
		!recv_line(socket_handle, line, sizeof(line)) ||
		strcmp(line, USBRELAY_PRINT_MAGIC " LIST-OK") != 0 ||
		!recv_line(socket_handle, line, sizeof(line)) ||
		!parse_key_value(line, "count", value, sizeof(value)))
		return FALSE;
	*printer_count = (DWORD)strtoul(value, NULL, 10);
	return TRUE;
}

static BOOL socket_peer_name(SOCKET socket_handle, wchar_t *name,
	size_t name_count)
{
	SOCKADDR_STORAGE peer;
	wchar_t resolved[NI_MAXHOST];
	int peer_length = sizeof(peer);

	if (name_count > 0)
		name[0] = L'\0';
	if (getpeername(socket_handle, (SOCKADDR *)&peer, &peer_length) != 0)
		return FALSE;
	if (GetNameInfoW((SOCKADDR *)&peer, peer_length, resolved,
		(DWORD)UI_ARRAY_COUNT(resolved), NULL, 0, NI_NAMEREQD) != 0)
		return FALSE;
	wcsncpy_s(name, name_count, resolved, _TRUNCATE);
	return name[0] != L'\0';
}

static void post_probe_result(ActiveProbeContext *context,
	const wchar_t *name, DWORD host_address, DWORD printer_count)
{
	ProbeResult *result;
	char address_text[64];
	IN_ADDR address;

	result = (ProbeResult *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		sizeof(*result));
	if (result == NULL)
		return;
	address.s_addr = htonl(host_address);
	if (inet_ntop(AF_INET, &address, address_text,
		sizeof(address_text)) == NULL) {
		HeapFree(GetProcessHeap(), 0, result);
		return;
	}
	wcsncpy_s(result->name, UI_ARRAY_COUNT(result->name),
		name != NULL && name[0] != L'\0' ? name : L"", _TRUNCATE);
	MultiByteToWideChar(CP_ACP, 0, address_text, -1, result->address,
		(int)UI_ARRAY_COUNT(result->address));
	if (result->name[0] == L'\0') {
		wcsncpy_s(result->name, UI_ARRAY_COUNT(result->name),
			result->address, _TRUNCATE);
	}
	result->printer_count = printer_count;
	if (!PostMessageW(context->window, CLIENT_PROBE_RESULT_MESSAGE, 0,
		(LPARAM)result))
		HeapFree(GetProcessHeap(), 0, result);
}

static void probe_address_batch(ActiveProbeContext *context, int start,
	int requested)
{
	SOCKET sockets[CLIENT_ACTIVE_PROBE_BATCH];
	DWORD addresses[CLIENT_ACTIVE_PROBE_BATCH];
	BOOL connected[CLIENT_ACTIVE_PROBE_BATCH];
	int count = 0;
	int index;
	DWORD deadline;

	if (requested > CLIENT_ACTIVE_PROBE_BATCH)
		requested = CLIENT_ACTIVE_PROBE_BATCH;
	for (index = 0; index < requested; index++) {
		struct sockaddr_in destination;
		u_long nonblocking = 1;
		SOCKET socket_handle;
		DWORD address = context->addresses[start + index];
		int result;

		sockets[count] = INVALID_SOCKET;
		addresses[count] = address;
		connected[count] = FALSE;
		socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (socket_handle == INVALID_SOCKET)
			continue;
		if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) != 0) {
			closesocket(socket_handle);
			continue;
		}
		ZeroMemory(&destination, sizeof(destination));
		destination.sin_family = AF_INET;
		destination.sin_port = htons(USBRELAY_PRINT_PORT);
		destination.sin_addr.s_addr = htonl(address);
		result = connect(socket_handle, (const SOCKADDR *)&destination,
			sizeof(destination));
		if (result == 0) {
			connected[count] = TRUE;
		}
		else if (WSAGetLastError() != WSAEWOULDBLOCK &&
			WSAGetLastError() != WSAEINPROGRESS) {
			closesocket(socket_handle);
			continue;
		}
		sockets[count++] = socket_handle;
	}

	deadline = GetTickCount() + CLIENT_ACTIVE_PROBE_CONNECT_TIMEOUT_MS;
	while ((LONG)(GetTickCount() - deadline) < 0 &&
		!InterlockedCompareExchange(context->stop, 0, 0)) {
		fd_set write_set;
		fd_set error_set;
		struct timeval timeout;
		BOOL pending = FALSE;
		int selected;

		FD_ZERO(&write_set);
		FD_ZERO(&error_set);
		for (index = 0; index < count; index++) {
			if (sockets[index] == INVALID_SOCKET || connected[index])
				continue;
			FD_SET(sockets[index], &write_set);
			FD_SET(sockets[index], &error_set);
			pending = TRUE;
		}
		if (!pending)
			break;
		timeout.tv_sec = 0;
		timeout.tv_usec = 20000;
		selected = select(0, NULL, &write_set, &error_set, &timeout);
		if (selected == SOCKET_ERROR)
			break;
		if (selected == 0)
			continue;
		for (index = 0; index < count; index++) {
			int error = 0;
			int error_size = sizeof(error);

			if (sockets[index] == INVALID_SOCKET || connected[index] ||
				(!FD_ISSET(sockets[index], &write_set) &&
					!FD_ISSET(sockets[index], &error_set)))
				continue;
			if (getsockopt(sockets[index], SOL_SOCKET, SO_ERROR,
				(char *)&error, &error_size) == 0 && error == 0) {
				connected[index] = TRUE;
			}
			else {
				closesocket(sockets[index]);
				sockets[index] = INVALID_SOCKET;
			}
		}
	}

	for (index = 0; index < count; index++) {
		DWORD printer_count = 0;
		u_long blocking = 0;
		wchar_t address_text[64];
		wchar_t peer_name[128];

		if (sockets[index] == INVALID_SOCKET)
			continue;
		if (!connected[index] ||
			InterlockedCompareExchange(context->stop, 0, 0)) {
			closesocket(sockets[index]);
			continue;
		}
		ioctlsocket(sockets[index], FIONBIO, &blocking);
		if (query_probe_server(sockets[index], &printer_count)) {
			if (!socket_peer_name(sockets[index], peer_name,
				UI_ARRAY_COUNT(peer_name))) {
				wcsncpy_s(peer_name, UI_ARRAY_COUNT(peer_name),
					L"", _TRUNCATE);
			}
			_snwprintf_s(address_text, UI_ARRAY_COUNT(address_text),
				_TRUNCATE, L"%lu.%lu.%lu.%lu",
				(unsigned long)((addresses[index] >> 24) & 0xff),
				(unsigned long)((addresses[index] >> 16) & 0xff),
				(unsigned long)((addresses[index] >> 8) & 0xff),
				(unsigned long)(addresses[index] & 0xff));
			post_probe_result(context, peer_name[0] != L'\0' ?
				peer_name : address_text, addresses[index], printer_count);
		}
		shutdown(sockets[index], SD_BOTH);
		closesocket(sockets[index]);
	}
}

static DWORD WINAPI active_probe_thread_proc(void *parameter)
{
	ActiveProbeContext *context = (ActiveProbeContext *)parameter;
	int index;

	resolve_preferred_probe_addresses(context);
	for (index = 0; index < context->preferred_count &&
		!InterlockedCompareExchange(context->stop, 0, 0); index++) {
		struct sockaddr_in destination;
		SOCKET socket_handle = socket(AF_INET, SOCK_STREAM,
			IPPROTO_TCP);
		DWORD printer_count = 0;

		if (socket_handle == INVALID_SOCKET)
			continue;
		ZeroMemory(&destination, sizeof(destination));
		destination.sin_family = AF_INET;
		destination.sin_port = htons(USBRELAY_PRINT_PORT);
		destination.sin_addr.s_addr =
			htonl(context->preferred_addresses[index]);
		if (connect_probe_socket(socket_handle, &destination) &&
			query_probe_server(socket_handle, &printer_count)) {
			post_probe_result(context, context->preferred,
				context->preferred_addresses[index], printer_count);
		}
		if (socket_handle != INVALID_SOCKET)
			closesocket(socket_handle);
	}

	add_neighbor_probe_addresses(context);
	if (context->scan_subnet)
		add_interface_probe_addresses(context);
	for (index = 0; index < context->count &&
		!InterlockedCompareExchange(context->stop, 0, 0);
		index += CLIENT_ACTIVE_PROBE_BATCH) {
		int remaining = context->count - index;

		probe_address_batch(context, index, remaining);
	}
	InterlockedExchange(context->running, 0);
	HeapFree(GetProcessHeap(), 0, context);
	return 0;
}

static BOOL start_active_probe(ClientState *state, BOOL scan_subnet)
{
	ActiveProbeContext *context;
	HANDLE thread;

	if (InterlockedCompareExchange(&state->active_probe_running, 1, 0) != 0)
		return FALSE;
	if (state->active_probe_thread != NULL) {
		CloseHandle(state->active_probe_thread);
		state->active_probe_thread = NULL;
	}
	context = (ActiveProbeContext *)HeapAlloc(GetProcessHeap(),
		HEAP_ZERO_MEMORY, sizeof(*context));
	if (context == NULL) {
		InterlockedExchange(&state->active_probe_running, 0);
		return FALSE;
	}
	context->window = state->window;
	context->stop = &state->active_probe_stop;
	context->running = &state->active_probe_running;
	context->scan_subnet = scan_subnet;
	GetWindowTextW(state->server, context->preferred,
		UI_ARRAY_COUNT(context->preferred));
	UiTrimWhitespace(context->preferred);
	thread = CreateThread(NULL, 0, active_probe_thread_proc, context, 0, NULL);
	if (thread == NULL) {
		HeapFree(GetProcessHeap(), 0, context);
		InterlockedExchange(&state->active_probe_running, 0);
		return FALSE;
	}
	state->active_probe_thread = thread;
	return TRUE;
}

static void merge_probe_result(ClientState *state, const ProbeResult *result)
{
	int index;
	BOOL changed = FALSE;

	if (result == NULL || result->address[0] == L'\0')
		return;
	index = find_server(state, result->name, result->address);
	if (index < 0) {
		if (state->discovered_count >= CLIENT_MAX_SERVERS)
			return;
		index = state->discovered_count++;
		changed = TRUE;
	}
	if (state->discovered_servers[index].name[0] == L'\0' ||
		(!_wcsicmp(state->discovered_servers[index].name,
			state->discovered_servers[index].address) &&
			_wcsicmp(result->name, result->address) != 0)) {
		wcsncpy_s(state->discovered_servers[index].name,
			UI_ARRAY_COUNT(state->discovered_servers[index].name),
			result->name, _TRUNCATE);
		changed = TRUE;
	}
	if (_wcsicmp(state->discovered_servers[index].address,
		result->address) != 0) {
		wcsncpy_s(state->discovered_servers[index].address,
			UI_ARRAY_COUNT(state->discovered_servers[index].address),
			result->address, _TRUNCATE);
		changed = TRUE;
	}
	if (state->discovered_servers[index].printer_count !=
		result->printer_count) {
		state->discovered_servers[index].printer_count =
			result->printer_count;
		changed = TRUE;
	}
	state->discovered_servers[index].last_seen = GetTickCount();
	if (changed)
		refresh_discovered_combo(state);
}

static void prune_discovered(ClientState *state)
{
	DWORD now = GetTickCount();
	int index = 0;
	BOOL changed = FALSE;

	while (index < state->discovered_count) {
		if ((DWORD)(now - state->discovered_servers[index].last_seen) >
			CLIENT_DISCOVERY_TIMEOUT_MS) {
			if (index + 1 < state->discovered_count)
				memmove(&state->discovered_servers[index],
					&state->discovered_servers[index + 1],
					(size_t)(state->discovered_count - index - 1) *
					sizeof(state->discovered_servers[0]));
			state->discovered_count--;
			changed = TRUE;
			continue;
		}
		index++;
	}
	if (changed)
		refresh_discovered_combo(state);
}

static BOOL connect_server(const wchar_t *host, SOCKET *socket_out)
{
	struct addrinfoW hints;
	struct addrinfoW *addresses = NULL;
	struct addrinfoW *address;
	SOCKET socket_handle = INVALID_SOCKET;
	int result;

	ZeroMemory(&hints, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	result = GetAddrInfoW(host, L"3242", &hints, &addresses);
	if (result != 0)
		return FALSE;
	for (address = addresses; address != NULL; address = address->ai_next) {
		socket_handle = socket(address->ai_family, address->ai_socktype,
			address->ai_protocol);
		if (socket_handle == INVALID_SOCKET)
			continue;
		set_socket_timeout(socket_handle);
		if (connect(socket_handle, address->ai_addr,
			(int)address->ai_addrlen) == 0)
			break;
		closesocket(socket_handle);
		socket_handle = INVALID_SOCKET;
	}
	FreeAddrInfoW(addresses);
	if (socket_handle == INVALID_SOCKET)
		return FALSE;
	*socket_out = socket_handle;
	return TRUE;
}

static BOOL read_list_item(SOCKET socket_handle, RemotePrinter *printer,
	const wchar_t *host)
{
	char line[CLIENT_MAX_LINE];
	char value[CLIENT_MAX_LINE];
	char utf8[CLIENT_MAX_LINE];
	BOOL got_id = FALSE;
	BOOL got_name = FALSE;
	BOOL got_driver = FALSE;
	BOOL got_port = FALSE;

	ZeroMemory(printer, sizeof(*printer));
	while (recv_line(socket_handle, line, sizeof(line))) {
		if (line[0] == '\0')
			break;
		if (parse_key_value(line, "printer", value, sizeof(value))) {
			printer->id = parse_hex_id(value);
			got_id = printer->id != 0;
		}
		else if (parse_key_value(line, "name", value, sizeof(value))) {
			strncpy_s(utf8, sizeof(utf8), value, _TRUNCATE);
			got_name = utf8_to_wide(utf8, printer->name,
				UI_ARRAY_COUNT(printer->name));
		}
		else if (parse_key_value(line, "driver", value, sizeof(value))) {
			strncpy_s(utf8, sizeof(utf8), value, _TRUNCATE);
			got_driver = utf8_to_wide(utf8, printer->driver,
				UI_ARRAY_COUNT(printer->driver));
		}
		else if (parse_key_value(line, "port", value, sizeof(value))) {
			strncpy_s(utf8, sizeof(utf8), value, _TRUNCATE);
			got_port = utf8_to_wide(utf8, printer->original_port,
				UI_ARRAY_COUNT(printer->original_port));
		}
	}
	if (!got_id || !got_name || !got_driver || !got_port)
		return FALSE;
	wcsncpy_s(printer->host, UI_ARRAY_COUNT(printer->host), host, _TRUNCATE);
	return TRUE;
}

static BOOL query_printers(ClientState *state, const wchar_t *host)
{
	SOCKET socket_handle = INVALID_SOCKET;
	char line[CLIENT_MAX_LINE];
	char value[128];
	RemotePrinter *printers = NULL;
	DWORD count;
	DWORD loaded = 0;
	DWORD index;

	if (!connect_server(host, &socket_handle))
		return FALSE;
	if (!send_all(socket_handle, USBRELAY_PRINT_MAGIC " LIST\r\n",
		(int)strlen(USBRELAY_PRINT_MAGIC " LIST\r\n")) ||
		!recv_line(socket_handle, line, sizeof(line)) ||
		strcmp(line, USBRELAY_PRINT_MAGIC " LIST-OK") != 0 ||
		!recv_line(socket_handle, line, sizeof(line)) ||
		!parse_key_value(line, "count", value, sizeof(value))) {
		closesocket(socket_handle);
		return FALSE;
	}
	count = (DWORD)strtoul(value, NULL, 10);
	if (count > CLIENT_MAX_PRINTERS)
		count = CLIENT_MAX_PRINTERS;
	if (count > 0) {
		printers = (RemotePrinter *)HeapAlloc(GetProcessHeap(),
			HEAP_ZERO_MEMORY, sizeof(RemotePrinter) * count);
		if (printers == NULL) {
			closesocket(socket_handle);
			SetLastError(ERROR_NOT_ENOUGH_MEMORY);
			return FALSE;
		}
	}
	for (index = 0; index < count; index++) {
		if (!read_list_item(socket_handle, &printers[loaded], host)) {
			closesocket(socket_handle);
			HeapFree(GetProcessHeap(), 0, printers);
			return FALSE;
		}
		loaded++;
	}
	shutdown(socket_handle, SD_BOTH);
	closesocket(socket_handle);
	if (loaded > 0) {
		memcpy(state->remote_printers, printers,
			(size_t)loaded * sizeof(RemotePrinter));
	}
	state->printer_count = (int)loaded;
	HeapFree(GetProcessHeap(), 0, printers);
	return TRUE;
}

/*
 * Legacy driver-install helpers are intentionally excluded from the client.
 * The client creates printer ports only; Windows' Add Printer workflow owns
 * driver selection and installation.
 */
#if 0
static const wchar_t *native_printer_environment(void)
{
	SYSTEM_INFO system_info;

	ZeroMemory(&system_info, sizeof(system_info));
	GetNativeSystemInfo(&system_info);
	if (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
		return L"Windows x64";
	if (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64)
		return L"Windows IA64";
	return L"Windows NT x86";
}

static BOOL driver_is_installed_in_environment(const wchar_t *environment,
	const wchar_t *driver_name)
{
	DWORD needed = 0;
	DWORD count = 0;
	BYTE *buffer = NULL;
	DWORD index;
	BOOL found = FALSE;

	if (driver_name == NULL || driver_name[0] == L'\0')
		return FALSE;
	EnumPrinterDriversW(NULL, (LPWSTR)environment, 2, NULL, 0,
		&needed, &count);
	if (needed == 0)
		return FALSE;
	buffer = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
	if (buffer == NULL)
		return FALSE;
	if (EnumPrinterDriversW(NULL, (LPWSTR)environment, 2, buffer, needed,
		&needed, &count)) {
		for (index = 0; index < count; index++) {
			DRIVER_INFO_2W *info =
				&((DRIVER_INFO_2W *)buffer)[index];
			if (info->pName != NULL && !_wcsicmp(info->pName, driver_name)) {
				found = TRUE;
				break;
			}
		}
	}
	HeapFree(GetProcessHeap(), 0, buffer);
	return found;
}

static BOOL driver_is_installed(const wchar_t *driver_name)
{
	const wchar_t *environment = native_printer_environment();

	if (driver_is_installed_in_environment(environment, driver_name))
		return TRUE;
	/* NULL asks the spooler for the caller's environment. This fallback is
	 * useful on older Windows versions that reject an explicit environment. */
	if (_wcsicmp(environment, L"Windows NT x86") != 0)
		return driver_is_installed_in_environment(NULL, driver_name);
	return FALSE;
}

static BOOL append_install_character(wchar_t *buffer, size_t buffer_count,
	size_t *length, wchar_t character)
{
	if (*length + 1 >= buffer_count)
		return FALSE;
	buffer[(*length)++] = character;
	buffer[*length] = L'\0';
	return TRUE;
}

static BOOL append_install_argument(wchar_t *buffer, size_t buffer_count,
	size_t *length, const wchar_t *value)
{
	const wchar_t *cursor;

	if (*length > 0 &&
		!append_install_character(buffer, buffer_count, length, L' '))
		return FALSE;
	if (!append_install_character(buffer, buffer_count, length, L'"'))
		return FALSE;
	for (cursor = value != NULL ? value : L""; *cursor != L'\0';) {
		if (*cursor == L'\\') {
			const wchar_t *run_start = cursor;
			size_t run_length = 0;
			BOOL before_quote;

			while (*cursor == L'\\') {
				run_length++;
				cursor++;
			}
			before_quote = *cursor == L'"';
			while (run_length > 0) {
				if (!append_install_character(buffer, buffer_count, length,
					L'\\'))
					return FALSE;
				run_length--;
			}
			if (before_quote || *cursor == L'\0') {
				/* Backslashes before a quote or the closing quote must be
				 * doubled for CommandLineToArgvW-compatible parsing. */
				for (run_length = (size_t)(cursor - run_start);
					run_length > 0; run_length--) {
					if (!append_install_character(buffer, buffer_count,
						length, L'\\'))
						return FALSE;
				}
			}
			if (before_quote) {
				if (!append_install_character(buffer, buffer_count, length,
					L'\\') ||
					!append_install_character(buffer, buffer_count, length,
					L'"'))
					return FALSE;
				cursor++;
			}
			continue;
		}
		if (*cursor == L'"' &&
			!append_install_character(buffer, buffer_count, length, L'\\'))
			return FALSE;
		if (!append_install_character(buffer, buffer_count, length, *cursor))
			return FALSE;
		cursor++;
	}
	return append_install_character(buffer, buffer_count, length, L'"');
}
#endif

static BOOL get_rundll32_path(wchar_t *path, size_t path_count)
{
	SYSTEM_INFO system_info;

	ZeroMemory(&system_info, sizeof(system_info));
	GetNativeSystemInfo(&system_info);
#if !defined(_WIN64)
	if (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
		system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64) {
		wchar_t windows_directory[MAX_PATH];
		UINT length;
		length = GetWindowsDirectoryW(windows_directory,
			(DWORD)UI_ARRAY_COUNT(windows_directory));
		if (length > 0 && length < UI_ARRAY_COUNT(windows_directory) &&
			_snwprintf_s(path, path_count, _TRUNCATE,
				L"%ls\\Sysnative\\rundll32.exe", windows_directory) >= 0 &&
			GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
			return TRUE;
	}
#endif
	return UiGetSystemToolPath(L"rundll32.exe", path, path_count);
}

#if 0
static const wchar_t *printer_driver_architecture(void)
{
	SYSTEM_INFO system_info;

	ZeroMemory(&system_info, sizeof(system_info));
	GetNativeSystemInfo(&system_info);
	if (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
		return L"x64";
	if (system_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64)
		return L"ia64";
	return L"x86";
}

static void trim_whitespace_in_place(wchar_t *text)
{
	wchar_t *start;
	wchar_t *end;

	if (text == NULL)
		return;
	start = text;
	while (*start != L'\0' && iswspace(*start))
		start++;
	if (start != text)
		memmove(text, start, (wcslen(start) + 1) * sizeof(wchar_t));
	end = text + wcslen(text);
	while (end > text && iswspace(end[-1]))
		end--;
	*end = L'\0';
}

static BOOL inf_model_name_matches(const wchar_t *entry,
	const wchar_t *driver_name, const wchar_t *inf_path)
{
	wchar_t candidate[512];
	wchar_t expanded[512];
	const wchar_t *closing_quote;
	size_t candidate_length;

	if (entry == NULL || driver_name == NULL)
		return FALSE;
	trim_whitespace_in_place((wchar_t *)entry);
	if (entry[0] == L'"') {
		closing_quote = wcsrchr(entry + 1, L'"');
		if (closing_quote == NULL || closing_quote == entry + 1)
			return FALSE;
		candidate_length = (size_t)(closing_quote - (entry + 1));
		if (candidate_length >= UI_ARRAY_COUNT(candidate))
			candidate_length = UI_ARRAY_COUNT(candidate) - 1;
		wcsncpy_s(candidate, UI_ARRAY_COUNT(candidate), entry + 1,
			candidate_length);
	}
	else {
		wcsncpy_s(candidate, UI_ARRAY_COUNT(candidate), entry, _TRUNCATE);
	}
	trim_whitespace_in_place(candidate);
	if (!_wcsicmp(candidate, driver_name))
		return TRUE;

	/* Printer INFs may use a [Strings] token such as
	 * "%HP_Laser_MFP_1136_1139_1188%". */
	if (candidate[0] == L'%' && candidate[1] != L'\0' &&
		candidate[wcslen(candidate) - 1] == L'%') {
		candidate[wcslen(candidate) - 1] = L'\0';
		if (candidate[1] != L'\0' &&
			GetPrivateProfileStringW(L"Strings", candidate + 1, L"",
				expanded, UI_ARRAY_COUNT(expanded), inf_path) > 0) {
			trim_whitespace_in_place(expanded);
			if (!_wcsicmp(expanded, driver_name))
				return TRUE;
		}
	}
	return FALSE;
}

static BOOL inf_section_contains_driver(const wchar_t *section,
	const wchar_t *driver_name, const wchar_t *inf_path)
{
	DWORD capacity;
	DWORD copied;
	wchar_t *keys = NULL;
	wchar_t *entry;
	wchar_t *end;
	wchar_t *separator;
	BOOL found = FALSE;

	capacity = 4096;
	while (capacity <= 1024 * 1024) {
		keys = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
			(size_t)capacity * sizeof(wchar_t));
		if (keys == NULL)
			return FALSE;
		copied = GetPrivateProfileSectionW(section, keys, capacity, inf_path);
		if (copied > 0 && copied < capacity - 2)
			break;
		HeapFree(GetProcessHeap(), 0, keys);
		keys = NULL;
		capacity *= 2;
	}
	if (keys != NULL) {
		entry = keys;
		while (*entry != L'\0') {
			end = entry + wcslen(entry);
			trim_whitespace_in_place(entry);
			separator = wcschr(entry, L'=');
			if (separator != NULL)
				*separator = L'\0';
			if (inf_model_name_matches(entry, driver_name, inf_path)) {
				found = TRUE;
				break;
			}
			entry = end + 1;
		}
		HeapFree(GetProcessHeap(), 0, keys);
	}
	return found;
}

static BOOL validate_driver_inf(const wchar_t *inf_path,
	const wchar_t *driver_name, wchar_t *message, size_t message_count)
{
	wchar_t inf_class[128];
	DWORD capacity;
	DWORD copied;
	wchar_t *sections = NULL;
	wchar_t *section;
	wchar_t *end;
	BOOL found = FALSE;

	if (message_count > 0)
		message[0] = L'\0';
	if (GetFileAttributesW(inf_path) == INVALID_FILE_ATTRIBUTES) {
		_snwprintf_s(message, message_count, _TRUNCATE,
			L"无法读取所选 INF 文件。");
		return FALSE;
	}
	if (GetPrivateProfileStringW(L"Version", L"Class", L"", inf_class,
		UI_ARRAY_COUNT(inf_class), inf_path) == 0) {
		_snwprintf_s(message, message_count, _TRUNCATE,
			L"该 INF 文件缺少 [Version] Class 声明，无法确认它是打印机驱动。\n\n"
			L"请选择厂商驱动包中 Class=Printer 的 INF 文件。");
		return FALSE;
	}
	trim_whitespace_in_place(inf_class);
	if (_wcsicmp(inf_class, L"Printer") != 0) {
		_snwprintf_s(message, message_count, _TRUNCATE,
			L"所选 INF 不是打印机驱动。\n\n"
			L"检测到的设备类型：%ls\n"
			L"请选择 Class=Printer 的打印机驱动 INF，不要选择扫描仪、"
			L"相机或其他设备的 INF。", inf_class);
		return FALSE;
	}

	capacity = 4096;
	while (capacity <= 1024 * 1024) {
		sections = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
			(size_t)capacity * sizeof(wchar_t));
		if (sections == NULL) {
			_snwprintf_s(message, message_count, _TRUNCATE,
				L"内存不足，无法检查 INF 驱动型号。");
			return FALSE;
		}
		copied = GetPrivateProfileSectionNamesW(sections, capacity, inf_path);
		if (copied > 0 && copied < capacity - 2)
			break;
		HeapFree(GetProcessHeap(), 0, sections);
		sections = NULL;
		capacity *= 2;
	}
	if (sections != NULL) {
		section = sections;
		while (*section != L'\0') {
			end = section + wcslen(section);
			if (inf_section_contains_driver(section, driver_name,
				inf_path)) {
				found = TRUE;
				break;
			}
			section = end + 1;
		}
		HeapFree(GetProcessHeap(), 0, sections);
	}
	else {
		_snwprintf_s(message, message_count, _TRUNCATE,
			L"无法读取 INF 的驱动型号列表。请确认文件完整且没有被占用。");
		return FALSE;
	}
	if (!found) {
		_snwprintf_s(message, message_count, _TRUNCATE,
			L"此 INF 中没有检测到服务端发布的驱动“%ls”。\n\n"
			L"请选择包含该型号的打印机 INF；厂商驱动安装包通常需要保留"
			L"完整的 INF、CAT 和子目录结构。", driver_name);
		return FALSE;
	}
	return TRUE;
}

static BOOL choose_driver_inf(HWND owner, wchar_t *path, size_t path_count)
{
	OPENFILENAMEW dialog;
	wchar_t filter[] =
		L"打印机驱动安装信息 (*.inf)\0*.inf\0"
		L"所有文件 (*.*)\0*.*\0\0";
	wchar_t *extension;
	DWORD dialog_error;

	if (path == NULL || path_count < 2 || path_count > 0xFFFF) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	path[0] = L'\0';
	ZeroMemory(&dialog, sizeof(dialog));
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = owner;
	dialog.lpstrFilter = filter;
	dialog.lpstrFile = path;
	dialog.nMaxFile = (DWORD)path_count;
	dialog.lpstrTitle = L"选择打印机驱动 INF 文件";
	dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST |
		OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&dialog)) {
		dialog_error = CommDlgExtendedError();
		SetLastError(dialog_error != 0 ? dialog_error : ERROR_CANCELLED);
		return FALSE;
	}
	extension = wcsrchr(path, L'.');
	if (extension == NULL || _wcsicmp(extension, L".inf") != 0) {
		SetLastError(ERROR_INVALID_NAME);
		return FALSE;
	}
	return TRUE;
}

static BOOL install_driver_from_inf(HWND owner, const wchar_t *inf_path,
	const wchar_t *driver_name, DWORD *exit_code)
{
	wchar_t rundll32[MAX_PATH];
	wchar_t arguments[CLIENT_INSTALL_ARGUMENTS];
	wchar_t working_directory[MAX_PATH * 4];
	wchar_t *separator;
	SHELLEXECUTEINFOW execute_info;
	size_t length = 0;
	DWORD wait_result;

	if (inf_path == NULL || driver_name == NULL ||
		inf_path[0] == L'\0' || driver_name[0] == L'\0') {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!get_rundll32_path(rundll32, UI_ARRAY_COUNT(rundll32)) ||
		!append_install_argument(arguments, UI_ARRAY_COUNT(arguments),
			&length, L"printui.dll,PrintUIEntry") ||
		!append_install_argument(arguments, UI_ARRAY_COUNT(arguments),
			&length, L"/ia") ||
		!append_install_argument(arguments, UI_ARRAY_COUNT(arguments),
			&length, L"/m") ||
		!append_install_argument(arguments, UI_ARRAY_COUNT(arguments),
			&length, driver_name) ||
		!append_install_argument(arguments, UI_ARRAY_COUNT(arguments),
			&length, L"/h") ||
		!append_install_argument(arguments, UI_ARRAY_COUNT(arguments),
			&length, printer_driver_architecture()) ||
		!append_install_argument(arguments, UI_ARRAY_COUNT(arguments),
			&length, L"/v") ||
		!append_install_argument(arguments, UI_ARRAY_COUNT(arguments),
			&length, L"3") ||
		!append_install_argument(arguments, UI_ARRAY_COUNT(arguments),
			&length, L"/f") ||
		!append_install_argument(arguments, UI_ARRAY_COUNT(arguments),
			&length, inf_path)) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	if (wcsncpy_s(working_directory, UI_ARRAY_COUNT(working_directory),
		inf_path, _TRUNCATE) != 0) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	separator = wcsrchr(working_directory, L'\\');
	if (separator != NULL)
		*separator = L'\0';

	ZeroMemory(&execute_info, sizeof(execute_info));
	execute_info.cbSize = sizeof(execute_info);
	execute_info.fMask = SEE_MASK_NOCLOSEPROCESS;
	execute_info.hwnd = owner;
	execute_info.lpVerb = L"runas";
	execute_info.lpFile = rundll32;
	execute_info.lpParameters = arguments;
	execute_info.lpDirectory = working_directory[0] != L'\0' ?
		working_directory : NULL;
	execute_info.nShow = SW_SHOWNORMAL;
	if (!ShellExecuteExW(&execute_info))
		return FALSE;
	if (execute_info.hProcess == NULL) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	wait_result = WaitForSingleObject(execute_info.hProcess, INFINITE);
	if (wait_result != WAIT_OBJECT_0) {
		CloseHandle(execute_info.hProcess);
		SetLastError(ERROR_GEN_FAILURE);
		return FALSE;
	}
	if (exit_code == NULL || GetExitCodeProcess(execute_info.hProcess,
		exit_code)) {
		CloseHandle(execute_info.hProcess);
		return TRUE;
	}
	CloseHandle(execute_info.hProcess);
	return FALSE;
}

static BOOL wait_for_driver_registration(const wchar_t *driver_name)
{
	int attempt;

	for (attempt = 0; attempt < 10; attempt++) {
		if (driver_is_installed(driver_name))
			return TRUE;
		Sleep(500);
	}
	return FALSE;
}
#endif

static BOOL make_port_name(const RemotePrinter *printer, wchar_t *port,
	size_t port_count)
{
	if (wcslen(printer->host) + 10 >= CLIENT_PORT_HOST_MAX ||
		_snwprintf_s(port, port_count, _TRUNCATE, L"USBRELAY:%ls:%08lX",
			printer->host, (unsigned long)printer->id) < 0) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	return TRUE;
}

static BOOL port_name_in_spooler(const wchar_t *port_name)
{
	DWORD needed = 0;
	DWORD returned = 0;
	BYTE *buffer = NULL;
	DWORD index;
	BOOL found = FALSE;

	if (port_name == NULL || port_name[0] == L'\0')
		return FALSE;
	if (!EnumPortsW(NULL, 1, NULL, 0, &needed, &returned) &&
		GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return FALSE;
	if (needed == 0)
		return FALSE;
	buffer = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
	if (buffer == NULL)
		return FALSE;
	if (EnumPortsW(NULL, 1, buffer, needed, &needed, &returned)) {
		for (index = 0; index < returned; index++) {
			PORT_INFO_1W *info = &((PORT_INFO_1W *)buffer)[index];

			if (info->pName != NULL &&
				_wcsicmp(info->pName, port_name) == 0) {
				found = TRUE;
				break;
			}
		}
	}
	HeapFree(GetProcessHeap(), 0, buffer);
	return found;
}

static BOOL call_monitor_xcv(const wchar_t *command,
	const wchar_t *port_name, DWORD *error)
{
	wchar_t monitor_object[128];
	HANDLE monitor = NULL;
	DWORD input_size;
	DWORD needed = 0;
	DWORD status = ERROR_SUCCESS;
	BOOL result;

	if (command == NULL || port_name == NULL || port_name[0] == L'\0') {
		if (error != NULL)
			*error = ERROR_INVALID_PARAMETER;
		return FALSE;
	}
	swprintf_s(monitor_object, UI_ARRAY_COUNT(monitor_object),
		L",XcvMonitor %ls", USBRELAY_PORT_MONITOR_NAME);
	if (!OpenPrinterW(monitor_object, &monitor, NULL)) {
		if (error != NULL)
			*error = GetLastError();
		return FALSE;
	}
	input_size = (DWORD)((wcslen(port_name) + 1) * sizeof(wchar_t));
	SetLastError(ERROR_SUCCESS);
	result = XcvDataW(monitor, command, (PBYTE)port_name, input_size,
		NULL, 0, &needed, &status);
	if (!result) {
		DWORD call_error = status != ERROR_SUCCESS ? status : GetLastError();

		if (call_error == ERROR_SUCCESS)
			call_error = ERROR_CAN_NOT_COMPLETE;
		if (error != NULL)
			*error = call_error;
		ClosePrinter(monitor);
		return FALSE;
	}
	ClosePrinter(monitor);
	if (error != NULL)
		*error = status;
	return TRUE;
}

static BOOL add_port(const wchar_t *port_name)
{
	DWORD error = ERROR_SUCCESS;

	if (call_monitor_xcv(L"AddPort", port_name, &error) &&
		(error == ERROR_SUCCESS || error == ERROR_ALREADY_EXISTS) &&
		port_name_in_spooler(port_name))
		return TRUE;
	if (port_name_in_spooler(port_name))
		return TRUE;
	if (error == ERROR_SUCCESS)
		error = ERROR_CAN_NOT_COMPLETE;
	SetLastError(error);
	return FALSE;
}

static BOOL local_port_exists(const wchar_t *port_name)
{
	HKEY key;
	LONG result;
	DWORD type = 0;
	DWORD size = 0;

	if (port_name_in_spooler(port_name))
		return TRUE;
	result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, CLIENT_PORT_REGISTRY_KEY, 0,
		KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
	if (result != ERROR_SUCCESS)
		result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, CLIENT_PORT_REGISTRY_KEY,
			0, KEY_QUERY_VALUE, &key);
	if (result != ERROR_SUCCESS)
		return FALSE;
	result = RegQueryValueExW(key, port_name, NULL, &type, NULL, &size);
	RegCloseKey(key);
	return result == ERROR_SUCCESS &&
		(type == REG_SZ || type == REG_EXPAND_SZ);
}

static BOOL add_local_port(RemotePrinter *printer, DWORD *error)
{
	wchar_t port_name[CLIENT_PORT_HOST_MAX + 32];

	if (!make_port_name(printer, port_name, UI_ARRAY_COUNT(port_name))) {
		if (error != NULL)
			*error = GetLastError();
		return FALSE;
	}
	if (!add_port(port_name)) {
		if (error != NULL)
			*error = GetLastError();
		return FALSE;
	}
	printer->port_exists = TRUE;
	return TRUE;
}

static BOOL find_local_queue_for_port(const wchar_t *port_name,
	wchar_t *queue_name, size_t queue_count)
{
	DWORD needed = 0;
	DWORD count = 0;
	BYTE *buffer = NULL;
	DWORD index;
	BOOL found = FALSE;

	if (queue_name != NULL && queue_count > 0)
		queue_name[0] = L'\0';
	EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, NULL, 0, &needed, &count);
	if (needed == 0)
		return FALSE;
	buffer = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
	if (buffer == NULL)
		return FALSE;
	if (EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, buffer, needed,
		&needed, &count)) {
		for (index = 0; index < count; index++) {
			PRINTER_INFO_2W *info = &((PRINTER_INFO_2W *)buffer)[index];

			if (info->pPortName == NULL || info->pPrinterName == NULL ||
				_wcsicmp(info->pPortName, port_name) != 0)
				continue;
			if (queue_name != NULL && queue_count > 0)
				wcsncpy_s(queue_name, queue_count, info->pPrinterName,
					_TRUNCATE);
			found = TRUE;
			break;
		}
	}
	HeapFree(GetProcessHeap(), 0, buffer);
	return found;
}

static BOOL delete_queues_for_port(const wchar_t *port_name, DWORD *error)
{
	DWORD needed = 0;
	DWORD count = 0;
	BYTE *buffer = NULL;
	DWORD index;
	DWORD first_error = ERROR_SUCCESS;
	BOOL success = TRUE;

	EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, NULL, 0, &needed, &count);
	if (needed == 0)
		return TRUE;
	buffer = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
	if (buffer == NULL) {
		if (error != NULL)
			*error = ERROR_NOT_ENOUGH_MEMORY;
		return FALSE;
	}
	if (!EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, buffer, needed,
		&needed, &count)) {
		if (error != NULL)
			*error = GetLastError();
		HeapFree(GetProcessHeap(), 0, buffer);
		return FALSE;
	}
	for (index = 0; index < count; index++) {
		PRINTER_INFO_2W *info = &((PRINTER_INFO_2W *)buffer)[index];
		HANDLE printer = NULL;
		DWORD delete_error;

		if (info->pPortName == NULL || info->pPrinterName == NULL ||
			_wcsicmp(info->pPortName, port_name) != 0)
			continue;
		if (!OpenPrinterW(info->pPrinterName, &printer, NULL)) {
			delete_error = GetLastError();
			if (first_error == ERROR_SUCCESS)
				first_error = delete_error;
			success = FALSE;
			continue;
		}
		if (!DeletePrinter(printer)) {
			delete_error = GetLastError();
			if (first_error == ERROR_SUCCESS)
				first_error = delete_error;
			success = FALSE;
		}
		ClosePrinter(printer);
	}
	HeapFree(GetProcessHeap(), 0, buffer);
	if (!success && error != NULL)
		*error = first_error;
	return success;
}

static BOOL delete_local_port(const wchar_t *port_name, DWORD *error)
{
	DWORD delete_error = ERROR_SUCCESS;

	if (call_monitor_xcv(L"DeletePort", port_name, &delete_error) &&
		delete_error == ERROR_SUCCESS)
		return TRUE;
	if (delete_error == ERROR_UNKNOWN_PORT ||
		delete_error == ERROR_FILE_NOT_FOUND)
		return TRUE;
	if (error != NULL)
		*error = delete_error;
	return FALSE;
}

static BOOL make_printer_cache_key(int index, wchar_t *key, size_t key_count)
{
	if (_snwprintf_s(key, key_count, _TRUNCATE,
		L"%ls\\Printers\\%d", CLIENT_REGISTRY_KEY, index) < 0) {
		key[0] = L'\0';
		return FALSE;
	}
	return TRUE;
}

static void save_printer_cache(ClientState *state)
{
	wchar_t key[CLIENT_REGISTRY_KEY_MAX];
	wchar_t timestamp[64];
	SYSTEMTIME local_time;
	int index;
	BOOL complete = TRUE;

	for (index = 0; index < state->printer_count; index++) {
		const RemotePrinter *printer = &state->remote_printers[index];

		if (!make_printer_cache_key(index, key, UI_ARRAY_COUNT(key))) {
			complete = FALSE;
			break;
		}
		if (!UiSetRegistryDword(HKEY_CURRENT_USER, key, L"Id",
				printer->id) ||
			!UiSetRegistryString(HKEY_CURRENT_USER, key, L"Name",
				printer->name) ||
			!UiSetRegistryString(HKEY_CURRENT_USER, key, L"Driver",
				printer->driver) ||
			!UiSetRegistryString(HKEY_CURRENT_USER, key, L"OriginalPort",
				printer->original_port) ||
			!UiSetRegistryString(HKEY_CURRENT_USER, key, L"Host",
				printer->host)) {
			complete = FALSE;
			break;
		}
	}
	if (!complete) {
		log_message(state, L"保存上次打印机状态失败，请检查当前用户注册表权限。");
		return;
	}

	GetLocalTime(&local_time);
	_snwprintf_s(timestamp, UI_ARRAY_COUNT(timestamp), _TRUNCATE,
		L"%04u-%02u-%02u %02u:%02u:%02u",
		(unsigned)local_time.wYear, (unsigned)local_time.wMonth,
		(unsigned)local_time.wDay, (unsigned)local_time.wHour,
		(unsigned)local_time.wMinute, (unsigned)local_time.wSecond);
	if (!UiSetRegistryDword(HKEY_CURRENT_USER, CLIENT_REGISTRY_KEY,
			L"PrinterCount", (DWORD)state->printer_count) ||
		!UiSetRegistryString(HKEY_CURRENT_USER, CLIENT_REGISTRY_KEY,
			L"LastRefresh", timestamp)) {
		log_message(state, L"保存上次刷新时间失败，请检查当前用户注册表权限。");
		return;
	}
	wcsncpy_s(state->last_refresh, UI_ARRAY_COUNT(state->last_refresh),
		timestamp, _TRUNCATE);
}

static BOOL load_printer_cache(ClientState *state)
{
	wchar_t key[CLIENT_REGISTRY_KEY_MAX];
	wchar_t default_host[CLIENT_PORT_HOST_MAX + 1];
	DWORD count;
	DWORD index;

	state->printer_count = 0;
	state->last_refresh[0] = L'\0';
	GetWindowTextW(state->server, default_host, UI_ARRAY_COUNT(default_host));
	UiTrimWhitespace(default_host);
	UiGetRegistryString(HKEY_CURRENT_USER, CLIENT_REGISTRY_KEY,
		L"LastRefresh", state->last_refresh,
		UI_ARRAY_COUNT(state->last_refresh));
	count = UiGetRegistryDword(HKEY_CURRENT_USER, CLIENT_REGISTRY_KEY,
		L"PrinterCount", 0);
	if (count > CLIENT_MAX_PRINTERS)
		count = CLIENT_MAX_PRINTERS;
	for (index = 0; index < count; index++) {
		RemotePrinter printer;

		ZeroMemory(&printer, sizeof(printer));
		if (!make_printer_cache_key((int)index, key, UI_ARRAY_COUNT(key)))
			break;
		printer.id = UiGetRegistryDword(HKEY_CURRENT_USER, key, L"Id", 0);
		UiGetRegistryString(HKEY_CURRENT_USER, key, L"Name", printer.name,
			UI_ARRAY_COUNT(printer.name));
		UiGetRegistryString(HKEY_CURRENT_USER, key, L"Driver", printer.driver,
			UI_ARRAY_COUNT(printer.driver));
		UiGetRegistryString(HKEY_CURRENT_USER, key, L"OriginalPort",
			printer.original_port, UI_ARRAY_COUNT(printer.original_port));
		UiGetRegistryString(HKEY_CURRENT_USER, key, L"Host", printer.host,
			UI_ARRAY_COUNT(printer.host));
		if (printer.id == 0 || printer.name[0] == L'\0')
			continue;
		if (printer.host[0] == L'\0')
			wcsncpy_s(printer.host, UI_ARRAY_COUNT(printer.host),
				default_host, _TRUNCATE);
		state->remote_printers[state->printer_count++] = printer;
	}
	return state->printer_count > 0;
}

static void update_printer_flags(ClientState *state)
{
	int index;
	for (index = 0; index < state->printer_count; index++) {
		RemotePrinter *printer = &state->remote_printers[index];
		wchar_t port_name[CLIENT_PORT_HOST_MAX + 32];

		if (make_port_name(printer, port_name, UI_ARRAY_COUNT(port_name))) {
			printer->port_exists = local_port_exists(port_name);
			printer->queue_exists = find_local_queue_for_port(port_name,
				printer->queue_name, UI_ARRAY_COUNT(printer->queue_name));
		}
		else {
			printer->port_exists = FALSE;
			printer->queue_exists = FALSE;
			printer->queue_name[0] = L'\0';
		}
	}
}

static void display_printers(ClientState *state)
{
	int index;

	ListView_DeleteAllItems(state->printer_list);
	for (index = 0; index < state->printer_count; index++) {
		RemotePrinter *printer = &state->remote_printers[index];
		LVITEMW item;
		wchar_t id[32];
		wchar_t status[192];

		_snwprintf_s(id, UI_ARRAY_COUNT(id), _TRUNCATE, L"%08lX",
			(unsigned long)printer->id);
		_snwprintf_s(status, UI_ARRAY_COUNT(status), _TRUNCATE,
			L"%ls；%ls",
			printer->port_exists ? L"端口已创建" : L"端口未创建",
			printer->queue_exists ? L"已绑定本地队列" : L"未绑定本地队列");
		ZeroMemory(&item, sizeof(item));
		item.mask = LVIF_TEXT | LVIF_PARAM;
		item.iItem = index;
		item.lParam = index;
		item.pszText = printer->name;
		ListView_InsertItem(state->printer_list, &item);
		ListView_SetItemText(state->printer_list, index, 1, printer->driver);
		ListView_SetItemText(state->printer_list, index, 2,
			printer->original_port);
		ListView_SetItemText(state->printer_list, index, 3, id);
		ListView_SetItemText(state->printer_list, index, 4, status);
	}
}

static int selected_printer_index(ClientState *state)
{
	int item = ListView_GetNextItem(state->printer_list, -1, LVNI_SELECTED);
	LVITEMW info;

	if (item < 0)
		return -1;
	ZeroMemory(&info, sizeof(info));
	info.mask = LVIF_PARAM;
	info.iItem = item;
	if (!ListView_GetItem(state->printer_list, &info) ||
		info.lParam < 0 || info.lParam >= state->printer_count)
		return -1;
	return (int)info.lParam;
}

static void refresh_printers(ClientState *state)
{
	wchar_t host[CLIENT_PORT_HOST_MAX + 1];

	GetWindowTextW(state->server, host, UI_ARRAY_COUNT(host));
	UiTrimWhitespace(host);
	if (!UiIsValidServerAddress(host)) {
		UiShowError(state->window, L"请输入有效的服务端 IP 地址或计算机名。");
		SetFocus(state->server);
		return;
	}
	SetWindowTextW(state->server, host);
	set_status(state, L"正在读取服务端打印机...");
	log_message(state, L"正在读取 %ls 的打印机列表。", host);
	if (!query_printers(state, host)) {
		int discovered_index = find_server_host(state, host);
		if (discovered_index >= 0 &&
			_wcsicmp(host, state->discovered_servers[discovered_index].address) != 0) {
			wchar_t fallback[CLIENT_PORT_HOST_MAX + 1];
			wcsncpy_s(fallback, UI_ARRAY_COUNT(fallback),
				state->discovered_servers[discovered_index].address, _TRUNCATE);
			if (query_printers(state, fallback)) {
				SetWindowTextW(state->server, fallback);
				log_message(state, L"计算机名连接失败，已回退到发现到的 IPv4 地址 %ls。",
					fallback);
				update_printer_flags(state);
				display_printers(state);
				save_printer_cache(state);
				set_status(state, L"服务端已连接，打印机列表已更新");
				log_message(state, L"已读取 %d 个服务端打印队列。", state->printer_count);
				return;
			}
		}
		if (state->printer_count > 0) {
			set_status(state, L"无法连接服务端，正在显示上次状态");
			log_message(state,
				L"读取失败，已保留上次获取的 %d 个打印队列。请确认服务端已运行且 TCP 3242 可访问。",
				state->printer_count);
		}
		else {
			set_status(state, L"无法连接服务端");
			log_message(state,
				L"读取失败，请确认服务端已运行且 TCP 3242 可访问。");
		}
		return;
	}
	update_printer_flags(state);
	display_printers(state);
	save_printer_cache(state);
	set_status(state, L"服务端已连接，打印机列表已更新");
	log_message(state, L"已读取 %d 个服务端打印队列。", state->printer_count);
}

static void create_selected_port(ClientState *state)
{
	int index = selected_printer_index(state);
	DWORD error = ERROR_SUCCESS;
	RemotePrinter *printer;
	wchar_t port_name[CLIENT_PORT_HOST_MAX + 32];

	if (index < 0) {
		UiShowError(state->window, L"请先选择服务端打印机。");
		return;
	}
	printer = &state->remote_printers[index];
	if (!add_local_port(printer, &error)) {
		log_message(state, L"创建本地端口失败，错误 %lu。", error);
		UiShowLastError(state->window, L"创建本地端口失败");
		return;
	}
	if (!make_port_name(printer, port_name, UI_ARRAY_COUNT(port_name))) {
		set_status(state, L"本地端口已创建");
		return;
	}
	display_printers(state);
	set_status(state, L"本地端口已创建，可从 Windows 添加打印机向导中选择");
	log_message(state,
		L"本地端口“%ls”已创建。请点击“打开添加打印机”，选择“添加本地打印机”，"
		L"再选择“使用现有端口”中的该 USBRELAY 端口。", port_name);
}

static void open_add_printer_wizard(ClientState *state)
{
	wchar_t rundll32[MAX_PATH];
	SHELLEXECUTEINFOW execute_info;

	if (!get_rundll32_path(rundll32, UI_ARRAY_COUNT(rundll32))) {
		UiShowLastError(state->window, L"找不到 Windows 打印机向导程序");
		return;
	}
	ZeroMemory(&execute_info, sizeof(execute_info));
	execute_info.cbSize = sizeof(execute_info);
	execute_info.hwnd = state->window;
	execute_info.lpVerb = L"open";
	execute_info.lpFile = rundll32;
	execute_info.lpParameters = L"printui.dll,PrintUIEntry /il";
	execute_info.nShow = SW_SHOWNORMAL;
	if (!ShellExecuteExW(&execute_info)) {
		UiShowLastError(state->window, L"打开 Windows 添加打印机向导失败");
		return;
	}
	if (execute_info.hProcess != NULL)
		CloseHandle(execute_info.hProcess);
	set_status(state, L"已打开 Windows 添加打印机向导");
	log_message(state, L"已打开 Windows 添加打印机向导。");
}

static void set_selected_default(ClientState *state)
{
	int index = selected_printer_index(state);
	RemotePrinter *printer;

	if (index < 0) {
		UiShowError(state->window, L"请先选择服务端打印机。");
		return;
	}
	printer = &state->remote_printers[index];
	if (!printer->queue_exists) {
		UiShowError(state->window,
			L"请先创建本地端口，再通过 Windows 添加打印机向导创建队列。");
		return;
	}
	if (!SetDefaultPrinterW(printer->queue_name)) {
		UiShowLastError(state->window, L"设置默认打印机失败");
		return;
	}
	log_message(state, L"已将“%ls”设为默认打印机。", printer->queue_name);
}

static void remove_selected_printer(ClientState *state)
{
	int index = selected_printer_index(state);
	DWORD error = ERROR_SUCCESS;
	RemotePrinter *printer;
	wchar_t port_name[CLIENT_PORT_HOST_MAX + 32];

	if (index < 0) {
		UiShowError(state->window, L"请先选择服务端打印机。");
		return;
	}
	printer = &state->remote_printers[index];
	if (!printer->port_exists && !printer->queue_exists) {
		log_message(state, L"所选打印机没有 USBRelay 本地端口或队列。");
		return;
	}
	if (!make_port_name(printer, port_name, UI_ARRAY_COUNT(port_name))) {
		UiShowLastError(state->window, L"生成本地端口名称失败");
		return;
	}
	if (!delete_queues_for_port(port_name, &error)) {
		log_message(state, L"删除使用端口“%ls”的本地队列失败，错误 %lu。",
			port_name, error);
		UiShowLastError(state->window, L"删除本地打印队列失败");
		return;
	}
	if (!delete_local_port(port_name, &error)) {
		log_message(state, L"删除本地端口“%ls”失败，错误 %lu。",
			port_name, error);
		UiShowLastError(state->window, L"删除本地端口失败");
		return;
	}
	printer->port_exists = FALSE;
	printer->queue_exists = FALSE;
	printer->queue_name[0] = L'\0';
	display_printers(state);
	log_message(state, L"已删除使用本地端口“%ls”的队列，并删除该端口。",
		port_name);
}

static BOOL is_usbrelay_port(const wchar_t *port_name)
{
	return port_name != NULL &&
		_wcsnicmp(port_name, USBRELAY_PORT_PREFIX,
			wcslen(USBRELAY_PORT_PREFIX)) == 0;
}

static void cleanup_local_queues(void)
{
	DWORD needed = 0;
	DWORD count = 0;
	BYTE *buffer = NULL;
	DWORD index;

	EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, NULL, 0, &needed, &count);
	if (needed == 0)
		return;
	buffer = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
	if (buffer == NULL)
		return;
	if (EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, buffer, needed,
		&needed, &count)) {
		for (index = 0; index < count; index++) {
			PRINTER_INFO_2W *info = &((PRINTER_INFO_2W *)buffer)[index];
			HANDLE printer = NULL;
			if (!is_usbrelay_port(info->pPortName) ||
				info->pPrinterName == NULL)
				continue;
			if (OpenPrinterW(info->pPrinterName, &printer, NULL)) {
				DeletePrinter(printer);
				ClosePrinter(printer);
			}
		}
	}
	HeapFree(GetProcessHeap(), 0, buffer);
}

static void cleanup_ports(void)
{
	HKEY key;
	DWORD index = 0;
	wchar_t names[128][512];
	DWORD count = 0;

	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, CLIENT_PORT_REGISTRY_KEY, 0,
		KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
		return;
	while (count < UI_ARRAY_COUNT(names)) {
		DWORD name_count = UI_ARRAY_COUNT(names[count]);
		LONG result = RegEnumValueW(key, index++, names[count], &name_count,
			NULL, NULL, NULL, NULL);
		if (result == ERROR_NO_MORE_ITEMS)
			break;
		if (result != ERROR_SUCCESS)
			continue;
		if (is_usbrelay_port(names[count]))
			count++;
	}
	RegCloseKey(key);
	for (index = 0; index < count; index++)
		DeletePortW(NULL, NULL, names[index]);
}

static int shortcut_command(HINSTANCE instance)
{
	wchar_t path[MAX_PATH];
	BOOL install = has_argument(L"/install-shortcuts");
	BOOL remove = has_argument(L"/remove-shortcuts");
	BOOL install_autostart = has_argument(L"/install-autostart");
	BOOL remove_autostart = has_argument(L"/remove-autostart");
	DWORD error = ERROR_SUCCESS;

	if (!install && !remove && !install_autostart && !remove_autostart)
		return -1;
	if (!GetModuleFileNameW(instance, path, UI_ARRAY_COUNT(path)))
		return 1;
	if (install_autostart)
		return UiCreateLogonTask(USBRELAY_CLIENT_TASK_NAME, path,
			L"/autostart", L"USBRelay 客户端开机启动并驻留托盘",
			&error) ? 0 : 1;
	if (remove_autostart)
		return UiDeleteLogonTask(USBRELAY_CLIENT_TASK_NAME,
			&error) ? 0 : 1;
	return install ?
		(UiInstallShortcuts(path, L"USBRelay 客户端",
			L"USBRelay 局域网打印客户端") ? 0 : 1) :
		(UiRemoveShortcuts(L"USBRelay 客户端") ? 0 : 1);
}

static void toggle_autostart(ClientState *state)
{
	LRESULT checked = SendMessageW(state->autostart, BM_GETCHECK, 0, 0);
	DWORD error = ERROR_SUCCESS;
	BOOL success;

	if (checked == BST_CHECKED) {
		success = UiCreateLogonTask(USBRELAY_CLIENT_TASK_NAME,
			state->executable, L"/autostart",
			L"USBRelay 客户端开机启动并驻留托盘", &error);
	}
	else {
		success = UiDeleteLogonTask(USBRELAY_CLIENT_TASK_NAME, &error);
	}
	if (!success) {
		SendMessageW(state->autostart, BM_SETCHECK,
			checked == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
		log_message(state, L"更新开机启动失败，错误 %lu。", error);
		return;
	}
	log_message(state, checked == BST_CHECKED ?
		L"已启用客户端开机启动。" : L"已关闭客户端开机启动。");
}

static void layout(ClientState *state)
{
	RECT rect;
	int width;
	int height;
	int list_height;

	GetClientRect(state->window, &rect);
	width = rect.right - rect.left;
	height = rect.bottom - rect.top;
	if (width < 800) width = 800;
	if (height < 620) height = 620;
	MoveWindow(state->status, 14, 12, width - 28, 24, TRUE);
	MoveWindow(state->discovered_label, 14, 44, 104, 24, TRUE);
	MoveWindow(state->discovered, 122, 41, width - 450, 200, TRUE);
	MoveWindow(state->server_label, 14, 78, 104, 24, TRUE);
	MoveWindow(state->server, 122, 75, width - 450, 26, TRUE);
	MoveWindow(state->refresh, width - 310, 74, 116, 28, TRUE);
	MoveWindow(state->autostart, width - 180, 76, 166, 24, TRUE);
	MoveWindow(state->add, width - 310, 110, 116, 28, TRUE);
	MoveWindow(state->open_printer, width - 180, 110, 166, 28, TRUE);
	MoveWindow(state->remove, width - 310, 144, 116, 28, TRUE);
	MoveWindow(state->default_printer, width - 180, 144, 166, 28, TRUE);
	list_height = height - 312;
	if (list_height < 220) list_height = 220;
	MoveWindow(state->printer_list, 14, 218, width - 28, list_height, TRUE);
	MoveWindow(state->log, 14, 230 + list_height, width - 28,
		height - list_height - 242, TRUE);
}

static BOOL create_controls(ClientState *state)
{
	LVCOLUMNW column;

	state->status = make_control(state, L"STATIC", L"正在初始化发现...",
		SS_LEFT, 0, 14, 12, 760, 24, IDC_STATUS);
	state->discovered_label = make_control(state, L"STATIC",
		L"已发现服务端：", SS_LEFT, 0, 14, 44, 104, 24,
		IDC_DISCOVERED_LABEL);
	state->discovered = make_control(state, L"COMBOBOX", L"",
		CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
		WS_EX_CLIENTEDGE, 122, 41, 380, 200, IDC_DISCOVERED);
	state->server_label = make_control(state, L"STATIC",
		L"地址/计算机名：", SS_LEFT, 0, 14, 78, 104, 24,
		IDC_SERVER_LABEL);
	state->server = make_control(state, L"EDIT", L"",
		ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 122, 75, 380, 26,
		IDC_SERVER);
	state->refresh = make_control(state, L"BUTTON", L"刷新打印机",
		BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 640, 74, 116, 28, IDC_REFRESH);
	state->autostart = make_control(state, L"BUTTON",
		L"开机启动并驻留托盘", BS_AUTOCHECKBOX | WS_TABSTOP, 0,
		770, 76, 166, 24, IDC_AUTOSTART);
	state->add = make_control(state, L"BUTTON", L"创建本地端口",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 640, 110, 116, 28, IDC_ADD);
	state->open_printer = make_control(state, L"BUTTON", L"打开添加打印机",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 770, 110, 166, 28,
		IDC_OPEN_PRINTER);
	state->default_printer = make_control(state, L"BUTTON", L"设为默认",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 770, 144, 166, 28, IDC_DEFAULT);
	state->remove = make_control(state, L"BUTTON", L"删除端口/队列",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 640, 144, 116, 28, IDC_REMOVE);
	state->printer_list = make_control(state, WC_LISTVIEWW, L"",
		LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
		WS_EX_CLIENTEDGE, 14, 218, 900, 300, IDC_PRINTERS);
	state->log = make_control(state, L"EDIT", L"",
		ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
		WS_EX_CLIENTEDGE, 14, 530, 900, 100, IDC_LOG);
	if (state->status == NULL || state->discovered_label == NULL ||
		state->discovered == NULL || state->server_label == NULL ||
		state->server == NULL || state->refresh == NULL ||
		state->autostart == NULL || state->printer_list == NULL ||
		state->add == NULL || state->open_printer == NULL ||
		state->default_printer == NULL || state->remove == NULL ||
		state->log == NULL)
		return FALSE;
	SendMessageW(state->server, EM_SETLIMITTEXT, CLIENT_PORT_HOST_MAX, 0);
	ListView_SetExtendedListViewStyle(state->printer_list,
		LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
	ZeroMemory(&column, sizeof(column));
	column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
	column.cx = 230; column.pszText = L"服务端打印机";
	ListView_InsertColumn(state->printer_list, 0, &column);
	column.cx = 220; column.pszText = L"驱动";
	ListView_InsertColumn(state->printer_list, 1, &column);
	column.cx = 140; column.pszText = L"服务端原端口";
	ListView_InsertColumn(state->printer_list, 2, &column);
	column.cx = 100; column.pszText = L"发布 ID";
	ListView_InsertColumn(state->printer_list, 3, &column);
	column.cx = 320; column.pszText = L"客户端状态";
	ListView_InsertColumn(state->printer_list, 4, &column);
	return TRUE;
}

static void load_preferences(ClientState *state)
{
	wchar_t value[CLIENT_PORT_HOST_MAX + 1];
	if (UiGetRegistryString(HKEY_CURRENT_USER, CLIENT_REGISTRY_KEY,
		L"Server", value, UI_ARRAY_COUNT(value)))
		SetWindowTextW(state->server, value);
	if (UiLogonTaskExists(USBRELAY_CLIENT_TASK_NAME, NULL))
		SendMessageW(state->autostart, BM_SETCHECK, BST_CHECKED, 0);
}

static void save_preferences(ClientState *state)
{
	wchar_t host[CLIENT_PORT_HOST_MAX + 1];
	GetWindowTextW(state->server, host, UI_ARRAY_COUNT(host));
	UiTrimWhitespace(host);
	UiSetRegistryString(HKEY_CURRENT_USER, CLIENT_REGISTRY_KEY,
		L"Server", host);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message,
	WPARAM w_param, LPARAM l_param)
{
	ClientState *state;
	wchar_t initial_host[CLIENT_PORT_HOST_MAX + 1];
	wchar_t initial_status[256];

	if (message == WM_NCCREATE) {
		CREATESTRUCTW *create = (CREATESTRUCTW *)l_param;
		state = (ClientState *)create->lpCreateParams;
		state->window = window;
		SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)state);
		return TRUE;
	}
	state = state_from_window(window);
	switch (message) {
	case CLIENT_PROBE_RESULT_MESSAGE:
		if (state != NULL)
			merge_probe_result(state, (const ProbeResult *)l_param);
		HeapFree(GetProcessHeap(), 0, (void *)l_param);
		return 0;
	case WM_CREATE:
		if (state == NULL || !create_controls(state))
			return -1;
		layout(state);
		load_preferences(state);
		if (load_printer_cache(state)) {
			update_printer_flags(state);
			display_printers(state);
			if (state->last_refresh[0] != L'\0') {
				_snwprintf_s(initial_status,
					UI_ARRAY_COUNT(initial_status), _TRUNCATE,
					L"已加载上次状态，正在自动刷新（上次更新：%ls）",
					state->last_refresh);
				set_status(state, initial_status);
			}
			else {
				set_status(state, L"已加载上次状态，正在自动刷新...");
			}
			log_message(state,
				L"已加载上次保存的 %d 个服务端打印队列，正在自动更新。",
				state->printer_count);
		}
		else {
			set_status(state, L"正在接收服务端广播...");
		}
		GetWindowTextW(state->server, initial_host,
			UI_ARRAY_COUNT(initial_host));
		UiTrimWhitespace(initial_host);
		if (UiIsValidServerAddress(initial_host))
			state->startup_refresh_countdown = 1;
		state->next_active_probe = GetTickCount() +
			CLIENT_ACTIVE_PROBE_START_DELAY_MS;
		state->next_full_probe = GetTickCount();
		SetTimer(window, CLIENT_TIMER, 1000, NULL);
		return 0;
	case WM_SIZE:
		layout(state);
		return 0;
	case WM_GETMINMAXINFO:
		((MINMAXINFO *)l_param)->ptMinTrackSize.x = 800;
		((MINMAXINFO *)l_param)->ptMinTrackSize.y = 620;
		return 0;
	case WM_COMMAND:
		if (state == NULL)
			return 0;
		if (LOWORD(w_param) == UI_TRAY_COMMAND_OPEN) {
			show_window(window);
			return 0;
		}
		if (LOWORD(w_param) == UI_TRAY_COMMAND_EXIT) {
			state->exiting = TRUE;
			DestroyWindow(window);
			return 0;
		}
		switch (LOWORD(w_param)) {
		case IDC_REFRESH:
			if (HIWORD(w_param) == BN_CLICKED)
				refresh_printers(state);
			return 0;
		case IDC_ADD:
			if (HIWORD(w_param) == BN_CLICKED)
				create_selected_port(state);
			return 0;
		case IDC_OPEN_PRINTER:
			if (HIWORD(w_param) == BN_CLICKED)
				open_add_printer_wizard(state);
			return 0;
		case IDC_DEFAULT:
			if (HIWORD(w_param) == BN_CLICKED)
				set_selected_default(state);
			return 0;
		case IDC_REMOVE:
			if (HIWORD(w_param) == BN_CLICKED)
				remove_selected_printer(state);
			return 0;
		case IDC_AUTOSTART:
			if (HIWORD(w_param) == BN_CLICKED)
				toggle_autostart(state);
			return 0;
		case IDC_DISCOVERED:
			if (HIWORD(w_param) == CBN_SELCHANGE &&
				!state->updating_discovered) {
				int selected = (int)SendMessageW(state->discovered,
					CB_GETCURSEL, 0, 0);
				if (selected >= 0) {
					int index = (int)SendMessageW(state->discovered,
						CB_GETITEMDATA, selected, 0);
					select_server(state, index, TRUE);
				}
			}
			return 0;
		case IDC_SERVER:
			if (HIWORD(w_param) == EN_CHANGE)
				save_preferences(state);
			return 0;
		default:
			break;
		}
		break;
	case WM_NOTIFY:
		if (state != NULL) {
			NMHDR *header = (NMHDR *)l_param;
			if (header->idFrom == IDC_PRINTERS &&
				header->code == NM_DBLCLK) {
				create_selected_port(state);
				return 0;
			}
		}
		break;
	case WM_TIMER:
		if (w_param == CLIENT_TIMER) {
			DWORD now = GetTickCount();

			pump_discovery(state);
			prune_discovered(state);
			if ((LONG)(now - state->next_active_probe) >= 0) {
				BOOL full_scan =
					(LONG)(now - state->next_full_probe) >= 0;

				if (start_active_probe(state, full_scan)) {
					state->next_active_probe = now +
						CLIENT_ACTIVE_PROBE_INTERVAL_MS;
					if (full_scan)
						state->next_full_probe = now +
							CLIENT_ACTIVE_PROBE_FULL_INTERVAL_MS;
				}
			}
			if (state->startup_refresh_countdown > 0) {
				state->startup_refresh_countdown--;
				if (state->startup_refresh_countdown == 0) {
					GetWindowTextW(state->server, initial_host,
						UI_ARRAY_COUNT(initial_host));
					UiTrimWhitespace(initial_host);
					if (UiIsValidServerAddress(initial_host))
						refresh_printers(state);
				}
			}
		}
		return 0;
	case UI_TRAY_CALLBACK_MESSAGE:
		if (LOWORD(l_param) == WM_LBUTTONDBLCLK)
			show_window(window);
		else if (LOWORD(l_param) == WM_RBUTTONUP ||
			LOWORD(l_param) == WM_CONTEXTMENU)
			UiTrayShowContextMenu(window);
		return 0;
	case WM_CLOSE:
		if (!state->exiting) {
			ShowWindow(window, SW_HIDE);
			return 0;
		}
		DestroyWindow(window);
		return 0;
	case WM_DESTROY:
		KillTimer(window, CLIENT_TIMER);
		UiTrayRemove(window);
		InterlockedExchange(&state->active_probe_stop, 1);
		if (state->active_probe_thread != NULL) {
			WaitForSingleObject(state->active_probe_thread, INFINITE);
			CloseHandle(state->active_probe_thread);
			state->active_probe_thread = NULL;
		}
		if (state->discovery_socket != INVALID_SOCKET) {
			closesocket(state->discovery_socket);
			state->discovery_socket = INVALID_SOCKET;
		}
		PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(window, message, w_param, l_param);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous,
	wchar_t *command_line, int show_command)
{
	WNDCLASSEXW window_class;
	INITCOMMONCONTROLSEX controls;
	ClientState state;
	MSG message;
	HANDLE mutex;
	HWND existing;
	WSADATA wsa_data;
	int shortcut;

	UNREFERENCED_PARAMETER(previous);
	UNREFERENCED_PARAMETER(command_line);
	shortcut = shortcut_command(instance);
	if (shortcut >= 0)
		return shortcut;
	if (has_argument(L"/cleanup")) {
		cleanup_local_queues();
		cleanup_ports();
		return 0;
	}
	mutex = CreateMutexW(NULL, TRUE, L"Local\\USBRelay-Client-UI");
	if (mutex == NULL)
		return 1;
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		existing = FindWindowW(CLIENT_WINDOW_CLASS, NULL);
		if (existing != NULL)
			show_window(existing);
		CloseHandle(mutex);
		return 0;
	}
	ZeroMemory(&state, sizeof(state));
	state.discovery_socket = INVALID_SOCKET;
	state.autostart_mode = has_argument(L"/autostart");
	state.font = UiCreateInterfaceFont();
	if (state.font == NULL)
		goto fail_mutex;
	if (!GetModuleFileNameW(instance, state.executable,
		UI_ARRAY_COUNT(state.executable)))
		goto fail_font;
	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
		goto fail_font;
	state.winsock_started = TRUE;
	if (!make_discovery_socket(&state))
		log_message(&state, L"无法监听 UDP 3241，自动发现不可用。请直接输入计算机名。");
	controls.dwSize = sizeof(controls);
	controls.dwICC = ICC_LISTVIEW_CLASSES;
	if (!InitCommonControlsEx(&controls))
		goto fail_winsock;
	ZeroMemory(&window_class, sizeof(window_class));
	window_class.cbSize = sizeof(window_class);
	window_class.style = CS_HREDRAW | CS_VREDRAW;
	window_class.lpfnWndProc = window_proc;
	window_class.hInstance = instance;
	window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
	window_class.hIconSm = (HICON)LoadImageW(instance,
		MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
		LR_DEFAULTCOLOR);
	window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
	window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	window_class.lpszClassName = CLIENT_WINDOW_CLASS;
	if (!RegisterClassExW(&window_class))
		goto fail_winsock;
	state.window = CreateWindowExW(0, CLIENT_WINDOW_CLASS,
		CLIENT_WINDOW_TITLE, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 980, 700, NULL, NULL, instance,
		&state);
	if (state.window == NULL)
		goto fail_winsock;
	UiCenterWindow(state.window);
	state.tray_icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
	state.tray_added = state.tray_icon != NULL && UiTrayAdd(state.window,
		state.tray_icon, L"USBRelay 客户端 - 双击打开");
	if (state.autostart_mode)
		ShowWindow(state.window, SW_HIDE);
	else
		ShowWindow(state.window, show_command == SW_HIDE ?
			SW_SHOWNORMAL : show_command);
	UpdateWindow(state.window);
	while (GetMessageW(&message, NULL, 0, 0) > 0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	if (state.discovery_socket != INVALID_SOCKET)
		closesocket(state.discovery_socket);
	if (state.winsock_started)
		WSACleanup();
	DeleteObject(state.font);
	CloseHandle(mutex);
	return (int)message.wParam;

fail_winsock:
	if (state.discovery_socket != INVALID_SOCKET)
		closesocket(state.discovery_socket);
	if (state.winsock_started)
		WSACleanup();
fail_font:
	DeleteObject(state.font);
fail_mutex:
	CloseHandle(mutex);
	return 1;
}
