// pi/engine/card_guard.cpp - see card_guard.h. PI5_TESTMODE=1 builds only (Makefile): the linker's --wrap sends every call
// to these FatFs functions, the engine's and the libraries', through here. Off (the default) it only passes them on.
// f_truncate, f_expand, f_write and f_sync work on a file already open, f_open decided on that; nothing in the engine
// calls f_chmod, f_utime, f_setlabel or f_mkfs.
#include "card_guard.h"
#include <circle/logger.h>
#include <fatfs/ff.h>

extern "C" {
FRESULT __real_f_open (FIL *fp, const TCHAR *path, BYTE mode);
FRESULT __real_f_unlink (const TCHAR *path);
FRESULT __real_f_rename (const TCHAR *path_old, const TCHAR *path_new);
FRESULT __real_f_mkdir (const TCHAR *path);
FRESULT __wrap_f_open (FIL *fp, const TCHAR *path, BYTE mode);
FRESULT __wrap_f_unlink (const TCHAR *path);
FRESULT __wrap_f_rename (const TCHAR *path_old, const TCHAR *path_new);
FRESULT __wrap_f_mkdir (const TCHAR *path);
}

bool g_bCardGuard = false;			// testcard=1 (kernel.cpp)

static FRESULT Refused (const char *pCall, const char *pPath)
{
	static unsigned s_nShown;		// (a loop that retries does not fill the log)
	if (++s_nShown <= 32)
		CLogger::Get ()->Write ("cardguard", LogWarning, "testcard: %s %s refused: not one of Sidecar's folders", pCall, pPath);
	return FR_DENIED;
}

FRESULT __wrap_f_open (FIL *fp, const TCHAR *path, BYTE mode)
{
	const BYTE Create = FA_CREATE_NEW | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS | FA_OPEN_APPEND;
	if (g_bCardGuard && (mode & (FA_WRITE | Create)) != 0 && !cardguard::open_ok (path, (mode & Create) != 0))
		return Refused ("write", path);
	return __real_f_open (fp, path, mode);
}

FRESULT __wrap_f_unlink (const TCHAR *path)
{
	if (g_bCardGuard && !cardguard::change_ok (path)) return Refused ("delete", path);
	return __real_f_unlink (path);
}

FRESULT __wrap_f_rename (const TCHAR *path_old, const TCHAR *path_new)
{
	if (g_bCardGuard && !cardguard::rename_ok (path_old, path_new)) return Refused ("rename", path_old);
	return __real_f_rename (path_old, path_new);
}

FRESULT __wrap_f_mkdir (const TCHAR *path)
{
	if (g_bCardGuard && !cardguard::change_ok (path)) return Refused ("mkdir", path);
	return __real_f_mkdir (path);
}
