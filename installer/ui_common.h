#ifndef USBRELAY_UI_COMMON_H
#define USBRELAY_UI_COMMON_H

#include <windows.h>

#include <stddef.h>

#define UI_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define UI_CLIENT_TASK_NAME L"USBRelay-Client-Autostart"
#define UI_SERVER_TASK_NAME L"USBRelay-Server-Autostart"
#define UI_TRAY_CALLBACK_MESSAGE (WM_APP + 100)
#define UI_TRAY_COMMAND_OPEN 40001
#define UI_TRAY_COMMAND_EXIT 40002

typedef struct UiCommandResult {
	DWORD exit_code;
	BOOL timed_out;
	wchar_t *output;
	size_t output_chars;
} UiCommandResult;

typedef struct UiProcess {
	PROCESS_INFORMATION process;
} UiProcess;

BOOL UiGetModuleDirectory(wchar_t *buffer, size_t buffer_count);
BOOL UiGetToolPath(const wchar_t *tool_name, wchar_t *buffer,
	size_t buffer_count);
BOOL UiGetSystemToolPath(const wchar_t *tool_name, wchar_t *buffer,
	size_t buffer_count);

BOOL UiRunCommandCapture(const wchar_t *executable,
	const wchar_t *arguments, const wchar_t *working_directory,
	DWORD timeout_ms, UiCommandResult *result);
void UiFreeCommandResult(UiCommandResult *result);

BOOL UiStartHiddenProcess(const wchar_t *executable,
	const wchar_t *arguments, const wchar_t *working_directory,
	UiProcess *process);
BOOL UiWaitForProcess(UiProcess *process, DWORD timeout_ms,
	DWORD *exit_code);
void UiCloseProcess(UiProcess *process, BOOL terminate);

HFONT UiCreateInterfaceFont(void);
void UiApplyFont(HWND window, HFONT font);
void UiCenterWindow(HWND window);
void UiAppendLog(HWND log_window, const wchar_t *format, ...);
void UiShowError(HWND owner, const wchar_t *message);
void UiShowLastError(HWND owner, const wchar_t *operation);

BOOL UiIsValidServerAddress(const wchar_t *address);
BOOL UiIsValidBusId(const wchar_t *busid);
void UiTrimWhitespace(wchar_t *text);

BOOL UiSetRegistryString(HKEY root, const wchar_t *subkey,
	const wchar_t *name, const wchar_t *value);
BOOL UiGetRegistryString(HKEY root, const wchar_t *subkey,
	const wchar_t *name, wchar_t *value, DWORD value_chars);
BOOL UiSetRegistryDword(HKEY root, const wchar_t *subkey,
	const wchar_t *name, DWORD value);
DWORD UiGetRegistryDword(HKEY root, const wchar_t *subkey,
	const wchar_t *name, DWORD default_value);

BOOL UiQueryService(const wchar_t *service_name, DWORD *state,
	DWORD *start_type, DWORD *win32_error);
BOOL UiSetServiceStartType(const wchar_t *service_name,
	DWORD start_type, DWORD *win32_error);
BOOL UiStartService(const wchar_t *service_name, DWORD *win32_error);
BOOL UiStopService(const wchar_t *service_name, DWORD *win32_error);

BOOL UiCreateLogonTask(const wchar_t *task_name,
	const wchar_t *executable, const wchar_t *arguments,
	const wchar_t *description, DWORD *win32_error);
BOOL UiDeleteLogonTask(const wchar_t *task_name, DWORD *win32_error);
BOOL UiLogonTaskExists(const wchar_t *task_name, DWORD *win32_error);

BOOL UiInstallShortcuts(const wchar_t *executable,
	const wchar_t *shortcut_name, const wchar_t *description);
BOOL UiRemoveShortcuts(const wchar_t *shortcut_name);

BOOL UiTrayAdd(HWND window, HICON icon, const wchar_t *tooltip);
void UiTrayRemove(HWND window);
void UiTrayShowContextMenu(HWND window);

#endif
