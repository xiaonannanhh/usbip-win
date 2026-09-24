
#include "usbip.h"
#include "usbip_common.h"

#include <windows.h>
#include <setupapi.h>
#include <newdev.h>

#include <shlwapi.h>

#include <string.h>
#include <assert.h>
#include <stdio.h>

#define BUFFER_SIZE 256
#define STR_BUFFER_SIZE 32

#if defined(_WIN64)
#define USBIP_VHCI_INF_SECTION "Standard.NTamd64"
#else
#define USBIP_VHCI_INF_SECTION "Standard.NTx86"
#endif

struct usbip_install_devinfo_struct {
	char inf_filename[STR_BUFFER_SIZE];
	char hwid_inf_section[STR_BUFFER_SIZE];
	char hwid_inf_key[STR_BUFFER_SIZE];
	char devdesc_inf_section[STR_BUFFER_SIZE];  // Section in INF file in which the Device desc. is located
	char devdesc_inf_key[STR_BUFFER_SIZE];      // Device Description / Device Name
	char dev_instance_path[STR_BUFFER_SIZE];    // Device instance Path - needed for reinstall feature
};

enum {
	DEVICE_LIST_VHCI_DRIVER = 0,
};

static struct usbip_install_devinfo_struct device_list[] = {
	{
		.inf_filename        = "usbip_vhci.inf",
		.hwid_inf_section    = USBIP_VHCI_INF_SECTION,
		.hwid_inf_key        = "USB/IP VHCI",
		.devdesc_inf_section = "Strings",
		.devdesc_inf_key     = "DeviceDesc",
		.dev_instance_path   = "ROOT\\USBIP\\0000"
	}
};

static const char usbip_install_usage_string[] =
"usbip install\n"
"    install or reinstall usbip VHCI driver\n";

void usbip_install_usage(void)
{
	printf("usage: %s", usbip_install_usage_string);
}

static const char usbip_uninstall_usage_string[] =
"usbip uninstall\n"
"    remove the usbip VHCI device node\n";

void usbip_uninstall_usage(void)
{
	printf("usage: %s", usbip_uninstall_usage_string);
}

static BOOL usbip_install_get_inf_path(char *inf_path_buffer, DWORD buffer_size,
		const struct usbip_install_devinfo_struct *dev_data) {
	HRESULT result = GetModuleFileName(NULL, inf_path_buffer, buffer_size - 1);
	if (!result) {
		return FALSE;
	}
	result = PathRemoveFileSpec(inf_path_buffer);
	if (!result) {
		return FALSE;
	}
	result = PathAppend(inf_path_buffer, dev_data->inf_filename);
	if (!result) {
		return FALSE;
	}
	return TRUE;
}

static BOOL usbip_install_get_class_id(GUID *class_guid, const char *inf_path)
{
	char buffer[BUFFER_SIZE] = { 0 };
	const BOOL class_ok = SetupDiGetINFClass(inf_path,
		class_guid, buffer, BUFFER_SIZE, NULL);
	if (!class_ok) {
		return FALSE;
	}
	return TRUE;
}

static BOOL usbip_install_get_device_description(char *device_desc_buffer, DWORD buffer_size,
		HINF inf_handle,
		const struct usbip_install_devinfo_struct *dev_data)
{
	const BOOL ok = SetupGetLineText(NULL,
		inf_handle,
		dev_data->devdesc_inf_section,
		dev_data->devdesc_inf_key,
		device_desc_buffer, buffer_size, NULL);
	if (!ok) {
		return FALSE;
	}
	return TRUE;
}

static BOOL usbip_install_get_hardware_id(char *buffer, DWORD buffer_size,
	HINF inf_handle, const struct usbip_install_devinfo_struct *dev_data)
{
	char device_info[BUFFER_SIZE] = { 0 };
	char *hardware_id;
	size_t hardware_id_len;

	assert(buffer != NULL);
	assert(buffer_size > 0);
	assert(inf_handle != INVALID_HANDLE_VALUE);

	const BOOL ok = SetupGetLineText(NULL,
		inf_handle,
		dev_data->hwid_inf_section,
		dev_data->hwid_inf_key,
		device_info, sizeof(device_info), NULL);
	if (!ok) {
		return FALSE;
	}

	char *separator_pos = strchr(device_info, ',');
	if (separator_pos == NULL) {
		return FALSE;
	}

	hardware_id = separator_pos + 1;
	while (*hardware_id == ' ' || *hardware_id == '\t') {
		hardware_id++;
	}
	hardware_id_len = strlen(hardware_id);
	while (hardware_id_len > 0 &&
		(hardware_id[hardware_id_len - 1] == ' ' ||
		 hardware_id[hardware_id_len - 1] == '\t')) {
		hardware_id_len--;
	}
	if (hardware_id_len == 0 || hardware_id_len >= buffer_size) {
		return FALSE;
	}

	memcpy(buffer, hardware_id, hardware_id_len);
	buffer[hardware_id_len] = '\0';

	return TRUE;
}

typedef BOOL (WINAPI *di_uninstall_device_fn)(HWND, HDEVINFO,
	PSP_DEVINFO_DATA, DWORD, PBOOL);

static BOOL usbip_install_remove_device_node(HDEVINFO devinfoset,
	PSP_DEVINFO_DATA devinfo_data)
{
	HMODULE newdev;
	di_uninstall_device_fn uninstall_device;
	SP_REMOVEDEVICE_PARAMS remove_params;

	/*
	 * DiUninstallDevice was introduced after Windows 7. Resolve it at
	 * runtime so usbip.exe can still start on Windows 7, then fall back
	 * to the SetupAPI removal operation available since Windows 2000.
	 */
	newdev = LoadLibraryA("newdev.dll");
	if (newdev != NULL) {
		uninstall_device = (di_uninstall_device_fn)GetProcAddress(newdev,
			"DiUninstallDevice");
		if (uninstall_device != NULL &&
			uninstall_device(GetConsoleWindow(), devinfoset,
				devinfo_data, 0, NULL)) {
			FreeLibrary(newdev);
			return TRUE;
		}
		FreeLibrary(newdev);
	}

	memset(&remove_params, 0, sizeof(remove_params));
	remove_params.ClassInstallHeader.cbSize =
		sizeof(remove_params.ClassInstallHeader);
	remove_params.ClassInstallHeader.InstallFunction = DIF_REMOVE;
	remove_params.Scope = DICS_FLAG_GLOBAL;
	remove_params.HwProfile = 0;

	if (!SetupDiSetClassInstallParams(devinfoset, devinfo_data,
		(SP_CLASSINSTALL_HEADER *)&remove_params,
		sizeof(remove_params))) {
		err("Cannot set device removal parameters, code %u", GetLastError());
		return FALSE;
	}

	if (!SetupDiCallClassInstaller(DIF_REMOVE, devinfoset, devinfo_data)) {
		err("Cannot remove existing device, code %u", GetLastError());
		return FALSE;
	}

	return TRUE;
}

static BOOL usbip_install_remove_device(const struct usbip_install_devinfo_struct *dev_data)
{
	assert(dev_data != NULL);
	info("removing instance of %s", dev_data->dev_instance_path);

	HDEVINFO devinfoset = { 0 };
	SP_DEVINFO_DATA devinfo_data = { 0 };
	devinfo_data.cbSize = sizeof(SP_DEVINFO_DATA);

	devinfoset = SetupDiCreateDeviceInfoList(NULL, NULL);
	if (devinfoset == INVALID_HANDLE_VALUE) {
		return FALSE;
	}

	// from now on use goto error so that we wont leak devinfoset
	BOOL result_ok = SetupDiOpenDeviceInfo(devinfoset,
		dev_data->dev_instance_path,
		GetConsoleWindow(),
		0,
		&devinfo_data);
	if (!result_ok) {
		err("Cannot open DeviceInfo, code %u", GetLastError());
		goto error;
	}

	result_ok = usbip_install_remove_device_node(devinfoset, &devinfo_data);
	if (!result_ok) {
		err("Cannot uninstall existing device!");
		goto error;
	}

error:
	SetupDiDestroyDeviceInfoList(devinfoset);
	return result_ok;
}

static int usbip_install_base(struct usbip_install_devinfo_struct *data)
{
	GUID class_guid = { 0 };
	HINF inf_handle = INVALID_HANDLE_VALUE;
	char inf_file[BUFFER_SIZE] = { 0 };
	char hw_id[BUFFER_SIZE] = { 0 };
	char dev_decription[BUFFER_SIZE] = { 0 };
	BOOL device_created = FALSE;

	HDEVINFO devinfoset = { 0 };
	SP_DEVINFO_DATA devinfo_data = { 0 };
	BOOL result_ok = TRUE;
	DWORD last_error = 0;

	result_ok = usbip_install_get_inf_path(inf_file, BUFFER_SIZE, data);
	if (!result_ok) {
		err("Cannot find usbip_vhci.inf file");
		goto error;
	}

	if (!usbip_install_get_class_id(&class_guid, inf_file)) {
		err("Cannot obtain class id");
		goto error;
	}

	inf_handle = SetupOpenInfFileA(inf_file, "System", INF_STYLE_WIN4, NULL);
	if (inf_handle == INVALID_HANDLE_VALUE) {
		err("Cannot open inf file");
		goto error;
	}
	result_ok = usbip_install_get_device_description(dev_decription, BUFFER_SIZE,
		inf_handle, data);
	if(!result_ok) {
		err("Cannot get Device decription");
		goto error;
	}

	devinfoset = SetupDiCreateDeviceInfoList(&class_guid, NULL);
	if (devinfoset == INVALID_HANDLE_VALUE) {
		goto error;
	}

	do {
		memset(&devinfo_data, 0, sizeof(SP_DEVINFO_DATA));
		devinfo_data.cbSize = sizeof(SP_DEVINFO_DATA);
		result_ok = SetupDiCreateDeviceInfo(devinfoset,
			data->dev_instance_path,
			&class_guid,
			dev_decription,
			GetConsoleWindow(),
			0,
			&devinfo_data);
		if (!result_ok) {
			last_error = GetLastError();
			if (last_error == ERROR_ACCESS_DENIED) {
				err("Access Denied - make sure you are running as Administrator");
				goto error;
			}

			if (last_error != ERROR_DEVINST_ALREADY_EXISTS) {
				err("Cannot get DeviceInfo. Remove device manually by Device Manager, restart PC and try again");
				goto error;
			}
			result_ok = usbip_install_remove_device(data);
			if (!result_ok) {
				err("Cannot get DeviceInfo. Remove device manually by Device Manager, restart PC and try again");
				goto error;
			}
			continue;
		}
		break;
	} while (TRUE);

	device_created = TRUE;
	result_ok = usbip_install_get_hardware_id(hw_id, BUFFER_SIZE - 1,
		inf_handle, data);
	if (!result_ok) {
		goto error;
	}

	// We need to set HW ID for PlugAndPlay driver install
	result_ok = SetupDiSetDeviceRegistryProperty(devinfoset,
		&devinfo_data,
		SPDRP_HARDWAREID,
		(const BYTE *)hw_id, (DWORD)(strlen(hw_id) + 2));
	if (!result_ok) {
		goto error;
	}

	result_ok = SetupDiRegisterDeviceInfo(devinfoset,
		&devinfo_data,
		SPRDI_FIND_DUPS, NULL, NULL, NULL);
	if (!result_ok) {
		goto error;
	}

	// Install driver by INF file and hardware id
	result_ok = UpdateDriverForPlugAndPlayDevices(NULL,
		hw_id,
		inf_file,
		INSTALLFLAG_FORCE,
		FALSE);
	if (!result_ok) {
		last_error = GetLastError();
		err("%s: UpdateDriverForPlugAndPlayDevices failed: status: %x", __FUNCTION__, last_error);
		if (last_error == ERROR_NO_CATALOG_FOR_OEM_INF) {
			err("Missing .cat file");
		}
		goto error;
	}

	SetupDiDestroyDeviceInfoList(devinfoset);
	info("usbip_vhci driver installed sucessfully");
	return 0;

error:
	err("Cannot install usbip_vhci driver");
	SetupDiDestroyDeviceInfoList(devinfoset);

	if (device_created) {
		result_ok = usbip_install_remove_device(data);
		if (!result_ok) {
			err("Cannot get DeviceInfo. Remove device manually by Device Manager, restart PC and try again");
		}
	}

	return 1;
}

int usbip_install(int argc, char *argv[])
{
	int return_code = 0;
	return_code = usbip_install_base(&device_list[DEVICE_LIST_VHCI_DRIVER]);
	return return_code;
}

int usbip_uninstall(int argc, char *argv[])
{
	if (usbip_install_remove_device(&device_list[DEVICE_LIST_VHCI_DRIVER])) {
		return 0;
	}
	return 1;
}
