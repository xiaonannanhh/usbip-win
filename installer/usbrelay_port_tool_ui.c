#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tcpxcv.h>
#include <winspool.h>

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <wchar.h>

#include "../include/usbrelay_standard_protocol.h"
#include "ui_common.h"
#include "ui_resource.h"

#define PORT_TOOL_WINDOW_CLASS L"USBRelay-Standard-Port-Tool"
#define PORT_TOOL_WINDOW_TITLE L"打印机内网共享 - 配置工具端"
#define PORT_TOOL_MUTEX_NAME L"Local\\USBRelay-Standard-Port-Tool-UI"
#define PORT_TOOL_TASK_NAME L"USBRelay-Standard-Port-Tool-Autostart"
#define PORT_TOOL_SHORTCUT_NAME L"打印机内网共享配置工具端"
#define PORT_TOOL_MESSAGE_TITLE L"打印机内网共享"

#define PORT_TOOL_MAX_SERVERS 64
#define PORT_TOOL_MAX_PRINTERS 1024
#define PORT_TOOL_MAX_LOCAL_PORTS 2048
#define PORT_TOOL_MAX_PACKET_FIELDS 32
#define PORT_TOOL_CACHE_VERSION 1

#define PORT_TOOL_TIMER_DISCOVERY 1
#define PORT_TOOL_TIMER_CACHE 2

#define PORT_TOOL_IDC_STATUS 3001
#define PORT_TOOL_IDC_SERVERS 3002
#define PORT_TOOL_IDC_MANUAL_HOST 3003
#define PORT_TOOL_IDC_CONNECT 3004
#define PORT_TOOL_IDC_REFRESH 3005
#define PORT_TOOL_IDC_AUTOSTART 3006
#define PORT_TOOL_IDC_PRINTERS 3007
#define PORT_TOOL_IDC_ADD_PORT 3008
#define PORT_TOOL_IDC_DELETE_PORT 3009
#define PORT_TOOL_IDC_OPEN_PRINTERS 3010
#define PORT_TOOL_IDC_EXIT 3011
#define PORT_TOOL_IDC_LOG 3012
#define PORT_TOOL_IDC_CLEAR_CACHE 3013
#define PORT_TOOL_IDC_TEST_CONNECTION 3014

#define PORT_TOOL_MESSAGE_TEST_COMPLETE (WM_APP + 1)
#define PORT_TOOL_MESSAGE_DNS_CHECK_COMPLETE (WM_APP + 2)
#define PORT_TOOL_MESSAGE_PORT_REFRESH_COMPLETE (WM_APP + 3)
#define PORT_TOOL_MESSAGE_PORT_REFRESH_NO_MEMORY (WM_APP + 4)
#define PORT_TOOL_PORT_REFRESH_RETRY_MS 30000
#define PORT_TOOL_PORT_PROBE_TIMEOUT_MS 2000

typedef struct PortToolServer {
	wchar_t name[128];
	wchar_t address[64];
	wchar_t dns_checked_address[64];
	int address_score;
	DWORD address_if_index;
	DWORD printer_count;
	DWORD last_seen;
	BOOL cached;
} PortToolServer;

typedef struct PortToolPrinter {
	wchar_t server_name[128];
	wchar_t address[64];
	wchar_t printer_name[256];
	wchar_t driver[256];
	wchar_t original_port[128];
	wchar_t endpoint[384];
	unsigned int raw_port;
	DWORD last_seen;
	BOOL online;
	BOOL cached;
	BOOL port_refresh_pending;
	DWORD last_port_refresh_tick;
	LONG port_refresh_generation;
	wchar_t last_port_refresh_address[64];
} PortToolPrinter;

typedef struct PortToolState {
	HWND window;
	HWND status;
	HWND servers;
	HWND manual_host;
	HWND connect;
	HWND refresh;
	HWND autostart;
	HWND printers;
	HWND add_port;
	HWND delete_port;
	HWND open_printers;
	HWND clear_cache;
	HWND test_connection;
	HWND exit_button;
	HWND log;
	HFONT font;
	HICON tray_icon;
	BOOL tray_added;
	BOOL exiting;
	BOOL hidden_start;
	BOOL updating_servers;
	BOOL updating_printers;
	BOOL cache_dirty;
	BOOL winsock_ready;
	volatile LONG port_refresh_shutdown;
	volatile LONG active_port_refresh_workers;
	HANDLE port_refresh_idle_event;
	HANDLE port_refresh_stop_event;
	CRITICAL_SECTION port_refresh_lock;
	BOOL port_refresh_lock_ready;
	HANDLE test_thread;
	HANDLE test_cancel_event;
	unsigned int test_port;
	wchar_t test_host[128];
	SOCKET discovery_socket;
	unsigned int selected_server;
	int selected_printer;
	BOOL selected_printer_valid;
	wchar_t selected_printer_server[128];
	wchar_t selected_printer_name[256];
	unsigned int selected_printer_raw_port;
	PortToolServer server_items[PORT_TOOL_MAX_SERVERS];
	DWORD server_count;
	PortToolPrinter printer_items[PORT_TOOL_MAX_PRINTERS];
	DWORD printer_count;
	int visible_printer_indices[PORT_TOOL_MAX_PRINTERS];
	DWORD visible_printer_count;
	wchar_t local_ports[PORT_TOOL_MAX_LOCAL_PORTS][MAX_PORTNAME_LEN];
	DWORD local_port_count;
} PortToolState;

typedef struct ConnectionTestArgs {
	HWND window;
	HANDLE cancel_event;
	wchar_t host[128];
	wchar_t address[64];
	unsigned int port;
} ConnectionTestArgs;

typedef struct ConnectionTestResult {
	HWND window;
	wchar_t host[128];
	wchar_t address[64];
	wchar_t resolved[64];
	unsigned int port;
	int lookup_error;
	int socket_error;
	BOOL connected;
	BOOL cancelled;
	BOOL timed_out;
} ConnectionTestResult;

typedef struct ServerDnsCheckArgs {
	HWND window;
	wchar_t server_name[128];
	wchar_t advertised_address[64];
} ServerDnsCheckArgs;

typedef struct ServerDnsCheckResult {
	HWND window;
	wchar_t server_name[128];
	wchar_t advertised_address[64];
	wchar_t resolved_address[64];
	int lookup_error;
	BOOL matches_advertised_address;
} ServerDnsCheckResult;

typedef struct PortRefreshArgs {
	PortToolState *state;
	HWND window;
	wchar_t server_name[128];
	wchar_t printer_name[256];
	wchar_t address[64];
	unsigned int raw_port;
	DWORD printer_index;
	LONG generation;
} PortRefreshArgs;

typedef struct PortRefreshResult {
	HWND window;
	wchar_t server_name[128];
	wchar_t printer_name[256];
	wchar_t address[64];
	unsigned int raw_port;
	DWORD printer_index;
	LONG generation;
	DWORD matched_ports;
	DWORD updated_ports;
	DWORD unchanged_ports;
	DWORD failed_ports;
	DWORD error;
	DWORD rollback_error;
	BOOL connected;
	BOOL cancelled;
} PortRefreshResult;

typedef BOOL (WINAPI *DnsFlushResolverCacheFn)(VOID);

typedef struct PacketField {
	char key[40];
	const char *value;
} PacketField;

static PortToolState g_state;

static void refresh_server_list(PortToolState *state);
static void refresh_printer_list(PortToolState *state);
static const PortToolPrinter *selected_printer(PortToolState *state);
static void schedule_port_refresh(PortToolState *state,
	PortToolPrinter *printer);
static void finish_port_refresh(PortToolState *state,
	PortRefreshResult *result);
static DWORD elapsed_ms(DWORD now, DWORD then);
static BOOL port_refresh_request_is_current(
	const PortRefreshArgs *arguments);
static void sanitize_port_component(const wchar_t *source,
	wchar_t *destination, size_t destination_count);

static PortToolState *get_state(HWND window)
{
	return (PortToolState *)GetWindowLongPtrW(window, GWLP_USERDATA);
}

static BOOL has_argument(const wchar_t *wanted)
{
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	int index;
	BOOL found = FALSE;

	if (argv == NULL) {
		return FALSE;
	}
	for (index = 1; index < argc; index++) {
		if (_wcsicmp(argv[index], wanted) == 0) {
			found = TRUE;
			break;
		}
	}
	LocalFree(argv);
	return found;
}

static wchar_t *argument_after(const wchar_t *wanted)
{
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	wchar_t *value = NULL;
	int index;

	if (argv == NULL) {
		return NULL;
	}
	for (index = 1; index + 1 < argc; index++) {
		if (_wcsicmp(argv[index], wanted) == 0) {
			value = argv[index + 1];
			break;
		}
	}
	if (value != NULL) {
		value = _wcsdup(value);
	}
	LocalFree(argv);
	return value;
}

static void show_window(HWND window)
{
	ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
	SetForegroundWindow(window);
}

static void log_text(PortToolState *state, const wchar_t *format, ...)
{
	wchar_t message[2048];
	wchar_t line[2100];
	va_list args;
	int length;

	va_start(args, format);
	_vsnwprintf_s(message, UI_ARRAY_COUNT(message), _TRUNCATE, format, args);
	va_end(args);
	_snwprintf_s(line, UI_ARRAY_COUNT(line), _TRUNCATE, L"%ls\r\n", message);

	length = GetWindowTextLengthW(state->log);
	if (length > 60000) {
		SetWindowTextW(state->log, L"");
		length = 0;
	}
	SendMessageW(state->log, EM_SETSEL, length, length);
	SendMessageW(state->log, EM_REPLACESEL, FALSE, (LPARAM)line);
}

static BOOL is_ipv4_address(const wchar_t *text)
{
	IN_ADDR address;

	return text != NULL && text[0] != L'\0' &&
		InetPtonW(AF_INET, text, &address) == 1;
}

static BOOL is_usable_server_ipv4(const wchar_t *text)
{
	IN_ADDR address;
	unsigned long host_address;

	if (!is_ipv4_address(text)) {
		return FALSE;
	}
	InetPtonW(AF_INET, text, &address);
	host_address = ntohl(address.S_un.S_addr);
	return host_address != INADDR_ANY &&
		(host_address >> 24) != 127 &&
		(host_address >> 28) != 14 &&
		(host_address >> 16) != 0xa9fe &&
		host_address != INADDR_BROADCAST;
}

static int server_address_score(const wchar_t *text, DWORD *if_index)
{
	IN_ADDR destination;
	MIB_IPFORWARDROW route;
	IP_ADAPTER_ADDRESSES *adapters = NULL;
	IP_ADAPTER_ADDRESSES *adapter;
	ULONG size = 0;
	ULONG result;
	int score = 0;
	BOOL route_found = FALSE;

	if (if_index != NULL) {
		*if_index = 0;
	}
	if (!is_usable_server_ipv4(text) ||
		InetPtonW(AF_INET, text, &destination) != 1) {
		return INT_MIN;
	}
	ZeroMemory(&route, sizeof(route));
	if (GetBestRoute(destination.S_un.S_addr, 0, &route) != NO_ERROR) {
		return INT_MIN;
	}
	route_found = TRUE;
	if (if_index != NULL) {
		*if_index = route.dwForwardIfIndex;
	}
	score = 100000;
	if (route.dwForwardNextHop == 0) {
		score += 1000000;
	}
	score -= (int)(route.dwForwardMetric1 > 50000 ? 50000 :
		route.dwForwardMetric1);
	result = GetAdaptersAddresses(AF_INET,
		GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
			GAA_FLAG_SKIP_DNS_SERVER,
		NULL, NULL, &size);
	if (result != ERROR_BUFFER_OVERFLOW || size == 0) {
		return route_found ? score : INT_MIN;
	}
	adapters = (IP_ADAPTER_ADDRESSES *)HeapAlloc(GetProcessHeap(),
		HEAP_ZERO_MEMORY, size);
	if (adapters == NULL) {
		return score;
	}
	result = GetAdaptersAddresses(AF_INET,
		GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
			GAA_FLAG_SKIP_DNS_SERVER,
		NULL, adapters, &size);
	if (result == ERROR_SUCCESS) {
		for (adapter = adapters; adapter != NULL;
			adapter = adapter->Next) {
			IP_ADAPTER_GATEWAY_ADDRESS *gateway;

			if (adapter->IfIndex != route.dwForwardIfIndex) {
				continue;
			}
			if (adapter->OperStatus == IfOperStatusUp) {
				score += 10000;
			}
			score -= (int)(adapter->Ipv4Metric > 50000 ? 50000 :
				adapter->Ipv4Metric);
			for (gateway = adapter->FirstGatewayAddress;
				gateway != NULL; gateway = gateway->Next) {
				const struct sockaddr_in *gateway_address;

				if (gateway->Address.lpSockaddr == NULL ||
					gateway->Address.lpSockaddr->sa_family != AF_INET) {
					continue;
				}
				gateway_address = (const struct sockaddr_in *)
					gateway->Address.lpSockaddr;
				if (gateway_address->sin_addr.s_addr !=
					htonl(INADDR_ANY)) {
					score += 20000;
					break;
				}
			}
			break;
		}
	}
	HeapFree(GetProcessHeap(), 0, adapters);
	return score;
}

static BOOL resolve_server_ipv4(const wchar_t *host, wchar_t *address_text,
	size_t address_text_count)
{
	WSADATA winsock_data;
	ADDRINFOW hints;
	ADDRINFOW *addresses = NULL;
	ADDRINFOW *address;
	int best_score = INT_MIN;
	IN_ADDR best_address;
	BOOL found = FALSE;

	if (host == NULL || host[0] == L'\0' || address_text == NULL ||
		address_text_count == 0) {
		return FALSE;
	}
	if (is_usable_server_ipv4(host)) {
		return wcsncpy_s(address_text, address_text_count, host,
			_TRUNCATE) == 0;
	}
	if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
		return FALSE;
	}
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	if (GetAddrInfoW(host, L"9100", &hints, &addresses) == 0) {
		for (address = addresses; address != NULL;
			address = address->ai_next) {
			const struct sockaddr_in *candidate;
			int score;

			if (address->ai_addr == NULL ||
				address->ai_addrlen < sizeof(struct sockaddr_in) ||
				address->ai_family != AF_INET) {
				continue;
			}
			candidate = (const struct sockaddr_in *)address->ai_addr;
			{
				wchar_t candidate_text[64];

				if (InetNtopW(AF_INET,
					(void *)&candidate->sin_addr, candidate_text,
					(DWORD)UI_ARRAY_COUNT(candidate_text)) == NULL) {
					continue;
				}
				score = server_address_score(candidate_text, NULL);
			}
			if (score > best_score) {
				best_score = score;
				best_address = candidate->sin_addr;
				found = TRUE;
			}
		}
	}
	if (addresses != NULL) {
		FreeAddrInfoW(addresses);
	}
	WSACleanup();
	if (!found || InetNtopW(AF_INET, &best_address, address_text,
		(DWORD)address_text_count) == NULL) {
		return FALSE;
	}
	return TRUE;
}

static BOOL flush_local_dns_cache(void)
{
	HMODULE dnsapi = LoadLibraryW(L"dnsapi.dll");
	DnsFlushResolverCacheFn flush_cache;
	BOOL flushed;

	if (dnsapi == NULL) {
		return FALSE;
	}
	flush_cache = (DnsFlushResolverCacheFn)GetProcAddress(dnsapi,
		"DnsFlushResolverCache");
	if (flush_cache == NULL) {
		FreeLibrary(dnsapi);
		return FALSE;
	}
	flushed = flush_cache();
	FreeLibrary(dnsapi);
	return flushed;
}

static DWORD WINAPI server_dns_check_thread(void *parameter)
{
	ServerDnsCheckArgs *arguments = (ServerDnsCheckArgs *)parameter;
	ServerDnsCheckResult *result;
	WSADATA winsock_data;
	ADDRINFOW hints;
	ADDRINFOW *addresses = NULL;
	ADDRINFOW *address;
	int best_score = INT_MIN;
	int winsock_error;
	IN_ADDR advertised;
	BOOL winsock_ready = FALSE;

	result = (ServerDnsCheckResult *)HeapAlloc(GetProcessHeap(),
		HEAP_ZERO_MEMORY, sizeof(*result));
	if (result == NULL) {
		HeapFree(GetProcessHeap(), 0, arguments);
		return 0;
	}
	result->window = arguments->window;
	wcsncpy_s(result->server_name, UI_ARRAY_COUNT(result->server_name),
		arguments->server_name, _TRUNCATE);
	wcsncpy_s(result->advertised_address,
		UI_ARRAY_COUNT(result->advertised_address),
		arguments->advertised_address, _TRUNCATE);

	winsock_error = WSAStartup(MAKEWORD(2, 2), &winsock_data);
	if (winsock_error != 0) {
		result->lookup_error = winsock_error;
		goto complete;
	}
	winsock_ready = TRUE;
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	result->lookup_error = GetAddrInfoW(arguments->server_name, NULL,
		&hints, &addresses);
	if (result->lookup_error == 0 &&
		InetPtonW(AF_INET, arguments->advertised_address, &advertised) == 1) {
		for (address = addresses; address != NULL;
			address = address->ai_next) {
			const struct sockaddr_in *candidate;
			wchar_t candidate_text[64];
			int score;

			if (address->ai_family != AF_INET || address->ai_addr == NULL ||
				address->ai_addrlen < sizeof(struct sockaddr_in)) {
				continue;
			}
			candidate = (const struct sockaddr_in *)address->ai_addr;
			if (InetNtopW(AF_INET, (void *)&candidate->sin_addr,
				candidate_text,
				(DWORD)UI_ARRAY_COUNT(candidate_text)) == NULL ||
				!is_usable_server_ipv4(candidate_text)) {
				continue;
			}
			if (candidate->sin_addr.s_addr == advertised.S_un.S_addr) {
				result->matches_advertised_address = TRUE;
			}
			score = server_address_score(candidate_text, NULL);
			if (score > best_score ||
				result->resolved_address[0] == L'\0') {
				best_score = score;
				wcsncpy_s(result->resolved_address,
					UI_ARRAY_COUNT(result->resolved_address),
					candidate_text, _TRUNCATE);
			}
		}
	}

complete:
	if (addresses != NULL) {
		FreeAddrInfoW(addresses);
	}
	if (result->lookup_error != 0 && result->resolved_address[0] == L'\0') {
		result->matches_advertised_address = FALSE;
	}
	if (winsock_ready) {
		WSACleanup();
	}
	HeapFree(GetProcessHeap(), 0, arguments);
	if (!PostMessageW(result->window, PORT_TOOL_MESSAGE_DNS_CHECK_COMPLETE,
		0, (LPARAM)result)) {
		HeapFree(GetProcessHeap(), 0, result);
	}
	return 0;
}

static void check_server_dns(PortToolState *state, unsigned int index)
{
	PortToolServer *server;
	ServerDnsCheckArgs *arguments;
	HANDLE thread;

	if (index >= state->server_count) {
		return;
	}
	server = &state->server_items[index];
	if (server->name[0] == L'\0' ||
		!is_usable_server_ipv4(server->address) ||
		_wcsicmp(server->dns_checked_address, server->address) == 0) {
		return;
	}
	arguments = (ServerDnsCheckArgs *)HeapAlloc(GetProcessHeap(),
		HEAP_ZERO_MEMORY, sizeof(*arguments));
	if (arguments == NULL) {
		log_text(state, L"无法分配 DNS 检测任务内存：%ls。", server->name);
		return;
	}
	arguments->window = state->window;
	wcsncpy_s(arguments->server_name,
		UI_ARRAY_COUNT(arguments->server_name), server->name, _TRUNCATE);
	wcsncpy_s(arguments->advertised_address,
		UI_ARRAY_COUNT(arguments->advertised_address), server->address,
		_TRUNCATE);
	log_text(state,
		L"正在刷新并检测服务端 %ls 的 DNS 解析；广播地址 %ls。",
		server->name, server->address);
	thread = CreateThread(NULL, 0, server_dns_check_thread,
		arguments, 0, NULL);
	if (thread == NULL) {
		DWORD error = GetLastError();

		HeapFree(GetProcessHeap(), 0, arguments);
		log_text(state, L"无法启动服务端 %ls 的 DNS 检测，错误 %lu。",
			server->name, (unsigned long)error);
		return;
	}
	wcsncpy_s(server->dns_checked_address,
		UI_ARRAY_COUNT(server->dns_checked_address), server->address,
		_TRUNCATE);
	CloseHandle(thread);
}

static void finish_server_dns_check(PortToolState *state,
	ServerDnsCheckResult *result)
{
	if (result->lookup_error != 0) {
		log_text(state,
			L"DNS 检测：刷新后仍无法解析服务端 %ls（错误 %d）；"
			L"后续端口将使用广播 IP %ls。",
			result->server_name, result->lookup_error,
			result->advertised_address);
	}
	else if (result->matches_advertised_address) {
		log_text(state,
			L"DNS 检测正常：%ls 包含服务端广播地址 %ls。",
			result->server_name, result->advertised_address);
	}
	else if (result->resolved_address[0] != L'\0') {
		log_text(state,
			L"DNS 地址不一致：%ls 解析到 %ls，广播确认地址为 %ls；"
			L"后续端口将使用广播 IP。",
			result->server_name, result->resolved_address,
			result->advertised_address);
	}
	else {
		log_text(state,
			L"DNS 检测：%ls 没有可用的 IPv4 地址；"
			L"后续端口将使用广播 IP %ls。",
			result->server_name, result->advertised_address);
	}
	HeapFree(GetProcessHeap(), 0, result);
}

static DWORD WINAPI connection_test_thread(void *parameter)
{
	ConnectionTestArgs *arguments = (ConnectionTestArgs *)parameter;
	ConnectionTestResult *result;
	WSADATA winsock_data;
	ADDRINFOW hints;
	PADDRINFOW addresses = NULL;
	PADDRINFOW address;
	wchar_t service[16];
	int lookup_result;
	BOOL winsock_ready = FALSE;
	BOOL connected = FALSE;
	DWORD started;
	const wchar_t *lookup_host;

	result = (ConnectionTestResult *)HeapAlloc(GetProcessHeap(),
		HEAP_ZERO_MEMORY, sizeof(*result));
	if (result == NULL) {
		PostMessageW(arguments->window, PORT_TOOL_MESSAGE_TEST_COMPLETE,
			0, 0);
		CloseHandle(arguments->cancel_event);
		HeapFree(GetProcessHeap(), 0, arguments);
		return 0;
	}
	result->window = arguments->window;
	wcsncpy_s(result->host, UI_ARRAY_COUNT(result->host),
		arguments->host, _TRUNCATE);
	wcsncpy_s(result->address, UI_ARRAY_COUNT(result->address),
		arguments->address, _TRUNCATE);
	result->port = arguments->port;
	lookup_result = WSAStartup(MAKEWORD(2, 2), &winsock_data);
	if (lookup_result != 0) {
		result->socket_error = lookup_result;
		goto complete;
	}
	winsock_ready = TRUE;
	_snwprintf_s(service, UI_ARRAY_COUNT(service), _TRUNCATE, L"%u",
		arguments->port);
	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	lookup_host = is_ipv4_address(arguments->address) ?
		arguments->address : arguments->host;
	started = GetTickCount();
	lookup_result = GetAddrInfoW(lookup_host, service, &hints,
		&addresses);
	if (lookup_result != 0) {
		result->lookup_error = lookup_result;
		goto complete;
	}
	if (WaitForSingleObject(arguments->cancel_event, 0) ==
		WAIT_OBJECT_0) {
		result->cancelled = TRUE;
		goto complete;
	}
	if ((DWORD)(GetTickCount() - started) >= 5000) {
		result->timed_out = TRUE;
		goto complete;
	}
	for (address = addresses; address != NULL;
		address = address->ai_next) {
		SOCKET socket_handle;
		u_long nonblocking = 1;
		int connect_result;
		wchar_t current_address[64];

		if (WaitForSingleObject(arguments->cancel_event, 0) ==
			WAIT_OBJECT_0) {
			result->cancelled = TRUE;
			break;
		}
		if ((DWORD)(GetTickCount() - started) >= 5000) {
			result->timed_out = TRUE;
			break;
		}
		if (address->ai_addr == NULL ||
			address->ai_addrlen < sizeof(struct sockaddr_in)) {
			continue;
		}
		{
			unsigned long ip = ntohl(((const struct sockaddr_in *)
				address->ai_addr)->sin_addr.s_addr);

			_snwprintf_s(current_address,
				UI_ARRAY_COUNT(current_address), _TRUNCATE,
				L"%u.%u.%u.%u", (unsigned int)((ip >> 24) & 0xff),
				(unsigned int)((ip >> 16) & 0xff),
				(unsigned int)((ip >> 8) & 0xff),
				(unsigned int)(ip & 0xff));
			if (result->resolved[0] == L'\0') {
				wcsncpy_s(result->resolved,
					UI_ARRAY_COUNT(result->resolved), current_address,
					_TRUNCATE);
			}
		}
		socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (socket_handle == INVALID_SOCKET) {
			result->socket_error = WSAGetLastError();
			continue;
		}
		if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) ==
			SOCKET_ERROR) {
			result->socket_error = WSAGetLastError();
			closesocket(socket_handle);
			continue;
		}
		connect_result = connect(socket_handle, address->ai_addr,
			(int)address->ai_addrlen);
		if (connect_result == 0) {
			connected = TRUE;
			wcsncpy_s(result->resolved,
				UI_ARRAY_COUNT(result->resolved), current_address,
				_TRUNCATE);
			closesocket(socket_handle);
			break;
		}
		result->socket_error = WSAGetLastError();
		if (result->socket_error == WSAEWOULDBLOCK ||
			result->socket_error == WSAEINPROGRESS ||
			result->socket_error == WSAEINVAL) {
			for (;;) {
				fd_set write_set;
				fd_set error_set;
				struct timeval timeout;
				DWORD elapsed;
				int selected;
				int socket_error = 0;
				int socket_error_size = sizeof(socket_error);

				if (WaitForSingleObject(arguments->cancel_event, 0) ==
					WAIT_OBJECT_0) {
					result->cancelled = TRUE;
					break;
				}
				elapsed = GetTickCount() - started;
				if (elapsed >= 5000) {
					result->timed_out = TRUE;
					break;
				}
				FD_ZERO(&write_set);
				FD_ZERO(&error_set);
				FD_SET(socket_handle, &write_set);
				FD_SET(socket_handle, &error_set);
				timeout.tv_sec = 0;
				timeout.tv_usec = (long)((5000 - elapsed) < 100 ?
					(5000 - elapsed) : 100) * 1000;
				selected = select(0, NULL, &write_set, &error_set,
					&timeout);
				if (selected == SOCKET_ERROR) {
					result->socket_error = WSAGetLastError();
					break;
				}
				if (selected > 0) {
					if (getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
						(char *)&socket_error, &socket_error_size) == 0 &&
						socket_error == 0) {
						connected = TRUE;
						wcsncpy_s(result->resolved,
							UI_ARRAY_COUNT(result->resolved),
							current_address, _TRUNCATE);
					}
					else {
						result->socket_error = socket_error != 0 ?
							socket_error : WSAGetLastError();
					}
					break;
				}
			}
		}
		closesocket(socket_handle);
		if (connected || result->cancelled) {
			break;
		}
	}
	result->connected = connected;

complete:
	if (addresses != NULL) {
		FreeAddrInfoW(addresses);
	}
	if (winsock_ready) {
		WSACleanup();
	}
	if (WaitForSingleObject(arguments->cancel_event, 0) ==
		WAIT_OBJECT_0) {
		HeapFree(GetProcessHeap(), 0, result);
		CloseHandle(arguments->cancel_event);
		HeapFree(GetProcessHeap(), 0, arguments);
		return 0;
	}
	CloseHandle(arguments->cancel_event);
	HeapFree(GetProcessHeap(), 0, arguments);
	if (!PostMessageW(result->window, PORT_TOOL_MESSAGE_TEST_COMPLETE,
		0, (LPARAM)result)) {
		HeapFree(GetProcessHeap(), 0, result);
	}
	return 0;
}

static void test_selected_connection(PortToolState *state)
{
	const PortToolPrinter *printer = selected_printer(state);
	ConnectionTestArgs *arguments;
	wchar_t probe_address[64] = { 0 };
	DWORD thread_id;

	if (printer == NULL || state->test_thread != NULL) {
		return;
	}
	if (printer->server_name[0] == L'\0' || printer->raw_port == 0) {
		log_text(state, L"连接检测失败：所选打印机没有有效的服务端或端口号。");
		return;
	}
	arguments = (ConnectionTestArgs *)HeapAlloc(GetProcessHeap(),
		HEAP_ZERO_MEMORY, sizeof(*arguments));
	if (arguments == NULL) {
		log_text(state, L"无法启动连接检测：内存不足。");
		return;
	}
	arguments->window = state->window;
	arguments->port = printer->raw_port;
	wcsncpy_s(arguments->host, UI_ARRAY_COUNT(arguments->host),
		printer->server_name, _TRUNCATE);
	if (printer->online && is_ipv4_address(printer->address)) {
		wcsncpy_s(probe_address, UI_ARRAY_COUNT(probe_address),
			printer->address, _TRUNCATE);
	}
	wcsncpy_s(arguments->address, UI_ARRAY_COUNT(arguments->address),
		probe_address, _TRUNCATE);
	state->test_cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (state->test_cancel_event == NULL) {
		HeapFree(GetProcessHeap(), 0, arguments);
		log_text(state, L"无法启动连接检测，错误 %lu。", GetLastError());
		return;
	}
	if (!DuplicateHandle(GetCurrentProcess(), state->test_cancel_event,
		GetCurrentProcess(), &arguments->cancel_event, 0, FALSE,
		DUPLICATE_SAME_ACCESS)) {
		DWORD error = GetLastError();

		CloseHandle(state->test_cancel_event);
		state->test_cancel_event = NULL;
		HeapFree(GetProcessHeap(), 0, arguments);
		log_text(state, L"无法启动连接检测，错误 %lu。", error);
		return;
	}
	state->test_thread = CreateThread(NULL, 0, connection_test_thread,
		arguments, 0, &thread_id);
	if (state->test_thread == NULL) {
		DWORD error = GetLastError();

		CloseHandle(arguments->cancel_event);
		CloseHandle(state->test_cancel_event);
		state->test_cancel_event = NULL;
		HeapFree(GetProcessHeap(), 0, arguments);
		log_text(state, L"无法启动连接检测，错误 %lu。", error);
		return;
	}
	wcsncpy_s(state->test_host, UI_ARRAY_COUNT(state->test_host),
		printer->server_name, _TRUNCATE);
	state->test_port = printer->raw_port;
	EnableWindow(state->test_connection, FALSE);
	if (probe_address[0] != L'\0') {
		log_text(state,
			L"正在检测服务端 %ls 的广播地址 %ls:%u；不发送打印数据，服务端可能记录为空连接。",
			printer->server_name, probe_address, printer->raw_port);
	}
	else {
		log_text(state,
			L"正在检测服务端 %ls:%u（没有在线广播地址，使用计算机名解析）；不发送打印数据，服务端可能记录为空连接。",
			printer->server_name, printer->raw_port);
	}
}

static void finish_connection_test(PortToolState *state,
	ConnectionTestResult *result)
{
	if (state->test_thread != NULL) {
		WaitForSingleObject(state->test_thread, INFINITE);
		CloseHandle(state->test_thread);
		state->test_thread = NULL;
	}
	if (state->test_cancel_event != NULL) {
		CloseHandle(state->test_cancel_event);
		state->test_cancel_event = NULL;
	}
	if (result == NULL) {
		log_text(state, L"连接检测失败：内存不足，无法保存检测结果。");
		EnableWindow(state->test_connection,
			selected_printer(state) != NULL);
		return;
	}
	if (result->cancelled) {
		if (result->address[0] != L'\0') {
			log_text(state, L"已取消对服务端 %ls（广播地址 %ls:%u）的连接检测。",
				result->host, result->address, result->port);
		}
		else {
			log_text(state, L"已取消对 %ls:%u 的连接检测。",
				result->host, result->port);
		}
	}
	else if (result->lookup_error != 0 && result->timed_out) {
		if (result->address[0] != L'\0') {
			log_text(state,
				L"连接检测超时：无法在 5 秒内使用服务端广播地址 %ls 解析连接目标。",
				result->address);
		}
		else {
			log_text(state,
				L"连接检测超时：服务端计算机名 %ls 在 5 秒内未能完成解析。",
				result->host);
		}
	}
	else if (result->lookup_error != 0) {
		if (result->address[0] != L'\0') {
			log_text(state,
				L"连接检测失败：无法使用服务端广播地址 %ls（错误 %d）。",
				result->address, result->lookup_error);
		}
		else {
			log_text(state,
				L"连接检测失败：无法解析服务端计算机名 %ls（名称解析错误 %d）。",
				result->host, result->lookup_error);
		}
	}
	else if (result->connected) {
		if (result->address[0] != L'\0') {
			log_text(state,
				L"连接检测成功：服务端 %ls 的广播地址 %ls，TCP 端口 %u 可连接。",
				result->host, result->resolved, result->port);
		}
		else {
			log_text(state,
				L"连接检测成功：%ls 解析为 %ls，TCP 端口 %u 可连接。",
				result->host, result->resolved, result->port);
		}
	}
	else if (result->resolved[0] != L'\0') {
		if (result->address[0] != L'\0' && result->timed_out) {
			log_text(state,
				L"连接检测超时：服务端 %ls 的广播地址 %ls 上，TCP 端口 %u 在 5 秒内未能连接。",
				result->host, result->resolved, result->port);
		}
		else if (result->address[0] != L'\0') {
			log_text(state,
				L"连接检测失败：服务端 %ls 的广播地址 %ls 上，TCP 端口 %u 不可连接（错误 %d）。",
				result->host, result->resolved, result->port,
				result->socket_error);
		}
		else if (result->timed_out) {
			log_text(state,
				L"连接检测超时：%ls 解析为 %ls，但 TCP 端口 %u 在 5 秒内未能连接。",
				result->host, result->resolved, result->port);
		}
		else {
			log_text(state,
				L"连接检测失败：%ls 解析为 %ls，但 TCP 端口 %u 不可连接（错误 %d）。",
				result->host, result->resolved, result->port,
				result->socket_error);
		}
	}
	else {
		if (result->timed_out) {
			log_text(state,
				L"连接检测超时：无法在 5 秒内连接 %ls:%u。",
				result->host, result->port);
		}
		else {
			log_text(state,
				L"连接检测失败：无法连接 %ls:%u（错误 %d）。",
				result->host, result->port, result->socket_error);
		}
	}
	HeapFree(GetProcessHeap(), 0, result);
	EnableWindow(state->test_connection,
		selected_printer(state) != NULL);
}

static BOOL utf8_to_wide(const char *source, wchar_t *destination,
	size_t destination_count)
{
	int length;

	if (destination == NULL || destination_count == 0) {
		return FALSE;
	}
	destination[0] = L'\0';
	if (source == NULL) {
		source = "";
	}
	length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, source, -1,
		destination, (int)destination_count);
	if (length <= 0) {
		length = MultiByteToWideChar(CP_ACP, 0, source, -1, destination,
			(int)destination_count);
	}
	return length > 0;
}

static int hex_value(char character)
{
	if (character >= '0' && character <= '9') {
		return character - '0';
	}
	if (character >= 'a' && character <= 'f') {
		return character - 'a' + 10;
	}
	if (character >= 'A' && character <= 'F') {
		return character - 'A' + 10;
	}
	return -1;
}

static BOOL percent_decode(const char *source, char *destination,
	size_t destination_count)
{
	size_t output = 0;

	if (destination == NULL || destination_count == 0) {
		return FALSE;
	}
	destination[0] = '\0';
	if (source == NULL) {
		return TRUE;
	}
	while (*source != '\0') {
		unsigned char character = (unsigned char)*source++;

		if (character == '%' && source[0] != '\0' && source[1] != '\0') {
			int high = hex_value(source[0]);
			int low = hex_value(source[1]);

			if (high >= 0 && low >= 0) {
				character = (unsigned char)((high << 4) | low);
				source += 2;
			}
		}
		if (output + 1 >= destination_count) {
			destination[output] = '\0';
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		destination[output++] = (char)character;
	}
	destination[output] = '\0';
	return TRUE;
}

static BOOL decode_packet_text(const char *value, wchar_t *destination,
	size_t destination_count)
{
	char decoded[USBRELAY_STANDARD_DISCOVERY_PACKET_MAX * 2];

	if (!percent_decode(value, decoded, sizeof(decoded))) {
		return FALSE;
	}
	return utf8_to_wide(decoded, destination, destination_count);
}

static const char *packet_field(const PacketField *fields, DWORD count,
	const char *key)
{
	DWORD index;

	for (index = 0; index < count; index++) {
		if (_stricmp(fields[index].key, key) == 0) {
			return fields[index].value;
		}
	}
	return NULL;
}

static int find_server(PortToolState *state, const wchar_t *name)
{
	DWORD index;

	for (index = 0; index < state->server_count; index++) {
		if (_wcsicmp(state->server_items[index].name, name) == 0) {
			return (int)index;
		}
	}
	return -1;
}

static int add_server(PortToolState *state, const wchar_t *name,
	const wchar_t *address, BOOL address_authoritative)
{
	PortToolServer *server;
	int found = find_server(state, name);
	DWORD address_if_index = 0;
	int address_score = INT_MIN;

	if (address != NULL && is_usable_server_ipv4(address) && found >= 0 &&
		wcscmp(state->server_items[found].address, address) == 0) {
		return found;
	}
	if (address != NULL && is_usable_server_ipv4(address)) {
		address_score = server_address_score(address, &address_if_index);
	}

	if (found >= 0) {
		server = &state->server_items[found];
		if (address != NULL && is_usable_server_ipv4(address) &&
			(address_authoritative || server->address[0] == L'\0' ||
				address_score > server->address_score)) {
			wcsncpy_s(server->address, UI_ARRAY_COUNT(server->address),
				address, _TRUNCATE);
			server->address_score = address_score;
			server->address_if_index = address_if_index;
			state->cache_dirty = TRUE;
		}
		return found;
	}
	if (state->server_count >= PORT_TOOL_MAX_SERVERS) {
		return -1;
	}
	server = &state->server_items[state->server_count];
	ZeroMemory(server, sizeof(*server));
	wcsncpy_s(server->name, UI_ARRAY_COUNT(server->name), name, _TRUNCATE);
	server->address_score = INT_MIN;
	if (address != NULL && is_usable_server_ipv4(address)) {
		wcsncpy_s(server->address, UI_ARRAY_COUNT(server->address),
			address, _TRUNCATE);
		server->address_score = address_score;
		server->address_if_index = address_if_index;
	}
	state->cache_dirty = TRUE;
	return (int)state->server_count++;
}

static int find_printer(PortToolState *state, const wchar_t *server_name,
	const wchar_t *printer_name, unsigned int raw_port)
{
	DWORD index;

	for (index = 0; index < state->printer_count; index++) {
		PortToolPrinter *printer = &state->printer_items[index];

		if (_wcsicmp(printer->server_name, server_name) == 0 &&
			((printer_name != NULL && printer_name[0] != L'\0' &&
				_wcsicmp(printer->printer_name, printer_name) == 0) ||
				((printer_name == NULL || printer_name[0] == L'\0') &&
					printer->raw_port == raw_port))) {
			return (int)index;
		}
	}
	return -1;
}

static int find_remembered_printer(const PortToolState *state)
{
	DWORD index;

	if (!state->selected_printer_valid) {
		return -1;
	}
	for (index = 0; index < state->printer_count; index++) {
		const PortToolPrinter *printer = &state->printer_items[index];

		if (_wcsicmp(printer->server_name,
				state->selected_printer_server) == 0 &&
			_wcsicmp(printer->printer_name,
				state->selected_printer_name) == 0 &&
			printer->raw_port == state->selected_printer_raw_port) {
			return (int)index;
		}
	}
	return -1;
}

static void clear_selected_printer(PortToolState *state)
{
	state->selected_printer = -1;
	state->selected_printer_valid = FALSE;
	state->selected_printer_server[0] = L'\0';
	state->selected_printer_name[0] = L'\0';
	state->selected_printer_raw_port = 0;
}

static void remember_selected_printer(PortToolState *state, int index)
{
	const PortToolPrinter *printer;

	if (index < 0 || index >= (int)state->printer_count) {
		clear_selected_printer(state);
		return;
	}
	printer = &state->printer_items[index];
	state->selected_printer = index;
	state->selected_printer_valid = TRUE;
	wcsncpy_s(state->selected_printer_server,
		UI_ARRAY_COUNT(state->selected_printer_server),
		printer->server_name, _TRUNCATE);
	wcsncpy_s(state->selected_printer_name,
		UI_ARRAY_COUNT(state->selected_printer_name),
		printer->printer_name, _TRUNCATE);
	state->selected_printer_raw_port = printer->raw_port;
}

static int add_printer(PortToolState *state, const wchar_t *server_name,
	const wchar_t *address)
{
	PortToolPrinter *printer;
	int found = find_printer(state, server_name, L"", 0);

	if (found >= 0) {
		return found;
	}
	if (state->printer_count >= PORT_TOOL_MAX_PRINTERS) {
		return -1;
	}
	printer = &state->printer_items[state->printer_count];
	ZeroMemory(printer, sizeof(*printer));
	wcsncpy_s(printer->server_name, UI_ARRAY_COUNT(printer->server_name),
		server_name, _TRUNCATE);
	wcsncpy_s(printer->address, UI_ARRAY_COUNT(printer->address),
		address != NULL ? address : L"", _TRUNCATE);
	state->cache_dirty = TRUE;
	return (int)state->printer_count++;
}

static BOOL same_text(const wchar_t *left, const wchar_t *right)
{
	return wcscmp(left != NULL ? left : L"",
		right != NULL ? right : L"") == 0;
}

static void set_printer_text(wchar_t *destination, size_t destination_count,
	const wchar_t *value, BOOL *changed)
{
	if (!same_text(destination, value)) {
		*changed = TRUE;
	}
	wcsncpy_s(destination, destination_count, value != NULL ? value : L"",
		_TRUNCATE);
}

static void set_status_text(PortToolState *state)
{
	DWORD online_servers = 0;
	DWORD online_printers = 0;
	DWORD index;
	wchar_t text[256];

	for (index = 0; index < state->server_count; index++) {
		if (!state->server_items[index].cached) {
			online_servers++;
		}
	}
	for (index = 0; index < state->printer_count; index++) {
		if (state->printer_items[index].online) {
			online_printers++;
		}
	}
	if (state->server_count == 0) {
		wcsncpy_s(text, UI_ARRAY_COUNT(text),
			L"正在等待局域网服务端广播；打开服务端后会自动出现。",
			_TRUNCATE);
	}
	else {
		_snwprintf_s(text, UI_ARRAY_COUNT(text), _TRUNCATE,
			L"发现 %lu 个在线服务端、%lu 台在线打印机；"
			L"缓存 %lu 台。",
			(unsigned long)online_servers,
			(unsigned long)online_printers,
			(unsigned long)state->printer_count);
	}
	SetWindowTextW(state->status, text);
}

static DWORD elapsed_ms(DWORD now, DWORD then)
{
	return now - then;
}

static void mark_discovery_timeouts(PortToolState *state)
{
	DWORD now = GetTickCount();
	DWORD index;
	BOOL changed = FALSE;

	for (index = 0; index < state->server_count; index++) {
		PortToolServer *server = &state->server_items[index];

		if (!server->cached && elapsed_ms(now, server->last_seen) > 15000) {
			server->cached = TRUE;
			changed = TRUE;
		}
	}
	for (index = 0; index < state->printer_count; index++) {
		PortToolPrinter *printer = &state->printer_items[index];

		if (printer->online &&
			elapsed_ms(now, printer->last_seen) > 15000) {
			printer->online = FALSE;
			printer->cached = TRUE;
			changed = TRUE;
		}
	}
	if (changed) {
		set_status_text(state);
	}
}

static void parse_discovery_packet(PortToolState *state,
	char *packet, const char *source_address)
{
	PacketField fields[PORT_TOOL_MAX_PACKET_FIELDS];
	DWORD field_count = 0;
	char *line;
	char *cursor;
	const char *name_value;
	const char *printer_value;
	const char *raw_port_value;
	const char *count_value;
	const char *advertised_address_value;
	wchar_t server_name[128];
	wchar_t printer_name[256];
	wchar_t driver[256];
	wchar_t original_port[128];
	wchar_t endpoint[384];
	wchar_t source_address_text[64];
	wchar_t advertised_address[64];
	wchar_t previous_server_address[64];
	unsigned int raw_port = 0;
	DWORD printer_count = 0;
	int server_index;
	int printer_index;
	BOOL changed = FALSE;

	if (strncmp(packet, USBRELAY_STANDARD_DISCOVERY_MAGIC,
		strlen(USBRELAY_STANDARD_DISCOVERY_MAGIC)) != 0) {
		return;
	}
	if (!utf8_to_wide(source_address, source_address_text,
		UI_ARRAY_COUNT(source_address_text))) {
		source_address_text[0] = L'\0';
	}
	cursor = strchr(packet, '\n');
	if (cursor == NULL) {
		return;
	}
	line = cursor + 1;
	while (*line != '\0' && field_count < PORT_TOOL_MAX_PACKET_FIELDS) {
		char *line_end;
		char *separator;
		size_t key_length;

		line_end = strchr(line, '\n');
		if (line_end != NULL) {
			*line_end = '\0';
			if (line_end > line && line_end[-1] == '\r') {
				line_end[-1] = '\0';
			}
		}
		if (line[0] != '\0' && line[0] != '\r') {
			separator = strchr(line, '=');
			if (separator != NULL) {
				*separator = '\0';
				key_length = strlen(line);
				if (key_length > 0 &&
					key_length < UI_ARRAY_COUNT(fields[0].key)) {
					strncpy_s(fields[field_count].key,
						UI_ARRAY_COUNT(fields[field_count].key),
						line, _TRUNCATE);
					fields[field_count].value = separator + 1;
					field_count++;
				}
			}
		}
		if (line_end == NULL) {
			break;
		}
		line = line_end + 1;
	}

	name_value = packet_field(fields, field_count, "name");
	if (name_value == NULL ||
		!decode_packet_text(name_value, server_name,
			UI_ARRAY_COUNT(server_name)) ||
		server_name[0] == L'\0') {
		return;
	}
	advertised_address_value = packet_field(fields, field_count, "address");
	advertised_address[0] = L'\0';
	if (advertised_address_value != NULL &&
		utf8_to_wide(advertised_address_value, advertised_address,
			UI_ARRAY_COUNT(advertised_address)) &&
		is_usable_server_ipv4(advertised_address)) {
		if (source_address_text[0] != L'\0' &&
			is_usable_server_ipv4(source_address_text)) {
			wcsncpy_s(advertised_address,
				UI_ARRAY_COUNT(advertised_address),
				source_address_text, _TRUNCATE);
		}
	}
	else if (source_address_text[0] != L'\0' &&
		is_usable_server_ipv4(source_address_text)) {
		wcsncpy_s(advertised_address, UI_ARRAY_COUNT(advertised_address),
			source_address_text, _TRUNCATE);
	}
	else {
		return;
	}
	server_index = find_server(state, server_name);
	previous_server_address[0] = L'\0';
	EnterCriticalSection(&state->port_refresh_lock);
	if (server_index >= 0) {
		wcsncpy_s(previous_server_address,
			UI_ARRAY_COUNT(previous_server_address),
			state->server_items[server_index].address, _TRUNCATE);
	}
	server_index = add_server(state, server_name, advertised_address, TRUE);
	if (server_index >= 0 && wcscmp(previous_server_address,
		state->server_items[server_index].address) != 0) {
		DWORD index;

		for (index = 0; index < state->printer_count; index++) {
			if (_wcsicmp(state->printer_items[index].server_name,
				server_name) == 0) {
			wcsncpy_s(state->printer_items[index].address,
				UI_ARRAY_COUNT(state->printer_items[index].address),
				state->server_items[server_index].address, _TRUNCATE);
				InterlockedIncrement(
					&state->printer_items[index].port_refresh_generation);
				state->printer_items[index].port_refresh_pending = FALSE;
				changed = TRUE;
			}
		}
	}
	LeaveCriticalSection(&state->port_refresh_lock);
	if (server_index < 0) {
		return;
	}
	check_server_dns(state, (unsigned int)server_index);
	if (wcscmp(previous_server_address,
		state->server_items[server_index].address) != 0) {
		changed = TRUE;
	}
	state->server_items[server_index].last_seen = GetTickCount();
	state->server_items[server_index].cached = FALSE;

	printer_value = packet_field(fields, field_count, "printer");
	if (printer_value == NULL) {
		count_value = packet_field(fields, field_count, "printer_count");
		if (count_value != NULL) {
			printer_count = (DWORD)strtoul(count_value, NULL, 10);
			if (state->server_items[server_index].printer_count !=
				printer_count) {
				state->server_items[server_index].printer_count =
					printer_count;
				changed = TRUE;
			}
		}
		for (DWORD printer = 0; printer < state->printer_count; printer++) {
			if (_wcsicmp(state->printer_items[printer].server_name,
				server_name) == 0) {
				schedule_port_refresh(state,
					&state->printer_items[printer]);
			}
		}
		if (changed) {
			state->cache_dirty = TRUE;
			refresh_server_list(state);
			refresh_printer_list(state);
		}
		return;
	}

	if (!decode_packet_text(packet_field(fields, field_count,
		"printer_name"), printer_name,
		UI_ARRAY_COUNT(printer_name))) {
		return;
	}
	raw_port_value = packet_field(fields, field_count, "raw_port");
	if (raw_port_value != NULL) {
		raw_port = (unsigned int)strtoul(raw_port_value, NULL, 10);
	}
	if (printer_name[0] == L'\0' || raw_port == 0) {
		return;
	}
	if (!decode_packet_text(packet_field(fields, field_count, "driver"),
		driver, UI_ARRAY_COUNT(driver))) {
		driver[0] = L'\0';
	}
	if (!decode_packet_text(packet_field(fields, field_count,
		"original_port"), original_port,
		UI_ARRAY_COUNT(original_port))) {
		original_port[0] = L'\0';
	}
	if (!decode_packet_text(packet_field(fields, field_count, "endpoint"),
		endpoint, UI_ARRAY_COUNT(endpoint))) {
		endpoint[0] = L'\0';
	}
	printer_index = find_printer(state, server_name, printer_name, raw_port);
	if (printer_index < 0) {
		if (state->printer_count >= PORT_TOOL_MAX_PRINTERS) {
			return;
		}
		printer_index = (int)state->printer_count++;
		ZeroMemory(&state->printer_items[printer_index],
			sizeof(state->printer_items[printer_index]));
		changed = TRUE;
	}
	{
		PortToolPrinter *printer =
			&state->printer_items[printer_index];
		const char *status_value = packet_field(fields, field_count,
			"status");
		wchar_t previous_printer_address[64];
		BOOL online = status_value != NULL &&
			_stricmp(status_value, "online") == 0;

		EnterCriticalSection(&state->port_refresh_lock);
		wcsncpy_s(previous_printer_address,
			UI_ARRAY_COUNT(previous_printer_address), printer->address,
			_TRUNCATE);
		set_printer_text(printer->server_name,
			UI_ARRAY_COUNT(printer->server_name), server_name, &changed);
		set_printer_text(printer->address,
			UI_ARRAY_COUNT(printer->address),
			state->server_items[server_index].address,
			&changed);
		set_printer_text(printer->printer_name,
			UI_ARRAY_COUNT(printer->printer_name), printer_name, &changed);
		set_printer_text(printer->driver,
			UI_ARRAY_COUNT(printer->driver), driver, &changed);
		set_printer_text(printer->original_port,
			UI_ARRAY_COUNT(printer->original_port), original_port,
			&changed);
		set_printer_text(printer->endpoint,
			UI_ARRAY_COUNT(printer->endpoint), endpoint, &changed);
		if (printer->raw_port != raw_port) {
			printer->raw_port = raw_port;
			changed = TRUE;
		}
		if (printer->online != online) {
			printer->online = online;
			changed = TRUE;
		}
		if (wcscmp(previous_printer_address, printer->address) != 0) {
			InterlockedIncrement(&printer->port_refresh_generation);
			printer->port_refresh_pending = FALSE;
		}
		LeaveCriticalSection(&state->port_refresh_lock);
		if (printer->cached) {
			printer->cached = FALSE;
			changed = TRUE;
		}
		printer->last_seen = GetTickCount();
		schedule_port_refresh(state, printer);
	}
	if (changed) {
		state->cache_dirty = TRUE;
		refresh_server_list(state);
		refresh_printer_list(state);
	}
	set_status_text(state);
}

static void pump_discovery(PortToolState *state)
{
	char packet[USBRELAY_STANDARD_DISCOVERY_PACKET_MAX + 1];
	struct sockaddr_in source;
	int source_length;

	if (state->discovery_socket == INVALID_SOCKET) {
		return;
	}
	for (;;) {
		char address_text[64];
		int received;

		source_length = sizeof(source);
		received = recvfrom(state->discovery_socket, packet,
			(int)sizeof(packet) - 1, 0, (SOCKADDR *)&source,
			&source_length);
		if (received == SOCKET_ERROR) {
			break;
		}
		packet[received] = '\0';
		if (inet_ntop(AF_INET, &source.sin_addr, address_text,
			sizeof(address_text)) == NULL) {
			continue;
		}
		parse_discovery_packet(state, packet, address_text);
	}
}

static BOOL send_discovery_query_to(const struct sockaddr_in *target)
{
	if (g_state.discovery_socket == INVALID_SOCKET) {
		return FALSE;
	}
	return sendto(g_state.discovery_socket,
		USBRELAY_STANDARD_QUERY_MAGIC "\r\n",
		(int)strlen(USBRELAY_STANDARD_QUERY_MAGIC "\r\n"), 0,
		(const SOCKADDR *)target, sizeof(*target)) != SOCKET_ERROR;
}

static void send_discovery_queries(void)
{
	struct sockaddr_in global_address;
	IP_ADAPTER_ADDRESSES *addresses = NULL;
	ULONG size = 0;
	ULONG result;

	ZeroMemory(&global_address, sizeof(global_address));
	global_address.sin_family = AF_INET;
	global_address.sin_port = htons(USBRELAY_STANDARD_DISCOVERY_PORT);
	global_address.sin_addr.s_addr = INADDR_BROADCAST;
	send_discovery_query_to(&global_address);

	result = GetAdaptersAddresses(AF_INET,
		GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
			GAA_FLAG_SKIP_DNS_SERVER,
		NULL, NULL, &size);
	if (result != ERROR_BUFFER_OVERFLOW || size == 0) {
		return;
	}
	addresses = (IP_ADAPTER_ADDRESSES *)HeapAlloc(GetProcessHeap(),
		HEAP_ZERO_MEMORY, size);
	if (addresses == NULL) {
		return;
	}
	result = GetAdaptersAddresses(AF_INET,
		GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
			GAA_FLAG_SKIP_DNS_SERVER,
		NULL, addresses, &size);
	if (result == ERROR_SUCCESS) {
		IP_ADAPTER_ADDRESSES *adapter;

		for (adapter = addresses; adapter != NULL;
			adapter = adapter->Next) {
			IP_ADAPTER_UNICAST_ADDRESS *unicast;

			if (adapter->OperStatus != IfOperStatusUp) {
				continue;
			}
			for (unicast = adapter->FirstUnicastAddress;
				unicast != NULL; unicast = unicast->Next) {
				struct sockaddr_in *local;
				struct sockaddr_in target;
				ULONG prefix;
				ULONG host_mask;
				ULONG broadcast;

				if (unicast->Address.lpSockaddr == NULL ||
					unicast->Address.lpSockaddr->sa_family !=
						AF_INET) {
					continue;
				}
				local = (struct sockaddr_in *)
					unicast->Address.lpSockaddr;
				if (local->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
					continue;
				}
				prefix = unicast->OnLinkPrefixLength;
				if (prefix == 0 || prefix > 32) {
					continue;
				}
				host_mask = prefix == 32 ? 0 :
					((1UL << (32 - prefix)) - 1);
				broadcast = ntohl(local->sin_addr.s_addr) |
					host_mask;
				ZeroMemory(&target, sizeof(target));
				target.sin_family = AF_INET;
				target.sin_port =
					htons(USBRELAY_STANDARD_DISCOVERY_PORT);
				target.sin_addr.s_addr = htonl(broadcast);
				send_discovery_query_to(&target);
			}
		}
	}
	HeapFree(GetProcessHeap(), 0, addresses);
}

static BOOL make_discovery_socket(PortToolState *state)
{
	struct sockaddr_in address;
	BOOL reuse = TRUE;
	BOOL broadcast = TRUE;
	u_long nonblocking = 1;

	state->discovery_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (state->discovery_socket == INVALID_SOCKET) {
		return FALSE;
	}
	setsockopt(state->discovery_socket, SOL_SOCKET, SO_REUSEADDR,
		(const char *)&reuse, sizeof(reuse));
	setsockopt(state->discovery_socket, SOL_SOCKET, SO_BROADCAST,
		(const char *)&broadcast, sizeof(broadcast));
	ZeroMemory(&address, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(USBRELAY_STANDARD_DISCOVERY_PORT);
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

static void close_discovery_socket(PortToolState *state)
{
	if (state->discovery_socket != INVALID_SOCKET) {
		closesocket(state->discovery_socket);
		state->discovery_socket = INVALID_SOCKET;
	}
	if (state->winsock_ready) {
		WSACleanup();
		state->winsock_ready = FALSE;
	}
}

static void send_manual_query(PortToolState *state)
{
	wchar_t host[256];
	char host_ansi[256];
	struct sockaddr_in target;

	GetWindowTextW(state->manual_host, host, UI_ARRAY_COUNT(host));
	UiTrimWhitespace(host);
	if (host[0] == L'\0') {
		MessageBoxW(state->window, L"请输入服务端计算机名或 IP 地址。",
			PORT_TOOL_MESSAGE_TITLE, MB_OK | MB_ICONINFORMATION);
		return;
	}
	if (WideCharToMultiByte(CP_ACP, 0, host, -1, host_ansi,
		sizeof(host_ansi), NULL, NULL) <= 0) {
		MessageBoxW(state->window, L"服务端名称无法转换。",
			PORT_TOOL_MESSAGE_TITLE,
			MB_OK | MB_ICONERROR);
		return;
	}
	ZeroMemory(&target, sizeof(target));
	target.sin_family = AF_INET;
	target.sin_port = htons(USBRELAY_STANDARD_DISCOVERY_PORT);
	if (inet_pton(AF_INET, host_ansi, &target.sin_addr) != 1) {
		ADDRINFOA hints;
		ADDRINFOA *result = NULL;

		ZeroMemory(&hints, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
		if (getaddrinfo(host_ansi, NULL, &hints, &result) != 0 ||
			result == NULL ||
			result->ai_addr == NULL ||
			result->ai_addrlen < sizeof(target)) {
			if (result != NULL) {
				freeaddrinfo(result);
			}
			MessageBoxW(state->window,
				L"无法解析服务端计算机名，请检查局域网名称解析。",
				PORT_TOOL_MESSAGE_TITLE, MB_OK | MB_ICONERROR);
			return;
		}
		memcpy(&target.sin_addr,
			&((struct sockaddr_in *)result->ai_addr)->sin_addr, 4);
		freeaddrinfo(result);
	}
	if (send_discovery_query_to(&target)) {
		log_text(state, L"已向 %ls 发送发现查询。", host);
	}
	else {
		log_text(state, L"向 %ls 发送发现查询失败，错误 %d。", host,
			WSAGetLastError());
	}
}

static BOOL get_cache_path(wchar_t *path, size_t path_count)
{
	wchar_t app_data[MAX_PATH];
	wchar_t directory[MAX_PATH];

	if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
		NULL, SHGFP_TYPE_CURRENT, app_data))) {
		return FALSE;
	}
	if (_snwprintf_s(directory, UI_ARRAY_COUNT(directory), _TRUNCATE,
		L"%ls\\USBRelay", app_data) < 0 ||
		!CreateDirectoryW(directory, NULL) &&
			GetLastError() != ERROR_ALREADY_EXISTS) {
		return FALSE;
	}
	if (_snwprintf_s(directory, UI_ARRAY_COUNT(directory), _TRUNCATE,
		L"%ls\\USBRelay\\PortTool", app_data) < 0 ||
		!CreateDirectoryW(directory, NULL) &&
			GetLastError() != ERROR_ALREADY_EXISTS) {
		return FALSE;
	}
	if (_snwprintf_s(path, path_count, _TRUNCATE,
		L"%ls\\discovery-cache.ini", directory) < 0) {
		return FALSE;
	}
	return TRUE;
}

static void write_cache_integer(const wchar_t *section,
	const wchar_t *name, DWORD value, const wchar_t *path)
{
	wchar_t text[32];

	_snwprintf_s(text, UI_ARRAY_COUNT(text), _TRUNCATE, L"%lu",
		(unsigned long)value);
	WritePrivateProfileStringW(section, name, text, path);
}

static DWORD read_cache_integer(const wchar_t *section,
	const wchar_t *name, DWORD default_value, const wchar_t *path)
{
	return GetPrivateProfileIntW(section, name, (INT)default_value, path);
}

static void read_cache_string(const wchar_t *section, const wchar_t *name,
	wchar_t *value, DWORD value_chars, const wchar_t *path)
{
	if (value_chars == 0) {
		return;
	}
	value[0] = L'\0';
	GetPrivateProfileStringW(section, name, L"", value, value_chars, path);
}

static void save_cache(PortToolState *state)
{
	wchar_t path[MAX_PATH];
	wchar_t section[32];
	wchar_t value[32];
	DWORD index;

	if (!state->cache_dirty || !get_cache_path(path, UI_ARRAY_COUNT(path))) {
		return;
	}
	DeleteFileW(path);
	write_cache_integer(L"General", L"Version",
		PORT_TOOL_CACHE_VERSION, path);
	write_cache_integer(L"General", L"Count", state->printer_count, path);
	for (index = 0; index < state->printer_count; index++) {
		PortToolPrinter *printer = &state->printer_items[index];

		_snwprintf_s(section, UI_ARRAY_COUNT(section), _TRUNCATE,
			L"Printer%lu", (unsigned long)index);
		WritePrivateProfileStringW(section, L"Server",
			printer->server_name, path);
		WritePrivateProfileStringW(section, L"Address",
			printer->address, path);
		WritePrivateProfileStringW(section, L"PrinterName",
			printer->printer_name, path);
		WritePrivateProfileStringW(section, L"Driver",
			printer->driver, path);
		WritePrivateProfileStringW(section, L"OriginalPort",
			printer->original_port, path);
		WritePrivateProfileStringW(section, L"Endpoint",
			printer->endpoint, path);
		_snwprintf_s(value, UI_ARRAY_COUNT(value), _TRUNCATE, L"%u",
			printer->raw_port);
		WritePrivateProfileStringW(section, L"RawPort", value, path);
	}
	state->cache_dirty = FALSE;
}

static void load_cache(PortToolState *state)
{
	wchar_t path[MAX_PATH];
	wchar_t section[32];
	DWORD count;
	DWORD index;
	BOOL deduplicated = FALSE;

	if (!get_cache_path(path, UI_ARRAY_COUNT(path))) {
		return;
	}
	count = read_cache_integer(L"General", L"Count", 0, path);
	if (count > PORT_TOOL_MAX_PRINTERS) {
		count = PORT_TOOL_MAX_PRINTERS;
	}
	for (index = 0; index < count; index++) {
		PortToolPrinter printer;
		wchar_t raw_port[32];
		int server_index;
		int printer_index;

		ZeroMemory(&printer, sizeof(printer));
		_snwprintf_s(section, UI_ARRAY_COUNT(section), _TRUNCATE,
			L"Printer%lu", (unsigned long)index);
		read_cache_string(section, L"Server", printer.server_name,
			UI_ARRAY_COUNT(printer.server_name), path);
		read_cache_string(section, L"Address", printer.address,
			UI_ARRAY_COUNT(printer.address), path);
		read_cache_string(section, L"PrinterName", printer.printer_name,
			UI_ARRAY_COUNT(printer.printer_name), path);
		read_cache_string(section, L"Driver", printer.driver,
			UI_ARRAY_COUNT(printer.driver), path);
		read_cache_string(section, L"OriginalPort", printer.original_port,
			UI_ARRAY_COUNT(printer.original_port), path);
		read_cache_string(section, L"Endpoint", printer.endpoint,
			UI_ARRAY_COUNT(printer.endpoint), path);
		read_cache_string(section, L"RawPort", raw_port,
			UI_ARRAY_COUNT(raw_port), path);
		printer.raw_port = (unsigned int)wcstoul(raw_port, NULL, 10);
		if (printer.server_name[0] == L'\0' ||
			printer.printer_name[0] == L'\0' ||
			printer.raw_port == 0) {
			continue;
		}
		server_index = add_server(state, printer.server_name,
			printer.address, FALSE);
		if (server_index < 0) {
			continue;
		}
		state->server_items[server_index].cached = TRUE;
		printer_index = find_printer(state, printer.server_name,
			printer.printer_name, printer.raw_port);
		if (printer_index >= 0) {
			if (printer.address[0] != L'\0' &&
				wcscmp(state->printer_items[printer_index].address,
					printer.address) != 0) {
				wcsncpy_s(state->printer_items[printer_index].address,
					UI_ARRAY_COUNT(
						state->printer_items[printer_index].address),
					printer.address, _TRUNCATE);
			}
			deduplicated = TRUE;
			continue;
		}
		printer_index = (int)state->printer_count;
		if (printer_index >= PORT_TOOL_MAX_PRINTERS) {
			break;
		}
		state->printer_items[printer_index] = printer;
		state->printer_items[printer_index].online = FALSE;
		state->printer_items[printer_index].cached = TRUE;
		state->printer_items[printer_index].last_seen = 0;
		state->printer_count++;
		state->server_items[server_index].printer_count++;
	}
	state->cache_dirty = deduplicated;
}

static void clear_local_cache(PortToolState *state)
{
	wchar_t path[MAX_PATH];
	DWORD error;
	BOOL removed = FALSE;

	if (MessageBoxW(state->window,
		L"确定清理本地服务端和打印机缓存吗？\r\n\r\n"
		L"清理后会重新执行局域网发现。\r\n"
		L"已创建的 Standard TCP/IP Port、打印机队列和驱动不会被删除。",
		PORT_TOOL_MESSAGE_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) {
		return;
	}
	if (!get_cache_path(path, UI_ARRAY_COUNT(path))) {
		log_text(state, L"读取本地缓存路径失败，错误 %lu。",
			(unsigned long)GetLastError());
		MessageBoxW(state->window, L"无法定位本地缓存文件。",
			PORT_TOOL_MESSAGE_TITLE, MB_OK | MB_ICONERROR);
		return;
	}
	if (DeleteFileW(path)) {
		removed = TRUE;
	}
	else {
		error = GetLastError();
		if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
			log_text(state, L"删除本地缓存失败，错误 %lu。",
				(unsigned long)error);
			SetLastError(error);
			UiShowLastError(state->window, L"清理本地缓存");
			return;
		}
	}

	ZeroMemory(state->server_items, sizeof(state->server_items));
	ZeroMemory(state->printer_items, sizeof(state->printer_items));
	state->server_count = 0;
	state->printer_count = 0;
	state->selected_server = 0;
	state->visible_printer_count = 0;
	state->cache_dirty = FALSE;
	clear_selected_printer(state);
	refresh_server_list(state);
	refresh_printer_list(state);
	set_status_text(state);
	send_discovery_queries();
	if (removed) {
		log_text(state, L"已清理本地发现缓存，并重新发送局域网发现查询。\r\n"
			L"已创建的端口、打印队列和驱动未被修改。");
	}
	else {
		log_text(state, L"本地发现缓存不存在，已清空当前发现数据并重新查询。\r\n"
			L"已创建的端口、打印队列和驱动未被修改。");
	}
}

static void build_server_port_list(const PortToolState *state,
	const wchar_t *server_name, wchar_t *text, size_t text_count)
{
	unsigned int ports[USBRELAY_STANDARD_PORT_COUNT];
	DWORD port_count = 0;
	DWORD index;
	size_t used = 0;

	if (text == NULL || text_count == 0) {
		return;
	}
	text[0] = L'\0';
	for (index = 0; index < state->printer_count; index++) {
		const PortToolPrinter *printer = &state->printer_items[index];
		DWORD position;
		BOOL duplicate = FALSE;

		if (_wcsicmp(printer->server_name, server_name) != 0 ||
			printer->raw_port < USBRELAY_STANDARD_PORT_BASE ||
			printer->raw_port > USBRELAY_STANDARD_PORT_LAST) {
			continue;
		}
		for (position = 0; position < port_count; position++) {
			if (ports[position] == printer->raw_port) {
				duplicate = TRUE;
				break;
			}
		}
		if (duplicate) {
			continue;
		}
		position = port_count;
		while (position > 0 && ports[position - 1] > printer->raw_port) {
			ports[position] = ports[position - 1];
			position--;
		}
		ports[position] = printer->raw_port;
		port_count++;
	}
	if (port_count == 0) {
		wcsncpy_s(text, text_count, L"待发现", _TRUNCATE);
		return;
	}
	for (index = 0; index < port_count; index++) {
		int written;

		if (used + 1 >= text_count) {
			break;
		}
		written = _snwprintf_s(text + used, text_count - used, _TRUNCATE,
			L"%ls%u", index == 0 ? L"" : L",", ports[index]);
		if (written < 0) {
			text[used] = L'\0';
			break;
		}
		used += (size_t)written;
	}
}

static void refresh_server_list(PortToolState *state)
{
	wchar_t previous_name[128] = { 0 };
	unsigned int previous;
	DWORD index;
	int selected_item = -1;

	previous = state->selected_server;
	if (previous < state->server_count) {
		wcsncpy_s(previous_name, UI_ARRAY_COUNT(previous_name),
			state->server_items[previous].name, _TRUNCATE);
	}
	state->updating_servers = TRUE;
	SendMessageW(state->servers, CB_RESETCONTENT, 0, 0);
	for (index = 0; index < state->server_count; index++) {
		wchar_t text[1024];
		wchar_t port_list[512];
		int item;

		build_server_port_list(state, state->server_items[index].name,
			port_list, UI_ARRAY_COUNT(port_list));
		_snwprintf_s(text, UI_ARRAY_COUNT(text), _TRUNCATE,
			L"%ls  (%ls)  - %lu 台 - 端口 %ls%ls",
			state->server_items[index].name,
			state->server_items[index].address[0] != L'\0' ?
				state->server_items[index].address : L"等待地址",
			(unsigned long)state->server_items[index].printer_count,
			port_list,
			state->server_items[index].cached ? L"  缓存" : L"");
		item = (int)SendMessageW(state->servers, CB_ADDSTRING, 0,
			(LPARAM)text);
		if (item >= 0) {
			SendMessageW(state->servers, CB_SETITEMDATA, item, index);
			if (previous_name[0] != L'\0' &&
				_wcsicmp(state->server_items[index].name,
					previous_name) == 0) {
				selected_item = item;
			}
		}
	}
	if (selected_item < 0 && SendMessageW(state->servers, CB_GETCOUNT,
		0, 0) > 0) {
		selected_item = 0;
	}
	if (selected_item >= 0) {
		unsigned int selected = (unsigned int)SendMessageW(
			state->servers, CB_GETITEMDATA, selected_item, 0);

		if (selected != state->selected_server) {
			state->selected_server = selected;
			clear_selected_printer(state);
			refresh_printer_list(state);
		}
		SendMessageW(state->servers, CB_SETCURSEL, selected_item, 0);
	}
	else {
		state->selected_server = 0;
		if (state->server_count == 0) {
			clear_selected_printer(state);
		}
	}
	SendMessageW(state->servers, CB_SETDROPPEDWIDTH, 960, 0);
	state->updating_servers = FALSE;
}

static BOOL local_port_exists(const PortToolState *state,
	const wchar_t *port_name)
{
	DWORD index;

	for (index = 0; index < state->local_port_count; index++) {
		if (_wcsicmp(state->local_ports[index], port_name) == 0) {
			return TRUE;
		}
	}
	return FALSE;
}

static void refresh_local_ports(PortToolState *state)
{
	DWORD needed = 0;
	DWORD returned = 0;
	BYTE *buffer = NULL;
	DWORD index;

	state->local_port_count = 0;
	if (!EnumPortsW(NULL, 2, NULL, 0, &needed, &returned) &&
		GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
		return;
	}
	if (needed == 0) {
		return;
	}
	buffer = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
	if (buffer == NULL) {
		return;
	}
	if (!EnumPortsW(NULL, 2, buffer, needed, &needed, &returned)) {
		HeapFree(GetProcessHeap(), 0, buffer);
		return;
	}
	{
		PORT_INFO_2W *ports = (PORT_INFO_2W *)buffer;

		for (index = 0; index < returned &&
			state->local_port_count < PORT_TOOL_MAX_LOCAL_PORTS; index++) {
			if (ports[index].pPortName == NULL ||
				ports[index].pPortName[0] == L'\0') {
				continue;
			}
			wcsncpy_s(state->local_ports[state->local_port_count],
				MAX_PORTNAME_LEN, ports[index].pPortName, _TRUNCATE);
			state->local_port_count++;
		}
	}
	HeapFree(GetProcessHeap(), 0, buffer);
}

static BOOL port_name_matches_identity(const wchar_t *port_name,
	const wchar_t *server_name, unsigned int raw_port)
{
	wchar_t host_component[31];
	wchar_t port_component[24];
	const wchar_t *host_start;
	const wchar_t *cursor;
	const wchar_t *host_end;
	BOOL service_port_seen = FALSE;

	if (port_name == NULL || server_name == NULL || raw_port == 0) {
		return FALSE;
	}
	sanitize_port_component(server_name, host_component,
		UI_ARRAY_COUNT(host_component));
	_snwprintf_s(port_component, UI_ARRAY_COUNT(port_component),
		_TRUNCATE, L"%u", raw_port);
	for (host_start = port_name; *host_start != L'\0'; host_start++) {
		if ((host_start != port_name && host_start[-1] != L'_') ||
			_wcsnicmp(host_start, host_component,
				wcslen(host_component)) != 0) {
			continue;
		}
		host_end = host_start + wcslen(host_component);
		if (*host_end != L'_') {
			continue;
		}
		cursor = host_end + 1;
		if (wcscmp(cursor, port_component) == 0) {
			return TRUE;
		}
		while (*cursor >= L'0' && *cursor <= L'9') {
			service_port_seen = TRUE;
			cursor++;
		}
		if (service_port_seen && *cursor == L'_' &&
			wcscmp(cursor + 1, port_component) == 0) {
			return TRUE;
		}
		service_port_seen = FALSE;
	}
	return FALSE;
}

static BOOL probe_ipv4_port(const wchar_t *address, unsigned int port_number,
	DWORD timeout_ms, DWORD *error)
{
	WSADATA winsock_data;
	struct sockaddr_in remote;
	SOCKET socket_handle = INVALID_SOCKET;
	u_long nonblocking = 1;
	int result;
	fd_set write_set;
	fd_set error_set;
	struct timeval timeout;
	int socket_error = 0;
	int socket_error_size = sizeof(socket_error);
	BOOL success = FALSE;

	if (error != NULL) {
		*error = ERROR_SUCCESS;
	}
	if (!is_usable_server_ipv4(address) || port_number == 0 ||
		port_number > 65535) {
		if (error != NULL) {
			*error = ERROR_INVALID_PARAMETER;
		}
		return FALSE;
	}
	result = WSAStartup(MAKEWORD(2, 2), &winsock_data);
	if (result != 0) {
		if (error != NULL) {
			*error = (DWORD)result;
		}
		return FALSE;
	}
	ZeroMemory(&remote, sizeof(remote));
	remote.sin_family = AF_INET;
	remote.sin_port = htons((u_short)port_number);
	if (InetPtonW(AF_INET, address, &remote.sin_addr) != 1) {
		if (error != NULL) {
			*error = WSAEINVAL;
		}
		goto cleanup;
	}
	socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket_handle == INVALID_SOCKET ||
		ioctlsocket(socket_handle, FIONBIO, &nonblocking) == SOCKET_ERROR) {
		if (error != NULL) {
			*error = WSAGetLastError();
		}
		goto cleanup;
	}
	result = connect(socket_handle, (const struct sockaddr *)&remote,
		sizeof(remote));
	if (result == 0) {
		success = TRUE;
		goto cleanup;
	}
	if (WSAGetLastError() != WSAEWOULDBLOCK &&
		WSAGetLastError() != WSAEINPROGRESS &&
		WSAGetLastError() != WSAEINVAL) {
		if (error != NULL) {
			*error = (DWORD)WSAGetLastError();
		}
		goto cleanup;
	}
	FD_ZERO(&write_set);
	FD_ZERO(&error_set);
	FD_SET(socket_handle, &write_set);
	FD_SET(socket_handle, &error_set);
	timeout.tv_sec = (long)(timeout_ms / 1000);
	timeout.tv_usec = (long)(timeout_ms % 1000) * 1000;
	result = select(0, NULL, &write_set, &error_set, &timeout);
	if (result > 0 && getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
		(char *)&socket_error, &socket_error_size) == 0 &&
		socket_error == 0) {
		success = TRUE;
	}
	else if (error != NULL) {
		*error = result == 0 ? WSAETIMEDOUT :
			(DWORD)(socket_error != 0 ? socket_error : WSAGetLastError());
	}

cleanup:
	if (socket_handle != INVALID_SOCKET) {
		closesocket(socket_handle);
	}
	WSACleanup();
	return success;
}

static BOOL get_tcp_port_config(const wchar_t *port_name, PORT_DATA_1 *data,
	DWORD *error)
{
	wchar_t xcv_name[96];
	PRINTER_DEFAULTS defaults;
	CONFIG_INFO_DATA_1 request;
	HANDLE port = NULL;
	DWORD needed = 0;
	DWORD status = ERROR_SUCCESS;
	BOOL result;

	_snwprintf_s(xcv_name, UI_ARRAY_COUNT(xcv_name), _TRUNCATE,
		L",XcvPort %ls", port_name);
	ZeroMemory(&defaults, sizeof(defaults));
	defaults.DesiredAccess = SERVER_ACCESS_ADMINISTER;
	if (!OpenPrinterW(xcv_name, &port, &defaults)) {
		if (error != NULL) {
			*error = GetLastError();
		}
		return FALSE;
	}
	ZeroMemory(&request, sizeof(request));
	request.dwVersion = 1;
	ZeroMemory(data, sizeof(*data));
	result = XcvDataW(port, L"GetConfigInfo", (PBYTE)&request,
		sizeof(request), (PBYTE)data, sizeof(*data), &needed, &status);
	if (!result || status != ERROR_SUCCESS || needed < sizeof(*data)) {
		if (error != NULL) {
			*error = status != ERROR_SUCCESS ? status :
				(result ? ERROR_INVALID_DATA : GetLastError());
		}
		ClosePrinter(port);
		return FALSE;
	}
	ClosePrinter(port);
	if (error != NULL) {
		*error = ERROR_SUCCESS;
	}
	return TRUE;
}

static BOOL config_tcp_port(const PORT_DATA_1 *data, DWORD *error)
{
	const wchar_t *monitor_name = L",XcvMonitor Standard TCP/IP Port";
	PRINTER_DEFAULTS defaults;
	HANDLE monitor = NULL;
	DWORD needed = 0;
	DWORD status = ERROR_SUCCESS;
	BOOL result;

	ZeroMemory(&defaults, sizeof(defaults));
	defaults.DesiredAccess = SERVER_ACCESS_ADMINISTER;
	if (!OpenPrinterW((LPWSTR)monitor_name, &monitor, &defaults)) {
		if (error != NULL) {
			*error = GetLastError();
		}
		return FALSE;
	}
	result = XcvDataW(monitor, L"ConfigPort", (PBYTE)data, sizeof(*data),
		NULL, 0, &needed, &status);
	ClosePrinter(monitor);
	if (!result || status != ERROR_SUCCESS) {
		if (error != NULL) {
			*error = status != ERROR_SUCCESS ? status : GetLastError();
		}
		return FALSE;
	}
	if (error != NULL) {
		*error = ERROR_SUCCESS;
	}
	return TRUE;
}

static BOOL set_tcp_port_address(const wchar_t *port_name,
	const wchar_t *address, DWORD *error, DWORD *rollback_error)
{
	PORT_DATA_1 original;
	PORT_DATA_1 updated;
	PORT_DATA_1 verified;
	DWORD operation_error = ERROR_SUCCESS;
	DWORD ignored_error = ERROR_SUCCESS;

	if (rollback_error != NULL) {
		*rollback_error = ERROR_SUCCESS;
	}
	if (!get_tcp_port_config(port_name, &original, &operation_error)) {
		goto failed;
	}
	if (wcscmp(original.sztHostAddress, address) == 0) {
		if (error != NULL) {
			*error = ERROR_ALREADY_EXISTS;
		}
		return TRUE;
	}
	updated = original;
	if (wcsncpy_s(updated.sztHostAddress,
		UI_ARRAY_COUNT(updated.sztHostAddress), address, _TRUNCATE) != 0) {
		operation_error = ERROR_INSUFFICIENT_BUFFER;
		goto failed;
	}
	if (!config_tcp_port(&updated, &operation_error)) {
		goto failed;
	}
	if (get_tcp_port_config(port_name, &verified, &operation_error) &&
		wcscmp(verified.sztHostAddress, address) == 0 &&
		verified.dwPortNumber == original.dwPortNumber &&
		wcscmp(verified.sztPortName, original.sztPortName) == 0) {
		if (error != NULL) {
			*error = ERROR_SUCCESS;
		}
		return TRUE;
	}
	if (operation_error == ERROR_SUCCESS) {
		operation_error = ERROR_INVALID_DATA;
	}
	if (!config_tcp_port(&original, &ignored_error) &&
		rollback_error != NULL) {
		*rollback_error = ignored_error;
	}

failed:
	if (error != NULL) {
		*error = operation_error;
	}
	return FALSE;
}

static BOOL port_refresh_request_is_current_locked(
	const PortRefreshArgs *arguments)
{
	PortToolState *state = arguments->state;
	PortToolPrinter *printer;

	if (InterlockedCompareExchange(&state->port_refresh_shutdown, 0, 0) == 0 &&
		arguments->printer_index < state->printer_count) {
		printer = &state->printer_items[arguments->printer_index];
		return printer->raw_port == arguments->raw_port &&
			_wcsicmp(printer->server_name, arguments->server_name) == 0 &&
			_wcsicmp(printer->printer_name, arguments->printer_name) == 0 &&
			wcscmp(printer->address, arguments->address) == 0 &&
			InterlockedCompareExchange(&printer->port_refresh_generation,
				0, 0) == arguments->generation;
	}
	return FALSE;
}

static BOOL port_refresh_request_is_current(
	const PortRefreshArgs *arguments)
{
	BOOL current;

	EnterCriticalSection(&arguments->state->port_refresh_lock);
	current = port_refresh_request_is_current_locked(arguments);
	LeaveCriticalSection(&arguments->state->port_refresh_lock);
	return current;
}

static void finish_port_refresh_worker(PortToolState *state)
{
	LONG remaining;

	EnterCriticalSection(&state->port_refresh_lock);
	remaining = InterlockedDecrement(&state->active_port_refresh_workers);
	if (remaining == 0) {
		SetEvent(state->port_refresh_idle_event);
	}
	LeaveCriticalSection(&state->port_refresh_lock);
}

static DWORD WINAPI port_refresh_thread(void *parameter)
{
	PortRefreshArgs *arguments = (PortRefreshArgs *)parameter;
	PortToolState *state = arguments->state;
	PortRefreshResult *result = (PortRefreshResult *)HeapAlloc(
		GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*result));
	DWORD needed = 0;
	DWORD returned = 0;
	BYTE *buffer = NULL;
	DWORD index;
	DWORD probe_error = ERROR_SUCCESS;
	BOOL port_reachable = FALSE;

	if (result == NULL) {
		BOOL posted = FALSE;

		EnterCriticalSection(&state->port_refresh_lock);
		if (InterlockedCompareExchange(&state->port_refresh_shutdown,
			0, 0) == 0) {
			posted = PostMessageW(arguments->window,
				PORT_TOOL_MESSAGE_PORT_REFRESH_NO_MEMORY, 0,
				(LPARAM)arguments);
		}
		LeaveCriticalSection(&state->port_refresh_lock);
		if (!posted) {
			EnterCriticalSection(&state->port_refresh_lock);
			if (arguments->printer_index < state->printer_count) {
				PortToolPrinter *printer =
					&state->printer_items[arguments->printer_index];

				if (printer->port_refresh_generation ==
					arguments->generation &&
					printer->raw_port == arguments->raw_port &&
					_wcsicmp(printer->server_name,
						arguments->server_name) == 0) {
					printer->port_refresh_pending = FALSE;
				}
			}
			LeaveCriticalSection(&state->port_refresh_lock);
			HeapFree(GetProcessHeap(), 0, arguments);
		}
		finish_port_refresh_worker(state);
		return 0;
	}
	result->window = arguments->window;
	wcsncpy_s(result->server_name, UI_ARRAY_COUNT(result->server_name),
		arguments->server_name, _TRUNCATE);
	wcsncpy_s(result->printer_name, UI_ARRAY_COUNT(result->printer_name),
		arguments->printer_name, _TRUNCATE);
	wcsncpy_s(result->address, UI_ARRAY_COUNT(result->address),
		arguments->address, _TRUNCATE);
	result->raw_port = arguments->raw_port;
	result->printer_index = arguments->printer_index;
	result->generation = arguments->generation;
	if (WaitForSingleObject(state->port_refresh_stop_event, 0) ==
		WAIT_OBJECT_0 || !port_refresh_request_is_current(arguments)) {
		result->cancelled = TRUE;
		goto complete;
	}
	if (!probe_ipv4_port(arguments->address, arguments->raw_port,
		PORT_TOOL_PORT_PROBE_TIMEOUT_MS, &probe_error)) {
		result->error = probe_error;
		goto complete;
	}
	result->connected = TRUE;
	port_reachable = TRUE;
	if (!EnumPortsW(NULL, 2, NULL, 0, &needed, &returned) &&
		GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
		result->error = GetLastError();
		goto complete;
	}
	if (needed == 0) {
		goto complete;
	}
	buffer = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
	if (buffer == NULL) {
		result->error = ERROR_NOT_ENOUGH_MEMORY;
		goto complete;
	}
	if (!EnumPortsW(NULL, 2, buffer, needed, &needed, &returned)) {
		result->error = GetLastError();
		goto complete;
	}
	{
		PORT_INFO_2W *ports = (PORT_INFO_2W *)buffer;

		for (index = 0; index < returned; index++) {
			PORT_DATA_1 current;
			DWORD update_error = ERROR_SUCCESS;
			DWORD rollback_error = ERROR_SUCCESS;

			if (ports[index].pPortName == NULL ||
				ports[index].pMonitorName == NULL ||
				_wcsicmp(ports[index].pMonitorName,
					L"Standard TCP/IP Port") != 0 ||
				!port_name_matches_identity(ports[index].pPortName,
					arguments->server_name, arguments->raw_port)) {
				continue;
			}
			if (WaitForSingleObject(state->port_refresh_stop_event, 0) ==
				WAIT_OBJECT_0) {
				result->cancelled = TRUE;
				break;
			}
			result->matched_ports++;
			if (!get_tcp_port_config(ports[index].pPortName, &current,
				&update_error)) {
				result->failed_ports++;
				if (result->error == ERROR_SUCCESS) {
					result->error = update_error;
				}
				continue;
			}
			if (current.dwProtocol != RAWTCP ||
				current.dwPortNumber != arguments->raw_port ||
				!port_name_matches_identity(current.sztPortName,
					arguments->server_name, arguments->raw_port)) {
				continue;
			}
			if (wcscmp(current.sztHostAddress, arguments->address) == 0) {
				result->unchanged_ports++;
				continue;
			}
			EnterCriticalSection(&state->port_refresh_lock);
			if (!port_refresh_request_is_current_locked(arguments)) {
				LeaveCriticalSection(&state->port_refresh_lock);
				result->cancelled = TRUE;
				break;
			}
			if (set_tcp_port_address(current.sztPortName,
				arguments->address, &update_error, &rollback_error)) {
				result->updated_ports++;
			}
			else {
				result->failed_ports++;
				if (result->error == ERROR_SUCCESS) {
					result->error = update_error;
				}
				if (rollback_error != ERROR_SUCCESS &&
					result->rollback_error == ERROR_SUCCESS) {
					result->rollback_error = rollback_error;
				}
			}
			LeaveCriticalSection(&state->port_refresh_lock);
		}
	}

complete:
	{
		BOOL posted = FALSE;

		if (buffer != NULL) {
			HeapFree(GetProcessHeap(), 0, buffer);
		}
		if (!port_reachable && result->error == ERROR_SUCCESS) {
			result->error = ERROR_CONNECTION_REFUSED;
		}
		EnterCriticalSection(&state->port_refresh_lock);
		if (InterlockedCompareExchange(&state->port_refresh_shutdown,
			0, 0) == 0) {
			posted = PostMessageW(result->window,
				PORT_TOOL_MESSAGE_PORT_REFRESH_COMPLETE, 0,
				(LPARAM)result);
		}
		LeaveCriticalSection(&state->port_refresh_lock);
		if (posted) {
			result = NULL;
		}
		else {
			EnterCriticalSection(&state->port_refresh_lock);
			if (InterlockedCompareExchange(&state->port_refresh_shutdown,
				0, 0) == 0 &&
				port_refresh_request_is_current_locked(arguments)) {
				state->printer_items[arguments->printer_index].
					port_refresh_pending = FALSE;
			}
			LeaveCriticalSection(&state->port_refresh_lock);
			HeapFree(GetProcessHeap(), 0, result);
			result = NULL;
		}
	}
	HeapFree(GetProcessHeap(), 0, arguments);
	finish_port_refresh_worker(state);
	return 0;
}

static void schedule_port_refresh(PortToolState *state,
	PortToolPrinter *printer)
{
	PortRefreshArgs *arguments;
	HANDLE thread;
	DWORD now = GetTickCount();
	DWORD index;
	DWORD printer_index;
	BOOL matching_port_found = FALSE;

	if (printer == NULL || !printer->online ||
		!is_usable_server_ipv4(printer->address) || printer->raw_port == 0 ||
		printer->port_refresh_pending ||
		InterlockedCompareExchange(&state->port_refresh_shutdown, 0, 0) != 0) {
		return;
	}
	printer_index = (DWORD)(printer - state->printer_items);
	for (index = 0; index < printer_index; index++) {
		PortToolPrinter *earlier = &state->printer_items[index];

		if (earlier->raw_port == printer->raw_port && earlier->online &&
			_wcsicmp(earlier->server_name, printer->server_name) == 0 &&
			wcscmp(earlier->address, printer->address) == 0) {
			return;
		}
	}
	for (index = 0; index < state->local_port_count; index++) {
		if (port_name_matches_identity(state->local_ports[index],
			printer->server_name, printer->raw_port)) {
			matching_port_found = TRUE;
			break;
		}
	}
	if (!matching_port_found) {
		return;
	}
	if (wcscmp(printer->last_port_refresh_address, printer->address) == 0 &&
		printer->last_port_refresh_tick != 0 &&
		elapsed_ms(now, printer->last_port_refresh_tick) <
		PORT_TOOL_PORT_REFRESH_RETRY_MS) {
		return;
	}
	arguments = (PortRefreshArgs *)HeapAlloc(GetProcessHeap(),
		HEAP_ZERO_MEMORY, sizeof(*arguments));
	if (arguments == NULL) {
		log_text(state, L"无法分配打印端口地址刷新任务内存。");
		return;
	}
	arguments->window = state->window;
	arguments->state = state;
	wcsncpy_s(arguments->server_name,
		UI_ARRAY_COUNT(arguments->server_name), printer->server_name,
		_TRUNCATE);
	wcsncpy_s(arguments->printer_name,
		UI_ARRAY_COUNT(arguments->printer_name), printer->printer_name,
		_TRUNCATE);
	wcsncpy_s(arguments->address, UI_ARRAY_COUNT(arguments->address),
		printer->address, _TRUNCATE);
	arguments->raw_port = printer->raw_port;
	arguments->printer_index = printer_index;
	arguments->generation = InterlockedCompareExchange(
		&printer->port_refresh_generation, 0, 0);
	EnterCriticalSection(&state->port_refresh_lock);
	if (InterlockedCompareExchange(&state->port_refresh_shutdown, 0, 0) != 0 ||
		printer->raw_port != arguments->raw_port ||
		_wcsicmp(printer->server_name, arguments->server_name) != 0 ||
		_wcsicmp(printer->printer_name, arguments->printer_name) != 0 ||
		wcscmp(printer->address, arguments->address) != 0 ||
		InterlockedCompareExchange(&printer->port_refresh_generation,
			0, 0) != arguments->generation) {
		LeaveCriticalSection(&state->port_refresh_lock);
		HeapFree(GetProcessHeap(), 0, arguments);
		return;
	}
	printer->port_refresh_pending = TRUE;
	printer->last_port_refresh_tick = now;
	wcsncpy_s(printer->last_port_refresh_address,
		UI_ARRAY_COUNT(printer->last_port_refresh_address),
		printer->address, _TRUNCATE);
	if (InterlockedCompareExchange(&state->active_port_refresh_workers,
		0, 0) == 0) {
		ResetEvent(state->port_refresh_idle_event);
	}
	InterlockedIncrement(&state->active_port_refresh_workers);
	LeaveCriticalSection(&state->port_refresh_lock);
	thread = CreateThread(NULL, 0, port_refresh_thread, arguments, 0, NULL);
	if (thread == NULL) {
		DWORD error = GetLastError();

		EnterCriticalSection(&state->port_refresh_lock);
		if (printer->port_refresh_generation == arguments->generation &&
			wcscmp(printer->address, arguments->address) == 0) {
			printer->port_refresh_pending = FALSE;
		}
		LeaveCriticalSection(&state->port_refresh_lock);
		HeapFree(GetProcessHeap(), 0, arguments);
		finish_port_refresh_worker(state);
		log_text(state, L"无法启动 %ls:%u 端口刷新，错误 %lu。",
			printer->server_name, printer->raw_port, error);
		return;
	}
	CloseHandle(thread);
}

static void finish_port_refresh(PortToolState *state,
	PortRefreshResult *result)
{
	PortToolPrinter *printer = NULL;

	if (result->printer_index < state->printer_count) {
		printer = &state->printer_items[result->printer_index];
	}
	if (printer != NULL &&
		(printer->raw_port != result->raw_port ||
		_wcsicmp(printer->server_name, result->server_name) != 0 ||
		_wcsicmp(printer->printer_name, result->printer_name) != 0)) {
		printer = NULL;
	}
	if (printer != NULL) {
		if (result->generation != InterlockedCompareExchange(
			&printer->port_refresh_generation, 0, 0) ||
			wcscmp(printer->address, result->address) != 0) {
			log_text(state,
				L"忽略服务端 %ls 的过期地址检测结果 %ls；保留并继续验证最新广播地址 %ls。",
				result->server_name, result->address, printer->address);
			HeapFree(GetProcessHeap(), 0, result);
			schedule_port_refresh(state, printer);
			return;
		}
		printer->port_refresh_pending = FALSE;
	}
	if (result->connected) {
		if (result->matched_ports == 0) {
			log_text(state,
				L"已验证 %ls:%u 可连接；没有找到服务器标识和发布端口匹配的 Standard TCP/IP Port。",
				result->address, result->raw_port);
		}
		else if (result->failed_ports > 0) {
			log_text(state,
				L"已验证 %ls:%u 可连接；端口匹配 %lu 个，已更新 %lu 个，未变更 %lu 个，失败 %lu 个，错误 %lu。旧地址会保留。",
				result->address, result->raw_port,
				(unsigned long)result->matched_ports,
				(unsigned long)result->updated_ports,
				(unsigned long)result->unchanged_ports,
				(unsigned long)result->failed_ports,
				(unsigned long)result->error);
			if (result->rollback_error != ERROR_SUCCESS) {
				log_text(state,
					L"恢复端口原配置也失败，错误 %lu；请检查打印后台处理程序和端口状态。",
					(unsigned long)result->rollback_error);
			}
		}
		else if (result->updated_ports > 0) {
			log_text(state,
				L"服务端 %ls 地址已验证；更新 %lu 个本地端口到 %ls:%u，端口名、端口号和打印机队列保持不变。",
				result->server_name,
				(unsigned long)result->updated_ports,
				result->address, result->raw_port);
		}
		else if (result->unchanged_ports > 0) {
			log_text(state,
				L"服务端 %ls 地址仍为 %ls；匹配的 %lu 个本地端口已是该地址，没有写入配置。",
				result->server_name, result->address,
				(unsigned long)result->unchanged_ports);
		}
	}
	else {
		log_text(state,
			L"服务端 %ls 的 TCP %u (%ls) 尚不可连接，暂不修改本地端口并保留旧 IP；将在后续广播重试。错误 %lu。",
			result->server_name, result->raw_port, result->address,
			(unsigned long)result->error);
	}
	if (result->failed_ports > 0 &&
		(result->error == ERROR_ACCESS_DENIED ||
		result->error == ERROR_PRIVILEGE_NOT_HELD)) {
		log_text(state,
			L"更新 Standard TCP/IP Port 需要管理员权限；请以管理员身份运行配置工具端。旧 IP 保持不变。");
	}
	HeapFree(GetProcessHeap(), 0, result);
}

static void sanitize_port_component(const wchar_t *source,
	wchar_t *destination, size_t destination_count)
{
	size_t output = 0;

	if (destination_count == 0) {
		return;
	}
	if (source == NULL || source[0] == L'\0') {
		source = L"Unknown";
	}
	while (*source != L'\0' && output + 1 < destination_count) {
		wchar_t character = *source++;

		if ((character >= L'a' && character <= L'z') ||
			(character >= L'A' && character <= L'Z') ||
			(character >= L'0' && character <= L'9') ||
			character == L'-' || character == L'_' || character >= 0x80) {
			destination[output++] = character;
		}
		else if (character == L'.') {
			destination[output++] = L'_';
		}
	}
	if (output == 0) {
		wcsncpy_s(destination, destination_count, L"Unknown", _TRUNCATE);
		return;
	}
	destination[output] = L'\0';
}

static void build_port_name(const PortToolPrinter *printer,
	wchar_t *port_name, size_t port_name_count)
{
	wchar_t driver_component[25];
	wchar_t host_component[31];
	wchar_t driver_token[256];
	const wchar_t *driver = printer->driver;
	const wchar_t *driver_end;
	size_t driver_length;

	while (*driver == L' ') {
		driver++;
	}
	driver_end = wcschr(driver, L' ');
	driver_length = driver_end != NULL ?
		(size_t)(driver_end - driver) : wcslen(driver);
	if (driver_length >= UI_ARRAY_COUNT(driver_token)) {
		driver_length = UI_ARRAY_COUNT(driver_token) - 1;
	}
	wmemcpy(driver_token, driver, driver_length);
	driver_token[driver_length] = L'\0';
	sanitize_port_component(driver_token, driver_component,
		UI_ARRAY_COUNT(driver_component));
	sanitize_port_component(printer->server_name, host_component,
		UI_ARRAY_COUNT(host_component));
	_snwprintf_s(port_name, port_name_count, _TRUNCATE,
		L"%ls_%ls_%u", driver_component, host_component,
		printer->raw_port);
}

static BOOL create_standard_tcp_port(const wchar_t *port_name,
	const wchar_t *host, unsigned int port_number, DWORD *error)
{
	const wchar_t *monitor_name = L",XcvMonitor Standard TCP/IP Port";
	PRINTER_DEFAULTS defaults;
	PORT_DATA_1 data;
	HANDLE monitor = NULL;
	DWORD needed = 0;
	DWORD status = ERROR_SUCCESS;
	BOOL result;
	BOOL success = FALSE;

	if (error != NULL) {
		*error = ERROR_SUCCESS;
	}
	if (port_name == NULL || host == NULL || port_number == 0 ||
		port_number > 65535) {
		if (error != NULL) {
			*error = ERROR_INVALID_PARAMETER;
		}
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ZeroMemory(&defaults, sizeof(defaults));
	defaults.DesiredAccess = SERVER_ACCESS_ADMINISTER;
	if (!OpenPrinterW((LPWSTR)monitor_name, &monitor, &defaults)) {
		if (error != NULL) {
			*error = GetLastError();
		}
		return FALSE;
	}
	ZeroMemory(&data, sizeof(data));
	if (wcsncpy_s(data.sztPortName, MAX_PORTNAME_LEN, port_name,
		_TRUNCATE) != 0 ||
		wcsncpy_s(data.sztHostAddress, MAX_NETWORKNAME_LEN, host,
		_TRUNCATE) != 0) {
		status = ERROR_INSUFFICIENT_BUFFER;
		goto cleanup;
	}
	data.dwVersion = 1;
	data.dwProtocol = RAWTCP;
	data.cbSize = sizeof(data);
	data.dwPortNumber = port_number;
	data.dwSNMPEnabled = FALSE;
	data.dwDoubleSpool = FALSE;
	result = XcvDataW(monitor, L"AddPort", (PBYTE)&data, sizeof(data),
		NULL, 0, &needed, &status);
	if (!result && status == ERROR_SUCCESS) {
		status = GetLastError();
	}
	success = result && status == ERROR_SUCCESS;

cleanup:
	ClosePrinter(monitor);
	if (!success && status == ERROR_SUCCESS) {
		status = ERROR_GEN_FAILURE;
	}
	if (error != NULL) {
		*error = status;
	}
	SetLastError(status);
	return success;
}

static BOOL delete_standard_tcp_port(const wchar_t *port_name,
	DWORD *error)
{
	const wchar_t *monitor_name = L",XcvMonitor Standard TCP/IP Port";
	PRINTER_DEFAULTS defaults;
	DELETE_PORT_DATA_1 data;
	HANDLE monitor = NULL;
	DWORD needed = 0;
	DWORD status = ERROR_SUCCESS;
	BOOL result;
	BOOL success = FALSE;

	if (error != NULL) {
		*error = ERROR_SUCCESS;
	}
	ZeroMemory(&defaults, sizeof(defaults));
	defaults.DesiredAccess = SERVER_ACCESS_ADMINISTER;
	if (!OpenPrinterW((LPWSTR)monitor_name, &monitor, &defaults)) {
		if (error != NULL) {
			*error = GetLastError();
		}
		return FALSE;
	}
	ZeroMemory(&data, sizeof(data));
	if (wcsncpy_s(data.psztPortName, MAX_PORTNAME_LEN, port_name,
		_TRUNCATE) != 0) {
		status = ERROR_INSUFFICIENT_BUFFER;
		goto cleanup;
	}
	data.dwVersion = 1;
	result = XcvDataW(monitor, L"DeletePort", (PBYTE)&data, sizeof(data),
		NULL, 0, &needed, &status);
	if (!result && status == ERROR_SUCCESS) {
		status = GetLastError();
	}
	success = result && status == ERROR_SUCCESS;

cleanup:
	ClosePrinter(monitor);
	if (!success && status == ERROR_SUCCESS) {
		status = ERROR_GEN_FAILURE;
	}
	if (error != NULL) {
		*error = status;
	}
	SetLastError(status);
	return success;
}

static const PortToolPrinter *selected_printer(PortToolState *state)
{
	int row;

	row = ListView_GetNextItem(state->printers, -1, LVNI_SELECTED);
	if (row >= 0 && (DWORD)row < state->visible_printer_count) {
		return &state->printer_items[state->visible_printer_indices[row]];
	}
	row = find_remembered_printer(state);
	if (row >= 0) {
		return &state->printer_items[row];
	}
	return NULL;
}

static void refresh_printer_list(PortToolState *state)
{
	DWORD index;
	int previous_index = find_remembered_printer(state);
	wchar_t previous_server[128] = { 0 };
	wchar_t previous_printer[256] = { 0 };
	unsigned int previous_raw_port = 0;
	int selected_item = -1;

	if (previous_index >= 0) {
		const PortToolPrinter *previous =
			&state->printer_items[previous_index];

		wcsncpy_s(previous_server, UI_ARRAY_COUNT(previous_server),
			previous->server_name, _TRUNCATE);
		wcsncpy_s(previous_printer, UI_ARRAY_COUNT(previous_printer),
			previous->printer_name, _TRUNCATE);
		previous_raw_port = previous->raw_port;
	}
	refresh_local_ports(state);
	state->updating_printers = TRUE;
	state->visible_printer_count = 0;
	ListView_DeleteAllItems(state->printers);
	for (index = 0; index < state->printer_count; index++) {
		PortToolPrinter *printer = &state->printer_items[index];
		wchar_t port_name[MAX_PORTNAME_LEN];
		wchar_t raw_port[32];
		LVITEMW item;
		wchar_t *port_state;
		int row;

		if (state->server_count > 0 &&
			_wcsicmp(printer->server_name,
				state->server_items[state->selected_server].name) != 0) {
			continue;
		}
		build_port_name(printer, port_name, UI_ARRAY_COUNT(port_name));
		if (local_port_exists(state, port_name)) {
			port_state = L"已创建";
		}
		else if (printer->online) {
			port_state = L"可添加";
		}
		else {
			port_state = L"缓存，服务端离线";
		}
		_snwprintf_s(raw_port, UI_ARRAY_COUNT(raw_port), _TRUNCATE,
			L"%u", printer->raw_port);
		ZeroMemory(&item, sizeof(item));
		item.mask = LVIF_TEXT | LVIF_PARAM;
		item.iItem = ListView_GetItemCount(state->printers);
		item.pszText = printer->printer_name;
		item.lParam = (LPARAM)index;
		row = ListView_InsertItem(state->printers, &item);
		if (row < 0) {
			continue;
		}
		state->visible_printer_indices[state->visible_printer_count++] =
			(int)index;
		ListView_SetItemText(state->printers, row, 1,
			printer->driver[0] != L'\0' ? printer->driver : L"-");
		ListView_SetItemText(state->printers, row, 2,
			printer->server_name);
		ListView_SetItemText(state->printers, row, 3, raw_port);
		ListView_SetItemText(state->printers, row, 4, port_name);
		ListView_SetItemText(state->printers, row, 5, port_state);
		if (previous_index >= 0 && selected_item < 0 &&
			_wcsicmp(printer->server_name, previous_server) == 0 &&
			_wcsicmp(printer->printer_name, previous_printer) == 0 &&
			printer->raw_port == previous_raw_port) {
			selected_item = row;
		}
	}
	if (selected_item < 0 && ListView_GetItemCount(state->printers) > 0) {
		selected_item = 0;
	}
	if (selected_item >= 0) {
		ListView_SetItemState(state->printers, selected_item,
			LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		remember_selected_printer(state,
			state->visible_printer_indices[selected_item]);
	}
	else {
		clear_selected_printer(state);
	}
	EnableWindow(state->add_port, selected_item >= 0);
	EnableWindow(state->delete_port, selected_item >= 0);
	EnableWindow(state->test_connection,
		selected_item >= 0 && state->test_thread == NULL);
	state->updating_printers = FALSE;
}

static void select_server(PortToolState *state, unsigned int index)
{
	if (index >= state->server_count) {
		return;
	}
	state->selected_server = index;
	clear_selected_printer(state);
	refresh_printer_list(state);
}

static void add_selected_port(PortToolState *state)
{
	const PortToolPrinter *printer = selected_printer(state);
	const wchar_t *target_host;
	wchar_t port_name[MAX_PORTNAME_LEN];
	DWORD error = ERROR_SUCCESS;

	if (printer == NULL) {
		return;
	}
	build_port_name(printer, port_name, UI_ARRAY_COUNT(port_name));
	if (local_port_exists(state, port_name)) {
		MessageBoxW(state->window,
			L"该 Standard TCP/IP 端口已经存在，无需重复添加。",
			PORT_TOOL_MESSAGE_TITLE, MB_OK | MB_ICONINFORMATION);
		return;
	}
	if (!printer->online &&
		MessageBoxW(state->window,
			L"服务端当前显示为离线。是否仍按缓存信息创建端口？",
			PORT_TOOL_MESSAGE_TITLE,
			MB_YESNO | MB_ICONWARNING) != IDYES) {
		return;
	}
	target_host = is_usable_server_ipv4(printer->address) ?
		printer->address : printer->server_name;
	if (create_standard_tcp_port(port_name, target_host,
		printer->raw_port, &error)) {
		log_text(state, L"已创建端口 %ls -> %ls:%u。", port_name,
		target_host, printer->raw_port);
		refresh_local_ports(state);
		refresh_printer_list(state);
		MessageBoxW(state->window,
			L"Standard TCP/IP 端口已创建。\r\n\r\n"
			L"请到“设备和打印机”中添加打印机并选择此端口。",
			PORT_TOOL_MESSAGE_TITLE, MB_OK | MB_ICONINFORMATION);
	}
	else {
		log_text(state, L"创建端口 %ls 失败，错误 %lu。", port_name,
			(unsigned long)error);
		SetLastError(error);
		UiShowLastError(state->window, L"创建 Standard TCP/IP 端口");
	}
}

static void delete_selected_port(PortToolState *state)
{
	const PortToolPrinter *printer = selected_printer(state);
	wchar_t port_name[MAX_PORTNAME_LEN];
	wchar_t message[512];
	DWORD error = ERROR_SUCCESS;

	if (printer == NULL) {
		return;
	}
	build_port_name(printer, port_name, UI_ARRAY_COUNT(port_name));
	if (!local_port_exists(state, port_name)) {
		MessageBoxW(state->window, L"该端口当前不存在。",
			PORT_TOOL_MESSAGE_TITLE,
			MB_OK | MB_ICONINFORMATION);
		return;
	}
	_snwprintf_s(message, UI_ARRAY_COUNT(message), _TRUNCATE,
		L"确定删除本机端口“%ls”吗？\r\n\r\n"
		L"如果已有打印队列使用该端口，删除可能失败。",
		port_name);
	if (MessageBoxW(state->window, message, PORT_TOOL_MESSAGE_TITLE,
		MB_YESNO | MB_ICONQUESTION) != IDYES) {
		return;
	}
	if (delete_standard_tcp_port(port_name, &error)) {
		log_text(state, L"已删除端口 %ls。", port_name);
		refresh_local_ports(state);
		refresh_printer_list(state);
	}
	else {
		log_text(state, L"删除端口 %ls 失败，错误 %lu。", port_name,
			(unsigned long)error);
		SetLastError(error);
		UiShowLastError(state->window, L"删除 Standard TCP/IP 端口");
	}
}

static void open_printer_management(PortToolState *state)
{
	HINSTANCE result = ShellExecuteW(state->window, L"open",
		L"control.exe", L"printers", NULL, SW_SHOWNORMAL);

	if ((INT_PTR)result <= 32) {
		MessageBoxW(state->window, L"无法打开设备和打印机。",
			PORT_TOOL_MESSAGE_TITLE, MB_OK | MB_ICONERROR);
	}
}

static void toggle_startup(PortToolState *state)
{
	LRESULT checked = SendMessageW(state->autostart, BM_GETCHECK, 0, 0);
	wchar_t executable[MAX_PATH];
	DWORD error = ERROR_SUCCESS;
	BOOL ok;

	if (!GetModuleFileNameW(NULL, executable,
		(DWORD)UI_ARRAY_COUNT(executable))) {
		log_text(state, L"读取程序路径失败，错误 %lu。", GetLastError());
		return;
	}
	if (checked == BST_CHECKED) {
		ok = UiCreateLogonTask(PORT_TOOL_TASK_NAME, executable,
			L"/autostart", L"打印机内网共享配置工具端开机启动",
			&error);
	}
	else {
		ok = UiDeleteLogonTask(PORT_TOOL_TASK_NAME, &error);
	}
	if (!ok) {
		SendMessageW(state->autostart, BM_SETCHECK,
			checked == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
		log_text(state, L"更新开机启动失败，错误 %lu。", error);
		return;
	}
	log_text(state, checked == BST_CHECKED ?
		L"已启用开机启动并驻留托盘。" : L"已关闭开机启动。");
}

static HWND make_control(PortToolState *state, const wchar_t *class_name,
	const wchar_t *text, DWORD style, DWORD ex_style, int x, int y,
	int width, int height, int id)
{
	HWND control = CreateWindowExW(ex_style, class_name, text,
		WS_CHILD | WS_VISIBLE | style, x, y, width, height, state->window,
		(HMENU)(INT_PTR)id, NULL, NULL);

	if (control != NULL) {
		SendMessageW(control, WM_SETFONT, (WPARAM)state->font, TRUE);
	}
	return control;
}

static void layout(PortToolState *state)
{
	RECT rect;
	int width;
	int height;
	int list_height;

	GetClientRect(state->window, &rect);
	width = rect.right - rect.left;
	height = rect.bottom - rect.top;
	if (width < 980) {
		width = 980;
	}
	if (height < 620) {
		height = 620;
	}
	MoveWindow(state->status, 14, 12, width - 28, 24, TRUE);
	MoveWindow(state->servers, 14, 42, 520, 240, TRUE);
	MoveWindow(state->manual_host, 548, 42, 250, 26, TRUE);
	MoveWindow(state->connect, 806, 41, 110, 28, TRUE);
	MoveWindow(state->refresh, 924, 41, 112, 28, TRUE);
	MoveWindow(state->autostart, 548, 78, 300, 24, TRUE);
	MoveWindow(state->clear_cache, 858, 76, 178, 28, TRUE);
	MoveWindow(state->open_printers, 548, 108, 210, 28, TRUE);
	MoveWindow(state->add_port, 766, 108, 130, 28, TRUE);
	MoveWindow(state->test_connection, 904, 108, 116, 28, TRUE);
	MoveWindow(state->delete_port, 766, 142, 132, 28, TRUE);
	MoveWindow(state->exit_button, 920, 142, 116, 28, TRUE);
	list_height = height - 350;
	if (list_height < 250) {
		list_height = 250;
	}
	MoveWindow(state->printers, 14, 170, width - 28, list_height, TRUE);
	MoveWindow(state->log, 14, 182 + list_height, width - 28,
		height - list_height - 196, TRUE);
}

static BOOL create_controls(PortToolState *state)
{
	LVCOLUMNW column;

	state->status = make_control(state, L"STATIC",
		L"正在读取缓存并等待局域网服务端广播...",
		SS_LEFT, 0, 14, 12, 900, 24, PORT_TOOL_IDC_STATUS);
	state->servers = make_control(state, L"COMBOBOX", L"",
		CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
		WS_EX_CLIENTEDGE, 14, 42, 520, 240, PORT_TOOL_IDC_SERVERS);
	state->manual_host = make_control(state, L"EDIT", L"",
		ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 548, 42, 250, 26,
		PORT_TOOL_IDC_MANUAL_HOST);
	state->connect = make_control(state, L"BUTTON", L"名称连接",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 806, 41, 110, 28,
		PORT_TOOL_IDC_CONNECT);
	state->refresh = make_control(state, L"BUTTON", L"刷新发现",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 924, 41, 112, 28,
		PORT_TOOL_IDC_REFRESH);
	state->autostart = make_control(state, L"BUTTON",
		L"开机启动并驻留系统托盘",
		BS_AUTOCHECKBOX | WS_TABSTOP, 0, 548, 78, 300, 24,
		PORT_TOOL_IDC_AUTOSTART);
	state->clear_cache = make_control(state, L"BUTTON", L"清理本地缓存",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 858, 76, 178, 28,
		PORT_TOOL_IDC_CLEAR_CACHE);
	state->open_printers = make_control(state, L"BUTTON",
		L"打开设备和打印机",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 548, 108, 210, 28,
		PORT_TOOL_IDC_OPEN_PRINTERS);
	state->add_port = make_control(state, L"BUTTON", L"添加所选端口",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 766, 108, 130, 28,
		PORT_TOOL_IDC_ADD_PORT);
	state->delete_port = make_control(state, L"BUTTON", L"删除所选端口",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 904, 108, 132, 28,
		PORT_TOOL_IDC_DELETE_PORT);
	state->test_connection = make_control(state, L"BUTTON", L"检测连接",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 904, 108, 116, 28,
		PORT_TOOL_IDC_TEST_CONNECTION);
	state->exit_button = make_control(state, L"BUTTON", L"退出",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 920, 142, 116, 28,
		PORT_TOOL_IDC_EXIT);
	state->printers = make_control(state, WC_LISTVIEWW, L"",
		LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
		WS_EX_CLIENTEDGE, 14, 170, 952, 250, PORT_TOOL_IDC_PRINTERS);
	state->log = make_control(state, L"EDIT", L"",
		ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
		WS_EX_CLIENTEDGE, 14, 430, 952, 130, PORT_TOOL_IDC_LOG);
	if (state->status == NULL || state->servers == NULL ||
		state->manual_host == NULL || state->connect == NULL ||
		state->refresh == NULL || state->autostart == NULL ||
		state->clear_cache == NULL ||
		state->open_printers == NULL || state->add_port == NULL ||
		state->delete_port == NULL || state->test_connection == NULL ||
		state->exit_button == NULL ||
		state->printers == NULL || state->log == NULL) {
		return FALSE;
	}
	ListView_SetExtendedListViewStyle(state->printers,
		LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
	ZeroMemory(&column, sizeof(column));
	column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
	column.cx = 225;
	column.pszText = L"打印机名称";
	ListView_InsertColumn(state->printers, 0, &column);
	column.cx = 165;
	column.pszText = L"驱动";
	ListView_InsertColumn(state->printers, 1, &column);
	column.cx = 150;
	column.pszText = L"连接主机";
	ListView_InsertColumn(state->printers, 2, &column);
	column.cx = 75;
	column.pszText = L"发布端口";
	ListView_InsertColumn(state->printers, 3, &column);
	column.cx = 195;
	column.pszText = L"本地端口名称";
	ListView_InsertColumn(state->printers, 4, &column);
	column.cx = 135;
	column.pszText = L"本地状态";
	ListView_InsertColumn(state->printers, 5, &column);
	EnableWindow(state->add_port, FALSE);
	EnableWindow(state->delete_port, FALSE);
	EnableWindow(state->test_connection, FALSE);
	return TRUE;
}

static void set_autostart_checkbox(PortToolState *state)
{
	if (UiLogonTaskExists(PORT_TOOL_TASK_NAME, NULL)) {
		SendMessageW(state->autostart, BM_SETCHECK, BST_CHECKED, 0);
	}
	else {
		SendMessageW(state->autostart, BM_SETCHECK, BST_UNCHECKED, 0);
	}
}

static void start_discovery(PortToolState *state)
{
	WSADATA winsock_data;

	if (flush_local_dns_cache()) {
		log_text(state, L"已刷新本机 DNS 解析缓存，正在检查已缓存服务端名称。");
	}
	else {
		log_text(state,
			L"刷新本机 DNS 解析缓存失败或不可用；继续检查服务端名称。");
	}
	load_cache(state);
	refresh_local_ports(state);
	{
		DWORD index;

		for (index = 0; index < state->server_count; index++) {
			check_server_dns(state, index);
		}
	}
	refresh_server_list(state);
	refresh_printer_list(state);
	set_status_text(state);
	if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
		log_text(state, L"Winsock 初始化失败，自动发现不可用。");
		return;
	}
	state->winsock_ready = TRUE;
	if (!make_discovery_socket(state)) {
		log_text(state,
			L"无法监听 UDP 3251，自动发现不可用；仍可使用名称连接。");
		return;
	}
	send_discovery_queries();
	log_text(state, L"自动发现已启动，等待 UDP 3251 服务端广播。");
}

static LRESULT handle_notify(PortToolState *state, NMHDR *header)
{
	if (state->updating_printers) {
		return FALSE;
	}
	if (header->idFrom == PORT_TOOL_IDC_PRINTERS) {
		if (header->code == LVN_ITEMCHANGED) {
			NMLISTVIEW *notification = (NMLISTVIEW *)header;

			if ((notification->uNewState & LVIS_SELECTED) != 0 &&
				notification->iItem >= 0 &&
				(DWORD)notification->iItem < state->visible_printer_count) {
				int index = state->visible_printer_indices[
					notification->iItem];

				remember_selected_printer(state, index);
				EnableWindow(state->add_port, index >= 0);
				EnableWindow(state->delete_port, index >= 0);
				EnableWindow(state->test_connection,
					index >= 0 && state->test_thread == NULL);
			}
		}
		if (header->code == NM_DBLCLK) {
			add_selected_port(state);
			return TRUE;
		}
	}
	return FALSE;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message,
	WPARAM w_param, LPARAM l_param)
{
	PortToolState *state;

	if (message == WM_NCCREATE) {
		CREATESTRUCTW *create = (CREATESTRUCTW *)l_param;
		state = (PortToolState *)create->lpCreateParams;
		state->window = window;
		SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)state);
		return TRUE;
	}
	state = get_state(window);
	switch (message) {
	case WM_CREATE:
		if (!create_controls(state)) {
			return -1;
		}
		layout(state);
		set_autostart_checkbox(state);
		start_discovery(state);
		SetTimer(window, PORT_TOOL_TIMER_DISCOVERY, 250, NULL);
		SetTimer(window, PORT_TOOL_TIMER_CACHE, 10000, NULL);
		return 0;
	case WM_SIZE:
		layout(state);
		return 0;
	case WM_GETMINMAXINFO:
		((MINMAXINFO *)l_param)->ptMinTrackSize.x = 1000;
		((MINMAXINFO *)l_param)->ptMinTrackSize.y = 640;
		return 0;
	case WM_COMMAND:
		switch (LOWORD(w_param)) {
		case PORT_TOOL_IDC_SERVERS:
			if (HIWORD(w_param) == CBN_SELCHANGE &&
				!state->updating_servers) {
				int item = (int)SendMessageW(state->servers,
					CB_GETCURSEL, 0, 0);
				if (item >= 0) {
					unsigned int selected =
						(unsigned int)SendMessageW(
							state->servers,
							CB_GETITEMDATA, item, 0);

					if (selected != state->selected_server) {
						select_server(state, selected);
					}
				}
			}
			return 0;
		case PORT_TOOL_IDC_CONNECT:
			if (HIWORD(w_param) == BN_CLICKED) {
				send_manual_query(state);
			}
			return 0;
		case PORT_TOOL_IDC_REFRESH:
			if (HIWORD(w_param) == BN_CLICKED) {
				send_discovery_queries();
				log_text(state, L"已发送局域网发现查询。");
			}
			return 0;
		case PORT_TOOL_IDC_AUTOSTART:
			if (HIWORD(w_param) == BN_CLICKED) {
				toggle_startup(state);
			}
			return 0;
		case PORT_TOOL_IDC_CLEAR_CACHE:
			if (HIWORD(w_param) == BN_CLICKED) {
				clear_local_cache(state);
			}
			return 0;
		case PORT_TOOL_IDC_ADD_PORT:
			if (HIWORD(w_param) == BN_CLICKED) {
				add_selected_port(state);
			}
			return 0;
		case PORT_TOOL_IDC_TEST_CONNECTION:
			if (HIWORD(w_param) == BN_CLICKED) {
				test_selected_connection(state);
			}
			return 0;
		case PORT_TOOL_IDC_DELETE_PORT:
			if (HIWORD(w_param) == BN_CLICKED) {
				delete_selected_port(state);
			}
			return 0;
		case PORT_TOOL_IDC_OPEN_PRINTERS:
			if (HIWORD(w_param) == BN_CLICKED) {
				open_printer_management(state);
			}
			return 0;
		case PORT_TOOL_IDC_EXIT:
			if (HIWORD(w_param) == BN_CLICKED) {
				state->exiting = TRUE;
				DestroyWindow(window);
			}
			return 0;
		case UI_TRAY_COMMAND_OPEN:
			show_window(window);
			return 0;
		case UI_TRAY_COMMAND_EXIT:
			state->exiting = TRUE;
			DestroyWindow(window);
			return 0;
		default:
			break;
		}
		break;
	case PORT_TOOL_MESSAGE_TEST_COMPLETE:
		finish_connection_test(state,
			(ConnectionTestResult *)l_param);
		return 0;
	case PORT_TOOL_MESSAGE_DNS_CHECK_COMPLETE:
		finish_server_dns_check(state,
			(ServerDnsCheckResult *)l_param);
		return 0;
	case PORT_TOOL_MESSAGE_PORT_REFRESH_COMPLETE:
		finish_port_refresh(state, (PortRefreshResult *)l_param);
		return 0;
	case PORT_TOOL_MESSAGE_PORT_REFRESH_NO_MEMORY:
	{
		PortRefreshArgs *arguments = (PortRefreshArgs *)l_param;

		if (arguments->printer_index < PORT_TOOL_MAX_PRINTERS) {
			PortToolPrinter *printer =
				&state->printer_items[arguments->printer_index];

			if (printer->raw_port == arguments->raw_port &&
				_wcsicmp(printer->server_name,
					arguments->server_name) == 0 &&
				_wcsicmp(printer->printer_name,
					arguments->printer_name) == 0 &&
				printer->port_refresh_generation == arguments->generation) {
				printer->port_refresh_pending = FALSE;
				log_text(state,
					L"无法分配 %ls:%u 刷新结果内存；已解除等待状态，将在后续周期重试。",
					arguments->server_name, arguments->raw_port);
			}
		}
		HeapFree(GetProcessHeap(), 0, arguments);
		return 0;
	}
	case WM_NOTIFY:
		return handle_notify(state, (NMHDR *)l_param);
	case WM_TIMER:
		if (w_param == PORT_TOOL_TIMER_DISCOVERY) {
			pump_discovery(state);
			mark_discovery_timeouts(state);
		}
		else if (w_param == PORT_TOOL_TIMER_CACHE) {
			DWORD index;

			send_discovery_queries();
			save_cache(state);
			refresh_local_ports(state);
			for (index = 0; index < state->printer_count; index++) {
				schedule_port_refresh(state,
					&state->printer_items[index]);
			}
			refresh_server_list(state);
			set_status_text(state);
		}
		return 0;
	case UI_TRAY_CALLBACK_MESSAGE:
		if (LOWORD(l_param) == WM_LBUTTONDBLCLK) {
			show_window(window);
		}
		else if (LOWORD(l_param) == WM_RBUTTONUP ||
			LOWORD(l_param) == WM_CONTEXTMENU) {
			UiTrayShowContextMenu(window);
		}
		return 0;
	case WM_CLOSE:
		if (!state->exiting) {
			ShowWindow(window, SW_HIDE);
			return 0;
		}
		DestroyWindow(window);
		return 0;
	case WM_QUERYENDSESSION:
		save_cache(state);
		return TRUE;
	case WM_DESTROY:
		KillTimer(window, PORT_TOOL_TIMER_DISCOVERY);
		KillTimer(window, PORT_TOOL_TIMER_CACHE);
		EnterCriticalSection(&state->port_refresh_lock);
		InterlockedExchange(&state->port_refresh_shutdown, 1);
		SetEvent(state->port_refresh_stop_event);
		LeaveCriticalSection(&state->port_refresh_lock);
		WaitForSingleObject(state->port_refresh_idle_event, INFINITE);
		{
			MSG pending_message;

			while (PeekMessageW(&pending_message, window,
				PORT_TOOL_MESSAGE_PORT_REFRESH_COMPLETE,
				PORT_TOOL_MESSAGE_PORT_REFRESH_NO_MEMORY, PM_REMOVE)) {
				if (pending_message.message ==
					PORT_TOOL_MESSAGE_PORT_REFRESH_COMPLETE) {
					HeapFree(GetProcessHeap(), 0,
						(void *)pending_message.lParam);
				}
				else if (pending_message.message ==
					PORT_TOOL_MESSAGE_PORT_REFRESH_NO_MEMORY) {
					HeapFree(GetProcessHeap(), 0,
						(void *)pending_message.lParam);
				}
			}
		}
		if (state->test_cancel_event != NULL) {
			SetEvent(state->test_cancel_event);
			CloseHandle(state->test_cancel_event);
			state->test_cancel_event = NULL;
		}
		if (state->test_thread != NULL) {
			CloseHandle(state->test_thread);
			state->test_thread = NULL;
		}
		save_cache(state);
		close_discovery_socket(state);
		UiTrayRemove(window);
		if (state->port_refresh_stop_event != NULL) {
			CloseHandle(state->port_refresh_stop_event);
			state->port_refresh_stop_event = NULL;
		}
		if (state->port_refresh_idle_event != NULL) {
			CloseHandle(state->port_refresh_idle_event);
			state->port_refresh_idle_event = NULL;
		}
		if (state->port_refresh_lock_ready) {
			DeleteCriticalSection(&state->port_refresh_lock);
			state->port_refresh_lock_ready = FALSE;
		}
		PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(window, message, w_param, l_param);
}

static int command_line_mode(HINSTANCE instance)
{
	wchar_t path[MAX_PATH];
	BOOL install = has_argument(L"/install-shortcuts");
	BOOL remove = has_argument(L"/remove-shortcuts");
	BOOL install_autostart = has_argument(L"/install-autostart");
	BOOL remove_autostart = has_argument(L"/remove-autostart");
	DWORD error = ERROR_SUCCESS;

	if (!install && !remove && !install_autostart && !remove_autostart) {
		return -1;
	}
	if (!GetModuleFileNameW(instance, path, UI_ARRAY_COUNT(path))) {
		return 1;
	}
	if (install_autostart) {
		return UiCreateLogonTask(PORT_TOOL_TASK_NAME, path, L"/autostart",
			L"打印机内网共享配置工具端开机启动并驻留托盘",
			&error) ? 0 : 1;
	}
	if (remove_autostart) {
		return UiDeleteLogonTask(PORT_TOOL_TASK_NAME, &error) ? 0 : 1;
	}
	if (install) {
		return UiInstallShortcuts(path, PORT_TOOL_SHORTCUT_NAME,
			L"打印机内网共享配置工具端") ? 0 : 1;
	}
	return UiRemoveShortcuts(PORT_TOOL_SHORTCUT_NAME) ? 0 : 1;
}

static int command_add_port(void)
{
	wchar_t *host = argument_after(L"/add-port");
	wchar_t *port = argument_after(L"/port");
	wchar_t *port_name = argument_after(L"/port-name");
	wchar_t generated_name[MAX_PORTNAME_LEN];
	wchar_t target_host[64];
	wchar_t printer_name[256];
	PortToolPrinter printer;
	DWORD error = ERROR_SUCCESS;
	unsigned int raw_port;
	int result;

	if (host == NULL || port == NULL) {
		MessageBoxW(NULL,
			L"用法：usbrelay-port-tool-ui.exe /add-port 计算机名 "
			L"/port 9100 [/port-name 自定义端口名]",
			PORT_TOOL_MESSAGE_TITLE, MB_OK | MB_ICONINFORMATION);
		return 2;
	}
	ZeroMemory(&printer, sizeof(printer));
	wcsncpy_s(target_host, UI_ARRAY_COUNT(target_host), host, _TRUNCATE);
	if (resolve_server_ipv4(host, target_host,
		UI_ARRAY_COUNT(target_host))) {
		wcsncpy_s(printer.address, UI_ARRAY_COUNT(printer.address),
			target_host, _TRUNCATE);
	}
	raw_port = (unsigned int)wcstoul(port, NULL, 10);
	wcsncpy_s(printer.server_name, UI_ARRAY_COUNT(printer.server_name),
		host, _TRUNCATE);
	wcsncpy_s(printer_name, UI_ARRAY_COUNT(printer_name), host,
		_TRUNCATE);
	wcsncat_s(printer_name, UI_ARRAY_COUNT(printer_name), L" raw printer",
		_TRUNCATE);
	wcsncpy_s(printer.printer_name,
		UI_ARRAY_COUNT(printer.printer_name), printer_name, _TRUNCATE);
	printer.raw_port = raw_port;
	if (port_name != NULL && port_name[0] != L'\0') {
		wcsncpy_s(generated_name, UI_ARRAY_COUNT(generated_name),
			port_name, _TRUNCATE);
	}
	else {
		build_port_name(&printer, generated_name,
			UI_ARRAY_COUNT(generated_name));
	}
	result = create_standard_tcp_port(generated_name, target_host, raw_port,
		&error) ? 0 : 1;
	if (result == 0) {
		wprintf(L"Port created: %ls -> %ls:%u\n", generated_name,
			target_host, raw_port);
	}
	else {
		fwprintf(stderr, L"Port creation failed: %lu\n",
			(unsigned long)error);
	}
	if (host != NULL) {
		free(host);
	}
	if (port != NULL) {
		free(port);
	}
	if (port_name != NULL) {
		free(port_name);
	}
	return result;
}

static int command_delete_port(void)
{
	wchar_t *port_name = argument_after(L"/delete-port");
	DWORD error = ERROR_SUCCESS;
	int result;

	if (port_name == NULL || port_name[0] == L'\0') {
		MessageBoxW(NULL,
			L"用法：usbrelay-port-tool-ui.exe /delete-port 端口名",
			PORT_TOOL_MESSAGE_TITLE, MB_OK | MB_ICONINFORMATION);
		return 2;
	}
	result = delete_standard_tcp_port(port_name, &error) ? 0 : 1;
	if (result == 0) {
		wprintf(L"Port deleted: %ls\n", port_name);
	}
	else {
		fwprintf(stderr, L"Port deletion failed: %lu\n",
			(unsigned long)error);
	}
	free(port_name);
	return result;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous,
	wchar_t *command_line, int show_command)
{
	WNDCLASSEXW window_class;
	INITCOMMONCONTROLSEX controls;
	MSG message;
	HANDLE mutex;
	HWND existing;
	int command_result;

	UNREFERENCED_PARAMETER(previous);
	UNREFERENCED_PARAMETER(command_line);
	if (has_argument(L"/add-port")) {
		return command_add_port();
	}
	if (has_argument(L"/delete-port")) {
		return command_delete_port();
	}
	command_result = command_line_mode(instance);
	if (command_result >= 0) {
		return command_result;
	}
	mutex = CreateMutexW(NULL, TRUE, PORT_TOOL_MUTEX_NAME);
	if (mutex == NULL) {
		return 1;
	}
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		existing = FindWindowW(PORT_TOOL_WINDOW_CLASS, NULL);
		if (existing != NULL) {
			show_window(existing);
		}
		CloseHandle(mutex);
		return 0;
	}
	controls.dwSize = sizeof(controls);
	controls.dwICC = ICC_LISTVIEW_CLASSES;
	if (!InitCommonControlsEx(&controls)) {
		CloseHandle(mutex);
		return 1;
	}
	ZeroMemory(&g_state, sizeof(g_state));
	InitializeCriticalSection(&g_state.port_refresh_lock);
	g_state.port_refresh_lock_ready = TRUE;
	g_state.port_refresh_idle_event = CreateEventW(NULL, TRUE, TRUE, NULL);
	g_state.port_refresh_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (g_state.port_refresh_idle_event == NULL ||
		g_state.port_refresh_stop_event == NULL) {
		if (g_state.port_refresh_idle_event != NULL) {
			CloseHandle(g_state.port_refresh_idle_event);
		}
		if (g_state.port_refresh_stop_event != NULL) {
			CloseHandle(g_state.port_refresh_stop_event);
		}
		DeleteCriticalSection(&g_state.port_refresh_lock);
		CloseHandle(mutex);
		return 1;
	}
	g_state.discovery_socket = INVALID_SOCKET;
	g_state.selected_printer = -1;
	g_state.hidden_start = has_argument(L"/autostart");
	g_state.font = UiCreateInterfaceFont();
	if (g_state.font == NULL) {
		CloseHandle(g_state.port_refresh_stop_event);
		CloseHandle(g_state.port_refresh_idle_event);
		DeleteCriticalSection(&g_state.port_refresh_lock);
		CloseHandle(mutex);
		return 1;
	}
	ZeroMemory(&window_class, sizeof(window_class));
	window_class.cbSize = sizeof(window_class);
	window_class.lpfnWndProc = window_proc;
	window_class.hInstance = instance;
	window_class.hIcon = LoadIconW(instance,
		MAKEINTRESOURCEW(IDI_APP_ICON));
	window_class.hIconSm = (HICON)LoadImageW(instance,
		MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON),
		GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
	window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
	window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	window_class.lpszClassName = PORT_TOOL_WINDOW_CLASS;
	if (!RegisterClassExW(&window_class)) {
		DeleteObject(g_state.font);
		CloseHandle(g_state.port_refresh_stop_event);
		CloseHandle(g_state.port_refresh_idle_event);
		DeleteCriticalSection(&g_state.port_refresh_lock);
		CloseHandle(mutex);
		return 1;
	}
	g_state.window = CreateWindowExW(0, PORT_TOOL_WINDOW_CLASS,
		PORT_TOOL_WINDOW_TITLE,
		WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 1080, 680,
		NULL, NULL, instance, &g_state);
	if (g_state.window == NULL) {
		DeleteObject(g_state.font);
		if (g_state.port_refresh_stop_event != NULL) {
			CloseHandle(g_state.port_refresh_stop_event);
		}
		if (g_state.port_refresh_idle_event != NULL) {
			CloseHandle(g_state.port_refresh_idle_event);
		}
		if (g_state.port_refresh_lock_ready) {
			DeleteCriticalSection(&g_state.port_refresh_lock);
		}
		CloseHandle(mutex);
		return 1;
	}
	UiCenterWindow(g_state.window);
	g_state.tray_icon = LoadIconW(instance,
		MAKEINTRESOURCEW(IDI_APP_ICON));
	g_state.tray_added = g_state.tray_icon != NULL &&
		UiTrayAdd(g_state.window, g_state.tray_icon,
			L"打印机内网共享配置工具端 - 双击打开");
	if (g_state.hidden_start) {
		ShowWindow(g_state.window, SW_HIDE);
	}
	else {
		ShowWindow(g_state.window,
			show_command == SW_HIDE ? SW_SHOWNORMAL : show_command);
	}
	UpdateWindow(g_state.window);
	while (GetMessageW(&message, NULL, 0, 0) > 0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	DeleteObject(g_state.font);
	CloseHandle(mutex);
	return (int)message.wParam;
}
