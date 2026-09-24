#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>

#include "../include/usbrelay_standard_protocol.h"
#include "../userspace/src/usbrelay/usbrelay_standard_server.h"
#include "ui_common.h"
#include "ui_resource.h"

#define STANDARD_WINDOW_CLASS L"USBRelay-Standard-Server-UI"
#define STANDARD_WINDOW_TITLE L"打印机内网共享 - 服务端"
#define STANDARD_TIMER_STATUS 1
#define STANDARD_MAX_UI_PRINTERS 1024

#define IDC_STATUS 2001
#define IDC_REFRESH 2002
#define IDC_START 2003
#define IDC_STOP 2004
#define IDC_MANAGE 2005
#define IDC_AUTOSTART 2006
#define IDC_PRINTERS 2007
#define IDC_LOG 2008
#define IDC_COMPUTER 2009
#define IDC_LOG_FILTER 2010
#define IDC_LOG_APPLY 2011
#define IDC_LOG_CLEAR_FILTER 2012
#define IDC_LOG_CLEAR_POOL 2013
#define IDC_LOG_OPEN 2014
#define IDC_TASK_LOG 2015
#define STANDARD_LOG_PATH_CHARS 1024
#define STANDARD_LOG_MAX_BYTES (8u * 1024u * 1024u)
#define STANDARD_LOG_VIEW_MAX_CHARS 48000

typedef struct StandardServerState {
	HWND window;
	HWND status;
	HWND computer;
	HWND printers;
	HWND log;
	HWND refresh;
	HWND start;
	HWND stop;
	HWND manage;
	HWND autostart;
	HWND log_filter_label;
	HWND log_filter;
	HWND apply_filter;
	HWND clear_filter;
	HWND clear_log_pool;
	HWND open_log_dir;
	HWND task_log;
	HFONT font;
	HICON tray_icon;
	BOOL tray_added;
	BOOL exiting;
	BOOL hidden_start;
	BOOL last_running;
	UiProcess process;
	HANDLE stop_event;
	wchar_t log_filter_text[256];
	ULONGLONG task_log_size;
	FILETIME task_log_write_time;
	BOOL task_log_loaded;
} StandardServerState;

static StandardServerState *get_state(HWND window)
{
	return (StandardServerState *)GetWindowLongPtrW(window, GWLP_USERDATA);
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

static void show_window(HWND window)
{
	ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
	SetForegroundWindow(window);
}

static HWND make_control(StandardServerState *state,
	const wchar_t *class_name, const wchar_t *text, DWORD style,
	DWORD ex_style, int x, int y, int width, int height, int id)
{
	HWND control = CreateWindowExW(ex_style, class_name, text,
		WS_CHILD | WS_VISIBLE | style, x, y, width, height, state->window,
		(HMENU)(INT_PTR)id, NULL, NULL);

	if (control != NULL) {
		SendMessageW(control, WM_SETFONT, (WPARAM)state->font, TRUE);
	}
	return control;
}

static void log_text(StandardServerState *state, const wchar_t *format, ...)
{
	wchar_t message[2048];
	wchar_t line[2100];
	va_list args;
	int length;

	va_start(args, format);
	_vsnwprintf_s(message, UI_ARRAY_COUNT(message), _TRUNCATE,
		format, args);
	va_end(args);
	_snwprintf_s(line, UI_ARRAY_COUNT(line), _TRUNCATE, L"%ls\r\n",
		message);
	length = GetWindowTextLengthW(state->log);
	if (length > 60000) {
		SetWindowTextW(state->log, L"");
		length = 0;
	}
	SendMessageW(state->log, EM_SETSEL, length, length);
	SendMessageW(state->log, EM_REPLACESEL, FALSE, (LPARAM)line);
}

static void set_status(StandardServerState *state, const wchar_t *text)
{
	SetWindowTextW(state->status, text);
}

static BOOL get_task_log_path(wchar_t *path, size_t path_chars,
	wchar_t *directory, size_t directory_chars)
{
	wchar_t program_data[MAX_PATH];
	DWORD length;

	if (path == NULL || directory == NULL || path_chars == 0 ||
		directory_chars == 0) {
		return FALSE;
	}
	length = GetEnvironmentVariableW(L"ProgramData", program_data,
		(DWORD)UI_ARRAY_COUNT(program_data));
	if (length == 0 || length >= UI_ARRAY_COUNT(program_data)) {
		length = GetTempPathW((DWORD)UI_ARRAY_COUNT(program_data),
			program_data);
		if (length == 0 || length >= UI_ARRAY_COUNT(program_data)) {
			return FALSE;
		}
		if (program_data[length - 1] == L'\\') {
			program_data[length - 1] = L'\0';
		}
	}
	if (_snwprintf_s(directory, directory_chars, _TRUNCATE,
		L"%ls\\USBRelay\\StandardServer", program_data) < 0 ||
		_snwprintf_s(path, path_chars, _TRUNCATE,
			L"%ls\\server-task-log.txt", directory) < 0) {
		return FALSE;
	}
	return TRUE;
}

static BOOL ensure_task_log_directory(const wchar_t *directory)
{
	wchar_t parent[STANDARD_LOG_PATH_CHARS];
	wchar_t *separator;
	DWORD attributes;

	if (directory == NULL || wcslen(directory) >= UI_ARRAY_COUNT(parent)) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	wcscpy_s(parent, UI_ARRAY_COUNT(parent), directory);
	separator = wcsrchr(parent, L'\\');
	if (separator == NULL || separator == parent) {
		SetLastError(ERROR_INVALID_NAME);
		return FALSE;
	}
	*separator = L'\0';
	if (!CreateDirectoryW(parent, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
		return FALSE;
	}
	attributes = GetFileAttributesW(parent);
	if (attributes == INVALID_FILE_ATTRIBUTES ||
		(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		SetLastError(ERROR_PATH_NOT_FOUND);
		return FALSE;
	}
	if (!CreateDirectoryW(directory, NULL) &&
		GetLastError() != ERROR_ALREADY_EXISTS) {
		return FALSE;
	}
	attributes = GetFileAttributesW(directory);
	if (attributes == INVALID_FILE_ATTRIBUTES ||
		(attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		SetLastError(ERROR_PATH_NOT_FOUND);
		return FALSE;
	}
	return TRUE;
}

static BOOL line_contains_ci(const wchar_t *line, size_t line_length,
	const wchar_t *filter)
{
	const wchar_t *cursor;
	size_t filter_length;

	if (filter == NULL || filter[0] == L'\0') {
		return TRUE;
	}
	filter_length = wcslen(filter);
	for (cursor = line; cursor != NULL && cursor < line + line_length;
		cursor++) {
		size_t index;
		for (index = 0; index < filter_length; index++) {
			if (cursor + index >= line + line_length ||
				towlower(cursor[index]) != towlower(filter[index])) {
				break;
			}
		}
		if (index == filter_length) {
			return TRUE;
		}
	}
	return FALSE;
}

static void refresh_task_log(StandardServerState *state)
{
	wchar_t path[STANDARD_LOG_PATH_CHARS];
	wchar_t directory[STANDARD_LOG_PATH_CHARS];
	wchar_t filter[256];
	HANDLE mutex;
	HANDLE file;
	DWORD wait_result;
	LARGE_INTEGER size;
	BYTE *bytes = NULL;
	wchar_t *text = NULL;
	wchar_t *output = NULL;
	DWORD bytes_read;
	int wide_chars;
	BY_HANDLE_FILE_INFORMATION file_info;
	const wchar_t *line;
	const wchar_t *display_text;
	wchar_t *write_cursor;
	wchar_t *end;
	int previous_first_line;
	int previous_scroll;
	int previous_scroll_min;
	int previous_scroll_max;
	BOOL was_at_bottom;

	if (!get_task_log_path(path, UI_ARRAY_COUNT(path), directory,
		UI_ARRAY_COUNT(directory))) {
		return;
	}
	ZeroMemory(&size, sizeof(size));
	ZeroMemory(&file_info, sizeof(file_info));
	GetWindowTextW(state->log_filter, filter, (int)UI_ARRAY_COUNT(filter));
	mutex = CreateMutexW(NULL, FALSE,
		USBRELAY_STANDARD_LOG_MUTEX_NAME);
	if (mutex == NULL) {
		return;
	}
	wait_result = WaitForSingleObject(mutex, INFINITE);
	if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
		CloseHandle(mutex);
		return;
	}
	file = CreateFileW(path, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		ReleaseMutex(mutex);
		CloseHandle(mutex);
		if (error == ERROR_FILE_NOT_FOUND) {
			SetWindowTextW(state->task_log, L"");
			state->task_log_size = 0;
			ZeroMemory(&state->task_log_write_time,
				sizeof(state->task_log_write_time));
			wcsncpy_s(state->log_filter_text,
				UI_ARRAY_COUNT(state->log_filter_text), filter, _TRUNCATE);
			state->task_log_loaded = TRUE;
		}
		return;
	}
	if (!GetFileSizeEx(file, &size)) {
		CloseHandle(file);
		ReleaseMutex(mutex);
		CloseHandle(mutex);
		return;
	}
	if (size.QuadPart <= 0) {
		CloseHandle(file);
		ReleaseMutex(mutex);
		CloseHandle(mutex);
		SetWindowTextW(state->task_log, L"");
		state->task_log_size = 0;
		ZeroMemory(&state->task_log_write_time,
			sizeof(state->task_log_write_time));
		wcsncpy_s(state->log_filter_text,
			UI_ARRAY_COUNT(state->log_filter_text), filter, _TRUNCATE);
		state->task_log_loaded = TRUE;
		return;
	}
	if (size.QuadPart > STANDARD_LOG_MAX_BYTES) {
		CloseHandle(file);
		ReleaseMutex(mutex);
		CloseHandle(mutex);
		return;
	}
	if (GetFileInformationByHandle(file, &file_info) &&
		state->task_log_loaded &&
		state->task_log_size == (ULONGLONG)size.QuadPart &&
		CompareFileTime(&state->task_log_write_time,
			&file_info.ftLastWriteTime) == 0 &&
		wcscmp(state->log_filter_text, filter) == 0) {
		CloseHandle(file);
		ReleaseMutex(mutex);
		CloseHandle(mutex);
		return;
	}
	previous_first_line = (int)SendMessageW(state->task_log,
		EM_GETFIRSTVISIBLELINE, 0, 0);
	previous_scroll = GetScrollPos(state->task_log, SB_VERT);
	GetScrollRange(state->task_log, SB_VERT, &previous_scroll_min,
		&previous_scroll_max);
	was_at_bottom = previous_scroll >= previous_scroll_max - 2;
	bytes = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
		(size_t)size.QuadPart + 1);
	if (bytes == NULL) {
		CloseHandle(file);
		ReleaseMutex(mutex);
		CloseHandle(mutex);
		return;
	}
	if (!ReadFile(file, bytes, (DWORD)size.QuadPart, &bytes_read, NULL)) {
		HeapFree(GetProcessHeap(), 0, bytes);
		CloseHandle(file);
		ReleaseMutex(mutex);
		CloseHandle(mutex);
		return;
	}
	GetFileInformationByHandle(file, &file_info);
	CloseHandle(file);
	ReleaseMutex(mutex);
	CloseHandle(mutex);
	bytes[bytes_read] = 0;
	wide_chars = MultiByteToWideChar(CP_UTF8, 0, (char *)bytes,
		(int)bytes_read, NULL, 0);
	if (wide_chars <= 0) {
		HeapFree(GetProcessHeap(), 0, bytes);
		return;
	}
	text = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		((size_t)wide_chars + 1) * sizeof(wchar_t));
	output = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		((size_t)wide_chars + 1) * sizeof(wchar_t));
	if (text == NULL || output == NULL ||
		MultiByteToWideChar(CP_UTF8, 0, (char *)bytes, (int)bytes_read,
			text, wide_chars) <= 0) {
		if (text != NULL) HeapFree(GetProcessHeap(), 0, text);
		if (output != NULL) HeapFree(GetProcessHeap(), 0, output);
		HeapFree(GetProcessHeap(), 0, bytes);
		return;
	}
	write_cursor = output;
	line = text;
	end = text + wide_chars;
	while (line < end && *line != L'\0') {
		const wchar_t *line_end = wcschr(line, L'\n');
		size_t line_length = line_end != NULL ?
			(size_t)(line_end - line) : wcslen(line);
		if (line_length > 0 && line[line_length - 1] == L'\r') {
			line_length--;
		}
		if (line_length > 0 && line[0] == 0xfeff) {
			line++;
			line_length--;
		}
		if (line_contains_ci(line, line_length, filter)) {
			if (write_cursor != output && write_cursor[-1] != L'\n') {
				*write_cursor++ = L'\r';
				*write_cursor++ = L'\n';
			}
			wmemcpy(write_cursor, line, line_length);
			write_cursor += line_length;
			*write_cursor++ = L'\r';
			*write_cursor++ = L'\n';
		}
		if (line_end == NULL) {
			break;
		}
		line = line_end + 1;
	}
	*write_cursor = L'\0';
	if (file_info.dwFileAttributes != 0) {
		state->task_log_write_time = file_info.ftLastWriteTime;
	}
	else {
		ZeroMemory(&state->task_log_write_time,
			sizeof(state->task_log_write_time));
	}
	state->task_log_size = (ULONGLONG)bytes_read;
	wcsncpy_s(state->log_filter_text,
		UI_ARRAY_COUNT(state->log_filter_text), filter, _TRUNCATE);
	state->task_log_loaded = TRUE;
	display_text = output;
	if ((size_t)(write_cursor - output) > STANDARD_LOG_VIEW_MAX_CHARS) {
		display_text = output + (write_cursor - output -
			STANDARD_LOG_VIEW_MAX_CHARS);
		while (*display_text != L'\0' && *display_text != L'\n') {
			display_text++;
		}
		if (*display_text == L'\n') {
			display_text++;
		}
	}
	SetWindowTextW(state->task_log, display_text);
	if (was_at_bottom) {
		int text_length = GetWindowTextLengthW(state->task_log);
		SendMessageW(state->task_log, EM_SETSEL, text_length, text_length);
		SendMessageW(state->task_log, EM_SCROLLCARET, 0, 0);
	}
	else {
		int first_line = (int)SendMessageW(state->task_log,
			EM_GETFIRSTVISIBLELINE, 0, 0);
		SendMessageW(state->task_log, EM_LINESCROLL, 0,
			previous_first_line - first_line);
	}
	HeapFree(GetProcessHeap(), 0, text);
	HeapFree(GetProcessHeap(), 0, output);
	HeapFree(GetProcessHeap(), 0, bytes);
}

static void clear_task_log(StandardServerState *state)
{
	wchar_t path[STANDARD_LOG_PATH_CHARS];
	wchar_t directory[STANDARD_LOG_PATH_CHARS];
	HANDLE mutex;
	HANDLE file;
	DWORD wait_result;
	DWORD error;
	int answer;

	answer = MessageBoxW(state->window,
		L"确定清空全部打印任务日志吗？此操作不可撤销，但不会影响打印机、端口或队列。",
		L"清理任务日志池", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
	if (answer != IDYES) {
		return;
	}

	if (!get_task_log_path(path, UI_ARRAY_COUNT(path), directory,
		UI_ARRAY_COUNT(directory))) {
		log_text(state, L"无法确定任务日志池路径。");
		return;
	}
	if (!ensure_task_log_directory(directory)) {
		error = GetLastError();
		log_text(state, L"无法创建任务日志目录，错误 %lu。", error);
		return;
	}
	mutex = CreateMutexW(NULL, FALSE,
		USBRELAY_STANDARD_LOG_MUTEX_NAME);
	if (mutex == NULL) {
		log_text(state, L"打开任务日志锁失败，错误 %lu。", GetLastError());
		return;
	}
	wait_result = WaitForSingleObject(mutex, INFINITE);
	if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
		error = GetLastError();
		CloseHandle(mutex);
		log_text(state, L"等待任务日志锁失败，错误 %lu。", error);
		return;
	}
	file = CreateFileW(path, GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		error = GetLastError();
		ReleaseMutex(mutex);
		CloseHandle(mutex);
		log_text(state, L"清理任务日志池失败，错误 %lu。", error);
		return;
	}
	CloseHandle(file);
	ReleaseMutex(mutex);
	CloseHandle(mutex);
	SetWindowTextW(state->task_log, L"");
	state->task_log_size = 0;
	ZeroMemory(&state->task_log_write_time,
		sizeof(state->task_log_write_time));
	GetWindowTextW(state->log_filter, state->log_filter_text,
		(int)UI_ARRAY_COUNT(state->log_filter_text));
	state->task_log_loaded = TRUE;
	log_text(state, L"任务日志池已清理；打印队列、端口和驱动未改变。");
}

static void open_task_log_directory(StandardServerState *state)
{
	wchar_t path[STANDARD_LOG_PATH_CHARS];
	wchar_t directory[STANDARD_LOG_PATH_CHARS];

	if (!get_task_log_path(path, UI_ARRAY_COUNT(path), directory,
		UI_ARRAY_COUNT(directory))) {
		return;
	}
	if (!ensure_task_log_directory(directory)) {
		return;
	}
	ShellExecuteW(state->window, L"open", directory, NULL, NULL,
		SW_SHOWNORMAL);
}

static BOOL child_process_running(StandardServerState *state)
{
	DWORD exit_code;

	if (state->process.process.hProcess == NULL) {
		return FALSE;
	}
	if (WaitForSingleObject(state->process.process.hProcess, 0) ==
		WAIT_TIMEOUT) {
		return TRUE;
	}
	GetExitCodeProcess(state->process.process.hProcess, &exit_code);
	UiCloseProcess(&state->process, FALSE);
	return FALSE;
}

static BOOL server_core_exists(void)
{
	return usbrelay_standard_engine_is_running();
}

static void refresh_printers(StandardServerState *state)
{
	UsbRelayStandardPrinterInfo *printers;
	DWORD printer_count = 0;
	DWORD index;
	BOOL running;

	printers = (UsbRelayStandardPrinterInfo *)HeapAlloc(
		GetProcessHeap(), HEAP_ZERO_MEMORY,
		sizeof(*printers) * STANDARD_MAX_UI_PRINTERS);
	if (printers == NULL) {
		log_text(state, L"内存不足，无法读取打印机列表。");
		return;
	}
	if (!usbrelay_standard_list_printers(printers,
		STANDARD_MAX_UI_PRINTERS, &printer_count)) {
		log_text(state, L"读取本机打印机失败，错误 %lu。", GetLastError());
		HeapFree(GetProcessHeap(), 0, printers);
		return;
	}

	running = server_core_exists();
	ListView_DeleteAllItems(state->printers);
	for (index = 0; index < printer_count; index++) {
		LVITEMW item;
		wchar_t raw_port[32];
		wchar_t status[160];
		UsbRelayStandardPrinterInfo *printer = &printers[index];

		ZeroMemory(&item, sizeof(item));
		item.mask = LVIF_TEXT;
		item.iItem = ListView_GetItemCount(state->printers);
		item.pszText = printer->name;
		ListView_InsertItem(state->printers, &item);
		ListView_SetItemText(state->printers, item.iItem, 1,
			printer->driver[0] != L'\0' ? printer->driver : L"-");
		ListView_SetItemText(state->printers, item.iItem, 2,
			printer->original_port[0] != L'\0' ?
				printer->original_port : L"-");
		if (printer->raw_port != 0) {
			_snwprintf_s(raw_port, UI_ARRAY_COUNT(raw_port), _TRUNCATE,
				L"%u", (unsigned int)printer->raw_port);
		}
		else {
			wcsncpy_s(raw_port, UI_ARRAY_COUNT(raw_port), L"-",
				_TRUNCATE);
		}
		ListView_SetItemText(state->printers, item.iItem, 3, raw_port);
		ListView_SetItemText(state->printers, item.iItem, 4,
			printer->endpoint[0] != L'\0' ? printer->endpoint : L"-");

		if (printer->published) {
			wcsncpy_s(status, UI_ARRAY_COUNT(status),
				L"端口已锁定，正在发布",
				_TRUNCATE);
		}
		else if (!running) {
			wcsncpy_s(status, UI_ARRAY_COUNT(status),
				L"端口已锁定，服务未启动", _TRUNCATE);
		}
		else if (printer->error != ERROR_SUCCESS) {
			_snwprintf_s(status, UI_ARRAY_COUNT(status), _TRUNCATE,
				L"端口不可用，错误 %lu",
				(unsigned long)printer->error);
		}
		else {
			wcsncpy_s(status, UI_ARRAY_COUNT(status),
				L"端口已锁定，正在建立监听", _TRUNCATE);
		}
		ListView_SetItemText(state->printers, item.iItem, 5, status);
	}
	HeapFree(GetProcessHeap(), 0, printers);
	log_text(state, L"已刷新打印机，共 %lu 台；设备与发布端口已锁定。",
		(unsigned long)printer_count);
}

static void refresh_service(StandardServerState *state)
{
	BOOL running = child_process_running(state) || server_core_exists();

	if (running) {
		set_status(state, L"服务运行中：正在发布本机打印队列");
		EnableWindow(state->start, FALSE);
		EnableWindow(state->stop, TRUE);
	}
	else {
		set_status(state, L"服务未运行");
		EnableWindow(state->start, TRUE);
		EnableWindow(state->stop, FALSE);
	}
	if (state->last_running != running) {
		state->last_running = running;
		refresh_printers(state);
	}
}

static BOOL ensure_stop_event(StandardServerState *state)
{
	if (state->stop_event != NULL) {
		return TRUE;
	}
	state->stop_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE,
		FALSE, USBRELAY_STANDARD_STOP_EVENT_NAME);
	if (state->stop_event == NULL) {
		state->stop_event = CreateEventW(NULL, TRUE, FALSE,
			USBRELAY_STANDARD_STOP_EVENT_NAME);
	}
	return state->stop_event != NULL;
}

static void start_server(StandardServerState *state)
{
	wchar_t executable[MAX_PATH];

	if (child_process_running(state) || server_core_exists()) {
		log_text(state, L"Standard TCP/IP 服务已经在运行。");
		refresh_service(state);
		return;
	}
	if (!ensure_stop_event(state)) {
		log_text(state, L"创建停止事件失败，错误 %lu。", GetLastError());
		return;
	}
	ResetEvent(state->stop_event);
	if (GetModuleFileNameW(NULL, executable,
		(DWORD)UI_ARRAY_COUNT(executable)) == 0 ||
		wcslen(executable) >= UI_ARRAY_COUNT(executable)) {
		log_text(state, L"读取服务端程序路径失败，错误 %lu。",
			GetLastError());
		return;
	}
	if (!UiStartHiddenProcess(executable, L"--engine", NULL,
		&state->process)) {
		log_text(state, L"启动后台服务失败，错误 %lu。", GetLastError());
		return;
	}
	Sleep(700);
	if (!child_process_running(state) && !server_core_exists()) {
		log_text(state, L"后台服务启动后立即退出。");
		refresh_service(state);
		return;
	}
	log_text(state, L"Standard TCP/IP RAW 服务已启动。");
	refresh_service(state);
}

static void shutdown_server(StandardServerState *state)
{
	DWORD wait_result = WAIT_OBJECT_0;

	if (!child_process_running(state) && !server_core_exists()) {
		return;
	}
	if (!ensure_stop_event(state)) {
		return;
	}
	SetEvent(state->stop_event);
	if (state->process.process.hProcess != NULL) {
		wait_result = WaitForSingleObject(state->process.process.hProcess,
			7000);
		UiCloseProcess(&state->process, wait_result == WAIT_TIMEOUT);
	}
	else {
		DWORD elapsed;

		for (elapsed = 0; elapsed < 7000; elapsed += 100) {
			if (!server_core_exists()) {
				break;
			}
			Sleep(100);
		}
	}
	if (state->stop_event != NULL) {
		CloseHandle(state->stop_event);
		state->stop_event = NULL;
	}
}

static void stop_server(StandardServerState *state)
{
	if (!child_process_running(state) && !server_core_exists()) {
		log_text(state, L"服务当前未运行。");
		refresh_service(state);
		return;
	}
	shutdown_server(state);
	if (server_core_exists()) {
		log_text(state, L"已请求停止，但后台服务仍在退出。");
	}
	else {
		log_text(state, L"Standard TCP/IP RAW 服务已停止。");
	}
	refresh_service(state);
}

static void toggle_startup(StandardServerState *state)
{
	LRESULT checked = SendMessageW(state->autostart, BM_GETCHECK, 0, 0);
	wchar_t executable[MAX_PATH];
	DWORD error = ERROR_SUCCESS;
	BOOL ok;

	if (!GetModuleFileNameW(NULL, executable,
		UI_ARRAY_COUNT(executable))) {
		log_text(state, L"读取界面路径失败，错误 %lu。", GetLastError());
		return;
	}
	if (checked == BST_CHECKED) {
		ok = UiCreateLogonTask(USBRELAY_STANDARD_TASK_NAME, executable,
			L"/autostart",
			L"打印机内网共享服务端开机启动",
			&error);
	}
	else {
		ok = UiDeleteLogonTask(USBRELAY_STANDARD_TASK_NAME, &error);
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

static void open_printer_management(StandardServerState *state)
{
	wchar_t path[MAX_PATH];
	UiProcess process;

	if (!UiGetSystemToolPath(L"control.exe", path,
		UI_ARRAY_COUNT(path))) {
		log_text(state, L"找不到打印机管理工具。");
		return;
	}
	ZeroMemory(&process, sizeof(process));
	if (!UiStartHiddenProcess(path, L"printers", NULL, &process)) {
		ShellExecuteW(state->window, L"open", L"control.exe",
			L"printers", NULL, SW_SHOWNORMAL);
	}
	else {
		UiCloseProcess(&process, FALSE);
	}
}

static void layout(StandardServerState *state)
{
	RECT rect;
	int width;
	int height;
	int list_height;
	int task_log_height;
	int runtime_log_height;

	GetClientRect(state->window, &rect);
	width = rect.right - rect.left;
	height = rect.bottom - rect.top;
	if (width < 960) {
		width = 960;
	}
	if (height < 600) {
		height = 600;
	}
	MoveWindow(state->status, 14, 12, width - 28, 24, TRUE);
	MoveWindow(state->computer, 14, 44, width - 28, 22, TRUE);
	MoveWindow(state->manage, 14, 72, 180, 28, TRUE);
	MoveWindow(state->autostart, 204, 74, 250, 24, TRUE);
	MoveWindow(state->refresh, width - 374, 72, 112, 28, TRUE);
	MoveWindow(state->start, width - 248, 72, 112, 28, TRUE);
	MoveWindow(state->stop, width - 122, 72, 108, 28, TRUE);
	list_height = height / 2 - 94;
	if (list_height < 220) {
		list_height = 220;
	}
	MoveWindow(state->printers, 14, 108, width - 28, list_height, TRUE);
	MoveWindow(state->log_filter_label, 14, 120 + list_height, 64, 24, TRUE);
	MoveWindow(state->log_filter, 86, 118 + list_height, 326, 26, TRUE);
	MoveWindow(state->apply_filter, 420, 118 + list_height, 82, 28, TRUE);
	MoveWindow(state->clear_filter, 510, 118 + list_height, 94, 28, TRUE);
	MoveWindow(state->clear_log_pool, width - 292, 118 + list_height, 132, 28, TRUE);
	MoveWindow(state->open_log_dir, width - 150, 118 + list_height, 136, 28, TRUE);
	task_log_height = height / 4;
	if (task_log_height < 100) {
		task_log_height = 100;
	}
	MoveWindow(state->task_log, 14, 150 + list_height, width - 28,
		task_log_height, TRUE);
	runtime_log_height = height - list_height - task_log_height - 184;
	if (runtime_log_height < 70) {
		runtime_log_height = 70;
	}
	MoveWindow(state->log, 14, 154 + list_height + task_log_height,
		width - 28, runtime_log_height, TRUE);
}

static BOOL create_controls(StandardServerState *state)
{
	LVCOLUMNW column;
	wchar_t computer_name[USBRELAY_STANDARD_NAME_CHARS];
	wchar_t computer_text[420];

	state->status = make_control(state, L"STATIC", L"读取服务状态...",
		SS_LEFT, 0, 14, 12, 900, 24, IDC_STATUS);
	state->computer = make_control(state, L"STATIC", L"",
		SS_LEFT, 0, 14, 44, 900, 22, IDC_COMPUTER);
	state->manage = make_control(state, L"BUTTON", L"打开打印机管理",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 14, 72, 180, 28, IDC_MANAGE);
	state->autostart = make_control(state, L"BUTTON",
		L"开机启动并驻留系统托盘",
		BS_AUTOCHECKBOX | WS_TABSTOP, 0, 204, 74, 250, 24,
		IDC_AUTOSTART);
	state->refresh = make_control(state, L"BUTTON", L"刷新打印机",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 586, 72, 112, 28, IDC_REFRESH);
	state->start = make_control(state, L"BUTTON", L"启动服务",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 712, 72, 112, 28, IDC_START);
	state->stop = make_control(state, L"BUTTON", L"停止服务",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 838, 72, 108, 28, IDC_STOP);
	state->printers = make_control(state, WC_LISTVIEWW, L"",
		LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
		WS_EX_CLIENTEDGE, 14, 108, 932, 280, IDC_PRINTERS);
	state->log = make_control(state, L"EDIT", L"",
		ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
		WS_EX_CLIENTEDGE, 14, 400, 932, 90, IDC_LOG);
	state->log_filter_label = make_control(state, L"STATIC",
		L"筛选：",
		SS_LEFT, 0, 14, 400, 64, 24, IDC_LOG_FILTER - 1);
	state->log_filter = make_control(state, L"EDIT", L"",
		ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
		86, 398, 300, 26, IDC_LOG_FILTER);
	state->apply_filter = make_control(state, L"BUTTON", L"筛选",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 394, 398, 82, 28,
		IDC_LOG_APPLY);
	state->clear_filter = make_control(state, L"BUTTON", L"清除筛选",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 484, 398, 94, 28,
		IDC_LOG_CLEAR_FILTER);
	state->clear_log_pool = make_control(state, L"BUTTON", L"清理日志池",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 650, 398, 132, 28,
		IDC_LOG_CLEAR_POOL);
	state->open_log_dir = make_control(state, L"BUTTON", L"打开日志目录",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 790, 398, 136, 28,
		IDC_LOG_OPEN);
	state->task_log = make_control(state, L"EDIT", L"",
		ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
		WS_EX_CLIENTEDGE, 14, 430, 932, 130, IDC_TASK_LOG);
	if (state->status == NULL || state->computer == NULL ||
		state->printers == NULL || state->log == NULL ||
		state->log_filter_label == NULL || state->log_filter == NULL ||
		state->apply_filter == NULL || state->clear_filter == NULL ||
		state->clear_log_pool == NULL || state->open_log_dir == NULL ||
		state->task_log == NULL ||
		state->refresh == NULL || state->start == NULL ||
		state->stop == NULL || state->manage == NULL ||
		state->autostart == NULL) {
		return FALSE;
	}
	ListView_SetExtendedListViewStyle(state->printers,
		LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
	ZeroMemory(&column, sizeof(column));
	column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
	column.cx = 180;
	column.pszText = L"打印机名称";
	ListView_InsertColumn(state->printers, 0, &column);
	column.cx = 150;
	column.pszText = L"驱动";
	ListView_InsertColumn(state->printers, 1, &column);
	column.cx = 105;
	column.pszText = L"原端口";
	ListView_InsertColumn(state->printers, 2, &column);
	column.cx = 80;
	column.pszText = L"发布端口";
	ListView_InsertColumn(state->printers, 3, &column);
	column.cx = 235;
	column.pszText = L"Standard TCP/IP 端点";
	ListView_InsertColumn(state->printers, 4, &column);
	column.cx = 250;
	column.pszText = L"状态";
	ListView_InsertColumn(state->printers, 5, &column);

	if (usbrelay_standard_get_computer_name(computer_name,
		UI_ARRAY_COUNT(computer_name))) {
		_snwprintf_s(computer_text, UI_ARRAY_COUNT(computer_text),
			_TRUNCATE,
			L"本机客户端连接地址使用计算机名：%ls；"
			L"RAW TCP 端口范围 9100-9199。",
			computer_name);
	}
	else {
		wcsncpy_s(computer_text, UI_ARRAY_COUNT(computer_text),
			L"RAW TCP 端口范围 9100-9199；"
			L"使用计算机名连接，不依赖固定 IP。", _TRUNCATE);
	}
	SetWindowTextW(state->computer, computer_text);
	SendMessageW(state->task_log, EM_SETLIMITTEXT,
		STANDARD_LOG_VIEW_MAX_CHARS + 1024, 0);
	return TRUE;
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
		return UiCreateLogonTask(USBRELAY_STANDARD_TASK_NAME, path,
			L"/autostart",
			L"打印机内网共享服务端开机启动并驻留托盘",
			&error) ? 0 : 1;
	}
	if (remove_autostart) {
		return UiDeleteLogonTask(USBRELAY_STANDARD_TASK_NAME, &error) ?
			0 : 1;
	}
	if (install) {
		return UiInstallShortcuts(path, USBRELAY_STANDARD_SHORTCUT_NAME,
			L"打印机内网共享服务端") ? 0 : 1;
	}
	return UiRemoveShortcuts(USBRELAY_STANDARD_SHORTCUT_NAME) ? 0 : 1;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message,
	WPARAM w_param, LPARAM l_param)
{
	StandardServerState *state;

	if (message == WM_NCCREATE) {
		CREATESTRUCTW *create = (CREATESTRUCTW *)l_param;
		state = (StandardServerState *)create->lpCreateParams;
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
		start_server(state);
		refresh_task_log(state);
		if (UiLogonTaskExists(USBRELAY_STANDARD_TASK_NAME, NULL)) {
			SendMessageW(state->autostart, BM_SETCHECK, BST_CHECKED, 0);
		}
		SetTimer(window, STANDARD_TIMER_STATUS, 2000, NULL);
		return 0;
	case WM_SIZE:
		layout(state);
		return 0;
	case WM_GETMINMAXINFO:
		((MINMAXINFO *)l_param)->ptMinTrackSize.x = 960;
		((MINMAXINFO *)l_param)->ptMinTrackSize.y = 640;
		return 0;
	case WM_COMMAND:
		switch (LOWORD(w_param)) {
		case IDC_REFRESH:
			if (HIWORD(w_param) == BN_CLICKED) {
				refresh_printers(state);
			}
			return 0;
		case IDC_START:
			if (HIWORD(w_param) == BN_CLICKED) {
				start_server(state);
			}
			return 0;
		case IDC_STOP:
			if (HIWORD(w_param) == BN_CLICKED) {
				stop_server(state);
			}
			return 0;
		case IDC_MANAGE:
			if (HIWORD(w_param) == BN_CLICKED) {
				open_printer_management(state);
			}
			return 0;
		case IDC_AUTOSTART:
			if (HIWORD(w_param) == BN_CLICKED) {
				toggle_startup(state);
			}
			return 0;
		case IDC_LOG_APPLY:
			if (HIWORD(w_param) == BN_CLICKED) {
				refresh_task_log(state);
			}
			return 0;
		case IDC_LOG_CLEAR_FILTER:
			if (HIWORD(w_param) == BN_CLICKED) {
				SetWindowTextW(state->log_filter, L"");
				refresh_task_log(state);
			}
			return 0;
		case IDC_LOG_CLEAR_POOL:
			if (HIWORD(w_param) == BN_CLICKED) {
				clear_task_log(state);
			}
			return 0;
		case IDC_LOG_OPEN:
			if (HIWORD(w_param) == BN_CLICKED) {
				open_task_log_directory(state);
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
	case WM_TIMER:
		if (w_param == STANDARD_TIMER_STATUS) {
			refresh_service(state);
			refresh_task_log(state);
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
		shutdown_server(state);
		return TRUE;
	case WM_DESTROY:
		shutdown_server(state);
		KillTimer(window, STANDARD_TIMER_STATUS);
		UiTrayRemove(window);
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
	StandardServerState state;
	MSG message;
	HANDLE mutex;
	HWND existing;
	int command_result;

	UNREFERENCED_PARAMETER(previous);
	UNREFERENCED_PARAMETER(command_line);
	if (has_argument(L"--engine")) {
		return usbrelay_standard_engine_run();
	}
	command_result = command_line_mode(instance);
	if (command_result >= 0) {
		return command_result;
	}
	mutex = CreateMutexW(NULL, TRUE, USBRELAY_STANDARD_UI_MUTEX_NAME);
	if (mutex == NULL) {
		return 1;
	}
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		existing = FindWindowW(STANDARD_WINDOW_CLASS, NULL);
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
	ZeroMemory(&state, sizeof(state));
	state.last_running = FALSE;
	state.hidden_start = has_argument(L"/autostart");
	state.font = UiCreateInterfaceFont();
	if (state.font == NULL) {
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
	window_class.lpszClassName = STANDARD_WINDOW_CLASS;
	if (!RegisterClassExW(&window_class)) {
		DeleteObject(state.font);
		CloseHandle(mutex);
		return 1;
	}
	state.window = CreateWindowExW(0, STANDARD_WINDOW_CLASS,
		STANDARD_WINDOW_TITLE,
		WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 1080, 650,
		NULL, NULL, instance, &state);
	if (state.window == NULL) {
		DeleteObject(state.font);
		CloseHandle(mutex);
		return 1;
	}
	UiCenterWindow(state.window);
	state.tray_icon = LoadIconW(instance,
		MAKEINTRESOURCEW(IDI_APP_ICON));
	state.tray_added = state.tray_icon != NULL &&
		UiTrayAdd(state.window, state.tray_icon,
			L"打印机内网共享服务端 - 双击打开");
	if (state.hidden_start) {
		ShowWindow(state.window, SW_HIDE);
	}
	else {
		ShowWindow(state.window,
			show_command == SW_HIDE ? SW_SHOWNORMAL : show_command);
	}
	UpdateWindow(state.window);
	while (GetMessageW(&message, NULL, 0, 0) > 0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	DeleteObject(state.font);
	CloseHandle(mutex);
	return (int)message.wParam;
}
