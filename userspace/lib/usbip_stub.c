#include "usbip_setupdi.h"
#include "usbip_stub.h"
#include "usbip_util.h"
#include "usbip_common.h"

#include <stdlib.h>
#include <stdio.h>
#include <newdev.h>

char *get_dev_property(HDEVINFO dev_info, PSP_DEVINFO_DATA pdev_info_data, DWORD prop);

BOOL build_cat(const char *path, const char *catname, const char *hwid);
BOOL sign_file(LPCSTR subject, LPCSTR fpath);

BOOL
is_service_usbip_stub(HDEVINFO dev_info, SP_DEVINFO_DATA *dev_info_data)
{
	char	*svcname;
	BOOL	res;

	svcname = get_dev_property(dev_info, dev_info_data, SPDRP_SERVICE);
	if (svcname == NULL)
		return FALSE;
	res = _stricmp(svcname, STUB_DRIVER_SVCNAME) == 0 ? TRUE: FALSE;
	free(svcname);
	return res;
}

static void
copy_file(const char *fname, const char *path_drvpkg)
{
	char	*path_src, *path_dst;
	char	*path_mod;

	path_mod = get_module_dir();
	if (path_mod == NULL) {
		return;
	}
	asprintf(&path_src, "%s\\%s", path_mod, fname);
	free(path_mod);
	asprintf(&path_dst, "%s\\%s", path_drvpkg, fname);

	CopyFile(path_src, path_dst, TRUE);
	free(path_src);
	free(path_dst);
}

static BOOL
replace_token(char *buf, size_t buf_size, const char *token, const char *replacement)
{
	size_t	token_len;
	size_t	replacement_len;
	size_t	buf_len;
	char	*mark;

	token_len = strlen(token);
	replacement_len = strlen(replacement);
	if (token_len == 0)
		return TRUE;

	buf_len = strlen(buf);
	while ((mark = strstr(buf, token)) != NULL) {
		size_t	suffix_len;

		if (buf_len - token_len + replacement_len + 1 > buf_size)
			return FALSE;

		suffix_len = buf_len - (size_t)(mark - buf) - token_len;
		memmove(mark + replacement_len, mark + token_len, suffix_len + 1);
		memcpy(mark, replacement, replacement_len);
		buf_len = buf_len - token_len + replacement_len;
	}
	return TRUE;
}

static BOOL
translate_inf(const char *id_hw, FILE *in, FILE *out)
{
	char	buf[4096];
	char	*line;
#if defined(_WIN64)
	const char	*arch = "amd64";
#else
	const char	*arch = "x86";
#endif

	while ((line = fgets(buf, 4096, in))) {
		if (!replace_token(line, sizeof(buf), "%hwid%", id_hw) ||
		    !replace_token(line, sizeof(buf), "$ARCH$", arch)) {
			err("%s: expanded INF line is too long", __FUNCTION__);
			return FALSE;
		}
		if (fwrite(line, strlen(line), 1, out) != 1) {
			err("%s: failed to write INF", __FUNCTION__);
			return FALSE;
		}
	}
	if (ferror(in)) {
		err("%s: failed to read INF", __FUNCTION__);
		return FALSE;
	}
	return TRUE;
}

static BOOL
copy_stub_inf(const char *id_hw, const char *path_drvpkg)
{
	char	*path_inx, *path_dst;
	char	*path_mod;
	FILE	*in, *out;
	errno_t	err;
	BOOL	ok;

	path_mod = get_module_dir();
	if (path_mod == NULL)
		return FALSE;
	asprintf(&path_inx, "%s\\usbip_stub.inx", path_mod);
	free(path_mod);

	err = fopen_s(&in, path_inx, "r");
	free(path_inx);
	if (err != 0) {
		err("%s: failed to open usbip_stub.inx", __FUNCTION__);
		return FALSE;
	}
	asprintf(&path_dst, "%s\\usbip_stub.inf", path_drvpkg);
	err = fopen_s(&out, path_dst, "w");
	free(path_dst);
	if (err != 0) {
		err("%s: failed to create usbip_stub.inf", __FUNCTION__);
		fclose(in);
		return FALSE;
	}

	ok = translate_inf(id_hw, in, out);
	fclose(in);
	if (fclose(out) != 0)
		ok = FALSE;
	return ok;
}

static void
remove_dir_all(const char *path_dir)
{
	char	*fpat;
	WIN32_FIND_DATA	wfd;
	HANDLE	hfs;

	asprintf(&fpat, "%s\\*", path_dir);
	hfs = FindFirstFile(fpat, &wfd);
	free(fpat);
	if (hfs != INVALID_HANDLE_VALUE) {
		do {
			if (*wfd.cFileName != '.') {
				char	*fpath;
				asprintf(&fpath, "%s\\%s", path_dir, wfd.cFileName);
				DeleteFile(fpath);
				free(fpath);
			}
		} while (FindNextFile(hfs, &wfd));

		FindClose(hfs);
	}
	RemoveDirectory(path_dir);
}

static BOOL
get_temp_drvpkg_path(char path_drvpkg[])
{
	char	tempdir[MAX_PATH + 1];

	if (GetTempPath(MAX_PATH + 1, tempdir) == 0)
		return FALSE;
	if (GetTempFileName(tempdir, "stub", 0, path_drvpkg) > 0) {
		DeleteFile(path_drvpkg);
		if (CreateDirectory(path_drvpkg, NULL))
			return TRUE;
	}
	else
		DeleteFile(path_drvpkg);
	return FALSE;
}

static BOOL
apply_stub_fdo(HDEVINFO dev_info, PSP_DEVINFO_DATA pdev_info_data)
{
	char	path_drvpkg[MAX_PATH + 1];
	char	*id_hw, *path_cat;
	char	*path_inf;
	BOOL	reboot_required;

	id_hw = get_id_hw(dev_info, pdev_info_data);
	if (id_hw == NULL)
		return FALSE;
	if (!get_temp_drvpkg_path(path_drvpkg)) {
		free(id_hw);
		return FALSE;
	}
	copy_file("usbip_stub.sys", path_drvpkg);
	if (!copy_stub_inf(id_hw, path_drvpkg)) {
		remove_dir_all(path_drvpkg);
		free(id_hw);
		return FALSE;
	}

	if (!build_cat(path_drvpkg, "usbip_stub.cat", id_hw)) {
		remove_dir_all(path_drvpkg);
		free(id_hw);
		return FALSE;
	}


	asprintf(&path_cat, "%s\\usbip_stub.cat", path_drvpkg);
	if (!sign_file("USBIP Test", path_cat)) {
		remove_dir_all(path_drvpkg);
		free(path_cat);
		free(id_hw);
		return FALSE;
	}

	free(path_cat);

	/* update driver */
	asprintf(&path_inf, "%s\\usbip_stub.inf", path_drvpkg);
	if (!UpdateDriverForPlugAndPlayDevicesA(NULL, id_hw, path_inf, INSTALLFLAG_NONINTERACTIVE, &reboot_required)) {
		err("failed to update driver: %lx", GetLastError());
		free(path_inf);
		free(id_hw);
		remove_dir_all(path_drvpkg);
		return FALSE;
	}
	free(path_inf);
	free(id_hw);

	remove_dir_all(path_drvpkg);

	return TRUE;
}

static BOOL
rollback_driver(HDEVINFO dev_info, PSP_DEVINFO_DATA pdev_info_data)
{
	BOOL	needReboot;

	if (!DiRollbackDriver(dev_info, pdev_info_data, NULL, ROLLBACK_FLAG_NO_UI, &needReboot)) {
		err("failed to rollback driver: %lx", GetLastError());
		return FALSE;
	}
	return TRUE;
}

static int
walker_attach(HDEVINFO dev_info, PSP_DEVINFO_DATA pdev_info_data, devno_t devno, void *ctx)
{
	devno_t	*pdevno = (devno_t *)ctx;

	if (devno == *pdevno) {
		if (!apply_stub_fdo(dev_info, pdev_info_data))
			return -2;
		return -1;
	}
	return 0;
}

BOOL
attach_stub_driver(devno_t devno)
{
	int	ret;

	ret = traverse_usbdevs(walker_attach, FALSE, &devno);
	if (ret == -1)
		return TRUE;
	return FALSE;
}

static int
walker_detach(HDEVINFO dev_info, PSP_DEVINFO_DATA pdev_info_data, devno_t devno, void *ctx)
{
	devno_t	*pdevno = (devno_t *)ctx;

	if (devno == *pdevno) {
		if (!rollback_driver(dev_info, pdev_info_data))
			return -2;
		return 1;
	}
	return 0;
}

BOOL
detach_stub_driver(devno_t devno)
{
	int	ret;

	ret = traverse_usbdevs(walker_detach, FALSE, &devno);
	if (ret == 1)
		return TRUE;
	return FALSE;
}
