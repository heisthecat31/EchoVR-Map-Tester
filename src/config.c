/* Settings in %APPDATA%\MapTester\config.ini (UTF-16, so map names keep their characters). */
#include "app.h"

#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

Config g_cfg;

static const wchar_t *SECTION = L"MapTester";

static void ini_path(wchar_t *out, size_t cap)
{
    /* MAPTESTER_CONFIG points at another ini -- for testing without touching the real settings */
    DWORD n = GetEnvironmentVariableW(L"MAPTESTER_CONFIG", out, (DWORD)cap);
    if (n > 0 && n < cap) return;
    wchar_t base[MAX_PATH] = L".";
    SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, base);
    wchar_t dir[MAX_PATH];
    path_join(dir, MAX_PATH, base, L"MapTester");
    CreateDirectoryW(dir, NULL);
    path_join(out, cap, dir, L"config.ini");
}

/* WritePrivateProfileString keeps Unicode only in a file that starts out as UTF-16 */
static void ensure_unicode(const wchar_t *file)
{
    if (file_exists(file)) return;
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, "\xFF\xFE", 2, &w, NULL);
        CloseHandle(h);
    }
}

static void get_str(const wchar_t *file, const wchar_t *key, const wchar_t *def, wchar_t *out, DWORD cap)
{
    GetPrivateProfileStringW(SECTION, key, def, out, cap, file);
}

static uint64_t get_u64(const wchar_t *file, const wchar_t *key)
{
    wchar_t t[32];
    GetPrivateProfileStringW(SECTION, key, L"0", t, 32, file);
    return _wcstoui64(t, NULL, 10);
}

static void put_u64(const wchar_t *file, const wchar_t *key, uint64_t v)
{
    wchar_t t[32];
    _snwprintf(t, 32, L"%llu", (unsigned long long)v);
    WritePrivateProfileStringW(SECTION, key, t, file);
}

void config_load(void)
{
    wchar_t file[MAX_PATH];
    ini_path(file, MAX_PATH);
    memset(&g_cfg, 0, sizeof g_cfg);
    get_str(file, L"EchoRoot", L"", g_cfg.echo_root, MAX_PATH);
    get_str(file, L"UploadKey", L"", g_cfg.upload_key, 256);
    get_str(file, L"InstalledId", L"", g_cfg.installed_id, 64);
    get_str(file, L"InstalledName", L"", g_cfg.installed_name, 256);
    g_cfg.installed_pkg_size = (int64_t)get_u64(file, L"InstalledPackageSize");
    g_cfg.installed_man_size = (int64_t)get_u64(file, L"InstalledManifestSize");
    g_cfg.installed_pkg_time = get_u64(file, L"InstalledPackageTime");
    g_cfg.installed_man_time = get_u64(file, L"InstalledManifestTime");
    /* a trailing slash would double up in every URL */
    /* The server is fixed for everyone. A Server= line saved by an early build is ignored. */
    wcscpy(g_cfg.server, DEFAULT_SERVER);
}

void config_save(void)
{
    wchar_t file[MAX_PATH];
    ini_path(file, MAX_PATH);
    ensure_unicode(file);
    WritePrivateProfileStringW(SECTION, L"EchoRoot", g_cfg.echo_root, file);
    WritePrivateProfileStringW(SECTION, L"Server", NULL, file);   /* drop any old saved address */
    WritePrivateProfileStringW(SECTION, L"UploadKey", g_cfg.upload_key, file);
    WritePrivateProfileStringW(SECTION, L"InstalledId", g_cfg.installed_id, file);
    WritePrivateProfileStringW(SECTION, L"InstalledName", g_cfg.installed_name, file);
    put_u64(file, L"InstalledPackageSize", (uint64_t)g_cfg.installed_pkg_size);
    put_u64(file, L"InstalledManifestSize", (uint64_t)g_cfg.installed_man_size);
    put_u64(file, L"InstalledPackageTime", g_cfg.installed_pkg_time);
    put_u64(file, L"InstalledManifestTime", g_cfg.installed_man_time);
}

/* ── maps uploaded from this PC ───────────────────────────────────────── */
static const wchar_t *UPLOADS = L"Uploads";

void uploads_remember(const wchar_t *id, const wchar_t *token)
{
    wchar_t file[MAX_PATH];
    ini_path(file, MAX_PATH);
    ensure_unicode(file);
    WritePrivateProfileStringW(UPLOADS, id, token, file);
}

bool uploads_token(const wchar_t *id, wchar_t *out, size_t cap)
{
    wchar_t file[MAX_PATH];
    ini_path(file, MAX_PATH);
    out[0] = 0;
    GetPrivateProfileStringW(UPLOADS, id, L"", out, (DWORD)cap, file);
    return out[0] != 0;
}

void uploads_forget(const wchar_t *id)
{
    wchar_t file[MAX_PATH];
    ini_path(file, MAX_PATH);
    WritePrivateProfileStringW(UPLOADS, id, NULL, file);
}
