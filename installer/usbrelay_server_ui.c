#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winspool.h>

#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#include "../include/usbrelay_protocol.h"
#include "ui_common.h"
#include "ui_resource.h"

#define SERVER_WINDOW_CLASS L"USBRelay-Server-UI"
#define SERVER_WINDOW_TITLE L"USBRelay 服务端"
#define SERVER_TIMER_STATUS 1
#define SERVER_CORE_MUTEX_NAME L"Local\\USBRelay-Server-Core"
#define SERVER_STOP_EVENT_NAME L"Local\\USBRelay-Server-Stop"

#define IDC_STATUS 1001
#define IDC_REFRESH 1002
#define IDC_START 1003
#define IDC_STOP 1004
#define IDC_MANAGE 1005
#define IDC_AUTOSTART 1006
#define IDC_PRINTERS 1007
#define IDC_LOG 1008

typedef struct ServerState {
	HWND window;
	HWND status;
	HWND printers;
	HWND log;
	HWND refresh;
	HWND start;
	HWND stop;
	HWND manage;
	HWND autostart;
	HFONT font;
	HICON tray_icon;
	BOOL tray_added;
	BOOL exiting;
	BOOL autostart_mode;
	BOOL startup_task;
	UiProcess process;
	HANDLE stop_event;
} ServerState;

int usbrelay_engine_run(void);

static ServerState *get_state(HWND window)
{
	return (ServerState *)GetWindowLongPtrW(window, GWLP_USERDATA);
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

static void show_window(HWND window)
{
	ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
	SetForegroundWindow(window);
}

static HWND make_control(ServerState *state, const wchar_t *class_name,
	const wchar_t *text, DWORD style, DWORD ex_style, int x, int y,
	int width, int height, int id)
{
	HWND control = CreateWindowExW(ex_style, class_name, text,
		WS_CHILD | WS_VISIBLE | style, x, y, width, height, state->window,
		(HMENU)(INT_PTR)id, NULL, NULL);
	if (control != NULL)
		SendMessageW(control, WM_SETFONT, (WPARAM)state->font, TRUE);
	return control;
}

static void log_text(ServerState *state, const wchar_t *format, ...)
{
	wchar_t message[2048];
	wchar_t line[2100];
	va_list args;
	int length;

	va_start(args, format);
	_vsnwprintf_s(message, UI_ARRAY_COUNT(message), _TRUNCATE,
		format, args);
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

static void set_status(ServerState *state, const wchar_t *text)
{
	SetWindowTextW(state->status, text);
}

static DWORD printer_id(const wchar_t *name)
{
	DWORD hash = 2166136261u;
	while (name != NULL && *name != L'\0') {
		wchar_t c = *name++;
		if (c >= L'A' && c <= L'Z')
			c = (wchar_t)(c - L'A' + L'a');
		hash ^= (DWORD)(c & 0xff);
		hash *= 16777619u;
		hash ^= (DWORD)((c >> 8) & 0xff);
		hash *= 16777619u;
	}
	return hash;
}

static BOOL enum_printers(BYTE **buffer, DWORD *count)
{
	DWORD needed = 0;
	DWORD returned = 0;
	BYTE *data;

	*buffer = NULL;
	*count = 0;
	SetLastError(ERROR_SUCCESS);
	if (EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, NULL, 0, &needed,
		&returned))
		return TRUE;
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return FALSE;
	if (needed == 0)
		return TRUE;
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

static void refresh_printers(ServerState *state)
{
	BYTE *buffer = NULL;
	DWORD count = 0;
	DWORD index;
	LVCOLUMNW column;

	if (!enum_printers(&buffer, &count)) {
		log_text(state, L"读取本机打印机失败，错误 %lu。", GetLastError());
		return;
	}
	ListView_DeleteAllItems(state->printers);
	for (index = 0; index < count; index++) {
		PRINTER_INFO_2W *info = &((PRINTER_INFO_2W *)buffer)[index];
		LVITEMW item;
		wchar_t id[32];

		if (info->pPrinterName == NULL || info->pPrinterName[0] == L'\0')
			continue;
		_snwprintf_s(id, UI_ARRAY_COUNT(id), _TRUNCATE, L"%08lX",
			(unsigned long)printer_id(info->pPrinterName));
		ZeroMemory(&item, sizeof(item));
		item.mask = LVIF_TEXT;
		item.iItem = ListView_GetItemCount(state->printers);
		item.pszText = info->pPrinterName;
		ListView_InsertItem(state->printers, &item);
		ListView_SetItemText(state->printers, item.iItem, 1,
			info->pDriverName != NULL ? info->pDriverName : L"-");
		ListView_SetItemText(state->printers, item.iItem, 2,
			info->pPortName != NULL ? info->pPortName : L"-");
		ListView_SetItemText(state->printers, item.iItem, 3, id);
		ListView_SetItemText(state->printers, item.iItem, 4,
			L"USBRelay 发布；本机队列不变");
	}
	if (buffer != NULL)
		HeapFree(GetProcessHeap(), 0, buffer);
	log_text(state, L"已刷新本机打印机，共 %d 个。",
		ListView_GetItemCount(state->printers));
	UNREFERENCED_PARAMETER(column);
}

static BOOL server_core_exists(void)
{
	HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE,
		SERVER_CORE_MUTEX_NAME);

	if (mutex == NULL)
		return FALSE;
	CloseHandle(mutex);
	return TRUE;
}

static BOOL child_process_running(ServerState *state)
{
	DWORD exit_code;

	if (state->process.process.hProcess == NULL)
		return FALSE;
	if (WaitForSingleObject(state->process.process.hProcess, 0) ==
		WAIT_TIMEOUT)
		return TRUE;
	GetExitCodeProcess(state->process.process.hProcess, &exit_code);
	UiCloseProcess(&state->process, FALSE);
	return FALSE;
}

static void refresh_service(ServerState *state)
{
	BOOL running = child_process_running(state) || server_core_exists();

	if (running) {
		set_status(state, L"打印共享运行中，正在发布本机打印机");
		EnableWindow(state->start, FALSE);
		EnableWindow(state->stop, TRUE);
	}
	else {
		set_status(state, L"打印共享未运行");
		EnableWindow(state->start, TRUE);
		EnableWindow(state->stop, FALSE);
	}
}

static BOOL ensure_stop_event(ServerState *state)
{
	if (state->stop_event != NULL)
		return TRUE;
	state->stop_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE,
		SERVER_STOP_EVENT_NAME);
	if (state->stop_event == NULL) {
		state->stop_event = CreateEventW(NULL, TRUE, FALSE,
			SERVER_STOP_EVENT_NAME);
	}
	return state->stop_event != NULL;
}

static void start_server(ServerState *state)
{
	wchar_t executable[MAX_PATH];

	if (child_process_running(state) || server_core_exists()) {
		log_text(state, L"打印共享已经在运行。");
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
		log_text(state, L"启动后台打印共享失败，错误 %lu。",
			GetLastError());
		return;
	}
	Sleep(300);
	if (!child_process_running(state) && !server_core_exists()) {
		log_text(state, L"后台打印共享启动后立即退出。");
		return;
	}
	log_text(state, L"打印共享已启动。");
	refresh_service(state);
}

static void shutdown_server(ServerState *state)
{
	DWORD wait_result = WAIT_OBJECT_0;

	if (!child_process_running(state) && !server_core_exists())
		return;
	if (!ensure_stop_event(state))
		return;
	SetEvent(state->stop_event);
	if (state->process.process.hProcess != NULL) {
		wait_result = WaitForSingleObject(state->process.process.hProcess,
			5000);
		UiCloseProcess(&state->process, wait_result == WAIT_TIMEOUT);
	}
	else {
		DWORD elapsed;
		for (elapsed = 0; elapsed < 5000; elapsed += 100) {
			if (!server_core_exists())
				break;
			Sleep(100);
		}
	}
	if (state->stop_event != NULL) {
		CloseHandle(state->stop_event);
		state->stop_event = NULL;
	}
}

static void stop_server(ServerState *state)
{
	BOOL was_running = child_process_running(state) || server_core_exists();

	if (!was_running) {
		log_text(state, L"打印共享当前未运行。");
		refresh_service(state);
		return;
	}
	shutdown_server(state);
	if (server_core_exists())
		log_text(state, L"已请求停止，但后台程序仍在退出。");
	else
		log_text(state, L"打印共享已停止。");
	refresh_service(state);
}

static void toggle_startup(ServerState *state)
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
		ok = UiCreateLogonTask(UI_SERVER_TASK_NAME,
			executable, L"/autostart", L"USBRelay 服务端界面开机启动",
			&error);
	}
	else {
		ok = UiDeleteLogonTask(UI_SERVER_TASK_NAME, &error);
	}
	if (!ok) {
		SendMessageW(state->autostart, BM_SETCHECK,
			checked == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
		log_text(state, L"更新开机启动失败，错误 %lu。", error);
		return;
	}
	log_text(state, checked == BST_CHECKED ?
		L"已启用服务端界面开机启动。" : L"已关闭服务端界面开机启动。");
}

static void open_printer_management(ServerState *state)
{
	wchar_t path[MAX_PATH];
	UiProcess process;
	if (!UiGetSystemToolPath(L"control.exe", path, UI_ARRAY_COUNT(path))) {
		log_text(state, L"找不到打印机管理工具。");
		return;
	}
	ZeroMemory(&process, sizeof(process));
	if (!UiStartHiddenProcess(path, L"printers", NULL, &process)) {
		ShellExecuteW(state->window, L"open", L"control.exe", L"printers",
			NULL, SW_SHOWNORMAL);
	}
	else {
		UiCloseProcess(&process, FALSE);
	}
}

static void layout(ServerState *state)
{
	RECT rect;
	int width;
	int height;
	int list_height;

	GetClientRect(state->window, &rect);
	width = rect.right - rect.left;
	height = rect.bottom - rect.top;
	if (width < 760) width = 760;
	if (height < 500) height = 500;
	MoveWindow(state->status, 14, 12, width - 28, 24, TRUE);
	MoveWindow(state->refresh, width - 374, 42, 112, 28, TRUE);
	MoveWindow(state->start, width - 248, 42, 112, 28, TRUE);
	MoveWindow(state->stop, width - 122, 42, 108, 28, TRUE);
	MoveWindow(state->manage, 14, 42, 170, 28, TRUE);
	MoveWindow(state->autostart, 198, 44, 230, 24, TRUE);
	list_height = height - 176;
	if (list_height < 170) list_height = 170;
	MoveWindow(state->printers, 14, 82, width - 28, list_height, TRUE);
	MoveWindow(state->log, 14, 94 + list_height, width - 28,
		height - list_height - 108, TRUE);
}

static BOOL create_controls(ServerState *state)
{
	LVCOLUMNW column;
	state->status = make_control(state, L"STATIC", L"读取服务状态...",
		SS_LEFT, 0, 14, 12, 740, 24, IDC_STATUS);
	state->manage = make_control(state, L"BUTTON", L"打开打印机管理",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 14, 42, 170, 28, IDC_MANAGE);
	state->autostart = make_control(state, L"BUTTON", L"界面开机启动并驻留托盘",
		BS_AUTOCHECKBOX | WS_TABSTOP, 0, 198, 44, 230, 24, IDC_AUTOSTART);
	state->refresh = make_control(state, L"BUTTON", L"刷新打印机",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 520, 42, 112, 28, IDC_REFRESH);
	state->start = make_control(state, L"BUTTON", L"启动打印共享",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 646, 42, 112, 28, IDC_START);
	state->stop = make_control(state, L"BUTTON", L"停止打印共享",
		BS_PUSHBUTTON | WS_TABSTOP, 0, 772, 42, 108, 28, IDC_STOP);
	state->printers = make_control(state, WC_LISTVIEWW, L"",
		LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
		WS_EX_CLIENTEDGE, 14, 82, 850, 320, IDC_PRINTERS);
	state->log = make_control(state, L"EDIT", L"",
		ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
		WS_EX_CLIENTEDGE, 14, 416, 850, 100, IDC_LOG);
	if (state->status == NULL || state->printers == NULL || state->log == NULL ||
		state->refresh == NULL || state->start == NULL || state->stop == NULL ||
		state->manage == NULL || state->autostart == NULL)
		return FALSE;
	ListView_SetExtendedListViewStyle(state->printers,
		LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
	ZeroMemory(&column, sizeof(column));
	column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
	column.cx = 220; column.pszText = L"打印机名称";
	ListView_InsertColumn(state->printers, 0, &column);
	column.cx = 190; column.pszText = L"驱动";
	ListView_InsertColumn(state->printers, 1, &column);
	column.cx = 130; column.pszText = L"原端口";
	ListView_InsertColumn(state->printers, 2, &column);
	column.cx = 100; column.pszText = L"发布 ID";
	ListView_InsertColumn(state->printers, 3, &column);
	column.cx = 260; column.pszText = L"状态";
	ListView_InsertColumn(state->printers, 4, &column);
	return TRUE;
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
	if (!GetModuleFileNameW(instance, path, UI_ARRAY_COUNT(path))) return 1;
	if (install_autostart)
		return UiCreateLogonTask(UI_SERVER_TASK_NAME, path, L"/autostart",
			L"USBRelay 服务端界面开机启动并驻留托盘", &error) ? 0 : 1;
	if (remove_autostart)
		return UiDeleteLogonTask(UI_SERVER_TASK_NAME, &error) ? 0 : 1;
	return install ?
		(UiInstallShortcuts(path, L"USBRelay 服务端", L"USBRelay 局域网打印服务端") ? 0 : 1) :
		(UiRemoveShortcuts(L"USBRelay 服务端") ? 0 : 1);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message,
	WPARAM w_param, LPARAM l_param)
{
	ServerState *state;
	if (message == WM_NCCREATE) {
		CREATESTRUCTW *create = (CREATESTRUCTW *)l_param;
		state = (ServerState *)create->lpCreateParams;
		state->window = window;
		SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)state);
		return TRUE;
	}
	state = get_state(window);
	switch (message) {
	case WM_CREATE:
		if (!create_controls(state)) return -1;
		layout(state);
		start_server(state);
		refresh_printers(state);
		if (UiLogonTaskExists(UI_SERVER_TASK_NAME, NULL))
			SendMessageW(state->autostart, BM_SETCHECK, BST_CHECKED, 0);
		SetTimer(window, SERVER_TIMER_STATUS, 2000, NULL);
		return 0;
	case WM_SIZE:
		layout(state);
		return 0;
	case WM_GETMINMAXINFO:
		((MINMAXINFO *)l_param)->ptMinTrackSize.x = 760;
		((MINMAXINFO *)l_param)->ptMinTrackSize.y = 500;
		return 0;
	case WM_COMMAND:
		switch (LOWORD(w_param)) {
		case IDC_REFRESH: if (HIWORD(w_param) == BN_CLICKED) refresh_printers(state); return 0;
		case IDC_START: if (HIWORD(w_param) == BN_CLICKED) start_server(state); return 0;
		case IDC_STOP: if (HIWORD(w_param) == BN_CLICKED) stop_server(state); return 0;
		case IDC_MANAGE: if (HIWORD(w_param) == BN_CLICKED) open_printer_management(state); return 0;
		case IDC_AUTOSTART: if (HIWORD(w_param) == BN_CLICKED) toggle_startup(state); return 0;
		case UI_TRAY_COMMAND_OPEN: show_window(window); return 0;
		case UI_TRAY_COMMAND_EXIT: state->exiting = TRUE; DestroyWindow(window); return 0;
		default: break;
		}
		break;
	case WM_TIMER:
		if (w_param == SERVER_TIMER_STATUS) refresh_service(state);
		return 0;
	case UI_TRAY_CALLBACK_MESSAGE:
		if (LOWORD(l_param) == WM_LBUTTONDBLCLK) show_window(window);
		else if (LOWORD(l_param) == WM_RBUTTONUP || LOWORD(l_param) == WM_CONTEXTMENU)
			UiTrayShowContextMenu(window);
		return 0;
	case WM_CLOSE:
		if (!state->exiting) { ShowWindow(window, SW_HIDE); return 0; }
		DestroyWindow(window); return 0;
	case WM_QUERYENDSESSION:
		shutdown_server(state);
		return TRUE;
	case WM_DESTROY:
		shutdown_server(state);
		KillTimer(window, SERVER_TIMER_STATUS);
		UiTrayRemove(window);
		PostQuitMessage(0);
		return 0;
	default: break;
	}
	return DefWindowProcW(window, message, w_param, l_param);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, wchar_t *command_line,
	int show_command)
{
	WNDCLASSEXW wc;
	INITCOMMONCONTROLSEX controls;
	ServerState state;
	MSG message;
	HANDLE mutex;
	HWND existing;
	int shortcut;

	UNREFERENCED_PARAMETER(previous);
	UNREFERENCED_PARAMETER(command_line);
	if (has_argument(L"--engine")) return usbrelay_engine_run();
	shortcut = shortcut_command(instance);
	if (shortcut >= 0) return shortcut;
	mutex = CreateMutexW(NULL, TRUE, L"Local\\USBRelay-Server-UI");
	if (mutex == NULL) return 1;
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		existing = FindWindowW(SERVER_WINDOW_CLASS, NULL);
		if (existing != NULL) show_window(existing);
		CloseHandle(mutex);
		return 0;
	}
	controls.dwSize = sizeof(controls);
	controls.dwICC = ICC_LISTVIEW_CLASSES;
	if (!InitCommonControlsEx(&controls)) { CloseHandle(mutex); return 1; }
	ZeroMemory(&state, sizeof(state));
	state.font = UiCreateInterfaceFont();
	if (state.font == NULL) { CloseHandle(mutex); return 1; }
	ZeroMemory(&wc, sizeof(wc));
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = window_proc;
	wc.hInstance = instance;
	wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
	wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON),
		IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
		LR_DEFAULTCOLOR);
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = SERVER_WINDOW_CLASS;
	if (!RegisterClassExW(&wc)) { DeleteObject(state.font); CloseHandle(mutex); return 1; }
	state.window = CreateWindowExW(0, SERVER_WINDOW_CLASS, SERVER_WINDOW_TITLE,
		WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
		920, 620, NULL, NULL, instance, &state);
	if (state.window == NULL) { DeleteObject(state.font); CloseHandle(mutex); return 1; }
	UiCenterWindow(state.window);
	state.tray_icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
	state.tray_added = state.tray_icon != NULL && UiTrayAdd(state.window,
		state.tray_icon, L"USBRelay 服务端 - 双击打开");
	if (state.autostart_mode || has_argument(L"/autostart")) {
		ShowWindow(state.window, SW_HIDE);
	} else {
		ShowWindow(state.window, show_command == SW_HIDE ? SW_SHOWNORMAL : show_command);
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
