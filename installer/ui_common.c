#include "ui_common.h"

#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winsvc.h>

#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <wctype.h>

#define UI_MAX_COMMAND_LINE 32768
#define UI_MAX_CAPTURE_BYTES (4u * 1024u * 1024u)

static BOOL join_path(wchar_t *buffer, size_t buffer_count,
	const wchar_t *directory, const wchar_t *name)
{
	int result = _snwprintf_s(buffer, buffer_count, _TRUNCATE,
		L"%ls\\%ls", directory, name);
	return result >= 0;
}

BOOL UiGetModuleDirectory(wchar_t *buffer, size_t buffer_count)
{
	DWORD length;
	wchar_t *separator;

	if (buffer == NULL || buffer_count < 4) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	length = GetModuleFileNameW(NULL, buffer, (DWORD)buffer_count);
	if (length == 0 || length >= buffer_count) {
		if (length >= buffer_count) {
			SetLastError(ERROR_INSUFFICIENT_BUFFER);
		}
		return FALSE;
	}

	separator = wcsrchr(buffer, L'\\');
	if (separator == NULL) {
		SetLastError(ERROR_BAD_PATHNAME);
		return FALSE;
	}
	*separator = L'\0';
	return TRUE;
}

BOOL UiGetToolPath(const wchar_t *tool_name, wchar_t *buffer,
	size_t buffer_count)
{
	wchar_t directory[MAX_PATH];

	if (!UiGetModuleDirectory(directory, UI_ARRAY_COUNT(directory))) {
		return FALSE;
	}
	return join_path(buffer, buffer_count, directory, tool_name);
}

BOOL UiGetSystemToolPath(const wchar_t *tool_name, wchar_t *buffer,
	size_t buffer_count)
{
	wchar_t directory[MAX_PATH];
	UINT length = GetSystemDirectoryW(directory,
		(DWORD)UI_ARRAY_COUNT(directory));

	if (length == 0 || length >= UI_ARRAY_COUNT(directory)) {
		return FALSE;
	}
	return join_path(buffer, buffer_count, directory, tool_name);
}

static BOOL create_capture_file(wchar_t *path, size_t path_count,
	HANDLE *file)
{
	wchar_t temp_directory[MAX_PATH];
	DWORD length;
	unsigned int attempt;
	SECURITY_ATTRIBUTES security;

	length = GetTempPathW((DWORD)UI_ARRAY_COUNT(temp_directory),
		temp_directory);
	if (length == 0 || length >= UI_ARRAY_COUNT(temp_directory)) {
		return FALSE;
	}

	security.nLength = sizeof(security);
	security.lpSecurityDescriptor = NULL;
	security.bInheritHandle = TRUE;

	for (attempt = 0; attempt < 1000; attempt++) {
		int result = _snwprintf_s(path, path_count, _TRUNCATE,
			L"%lsusbrelay-ui-%lu-%u.log", temp_directory,
			GetCurrentProcessId(), attempt);
		if (result < 0) {
			return FALSE;
		}

		*file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
			CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
		if (*file != INVALID_HANDLE_VALUE) {
			return TRUE;
		}
		if (GetLastError() != ERROR_FILE_EXISTS &&
			GetLastError() != ERROR_ALREADY_EXISTS) {
			return FALSE;
		}
	}

	SetLastError(ERROR_ALREADY_EXISTS);
	return FALSE;
}

static BOOL read_capture_file(const wchar_t *path,
	UiCommandResult *result)
{
	HANDLE file;
	LARGE_INTEGER size;
	DWORD bytes_read;
	DWORD bytes_to_read;
	char *bytes;
	int wide_chars;
	DWORD code_page;

	file = CreateFileW(path, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		return FALSE;
	}

	if (!GetFileSizeEx(file, &size)) {
		CloseHandle(file);
		return FALSE;
	}
	if (size.QuadPart <= 0) {
		CloseHandle(file);
		result->output = (wchar_t *)HeapAlloc(GetProcessHeap(),
			HEAP_ZERO_MEMORY, sizeof(wchar_t));
		return result->output != NULL;
	}
	if (size.QuadPart > UI_MAX_CAPTURE_BYTES) {
		CloseHandle(file);
		SetLastError(ERROR_FILE_TOO_LARGE);
		return FALSE;
	}

	bytes_to_read = (DWORD)size.QuadPart;
	bytes = (char *)HeapAlloc(GetProcessHeap(), 0,
		(size_t)bytes_to_read + 1);
	if (bytes == NULL) {
		CloseHandle(file);
		return FALSE;
	}

	if (!ReadFile(file, bytes, bytes_to_read, &bytes_read, NULL) ||
		bytes_read != bytes_to_read) {
		HeapFree(GetProcessHeap(), 0, bytes);
		CloseHandle(file);
		return FALSE;
	}
	CloseHandle(file);
	bytes[bytes_read] = '\0';

	code_page = GetOEMCP();
	wide_chars = MultiByteToWideChar(code_page, 0, bytes,
		(int)bytes_read, NULL, 0);
	if (wide_chars <= 0) {
		code_page = CP_ACP;
		wide_chars = MultiByteToWideChar(code_page, 0, bytes,
			(int)bytes_read, NULL, 0);
	}
	if (wide_chars <= 0) {
		HeapFree(GetProcessHeap(), 0, bytes);
		SetLastError(ERROR_NO_UNICODE_TRANSLATION);
		return FALSE;
	}

	result->output = (wchar_t *)HeapAlloc(GetProcessHeap(),
		HEAP_ZERO_MEMORY, ((size_t)wide_chars + 1) * sizeof(wchar_t));
	if (result->output == NULL) {
		HeapFree(GetProcessHeap(), 0, bytes);
		return FALSE;
	}
	if (MultiByteToWideChar(code_page, 0, bytes, (int)bytes_read,
		result->output, wide_chars) <= 0) {
		HeapFree(GetProcessHeap(), 0, result->output);
		result->output = NULL;
		HeapFree(GetProcessHeap(), 0, bytes);
		SetLastError(ERROR_NO_UNICODE_TRANSLATION);
		return FALSE;
	}
	result->output_chars = (size_t)wide_chars;
	HeapFree(GetProcessHeap(), 0, bytes);
	return TRUE;
}

BOOL UiRunCommandCapture(const wchar_t *executable,
	const wchar_t *arguments, const wchar_t *working_directory,
	DWORD timeout_ms, UiCommandResult *result)
{
	wchar_t command_line[UI_MAX_COMMAND_LINE];
	wchar_t capture_path[MAX_PATH];
	HANDLE capture_file = INVALID_HANDLE_VALUE;
	HANDLE nul_file = INVALID_HANDLE_VALUE;
	SECURITY_ATTRIBUTES security;
	STARTUPINFOW startup;
	PROCESS_INFORMATION process;
	DWORD wait_result;
	BOOL success = FALSE;
	int result_length;

	if (result == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ZeroMemory(result, sizeof(*result));

	result_length = _snwprintf_s(command_line,
		UI_ARRAY_COUNT(command_line), _TRUNCATE,
		L"\"%ls\" %ls", executable,
		arguments != NULL ? arguments : L"");
	if (result_length < 0) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}

	if (!create_capture_file(capture_path,
		UI_ARRAY_COUNT(capture_path), &capture_file)) {
		return FALSE;
	}

	security.nLength = sizeof(security);
	security.lpSecurityDescriptor = NULL;
	security.bInheritHandle = TRUE;
	nul_file = CreateFileW(L"NUL", GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (nul_file == INVALID_HANDLE_VALUE) {
		CloseHandle(capture_file);
		DeleteFileW(capture_path);
		return FALSE;
	}

	ZeroMemory(&startup, sizeof(startup));
	ZeroMemory(&process, sizeof(process));
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES |
		STARTF_USESHOWWINDOW;
	startup.wShowWindow = SW_HIDE;
	startup.hStdInput = nul_file;
	startup.hStdOutput = capture_file;
	startup.hStdError = capture_file;

	if (!CreateProcessW(executable, command_line, NULL, NULL, TRUE,
		CREATE_NO_WINDOW, NULL, working_directory, &startup,
		&process)) {
		goto cleanup;
	}

	CloseHandle(capture_file);
	capture_file = INVALID_HANDLE_VALUE;
	CloseHandle(nul_file);
	nul_file = INVALID_HANDLE_VALUE;

	wait_result = WaitForSingleObject(process.hProcess, timeout_ms);
	if (wait_result == WAIT_TIMEOUT) {
		result->timed_out = TRUE;
		TerminateProcess(process.hProcess, ERROR_TIMEOUT);
		WaitForSingleObject(process.hProcess, 5000);
	}
	else if (wait_result != WAIT_OBJECT_0) {
		TerminateProcess(process.hProcess, ERROR_GEN_FAILURE);
		WaitForSingleObject(process.hProcess, 1000);
	}

	if (!GetExitCodeProcess(process.hProcess, &result->exit_code)) {
		result->exit_code = ERROR_GEN_FAILURE;
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	process.hThread = NULL;
	process.hProcess = NULL;

	if (!read_capture_file(capture_path, result)) {
		goto cleanup;
	}
	success = TRUE;

cleanup:
	if (capture_file != INVALID_HANDLE_VALUE) {
		CloseHandle(capture_file);
	}
	if (nul_file != INVALID_HANDLE_VALUE) {
		CloseHandle(nul_file);
	}
	if (process.hThread != NULL) {
		CloseHandle(process.hThread);
	}
	if (process.hProcess != NULL) {
		CloseHandle(process.hProcess);
	}
	DeleteFileW(capture_path);
	if (!success) {
		UiFreeCommandResult(result);
	}
	return success;
}

void UiFreeCommandResult(UiCommandResult *result)
{
	if (result == NULL) {
		return;
	}
	if (result->output != NULL) {
		HeapFree(GetProcessHeap(), 0, result->output);
	}
	ZeroMemory(result, sizeof(*result));
}

BOOL UiStartHiddenProcess(const wchar_t *executable,
	const wchar_t *arguments, const wchar_t *working_directory,
	UiProcess *process)
{
	wchar_t command_line[UI_MAX_COMMAND_LINE];
	STARTUPINFOW startup;
	int result_length;

	if (process == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ZeroMemory(process, sizeof(*process));

	result_length = _snwprintf_s(command_line,
		UI_ARRAY_COUNT(command_line), _TRUNCATE,
		L"\"%ls\" %ls", executable,
		arguments != NULL ? arguments : L"");
	if (result_length < 0) {
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}

	ZeroMemory(&startup, sizeof(startup));
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESHOWWINDOW;
	startup.wShowWindow = SW_HIDE;

	if (!CreateProcessW(executable, command_line, NULL, NULL, FALSE,
		CREATE_NO_WINDOW, NULL, working_directory, &startup,
		&process->process)) {
		return FALSE;
	}
	CloseHandle(process->process.hThread);
	process->process.hThread = NULL;
	return TRUE;
}

BOOL UiWaitForProcess(UiProcess *process, DWORD timeout_ms,
	DWORD *exit_code)
{
	DWORD wait_result;

	if (process == NULL || process->process.hProcess == NULL) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	wait_result = WaitForSingleObject(process->process.hProcess,
		timeout_ms);
	if (wait_result == WAIT_TIMEOUT) {
		return FALSE;
	}
	if (wait_result != WAIT_OBJECT_0) {
		return FALSE;
	}
	return GetExitCodeProcess(process->process.hProcess, exit_code) != 0;
}

void UiCloseProcess(UiProcess *process, BOOL terminate)
{
	if (process == NULL) {
		return;
	}
	if (process->process.hProcess != NULL) {
		if (terminate &&
			WaitForSingleObject(process->process.hProcess, 0) ==
				WAIT_TIMEOUT) {
			TerminateProcess(process->process.hProcess, ERROR_CANCELLED);
			WaitForSingleObject(process->process.hProcess, 3000);
		}
		CloseHandle(process->process.hProcess);
	}
	if (process->process.hThread != NULL) {
		CloseHandle(process->process.hThread);
	}
	ZeroMemory(process, sizeof(*process));
}

HFONT UiCreateInterfaceFont(void)
{
	return CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
		L"Microsoft YaHei UI");
}

static BOOL CALLBACK apply_font_to_child(HWND child, LPARAM parameter)
{
	SendMessageW(child, WM_SETFONT, (WPARAM)parameter, TRUE);
	return TRUE;
}

void UiApplyFont(HWND window, HFONT font)
{
	SendMessageW(window, WM_SETFONT, (WPARAM)font, TRUE);
	EnumChildWindows(window, apply_font_to_child, (LPARAM)font);
}

void UiCenterWindow(HWND window)
{
	RECT window_rect;
	RECT work_area;
	int width;
	int height;
	int x;
	int y;

	if (!GetWindowRect(window, &window_rect)) {
		return;
	}
	if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0)) {
		return;
	}

	width = window_rect.right - window_rect.left;
	height = window_rect.bottom - window_rect.top;
	x = work_area.left + ((work_area.right - work_area.left) - width) / 2;
	y = work_area.top + ((work_area.bottom - work_area.top) - height) / 2;
	if (x < work_area.left) {
		x = work_area.left;
	}
	if (y < work_area.top) {
		y = work_area.top;
	}
	SetWindowPos(window, NULL, x, y, 0, 0,
		SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
}

void UiAppendLog(HWND log_window, const wchar_t *format, ...)
{
	wchar_t message[2048];
	wchar_t line[2304];
	va_list arguments;
	int text_length;

	if (log_window == NULL || format == NULL) {
		return;
	}

	va_start(arguments, format);
	_vsnwprintf_s(message, UI_ARRAY_COUNT(message), _TRUNCATE,
		format, arguments);
	va_end(arguments);

	_snwprintf_s(line, UI_ARRAY_COUNT(line), _TRUNCATE,
		L"%ls\r\n", message);

	text_length = GetWindowTextLengthW(log_window);
	if (text_length > 60000) {
		SetWindowTextW(log_window, L"");
		text_length = 0;
	}
	SendMessageW(log_window, EM_SETSEL, text_length, text_length);
	SendMessageW(log_window, EM_REPLACESEL, FALSE, (LPARAM)line);
}

static void format_windows_error(DWORD error, wchar_t *buffer,
	size_t buffer_count)
{
	DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0, buffer,
		(DWORD)buffer_count, NULL);

	if (length == 0) {
		_snwprintf_s(buffer, buffer_count, _TRUNCATE,
			L"Windows error %lu.", error);
		return;
	}
	while (length > 0 &&
		(buffer[length - 1] == L'\r' || buffer[length - 1] == L'\n' ||
			buffer[length - 1] == L' ')) {
		buffer[--length] = L'\0';
	}
}

void UiShowError(HWND owner, const wchar_t *message)
{
	MessageBoxW(owner, message, L"打印机内网共享", MB_OK | MB_ICONERROR);
}

void UiShowLastError(HWND owner, const wchar_t *operation)
{
	wchar_t details[1024];
	wchar_t message[1400];

	format_windows_error(GetLastError(), details,
		UI_ARRAY_COUNT(details));
	_snwprintf_s(message, UI_ARRAY_COUNT(message), _TRUNCATE,
		L"%ls\r\n\r\n%ls", operation, details);
	UiShowError(owner, message);
}

BOOL UiIsValidServerAddress(const wchar_t *address)
{
	size_t length;
	size_t index;

	if (address == NULL) {
		return FALSE;
	}
	length = wcslen(address);
	if (length == 0 || length > 253) {
		return FALSE;
	}
	for (index = 0; index < length; index++) {
		wchar_t character = address[index];
		if (!iswalnum(character) && character != L'.' &&
			character != L'-' && character != L'_') {
			return FALSE;
		}
	}
	return TRUE;
}

BOOL UiIsValidBusId(const wchar_t *busid)
{
	size_t length;
	size_t index;
	BOOL has_hyphen = FALSE;

	if (busid == NULL) {
		return FALSE;
	}
	length = wcslen(busid);
	if (length == 0 || length > 31) {
		return FALSE;
	}
	for (index = 0; index < length; index++) {
		if (busid[index] == L'-') {
			has_hyphen = TRUE;
		}
		else if (!iswdigit(busid[index])) {
			return FALSE;
		}
	}
	return has_hyphen;
}

void UiTrimWhitespace(wchar_t *text)
{
	wchar_t *start;
	wchar_t *end;

	if (text == NULL) {
		return;
	}
	start = text;
	while (*start == L' ' || *start == L'\t' ||
		*start == L'\r' || *start == L'\n') {
		start++;
	}
	if (start != text) {
		memmove(text, start, (wcslen(start) + 1) * sizeof(wchar_t));
	}

	end = text + wcslen(text);
	while (end > text &&
		(end[-1] == L' ' || end[-1] == L'\t' ||
			end[-1] == L'\r' || end[-1] == L'\n')) {
		--end;
	}
	*end = L'\0';
}

BOOL UiSetRegistryString(HKEY root, const wchar_t *subkey,
	const wchar_t *name, const wchar_t *value)
{
	HKEY key;
	LONG status;
	DWORD byte_count;

	status = RegCreateKeyExW(root, subkey, 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL);
	if (status != ERROR_SUCCESS) {
		SetLastError((DWORD)status);
		return FALSE;
	}
	byte_count = (DWORD)((wcslen(value) + 1) * sizeof(wchar_t));
	status = RegSetValueExW(key, name, 0, REG_SZ,
		(const BYTE *)value, byte_count);
	RegCloseKey(key);
	if (status != ERROR_SUCCESS) {
		SetLastError((DWORD)status);
		return FALSE;
	}
	return TRUE;
}

BOOL UiGetRegistryString(HKEY root, const wchar_t *subkey,
	const wchar_t *name, wchar_t *value, DWORD value_chars)
{
	HKEY key;
	LONG status;
	DWORD type = 0;
	DWORD byte_count = value_chars * sizeof(wchar_t);

	if (value == NULL || value_chars == 0) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	value[0] = L'\0';

	status = RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key);
	if (status != ERROR_SUCCESS) {
		SetLastError((DWORD)status);
		return FALSE;
	}
	status = RegQueryValueExW(key, name, NULL, &type,
		(BYTE *)value, &byte_count);
	RegCloseKey(key);
	if (status != ERROR_SUCCESS || type != REG_SZ) {
		SetLastError((DWORD)status);
		return FALSE;
	}
	value[value_chars - 1] = L'\0';
	return TRUE;
}

BOOL UiSetRegistryDword(HKEY root, const wchar_t *subkey,
	const wchar_t *name, DWORD value)
{
	HKEY key;
	LONG status;

	status = RegCreateKeyExW(root, subkey, 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL);
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

DWORD UiGetRegistryDword(HKEY root, const wchar_t *subkey,
	const wchar_t *name, DWORD default_value)
{
	HKEY key;
	LONG status;
	DWORD type = 0;
	DWORD value = default_value;
	DWORD byte_count = sizeof(value);

	status = RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key);
	if (status != ERROR_SUCCESS) {
		return default_value;
	}
	status = RegQueryValueExW(key, name, NULL, &type,
		(BYTE *)&value, &byte_count);
	RegCloseKey(key);
	if (status != ERROR_SUCCESS || type != REG_DWORD) {
		return default_value;
	}
	return value;
}

BOOL UiQueryService(const wchar_t *service_name, DWORD *state,
	DWORD *start_type, DWORD *win32_error)
{
	SC_HANDLE manager = NULL;
	SC_HANDLE service = NULL;
	SERVICE_STATUS status;
	QUERY_SERVICE_CONFIGW *config = NULL;
	DWORD needed = 0;
	DWORD error = ERROR_SUCCESS;
	BOOL success = FALSE;

	manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (manager == NULL) {
		error = GetLastError();
		goto cleanup;
	}
	service = OpenServiceW(manager, service_name,
		SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
	if (service == NULL) {
		error = GetLastError();
		goto cleanup;
	}

	if (!QueryServiceStatus(service, &status)) {
		error = GetLastError();
		goto cleanup;
	}
	QueryServiceConfigW(service, NULL, 0, &needed);
	if (needed == 0) {
		error = GetLastError();
		goto cleanup;
	}
	config = (QUERY_SERVICE_CONFIGW *)HeapAlloc(GetProcessHeap(), 0,
		needed);
	if (config == NULL) {
		error = ERROR_NOT_ENOUGH_MEMORY;
		goto cleanup;
	}
	if (!QueryServiceConfigW(service, config, needed, &needed)) {
		error = GetLastError();
		goto cleanup;
	}

	if (state != NULL) {
		*state = status.dwCurrentState;
	}
	if (start_type != NULL) {
		*start_type = config->dwStartType;
	}
	success = TRUE;

cleanup:
	if (config != NULL) {
		HeapFree(GetProcessHeap(), 0, config);
	}
	if (service != NULL) {
		CloseServiceHandle(service);
	}
	if (manager != NULL) {
		CloseServiceHandle(manager);
	}
	if (win32_error != NULL) {
		*win32_error = error;
	}
	SetLastError(error);
	return success;
}

BOOL UiSetServiceStartType(const wchar_t *service_name,
	DWORD start_type, DWORD *win32_error)
{
	SC_HANDLE manager = NULL;
	SC_HANDLE service = NULL;
	DWORD error = ERROR_SUCCESS;
	BOOL success = FALSE;

	manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (manager == NULL) {
		error = GetLastError();
		goto cleanup;
	}
	service = OpenServiceW(manager, service_name,
		SERVICE_CHANGE_CONFIG);
	if (service == NULL) {
		error = GetLastError();
		goto cleanup;
	}
	if (!ChangeServiceConfigW(service, SERVICE_NO_CHANGE, start_type,
		SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL, NULL, NULL,
		NULL)) {
		error = GetLastError();
		goto cleanup;
	}
	success = TRUE;

cleanup:
	if (service != NULL) {
		CloseServiceHandle(service);
	}
	if (manager != NULL) {
		CloseServiceHandle(manager);
	}
	if (win32_error != NULL) {
		*win32_error = error;
	}
	SetLastError(error);
	return success;
}

BOOL UiStartService(const wchar_t *service_name, DWORD *win32_error)
{
	SC_HANDLE manager = NULL;
	SC_HANDLE service = NULL;
	DWORD error = ERROR_SUCCESS;
	BOOL success = FALSE;

	manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (manager == NULL) {
		error = GetLastError();
		goto cleanup;
	}
	service = OpenServiceW(manager, service_name,
		SERVICE_START | SERVICE_QUERY_STATUS);
	if (service == NULL) {
		error = GetLastError();
		goto cleanup;
	}
	if (!StartServiceW(service, 0, NULL)) {
		error = GetLastError();
		if (error == ERROR_SERVICE_ALREADY_RUNNING) {
			error = ERROR_SUCCESS;
			success = TRUE;
		}
		goto cleanup;
	}
	success = TRUE;

cleanup:
	if (service != NULL) {
		CloseServiceHandle(service);
	}
	if (manager != NULL) {
		CloseServiceHandle(manager);
	}
	if (win32_error != NULL) {
		*win32_error = error;
	}
	SetLastError(error);
	return success;
}

BOOL UiStopService(const wchar_t *service_name, DWORD *win32_error)
{
	SC_HANDLE manager = NULL;
	SC_HANDLE service = NULL;
	SERVICE_STATUS status;
	DWORD error = ERROR_SUCCESS;
	BOOL success = FALSE;

	manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (manager == NULL) {
		error = GetLastError();
		goto cleanup;
	}
	service = OpenServiceW(manager, service_name,
		SERVICE_STOP | SERVICE_QUERY_STATUS);
	if (service == NULL) {
		error = GetLastError();
		goto cleanup;
	}
	if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
		error = GetLastError();
		if (error == ERROR_SERVICE_NOT_ACTIVE) {
			error = ERROR_SUCCESS;
			success = TRUE;
		}
		goto cleanup;
	}
	success = TRUE;

cleanup:
	if (service != NULL) {
		CloseServiceHandle(service);
	}
	if (manager != NULL) {
		CloseServiceHandle(manager);
	}
	if (win32_error != NULL) {
		*win32_error = error;
	}
	SetLastError(error);
	return success;
}

static BOOL xml_escape(const wchar_t *source, wchar_t *destination,
	size_t destination_count)
{
	size_t output = 0;
	size_t index;

	if (source == NULL || destination == NULL || destination_count == 0) {
		return FALSE;
	}
	for (index = 0; source[index] != L'\0'; index++) {
		const wchar_t *replacement = NULL;

		switch (source[index]) {
		case L'&':
			replacement = L"&amp;";
			break;
		case L'<':
			replacement = L"&lt;";
			break;
		case L'>':
			replacement = L"&gt;";
			break;
		case L'"':
			replacement = L"&quot;";
			break;
		case L'\'':
			replacement = L"&apos;";
			break;
		default:
			break;
		}
		if (replacement != NULL) {
			size_t replacement_length = wcslen(replacement);
			if (output + replacement_length >= destination_count) {
				return FALSE;
			}
			wmemcpy(destination + output, replacement,
				replacement_length);
			output += replacement_length;
		}
		else {
			if (output + 1 >= destination_count) {
				return FALSE;
			}
			destination[output++] = source[index];
		}
	}
	destination[output] = L'\0';
	return TRUE;
}

static BOOL get_current_account(wchar_t *account, size_t account_count)
{
	wchar_t user[128];
	wchar_t domain[128];
	DWORD user_length = (DWORD)UI_ARRAY_COUNT(user);
	DWORD domain_length;

	if (!GetUserNameW(user, &user_length) || user_length == 0) {
		return FALSE;
	}
	domain_length = GetEnvironmentVariableW(L"USERDOMAIN", domain,
		(DWORD)UI_ARRAY_COUNT(domain));
	if (domain_length > 0 && domain_length < UI_ARRAY_COUNT(domain)) {
		return _snwprintf_s(account, account_count, _TRUNCATE,
			L"%ls\\%ls", domain, user) >= 0;
	}
	return wcsncpy_s(account, account_count, user, _TRUNCATE) == 0;
}

static BOOL write_utf16_file(const wchar_t *path, const wchar_t *text)
{
	HANDLE file;
	DWORD written;
	DWORD bytes;
	BOOL success = FALSE;
	const BYTE bom[2] = { 0xff, 0xfe };

	file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_TEMPORARY, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		return FALSE;
	}
	if (!WriteFile(file, bom, sizeof(bom), &written, NULL) ||
		written != sizeof(bom)) {
		goto cleanup;
	}
	bytes = (DWORD)(wcslen(text) * sizeof(wchar_t));
	if (!WriteFile(file, text, bytes, &written, NULL) ||
		written != bytes) {
		goto cleanup;
	}
	success = TRUE;

cleanup:
	CloseHandle(file);
	return success;
}

static BOOL create_logon_task_xml(wchar_t *path, size_t path_count,
	const wchar_t *executable, const wchar_t *arguments,
	const wchar_t *description)
{
	wchar_t temp_directory[MAX_PATH];
	wchar_t account[MAX_PATH];
	wchar_t escaped_executable[2048];
	wchar_t escaped_arguments[1024];
	wchar_t escaped_description[1024];
	wchar_t escaped_account[512];
	wchar_t escaped_working_directory[2048];
	wchar_t working_directory[MAX_PATH];
	wchar_t *separator;
	wchar_t xml[8192];
	DWORD length;
	int result;

	length = GetTempPathW((DWORD)UI_ARRAY_COUNT(temp_directory),
		temp_directory);
	if (length == 0 || length >= UI_ARRAY_COUNT(temp_directory)) {
		return FALSE;
	}
	result = _snwprintf_s(path, path_count, _TRUNCATE,
		L"%lsusbrelay-client-task-%lu.xml", temp_directory,
		GetCurrentProcessId());
	if (result < 0) {
		return FALSE;
	}

	if (!get_current_account(account, UI_ARRAY_COUNT(account)) ||
		!xml_escape(executable, escaped_executable,
			UI_ARRAY_COUNT(escaped_executable)) ||
		!xml_escape(arguments, escaped_arguments,
			UI_ARRAY_COUNT(escaped_arguments)) ||
		!xml_escape(description, escaped_description,
			UI_ARRAY_COUNT(escaped_description)) ||
		!xml_escape(account, escaped_account,
			UI_ARRAY_COUNT(escaped_account))) {
		return FALSE;
	}
	if (wcsncpy_s(working_directory, UI_ARRAY_COUNT(working_directory),
		executable, _TRUNCATE) != 0) {
		return FALSE;
	}
	separator = wcsrchr(working_directory, L'\\');
	if (separator == NULL) {
		return FALSE;
	}
	*separator = L'\0';
	if (!xml_escape(working_directory, escaped_working_directory,
		UI_ARRAY_COUNT(escaped_working_directory))) {
		return FALSE;
	}

	result = _snwprintf_s(xml, UI_ARRAY_COUNT(xml), _TRUNCATE,
		L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
		L"<Task version=\"1.2\" "
		L"xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
		L"  <RegistrationInfo><Description>%ls</Description></RegistrationInfo>\r\n"
		L"  <Triggers><LogonTrigger><Enabled>true</Enabled>"
		L"<UserId>%ls</UserId><Delay>PT10S</Delay></LogonTrigger></Triggers>\r\n"
		L"  <Principals><Principal id=\"Author\"><UserId>%ls</UserId>"
		L"<LogonType>InteractiveToken</LogonType>"
		L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
		L"  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
		L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
		L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
		L"<AllowHardTerminate>true</AllowHardTerminate>"
		L"<StartWhenAvailable>true</StartWhenAvailable>"
		L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>"
		L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd>"
		L"<RestartOnIdle>false</RestartOnIdle></IdleSettings>"
		L"<AllowStartOnDemand>true</AllowStartOnDemand>"
		L"<Enabled>true</Enabled><Hidden>false</Hidden>"
		L"<RunOnlyIfIdle>false</RunOnlyIfIdle><WakeToRun>false</WakeToRun>"
		L"<Priority>7</Priority></Settings>\r\n"
		L"  <Actions Context=\"Author\"><Exec><Command>%ls</Command>"
		L"<Arguments>%ls</Arguments><WorkingDirectory>%ls</WorkingDirectory>"
		L"</Exec></Actions>\r\n"
		L"</Task>\r\n",
		escaped_description, escaped_account, escaped_account,
		escaped_executable, escaped_arguments,
		escaped_working_directory);
	if (result < 0) {
		return FALSE;
	}
	return write_utf16_file(path, xml);
}

BOOL UiCreateLogonTask(const wchar_t *task_name,
	const wchar_t *executable, const wchar_t *arguments,
	const wchar_t *description, DWORD *win32_error)
{
	wchar_t xml_path[MAX_PATH] = { 0 };
	wchar_t schtasks[MAX_PATH];
	wchar_t command_arguments[2600];
	wchar_t account[MAX_PATH];
	wchar_t task_run[2048];
	UiCommandResult result;
	DWORD error = ERROR_SUCCESS;
	BOOL success = FALSE;

	if (!UiGetSystemToolPath(L"schtasks.exe", schtasks,
		UI_ARRAY_COUNT(schtasks))) {
		error = GetLastError();
		goto cleanup;
	}

	/* The XML form is preferred when available. Some Windows 7/10
	 * configurations reject otherwise valid XML task definitions, so fall
	 * back to the stable schtasks command-line form. */
	if (create_logon_task_xml(xml_path, UI_ARRAY_COUNT(xml_path),
		executable, arguments, description)) {
		if (_snwprintf_s(command_arguments,
			UI_ARRAY_COUNT(command_arguments), _TRUNCATE,
			L"/Create /TN \"%ls\" /XML \"%ls\" /F",
			task_name, xml_path) < 0) {
			error = ERROR_INSUFFICIENT_BUFFER;
			goto cleanup;
		}
		if (UiRunCommandCapture(schtasks, command_arguments, NULL,
			15000, &result)) {
			success = result.exit_code == 0;
			if (!success) {
				error = result.exit_code != 0 ? result.exit_code :
					ERROR_GEN_FAILURE;
			}
			UiFreeCommandResult(&result);
		}
		else {
			error = GetLastError();
		}
	}

	if (!success) {
		if (!get_current_account(account, UI_ARRAY_COUNT(account)) ||
			_snwprintf_s(task_run, UI_ARRAY_COUNT(task_run),
				_TRUNCATE, L"\"%ls\" %ls", executable,
				arguments != NULL ? arguments : L"") < 0 ||
			_snwprintf_s(command_arguments,
				UI_ARRAY_COUNT(command_arguments), _TRUNCATE,
				L"/Create /TN \"%ls\" /SC ONLOGON "
				L"/TR \"\\\"%ls\\\" %ls\" /RU \"%ls\" "
				L"/IT /RL HIGHEST /F",
				task_name, executable,
				arguments != NULL ? arguments : L"",
				account) < 0) {
			error = ERROR_INSUFFICIENT_BUFFER;
			goto cleanup;
		}
		if (!UiRunCommandCapture(schtasks, command_arguments, NULL,
			15000, &result)) {
			error = GetLastError();
			goto cleanup;
		}
		success = result.exit_code == 0;
		if (!success) {
			error = result.exit_code != 0 ? result.exit_code :
				ERROR_GEN_FAILURE;
		}
		UiFreeCommandResult(&result);
	}

cleanup:
	if (xml_path[0] != L'\0') {
		DeleteFileW(xml_path);
	}
	if (win32_error != NULL) {
		*win32_error = error;
	}
	SetLastError(error);
	return success;
}

BOOL UiDeleteLogonTask(const wchar_t *task_name, DWORD *win32_error)
{
	wchar_t schtasks[MAX_PATH];
	wchar_t command_arguments[1024];
	UiCommandResult result;
	DWORD error = ERROR_SUCCESS;
	BOOL success = FALSE;

	if (!UiGetSystemToolPath(L"schtasks.exe", schtasks,
		UI_ARRAY_COUNT(schtasks))) {
		error = GetLastError();
		goto cleanup;
	}
	if (_snwprintf_s(command_arguments,
		UI_ARRAY_COUNT(command_arguments), _TRUNCATE,
		L"/Delete /TN \"%ls\" /F", task_name) < 0) {
		error = ERROR_INSUFFICIENT_BUFFER;
		goto cleanup;
	}
	if (!UiRunCommandCapture(schtasks, command_arguments, NULL,
		15000, &result)) {
		error = GetLastError();
		goto cleanup;
	}
	if (result.exit_code == 0) {
		success = TRUE;
	}
	else {
		/* Deleting an already absent task is a successful cleanup. */
		if (result.output != NULL &&
			(wcsstr(result.output, L"cannot find") != NULL ||
				wcsstr(result.output, L"找不到") != NULL ||
				wcsstr(result.output, L"系统找不到") != NULL)) {
			success = TRUE;
			error = ERROR_SUCCESS;
		}
		else {
			error = result.exit_code != 0 ? result.exit_code :
				ERROR_GEN_FAILURE;
		}
	}
	UiFreeCommandResult(&result);

cleanup:
	if (win32_error != NULL) {
		*win32_error = error;
	}
	SetLastError(error);
	return success;
}

BOOL UiLogonTaskExists(const wchar_t *task_name, DWORD *win32_error)
{
	wchar_t schtasks[MAX_PATH];
	wchar_t command_arguments[1024];
	UiCommandResult result;
	DWORD error = ERROR_SUCCESS;
	BOOL exists = FALSE;

	if (!UiGetSystemToolPath(L"schtasks.exe", schtasks,
		UI_ARRAY_COUNT(schtasks))) {
		error = GetLastError();
		goto cleanup;
	}
	if (_snwprintf_s(command_arguments,
		UI_ARRAY_COUNT(command_arguments), _TRUNCATE,
		L"/Query /TN \"%ls\"", task_name) < 0) {
		error = ERROR_INSUFFICIENT_BUFFER;
		goto cleanup;
	}
	if (!UiRunCommandCapture(schtasks, command_arguments, NULL,
		15000, &result)) {
		error = GetLastError();
		goto cleanup;
	}
	exists = result.exit_code == 0;
	if (!exists) {
		error = ERROR_FILE_NOT_FOUND;
	}
	UiFreeCommandResult(&result);

cleanup:
	if (win32_error != NULL) {
		*win32_error = error;
	}
	SetLastError(error);
	return exists;
}

static BOOL create_shell_shortcut(const wchar_t *directory,
	const wchar_t *shortcut_name, const wchar_t *executable,
	const wchar_t *description)
{
	wchar_t shortcut_path[MAX_PATH];
	wchar_t working_directory[MAX_PATH];
	wchar_t *separator;
	IShellLinkW *link = NULL;
	IPersistFile *persist = NULL;
	HRESULT status;
	BOOL success = FALSE;

	if (!join_path(shortcut_path, UI_ARRAY_COUNT(shortcut_path),
		directory, shortcut_name) ||
		wcslen(shortcut_path) + 4 >= UI_ARRAY_COUNT(shortcut_path)) {
		return FALSE;
	}
	wcscat_s(shortcut_path, UI_ARRAY_COUNT(shortcut_path), L".lnk");

	if (wcsncpy_s(working_directory,
		UI_ARRAY_COUNT(working_directory), executable,
		_TRUNCATE) != 0) {
		return FALSE;
	}
	separator = wcsrchr(working_directory, L'\\');
	if (separator == NULL) {
		return FALSE;
	}
	*separator = L'\0';

	status = CoCreateInstance(&CLSID_ShellLink, NULL,
		CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
	if (FAILED(status)) {
		goto cleanup;
	}
	status = link->lpVtbl->SetPath(link, executable);
	if (FAILED(status)) {
		goto cleanup;
	}
	link->lpVtbl->SetDescription(link, description);
	link->lpVtbl->SetWorkingDirectory(link, working_directory);
	link->lpVtbl->SetIconLocation(link, executable, 0);

	status = link->lpVtbl->QueryInterface(link, &IID_IPersistFile,
		(void **)&persist);
	if (FAILED(status)) {
		goto cleanup;
	}
	status = persist->lpVtbl->Save(persist, shortcut_path, TRUE);
	if (SUCCEEDED(status)) {
		success = TRUE;
	}

cleanup:
	if (persist != NULL) {
		persist->lpVtbl->Release(persist);
	}
	if (link != NULL) {
		link->lpVtbl->Release(link);
	}
	return success;
}

static BOOL remove_shell_shortcut(const wchar_t *directory,
	const wchar_t *shortcut_name)
{
	wchar_t shortcut_path[MAX_PATH];

	if (!join_path(shortcut_path, UI_ARRAY_COUNT(shortcut_path),
		directory, shortcut_name) ||
		wcslen(shortcut_path) + 4 >= UI_ARRAY_COUNT(shortcut_path)) {
		return FALSE;
	}
	wcscat_s(shortcut_path, UI_ARRAY_COUNT(shortcut_path), L".lnk");
	if (DeleteFileW(shortcut_path)) {
		return TRUE;
	}
	return GetLastError() == ERROR_FILE_NOT_FOUND ||
		GetLastError() == ERROR_PATH_NOT_FOUND;
}

BOOL UiInstallShortcuts(const wchar_t *executable,
	const wchar_t *shortcut_name, const wchar_t *description)
{
	wchar_t user_desktop[MAX_PATH];
	wchar_t desktop[MAX_PATH];
	wchar_t programs[MAX_PATH];
	HRESULT com_status;
	BOOL created = FALSE;

	com_status = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(com_status) && com_status != RPC_E_CHANGED_MODE) {
		return FALSE;
	}

	if (SUCCEEDED(SHGetFolderPathW(NULL,
		CSIDL_DESKTOPDIRECTORY | CSIDL_FLAG_CREATE,
		NULL, SHGFP_TYPE_CURRENT, user_desktop))) {
		if (create_shell_shortcut(user_desktop, shortcut_name,
			executable, description)) {
			created = TRUE;
		}
	}
	if (SUCCEEDED(SHGetFolderPathW(NULL,
		CSIDL_COMMON_DESKTOPDIRECTORY | CSIDL_FLAG_CREATE,
		NULL, SHGFP_TYPE_CURRENT, desktop))) {
		if (create_shell_shortcut(desktop, shortcut_name,
			executable, description)) {
			created = TRUE;
		}
	}
	if (SUCCEEDED(SHGetFolderPathW(NULL,
		CSIDL_COMMON_PROGRAMS | CSIDL_FLAG_CREATE,
		NULL, SHGFP_TYPE_CURRENT, programs))) {
		if (create_shell_shortcut(programs, shortcut_name,
			executable, description)) {
			created = TRUE;
		}
	}

	if (SUCCEEDED(com_status)) {
		CoUninitialize();
	}
	return created;
}

BOOL UiRemoveShortcuts(const wchar_t *shortcut_name)
{
	wchar_t user_desktop[MAX_PATH];
	wchar_t desktop[MAX_PATH];
	wchar_t programs[MAX_PATH];
	HRESULT com_status;
	BOOL removed = TRUE;

	com_status = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	if (FAILED(com_status) && com_status != RPC_E_CHANGED_MODE) {
		return FALSE;
	}

	if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY,
		NULL, SHGFP_TYPE_CURRENT, user_desktop))) {
		if (!remove_shell_shortcut(user_desktop, shortcut_name)) {
			removed = FALSE;
		}
	}
	if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY,
		NULL, SHGFP_TYPE_CURRENT, desktop))) {
		if (!remove_shell_shortcut(desktop, shortcut_name)) {
			removed = FALSE;
		}
	}
	if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS,
		NULL, SHGFP_TYPE_CURRENT, programs))) {
		if (!remove_shell_shortcut(programs, shortcut_name)) {
			removed = FALSE;
		}
	}

	if (SUCCEEDED(com_status)) {
		CoUninitialize();
	}
	return removed;
}

BOOL UiTrayAdd(HWND window, HICON icon, const wchar_t *tooltip)
{
	NOTIFYICONDATAW data;

	if (window == NULL || icon == NULL) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	ZeroMemory(&data, sizeof(data));
	data.cbSize = sizeof(data);
	data.hWnd = window;
	data.uID = 1;
	data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	data.uCallbackMessage = UI_TRAY_CALLBACK_MESSAGE;
	data.hIcon = icon;
	if (tooltip != NULL) {
		wcsncpy_s(data.szTip, UI_ARRAY_COUNT(data.szTip),
			tooltip, _TRUNCATE);
	}
	else {
		data.szTip[0] = L'\0';
	}

	if (!Shell_NotifyIconW(NIM_ADD, &data)) {
		SetLastError(ERROR_GEN_FAILURE);
		return FALSE;
	}
	return TRUE;
}

void UiTrayRemove(HWND window)
{
	NOTIFYICONDATAW data;

	if (window == NULL) {
		return;
	}
	ZeroMemory(&data, sizeof(data));
	data.cbSize = sizeof(data);
	data.hWnd = window;
	data.uID = 1;
	Shell_NotifyIconW(NIM_DELETE, &data);
}

void UiTrayShowContextMenu(HWND window)
{
	HMENU menu;
	POINT cursor;
	UINT command;

	if (window == NULL) {
		return;
	}
	menu = CreatePopupMenu();
	if (menu == NULL) {
		return;
	}
	AppendMenuW(menu, MF_STRING, UI_TRAY_COMMAND_OPEN,
		L"打开主界面");
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING, UI_TRAY_COMMAND_EXIT, L"退出");

	if (!GetCursorPos(&cursor)) {
		DestroyMenu(menu);
		return;
	}
	SetForegroundWindow(window);
	command = TrackPopupMenu(menu,
		TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
		cursor.x, cursor.y, 0, window, NULL);
	DestroyMenu(menu);
	PostMessageW(window, WM_NULL, 0, 0);
	if (command != 0) {
		PostMessageW(window, WM_COMMAND,
			MAKEWPARAM(command, 0), 0);
	}
}
