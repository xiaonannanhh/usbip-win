#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <winspool.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#include "../../../include/usbrelay_standard_protocol.h"
#include "usbrelay_standard_server.h"

#define STANDARD_MAX_PRINTERS 1024
#define STANDARD_MAX_BROADCAST_TARGETS 64
#define STANDARD_REFRESH_INTERVAL_MS 10000
#define STANDARD_DISCOVERY_INTERVAL_MS 2000
#define STANDARD_SOCKET_TIMEOUT_MS (5 * 60 * 1000)
#define STANDARD_FIRST_DATA_TIMEOUT_MS 500
#define STANDARD_IDLE_FINALIZE_MS 50
#define STANDARD_CLIENT_BUFFER_SIZE (64 * 1024)
#define STANDARD_LOG_MAX_BYTES (8 * 1024 * 1024)
#define STANDARD_LOG_PATH_CHARS 1024
#define STANDARD_RUNTIME_KEY \
	USBRELAY_STANDARD_REGISTRY_PATH L"\\Runtime"
#define STANDARD_REGISTRY_QUERY_ACCESS \
	(KEY_QUERY_VALUE | KEY_WOW64_64KEY)
#define STANDARD_REGISTRY_SET_ACCESS \
	(KEY_SET_VALUE | KEY_WOW64_64KEY)
#define STANDARD_REGISTRY_ENUM_ACCESS \
	(KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY)

typedef struct StandardEndpoint StandardEndpoint;
typedef struct StandardClient StandardClient;
typedef struct StandardPrintJob StandardPrintJob;

struct StandardPrintJob {
	StandardPrintJob *next;
	HANDLE file_handle;
	LONG task_id;
	char remote_ip[INET_ADDRSTRLEN];
	char remote_host[NI_MAXHOST];
	ULONGLONG bytes_received;
};

struct StandardEndpoint {
	StandardEndpoint *next;
	wchar_t device_key[USBRELAY_STANDARD_DEVICE_KEY_CHARS];
	wchar_t printer_name[USBRELAY_STANDARD_NAME_CHARS];
	wchar_t driver[USBRELAY_STANDARD_DRIVER_CHARS];
	wchar_t original_port[USBRELAY_STANDARD_PORT_NAME_CHARS];
	unsigned short raw_port;
	volatile LONG stopping;
	volatile LONG listening;
	SOCKET listen_socket;
	HANDLE accept_thread;
	HANDLE worker_thread;
	HANDLE queue_event;
	HANDLE clients_done;
	CRITICAL_SECTION queue_lock;
	StandardPrintJob *queue_head;
	StandardPrintJob *queue_tail;
	volatile LONG worker_stopping;
	LONG client_count;
	DWORD last_error;
};

struct StandardClient {
	StandardClient *next;
	StandardEndpoint *endpoint;
	SOCKET socket_handle;
	LONG task_id;
	char remote_ip[INET_ADDRSTRLEN];
	char remote_host[NI_MAXHOST];
	ULONGLONG bytes_received;
};

typedef struct StandardBroadcastTarget {
	struct sockaddr_in address;
	char local_address[16];
} StandardBroadcastTarget;

static CRITICAL_SECTION g_client_lock;
static StandardClient *g_clients;
static StandardEndpoint *g_endpoints;
static HANDLE g_stop_event;
static HANDLE g_core_mutex;
static SOCKET g_discovery_socket = INVALID_SOCKET;
static volatile LONG g_stopping;
static volatile LONG g_initialized;
static volatile LONG g_next_task_id;
static HANDLE g_log_mutex;
static wchar_t g_computer_name[USBRELAY_STANDARD_NAME_CHARS];
static wchar_t g_log_path[STANDARD_LOG_PATH_CHARS];
static CRITICAL_SECTION g_log_lock;

static BOOL wide_to_utf8(const wchar_t *source, char *destination,
	int destination_size);

static void standard_log(const char *message)
{
	SYSTEMTIME time;
	char line[2300];
	HANDLE file;
	LARGE_INTEGER size;
	DWORD written;
	DWORD wait_result;
	int length;

	if (message == NULL) {
		message = "";
	}
	fprintf(stderr, "[打印机内网共享] %s\n", message);
	if (!InterlockedCompareExchange(&g_initialized, 0, 0) ||
		g_log_path[0] == L'\0') {
		return;
	}
	GetLocalTime(&time);
	length = _snprintf_s(line, sizeof(line), _TRUNCATE,
		"%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n",
		(unsigned int)time.wYear, (unsigned int)time.wMonth,
		(unsigned int)time.wDay, (unsigned int)time.wHour,
		(unsigned int)time.wMinute, (unsigned int)time.wSecond,
		(unsigned int)time.wMilliseconds, message);
	if (length <= 0) {
		return;
	}
	EnterCriticalSection(&g_log_lock);
	if (g_log_mutex == NULL) {
		LeaveCriticalSection(&g_log_lock);
		return;
	}
	wait_result = WaitForSingleObject(g_log_mutex, INFINITE);
	if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
		LeaveCriticalSection(&g_log_lock);
		return;
	}
	file = CreateFileW(g_log_path, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
		OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE) {
		if (!GetFileSizeEx(file, &size)) {
			CloseHandle(file);
			file = INVALID_HANDLE_VALUE;
		}
		if (file != INVALID_HANDLE_VALUE &&
			size.QuadPart + length > STANDARD_LOG_MAX_BYTES) {
			LARGE_INTEGER zero;
			zero.QuadPart = 0;
			SetFilePointerEx(file, zero, NULL, FILE_BEGIN);
			SetEndOfFile(file);
		}
		if (file != INVALID_HANDLE_VALUE) {
			LARGE_INTEGER end;
			end.QuadPart = 0;
			SetFilePointerEx(file, end, NULL, FILE_END);
			WriteFile(file, line, (DWORD)length, &written, NULL);
			CloseHandle(file);
		}
	}
	ReleaseMutex(g_log_mutex);
	LeaveCriticalSection(&g_log_lock);
}

static void standard_logf(const char *format, ...)
{
	char message[2048];
	va_list args;

	va_start(args, format);
	_vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
	va_end(args);
	standard_log(message);
}

static void standard_logw(const wchar_t *format, ...)
{
	wchar_t wide_message[2048];
	char message[8192];
	va_list args;

	va_start(args, format);
	_vsnwprintf_s(wide_message, _countof(wide_message), _TRUNCATE,
		format, args);
	va_end(args);
	if (wide_to_utf8(wide_message, message, (int)sizeof(message))) {
		standard_log(message);
	}
}

static void initialize_log_path(void)
{
	wchar_t program_data[MAX_PATH];
	wchar_t usbrelay_directory[STANDARD_LOG_PATH_CHARS];
	wchar_t server_directory[STANDARD_LOG_PATH_CHARS];
	DWORD length;

	ZeroMemory(g_log_path, sizeof(g_log_path));
	length = GetEnvironmentVariableW(L"ProgramData", program_data,
		(DWORD)_countof(program_data));
	if (length == 0 || length >= _countof(program_data)) {
		length = GetTempPathW((DWORD)_countof(program_data), program_data);
		if (length == 0 || length >= _countof(program_data)) {
			return;
		}
		if (program_data[length - 1] == L'\\') {
			program_data[length - 1] = L'\0';
		}
	}
	_snwprintf_s(g_log_path, _countof(g_log_path), _TRUNCATE,
		L"%ls\\USBRelay\\StandardServer\\server-task-log.txt",
		program_data);
	_snwprintf_s(usbrelay_directory, _countof(usbrelay_directory),
		_TRUNCATE, L"%ls\\USBRelay", program_data);
	_snwprintf_s(server_directory, _countof(server_directory),
		_TRUNCATE, L"%ls\\StandardServer", usbrelay_directory);
	CreateDirectoryW(usbrelay_directory, NULL);
	CreateDirectoryW(server_directory, NULL);
}

static BOOL wide_to_utf8(const wchar_t *source, char *destination,
	int destination_size)
{
	int result;

	if (destination == NULL || destination_size <= 0) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	destination[0] = '\0';
	if (source == NULL) {
		source = L"";
	}
	result = WideCharToMultiByte(CP_UTF8, 0, source, -1, destination,
		destination_size, NULL, NULL);
	if (result <= 0) {
		destination[0] = '\0';
		return FALSE;
	}
	destination[destination_size - 1] = '\0';
	return TRUE;
}

static BOOL percent_encode_utf8(const wchar_t *source, char *destination,
	size_t destination_count)
{
	char utf8[2048];
	size_t input;
	size_t output = 0;

	if (source == NULL) {
		source = L"";
	}
	if (!wide_to_utf8(source, utf8, (int)sizeof(utf8))) {
		return FALSE;
	}
	for (input = 0; utf8[input] != '\0'; input++) {
		unsigned char character = (unsigned char)utf8[input];
		BOOL safe = (character >= 'a' && character <= 'z') ||
			(character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') ||
			character == '-' || character == '_' ||
			character == '.' || character == ':' || character == ' ';

		if (safe) {
			if (output + 1 >= destination_count) {
				SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return FALSE;
			}
			destination[output++] = (char)character;
		}
		else {
			static const char hex[] = "0123456789ABCDEF";
			if (output + 3 >= destination_count) {
				SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return FALSE;
			}
			destination[output++] = '%';
			destination[output++] = hex[(character >> 4) & 0x0f];
			destination[output++] = hex[character & 0x0f];
		}
	}
	destination[output] = '\0';
	return TRUE;
}

static BOOL standard_get_computer_name(wchar_t *name, size_t name_chars)
{
	DWORD length;

	if (name == NULL || name_chars < 2) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	length = (DWORD)name_chars;
	if (!GetComputerNameW(name, &length)) {
		name[0] = L'\0';
		return FALSE;
	}
	name[name_chars - 1] = L'\0';
	return name[0] != L'\0';
}

static BOOL enum_local_printers(BYTE **buffer, DWORD *count)
{
	DWORD needed = 0;
	DWORD returned = 0;
	BYTE *data;

	if (buffer == NULL || count == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*buffer = NULL;
	*count = 0;
	SetLastError(ERROR_SUCCESS);
	if (!EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, NULL, 0, &needed,
		&returned) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
		return FALSE;
	}
	if (needed == 0) {
		return TRUE;
	}
	data = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
	if (data == NULL) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	if (!EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, data, needed,
		&needed, &returned)) {
		DWORD error = GetLastError();
		HeapFree(GetProcessHeap(), 0, data);
		SetLastError(error);
		return FALSE;
	}
	*buffer = data;
	*count = returned;
	return TRUE;
}

static unsigned int stable_text_hash(const wchar_t *text,
	unsigned int hash)
{
	while (text != NULL && *text != L'\0') {
		hash ^= (unsigned int)*text++;
		hash *= 16777619u;
	}
	return hash;
}

static BOOL query_device_interface_id(const wchar_t *printer_name,
	wchar_t *device_key, size_t device_key_chars)
{
	HANDLE printer = NULL;
	DWORD type = 0;
	DWORD value_size;
	DWORD needed = 0;
	DWORD status;

	if (printer_name == NULL || printer_name[0] == L'\0' ||
		device_key == NULL || device_key_chars < 2) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	device_key[0] = L'\0';
	if (!OpenPrinterW((LPWSTR)printer_name, &printer, NULL)) {
		return FALSE;
	}
	value_size = (DWORD)(device_key_chars * sizeof(*device_key));
	status = GetPrinterDataW(printer, L"DeviceInterfaceId", &type,
		(LPBYTE)device_key, value_size, &needed);
	ClosePrinter(printer);
	if (status != ERROR_SUCCESS || type != REG_SZ ||
		device_key[0] == L'\0') {
		device_key[0] = L'\0';
		SetLastError(status == ERROR_SUCCESS ? ERROR_INVALID_DATA :
			status);
		return FALSE;
	}
	device_key[device_key_chars - 1] = L'\0';
	return TRUE;
}

static void build_printer_device_key(const PRINTER_INFO_2W *info,
	wchar_t *device_key, size_t device_key_chars)
{
	unsigned int hash = 2166136261u;

	if (device_key == NULL || device_key_chars < 2) {
		return;
	}
	if (info != NULL && query_device_interface_id(info->pPrinterName,
		device_key, device_key_chars)) {
		return;
	}
	hash = stable_text_hash(info != NULL ? info->pPrinterName : NULL,
		hash);
	hash = stable_text_hash(info != NULL ? info->pDriverName : NULL,
		hash);
	hash = stable_text_hash(info != NULL ? info->pPortName : NULL,
		hash);
	_snwprintf_s(device_key, device_key_chars, _TRUNCATE,
		L"fallback-%08X", hash);
}

static HANDLE acquire_config_mutex(void)
{
	HANDLE mutex = CreateMutexW(NULL, FALSE,
		USBRELAY_STANDARD_CONFIG_MUTEX_NAME);
	DWORD wait_result;

	if (mutex == NULL) {
		return NULL;
	}
	wait_result = WaitForSingleObject(mutex, 15000);
	if (wait_result != WAIT_OBJECT_0 &&
		wait_result != WAIT_ABANDONED) {
		CloseHandle(mutex);
		SetLastError(wait_result == WAIT_TIMEOUT ?
			ERROR_TIMEOUT : ERROR_GEN_FAILURE);
		return NULL;
	}
	return mutex;
}

static void release_config_mutex(HANDLE mutex)
{
	if (mutex != NULL) {
		ReleaseMutex(mutex);
		CloseHandle(mutex);
	}
}

static BOOL query_port_mapping(const wchar_t *mapping_key,
	unsigned short *port)
{
	HKEY key;
	DWORD type = 0;
	DWORD value = 0;
	DWORD value_size = sizeof(value);
	LONG status;

	status = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		USBRELAY_STANDARD_PORTS_REGISTRY_PATH, 0,
		STANDARD_REGISTRY_QUERY_ACCESS, &key);
	if (status != ERROR_SUCCESS) {
		SetLastError((DWORD)status);
		return FALSE;
	}
	status = RegQueryValueExW(key, mapping_key, NULL, &type,
		(BYTE *)&value, &value_size);
	RegCloseKey(key);
	if (status != ERROR_SUCCESS || type != REG_DWORD ||
		value_size != sizeof(value) ||
		value < USBRELAY_STANDARD_PORT_BASE ||
		value > USBRELAY_STANDARD_PORT_LAST) {
		SetLastError(status == ERROR_SUCCESS ? ERROR_INVALID_DATA :
			(DWORD)status);
		return FALSE;
	}
	*port = (unsigned short)value;
	return TRUE;
}

static BOOL write_port_mapping(const wchar_t *mapping_key,
	unsigned short port)
{
	HKEY key;
	DWORD value = port;
	LONG status;

	status = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
		USBRELAY_STANDARD_PORTS_REGISTRY_PATH, 0, NULL,
		REG_OPTION_NON_VOLATILE, STANDARD_REGISTRY_SET_ACCESS,
		NULL, &key, NULL);
	if (status != ERROR_SUCCESS) {
		SetLastError((DWORD)status);
		return FALSE;
	}
	status = RegSetValueExW(key, mapping_key, 0, REG_DWORD,
		(const BYTE *)&value, sizeof(value));
	RegCloseKey(key);
	if (status != ERROR_SUCCESS) {
		SetLastError((DWORD)status);
		return FALSE;
	}
	return TRUE;
}

static void delete_port_mapping(const wchar_t *mapping_key)
{
	HKEY key;
	LONG status;

	status = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		USBRELAY_STANDARD_PORTS_REGISTRY_PATH, 0,
		STANDARD_REGISTRY_SET_ACCESS, &key);
	if (status != ERROR_SUCCESS) {
		return;
	}
	RegDeleteValueW(key, mapping_key);
	RegCloseKey(key);
}

static void mark_used_ports(const wchar_t *except_key,
	const wchar_t *except_legacy_name, BOOL *used)
{
	HKEY key;
	DWORD index = 0;
	LONG status;

	ZeroMemory(used, sizeof(BOOL) * USBRELAY_STANDARD_PORT_COUNT);
	status = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		USBRELAY_STANDARD_PORTS_REGISTRY_PATH, 0,
		STANDARD_REGISTRY_QUERY_ACCESS, &key);
	if (status != ERROR_SUCCESS) {
		return;
	}
	for (;;) {
		wchar_t value_name[USBRELAY_STANDARD_DEVICE_KEY_CHARS];
		DWORD value_name_chars = (DWORD)_countof(value_name);
		DWORD type = 0;
		DWORD value = 0;
		DWORD value_size = sizeof(value);

		status = RegEnumValueW(key, index++, value_name,
			&value_name_chars, NULL, &type, (BYTE *)&value,
			&value_size);
		if (status == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (status != ERROR_SUCCESS) {
			continue;
		}
		if (except_key != NULL &&
			_wcsicmp(value_name, except_key) == 0) {
			continue;
		}
		if (except_legacy_name != NULL &&
			_wcsicmp(value_name, except_legacy_name) == 0) {
			continue;
		}
		if (type == REG_DWORD && value_size == sizeof(value) &&
			value >= USBRELAY_STANDARD_PORT_BASE &&
			value <= USBRELAY_STANDARD_PORT_LAST) {
			used[value - USBRELAY_STANDARD_PORT_BASE] = TRUE;
		}
	}
	RegCloseKey(key);
}

static BOOL tcp_port_is_available(unsigned short port)
{
	DWORD size = 0;
	DWORD result;
	MIB_TCPTABLE_OWNER_PID *table;
	DWORD index;

	/*
	 * Check only active listeners instead of probing with bind().  A bind
	 * probe treats a recently closed port in TIME_WAIT as occupied, which
	 * would make persistent RAW ports move after every server restart.
	 */
	result = GetExtendedTcpTable(NULL, &size, FALSE, AF_INET,
		TCP_TABLE_OWNER_PID_LISTENER, 0);
	if (result != ERROR_INSUFFICIENT_BUFFER || size == 0) {
		SetLastError(result);
		return FALSE;
	}
	table = (MIB_TCPTABLE_OWNER_PID *)HeapAlloc(GetProcessHeap(), 0,
		size);
	if (table == NULL) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	result = GetExtendedTcpTable(table, &size, FALSE, AF_INET,
		TCP_TABLE_OWNER_PID_LISTENER, 0);
	if (result != ERROR_SUCCESS) {
		HeapFree(GetProcessHeap(), 0, table);
		SetLastError(result);
		return FALSE;
	}
	for (index = 0; index < table->dwNumEntries; index++) {
		unsigned short local_port = ntohs(
			(unsigned short)table->table[index].dwLocalPort);
		if (local_port == port) {
			HeapFree(GetProcessHeap(), 0, table);
			SetLastError(WSAEADDRINUSE);
			return FALSE;
		}
	}
	HeapFree(GetProcessHeap(), 0, table);
	return TRUE;
}

static BOOL assign_printer_port(const wchar_t *device_key,
	const wchar_t *legacy_printer_name,
	unsigned short *port, BOOL *persistent)
{
	BOOL used[USBRELAY_STANDARD_PORT_COUNT];
	HANDLE mutex;
	unsigned int index;
	unsigned short legacy_port = 0;
	BOOL legacy_exists = FALSE;

	if (device_key == NULL || device_key[0] == L'\0' ||
		port == NULL || persistent == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*port = 0;
	*persistent = FALSE;
	mutex = acquire_config_mutex();
	if (mutex == NULL) {
		return FALSE;
	}

	mark_used_ports(device_key, legacy_printer_name, used);
	if (query_port_mapping(device_key, port)) {
		if (!used[*port - USBRELAY_STANDARD_PORT_BASE]) {
			*persistent = TRUE;
			release_config_mutex(mutex);
			return TRUE;
		}
	}
	if (legacy_printer_name != NULL &&
		legacy_printer_name[0] != L'\0' &&
		_wcsicmp(device_key, legacy_printer_name) != 0 &&
		query_port_mapping(legacy_printer_name, &legacy_port)) {
		legacy_exists = TRUE;
		if (!used[legacy_port - USBRELAY_STANDARD_PORT_BASE]) {
			if (!write_port_mapping(device_key, legacy_port)) {
				release_config_mutex(mutex);
				return FALSE;
			}
			delete_port_mapping(legacy_printer_name);
			*port = legacy_port;
			*persistent = TRUE;
			release_config_mutex(mutex);
			return TRUE;
		}
	}

	mark_used_ports(device_key, legacy_printer_name, used);
	for (index = 0; index < USBRELAY_STANDARD_PORT_COUNT; index++) {
		unsigned short candidate =
			(unsigned short)(USBRELAY_STANDARD_PORT_BASE + index);

		if (used[index] || !tcp_port_is_available(candidate)) {
			continue;
		}
		if (!write_port_mapping(device_key, candidate)) {
			release_config_mutex(mutex);
			return FALSE;
		}
		if (legacy_exists && legacy_printer_name != NULL) {
			delete_port_mapping(legacy_printer_name);
		}
		*port = candidate;
		*persistent = TRUE;
		release_config_mutex(mutex);
		return TRUE;
	}

	release_config_mutex(mutex);
	SetLastError(ERROR_NO_MORE_ITEMS);
	return FALSE;
}

static BOOL reassign_printer_port(const wchar_t *device_key,
	const wchar_t *legacy_printer_name,
	unsigned short current_port, unsigned short *new_port)
{
	BOOL used[USBRELAY_STANDARD_PORT_COUNT];
	HANDLE mutex;
	unsigned int index;

	if (device_key == NULL || device_key[0] == L'\0' ||
		new_port == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*new_port = 0;
	mutex = acquire_config_mutex();
	if (mutex == NULL) {
		return FALSE;
	}
	mark_used_ports(device_key, legacy_printer_name, used);
	if (current_port >= USBRELAY_STANDARD_PORT_BASE &&
		current_port <= USBRELAY_STANDARD_PORT_LAST) {
		used[current_port - USBRELAY_STANDARD_PORT_BASE] = TRUE;
	}
	for (index = 0; index < USBRELAY_STANDARD_PORT_COUNT; index++) {
		unsigned short candidate =
			(unsigned short)(USBRELAY_STANDARD_PORT_BASE + index);

		if (used[index] || !tcp_port_is_available(candidate)) {
			continue;
		}
		if (!write_port_mapping(device_key, candidate)) {
			release_config_mutex(mutex);
			return FALSE;
		}
		if (legacy_printer_name != NULL &&
			_wcsicmp(device_key, legacy_printer_name) != 0) {
			delete_port_mapping(legacy_printer_name);
		}
		*new_port = candidate;
		release_config_mutex(mutex);
		return TRUE;
	}
	release_config_mutex(mutex);
	SetLastError(ERROR_NO_MORE_ITEMS);
	return FALSE;
}

static BOOL write_runtime_dword(const wchar_t *name, DWORD value)
{
	HKEY key;
	LONG status;

	status = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
		STANDARD_RUNTIME_KEY, 0, NULL, REG_OPTION_NON_VOLATILE,
		STANDARD_REGISTRY_SET_ACCESS, NULL, &key, NULL);
	if (status != ERROR_SUCCESS) {
		SetLastError((DWORD)status);
		return FALSE;
	}
	status = RegSetValueExW(key, name, 0, REG_DWORD,
		(const BYTE *)&value, sizeof(value));
	RegCloseKey(key);
	if (status != ERROR_SUCCESS) {
		SetLastError((DWORD)status);
		return FALSE;
	}
	return TRUE;
}

static void clear_runtime_ports_locked(void)
{
	HKEY key;
	DWORD index = 0;
	DWORD port_name_count = 0;
	wchar_t port_names[USBRELAY_STANDARD_PORT_COUNT][16];
	LONG status;

	status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, STANDARD_RUNTIME_KEY,
		0, STANDARD_REGISTRY_ENUM_ACCESS, &key);
	if (status != ERROR_SUCCESS) {
		return;
	}
	for (;;) {
		wchar_t value_name[64];
		DWORD value_name_chars = (DWORD)_countof(value_name);

		status = RegEnumValueW(key, index++, value_name,
			&value_name_chars, NULL, NULL, NULL, NULL);
		if (status == ERROR_NO_MORE_ITEMS) {
			break;
		}
		if (status != ERROR_SUCCESS) {
			continue;
		}
		if (wcsncmp(value_name, L"Port_", 5) == 0 &&
			port_name_count < _countof(port_names)) {
			wcsncpy_s(port_names[port_name_count],
				_countof(port_names[port_name_count]),
				value_name, _TRUNCATE);
			port_name_count++;
		}
	}
	RegCloseKey(key);

	if (port_name_count == 0) {
		return;
	}
	status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, STANDARD_RUNTIME_KEY,
		0, STANDARD_REGISTRY_SET_ACCESS, &key);
	if (status != ERROR_SUCCESS) {
		return;
	}
	for (index = 0; index < port_name_count; index++) {
		DWORD value = 0;

		RegSetValueExW(key, port_names[index], 0, REG_DWORD,
			(const BYTE *)&value, sizeof(value));
	}
	RegCloseKey(key);
}

static BOOL set_runtime_running(BOOL running)
{
	HANDLE mutex = acquire_config_mutex();
	DWORD value = running ? 1 : 0;
	DWORD process_id = GetCurrentProcessId();
	BOOL success;

	if (mutex == NULL) {
		return FALSE;
	}
	if (running) {
		clear_runtime_ports_locked();
	}
	success = write_runtime_dword(L"Running", value) &&
		write_runtime_dword(L"ProcessId", process_id) &&
		write_runtime_dword(L"UpdatedTick", GetTickCount());
	release_config_mutex(mutex);
	return success;
}

static BOOL set_runtime_port_status(unsigned short port, BOOL listening)
{
	wchar_t name[32];
	DWORD value = listening ? 1 : 0;
	HANDLE mutex;
	BOOL success;

	_snwprintf_s(name, _countof(name), _TRUNCATE, L"Port_%u",
		(unsigned int)port);
	mutex = acquire_config_mutex();
	if (mutex == NULL) {
		return FALSE;
	}
	success = write_runtime_dword(name, value);
	release_config_mutex(mutex);
	return success;
}

static DWORD get_runtime_dword(const wchar_t *name, DWORD default_value)
{
	HKEY key;
	DWORD type = 0;
	DWORD value = default_value;
	DWORD value_size = sizeof(value);
	LONG status;

	status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, STANDARD_RUNTIME_KEY,
		0, STANDARD_REGISTRY_QUERY_ACCESS, &key);
	if (status != ERROR_SUCCESS) {
		return default_value;
	}
	status = RegQueryValueExW(key, name, NULL, &type,
		(BYTE *)&value, &value_size);
	RegCloseKey(key);
	if (status != ERROR_SUCCESS || type != REG_DWORD ||
		value_size != sizeof(value)) {
		return default_value;
	}
	return value;
}

static BOOL get_runtime_port_status(unsigned short port)
{
	wchar_t name[32];

	_snwprintf_s(name, _countof(name), _TRUNCATE, L"Port_%u",
		(unsigned int)port);
	return get_runtime_dword(name, 0) != 0;
}

static void set_socket_timeout(SOCKET socket_handle, DWORD milliseconds)
{
	int timeout = (int)milliseconds;

	setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
		(const char *)&timeout, sizeof(timeout));
	setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
		(const char *)&timeout, sizeof(timeout));
}

static StandardEndpoint *find_endpoint(const wchar_t *device_key)
{
	StandardEndpoint *endpoint;

	for (endpoint = g_endpoints; endpoint != NULL;
		endpoint = endpoint->next) {
		if (_wcsicmp(endpoint->device_key, device_key) == 0) {
			return endpoint;
		}
	}
	return NULL;
}

static BOOL device_key_exists(
	const UsbRelayStandardPrinterInfo *printers, DWORD printer_count,
	const wchar_t *device_key)
{
	DWORD index;

	for (index = 0; index < printer_count; index++) {
		if (_wcsicmp(printers[index].device_key, device_key) == 0) {
			return TRUE;
		}
	}
	return FALSE;
}

static void remove_client(StandardClient *client)
{
	StandardClient **cursor;

	EnterCriticalSection(&g_client_lock);
	cursor = &g_clients;
	while (*cursor != NULL) {
		if (*cursor == client) {
			*cursor = client->next;
			if (client->endpoint != NULL &&
				client->endpoint->client_count > 0) {
				client->endpoint->client_count--;
				if (client->endpoint->client_count == 0) {
					SetEvent(client->endpoint->clients_done);
				}
			}
			break;
		}
		cursor = &(*cursor)->next;
	}
	LeaveCriticalSection(&g_client_lock);
}

static BOOL write_all_printer(HANDLE printer, const BYTE *data,
	DWORD length, DWORD *error)
{
	DWORD offset = 0;

	while (offset < length) {
		DWORD written = 0;

		if (!WritePrinter(printer, (LPVOID)(data + offset),
			length - offset, &written) || written == 0) {
			*error = GetLastError();
			if (*error == ERROR_SUCCESS) {
				*error = ERROR_WRITE_FAULT;
			}
			return FALSE;
		}
		offset += written;
	}
	return TRUE;
}

static int wait_for_socket_data(SOCKET socket_handle, DWORD timeout_ms)
{
	fd_set read_set;
	struct timeval timeout;

	FD_ZERO(&read_set);
	FD_SET(socket_handle, &read_set);
	timeout.tv_sec = (long)(timeout_ms / 1000);
	timeout.tv_usec = (long)((timeout_ms % 1000) * 1000);
	return select(0, &read_set, NULL, NULL, &timeout);
}

static HANDLE create_print_job_file(void)
{
	wchar_t temporary_path[MAX_PATH];
	wchar_t temporary_file[MAX_PATH];
	DWORD path_length;
	HANDLE file_handle;

	path_length = GetTempPathW(_countof(temporary_path), temporary_path);
	if (path_length == 0 || path_length >= _countof(temporary_path)) {
		return INVALID_HANDLE_VALUE;
	}
	if (GetTempFileNameW(temporary_path, L"PRN", 0,
		temporary_file) == 0) {
		return INVALID_HANDLE_VALUE;
	}
	file_handle = CreateFileW(temporary_file,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
		NULL);
	if (file_handle == INVALID_HANDLE_VALUE) {
		DeleteFileW(temporary_file);
	}
	return file_handle;
}

static BOOL write_all_file(HANDLE file_handle, const BYTE *data,
	DWORD length, DWORD *error)
{
	DWORD offset = 0;

	while (offset < length) {
		DWORD written = 0;

		if (!WriteFile(file_handle, data + offset, length - offset,
			&written, NULL) || written == 0) {
			*error = GetLastError();
			if (*error == ERROR_SUCCESS) {
				*error = ERROR_WRITE_FAULT;
			}
			return FALSE;
		}
		offset += written;
	}
	return TRUE;
}

static BOOL enqueue_print_job(StandardEndpoint *endpoint,
	HANDLE file_handle, const StandardClient *client)
{
	StandardPrintJob *job;
	BOOL accepted = FALSE;

	if (endpoint == NULL || file_handle == NULL ||
		file_handle == INVALID_HANDLE_VALUE) {
		return FALSE;
	}
	job = (StandardPrintJob *)HeapAlloc(GetProcessHeap(),
		HEAP_ZERO_MEMORY, sizeof(*job));
	if (job == NULL) {
		return FALSE;
	}
	job->file_handle = file_handle;
	if (client != NULL) {
		job->task_id = client->task_id;
		strncpy_s(job->remote_ip, _countof(job->remote_ip),
			client->remote_ip, _TRUNCATE);
		strncpy_s(job->remote_host, _countof(job->remote_host),
			client->remote_host, _TRUNCATE);
		job->bytes_received = client->bytes_received;
	}
	EnterCriticalSection(&endpoint->queue_lock);
	if (!InterlockedCompareExchange(&endpoint->stopping, 0, 0) &&
		!InterlockedCompareExchange(&endpoint->worker_stopping, 0, 0) &&
		!InterlockedCompareExchange(&g_stopping, 0, 0)) {
		if (endpoint->queue_tail != NULL) {
			endpoint->queue_tail->next = job;
		}
		else {
			endpoint->queue_head = job;
		}
		endpoint->queue_tail = job;
		accepted = TRUE;
		SetEvent(endpoint->queue_event);
	}
	LeaveCriticalSection(&endpoint->queue_lock);
	if (!accepted) {
		HeapFree(GetProcessHeap(), 0, job);
	}
	return accepted;
}

static StandardPrintJob *dequeue_print_job(StandardEndpoint *endpoint)
{
	StandardPrintJob *job;

	EnterCriticalSection(&endpoint->queue_lock);
	job = endpoint->queue_head;
	if (job != NULL) {
		endpoint->queue_head = job->next;
		if (endpoint->queue_head == NULL) {
			endpoint->queue_tail = NULL;
		}
		job->next = NULL;
	}
	LeaveCriticalSection(&endpoint->queue_lock);
	return job;
}

static BOOL print_job_file(StandardEndpoint *endpoint,
	StandardPrintJob *job, DWORD *error)
{
	BYTE *buffer = NULL;
	HANDLE printer = NULL;
	DOC_INFO_1W document;
	BOOL document_started = FALSE;
	BOOL page_started = FALSE;
	BOOL complete = FALSE;
	BOOL result = FALSE;

	buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
		STANDARD_CLIENT_BUFFER_SIZE);
	if (buffer == NULL) {
		*error = ERROR_NOT_ENOUGH_MEMORY;
		goto cleanup;
	}
	{
		LARGE_INTEGER file_offset;

		file_offset.QuadPart = 0;
		if (!SetFilePointerEx(job->file_handle, file_offset,
			NULL, FILE_BEGIN)) {
			*error = GetLastError();
			goto cleanup;
		}
	}
	if (!OpenPrinterW(endpoint->printer_name, &printer, NULL)) {
		*error = GetLastError();
		goto cleanup;
	}
	ZeroMemory(&document, sizeof(document));
	document.pDocName = L"打印机内网共享 RAW 打印任务";
	document.pDatatype = L"RAW";
	if (!StartDocPrinterW(printer, 1, (LPBYTE)&document)) {
		*error = GetLastError();
		goto cleanup;
	}
	document_started = TRUE;
	if (!StartPagePrinter(printer)) {
		*error = GetLastError();
		goto cleanup;
	}
	page_started = TRUE;
	for (;;) {
		DWORD bytes_read = 0;

		if (!ReadFile(job->file_handle, buffer,
			STANDARD_CLIENT_BUFFER_SIZE, &bytes_read, NULL)) {
			*error = GetLastError();
			goto cleanup;
		}
		if (bytes_read == 0) {
			break;
		}
		if (!write_all_printer(printer, buffer, bytes_read, error)) {
			goto cleanup;
		}
	}
	complete = TRUE;
	result = TRUE;

cleanup:
	if (document_started) {
		BOOL ended = TRUE;

		if (complete) {
			if (page_started && !EndPagePrinter(printer)) {
				ended = FALSE;
				if (*error == ERROR_SUCCESS) {
					*error = GetLastError();
				}
			}
			if (!EndDocPrinter(printer)) {
				ended = FALSE;
				if (*error == ERROR_SUCCESS) {
					*error = GetLastError();
				}
			}
		}
		else {
			ended = FALSE;
			AbortPrinter(printer);
		}
		if (!ended) {
			AbortPrinter(printer);
			result = FALSE;
		}
	}
	if (printer != NULL) {
		ClosePrinter(printer);
	}
	if (buffer != NULL) {
		HeapFree(GetProcessHeap(), 0, buffer);
	}
	if (!result && *error == ERROR_SUCCESS) {
		*error = ERROR_PRINT_CANCELLED;
	}
	return result;
}

static DWORD WINAPI print_worker_thread(LPVOID parameter)
{
	StandardEndpoint *endpoint = (StandardEndpoint *)parameter;

	for (;;) {
		StandardPrintJob *job;
		DWORD error = ERROR_SUCCESS;

		job = dequeue_print_job(endpoint);
		if (job == NULL) {
			if (InterlockedCompareExchange(&endpoint->worker_stopping,
				0, 0)) {
				break;
			}
			WaitForSingleObject(endpoint->queue_event, INFINITE);
			continue;
		}
		if (!print_job_file(endpoint, job, &error)) {
			standard_logw(L"任务 #%ld 打印失败 sender=%S host=%S printer=\"%ls\" port=%u bytes=%I64u error=%lu",
				(long)job->task_id, job->remote_ip, job->remote_host,
				endpoint->printer_name, (unsigned int)endpoint->raw_port,
				job->bytes_received, (unsigned long)error);
		}
		else {
			standard_logw(L"任务 #%ld 打印完成 sender=%S host=%S printer=\"%ls\" port=%u bytes=%I64u result=success",
				(long)job->task_id, job->remote_ip, job->remote_host,
				endpoint->printer_name, (unsigned int)endpoint->raw_port,
				job->bytes_received);
		}
		CloseHandle(job->file_handle);
		HeapFree(GetProcessHeap(), 0, job);
	}
	return 0;
}

static void discard_print_jobs(StandardEndpoint *endpoint)
{
	StandardPrintJob *job;

	EnterCriticalSection(&endpoint->queue_lock);
	job = endpoint->queue_head;
	endpoint->queue_head = NULL;
	endpoint->queue_tail = NULL;
	LeaveCriticalSection(&endpoint->queue_lock);
	while (job != NULL) {
		StandardPrintJob *next = job->next;

		CloseHandle(job->file_handle);
		HeapFree(GetProcessHeap(), 0, job);
		job = next;
	}
}

static BOOL start_print_worker(StandardEndpoint *endpoint)
{
	if (endpoint->worker_thread != NULL) {
		return TRUE;
	}
	InterlockedExchange(&endpoint->worker_stopping, FALSE);
	endpoint->worker_thread = CreateThread(NULL, 0,
		print_worker_thread, endpoint, 0, NULL);
	if (endpoint->worker_thread == NULL) {
		endpoint->last_error = GetLastError();
		return FALSE;
	}
	return TRUE;
}

static void stop_print_worker(StandardEndpoint *endpoint)
{
	InterlockedExchange(&endpoint->worker_stopping, TRUE);
	if (endpoint->queue_event != NULL) {
		SetEvent(endpoint->queue_event);
	}
	if (endpoint->worker_thread != NULL) {
		WaitForSingleObject(endpoint->worker_thread, INFINITE);
		CloseHandle(endpoint->worker_thread);
		endpoint->worker_thread = NULL;
	}
	discard_print_jobs(endpoint);
}

static DWORD WINAPI client_thread(LPVOID parameter)
{
	StandardClient *client = (StandardClient *)parameter;
	StandardEndpoint *endpoint = client->endpoint;
	BYTE *buffer = NULL;
	HANDLE file_handle = INVALID_HANDLE_VALUE;
	BOOL complete = FALSE;
	BOOL idle_finalized = FALSE;
	BOOL fin_received = FALSE;
	BOOL queued = FALSE;
	BOOL has_data = FALSE;
	DWORD error = ERROR_SUCCESS;
	DWORD last_data_tick;
	DWORD idle_timeout_ms = STANDARD_FIRST_DATA_TIMEOUT_MS;
	DWORD wait_ms;

	buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
		STANDARD_CLIENT_BUFFER_SIZE);
	if (buffer == NULL) {
		error = ERROR_NOT_ENOUGH_MEMORY;
		goto cleanup;
	}
	file_handle = create_print_job_file();
	if (file_handle == INVALID_HANDLE_VALUE) {
		error = GetLastError();
		goto cleanup;
	}
	last_data_tick = GetTickCount();

	for (;;) {
		DWORD idle_elapsed;
		int ready;

		idle_timeout_ms = has_data ? STANDARD_IDLE_FINALIZE_MS :
			STANDARD_FIRST_DATA_TIMEOUT_MS;
		idle_elapsed = (DWORD)(GetTickCount() - last_data_tick);
		if (idle_elapsed >= idle_timeout_ms) {
			/* A slow file write may leave the next TCP chunk buffered. */
			ready = wait_for_socket_data(client->socket_handle, 0);
			if (ready == 0) {
				complete = TRUE;
				idle_finalized = TRUE;
				break;
			}
			if (ready == SOCKET_ERROR) {
				int socket_error = WSAGetLastError();

				if (socket_error == WSAEINTR) {
					continue;
				}
				error = socket_error == WSAETIMEDOUT ? ERROR_TIMEOUT :
					ERROR_CONNECTION_ABORTED;
				break;
			}
			wait_ms = 0;
		}
		else {
			wait_ms = idle_timeout_ms - idle_elapsed;
		}
		if (wait_ms != 0) {
			ready = wait_for_socket_data(client->socket_handle,
				wait_ms);

			if (ready == 0) {
				complete = TRUE;
				idle_finalized = TRUE;
				break;
			}
			if (ready == SOCKET_ERROR) {
				int socket_error = WSAGetLastError();

				if (socket_error == WSAEINTR) {
					continue;
				}
				error = socket_error == WSAETIMEDOUT ? ERROR_TIMEOUT :
					ERROR_CONNECTION_ABORTED;
				break;
			}
		}

		{
			int received = recv(client->socket_handle, (char *)buffer,
				STANDARD_CLIENT_BUFFER_SIZE, 0);

			if (received == 0) {
				complete = TRUE;
				fin_received = TRUE;
				break;
			}
			if (received == SOCKET_ERROR) {
				int socket_error = WSAGetLastError();
				error = socket_error == WSAETIMEDOUT ? ERROR_TIMEOUT :
					ERROR_CONNECTION_ABORTED;
				break;
			}
			last_data_tick = GetTickCount();
			if (!write_all_file(file_handle, buffer, (DWORD)received,
				&error)) {
				break;
			}
			client->bytes_received += (ULONGLONG)received;
			has_data = TRUE;
		}
	}
	if (complete && has_data && error == ERROR_SUCCESS) {
		if (!FlushFileBuffers(file_handle)) {
			error = GetLastError();
		}
		else if (enqueue_print_job(endpoint, file_handle, client)) {
			queued = TRUE;
			file_handle = INVALID_HANDLE_VALUE;
		}
		else {
			error = ERROR_SERVICE_NOT_ACTIVE;
		}
	}

cleanup:
	if (file_handle != INVALID_HANDLE_VALUE) {
		CloseHandle(file_handle);
	}
	if (buffer != NULL) {
		HeapFree(GetProcessHeap(), 0, buffer);
	}
	if (error != ERROR_SUCCESS) {
		standard_logw(L"任务 #%ld 接收失败 sender=%S host=%S printer=\"%ls\" port=%u bytes=%I64u error=%lu",
			(long)client->task_id, client->remote_ip, client->remote_host,
			endpoint->printer_name, (unsigned int)endpoint->raw_port,
			client->bytes_received, (unsigned long)error);
	}
	else if (queued) {
		standard_logw(L"任务 #%ld 已入队 sender=%S host=%S printer=\"%ls\" port=%u bytes=%I64u end=%ls result=queued",
			(long)client->task_id, client->remote_ip, client->remote_host,
			endpoint->printer_name, (unsigned int)endpoint->raw_port,
			client->bytes_received,
			fin_received ? L"FIN" :
			(idle_finalized ? L"50ms" : L"connection"));
	}
	else if (idle_finalized) {
		standard_logw(L"任务 #%ld 空任务 sender=%S host=%S printer=\"%ls\" port=%u bytes=0 end=%lu-ms-idle result=ignored",
			(long)client->task_id, client->remote_ip, client->remote_host,
			endpoint->printer_name, (unsigned int)endpoint->raw_port,
			(unsigned long)idle_timeout_ms);
	}
	shutdown(client->socket_handle, SD_BOTH);
	closesocket(client->socket_handle);
	remove_client(client);
	HeapFree(GetProcessHeap(), 0, client);
	return 0;
}

static BOOL add_client(StandardEndpoint *endpoint, SOCKET socket_handle)
{
	StandardClient *client;
	HANDLE thread;

	client = (StandardClient *)HeapAlloc(GetProcessHeap(),
		HEAP_ZERO_MEMORY, sizeof(*client));
	if (client == NULL) {
		closesocket(socket_handle);
		return FALSE;
	}
	client->endpoint = endpoint;
	client->socket_handle = socket_handle;
	client->task_id = InterlockedIncrement(&g_next_task_id);
	if (client->task_id <= 0) {
		client->task_id = InterlockedIncrement(&g_next_task_id);
	}
	{
		struct sockaddr_in address;
		int address_length = sizeof(address);
		char host[NI_MAXHOST];

		ZeroMemory(&address, sizeof(address));
		if (getpeername(socket_handle, (SOCKADDR *)&address,
			&address_length) == 0 && address.sin_family == AF_INET) {
			const char *ip = inet_ntoa(address.sin_addr);
			strncpy_s(client->remote_ip, _countof(client->remote_ip),
				ip != NULL ? ip : "unknown", _TRUNCATE);
			ZeroMemory(host, sizeof(host));
			if (getnameinfo((SOCKADDR *)&address, address_length, host,
				(int)sizeof(host), NULL, 0, NI_NAMEREQD) == 0) {
				strncpy_s(client->remote_host, _countof(client->remote_host),
					host, _TRUNCATE);
			}
		}
	}
	if (client->remote_ip[0] == '\0') {
		strncpy_s(client->remote_ip, _countof(client->remote_ip),
			"unknown", _TRUNCATE);
	}
	if (client->remote_host[0] == '\0') {
		strncpy_s(client->remote_host, _countof(client->remote_host),
			client->remote_ip, _TRUNCATE);
	}
	standard_logw(L"任务 #%ld 接入 sender=%S host=%S printer=\"%ls\" port=%u",
		(long)client->task_id, client->remote_ip, client->remote_host,
		endpoint->printer_name, (unsigned int)endpoint->raw_port);

	EnterCriticalSection(&g_client_lock);
	if (InterlockedCompareExchange(&endpoint->stopping, 0, 0) ||
		InterlockedCompareExchange(&g_stopping, 0, 0)) {
		LeaveCriticalSection(&g_client_lock);
		closesocket(socket_handle);
		HeapFree(GetProcessHeap(), 0, client);
		return FALSE;
	}
	client->next = g_clients;
	g_clients = client;
	endpoint->client_count++;
	ResetEvent(endpoint->clients_done);
	LeaveCriticalSection(&g_client_lock);

	thread = CreateThread(NULL, 0, client_thread, client, 0, NULL);
	if (thread == NULL) {
		remove_client(client);
		shutdown(socket_handle, SD_BOTH);
		closesocket(socket_handle);
		HeapFree(GetProcessHeap(), 0, client);
		return FALSE;
	}
	CloseHandle(thread);
	return TRUE;
}

static DWORD WINAPI accept_thread(LPVOID parameter)
{
	StandardEndpoint *endpoint = (StandardEndpoint *)parameter;

	while (!InterlockedCompareExchange(&endpoint->stopping, 0, 0) &&
		!InterlockedCompareExchange(&g_stopping, 0, 0)) {
		SOCKET socket_handle = accept(endpoint->listen_socket, NULL, NULL);

		if (socket_handle == INVALID_SOCKET) {
			DWORD error = (DWORD)WSAGetLastError();
			if (InterlockedCompareExchange(&endpoint->stopping, 0, 0) ||
				InterlockedCompareExchange(&g_stopping, 0, 0) ||
				error == WSAEINTR || error == WSAENOTSOCK ||
				error == WSAEINVAL) {
				break;
			}
			Sleep(100);
			continue;
		}
		if (InterlockedCompareExchange(&endpoint->stopping, 0, 0) ||
			InterlockedCompareExchange(&g_stopping, 0, 0)) {
			closesocket(socket_handle);
			break;
		}
		set_socket_timeout(socket_handle, STANDARD_SOCKET_TIMEOUT_MS);
		add_client(endpoint, socket_handle);
	}
	return 0;
}

static BOOL start_endpoint_listener(StandardEndpoint *endpoint)
{
	SOCKET socket_handle;
	int reuse = 1;
	struct sockaddr_in address;
	HANDLE thread;
	DWORD error;

	if (InterlockedCompareExchange(&endpoint->listening, 0, 0)) {
		return TRUE;
	}
	socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket_handle == INVALID_SOCKET) {
		endpoint->last_error = (DWORD)WSAGetLastError();
		return FALSE;
	}
	/*
	 * SO_REUSEADDR allows a clean restart while completed RAW connections
	 * are still in TIME_WAIT.  Collision avoidance uses the active-listener
	 * table in tcp_port_is_available().
	 */
	setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR,
		(const char *)&reuse, sizeof(reuse));
	ZeroMemory(&address, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(endpoint->raw_port);
	if (bind(socket_handle, (const SOCKADDR *)&address,
		 sizeof(address)) == SOCKET_ERROR ||
		listen(socket_handle, SOMAXCONN) == SOCKET_ERROR) {
		error = (DWORD)WSAGetLastError();
		closesocket(socket_handle);
		endpoint->last_error = error;
		SetLastError(error);
		return FALSE;
	}
	InterlockedExchange(&endpoint->stopping, FALSE);
	if (!start_print_worker(endpoint)) {
		error = endpoint->last_error;
		closesocket(socket_handle);
		SetLastError(error);
		return FALSE;
	}
	endpoint->listen_socket = socket_handle;
	InterlockedExchange(&endpoint->listening, TRUE);
	thread = CreateThread(NULL, 0, accept_thread, endpoint, 0, NULL);
	if (thread == NULL) {
		error = GetLastError();
		InterlockedExchange(&endpoint->stopping, TRUE);
		InterlockedExchange(&endpoint->listening, FALSE);
		closesocket(socket_handle);
		endpoint->listen_socket = INVALID_SOCKET;
		stop_print_worker(endpoint);
		endpoint->last_error = error;
		SetLastError(error);
		return FALSE;
	}
	endpoint->accept_thread = thread;
	endpoint->last_error = ERROR_SUCCESS;
	set_runtime_port_status(endpoint->raw_port, TRUE);
	return TRUE;
}

static void shutdown_endpoint_clients(StandardEndpoint *endpoint)
{
	StandardClient *client;

	EnterCriticalSection(&g_client_lock);
	for (client = g_clients; client != NULL; client = client->next) {
		if (client->endpoint == endpoint) {
			shutdown(client->socket_handle, SD_BOTH);
		}
	}
	LeaveCriticalSection(&g_client_lock);
}

static void stop_endpoint_listener(StandardEndpoint *endpoint)
{
	SOCKET socket_handle;

	InterlockedExchange(&endpoint->stopping, TRUE);
	socket_handle = endpoint->listen_socket;
	endpoint->listen_socket = INVALID_SOCKET;
	if (socket_handle != INVALID_SOCKET) {
		closesocket(socket_handle);
	}
	if (endpoint->accept_thread != NULL) {
		WaitForSingleObject(endpoint->accept_thread, INFINITE);
		CloseHandle(endpoint->accept_thread);
		endpoint->accept_thread = NULL;
	}
	InterlockedExchange(&endpoint->listening, FALSE);
	shutdown_endpoint_clients(endpoint);
	if (endpoint->clients_done != NULL) {
		WaitForSingleObject(endpoint->clients_done, INFINITE);
	}
	stop_print_worker(endpoint);
	if (endpoint->raw_port != 0) {
		set_runtime_port_status(endpoint->raw_port, FALSE);
	}
}

static void free_endpoint(StandardEndpoint *endpoint)
{
	if (endpoint == NULL) {
		return;
	}
	stop_print_worker(endpoint);
	if (endpoint->clients_done != NULL) {
		CloseHandle(endpoint->clients_done);
	}
	if (endpoint->queue_event != NULL) {
		CloseHandle(endpoint->queue_event);
	}
	DeleteCriticalSection(&endpoint->queue_lock);
	HeapFree(GetProcessHeap(), 0, endpoint);
}

static StandardEndpoint *create_endpoint(
	const UsbRelayStandardPrinterInfo *printer)
{
	StandardEndpoint *endpoint;

	endpoint = (StandardEndpoint *)HeapAlloc(GetProcessHeap(),
		HEAP_ZERO_MEMORY, sizeof(*endpoint));
	if (endpoint == NULL) {
		return NULL;
	}
	wcsncpy_s(endpoint->device_key,
		_countof(endpoint->device_key), printer->device_key, _TRUNCATE);
	wcsncpy_s(endpoint->printer_name,
		_countof(endpoint->printer_name), printer->name, _TRUNCATE);
	wcsncpy_s(endpoint->driver, _countof(endpoint->driver),
		printer->driver, _TRUNCATE);
	wcsncpy_s(endpoint->original_port,
		_countof(endpoint->original_port), printer->original_port,
		_TRUNCATE);
	endpoint->raw_port = printer->raw_port;
	endpoint->listen_socket = INVALID_SOCKET;
	InitializeCriticalSection(&endpoint->queue_lock);
	endpoint->queue_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	endpoint->clients_done = CreateEventW(NULL, TRUE, TRUE, NULL);
	if (endpoint->queue_event == NULL || endpoint->clients_done == NULL) {
		if (endpoint->clients_done != NULL) {
			CloseHandle(endpoint->clients_done);
		}
		if (endpoint->queue_event != NULL) {
			CloseHandle(endpoint->queue_event);
		}
		DeleteCriticalSection(&endpoint->queue_lock);
		HeapFree(GetProcessHeap(), 0, endpoint);
		return NULL;
	}
	return endpoint;
}

static void stop_all_endpoints(void)
{
	StandardEndpoint *endpoint = g_endpoints;

	while (endpoint != NULL) {
		StandardEndpoint *next = endpoint->next;
		stop_endpoint_listener(endpoint);
		free_endpoint(endpoint);
		endpoint = next;
	}
	g_endpoints = NULL;
}

static void remove_unseen_endpoints(
	const UsbRelayStandardPrinterInfo *printers, DWORD printer_count)
{
	StandardEndpoint **cursor = &g_endpoints;

	while (*cursor != NULL) {
		StandardEndpoint *endpoint = *cursor;

		if (device_key_exists(printers, printer_count,
			endpoint->device_key)) {
			cursor = &endpoint->next;
			continue;
		}
		*cursor = endpoint->next;
		stop_endpoint_listener(endpoint);
		free_endpoint(endpoint);
	}
}

static BOOL sync_endpoint(StandardEndpoint *endpoint,
	const UsbRelayStandardPrinterInfo *printer)
{
	if (!InterlockedCompareExchange(&endpoint->listening, 0, 0)) {
		unsigned short retry_port = 0;

		if (endpoint->raw_port != printer->raw_port) {
			endpoint->raw_port = printer->raw_port;
		}
		if (start_endpoint_listener(endpoint)) {
			return TRUE;
		}
		if (endpoint->last_error == WSAEADDRINUSE &&
			reassign_printer_port(endpoint->device_key,
				endpoint->printer_name,
				endpoint->raw_port, &retry_port)) {
			endpoint->raw_port = retry_port;
			return start_endpoint_listener(endpoint);
		}
	}
	return InterlockedCompareExchange(&endpoint->listening, 0, 0) != 0;
}

static void sync_endpoints(void)
{
	UsbRelayStandardPrinterInfo *printers;
	DWORD printer_count = 0;
	DWORD index;

	printers = (UsbRelayStandardPrinterInfo *)HeapAlloc(
		GetProcessHeap(), HEAP_ZERO_MEMORY,
		sizeof(*printers) * STANDARD_MAX_PRINTERS);
	if (printers == NULL) {
		standard_log("Unable to allocate the printer list.");
		return;
	}
	if (!usbrelay_standard_list_printers(printers,
		STANDARD_MAX_PRINTERS, &printer_count)) {
		standard_log("Unable to enumerate local printers.");
		HeapFree(GetProcessHeap(), 0, printers);
		return;
	}
	remove_unseen_endpoints(printers, printer_count);

	for (index = 0; index < printer_count; index++) {
		StandardEndpoint *endpoint;

		if (printers[index].name[0] == L'\0' ||
			printers[index].raw_port == 0) {
			continue;
		}
		endpoint = find_endpoint(printers[index].device_key);
		if (endpoint == NULL) {
			endpoint = create_endpoint(&printers[index]);
			if (endpoint == NULL) {
				continue;
			}
			endpoint->next = g_endpoints;
			g_endpoints = endpoint;
		}
		else {
			wcsncpy_s(endpoint->printer_name,
				_countof(endpoint->printer_name),
				printers[index].name, _TRUNCATE);
			wcsncpy_s(endpoint->driver,
				_countof(endpoint->driver),
				printers[index].driver, _TRUNCATE);
			wcsncpy_s(endpoint->original_port,
				_countof(endpoint->original_port),
				printers[index].original_port, _TRUNCATE);
		}
		sync_endpoint(endpoint, &printers[index]);
	}
	HeapFree(GetProcessHeap(), 0, printers);
}

static BOOL add_broadcast_target(StandardBroadcastTarget *targets,
	DWORD *target_count, const struct sockaddr_in *address)
{
	DWORD index;

	if (*target_count >= STANDARD_MAX_BROADCAST_TARGETS) {
		return FALSE;
	}
	for (index = 0; index < *target_count; index++) {
		if (targets[index].address.sin_addr.s_addr ==
			address->sin_addr.s_addr) {
			return TRUE;
		}
	}
	targets[*target_count].address = *address;
	(*target_count)++;
	return TRUE;
}

static DWORD collect_broadcast_targets(
	StandardBroadcastTarget *targets)
{
	ULONG buffer_size = 16 * 1024;
	IP_ADAPTER_ADDRESSES *addresses = NULL;
	IP_ADAPTER_ADDRESSES *adapter;
	ULONG result;
	DWORD target_count = 0;
	int attempt;
	struct sockaddr_in global_address;

	for (attempt = 0; attempt < 3; attempt++) {
		addresses = (IP_ADAPTER_ADDRESSES *)HeapAlloc(
			GetProcessHeap(), HEAP_ZERO_MEMORY, buffer_size);
		if (addresses == NULL) {
			return 0;
		}
		result = GetAdaptersAddresses(AF_INET,
			GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
			GAA_FLAG_SKIP_DNS_SERVER, NULL, addresses,
			&buffer_size);
		if (result == ERROR_SUCCESS) {
			break;
		}
		HeapFree(GetProcessHeap(), 0, addresses);
		addresses = NULL;
		if (result != ERROR_BUFFER_OVERFLOW) {
			return 0;
		}
	}
	if (addresses != NULL) {
		for (adapter = addresses; adapter != NULL;
			adapter = adapter->Next) {
			IP_ADAPTER_UNICAST_ADDRESS *unicast;

			if (adapter->OperStatus != IfOperStatusUp ||
				adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
				continue;
			}
			for (unicast = adapter->FirstUnicastAddress;
				unicast != NULL; unicast = unicast->Next) {
				const struct sockaddr_in *address;
				struct sockaddr_in destination;
				unsigned long host_address;
				unsigned long mask;
				DWORD prefix;

				if (unicast->Address.lpSockaddr == NULL ||
					unicast->Address.lpSockaddr->sa_family !=
						AF_INET) {
					continue;
				}
				address = (const struct sockaddr_in *)
					unicast->Address.lpSockaddr;
				host_address = ntohl(address->sin_addr.s_addr);
				if (host_address == INADDR_ANY ||
					(host_address >> 24) == 127) {
					continue;
				}
				prefix = unicast->OnLinkPrefixLength;
				if (prefix >= 31) {
					continue;
				}
				mask = prefix == 0 ? 0 :
					0xffffffffUL << (32 - prefix);
				ZeroMemory(&destination, sizeof(destination));
				destination.sin_family = AF_INET;
				destination.sin_port =
					htons(USBRELAY_STANDARD_DISCOVERY_PORT);
				destination.sin_addr.s_addr =
					htonl(host_address | ~mask);
				inet_ntop(AF_INET, &address->sin_addr,
					targets[target_count].local_address,
					sizeof(targets[target_count].local_address));
				add_broadcast_target(targets, &target_count,
					&destination);
			}
		}
		HeapFree(GetProcessHeap(), 0, addresses);
	}

	ZeroMemory(&global_address, sizeof(global_address));
	global_address.sin_family = AF_INET;
	global_address.sin_port =
		htons(USBRELAY_STANDARD_DISCOVERY_PORT);
	global_address.sin_addr.s_addr = INADDR_BROADCAST;
	add_broadcast_target(targets, &target_count, &global_address);
	return target_count;
}

static BOOL send_discovery_packet(const char *packet, int length,
	const struct sockaddr_in *destination)
{
	if (g_discovery_socket == INVALID_SOCKET || length <= 0) {
		return FALSE;
	}
	return sendto(g_discovery_socket, packet, length, 0,
		(const SOCKADDR *)destination,
		sizeof(*destination)) != SOCKET_ERROR;
}

static int build_header_packet(char *packet, size_t packet_count,
	const char *address, DWORD printer_count)
{
	char encoded_name[1024];

	if (!percent_encode_utf8(g_computer_name, encoded_name,
		sizeof(encoded_name))) {
		return -1;
	}
	return _snprintf_s(packet, packet_count, _TRUNCATE,
		USBRELAY_STANDARD_DISCOVERY_MAGIC "\r\n"
		"version=1\r\n"
		"name=%s\r\n"
		"address=%s\r\n"
		"printer_count=%lu\r\n"
		"\r\n",
		encoded_name,
		address != NULL ? address : "0.0.0.0",
		(unsigned long)printer_count);
}

static int build_printer_packet(char *packet, size_t packet_count,
	const StandardEndpoint *endpoint, const char *address)
{
	char encoded_name[2048];
	char encoded_computer[1024];
	char encoded_driver[2048];
	char encoded_port[1024];
	char encoded_endpoint[USBRELAY_STANDARD_ENDPOINT_CHARS * 3];
	wchar_t endpoint_text[USBRELAY_STANDARD_ENDPOINT_CHARS];

	if (!percent_encode_utf8(endpoint->printer_name, encoded_name,
		sizeof(encoded_name)) ||
		!percent_encode_utf8(g_computer_name, encoded_computer,
			sizeof(encoded_computer)) ||
		!percent_encode_utf8(endpoint->driver, encoded_driver,
			sizeof(encoded_driver)) ||
		!percent_encode_utf8(endpoint->original_port, encoded_port,
			sizeof(encoded_port))) {
		return -1;
	}
	if (_snwprintf_s(endpoint_text, _countof(endpoint_text),
		_TRUNCATE, L"%ls:%u",
		g_computer_name, (unsigned int)endpoint->raw_port) < 0) {
		return -1;
	}
	if (!percent_encode_utf8(endpoint_text, encoded_endpoint,
		sizeof(encoded_endpoint))) {
		return -1;
	}
	return _snprintf_s(packet, packet_count, _TRUNCATE,
		USBRELAY_STANDARD_DISCOVERY_MAGIC "\r\n"
		"version=1\r\n"
		"name=%s\r\n"
		"address=%s\r\n"
		"printer=\r\n"
		"protocol=raw\r\n"
		"raw_port=%u\r\n"
		"endpoint=%s\r\n"
		"printer_name=%s\r\n"
		"driver=%s\r\n"
		"original_port=%s\r\n"
		"status=%s\r\n"
		"\r\n",
		encoded_computer,
		address != NULL ? address : "0.0.0.0",
		(unsigned int)endpoint->raw_port,
		encoded_endpoint,
		encoded_name,
		encoded_driver,
		encoded_port,
		InterlockedCompareExchange(
			(LONG volatile *)&endpoint->listening, 0, 0) ?
			"online" : "unavailable");
}

static void send_discovery_to(const struct sockaddr_in *destination,
	const char *address)
{
	StandardEndpoint *endpoint;
	DWORD printer_count = 0;
	char packet[USBRELAY_STANDARD_DISCOVERY_PACKET_MAX];
	int length;

	for (endpoint = g_endpoints; endpoint != NULL;
		endpoint = endpoint->next) {
		if (endpoint->raw_port != 0) {
			printer_count++;
		}
	}
	length = build_header_packet(packet, sizeof(packet), address,
		printer_count);
	if (length > 0) {
		send_discovery_packet(packet, length, destination);
	}
	for (endpoint = g_endpoints; endpoint != NULL;
		endpoint = endpoint->next) {
		if (endpoint->raw_port == 0) {
			continue;
		}
		length = build_printer_packet(packet, sizeof(packet),
			endpoint, address);
		if (length > 0) {
			send_discovery_packet(packet, length, destination);
		}
	}
}

static void send_discovery_cycle(void)
{
	StandardBroadcastTarget targets[STANDARD_MAX_BROADCAST_TARGETS];
	DWORD target_count;
	DWORD index;

	ZeroMemory(targets, sizeof(targets));
	target_count = collect_broadcast_targets(targets);
	for (index = 0; index < target_count; index++) {
		const char *address = targets[index].local_address;

		if (address[0] == '\0') {
			strcpy_s(targets[index].local_address,
				sizeof(targets[index].local_address), "0.0.0.0");
			address = targets[index].local_address;
		}
		send_discovery_to(&targets[index].address, address);
	}
}

static void process_discovery_queries(void)
{
	for (;;) {
		char buffer[256];
		struct sockaddr_in sender;
		int sender_size = sizeof(sender);
		int received;

		received = recvfrom(g_discovery_socket, buffer,
			sizeof(buffer) - 1, 0, (SOCKADDR *)&sender,
			&sender_size);
		if (received == SOCKET_ERROR) {
			break;
		}
		buffer[received] = '\0';
		if (_strnicmp(buffer, USBRELAY_STANDARD_QUERY_MAGIC,
			strlen(USBRELAY_STANDARD_QUERY_MAGIC)) == 0) {
			send_discovery_to(&sender, NULL);
		}
	}
}

static BOOL open_discovery_socket(void)
{
	struct sockaddr_in address;
	BOOL reuse = TRUE;
	BOOL broadcast = TRUE;
	u_long nonblocking = 1;

	g_discovery_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (g_discovery_socket == INVALID_SOCKET) {
		return FALSE;
	}
	setsockopt(g_discovery_socket, SOL_SOCKET, SO_REUSEADDR,
		(const char *)&reuse, sizeof(reuse));
	if (setsockopt(g_discovery_socket, SOL_SOCKET, SO_BROADCAST,
		(const char *)&broadcast, sizeof(broadcast)) == SOCKET_ERROR) {
		closesocket(g_discovery_socket);
		g_discovery_socket = INVALID_SOCKET;
		return FALSE;
	}
	ZeroMemory(&address, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(USBRELAY_STANDARD_DISCOVERY_PORT);
	if (bind(g_discovery_socket, (const SOCKADDR *)&address,
		sizeof(address)) == SOCKET_ERROR) {
		closesocket(g_discovery_socket);
		g_discovery_socket = INVALID_SOCKET;
		return FALSE;
	}
	ioctlsocket(g_discovery_socket, FIONBIO, &nonblocking);
	return TRUE;
}

static BOOL start_engine(void)
{
	WSADATA wsa_data;

	if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
		return FALSE;
	}
	g_core_mutex = CreateMutexW(NULL, FALSE,
		USBRELAY_STANDARD_CORE_MUTEX_NAME);
	if (g_core_mutex == NULL ||
		GetLastError() == ERROR_ALREADY_EXISTS) {
		if (g_core_mutex != NULL) {
			CloseHandle(g_core_mutex);
		}
		g_core_mutex = NULL;
		WSACleanup();
		return FALSE;
	}
	InitializeCriticalSection(&g_client_lock);
	InitializeCriticalSection(&g_log_lock);
	g_log_mutex = CreateMutexW(NULL, FALSE,
		USBRELAY_STANDARD_LOG_MUTEX_NAME);
	if (g_log_mutex == NULL) {
		DeleteCriticalSection(&g_client_lock);
		DeleteCriticalSection(&g_log_lock);
		CloseHandle(g_core_mutex);
		g_core_mutex = NULL;
		WSACleanup();
		return FALSE;
	}
	initialize_log_path();
	g_stop_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
		USBRELAY_STANDARD_STOP_EVENT_NAME);
	if (g_stop_event == NULL) {
		g_stop_event = CreateEventW(NULL, TRUE, FALSE,
			USBRELAY_STANDARD_STOP_EVENT_NAME);
	}
	if (g_stop_event == NULL || !open_discovery_socket()) {
		if (g_stop_event != NULL) {
			CloseHandle(g_stop_event);
			g_stop_event = NULL;
		}
		CloseHandle(g_log_mutex);
		g_log_mutex = NULL;
		DeleteCriticalSection(&g_client_lock);
		DeleteCriticalSection(&g_log_lock);
		if (g_core_mutex != NULL) {
			CloseHandle(g_core_mutex);
			g_core_mutex = NULL;
		}
		WSACleanup();
		return FALSE;
	}
	ResetEvent(g_stop_event);
	InterlockedExchange(&g_stopping, FALSE);
	InterlockedExchange(&g_initialized, TRUE);
	standard_get_computer_name(g_computer_name,
		_countof(g_computer_name));
	if (g_computer_name[0] == L'\0') {
		wcsncpy_s(g_computer_name, _countof(g_computer_name),
			L"USBRelay-Standard-Server", _TRUNCATE);
	}
	set_runtime_running(TRUE);
	sync_endpoints();
	return TRUE;
}

static void stop_engine(void)
{
	if (!InterlockedCompareExchange(&g_initialized, 0, 0)) {
		return;
	}
	InterlockedExchange(&g_stopping, TRUE);
	if (g_stop_event != NULL) {
		SetEvent(g_stop_event);
	}
	if (g_discovery_socket != INVALID_SOCKET) {
		closesocket(g_discovery_socket);
		g_discovery_socket = INVALID_SOCKET;
	}
	stop_all_endpoints();
	set_runtime_running(FALSE);
	if (g_stop_event != NULL) {
		CloseHandle(g_stop_event);
		g_stop_event = NULL;
	}
	if (g_core_mutex != NULL) {
		CloseHandle(g_core_mutex);
		g_core_mutex = NULL;
	}
	if (g_log_mutex != NULL) {
		CloseHandle(g_log_mutex);
		g_log_mutex = NULL;
	}
	DeleteCriticalSection(&g_client_lock);
	DeleteCriticalSection(&g_log_lock);
	WSACleanup();
	InterlockedExchange(&g_initialized, FALSE);
}

int usbrelay_standard_engine_run(void)
{
	DWORD last_refresh;
	DWORD last_discovery = 0;

	if (!start_engine()) {
		standard_log("Unable to start the Standard TCP/IP server.");
		return 1;
	}
	standard_log("Standard TCP/IP RAW server started.");
	last_refresh = GetTickCount();
	for (;;) {
		DWORD now;

		if (WaitForSingleObject(g_stop_event, 250) == WAIT_OBJECT_0) {
			break;
		}
		process_discovery_queries();
		now = GetTickCount();
		if ((DWORD)(now - last_discovery) >=
			STANDARD_DISCOVERY_INTERVAL_MS) {
			last_discovery = now;
			send_discovery_cycle();
		}
		if ((DWORD)(now - last_refresh) >=
			STANDARD_REFRESH_INTERVAL_MS) {
			last_refresh = now;
			sync_endpoints();
		}
	}
	standard_log("Standard TCP/IP RAW server stopped.");
	stop_engine();
	return 0;
}

BOOL usbrelay_standard_engine_is_running(void)
{
	HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE,
		USBRELAY_STANDARD_CORE_MUTEX_NAME);

	if (mutex == NULL) {
		return FALSE;
	}
	CloseHandle(mutex);
	return TRUE;
}

BOOL usbrelay_standard_list_printers(
	UsbRelayStandardPrinterInfo *printers, DWORD printer_capacity,
	DWORD *printer_count)
{
	BYTE *buffer = NULL;
	DWORD count = 0;
	DWORD output_count = 0;
	DWORD index;
	BOOL running;

	if (printer_count == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*printer_count = 0;
	if (g_computer_name[0] == L'\0') {
		standard_get_computer_name(g_computer_name,
			_countof(g_computer_name));
	}
	if (!enum_local_printers(&buffer, &count)) {
		return FALSE;
	}
	running = usbrelay_standard_engine_is_running();
	for (index = 0; index < count; index++) {
		PRINTER_INFO_2W *info =
			&((PRINTER_INFO_2W *)buffer)[index];
		UsbRelayStandardPrinterInfo result;
		BOOL persistent = FALSE;
		unsigned short port = 0;

		if (info->pPrinterName == NULL ||
			info->pPrinterName[0] == L'\0') {
			continue;
		}
		if (output_count >= printer_capacity) {
			continue;
		}
		ZeroMemory(&result, sizeof(result));
		wcsncpy_s(result.name, _countof(result.name),
			info->pPrinterName, _TRUNCATE);
		if (info->pDriverName != NULL) {
			wcsncpy_s(result.driver, _countof(result.driver),
				info->pDriverName, _TRUNCATE);
		}
		if (info->pPortName != NULL) {
			wcsncpy_s(result.original_port,
				_countof(result.original_port),
				info->pPortName, _TRUNCATE);
		}
		build_printer_device_key(info, result.device_key,
			_countof(result.device_key));
		if (assign_printer_port(result.device_key, result.name,
			&port, &persistent)) {
			result.raw_port = port;
			result.persistent = persistent;
			result.published = running &&
				get_runtime_port_status(port);
			_snwprintf_s(result.endpoint,
				_countof(result.endpoint), _TRUNCATE,
				L"%ls:%u", g_computer_name[0] != L'\0' ?
					g_computer_name : L"<computer>:0",
				(unsigned int)port);
			if (result.published) {
				result.error = ERROR_SUCCESS;
			}
			else {
				result.error = ERROR_SERVICE_NOT_ACTIVE;
			}
		}
		else {
			result.error = GetLastError();
		}
		printers[output_count++] = result;
	}
	if (buffer != NULL) {
		HeapFree(GetProcessHeap(), 0, buffer);
	}
	*printer_count = output_count;
	return TRUE;
}

BOOL usbrelay_standard_get_computer_name(wchar_t *name,
	size_t name_chars)
{
	return standard_get_computer_name(name, name_chars);
}

#ifndef USBRELAY_STANDARD_ENGINE_NO_MAIN
int main(int argc, char **argv)
{
	int index;

	for (index = 1; index < argc; index++) {
		if (strcmp(argv[index], "--help") == 0 ||
			strcmp(argv[index], "-h") == 0) {
			printf("打印机内网共享 RAW 打印服务端\n");
			printf("Publishes local print queues on TCP ports %u-%u.\n",
				(unsigned int)USBRELAY_STANDARD_PORT_BASE,
				(unsigned int)USBRELAY_STANDARD_PORT_LAST);
			return 0;
		}
	}
	return usbrelay_standard_engine_run();
}
#endif
