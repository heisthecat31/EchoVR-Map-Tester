/*
 * Finding Echo VR and swapping the map files in.
 *
 * BACKUP RULES -- they record what was there before, so it can be told apart later:
 *
 *   A mod was already installed  (packages\48037dc70b0ecab2_3 exists, or manifests\48037dc70b0ecab2.bak does)
 *       48037dc70b0ecab2.bak is left alone -- it is the base game's manifest
 *       48037dc70b0ecab2_3   -> 48037dc70b0ecab2_3.bak
 *       48037dc70b0ecab2     -> 48037dc70b0ecab2.modbak
 *
 *   Base Echo VR  (neither exists)
 *       48037dc70b0ecab2     -> 48037dc70b0ecab2.bak
 *
 * A file Map Tester installed itself is simply replaced: backing it up would push the user's own
 * .bak / .modbak out of the way on every install. A backup name that is already taken is never
 * overwritten -- the new backup gets ".1", ".2", ... appended instead.
 *
 * Every rename is undone if any later step fails, so a failed install leaves the folder as it was.
 */
#include "app.h"

#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

bool echo_paths_from_root(const wchar_t *root, EchoPaths *out)
{
    EchoPaths p;
    memset(&p, 0, sizeof p);
    _snwprintf(p.root, MAX_PATH, L"%ls", root);
    size_t n = wcslen(p.root);
    while (n > 3 && (p.root[n - 1] == L'\\' || p.root[n - 1] == L'/')) p.root[--n] = 0;
    wchar_t data[MAX_PATH];
    path_join(data, MAX_PATH, p.root, ECHO_DATA_REL);
    path_join(p.packages, MAX_PATH, data, L"packages");
    path_join(p.manifests, MAX_PATH, data, L"manifests");
    if (!dir_exists(p.packages) || !dir_exists(p.manifests)) return false;
    path_join(p.package, MAX_PATH, p.packages, PACKAGE_NAME);
    path_join(p.manifest, MAX_PATH, p.manifests, MANIFEST_NAME);
    *out = p;
    return true;
}

bool echo_find(const wchar_t *picked, EchoPaths *out)
{
    if (!picked || !*picked) return false;
    wchar_t dir[MAX_PATH];
    _snwprintf(dir, MAX_PATH, L"%ls", picked);
    dir[MAX_PATH - 1] = 0;

    /* the pick itself, or any folder above it (someone who picked bin\win10 or _data) */
    wchar_t up[MAX_PATH];
    wcscpy(up, dir);
    for (int i = 0; i < 8; i++) {
        if (echo_paths_from_root(up, out)) return true;
        wchar_t *slash = wcsrchr(up, L'\\');
        if (!slash) slash = wcsrchr(up, L'/');
        if (!slash || slash == up) break;
        *slash = 0;
        if (wcslen(up) == 2 && up[1] == L':') wcscat(up, L"\\");
    }

    /* or a folder below it (someone who picked Software, or the Oculus library) */
    const wchar_t *direct[] = { L"ready-at-dawn-echo-arena", L"Software\\ready-at-dawn-echo-arena",
                                L"Software\\Software\\ready-at-dawn-echo-arena" };
    for (size_t i = 0; i < sizeof direct / sizeof *direct; i++) {
        wchar_t cand[MAX_PATH];
        path_join(cand, MAX_PATH, dir, direct[i]);
        if (echo_paths_from_root(cand, out)) return true;
    }
    wchar_t pattern[MAX_PATH];
    path_join(pattern, MAX_PATH, dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    bool found = false;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            wchar_t cand[MAX_PATH];
            path_join(cand, MAX_PATH, dir, fd.cFileName);
            found = echo_paths_from_root(cand, out);
        } while (!found && FindNextFileW(h, &fd));
        FindClose(h);
    }
    return found;
}

bool echo_guess(EchoPaths *out)
{
    const wchar_t *rel[] = {
        L"\\Oculus\\Games\\Software\\Software\\ready-at-dawn-echo-arena",
        L"\\Oculus\\Software\\Software\\ready-at-dawn-echo-arena",
        L"\\Program Files\\Oculus\\Software\\Software\\ready-at-dawn-echo-arena",
        L"\\Program Files\\Meta Horizon\\Software\\Software\\ready-at-dawn-echo-arena",
        L"\\Games\\ready-at-dawn-echo-arena",
        L"\\ready-at-dawn-echo-arena",
        L"\\EchoVR\\ready-at-dawn-echo-arena",
    };
    DWORD drives = GetLogicalDrives();
    for (int d = 2; d < 26; d++) {           /* C: .. Z: */
        if (!(drives & (1u << d))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + d), L':', 0, 0 };
        wchar_t probe[8];
        _snwprintf(probe, 8, L"%ls\\", root);
        if (GetDriveTypeW(probe) != DRIVE_FIXED) continue;
        for (size_t i = 0; i < sizeof rel / sizeof *rel; i++) {
            wchar_t cand[MAX_PATH];
            _snwprintf(cand, MAX_PATH, L"%ls%ls", root, rel[i]);
            if (echo_paths_from_root(cand, out)) return true;
        }
    }
    return false;
}

InstallState install_state(const EchoPaths *p, wchar_t *label, size_t cap)
{
    if (!p || !dir_exists(p->packages)) {
        _snwprintf(label, cap, L"Echo VR not found");
        return STATE_NO_ECHO;
    }
    bool has_pkg = file_exists(p->package);
    if (has_pkg && g_cfg.installed_id[0] &&
        file_size(p->package) == g_cfg.installed_pkg_size &&
        file_size(p->manifest) == g_cfg.installed_man_size &&
        file_time(p->package) == g_cfg.installed_pkg_time &&
        file_time(p->manifest) == g_cfg.installed_man_time) {
        _snwprintf(label, cap, L"%ls", g_cfg.installed_name[0] ? g_cfg.installed_name : L"Custom map");
        return STATE_OURS;
    }
    if (has_pkg) {
        _snwprintf(label, cap, L"Unknown mod");
        return STATE_OTHER_MOD;
    }
    _snwprintf(label, cap, L"Base Echo VR");
    return STATE_BASE;
}

bool echo_is_running(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    bool running = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!_wcsicmp(pe.szExeFile, L"echovr.exe")) { running = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return running;
}

/* ── the swap ─────────────────────────────────────────────────────────── */
typedef struct { wchar_t from[MAX_PATH], to[MAX_PATH]; } Move;
typedef struct { Move done[8]; int n; } Journal;

static bool journal_move(Journal *j, const wchar_t *from, const wchar_t *to, bool replace)
{
    if (!MoveFileExW(from, to, replace ? MOVEFILE_REPLACE_EXISTING : 0)) return false;
    if (j->n < 8) {
        wcscpy(j->done[j->n].from, from);
        wcscpy(j->done[j->n].to, to);
        j->n++;
    }
    return true;
}

static void journal_undo(Journal *j)
{
    while (j->n > 0) {
        j->n--;
        MoveFileExW(j->done[j->n].to, j->done[j->n].from, MOVEFILE_REPLACE_EXISTING);
    }
}

/* `path` -> `path + suffix`, or `path + suffix + ".N"` when that name is taken */
static bool backup(Journal *j, const wchar_t *path, const wchar_t *suffix, wchar_t *used, size_t cap)
{
    wchar_t target[MAX_PATH];
    _snwprintf(target, MAX_PATH, L"%ls%ls", path, suffix);
    for (int i = 1; file_exists(target) || dir_exists(target); i++) {
        if (i > 999) return false;
        _snwprintf(target, MAX_PATH, L"%ls%ls.%d", path, suffix, i);
    }
    if (!journal_move(j, path, target, false)) return false;
    const wchar_t *name = wcsrchr(target, L'\\');
    _snwprintf(used, cap, L"%ls", name ? name + 1 : target);
    return true;
}

static void append(wchar_t *report, size_t cap, const wchar_t *fmt, const wchar_t *a, const wchar_t *b)
{
    size_t n = wcslen(report);
    if (n + 1 >= cap) return;
    _snwprintf(report + n, cap - n, fmt, a, b);
    report[cap - 1] = 0;
}

bool install_files(const EchoPaths *p, const wchar_t *pkg_tmp, const wchar_t *man_tmp,
                   wchar_t *report, size_t cap)
{
    report[0] = 0;
    wchar_t label[64];
    InstallState state = install_state(p, label, 64);
    Journal j = { .n = 0 };
    wchar_t used[MAX_PATH];
    wchar_t pkg_old[MAX_PATH], man_old[MAX_PATH];
    pkg_old[0] = man_old[0] = 0;

    if (state == STATE_OURS) {
        /* our own previous map: set it aside only until the new one is in */
        _snwprintf(pkg_old, MAX_PATH, L"%ls.maptester-old", p->package);
        _snwprintf(man_old, MAX_PATH, L"%ls.maptester-old", p->manifest);
        DeleteFileW(pkg_old);
        DeleteFileW(man_old);
        if (!journal_move(&j, p->package, pkg_old, false) ||
            (file_exists(p->manifest) && !journal_move(&j, p->manifest, man_old, false))) {
            _snwprintf(report, cap, L"Could not move the previous map aside (error %lu). Is Echo VR still open?",
                       (unsigned long)GetLastError());
            journal_undo(&j);
            return false;
        }
        append(report, cap, L"Replaced the map Map Tester installed before.\n", L"", L"");
    } else {
        wchar_t man_bak[MAX_PATH];
        _snwprintf(man_bak, MAX_PATH, L"%ls.bak", p->manifest);
        bool mod_before = file_exists(p->package) || file_exists(man_bak);
        if (file_exists(p->package)) {
            if (!backup(&j, p->package, L".bak", used, MAX_PATH)) goto fail;
            append(report, cap, L"Backed up the existing %ls as %ls.\n", PACKAGE_NAME, used);
        }
        if (file_exists(p->manifest)) {
            if (!backup(&j, p->manifest, mod_before ? L".modbak" : L".bak", used, MAX_PATH)) goto fail;
            append(report, cap, mod_before ? L"A mod was installed before: kept %ls.bak, saved the mod manifest as %ls.\n"
                                           : L"Base Echo VR: saved the original manifest as %ls%ls.\n",
                   mod_before ? MANIFEST_NAME : L"", used);
        }
    }

    if (!journal_move(&j, pkg_tmp, p->package, false) ||
        !journal_move(&j, man_tmp, p->manifest, false))
        goto fail;

    if (pkg_old[0]) DeleteFileW(pkg_old);
    if (man_old[0]) DeleteFileW(man_old);
    return true;

fail:;
    DWORD err = GetLastError();
    journal_undo(&j);
    _snwprintf(report, cap, L"Installing failed (Windows error %lu) and every change was undone. "
                            L"Close Echo VR and try again.", (unsigned long)err);
    return false;
}

/* ── reverting ────────────────────────────────────────────────────────── */
/*
 *   Modded before  (48037dc70b0ecab2.modbak exists)
 *       48037dc70b0ecab2_3.bak  -> 48037dc70b0ecab2_3   (or the current _3 is removed if there is none)
 *       48037dc70b0ecab2.modbak -> 48037dc70b0ecab2
 *       48037dc70b0ecab2.bak stays: it is still the base game's manifest
 *
 *   Base Echo VR before  (only 48037dc70b0ecab2.bak)
 *       48037dc70b0ecab2_3 is removed
 *       48037dc70b0ecab2.bak    -> 48037dc70b0ecab2
 *
 * The current files are moved aside first and only deleted once the backup is in place, so a
 * failure part way puts everything back.
 */
static void backup_names(const EchoPaths *p, wchar_t *pkg_bak, wchar_t *man_bak, wchar_t *man_modbak)
{
    _snwprintf(pkg_bak, MAX_PATH, L"%ls.bak", p->package);
    _snwprintf(man_bak, MAX_PATH, L"%ls.bak", p->manifest);
    _snwprintf(man_modbak, MAX_PATH, L"%ls.modbak", p->manifest);
}

BackupKind backup_kind(const EchoPaths *p)
{
    if (!p || !dir_exists(p->manifests)) return BACKUP_NONE;
    wchar_t pkg_bak[MAX_PATH], man_bak[MAX_PATH], man_modbak[MAX_PATH];
    backup_names(p, pkg_bak, man_bak, man_modbak);
    if (file_exists(man_modbak)) return BACKUP_MODDED;
    if (file_exists(man_bak)) return BACKUP_BASE;
    return BACKUP_NONE;
}

bool revert_backup(const EchoPaths *p, wchar_t *report, size_t cap)
{
    report[0] = 0;
    BackupKind kind = backup_kind(p);
    if (kind == BACKUP_NONE) {
        _snwprintf(report, cap, L"There is no backup to go back to.");
        return false;
    }
    wchar_t pkg_bak[MAX_PATH], man_bak[MAX_PATH], man_modbak[MAX_PATH];
    backup_names(p, pkg_bak, man_bak, man_modbak);
    wchar_t pkg_aside[MAX_PATH], man_aside[MAX_PATH];
    _snwprintf(pkg_aside, MAX_PATH, L"%ls.maptester-revert", p->package);
    _snwprintf(man_aside, MAX_PATH, L"%ls.maptester-revert", p->manifest);
    DeleteFileW(pkg_aside);
    DeleteFileW(man_aside);

    Journal j = { .n = 0 };
    if (file_exists(p->package) && !journal_move(&j, p->package, pkg_aside, false)) goto fail;
    if (file_exists(p->manifest) && !journal_move(&j, p->manifest, man_aside, false)) goto fail;

    if (kind == BACKUP_MODDED) {
        if (file_exists(pkg_bak) && !journal_move(&j, pkg_bak, p->package, false)) goto fail;
        if (!journal_move(&j, man_modbak, p->manifest, false)) goto fail;
        _snwprintf(report, cap, L"Restored the mod that was installed before (%ls and %ls).",
                   file_exists(p->package) ? PACKAGE_NAME L".bak" : L"no package",
                   MANIFEST_NAME L".modbak");
    } else {
        if (!journal_move(&j, man_bak, p->manifest, false)) goto fail;
        _snwprintf(report, cap, L"Restored base Echo VR (%ls.bak), and removed %ls.", MANIFEST_NAME, PACKAGE_NAME);
    }
    DeleteFileW(pkg_aside);
    DeleteFileW(man_aside);
    return true;

fail:;
    DWORD err = GetLastError();
    journal_undo(&j);
    _snwprintf(report, cap, L"Reverting failed (Windows error %lu) and nothing was changed. "
                            L"Close Echo VR and try again.", (unsigned long)err);
    return false;
}
