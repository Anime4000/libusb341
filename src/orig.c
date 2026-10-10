/*
 * libusb341 - loader for the original libusb-1.0.dll.
 *
 * Devices other than CH341s bound to the WCH driver (CH347, FT232H, a CH341
 * still on WinUSB/libusbK, ...) are handed to the real libusb.  It is looked
 * up, in order:
 *   1. %LIBUSB341_ORIG% (full path)
 *   2. libusb-1.0.orig.dll next to this DLL
 */
#include "shim.h"
#include <string.h>
#include <wchar.h>

struct orig_api O;
volatile LONG g_orig_inflight;

static INIT_ONCE g_orig_once = INIT_ONCE_STATIC_INIT;
static int g_orig_ok;

static HMODULE self_module(void)
{
	HMODULE m = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)(void *)&self_module, &m);
	return m;
}

static BOOL CALLBACK load_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	WCHAR path[MAX_PATH * 2];
	DWORD n;
	HMODULE m = NULL;

	(void)once; (void)param; (void)ctx;
	n = GetEnvironmentVariableW(L"LIBUSB341_ORIG", path, MAX_PATH * 2);
	if (n == 0 || n >= MAX_PATH * 2) {
		WCHAR *slash;
		n = GetModuleFileNameW(self_module(), path, MAX_PATH);
		if (n == 0 || n >= MAX_PATH)
			return TRUE;
		slash = wcsrchr(path, L'\\');
		if (!slash)
			return TRUE;
		wcscpy(slash + 1, L"libusb-1.0.orig.dll");
	}
	if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
		LOGI("original libusb not found (%ls); passthrough disabled", path);
		return TRUE;
	}
	m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!m) {
		LOGE("cannot load original libusb %ls (error %lu)", path, (unsigned long)GetLastError());
		return TRUE;
	}
	if (m == self_module() || GetProcAddress(m, "libusb341_version")) {
		LOGE("%ls is this shim, not the original libusb", path);
		FreeLibrary(m);
		return TRUE;
	}
#define ORIG_GET(name) O.name = (__typeof__(O.name))(void *)GetProcAddress(m, #name);
	ORIG_FUNCS(ORIG_GET)
#undef ORIG_GET
	if (!O.libusb_init || !O.libusb_exit || !O.libusb_get_device_list) {
		LOGE("%ls does not look like libusb-1.0", path);
		memset(&O, 0, sizeof(O));
		FreeLibrary(m);
		return TRUE;
	}
	O.mod = m;
	g_orig_ok = 1;
	LOGI("passthrough to %ls", path);
	return TRUE;
}

int orig_load(void)
{
	InitOnceExecuteOnce(&g_orig_once, load_once, NULL, NULL);
	return g_orig_ok;
}
