#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <wchar.h>

#include "universal_resource.h"

#if !defined(UNIVERSAL_SERVER) && !defined(UNIVERSAL_STANDARD_SERVER) && \
	!defined(UNIVERSAL_PORT_TOOL) && !defined(UNIVERSAL_CLIENT)
#error Define UNIVERSAL_SERVER, UNIVERSAL_STANDARD_SERVER, UNIVERSAL_PORT_TOOL, or UNIVERSAL_CLIENT when compiling universal_launcher.c.
#endif

#if defined(UNIVERSAL_SERVER)
#define SETUP_PRODUCT L"USBRelay Windows 7 x86/x64 Server Setup"
#define SETUP_TEMP_PREFIX L"USBRelay-Server-Universal-"
#define SETUP_X86_NAME L"USBRelay-Server-Setup-x86.exe"
#define SETUP_X64_NAME L"USBRelay-Server-Setup-x64.exe"
#elif defined(UNIVERSAL_STANDARD_SERVER)
#define SETUP_PRODUCT L"打印机内网共享服务端混合安装包"
#define SETUP_TEMP_PREFIX L"USBRelay-Standard-Server-Universal-"
#define SETUP_X86_NAME L"USBRelay-Standard-Server-Setup-x86.exe"
#define SETUP_X64_NAME L"USBRelay-Standard-Server-Setup-x64.exe"
#elif defined(UNIVERSAL_PORT_TOOL)
#define SETUP_PRODUCT L"打印机内网共享配置工具端混合安装包"
#define SETUP_TEMP_PREFIX L"USBRelay-Standard-Port-Tool-Universal-"
#define SETUP_X86_NAME L"USBRelay-Standard-Port-Tool-Setup-x86.exe"
#define SETUP_X64_NAME L"USBRelay-Standard-Port-Tool-Setup-x64.exe"
#elif defined(UNIVERSAL_CLIENT)
#define SETUP_PRODUCT L"USBRelay Windows 7 x86/x64 Client Setup"
#define SETUP_TEMP_PREFIX L"USBRelay-Client-Universal-"
#define SETUP_X86_NAME L"USBRelay-Client-Setup-x86.exe"
#define SETUP_X64_NAME L"USBRelay-Client-Setup-x64.exe"
#endif

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define COMMAND_LINE_CAPACITY 32768

typedef enum target_architecture {
	TARGET_ARCH_AUTO = 0,
	TARGET_ARCH_X86,
	TARGET_ARCH_X64
} target_architecture;

static void print_error(const wchar_t *message)
{
#if defined(UNIVERSAL_PORT_TOOL)
	MessageBoxW(NULL, message, SETUP_PRODUCT,
		MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#else
	fwprintf(stderr, L"[ERROR] %ls\n", message);
#endif
}

static void print_system_error(const wchar_t *action, const wchar_t *path,
	DWORD error)
{
	wchar_t message[MAX_PATH + 256];

	_snwprintf_s(message, ARRAY_COUNT(message), _TRUNCATE,
		L"%ls\n%ls\nWindows 错误代码：%lu", action, path, error);
	print_error(message);
}

static BOOL join_path(wchar_t *buffer, size_t buffer_count,
	const wchar_t *directory, const wchar_t *name)
{
	int result = _snwprintf_s(buffer, buffer_count, _TRUNCATE,
		L"%ls\\%ls", directory, name);
	return result >= 0;
}

static BOOL create_temp_directory(wchar_t *buffer, size_t buffer_count)
{
	wchar_t temp_root[MAX_PATH];
	DWORD length = GetTempPathW((DWORD)ARRAY_COUNT(temp_root), temp_root);
	DWORD process_id = GetCurrentProcessId();
	unsigned int attempt;

	if (length == 0 || length >= ARRAY_COUNT(temp_root)) {
		return FALSE;
	}

	for (attempt = 0; attempt < 1000; attempt++) {
		int result = _snwprintf_s(buffer, buffer_count, _TRUNCATE,
			L"%ls%ls%lu-%u", temp_root, SETUP_TEMP_PREFIX,
			process_id, attempt);

		if (result < 0) {
			return FALSE;
		}
		if (CreateDirectoryW(buffer, NULL)) {
			return TRUE;
		}
		if (GetLastError() != ERROR_ALREADY_EXISTS) {
			return FALSE;
		}
	}

	SetLastError(ERROR_ALREADY_EXISTS);
	return FALSE;
}

static BOOL extract_resource_to_file(int resource_id, const wchar_t *path)
{
	HRSRC resource;
	HGLOBAL resource_data;
	const BYTE *data;
	DWORD data_size;
	DWORD remaining;
	HANDLE file;

	resource = FindResourceW(NULL, MAKEINTRESOURCEW(resource_id),
		RT_RCDATA);
	if (resource == NULL) {
		print_error(L"The embedded architecture-specific installer is missing.");
		return FALSE;
	}

	data_size = SizeofResource(NULL, resource);
	resource_data = LoadResource(NULL, resource);
	if (resource_data == NULL || data_size == 0) {
		print_error(L"Could not load the embedded installer.");
		return FALSE;
	}

	data = (const BYTE *)LockResource(resource_data);
	if (data == NULL) {
		print_error(L"Could not access the embedded installer.");
		return FALSE;
	}

	file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		print_system_error(L"无法创建临时安装程序：", path,
			GetLastError());
		return FALSE;
	}

	remaining = data_size;
	while (remaining > 0) {
		DWORD written = 0;

		if (!WriteFile(file, data, remaining, &written, NULL) ||
			written == 0) {
			DWORD error = GetLastError();
			CloseHandle(file);
			print_system_error(L"无法写入临时安装程序：", path, error);
			return FALSE;
		}

		data += written;
		remaining -= written;
	}

	CloseHandle(file);
	return TRUE;
}

static BOOL append_text(wchar_t *buffer, size_t buffer_count,
	size_t *length, const wchar_t *text)
{
	size_t text_length = wcslen(text);

	if (*length + text_length >= buffer_count) {
		return FALSE;
	}

	wmemcpy(buffer + *length, text, text_length);
	*length += text_length;
	buffer[*length] = L'\0';
	return TRUE;
}

static BOOL append_character(wchar_t *buffer, size_t buffer_count,
	size_t *length, wchar_t character)
{
	if (*length + 1 >= buffer_count) {
		return FALSE;
	}

	buffer[*length] = character;
	(*length)++;
	buffer[*length] = L'\0';
	return TRUE;
}

static BOOL append_quoted_argument(wchar_t *buffer, size_t buffer_count,
	size_t *length, const wchar_t *argument)
{
	const wchar_t *cursor;
	size_t backslash_count = 0;

	if (*length != 0 &&
		!append_character(buffer, buffer_count, length, L' ')) {
		return FALSE;
	}
	if (!append_character(buffer, buffer_count, length, L'"')) {
		return FALSE;
	}

	for (cursor = argument; *cursor != L'\0'; cursor++) {
		if (*cursor == L'\\') {
			backslash_count++;
			continue;
		}
		if (*cursor == L'"') {
			while (backslash_count > 0) {
				if (!append_character(buffer, buffer_count, length,
					L'\\') ||
					!append_character(buffer, buffer_count, length,
						L'\\')) {
					return FALSE;
				}
				backslash_count--;
			}
			if (!append_character(buffer, buffer_count, length, L'\\') ||
				!append_character(buffer, buffer_count, length, L'"')) {
				return FALSE;
			}
			continue;
		}
		while (backslash_count > 0) {
			if (!append_character(buffer, buffer_count, length, L'\\')) {
				return FALSE;
			}
			backslash_count--;
		}
		if (!append_character(buffer, buffer_count, length, *cursor)) {
			return FALSE;
		}
	}

	while (backslash_count > 0) {
		if (!append_character(buffer, buffer_count, length, L'\\') ||
			!append_character(buffer, buffer_count, length, L'\\')) {
			return FALSE;
		}
		backslash_count--;
	}

	return append_character(buffer, buffer_count, length, L'"');
}

static BOOL parse_architecture(const wchar_t *value,
	target_architecture *architecture)
{
	if (!_wcsicmp(value, L"auto")) {
		*architecture = TARGET_ARCH_AUTO;
		return TRUE;
	}
	if (!_wcsicmp(value, L"x86")) {
		*architecture = TARGET_ARCH_X86;
		return TRUE;
	}
	if (!_wcsicmp(value, L"x64")) {
		*architecture = TARGET_ARCH_X64;
		return TRUE;
	}
	return FALSE;
}

static BOOL native_windows_is_x64(void)
{
	SYSTEM_INFO system_info;

	ZeroMemory(&system_info, sizeof(system_info));
	GetNativeSystemInfo(&system_info);
	return system_info.wProcessorArchitecture ==
		PROCESSOR_ARCHITECTURE_AMD64;
}

static void print_usage(void)
{
#if defined(UNIVERSAL_PORT_TOOL)
	MessageBoxW(NULL,
		L"混合安装包将检测 Windows 架构并自动启动对应安装程序。\n"
		L"安装参数会传递给对应的安装程序。",
		SETUP_PRODUCT, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
#else
	wprintf(L"%ls\n\n", SETUP_PRODUCT);
	wprintf(L"Usage:\n");
	wprintf(L"  setup-Universal.exe [options] [/arch auto|x86|x64]\n\n");
	wprintf(L"Without /arch, the launcher detects the Windows architecture.\n");
	wprintf(L"All other options are passed to the selected installer.\n");
#endif
}

static int run_launcher(int argc, wchar_t *argv[])
{
	target_architecture requested_architecture = TARGET_ARCH_AUTO;
	target_architecture selected_architecture;
	wchar_t work_directory[MAX_PATH];
	wchar_t installer_path[MAX_PATH];
	wchar_t command_line[COMMAND_LINE_CAPACITY];
	const wchar_t *installer_name;
	int resource_id;
	STARTUPINFOW startup_info;
	PROCESS_INFORMATION process_info;
	DWORD child_exit_code = 0;
	size_t command_length = 0;
	BOOL native_x64;
	int index;
	int result = 1;

	ZeroMemory(work_directory, sizeof(work_directory));
	ZeroMemory(installer_path, sizeof(installer_path));

	for (index = 1; index < argc; index++) {
		if (!_wcsicmp(argv[index], L"/arch") ||
			!_wcsicmp(argv[index], L"--arch")) {
			if (index + 1 >= argc ||
				!parse_architecture(argv[++index],
					&requested_architecture)) {
				print_error(L"/arch must be followed by auto, x86, or x64.");
				return 2;
			}
			continue;
		}
		if (!_wcsnicmp(argv[index], L"/arch=", 6) ||
			!_wcsnicmp(argv[index], L"--arch=", 7)) {
			const wchar_t *value = wcschr(argv[index], L'=');
			if (value == NULL ||
				!parse_architecture(value + 1,
					&requested_architecture)) {
				print_error(L"/arch must use auto, x86, or x64.");
				return 2;
			}
			continue;
		}
		if (!_wcsicmp(argv[index], L"/?") ||
			!_wcsicmp(argv[index], L"-h") ||
			!_wcsicmp(argv[index], L"--help") ||
			!_wcsicmp(argv[index], L"/help")) {
			print_usage();
			return 0;
		}
	}

	native_x64 = native_windows_is_x64();
	if (requested_architecture == TARGET_ARCH_AUTO) {
		selected_architecture = native_x64 ?
			TARGET_ARCH_X64 : TARGET_ARCH_X86;
	} else {
		selected_architecture = requested_architecture;
	}

	if (selected_architecture == TARGET_ARCH_X64 && !native_x64) {
		print_error(L"The x64 installer cannot run on 32-bit Windows.");
		return 1;
	}

	if (selected_architecture == TARGET_ARCH_X64) {
		installer_name = SETUP_X64_NAME;
		resource_id = IDR_SETUP_X64;
	} else {
		installer_name = SETUP_X86_NAME;
		resource_id = IDR_SETUP_X86;
	}

#if !defined(UNIVERSAL_PORT_TOOL)
	wprintf(L"Detected Windows architecture: %ls\n",
		native_x64 ? L"x64" : L"x86");
	wprintf(L"Selected installer: %ls\n", installer_name);
#endif

	if (!create_temp_directory(work_directory,
		ARRAY_COUNT(work_directory))) {
		print_error(L"Could not create a temporary launcher directory.");
		return 1;
	}

	if (!join_path(installer_path, ARRAY_COUNT(installer_path),
		work_directory, installer_name)) {
		print_error(L"The temporary installer path is too long.");
		goto cleanup;
	}

	if (!extract_resource_to_file(resource_id, installer_path)) {
		goto cleanup;
	}

	if (!append_quoted_argument(command_line,
		ARRAY_COUNT(command_line), &command_length, installer_path)) {
		print_error(L"The installer command line is too long.");
		goto cleanup;
	}

	for (index = 1; index < argc; index++) {
		if (!_wcsicmp(argv[index], L"/arch") ||
			!_wcsicmp(argv[index], L"--arch")) {
			index++;
			continue;
		}
		if (!_wcsnicmp(argv[index], L"/arch=", 6) ||
			!_wcsnicmp(argv[index], L"--arch=", 7)) {
			continue;
		}
		if (!append_quoted_argument(command_line,
			ARRAY_COUNT(command_line), &command_length, argv[index])) {
			print_error(L"The installer command line is too long.");
			goto cleanup;
		}
	}

	ZeroMemory(&startup_info, sizeof(startup_info));
	ZeroMemory(&process_info, sizeof(process_info));
	startup_info.cb = sizeof(startup_info);

	if (!CreateProcessW(installer_path, command_line, NULL, NULL, TRUE,
#if defined(UNIVERSAL_PORT_TOOL)
		CREATE_NEW_CONSOLE,
#else
		0,
#endif
		NULL, work_directory, &startup_info, &process_info)) {
		print_system_error(L"无法启动对应架构的安装程序：",
			installer_path, GetLastError());
		goto cleanup;
	}

	if (WaitForSingleObject(process_info.hProcess, INFINITE) !=
		WAIT_OBJECT_0 ||
		!GetExitCodeProcess(process_info.hProcess, &child_exit_code)) {
		print_error(L"Could not wait for the architecture-specific installer.");
		CloseHandle(process_info.hThread);
		CloseHandle(process_info.hProcess);
		goto cleanup;
	}

	CloseHandle(process_info.hThread);
	CloseHandle(process_info.hProcess);
	result = (int)child_exit_code;

cleanup:
	if (installer_path[0] != L'\0') {
		SetFileAttributesW(installer_path, FILE_ATTRIBUTE_NORMAL);
		DeleteFileW(installer_path);
	}
	if (work_directory[0] != L'\0') {
		RemoveDirectoryW(work_directory);
	}
	return result;
}

#if defined(UNIVERSAL_PORT_TOOL)
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous_instance,
	PWSTR command_line, int show_command)
{
	wchar_t **arguments;
	int argument_count;
	int result;

	UNREFERENCED_PARAMETER(instance);
	UNREFERENCED_PARAMETER(previous_instance);
	UNREFERENCED_PARAMETER(command_line);
	UNREFERENCED_PARAMETER(show_command);
	arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
	if (arguments == NULL) {
		print_error(L"无法读取安装程序命令行参数。");
		return 1;
	}
	result = run_launcher(argument_count, arguments);
	LocalFree(arguments);
	return result;
}
#else
int wmain(int argc, wchar_t *argv[])
{
	return run_launcher(argc, argv);
}
#endif
