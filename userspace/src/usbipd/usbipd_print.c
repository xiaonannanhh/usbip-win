#include "usbipd.h"

#include <ws2tcpip.h>

#include "usbipd_print.h"

#define PRINT_PROTOCOL "USBIP-PRINT/1"
#define PRINT_MAX_LINE 4096
#define PRINT_MAX_CHUNK (16 * 1024 * 1024)

typedef struct _print_client {
	struct _print_client *next;
	SOCKET			sockfd;
	HANDLE			thread;
} print_client_t;

static CRITICAL_SECTION	print_lock;
static HANDLE		print_done_event;
static print_client_t	*print_clients;
static SOCKET		print_listen_socket = INVALID_SOCKET;
static HANDLE		print_accept_thread;
static LONG		print_active;
static volatile LONG	print_stopping;
static BOOL		print_initialized;

static void
print_active_changed(void)
{
	if (InterlockedCompareExchange(&print_active, 0, 0) == 0)
		SetEvent(print_done_event);
}

static void
print_remove_client(print_client_t *client)
{
	print_client_t **cursor;

	EnterCriticalSection(&print_lock);
	cursor = &print_clients;
	while (*cursor != NULL) {
		if (*cursor == client) {
			*cursor = client->next;
			InterlockedDecrement(&print_active);
			break;
		}
		cursor = &(*cursor)->next;
	}
	LeaveCriticalSection(&print_lock);
	print_active_changed();
}

static BOOL
send_all(SOCKET sockfd, const void *buffer, int length)
{
	const char *cursor = (const char *)buffer;

	while (length > 0) {
		int sent = send(sockfd, cursor, length, 0);

		if (sent == SOCKET_ERROR || sent == 0)
			return FALSE;
		cursor += sent;
		length -= sent;
	}
	return TRUE;
}

static BOOL
recv_exact(SOCKET sockfd, void *buffer, int length)
{
	char *cursor = (char *)buffer;

	while (length > 0) {
		int received = recv(sockfd, cursor, length, 0);

		if (received == SOCKET_ERROR || received == 0)
			return FALSE;
		cursor += received;
		length -= received;
	}
	return TRUE;
}

static int
recv_line(SOCKET sockfd, char *buffer, int buffer_size)
{
	int length = 0;

	if (buffer_size < 2)
		return -1;
	while (length < buffer_size - 1) {
		char character;
		int received = recv(sockfd, &character, 1, 0);

		if (received == SOCKET_ERROR)
			return -1;
		if (received == 0)
			return length > 0 ? length : 0;
		if (character == '\n')
			break;
		if (character != '\r')
			buffer[length++] = character;
	}
	buffer[length] = '\0';
	return length + 1;
}

static BOOL
send_text(SOCKET sockfd, const char *text)
{
	return send_all(sockfd, text, (int)strlen(text)) &&
		send_all(sockfd, "\r\n", 2);
}

static BOOL
wide_to_utf8(const wchar_t *source, char *destination, int destination_size)
{
	int length;

	if (destination_size <= 0)
		return FALSE;
	if (source == NULL)
		source = L"";
	length = WideCharToMultiByte(CP_UTF8, 0, source, -1, destination,
		destination_size, NULL, NULL);
	if (length <= 0) {
		destination[0] = '\0';
		return FALSE;
	}
	destination[destination_size - 1] = '\0';
	return TRUE;
}

static DWORD
printer_id(const wchar_t *printer_name)
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

static BOOL
enum_local_printers(BYTE **buffer, DWORD *count)
{
	DWORD needed = 0;
	DWORD returned = 0;
	BYTE *data;

	*buffer = NULL;
	*count = 0;
	SetLastError(ERROR_SUCCESS);
	if (!EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, NULL, 0, &needed,
		&returned) && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return FALSE;
	if (needed == 0)
		return TRUE;

	data = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, needed);
	if (data == NULL) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	if (!EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, data, needed, &needed,
		&returned)) {
		DWORD error = GetLastError();

		HeapFree(GetProcessHeap(), 0, data);
		SetLastError(error);
		return FALSE;
	}

	*buffer = data;
	*count = returned;
	return TRUE;
}

static BOOL
find_printer_by_id(DWORD id, wchar_t *printer_name, size_t printer_name_count)
{
	BYTE *buffer = NULL;
	DWORD count = 0;
	PRINTER_INFO_2W *printers;
	BOOL found = FALSE;
	DWORD index;

	if (printer_name_count == 0)
		return FALSE;
	printer_name[0] = L'\0';
	if (!enum_local_printers(&buffer, &count))
		return FALSE;

	printers = (PRINTER_INFO_2W *)buffer;
	for (index = 0; index < count; index++) {
		const wchar_t *name = printers[index].pPrinterName;

		if (name != NULL && *name != L'\0' && printer_id(name) == id) {
			wcsncpy_s(printer_name, printer_name_count, name, _TRUNCATE);
			found = TRUE;
			break;
		}
	}
	if (buffer != NULL)
		HeapFree(GetProcessHeap(), 0, buffer);
	return found;
}

static BOOL
send_printer_list(SOCKET sockfd)
{
	BYTE *buffer = NULL;
	DWORD count = 0;
	PRINTER_INFO_2W *printers;
	DWORD index;
	char line[4096];
	char name[1024];
	char driver[1024];
	char port[1024];

	if (!enum_local_printers(&buffer, &count)) {
		snprintf(line, sizeof(line), "ERR spooler %lu",
			(unsigned long)GetLastError());
		send_text(sockfd, line);
		return FALSE;
	}

	if (!send_text(sockfd, PRINT_PROTOCOL " LIST-OK") ||
		_snprintf_s(line, sizeof(line), _TRUNCATE, "count=%lu",
			(unsigned long)count) < 0 ||
		!send_text(sockfd, line)) {
		if (buffer != NULL)
			HeapFree(GetProcessHeap(), 0, buffer);
		return FALSE;
	}

	printers = (PRINTER_INFO_2W *)buffer;
	for (index = 0; index < count; index++) {
		const wchar_t *printer_name = printers[index].pPrinterName;

		if (printer_name == NULL || *printer_name == L'\0')
			continue;
		wide_to_utf8(printer_name, name, sizeof(name));
		wide_to_utf8(printers[index].pDriverName, driver, sizeof(driver));
		wide_to_utf8(printers[index].pPortName, port, sizeof(port));

		if (_snprintf_s(line, sizeof(line), _TRUNCATE,
			"printer=%08lX\r\nname=%s\r\ndriver=%s\r\nport=%s\r\n",
			(unsigned long)printer_id(printer_name), name, driver, port) < 0 ||
			!send_all(sockfd, line, (int)strlen(line)) ||
			!send_all(sockfd, "\r\n", 2)) {
			HeapFree(GetProcessHeap(), 0, buffer);
			return FALSE;
		}
	}

	if (buffer != NULL)
		HeapFree(GetProcessHeap(), 0, buffer);
	return TRUE;
}

static BOOL
start_spooler_job(const wchar_t *printer_name, HANDLE *printer)
{
	DOC_INFO_1W document;

	*printer = NULL;
	if (!OpenPrinterW((LPWSTR)printer_name, printer, NULL))
		return FALSE;

	ZeroMemory(&document, sizeof(document));
	document.pDocName = (LPWSTR)L"USB/IP Remote Print";
	document.pDatatype = (LPWSTR)L"RAW";
	if (!StartDocPrinterW(*printer, 1, (LPBYTE)&document) ||
		!StartPagePrinter(*printer)) {
		DWORD error = GetLastError();

		if (*printer != NULL) {
			ClosePrinter(*printer);
			*printer = NULL;
		}
		SetLastError(error);
		return FALSE;
	}
	return TRUE;
}

static BOOL
write_chunked_job(SOCKET sockfd, HANDLE printer, DWORD *spooler_error)
{
	BYTE *buffer = NULL;
	DWORD buffer_size = 0;
	BOOL success = FALSE;

	for (;;) {
		unsigned char length_bytes[4];
		DWORD length;
		DWORD written = 0;
		DWORD offset = 0;

		if (!recv_exact(sockfd, length_bytes, sizeof(length_bytes)))
			break;
		length = ((DWORD)length_bytes[0] << 24) |
			((DWORD)length_bytes[1] << 16) |
			((DWORD)length_bytes[2] << 8) |
			(DWORD)length_bytes[3];
		if (length == 0) {
			success = TRUE;
			break;
		}
		if (length > PRINT_MAX_CHUNK) {
			*spooler_error = ERROR_INVALID_DATA;
			break;
		}
		if (length > buffer_size) {
			BYTE *new_buffer = (BYTE *)HeapReAlloc(GetProcessHeap(), 0,
				buffer, length);

			if (new_buffer == NULL) {
				*spooler_error = ERROR_NOT_ENOUGH_MEMORY;
				break;
			}
			buffer = new_buffer;
			buffer_size = length;
		}
		if (!recv_exact(sockfd, buffer, (int)length))
			break;

		while (offset < length) {
			if (!WritePrinter(printer, buffer + offset, length - offset,
				&written) || written == 0) {
				*spooler_error = GetLastError();
				if (*spooler_error == ERROR_SUCCESS)
					*spooler_error = ERROR_WRITE_FAULT;
				goto out;
			}
			offset += written;
		}
	}

out:
	if (buffer != NULL)
		HeapFree(GetProcessHeap(), 0, buffer);
	return success;
}

static void
handle_print_request(SOCKET sockfd, const char *command)
{
	char response[128];
	unsigned long id;
	wchar_t printer_name[1024];
	HANDLE printer = NULL;
	DWORD spooler_error = ERROR_SUCCESS;

	if (sscanf_s(command, PRINT_PROTOCOL " PRINT %lx CHUNKED", &id) != 1) {
		send_text(sockfd, "ERR protocol expected PRINT <id> CHUNKED");
		return;
	}
	if (!find_printer_by_id((DWORD)id, printer_name,
		sizeof(printer_name) / sizeof(printer_name[0]))) {
		send_text(sockfd, "ERR printer not found");
		return;
	}
	if (!start_spooler_job(printer_name, &printer)) {
		_snprintf_s(response, sizeof(response), _TRUNCATE,
			"ERR spooler %lu", (unsigned long)GetLastError());
		send_text(sockfd, response);
		return;
	}

	if (write_chunked_job(sockfd, printer, &spooler_error)) {
		if (!EndPagePrinter(printer) || !EndDocPrinter(printer)) {
			spooler_error = GetLastError();
			AbortPrinter(printer);
		}
	}
	else {
		AbortPrinter(printer);
	}
	ClosePrinter(printer);

	if (spooler_error == ERROR_SUCCESS)
		send_text(sockfd, "OK");
	else {
		_snprintf_s(response, sizeof(response), _TRUNCATE,
			"ERR spooler %lu", (unsigned long)spooler_error);
		send_text(sockfd, response);
	}
}

static DWORD WINAPI
print_client_thread(LPVOID context)
{
	print_client_t *client = (print_client_t *)context;
	char line[PRINT_MAX_LINE];

	for (;;) {
		int result = recv_line(client->sockfd, line, sizeof(line));

		if (result <= 0)
			break;
		if (strcmp(line, PRINT_PROTOCOL " LIST") == 0) {
			if (!send_printer_list(client->sockfd))
				break;
		}
		else if (strncmp(line, PRINT_PROTOCOL " PRINT ",
			strlen(PRINT_PROTOCOL " PRINT ")) == 0) {
			handle_print_request(client->sockfd, line);
			break;
		}
		else {
			send_text(client->sockfd, "ERR protocol unsupported");
			break;
		}
	}

	shutdown(client->sockfd, SD_BOTH);
	closesocket(client->sockfd);
	print_remove_client(client);
	free(client);
	return 0;
}

static DWORD WINAPI
print_accept_loop(LPVOID context)
{
	UNREFERENCED_PARAMETER(context);

	while (!print_stopping) {
		SOCKET sockfd = accept(print_listen_socket, NULL, NULL);

		if (sockfd == INVALID_SOCKET)
			break;
		if (print_stopping) {
			closesocket(sockfd);
			break;
		}

		{
			print_client_t *client =
				(print_client_t *)calloc(1, sizeof(*client));
			HANDLE thread;

			if (client == NULL) {
				closesocket(sockfd);
				continue;
			}
			client->sockfd = sockfd;

			EnterCriticalSection(&print_lock);
			if (print_stopping) {
				LeaveCriticalSection(&print_lock);
				closesocket(sockfd);
				free(client);
				break;
			}
			client->next = print_clients;
			print_clients = client;
			InterlockedIncrement(&print_active);
			LeaveCriticalSection(&print_lock);

			thread = CreateThread(NULL, 0, print_client_thread, client,
				CREATE_SUSPENDED, NULL);
			if (thread == NULL) {
				shutdown(sockfd, SD_BOTH);
				closesocket(sockfd);
				print_remove_client(client);
				free(client);
				continue;
			}
			client->thread = thread;
			if (ResumeThread(thread) == (DWORD)-1) {
				TerminateThread(thread, 1);
				WaitForSingleObject(thread, INFINITE);
				CloseHandle(thread);
				shutdown(sockfd, SD_BOTH);
				closesocket(sockfd);
				print_remove_client(client);
				free(client);
				continue;
			}
			CloseHandle(thread);
		}
	}

	InterlockedDecrement(&print_active);
	print_active_changed();
	return 0;
}

BOOL
usbipd_print_relay_init(void)
{
	int reuse = 1;
	SOCKADDR_IN address;

	if (print_initialized)
		return TRUE;

	print_clients = NULL;
	print_listen_socket = INVALID_SOCKET;
	print_accept_thread = NULL;
	print_active = 0;
	print_stopping = FALSE;
	print_initialized = FALSE;

	InitializeCriticalSection(&print_lock);
	print_done_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (print_done_event == NULL) {
		DeleteCriticalSection(&print_lock);
		return FALSE;
	}

	print_listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (print_listen_socket == INVALID_SOCKET)
		goto fail;
	setsockopt(print_listen_socket, SOL_SOCKET, SO_REUSEADDR,
		(const char *)&reuse, sizeof(reuse));

	ZeroMemory(&address, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(USBIPD_PRINT_PORT);
	if (bind(print_listen_socket, (const SOCKADDR *)&address,
		sizeof(address)) == SOCKET_ERROR ||
		listen(print_listen_socket, SOMAXCONN) == SOCKET_ERROR)
		goto fail;

	InterlockedIncrement(&print_active);
	print_accept_thread = CreateThread(NULL, 0, print_accept_loop, NULL, 0,
		NULL);
	if (print_accept_thread == NULL) {
		InterlockedDecrement(&print_active);
		goto fail;
	}

	print_initialized = TRUE;
	info("print relay is listening on TCP %d", USBIPD_PRINT_PORT);
	return TRUE;

fail:
	if (print_listen_socket != INVALID_SOCKET) {
		closesocket(print_listen_socket);
		print_listen_socket = INVALID_SOCKET;
	}
	if (print_done_event != NULL) {
		CloseHandle(print_done_event);
		print_done_event = NULL;
	}
	DeleteCriticalSection(&print_lock);
	return FALSE;
}

void
usbipd_print_relay_request_stop(void)
{
	print_client_t *client;
	SOCKET listen_socket;

	InterlockedExchange(&print_stopping, TRUE);
	if (!print_initialized)
		return;

	EnterCriticalSection(&print_lock);
	listen_socket = print_listen_socket;
	print_listen_socket = INVALID_SOCKET;
	LeaveCriticalSection(&print_lock);
	if (listen_socket != INVALID_SOCKET)
		closesocket(listen_socket);

	EnterCriticalSection(&print_lock);
	for (client = print_clients; client != NULL; client = client->next)
		shutdown(client->sockfd, SD_BOTH);
	LeaveCriticalSection(&print_lock);
}

BOOL
usbipd_print_relay_wait(DWORD timeout_ms)
{
	if (!print_initialized)
		return TRUE;
	return WaitForSingleObject(print_done_event, timeout_ms) == WAIT_OBJECT_0;
}

void
usbipd_print_relay_cleanup(void)
{
	if (!print_initialized)
		return;
	usbipd_print_relay_request_stop();
	if (!usbipd_print_relay_wait(10000)) {
		err("print relay did not stop; preserving synchronization objects until process exit");
		return;
	}
	if (print_accept_thread != NULL) {
		WaitForSingleObject(print_accept_thread, INFINITE);
		CloseHandle(print_accept_thread);
		print_accept_thread = NULL;
	}
	if (print_listen_socket != INVALID_SOCKET) {
		closesocket(print_listen_socket);
		print_listen_socket = INVALID_SOCKET;
	}
	CloseHandle(print_done_event);
	print_done_event = NULL;
	DeleteCriticalSection(&print_lock);
	print_initialized = FALSE;
}

BOOL
usbipd_print_relay_running(void)
{
	return print_initialized && !print_stopping;
}
