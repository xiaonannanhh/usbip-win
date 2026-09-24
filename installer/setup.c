#include <windows.h>

#include <stdio.h>
#include <wchar.h>

#include "resource.h"

#if defined(SETUP_X64)
#define SETUP_ARCH_TEXT L"x64"
#elif defined(SETUP_X86)
#define SETUP_ARCH_TEXT L"x86"
#else
#error Define SETUP_X86 or SETUP_X64 when compiling setup.c.
#endif

#if defined(SETUP_SERVER)
#define SETUP_PRODUCT L"USBRelay " SETUP_ARCH_TEXT L" Server Setup"
#define SETUP_SCRIPT L"install-server.cmd"
#define SETUP_UNINSTALL_SCRIPT L"uninstall-server.cmd"
#define SETUP_TEMP_PREFIX L"USBRelay-Server-"
#elif defined(SETUP_STANDARD_SERVER)
#define SETUP_PRODUCT \
	L"打印机内网共享服务端 " SETUP_ARCH_TEXT L" 安装程序"
#define SETUP_SCRIPT L"install-standard-server.cmd"
#define SETUP_UNINSTALL_SCRIPT L"uninstall-standard-server.cmd"
#define SETUP_TEMP_PREFIX L"USBRelay-Standard-Server-"
#elif defined(SETUP_PORT_TOOL)
#define SETUP_PRODUCT \
	L"打印机内网共享配置工具端 " SETUP_ARCH_TEXT L" 安装程序"
#define SETUP_SCRIPT L"install-port-tool.cmd"
#define SETUP_UNINSTALL_SCRIPT L"uninstall-port-tool.cmd"
#define SETUP_TEMP_PREFIX L"USBRelay-Standard-Port-Tool-"
#elif defined(SETUP_CLIENT)
#define SETUP_PRODUCT L"USBRelay " SETUP_ARCH_TEXT L" Client Setup"
#define SETUP_SCRIPT L"install-client.cmd"
#define SETUP_UNINSTALL_SCRIPT L"uninstall-client.cmd"
#define SETUP_TEMP_PREFIX L"USBRelay-Client-"
#else
#error Define SETUP_SERVER, SETUP_STANDARD_SERVER, SETUP_PORT_TOOL, or SETUP_CLIENT when compiling setup.c.
#endif

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void print_error(const wchar_t *message)
{
	fwprintf(stderr, L"[ERROR] %ls\n", message);
}

static BOOL join_path(wchar_t *buffer, size_t buffer_count,
	const wchar_t *directory, const wchar_t *name)
{
	int result = _snwprintf_s(buffer, buffer_count, _TRUNCATE,
		L"%ls\\%ls", directory, name);
	return result >= 0;
}

static BOOL get_system_executable(wchar_t *buffer, size_t buffer_count,
	const wchar_t *name)
{
	wchar_t system_directory[MAX_PATH];
	UINT length = GetSystemDirectoryW(system_directory,
		(DWORD)ARRAY_COUNT(system_directory));

	if (length == 0 || length >= ARRAY_COUNT(system_directory)) {
		return FALSE;
	}

	return join_path(buffer, buffer_count, system_directory, name);
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

static BOOL remove_directory_tree(const wchar_t *directory)
{
	wchar_t search_path[MAX_PATH];
	WIN32_FIND_DATAW find_data;
	HANDLE find_handle;
	BOOL result = TRUE;

	if (!join_path(search_path, ARRAY_COUNT(search_path), directory, L"*")) {
		return FALSE;
	}

	find_handle = FindFirstFileW(search_path, &find_data);
	if (find_handle == INVALID_HANDLE_VALUE) {
		DWORD error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
			return TRUE;
		}
		return FALSE;
	}

	do {
		wchar_t child[MAX_PATH];

		if (!wcscmp(find_data.cFileName, L".") ||
			!wcscmp(find_data.cFileName, L"..")) {
			continue;
		}
		if (!join_path(child, ARRAY_COUNT(child), directory,
			find_data.cFileName)) {
			result = FALSE;
			continue;
		}

		if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (!remove_directory_tree(child)) {
				result = FALSE;
			}
		} else {
			SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
			if (!DeleteFileW(child)) {
				result = FALSE;
			}
		}
	} while (FindNextFileW(find_handle, &find_data));

	if (GetLastError() != ERROR_NO_MORE_FILES) {
		result = FALSE;
	}
	FindClose(find_handle);

	if (!RemoveDirectoryW(directory)) {
		result = FALSE;
	}
	return result;
}

static BOOL extract_payload(const wchar_t *cab_path)
{
	HRSRC resource;
	HGLOBAL resource_data;
	const BYTE *data;
	DWORD data_size;
	DWORD written;
	DWORD remaining;
	HANDLE file;

	resource = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_PAYLOAD),
		RT_RCDATA);
	if (resource == NULL) {
		print_error(L"The embedded setup payload is missing.");
		return FALSE;
	}

	data_size = SizeofResource(NULL, resource);
	resource_data = LoadResource(NULL, resource);
	if (resource_data == NULL || data_size == 0) {
		print_error(L"Could not load the embedded setup payload.");
		return FALSE;
	}

	data = (const BYTE *)LockResource(resource_data);
	if (data == NULL) {
		print_error(L"Could not access the embedded setup payload.");
		return FALSE;
	}

	file = CreateFileW(cab_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		fwprintf(stderr, L"[ERROR] Could not create \"%ls\" (error %lu).\n",
			cab_path, GetLastError());
		return FALSE;
	}

	remaining = data_size;
	while (remaining > 0) {
		DWORD chunk = remaining;
		if (!WriteFile(file, data, chunk, &written, NULL) ||
			written == 0) {
			DWORD error = GetLastError();
			CloseHandle(file);
			fwprintf(stderr,
				L"[ERROR] Could not write the setup payload (error %lu).\n",
				error);
			return FALSE;
		}
		data += written;
		remaining -= written;
	}

	CloseHandle(file);
	return TRUE;
}

static BOOL run_command(wchar_t *command, const wchar_t *working_directory,
	DWORD *exit_code)
{
	STARTUPINFOW startup_info;
	PROCESS_INFORMATION process_info;
	DWORD wait_result;

	ZeroMemory(&startup_info, sizeof(startup_info));
	ZeroMemory(&process_info, sizeof(process_info));
	startup_info.cb = sizeof(startup_info);

	if (!CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL,
		working_directory, &startup_info, &process_info)) {
		fwprintf(stderr, L"[ERROR] Could not start command (error %lu).\n",
			GetLastError());
		return FALSE;
	}

	wait_result = WaitForSingleObject(process_info.hProcess, INFINITE);
	if (wait_result != WAIT_OBJECT_0 ||
		!GetExitCodeProcess(process_info.hProcess, exit_code)) {
		CloseHandle(process_info.hThread);
		CloseHandle(process_info.hProcess);
		print_error(L"Could not wait for the setup command.");
		return FALSE;
	}

	CloseHandle(process_info.hThread);
	CloseHandle(process_info.hProcess);
	return TRUE;
}

static BOOL expand_payload(const wchar_t *work_directory,
	const wchar_t *cab_path)
{
	wchar_t expand_path[MAX_PATH];
	wchar_t command[(MAX_PATH * 3) + 64];
	DWORD exit_code = 0;

	if (!get_system_executable(expand_path, ARRAY_COUNT(expand_path),
		L"expand.exe")) {
		print_error(L"Could not locate the Windows expand.exe utility.");
		return FALSE;
	}

	if (_snwprintf_s(command, ARRAY_COUNT(command), _TRUNCATE,
		L"\"%ls\" -F:* \"%ls\" \"%ls\"",
		expand_path, cab_path, work_directory) < 0) {
		print_error(L"The payload extraction command is too long.");
		return FALSE;
	}

	if (!run_command(command, NULL, &exit_code)) {
		return FALSE;
	}
	if (exit_code != 0) {
		fwprintf(stderr,
			L"[ERROR] expand.exe failed with exit code %lu.\n",
			exit_code);
		return FALSE;
	}
	return TRUE;
}

static BOOL run_payload_script(const wchar_t *work_directory,
	const wchar_t *script_name, DWORD *exit_code)
{
	wchar_t command_path[MAX_PATH];
	wchar_t command[(MAX_PATH * 2) + 64];

	if (!get_system_executable(command_path, ARRAY_COUNT(command_path),
		L"cmd.exe")) {
		print_error(L"Could not locate the Windows command processor.");
		return FALSE;
	}

	if (_snwprintf_s(command, ARRAY_COUNT(command), _TRUNCATE,
		L"\"%ls\" /d /c call \"%ls\"",
		command_path, script_name) < 0) {
		print_error(L"The setup script command is too long.");
		return FALSE;
	}

	return run_command(command, work_directory, exit_code);
}

static BOOL use_existing_or_create_directory(const wchar_t *path,
	wchar_t *full_path, size_t full_path_count)
{
	DWORD length = GetFullPathNameW(path, (DWORD)full_path_count,
		full_path, NULL);
	DWORD attributes;

	if (length == 0 || length >= full_path_count) {
		return FALSE;
	}

	attributes = GetFileAttributesW(full_path);
	if (attributes != INVALID_FILE_ATTRIBUTES) {
		return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	}
	return CreateDirectoryW(full_path, NULL) != 0;
}

static void print_usage(void)
{
	wprintf(L"%ls\n\n", SETUP_PRODUCT);
	wprintf(L"Usage:\n");
	wprintf(L"  setup.exe                 Install the package.\n");
	wprintf(L"  setup.exe /uninstall      Remove the installed device/service.\n");
	wprintf(L"  setup.exe /extract-only [directory]\n");
	wprintf(L"                            Extract files without installing.\n");
	wprintf(L"  setup.exe /help           Show this help.\n");
}

int wmain(int argc, wchar_t *argv[])
{
	const wchar_t *mode = L"install";
	const wchar_t *extract_directory = NULL;
	wchar_t work_directory[MAX_PATH];
	wchar_t cab_path[MAX_PATH];
	wchar_t original_directory[MAX_PATH];
	DWORD current_directory_length;
	BOOL temporary_directory = FALSE;
	DWORD exit_code = 0;
	int index;
	int result;

	current_directory_length = GetCurrentDirectoryW(
		(DWORD)ARRAY_COUNT(original_directory), original_directory);
	if (current_directory_length == 0 ||
		current_directory_length >= ARRAY_COUNT(original_directory)) {
		print_error(L"Could not read the current working directory.");
		return 1;
	}

	for (index = 1; index < argc; index++) {
		if (!_wcsicmp(argv[index], L"/?") ||
			!_wcsicmp(argv[index], L"-h") ||
			!_wcsicmp(argv[index], L"--help") ||
			!_wcsicmp(argv[index], L"/help")) {
			print_usage();
			return 0;
		}
		if (!_wcsicmp(argv[index], L"/uninstall") ||
			!_wcsicmp(argv[index], L"--uninstall")) {
			mode = L"uninstall";
			continue;
		}
		if (!_wcsicmp(argv[index], L"/extract-only") ||
			!_wcsicmp(argv[index], L"--extract-only")) {
			mode = L"extract";
			if (index + 1 < argc && argv[index + 1][0] != L'/') {
				extract_directory = argv[++index];
			}
			continue;
		}
		fwprintf(stderr, L"[ERROR] Unknown option: %ls\n\n", argv[index]);
		print_usage();
		return 2;
	}

	if (!wcscmp(mode, L"extract") && extract_directory != NULL) {
		if (!use_existing_or_create_directory(extract_directory,
			work_directory, ARRAY_COUNT(work_directory))) {
			print_error(L"Could not create the extraction directory.");
			return 1;
		}
	} else {
		if (!create_temp_directory(work_directory,
			ARRAY_COUNT(work_directory))) {
			print_error(L"Could not create a temporary setup directory.");
			return 1;
		}
		temporary_directory = wcscmp(mode, L"extract") != 0;
	}

	if (!join_path(cab_path, ARRAY_COUNT(cab_path), work_directory,
		L"setup-payload.cab")) {
		print_error(L"The temporary payload path is too long.");
		result = 1;
		goto cleanup;
	}

	if (!extract_payload(cab_path)) {
		result = 1;
		goto cleanup;
	}

	if (wcscmp(mode, L"extract") == 0) {
		if (!expand_payload(work_directory, cab_path)) {
			result = 1;
			goto cleanup;
		}
		if (!DeleteFileW(cab_path)) {
			fwprintf(stderr,
				L"[WARN] Could not remove the temporary CAB file.\n");
		}
		wprintf(L"Files extracted to:\n%ls\n", work_directory);
		result = 0;
		goto cleanup;
	}

	if (!expand_payload(work_directory, cab_path)) {
		result = 1;
		goto cleanup;
	}

	if (!DeleteFileW(cab_path)) {
		fwprintf(stderr,
			L"[WARN] Could not remove the temporary CAB file.\n");
	}

	if (wcscmp(mode, L"uninstall") == 0) {
		if (!run_payload_script(work_directory,
			SETUP_UNINSTALL_SCRIPT, &exit_code)) {
			result = 1;
			goto cleanup;
		}
	} else {
		if (!run_payload_script(work_directory,
			SETUP_SCRIPT, &exit_code)) {
			result = 1;
			goto cleanup;
		}
	}

	result = (int)exit_code;

cleanup:
	SetCurrentDirectoryW(original_directory);

	if (!temporary_directory) {
		return result;
	}

	if (!remove_directory_tree(work_directory)) {
		fwprintf(stderr,
			L"[WARN] Could not completely remove temporary files from:\n%ls\n",
			work_directory);
	}

	if (result != 0) {
		fwprintf(stderr, L"[ERROR] %ls failed with exit code %d.\n",
			SETUP_PRODUCT, result);
	} else {
		wprintf(L"%ls completed successfully.\n", SETUP_PRODUCT);
	}

	return result;
}
