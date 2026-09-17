/* Map Tester -- main window: map list, preview, install. */
#define COBJMACROS
#include "app.h"

#include <dwmapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

HINSTANCE g_inst;
HWND g_main;
EchoPaths g_echo;
bool g_have_echo;

/* ── layout, in 96-dpi units ─────────────────────────────────────────── */
#define HEADER_H  72
#define SIDE_W    370
#define PAD       20
#define CARD_H    112
#define CARD_GAP  10
#define BTN_H     50

static int g_dpi = 96;
int dpi(int v) { return MulDiv(v, g_dpi, 96); }

/* ── maps ────────────────────────────────────────────────────────────── */
typedef struct {
    wchar_t *id, *name, *gametype, *creator, *description;
    int64_t pkg_size, man_size;
    char pkg_sha[65], man_sha[65];
    GpImage image;
    int image_state;                 /* 0 not asked, 1 loading, 2 ready, 3 none */
    bool owned;                      /* uploaded from this PC, so it can be removed from here */
    /* preview cache file name: id + the image's hash, so an edited preview is fetched again */
    wchar_t cache_key[64];
} MapItem;

typedef struct { MapItem *items; int count; } MapList;

static MapList g_maps;
static enum { LIST_LOADING, LIST_READY, LIST_ERROR } g_list_state = LIST_LOADING;
static wchar_t g_list_error[300];
static int g_sel = -1;
static int g_scroll;
static wchar_t g_pending_select[64];

/* ── install job ─────────────────────────────────────────────────────── */
typedef struct { int phase; int64_t done, total; } Progress;

typedef struct {
    bool ok;
    wchar_t id[64], name[256];
    wchar_t message[1200];
    int64_t pkg_size, man_size;
    uint64_t pkg_time, man_time;
} InstallResult;

static struct {
    bool busy;
    wchar_t id[64];
    Progress progress;
    wchar_t status[1200];
    COLORREF status_color;
} g_job;
static volatile LONG g_cancel;

static wchar_t g_installed_label[256] = L"…";
static InstallState g_installed_state = STATE_NO_ECHO;

/* ── hover / hit testing ─────────────────────────────────────────────── */
typedef enum { HIT_NONE, HIT_CARD, HIT_UPLOAD, HIT_INSTALL, HIT_SETTINGS, HIT_REFRESH, HIT_RETRY, HIT_PILL, HIT_REMOVE, HIT_EDIT } Hit;
static Hit g_hot = HIT_NONE;
static int g_hot_card = -1;
static bool g_tracking;
/* what the left button went down on -- a click needs both halves on the same target */
static Hit g_press_hit = HIT_NONE;
static int g_press_card = -1;

typedef struct {
    RECT header, side, list, upload, refresh, settings, pill;
    RECT main, title, meta, desc, image, install, status, retry, remove, edit;
} Layout;

/* ── fonts ───────────────────────────────────────────────────────────── */
typedef struct { int px, weight; bool icon; HFONT font; } FontSlot;
static FontSlot g_fonts[24];

static HFONT make_font(int px, int weight, bool icon)
{
    for (int i = 0; i < 24; i++) {
        if (g_fonts[i].font && g_fonts[i].px == px && g_fonts[i].weight == weight && g_fonts[i].icon == icon)
            return g_fonts[i].font;
    }
    for (int i = 0; i < 24; i++) {
        if (!g_fonts[i].font) {
            g_fonts[i].px = px;
            g_fonts[i].weight = weight;
            g_fonts[i].icon = icon;
            g_fonts[i].font = CreateFontW(-dpi(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                          DEFAULT_PITCH, icon ? L"Segoe MDL2 Assets" : L"Segoe UI");
            return g_fonts[i].font;
        }
    }
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

HFONT ui_font(int px, int weight) { return make_font(px, weight, false); }
static HFONT icon_font(int px) { return make_font(px, FW_NORMAL, true); }

static void draw_text(HDC dc, const wchar_t *s, RECT r, HFONT f, COLORREF c, UINT flags)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

static int text_width(HDC dc, const wchar_t *s, HFONT f)
{
    HGDIOBJ old = SelectObject(dc, f);
    RECT r = { 0, 0, 0, 0 };
    DrawTextW(dc, s, -1, &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.right - r.left;
}

static bool in_rect(const RECT *r, int x, int y) { return x >= r->left && x < r->right && y >= r->top && y < r->bottom; }

static COLORREF type_color(const wchar_t *type)
{
    if (!_wcsicmp(type, L"Arena")) return C_ACCENT;
    if (!_wcsicmp(type, L"Combat")) return RGB(239, 83, 97);
    if (!_wcsicmp(type, L"Social")) return C_ACCENT2;
    return RGB(170, 120, 255);
}

/* ── map list lifetime ───────────────────────────────────────────────── */
static void free_maps(MapList *l)
{
    for (int i = 0; i < l->count; i++) {
        MapItem *m = &l->items[i];
        free(m->id); free(m->name); free(m->gametype); free(m->creator); free(m->description);
        gfx_free_image(m->image);
    }
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

static bool safe_id(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n > 32) return false;
    for (size_t i = 0; i < n; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
    return true;
}

static MapList *parse_maps(const char *text, wchar_t *err, size_t cap)
{
    JVal *root = json_parse(text);
    JVal *arr = json_get(root, "maps");
    if (!arr || arr->type != J_ARR) {
        _snwprintf(err, cap, L"The server's map list could not be read");
        json_free(root);
        return NULL;
    }
    int n = 0;
    for (JVal *o = arr->child; o; o = o->next) n++;
    MapList *l = (MapList *)calloc(1, sizeof(MapList));
    l->items = (MapItem *)calloc(n ? n : 1, sizeof(MapItem));
    for (JVal *o = arr->child; o; o = o->next) {
        const char *id = json_str(o, "id", "");
        if (!safe_id(id)) continue;          /* ids become file names and URLs */
        MapItem *m = &l->items[l->count++];
        m->id = utf8_to_wide(id);
        m->name = utf8_to_wide(json_str(o, "name", "Untitled"));
        m->gametype = utf8_to_wide(json_str(o, "gametype", "Other"));
        m->creator = utf8_to_wide(json_str(o, "creator", "Unknown"));
        m->description = utf8_to_wide(json_str(o, "description", ""));
        m->pkg_size = (int64_t)json_num(o, "package_size", 0);
        m->man_size = (int64_t)json_num(o, "manifest_size", 0);
        const char *img = json_str(o, "image_sha256", "");
        char tag[13] = "";
        size_t t = 0;
        while (t < 12 && ((img[t] >= '0' && img[t] <= '9') || (img[t] >= 'a' && img[t] <= 'f'))) {
            tag[t] = img[t];
            t++;
        }
        tag[t] = 0;
        _snwprintf(m->cache_key, 64, t ? L"%ls_%hs" : L"%ls", m->id, tag);
        snprintf(m->pkg_sha, 65, "%s", json_str(o, "package_sha256", ""));
        snprintf(m->man_sha, 65, "%s", json_str(o, "manifest_sha256", ""));
    }
    json_free(root);
    return l;
}

static DWORD WINAPI fetch_maps_thread(LPVOID arg)
{
    wchar_t *server = (wchar_t *)arg;
    wchar_t url[700];
    _snwprintf(url, 700, L"%ls/maps", server);
    NetResult r;
    MapList *list = NULL;
    wchar_t *err = (wchar_t *)calloc(300, sizeof(wchar_t));
    if (net_request(L"GET", url, NULL, NULL, 0, &r)) {
        list = parse_maps(r.body.data, err, 300);
    } else {
        wchar_t why[250];
        net_describe_error(&r, why, 250);
        _snwprintf(err, 300, L"Couldn't load maps from %ls — %ls", server, why);
    }
    net_result_free(&r);
    free(server);
    if (!PostMessageW(g_main, WM_APP_MAPS, (WPARAM)err, (LPARAM)list)) {
        free(err);
        if (list) { free_maps(list); free(list); }
    }
    return 0;
}

void refresh_maps(void)
{
    g_list_state = LIST_LOADING;
    InvalidateRect(g_main, NULL, FALSE);
    HANDLE t = CreateThread(NULL, 0, fetch_maps_thread, wcsdup_safe(g_cfg.server), 0, NULL);
    if (t) CloseHandle(t);
}

void select_map_by_id(const wchar_t *id)
{
    _snwprintf(g_pending_select, 64, L"%ls", id);
    refresh_maps();
}

/* ── preview images ──────────────────────────────────────────────────── */
static void cache_path(const wchar_t *key, wchar_t *out, size_t cap)
{
    wchar_t dir[MAX_PATH];
    local_data_dir(L"cache", dir, MAX_PATH);
    wchar_t name[80];
    _snwprintf(name, 80, L"%ls.img", key);
    path_join(out, cap, dir, name);
}

typedef struct { wchar_t server[512], id[64], key[64]; } ImageJob;

static DWORD WINAPI fetch_image_thread(LPVOID arg)
{
    ImageJob *job = (ImageJob *)arg;
    wchar_t url[700], dest[MAX_PATH], part[MAX_PATH];
    _snwprintf(url, 700, L"%ls/maps/%ls/image", job->server, job->id);
    cache_path(job->key, dest, MAX_PATH);
    _snwprintf(part, MAX_PATH, L"%ls.part", dest);
    NetResult r;
    bool ok = net_download(url, part, NULL, NULL, NULL, &r) &&
              MoveFileExW(part, dest, MOVEFILE_REPLACE_EXISTING);
    net_result_free(&r);
    if (!ok) DeleteFileW(part);
    wchar_t *id = wcsdup_safe(job->id);
    free(job);
    if (!PostMessageW(g_main, WM_APP_IMAGE, ok, (LPARAM)id)) free(id);
    return 0;
}

static void ensure_image(int index)
{
    if (index < 0 || index >= g_maps.count) return;
    MapItem *m = &g_maps.items[index];
    if (m->image_state != 0) return;
    wchar_t path[MAX_PATH];
    cache_path(m->cache_key, path, MAX_PATH);
    if (file_exists(path) && (m->image = gfx_load_image(path)) != NULL) {
        m->image_state = 2;
        return;
    }
    m->image_state = 1;
    ImageJob *job = (ImageJob *)calloc(1, sizeof(ImageJob));
    _snwprintf(job->server, 512, L"%ls", g_cfg.server);
    _snwprintf(job->id, 64, L"%ls", m->id);
    _snwprintf(job->key, 64, L"%ls", m->cache_key);
    HANDLE t = CreateThread(NULL, 0, fetch_image_thread, job, 0, NULL);
    if (t) CloseHandle(t); else { free(job); m->image_state = 3; }
}

/* ── installed-map label ─────────────────────────────────────────────── */
static void update_installed(void)
{
    g_installed_state = g_have_echo ? install_state(&g_echo, g_installed_label, 256) : STATE_NO_ECHO;
    if (!g_have_echo) wcscpy(g_installed_label, L"Echo VR folder not set");
    InvalidateRect(g_main, NULL, FALSE);
}

/* ── folder picker & prompt ──────────────────────────────────────────── */
bool pick_folder(HWND owner, const wchar_t *title, const wchar_t *start, wchar_t *out, size_t cap)
{
    bool ok = false;
    IFileOpenDialog *dlg = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg)))
        return false;
    FILEOPENDIALOGOPTIONS opts = 0;
    IFileOpenDialog_GetOptions(dlg, &opts);
    IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(dlg, title);
    if (start && *start && dir_exists(start)) {
        IShellItem *folder = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(start, NULL, &IID_IShellItem, (void **)&folder))) {
            IFileOpenDialog_SetFolder(dlg, folder);
            IShellItem_Release(folder);
        }
    }
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, owner))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
            PWSTR path = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
                _snwprintf(out, cap, L"%ls", path);
                out[cap - 1] = 0;
                CoTaskMemFree(path);
                ok = true;
            }
            IShellItem_Release(item);
        }
    }
    IFileOpenDialog_Release(dlg);
    return ok;
}

typedef struct { const wchar_t *title, *label; wchar_t *value; size_t cap; HWND edit; bool done, ok; } Prompt;
#define ID_PROMPT_EDIT 100

static LRESULT CALLBACK prompt_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Prompt *p = (Prompt *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        p = (Prompt *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)p);
        p->edit = CreateWindowExW(0, L"EDIT", p->value, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                                  dpi(34), dpi(78), dpi(412), dpi(24), h, (HMENU)(INT_PTR)ID_PROMPT_EDIT, g_inst, NULL);
        SendMessageW(p->edit, WM_SETFONT, (WPARAM)ui_font(15, FW_NORMAL), TRUE);
        SendMessageW(p->edit, EM_LIMITTEXT, p->cap - 1, 0);
        HWND ok = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                                  dpi(340), dpi(126), dpi(120), dpi(40), h, (HMENU)IDOK, g_inst, NULL);
        HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                                      dpi(210), dpi(126), dpi(120), dpi(40), h, (HMENU)IDCANCEL, g_inst, NULL);
        SendMessageW(ok, WM_SETFONT, (WPARAM)ui_font(15, FW_SEMIBOLD), TRUE);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)ui_font(15, FW_SEMIBOLD), TRUE);
        SetFocus(p->edit);
        SendMessageW(p->edit, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(C_PANEL);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        RECT t = { dpi(24), dpi(18), rc.right - dpi(24), dpi(46) };
        draw_text(dc, p->title, t, ui_font(19, FW_SEMIBOLD), C_TEXT, DT_SINGLELINE | DT_VCENTER);
        RECT l = { dpi(24), dpi(46), rc.right - dpi(24), dpi(68) };
        draw_text(dc, p->label, l, ui_font(14, FW_NORMAL), C_MUTED, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        gfx_fill_round(dc, dpi(24), dpi(70), dpi(432), dpi(40), dpi(8), ARGB(255, C_FIELD));
        gfx_stroke_round(dc, dpi(24), dpi(70), dpi(432), dpi(40), dpi(8), ARGB(255, C_BORDER), 1.0f);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        static HBRUSH field;
        if (!field) field = CreateSolidBrush(C_FIELD);
        SetTextColor((HDC)wp, C_TEXT);
        SetBkColor((HDC)wp, C_FIELD);
        return (LRESULT)field;
    }
    case WM_DRAWITEM:
        ui_draw_button((const DRAWITEMSTRUCT *)lp, ((const DRAWITEMSTRUCT *)lp)->CtlID == IDOK);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            GetWindowTextW(p->edit, p->value, (int)p->cap);
            p->ok = true;
            DestroyWindow(h);
        } else if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(h);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        p->done = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool prompt_text(HWND owner, const wchar_t *title, const wchar_t *label, wchar_t *value, size_t cap)
{
    Prompt p = { title, label, value, cap, NULL, false, false };
    RECT o;
    GetWindowRect(owner, &o);
    int w = dpi(480), h = dpi(186);
    RECT adj = { 0, 0, w, h };
    AdjustWindowRectEx(&adj, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int ww = adj.right - adj.left, wh = adj.bottom - adj.top;
    HWND win = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MapTesterPrompt", title, WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               o.left + (o.right - o.left - ww) / 2, o.top + (o.bottom - o.top - wh) / 2, ww, wh,
                               owner, NULL, g_inst, &p);
    if (!win) return false;
    ui_dark_titlebar(win);
    EnableWindow(owner, FALSE);
    MSG msg;
    while (!p.done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) { SendMessageW(win, WM_COMMAND, IDOK, 0); continue; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { SendMessageW(win, WM_COMMAND, IDCANCEL, 0); continue; }
        if (!IsDialogMessageW(win, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    return p.ok;
}

/* ── shared widgets ──────────────────────────────────────────────────── */
void ui_dark_titlebar(HWND h)
{
    BOOL on = TRUE;
    if (FAILED(DwmSetWindowAttribute(h, 20, &on, sizeof on)))   /* DWMWA_USE_IMMERSIVE_DARK_MODE */
        DwmSetWindowAttribute(h, 19, &on, sizeof on);           /* builds before 20H1 */
}

void ui_draw_button(const DRAWITEMSTRUCT *d, bool primary)
{
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    HBRUSH bg = CreateSolidBrush(C_PANEL);
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    bool disabled = d->itemState & ODS_DISABLED;
    bool pressed = d->itemState & ODS_SELECTED;
    COLORREF fill = primary ? C_ACCENT : C_CARD_HOT;
    if (pressed) fill = primary ? RGB(220, 112, 20) : C_BORDER;
    gfx_fill_round(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, dpi(10), ARGB(disabled ? 110 : 255, fill));
    if (!primary) gfx_stroke_round(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, dpi(10), ARGB(255, C_BORDER), 1.0f);
    if (d->itemState & ODS_FOCUS)
        gfx_stroke_round(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, dpi(10), ARGB(200, C_ACCENT2), 2.0f);
    wchar_t text[128];
    GetWindowTextW(d->hwndItem, text, 128);
    draw_text(dc, text, r, ui_font(15, FW_SEMIBOLD), disabled ? C_FAINT : (primary ? RGB(20, 12, 4) : C_TEXT),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* ── install ─────────────────────────────────────────────────────────── */
typedef struct {
    wchar_t server[512], id[64], name[256];
    char pkg_sha[65], man_sha[65];
    int64_t pkg_size, man_size;
    EchoPaths echo;
    uint64_t last_post;
    int phase;
} InstallJob;

static bool install_progress(void *ctx, int64_t done, int64_t total)
{
    InstallJob *job = (InstallJob *)ctx;
    uint64_t now = GetTickCount64();
    if (now - job->last_post >= 100 || done == total) {
        job->last_post = now;
        Progress *p = (Progress *)malloc(sizeof(Progress));
        p->phase = job->phase;
        p->done = done;
        p->total = total;
        if (!PostMessageW(g_main, WM_APP_PROGRESS, 0, (LPARAM)p)) free(p);
    }
    return !InterlockedCompareExchange(&g_cancel, 0, 0);
}

static bool fetch_part(InstallJob *job, const wchar_t *part, const wchar_t *dest, const char *want_sha,
                       wchar_t *msg, size_t cap)
{
    wchar_t url[700];
    _snwprintf(url, 700, L"%ls/maps/%ls/%ls", job->server, job->id, part);
    char got_sha[65] = "";
    NetResult r;
    bool ok = net_download(url, dest, got_sha, install_progress, job, &r);
    if (!ok) {
        wchar_t why[300];
        net_describe_error(&r, why, 300);
        _snwprintf(msg, cap, L"Downloading the %ls failed: %ls", part, why);
    } else if (want_sha[0] && _stricmp(want_sha, got_sha) != 0) {
        _snwprintf(msg, cap, L"The downloaded %ls is damaged (checksum mismatch). Try again.", part);
        DeleteFileW(dest);
        ok = false;
    }
    net_result_free(&r);
    return ok;
}

static DWORD WINAPI install_thread(LPVOID arg)
{
    InstallJob *job = (InstallJob *)arg;
    InstallResult *res = (InstallResult *)calloc(1, sizeof(InstallResult));
    _snwprintf(res->id, 64, L"%ls", job->id);
    _snwprintf(res->name, 256, L"%ls", job->name);
    wchar_t pkg_tmp[MAX_PATH], man_tmp[MAX_PATH];
    _snwprintf(pkg_tmp, MAX_PATH, L"%ls.maptester-download", job->echo.package);
    _snwprintf(man_tmp, MAX_PATH, L"%ls.maptester-download", job->echo.manifest);

    ULARGE_INTEGER free_bytes;
    int64_t need = job->pkg_size + job->man_size + (64 << 20);
    if (echo_is_running()) {
        wcscpy(res->message, L"Echo VR is running. Close the game, then install the map.");
    } else if (GetDiskFreeSpaceExW(job->echo.packages, &free_bytes, NULL, NULL) &&
               (int64_t)free_bytes.QuadPart < need) {
        wchar_t a[32], b[32];
        format_size(need, a, 32);
        format_size((int64_t)free_bytes.QuadPart, b, 32);
        _snwprintf(res->message, 1200, L"Not enough disk space: the map needs %ls and the Echo VR drive has %ls free.", a, b);
    } else {
        job->phase = 1;
        if (fetch_part(job, L"package", pkg_tmp, job->pkg_sha, res->message, 1200)) {
            job->phase = 2;
            if (fetch_part(job, L"manifest", man_tmp, job->man_sha, res->message, 1200)) {
                Progress *p = (Progress *)calloc(1, sizeof(Progress));
                p->phase = 3;
                if (!PostMessageW(g_main, WM_APP_PROGRESS, 0, (LPARAM)p)) free(p);
                if (echo_is_running()) {
                    wcscpy(res->message, L"Echo VR was started during the download. Close it and install again.");
                } else if (install_files(&job->echo, pkg_tmp, man_tmp, res->message, 1200)) {
                    res->ok = true;
                    res->pkg_size = file_size(job->echo.package);
                    res->man_size = file_size(job->echo.manifest);
                    res->pkg_time = file_time(job->echo.package);
                    res->man_time = file_time(job->echo.manifest);
                }
            }
        }
    }
    DeleteFileW(pkg_tmp);
    DeleteFileW(man_tmp);
    free(job);
    if (!PostMessageW(g_main, WM_APP_INSTALLED, 0, (LPARAM)res)) free(res);
    return 0;
}

static bool ask_for_echo(HWND owner, bool first_run)
{
    EchoPaths guess;
    wchar_t start[MAX_PATH] = L"";
    if (g_have_echo) wcscpy(start, g_echo.root);
    else if (echo_guess(&guess)) wcscpy(start, guess.root);
    for (;;) {
        wchar_t picked[MAX_PATH];
        if (!pick_folder(owner, first_run ? L"Where is Echo VR installed? Select the ready-at-dawn-echo-arena folder"
                                          : L"Select the ready-at-dawn-echo-arena folder",
                         start, picked, MAX_PATH))
            return false;
        EchoPaths found;
        if (echo_find(picked, &found)) {
            g_echo = found;
            g_have_echo = true;
            wcscpy(g_cfg.echo_root, found.root);
            config_save();
            update_installed();
            return true;
        }
        wchar_t msg[800];
        _snwprintf(msg, 800, L"Echo VR's map files weren't found in or around\n%ls\n\n"
                             L"Map Tester looks for %ls\\packages and \\manifests.\n"
                             L"Pick the ready-at-dawn-echo-arena folder.", picked, ECHO_DATA_REL);
        if (MessageBoxW(owner, msg, APP_NAME, MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY) return false;
        wcscpy(start, picked);
    }
}

static void do_revert(void)
{
    if (g_job.busy) return;
    if (!g_have_echo && !ask_for_echo(g_main, false)) return;
    BackupKind kind = backup_kind(&g_echo);
    if (kind == BACKUP_NONE) {
        MessageBoxW(g_main, L"No backup was found, so there is nothing to revert to.\n\n"
                            L"Map Tester looks for " MANIFEST_NAME L".modbak (a mod you had before) or "
                            MANIFEST_NAME L".bak (base Echo VR) in the manifests folder.",
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if (echo_is_running()) {
        MessageBoxW(g_main, L"Echo VR is running. Close the game first.", APP_NAME, MB_ICONWARNING);
        return;
    }
    wchar_t msg[1600];
    _snwprintf(msg, 1600, L"%ls\n\n%ls%ls",
               kind == BACKUP_MODDED
                   ? L"Revert to the modded version you had before?\n\n"
                     PACKAGE_NAME L".bak  →  " PACKAGE_NAME L"\n" MANIFEST_NAME L".modbak  →  " MANIFEST_NAME
                   : L"Revert to base Echo VR?\n\n" MANIFEST_NAME L".bak  →  " MANIFEST_NAME L"\n" PACKAGE_NAME L" is removed",
               g_installed_state == STATE_OTHER_MOD
                   ? L"⚠ The files installed right now were not installed by Map Tester and will be deleted.\n"
                   : L"The map installed now is removed (you can install it again any time).\n",
               L"");
    if (MessageBoxW(g_main, msg, APP_NAME, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    wchar_t report[600];
    if (revert_backup(&g_echo, report, 600)) {
        g_cfg.installed_id[0] = 0;
        g_cfg.installed_name[0] = 0;
        g_cfg.installed_pkg_size = g_cfg.installed_man_size = 0;
        g_cfg.installed_pkg_time = g_cfg.installed_man_time = 0;
        config_save();
        _snwprintf(g_job.status, 1200, L"✓ %ls", report);
        g_job.status_color = C_OK;
        update_installed();
        MessageBoxW(g_main, report, APP_NAME, MB_ICONINFORMATION);
    } else {
        _snwprintf(g_job.status, 1200, L"%ls", report);
        g_job.status_color = C_BAD;
        InvalidateRect(g_main, NULL, FALSE);
        MessageBoxW(g_main, report, APP_NAME, MB_ICONWARNING);
    }
}

/* ── removing your own upload ────────────────────────────────────────── */
typedef struct { bool ok; wchar_t id[64], name[256], message[400]; } RemoveResult;
typedef struct { wchar_t server[512], id[64], token[128], name[256]; } RemoveJob;

static DWORD WINAPI remove_thread(LPVOID arg)
{
    RemoveJob *job = (RemoveJob *)arg;
    RemoveResult *res = (RemoveResult *)calloc(1, sizeof(RemoveResult));
    _snwprintf(res->id, 64, L"%ls", job->id);
    _snwprintf(res->name, 256, L"%ls", job->name);
    wchar_t url[700], headers[200];
    _snwprintf(url, 700, L"%ls/maps/%ls", job->server, job->id);
    _snwprintf(headers, 200, L"X-Map-Token: %ls\r\n", job->token);
    NetResult r;
    bool ok = net_request(L"DELETE", url, headers, NULL, 0, &r);
    if (ok || r.status == 404) {
        res->ok = true;                    /* 404: already gone, which is what was wanted */
    } else if (r.status == 403) {
        _snwprintf(res->message, 400, L"The server didn't accept this PC as the uploader of this map.");
    } else {
        net_describe_error(&r, res->message, 400);
    }
    net_result_free(&r);
    SecureZeroMemory(job->token, sizeof job->token);
    free(job);
    if (!PostMessageW(g_main, WM_APP_REMOVED, 0, (LPARAM)res)) free(res);
    return 0;
}

static void do_edit(void)
{
    if (g_sel < 0 || g_sel >= g_maps.count) return;
    MapItem *m = &g_maps.items[g_sel];
    wchar_t token[128];
    if (!uploads_token(m->id, token, 128)) {
        m->owned = false;
        InvalidateRect(g_main, NULL, FALSE);
        return;
    }
    MapEditInit init = { m->id, token, m->name, m->creator, m->gametype, m->description };
    upload_open_edit(g_main, &init);
    SecureZeroMemory(token, sizeof token);
}

static void do_remove(void)
{
    if (g_sel < 0 || g_sel >= g_maps.count) return;
    MapItem *m = &g_maps.items[g_sel];
    RemoveJob *job = (RemoveJob *)calloc(1, sizeof(RemoveJob));
    if (!uploads_token(m->id, job->token, 128)) {
        free(job);
        m->owned = false;
        InvalidateRect(g_main, NULL, FALSE);
        return;
    }
    wchar_t msg[800];
    _snwprintf(msg, 800, L"Remove \"%ls\" from Map Tester?\n\nIt disappears from the list for everyone. "
                         L"People who already installed it keep their copy.\n\nThis can't be undone.", m->name);
    if (MessageBoxW(g_main, msg, APP_NAME, MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) {
        free(job);
        return;
    }
    _snwprintf(job->server, 512, L"%ls", g_cfg.server);
    _snwprintf(job->id, 64, L"%ls", m->id);
    _snwprintf(job->name, 256, L"%ls", m->name);
    HANDLE t = CreateThread(NULL, 0, remove_thread, job, 0, NULL);
    if (!t) { free(job); return; }
    CloseHandle(t);
    _snwprintf(g_job.status, 1200, L"Removing %ls…", m->name);
    g_job.status_color = C_MUTED;
    InvalidateRect(g_main, NULL, FALSE);
}

static void start_install(void)
{
    if (g_sel < 0 || g_sel >= g_maps.count || g_job.busy) return;
    if (!g_have_echo && !ask_for_echo(g_main, false)) return;
    MapItem *m = &g_maps.items[g_sel];

    wchar_t size_text[32], msg[1400];
    format_size(m->pkg_size + m->man_size, size_text, 32);
    const wchar_t *what = g_installed_state == STATE_OURS ? L"It replaces the map Map Tester installed before."
                        : g_installed_state == STATE_OTHER_MOD ? L"The mod you have installed now is backed up first (.bak / .modbak)."
                        : L"Your original Echo VR manifest is backed up first (.bak).";
    _snwprintf(msg, 1400, L"Install \"%ls\" (%ls download)?\n\n%ls\n\nEcho VR: %ls", m->name, size_text, what, g_echo.root);
    if (MessageBoxW(g_main, msg, APP_NAME, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;

    InstallJob *job = (InstallJob *)calloc(1, sizeof(InstallJob));
    _snwprintf(job->server, 512, L"%ls", g_cfg.server);
    _snwprintf(job->id, 64, L"%ls", m->id);
    _snwprintf(job->name, 256, L"%ls", m->name);
    memcpy(job->pkg_sha, m->pkg_sha, 65);
    memcpy(job->man_sha, m->man_sha, 65);
    job->pkg_size = m->pkg_size;
    job->man_size = m->man_size;
    job->echo = g_echo;
    InterlockedExchange(&g_cancel, 0);
    HANDLE t = CreateThread(NULL, 0, install_thread, job, 0, NULL);
    if (!t) { free(job); return; }
    CloseHandle(t);
    g_job.busy = true;
    _snwprintf(g_job.id, 64, L"%ls", m->id);
    memset(&g_job.progress, 0, sizeof g_job.progress);
    g_job.progress.phase = 1;
    _snwprintf(g_job.status, 1200, L"Starting download…");
    g_job.status_color = C_MUTED;
    InvalidateRect(g_main, NULL, FALSE);
}

/* ── layout & painting ───────────────────────────────────────────────── */
static void compute_layout(HWND h, HDC dc, Layout *L)
{
    RECT rc;
    GetClientRect(h, &rc);
    memset(L, 0, sizeof *L);
    SetRect(&L->header, 0, 0, rc.right, dpi(HEADER_H));
    SetRect(&L->side, 0, L->header.bottom, dpi(SIDE_W), rc.bottom);
    SetRect(&L->upload, dpi(PAD), rc.bottom - dpi(PAD) - dpi(BTN_H), dpi(SIDE_W) - dpi(PAD), rc.bottom - dpi(PAD));
    SetRect(&L->list, dpi(PAD), L->header.bottom + dpi(56), dpi(SIDE_W) - dpi(PAD), L->upload.top - dpi(16));
    SetRect(&L->refresh, dpi(SIDE_W) - dpi(PAD) - dpi(34), L->header.bottom + dpi(14), dpi(SIDE_W) - dpi(PAD), L->header.bottom + dpi(48));
    SetRect(&L->settings, rc.right - dpi(PAD) - dpi(40), dpi(16), rc.right - dpi(PAD), dpi(56));

    wchar_t pill[320];
    _snwprintf(pill, 320, L"Installed:  %ls", g_installed_label);
    int pw = text_width(dc, pill, ui_font(14, FW_SEMIBOLD)) + dpi(46);
    SetRect(&L->pill, L->settings.left - dpi(12) - pw, dpi(18), L->settings.left - dpi(12), dpi(54));

    SetRect(&L->main, dpi(SIDE_W) + dpi(28), L->header.bottom + dpi(24), rc.right - dpi(28), rc.bottom - dpi(PAD));
    SetRect(&L->retry, L->main.left, L->main.top + dpi(90), L->main.left + dpi(140), L->main.top + dpi(130));
    if (g_sel < 0) return;
    MapItem *m = &g_maps.items[g_sel];
    SetRect(&L->title, L->main.left, L->main.top, L->main.right, L->main.top + dpi(40));
    SetRect(&L->meta, L->main.left, L->title.bottom + dpi(4), L->main.right, L->title.bottom + dpi(32));
    if (m->owned)   /* right end of the meta row, only on maps uploaded from this PC */
    {
        SetRect(&L->remove, L->meta.right - dpi(132), L->meta.top - dpi(2), L->meta.right, L->meta.bottom + dpi(2));
        SetRect(&L->edit, L->remove.left - dpi(10) - dpi(122), L->remove.top, L->remove.left - dpi(10), L->remove.bottom);
    }
    RECT d = { L->main.left, L->meta.bottom + dpi(10), L->main.right, L->meta.bottom + dpi(10) };
    if (m->description[0]) {
        HGDIOBJ old = SelectObject(dc, ui_font(15, FW_NORMAL));
        DrawTextW(dc, m->description, -1, &d, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        SelectObject(dc, old);
        int max_h = dpi(66);                         /* three lines */
        if (d.bottom - d.top > max_h) d.bottom = d.top + max_h;
        d.right = L->main.right;
    }
    L->desc = d;
    SetRect(&L->status, L->main.left, L->main.bottom - dpi(24), L->main.right, L->main.bottom);
    int top = L->desc.bottom + dpi(18);
    int avail_h = L->status.top - dpi(12) - top;
    int w = L->main.right - L->main.left;
    int ih = w * 9 / 16;
    if (ih > avail_h) ih = avail_h;
    if (ih < dpi(120)) ih = dpi(120);
    SetRect(&L->image, L->main.left, top, L->main.right, top + ih);
    SetRect(&L->install, L->image.right - dpi(18) - dpi(200), L->image.bottom - dpi(18) - dpi(BTN_H),
            L->image.right - dpi(18), L->image.bottom - dpi(18));
}

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, r, b);
    DeleteObject(b);
}

static void paint_header(HDC dc, const Layout *L)
{
    gfx_fill_gradient_v(dc, L->header.left, L->header.top, L->header.right - L->header.left,
                        L->header.bottom - L->header.top, ARGB(255, RGB(26, 30, 44)), ARGB(255, RGB(17, 20, 30)));
    RECT line = { L->header.left, L->header.bottom - 1, L->header.right, L->header.bottom };
    fill(dc, &line, C_BORDER);

    /* logo: an orange and a blue tile, the two team colours */
    int lx = dpi(PAD), ly = dpi(20);
    gfx_fill_round(dc, lx, ly, dpi(20), dpi(32), dpi(6), ARGB(255, C_ACCENT));
    gfx_fill_round(dc, lx + dpi(14), ly, dpi(20), dpi(32), dpi(6), ARGB(235, C_ACCENT2));
    RECT t = { lx + dpi(48), dpi(12), dpi(600), dpi(42) };
    draw_text(dc, L"MAP TESTER", t, ui_font(22, FW_BOLD), C_TEXT, DT_SINGLELINE | DT_VCENTER);
    RECT s = { lx + dpi(48), dpi(40), dpi(600), dpi(60) };
    draw_text(dc, L"Custom maps for Echo VR", s, ui_font(13, FW_NORMAL), C_MUTED, DT_SINGLELINE | DT_VCENTER);

    /* installed pill, top right */
    const RECT *p = &L->pill;
    gfx_fill_round(dc, p->left, p->top, p->right - p->left, p->bottom - p->top, dpi(18),
                   ARGB(255, g_hot == HIT_PILL ? C_CARD_HOT : C_CARD));
    gfx_stroke_round(dc, p->left, p->top, p->right - p->left, p->bottom - p->top, dpi(18), ARGB(255, C_BORDER), 1.0f);
    COLORREF dot = g_installed_state == STATE_OURS ? C_OK : g_installed_state == STATE_OTHER_MOD ? C_ACCENT
                 : g_installed_state == STATE_BASE ? C_ACCENT2 : C_FAINT;
    int cy = (p->top + p->bottom) / 2;
    gfx_fill_ellipse(dc, p->left + dpi(16), cy - dpi(5), dpi(10), dpi(10), ARGB(255, dot));
    RECT lbl = { p->left + dpi(34), p->top, p->right - dpi(12), p->bottom };
    draw_text(dc, L"Installed:", lbl, ui_font(14, FW_NORMAL), C_MUTED, DT_SINGLELINE | DT_VCENTER);
    lbl.left += text_width(dc, L"Installed:  ", ui_font(14, FW_NORMAL));
    draw_text(dc, g_installed_label, lbl, ui_font(14, FW_SEMIBOLD), C_TEXT, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    const RECT *g = &L->settings;
    gfx_fill_round(dc, g->left, g->top, g->right - g->left, g->bottom - g->top, dpi(10),
                   ARGB(255, g_hot == HIT_SETTINGS ? C_CARD_HOT : C_CARD));
    draw_text(dc, L"\xE713", *g, icon_font(16), g_hot == HIT_SETTINGS ? C_TEXT : C_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static bool is_installed(const MapItem *m)
{
    return g_installed_state == STATE_OURS && !wcscmp(m->id, g_cfg.installed_id);
}

static void paint_card(HDC dc, const MapItem *m, RECT r, bool selected, bool hot)
{
    int w = r.right - r.left, h = r.bottom - r.top;
    gfx_fill_round(dc, r.left, r.top, w, h, dpi(14), ARGB(255, selected ? C_CARD_HOT : hot ? RGB(31, 35, 50) : C_CARD));
    if (selected) {
        gfx_stroke_round(dc, r.left, r.top, w, h, dpi(14), ARGB(255, C_ACCENT), 2.0f);
        gfx_fill_round(dc, r.left + dpi(1), r.top + dpi(18), dpi(4), h - dpi(36), dpi(2), ARGB(255, C_ACCENT));
    }
    int x = r.left + dpi(16), right = r.right - dpi(14);

    /* game type pill on the right of the title row */
    COLORREF tc = type_color(m->gametype);
    int tw = text_width(dc, m->gametype, ui_font(12, FW_SEMIBOLD)) + dpi(18);
    RECT pill = { right - tw, r.top + dpi(14), right, r.top + dpi(36) };
    gfx_fill_round(dc, pill.left, pill.top, tw, pill.bottom - pill.top, dpi(11), ARGB(48, tc));
    draw_text(dc, m->gametype, pill, ui_font(12, FW_SEMIBOLD), tc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT name = { x, r.top + dpi(11), pill.left - dpi(10), r.top + dpi(38) };
    draw_text(dc, m->name, name, ui_font(17, FW_SEMIBOLD), C_TEXT, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    wchar_t by[300];
    _snwprintf(by, 300, is_installed(m) ? L"by %ls   ·   ✓ installed" : L"by %ls", m->creator);
    RECT cr = { x, r.top + dpi(38), right, r.top + dpi(58) };
    draw_text(dc, by, cr, ui_font(13, FW_NORMAL), is_installed(m) ? C_OK : C_MUTED, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    RECT dr = { x, r.top + dpi(62), right, r.bottom - dpi(10) };
    draw_text(dc, m->description[0] ? m->description : L"No description.", dr, ui_font(13, FW_NORMAL),
              m->description[0] ? C_FAINT : RGB(78, 85, 104), DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);
}

static void paint_sidebar(HDC dc, const Layout *L)
{
    fill(dc, &L->side, C_PANEL);
    RECT edge = { L->side.right - 1, L->side.top, L->side.right, L->side.bottom };
    fill(dc, &edge, C_BORDER);

    RECT t = { dpi(PAD), L->header.bottom + dpi(14), L->refresh.left - dpi(8), L->header.bottom + dpi(48) };
    wchar_t title[64];
    if (g_list_state == LIST_READY) _snwprintf(title, 64, L"MAPS  ·  %d", g_maps.count);
    else wcscpy(title, L"MAPS");
    draw_text(dc, title, t, ui_font(13, FW_BOLD), C_MUTED, DT_SINGLELINE | DT_VCENTER);
    const RECT *rf = &L->refresh;
    if (g_hot == HIT_REFRESH)
        gfx_fill_round(dc, rf->left, rf->top, rf->right - rf->left, rf->bottom - rf->top, dpi(8), ARGB(255, C_CARD_HOT));
    draw_text(dc, L"\xE72C", *rf, icon_font(14), g_hot == HIT_REFRESH ? C_TEXT : C_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int saved = SaveDC(dc);
    IntersectClipRect(dc, L->list.left, L->list.top, L->list.right + dpi(8), L->list.bottom);
    if (g_list_state == LIST_LOADING) {
        for (int i = 0; i < 4; i++) {
            int y = L->list.top + i * (dpi(CARD_H) + dpi(CARD_GAP));
            gfx_fill_round(dc, L->list.left, y, L->list.right - L->list.left, dpi(CARD_H), dpi(14), ARGB(255 - i * 50, C_CARD));
        }
        RECT msg = { L->list.left, L->list.top, L->list.right, L->list.top + dpi(CARD_H) };
        draw_text(dc, L"Loading maps…", msg, ui_font(14, FW_NORMAL), C_MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (g_list_state == LIST_ERROR) {
        RECT msg = { L->list.left + dpi(8), L->list.top + dpi(8), L->list.right - dpi(8), L->list.top + dpi(160) };
        draw_text(dc, g_list_error, msg, ui_font(14, FW_NORMAL), C_BAD, DT_WORDBREAK | DT_EDITCONTROL);
    } else if (g_maps.count == 0) {
        RECT msg = { L->list.left, L->list.top + dpi(20), L->list.right, L->list.top + dpi(90) };
        draw_text(dc, L"No maps yet.\nBe the first — upload one below.", msg, ui_font(14, FW_NORMAL), C_MUTED,
                  DT_CENTER | DT_WORDBREAK);
    } else {
        int step = dpi(CARD_H) + dpi(CARD_GAP);
        for (int i = 0; i < g_maps.count; i++) {
            int y = L->list.top + i * step - g_scroll;
            if (y + dpi(CARD_H) < L->list.top || y > L->list.bottom) continue;
            RECT r = { L->list.left, y, L->list.right, y + dpi(CARD_H) };
            paint_card(dc, &g_maps.items[i], r, i == g_sel, g_hot == HIT_CARD && g_hot_card == i);
        }
        int content = g_maps.count * step - dpi(CARD_GAP);
        int view = L->list.bottom - L->list.top;
        if (content > view) {
            int track = view;
            int thumb = track * view / content;
            if (thumb < dpi(30)) thumb = dpi(30);
            int ty = L->list.top + (track - thumb) * g_scroll / (content - view);
            gfx_fill_round(dc, L->list.right + dpi(4), ty, dpi(4), thumb, dpi(2), ARGB(140, C_FAINT));
        }
    }
    RestoreDC(dc, saved);

    const RECT *u = &L->upload;
    bool hot = g_hot == HIT_UPLOAD;
    gfx_fill_round(dc, u->left, u->top, u->right - u->left, u->bottom - u->top, dpi(12), ARGB(255, hot ? C_CARD_HOT : C_CARD));
    gfx_stroke_round(dc, u->left, u->top, u->right - u->left, u->bottom - u->top, dpi(12), ARGB(255, hot ? C_ACCENT2 : C_BORDER), 1.5f);
    wchar_t label[64];
    _snwprintf(label, 64, L"\xE898   Upload Map");
    RECT ul = *u;
    int iw = text_width(dc, L"\xE898", icon_font(15));
    int lw = text_width(dc, L"Upload Map", ui_font(16, FW_SEMIBOLD));
    int total = iw + dpi(12) + lw;
    RECT ir = { (u->left + u->right - total) / 2, u->top, (u->left + u->right - total) / 2 + iw, u->bottom };
    draw_text(dc, L"\xE898", ir, icon_font(15), C_ACCENT2, DT_SINGLELINE | DT_VCENTER);
    ul.left = ir.right + dpi(12);
    draw_text(dc, L"Upload Map", ul, ui_font(16, FW_SEMIBOLD), C_TEXT, DT_SINGLELINE | DT_VCENTER);
}

static void install_button_text(const MapItem *m, wchar_t *out, size_t cap, bool *enabled, bool *primary)
{
    *enabled = true;
    *primary = true;
    if (g_job.busy) {
        *primary = false;
        if (wcscmp(g_job.id, m->id)) { _snwprintf(out, cap, L"Installing another map…"); *enabled = false; return; }
        const Progress *p = &g_job.progress;
        if (p->phase == 3) { _snwprintf(out, cap, L"Installing…"); *enabled = false; return; }
        int pct = p->total > 0 ? (int)(p->done * 100 / p->total) : 0;
        _snwprintf(out, cap, g_hot == HIT_INSTALL ? L"Cancel" : L"%ls  %d%%",
                   p->phase == 1 ? L"Downloading" : L"Manifest", pct);
        return;
    }
    if (is_installed(m)) { _snwprintf(out, cap, L"✓  Reinstall"); *primary = false; return; }
    _snwprintf(out, cap, L"Install Map");
}

static void paint_main(HDC dc, const Layout *L)
{
    if (g_list_state == LIST_ERROR && g_sel < 0) {
        RECT t = { L->main.left, L->main.top, L->main.right, L->main.top + dpi(40) };
        draw_text(dc, L"Can't reach the map server", t, ui_font(24, FW_SEMIBOLD), C_TEXT, DT_SINGLELINE | DT_VCENTER);
        RECT s = { L->main.left, L->main.top + dpi(42), L->main.right, L->main.top + dpi(80) };
        wchar_t msg[700];
        _snwprintf(msg, 700, L"Check your internet connection, then try again.   (%ls)", g_cfg.server);
        draw_text(dc, msg, s, ui_font(14, FW_NORMAL), C_MUTED, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        const RECT *r = &L->retry;
        gfx_fill_round(dc, r->left, r->top, r->right - r->left, r->bottom - r->top, dpi(10),
                       ARGB(255, g_hot == HIT_RETRY ? RGB(255, 160, 70) : C_ACCENT));
        draw_text(dc, L"Try again", *r, ui_font(15, FW_SEMIBOLD), RGB(20, 12, 4), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    if (g_sel < 0) {
        RECT t = L->main;
        draw_text(dc, g_list_state == LIST_LOADING ? L"" : L"Select a map to see it here",
                  t, ui_font(18, FW_NORMAL), C_FAINT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    MapItem *m = &g_maps.items[g_sel];
    draw_text(dc, m->name, L->title, ui_font(28, FW_BOLD), C_TEXT, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    /* meta row: type pill, creator, download size */
    COLORREF tc = type_color(m->gametype);
    int tw = text_width(dc, m->gametype, ui_font(13, FW_SEMIBOLD)) + dpi(22);
    RECT pill = { L->meta.left, L->meta.top + dpi(2), L->meta.left + tw, L->meta.bottom - dpi(2) };
    gfx_fill_round(dc, pill.left, pill.top, tw, pill.bottom - pill.top, dpi(12), ARGB(48, tc));
    draw_text(dc, m->gametype, pill, ui_font(13, FW_SEMIBOLD), tc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    wchar_t size[32], meta[400];
    format_size(m->pkg_size + m->man_size, size, 32);
    _snwprintf(meta, 400, L"by %ls     ·     %ls download", m->creator, size);
    RECT mr = { pill.right + dpi(14), L->meta.top, m->owned ? L->edit.left - dpi(12) : L->meta.right, L->meta.bottom };
    draw_text(dc, meta, mr, ui_font(14, FW_NORMAL), C_MUTED, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (m->owned) {
        const RECT *rb = &L->remove;
        bool hot = g_hot == HIT_REMOVE;
        int rw = rb->right - rb->left, rh = rb->bottom - rb->top;
        gfx_fill_round(dc, rb->left, rb->top, rw, rh, dpi(10), ARGB(hot ? 70 : 28, C_BAD));
        gfx_stroke_round(dc, rb->left, rb->top, rw, rh, dpi(10), ARGB(hot ? 255 : 150, C_BAD), 1.0f);
        int iw = text_width(dc, L"\xE74D", icon_font(13));
        int lw = text_width(dc, L"Remove map", ui_font(14, FW_SEMIBOLD));
        int x0 = rb->left + (rw - iw - dpi(8) - lw) / 2;
        RECT ir = { x0, rb->top, x0 + iw, rb->bottom };
        draw_text(dc, L"\xE74D", ir, icon_font(13), C_BAD, DT_SINGLELINE | DT_VCENTER);
        RECT lr = { ir.right + dpi(8), rb->top, rb->right, rb->bottom };
        draw_text(dc, L"Remove map", lr, ui_font(14, FW_SEMIBOLD), hot ? C_TEXT : RGB(255, 150, 150), DT_SINGLELINE | DT_VCENTER);

        const RECT *eb = &L->edit;
        bool ehot = g_hot == HIT_EDIT;
        int ew = eb->right - eb->left, eh = eb->bottom - eb->top;
        gfx_fill_round(dc, eb->left, eb->top, ew, eh, dpi(10), ARGB(255, ehot ? C_CARD_HOT : C_CARD));
        gfx_stroke_round(dc, eb->left, eb->top, ew, eh, dpi(10), ARGB(255, ehot ? C_ACCENT2 : C_BORDER), 1.0f);
        int eiw = text_width(dc, L"\xE70F", icon_font(13));
        int elw = text_width(dc, L"Edit details", ui_font(14, FW_SEMIBOLD));
        int ex0 = eb->left + (ew - eiw - dpi(8) - elw) / 2;
        RECT eir = { ex0, eb->top, ex0 + eiw, eb->bottom };
        draw_text(dc, L"\xE70F", eir, icon_font(13), C_ACCENT2, DT_SINGLELINE | DT_VCENTER);
        RECT elr = { eir.right + dpi(8), eb->top, eb->right, eb->bottom };
        draw_text(dc, L"Edit details", elr, ui_font(14, FW_SEMIBOLD), C_TEXT, DT_SINGLELINE | DT_VCENTER);
    }

    if (m->description[0])
        draw_text(dc, m->description, L->desc, ui_font(15, FW_NORMAL), RGB(196, 202, 218),
                  DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);

    /* the preview */
    const RECT *im = &L->image;
    int iw = im->right - im->left, ih = im->bottom - im->top;
    if (m->image_state == 2 && m->image) {
        gfx_draw_image_cover(dc, m->image, im->left, im->top, iw, ih, dpi(16));
    } else {
        gfx_fill_gradient_v(dc, im->left, im->top, iw, ih, ARGB(255, RGB(30, 36, 54)), ARGB(255, RGB(18, 21, 31)));
        RECT c = *im;
        draw_text(dc, m->image_state == 1 ? L"Loading preview…" : L"No preview image", c, ui_font(16, FW_NORMAL),
                  C_FAINT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    /* darken the bottom so the button reads on any screenshot */
    gfx_fill_gradient_v(dc, im->left, im->bottom - dpi(110), iw, dpi(110), ARGB(0, RGB(0, 0, 0)), ARGB(170, RGB(0, 0, 0)));
    gfx_stroke_round(dc, im->left, im->top, iw, ih, dpi(16), ARGB(255, C_BORDER), 1.0f);

    /* install button, bottom right of the preview */
    wchar_t label[64];
    bool enabled, primary;
    install_button_text(m, label, 64, &enabled, &primary);
    const RECT *b = &L->install;
    int bw = b->right - b->left, bh = b->bottom - b->top;
    bool hot = g_hot == HIT_INSTALL && enabled;
    if (g_job.busy && !wcscmp(g_job.id, m->id)) {
        gfx_fill_round(dc, b->left, b->top, bw, bh, dpi(12), ARGB(235, RGB(22, 26, 38)));
        const Progress *p = &g_job.progress;
        int pw = p->phase == 3 ? bw : p->total > 0 ? (int)(bw * p->done / p->total) : 0;
        if (pw > dpi(24)) gfx_fill_round(dc, b->left, b->top, pw, bh, dpi(12), ARGB(hot ? 120 : 200, C_ACCENT));
        gfx_stroke_round(dc, b->left, b->top, bw, bh, dpi(12), ARGB(255, hot ? C_BAD : C_ACCENT), 1.5f);
        draw_text(dc, label, *b, ui_font(15, FW_SEMIBOLD), C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (primary) {
        gfx_fill_round(dc, b->left, b->top, bw, bh, dpi(12), ARGB(enabled ? 255 : 120, hot ? RGB(255, 160, 70) : C_ACCENT));
        int icon_w = text_width(dc, L"\xE896", icon_font(15));
        int text_w = text_width(dc, label, ui_font(16, FW_BOLD));
        int x0 = b->left + (bw - icon_w - dpi(10) - text_w) / 2;
        RECT ir = { x0, b->top, x0 + icon_w, b->bottom };
        draw_text(dc, L"\xE896", ir, icon_font(15), RGB(20, 12, 4), DT_SINGLELINE | DT_VCENTER);
        RECT tr = { ir.right + dpi(10), b->top, b->right, b->bottom };
        draw_text(dc, label, tr, ui_font(16, FW_BOLD), RGB(20, 12, 4), DT_SINGLELINE | DT_VCENTER);
    } else {
        gfx_fill_round(dc, b->left, b->top, bw, bh, dpi(12), ARGB(225, hot ? C_CARD_HOT : C_CARD));
        gfx_stroke_round(dc, b->left, b->top, bw, bh, dpi(12), ARGB(255, enabled ? C_OK : C_BORDER), 1.5f);
        draw_text(dc, label, *b, ui_font(15, FW_SEMIBOLD), enabled ? C_TEXT : C_FAINT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (g_job.status[0])
        draw_text(dc, g_job.status, L->status, ui_font(13, FW_NORMAL), g_job.status_color,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(dc, bmp);
    fill(dc, &rc, C_BG);
    Layout L;
    compute_layout(h, dc, &L);
    paint_main(dc, &L);
    paint_sidebar(dc, &L);
    paint_header(dc, &L);
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

/* ── input ───────────────────────────────────────────────────────────── */
static void get_layout(Layout *L)
{
    HDC dc = GetDC(g_main);
    compute_layout(g_main, dc, L);
    ReleaseDC(g_main, dc);
}

static Hit hit_test(int x, int y, int *card)
{
    Layout L;
    get_layout(&L);
    *card = -1;
    if (in_rect(&L.settings, x, y)) return HIT_SETTINGS;
    if (in_rect(&L.pill, x, y)) return HIT_PILL;
    if (in_rect(&L.refresh, x, y)) return HIT_REFRESH;
    if (in_rect(&L.upload, x, y)) return HIT_UPLOAD;
    if (g_list_state == LIST_READY && in_rect(&L.list, x, y)) {
        int step = dpi(CARD_H) + dpi(CARD_GAP);
        int i = (y - L.list.top + g_scroll) / step;
        int within = (y - L.list.top + g_scroll) % step;
        if (i >= 0 && i < g_maps.count && within < dpi(CARD_H)) { *card = i; return HIT_CARD; }
    }
    if (g_list_state == LIST_ERROR && g_sel < 0 && in_rect(&L.retry, x, y)) return HIT_RETRY;
    if (g_sel >= 0 && in_rect(&L.install, x, y)) return HIT_INSTALL;
    if (g_sel >= 0 && g_maps.items[g_sel].owned && in_rect(&L.remove, x, y)) return HIT_REMOVE;
    if (g_sel >= 0 && g_maps.items[g_sel].owned && in_rect(&L.edit, x, y)) return HIT_EDIT;
    return HIT_NONE;
}

static void clamp_scroll(void)
{
    Layout L;
    get_layout(&L);
    int content = g_maps.count * (dpi(CARD_H) + dpi(CARD_GAP)) - dpi(CARD_GAP);
    int max = content - (L.list.bottom - L.list.top);
    if (g_scroll > max) g_scroll = max;
    if (g_scroll < 0) g_scroll = 0;
}

static void select_index(int i)
{
    if (i < 0 || i >= g_maps.count) return;
    g_sel = i;
    ensure_image(i);
    /* keep the selected card on screen */
    Layout L;
    get_layout(&L);
    int step = dpi(CARD_H) + dpi(CARD_GAP);
    int top = i * step, bottom = top + dpi(CARD_H), view = L.list.bottom - L.list.top;
    if (top < g_scroll) g_scroll = top;
    if (bottom > g_scroll + view) g_scroll = bottom - view;
    clamp_scroll();
    if (!g_job.busy) g_job.status[0] = 0;
    InvalidateRect(g_main, NULL, FALSE);
}

static void settings_menu(void)
{
    Layout L;
    get_layout(&L);
    POINT pt = { L.settings.right, L.settings.bottom + dpi(4) };
    ClientToScreen(g_main, &pt);
    HMENU menu = CreatePopupMenu();
    wchar_t echo[MAX_PATH + 40];
    _snwprintf(echo, MAX_PATH + 40, L"Echo VR folder…\t%ls", g_have_echo ? g_echo.root : L"not set");
    AppendMenuW(menu, MF_STRING, 1, echo);
    AppendMenuW(menu, MF_STRING, 3, g_cfg.upload_key[0] ? L"Upload key…\tset" : L"Upload key…");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    BackupKind kind = g_have_echo ? backup_kind(&g_echo) : BACKUP_NONE;
    AppendMenuW(menu, MF_STRING | (kind != BACKUP_NONE && !g_job.busy ? 0 : MF_GRAYED), 7,
                kind == BACKUP_MODDED ? L"Revert to backup…	modded version"
                : kind == BACKUP_BASE ? L"Revert to backup…	base Echo VR" : L"Revert to backup…	none found");
    AppendMenuW(menu, MF_STRING | (g_have_echo ? 0 : MF_GRAYED), 4, L"Open Echo VR map folders");
    AppendMenuW(menu, MF_STRING, 5, L"Refresh maps\tF5");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 6, L"About Map Tester");
    int cmd = TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, g_main, NULL);
    DestroyMenu(menu);
    switch (cmd) {
    case 1:
        ask_for_echo(g_main, false);
        break;
    case 3: {
        wchar_t v[256];
        wcscpy(v, g_cfg.upload_key);
        if (prompt_text(g_main, L"Upload key", L"Only needed if the server owner requires one", v, 256)) {
            wcscpy(g_cfg.upload_key, v);
            config_save();
        }
        break;
    }
    case 4:
        ShellExecuteW(g_main, L"open", g_echo.packages, NULL, NULL, SW_SHOWNORMAL);
        ShellExecuteW(g_main, L"open", g_echo.manifests, NULL, NULL, SW_SHOWNORMAL);
        break;
    case 5:
        refresh_maps();
        break;
    case 7:
        do_revert();
        break;
    case 6:
        MessageBoxW(g_main, L"Map Tester " APP_VERSION L"\n\nBrowse, install and upload custom Echo VR maps.\n\n"
                            L"Installs go to:\n  _data\\5932408047\\rad15\\win10\\packages\\48037dc70b0ecab2_3\n"
                            L"  _data\\5932408047\\rad15\\win10\\manifests\\48037dc70b0ecab2\n\n"
                            L"The files they replace are kept as .bak / .modbak next to them.",
                    APP_NAME, MB_ICONINFORMATION);
        break;
    }
}

static void on_click(int x, int y)
{
    int card;
    Hit hit = hit_test(x, y, &card);
    switch (hit) {
    case HIT_CARD: select_index(card); break;
    case HIT_UPLOAD: upload_open(g_main); break;
    case HIT_SETTINGS: settings_menu(); break;
    case HIT_REFRESH: refresh_maps(); break;
    case HIT_PILL: do_revert(); break;
    case HIT_REMOVE: do_remove(); break;
    case HIT_EDIT: do_edit(); break;
    case HIT_RETRY: refresh_maps(); break;
    case HIT_INSTALL:
        if (g_job.busy) {
            if (!wcscmp(g_job.id, g_maps.items[g_sel].id) && g_job.progress.phase != 3 &&
                MessageBoxW(g_main, L"Cancel the download? Nothing has been changed yet.", APP_NAME,
                            MB_YESNO | MB_ICONQUESTION) == IDYES)
                InterlockedExchange(&g_cancel, 1);
        } else {
            start_install();
        }
        break;
    default: break;
    }
}

/* ── window procedure ────────────────────────────────────────────────── */
#define WM_APP_FIRST_RUN (WM_APP + 20)

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_main = h;
        ui_dark_titlebar(h);
        PostMessageW(h, WM_APP_FIRST_RUN, 0, 0);
        return 0;

    case WM_APP_FIRST_RUN: {
        EchoPaths p;
        if (g_cfg.echo_root[0] && echo_paths_from_root(g_cfg.echo_root, &p)) {
            g_echo = p;
            g_have_echo = true;
        }
        update_installed();
        refresh_maps();
        if (!g_have_echo) ask_for_echo(h, true);
        return 0;
    }

    case WM_APP_MAPS: {
        wchar_t *err = (wchar_t *)wp;
        MapList *list = (MapList *)lp;
        wchar_t keep[64] = L"";
        if (g_pending_select[0]) wcscpy(keep, g_pending_select);
        else if (g_sel >= 0 && g_sel < g_maps.count) wcscpy(keep, g_maps.items[g_sel].id);
        g_pending_select[0] = 0;
        if (list) {
            free_maps(&g_maps);
            g_maps = *list;
            free(list);
            g_list_state = LIST_READY;
            for (int i = 0; i < g_maps.count; i++) {
                wchar_t token[128];
                g_maps.items[i].owned = uploads_token(g_maps.items[i].id, token, 128);
            }
            g_sel = -1;
            for (int i = 0; i < g_maps.count; i++)
                if (!wcscmp(g_maps.items[i].id, keep)) g_sel = i;
            if (g_sel < 0 && g_maps.count) g_sel = 0;
            clamp_scroll();
            if (g_sel >= 0) select_index(g_sel);
        } else {
            _snwprintf(g_list_error, 300, L"%ls", err);
            /* keep showing the maps we already had */
            g_list_state = g_maps.count ? LIST_READY : LIST_ERROR;
            if (g_maps.count) {
                _snwprintf(g_job.status, 1200, L"%ls", err);
                g_job.status_color = C_BAD;
            }
        }
        free(err);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_APP_IMAGE: {
        wchar_t *id = (wchar_t *)lp;
        for (int i = 0; i < g_maps.count; i++) {
            MapItem *m = &g_maps.items[i];
            if (wcscmp(m->id, id)) continue;
            wchar_t path[MAX_PATH];
            cache_path(m->cache_key, path, MAX_PATH);
            gfx_free_image(m->image);
            m->image = wp ? gfx_load_image(path) : NULL;
            m->image_state = m->image ? 2 : 3;
        }
        free(id);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_APP_PROGRESS: {
        Progress *p = (Progress *)lp;
        g_job.progress = *p;
        free(p);
        wchar_t a[32], b[32];
        format_size(g_job.progress.done, a, 32);
        format_size(g_job.progress.total, b, 32);
        if (g_job.progress.phase == 3)
            _snwprintf(g_job.status, 1200, L"Backing up and installing files…");
        else
            _snwprintf(g_job.status, 1200, L"Downloading %ls…  %ls of %ls",
                       g_job.progress.phase == 1 ? L"map package" : L"manifest", a, b);
        g_job.status_color = C_MUTED;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case WM_APP_REMOVED: {
        RemoveResult *r = (RemoveResult *)lp;
        if (r->ok) {
            uploads_forget(r->id);
            _snwprintf(g_job.status, 1200, L"✓ Removed %ls.", r->name);
            g_job.status_color = C_OK;
            refresh_maps();
        } else {
            _snwprintf(g_job.status, 1200, L"Couldn't remove %ls: %ls", r->name, r->message);
            g_job.status_color = C_BAD;
            InvalidateRect(h, NULL, FALSE);
            MessageBoxW(h, g_job.status, APP_NAME, MB_ICONWARNING);
        }
        free(r);
        return 0;
    }

    case WM_APP_INSTALLED: {
        InstallResult *r = (InstallResult *)lp;
        g_job.busy = false;
        if (r->ok) {
            wcscpy(g_cfg.installed_id, r->id);
            wcscpy(g_cfg.installed_name, r->name);
            g_cfg.installed_pkg_size = r->pkg_size;
            g_cfg.installed_man_size = r->man_size;
            g_cfg.installed_pkg_time = r->pkg_time;
            g_cfg.installed_man_time = r->man_time;
            config_save();
            _snwprintf(g_job.status, 1200, L"✓ %ls is installed. Start Echo VR to play it.", r->name);
            g_job.status_color = C_OK;
        } else {
            _snwprintf(g_job.status, 1200, L"%ls", r->message);
            g_job.status_color = C_BAD;
        }
        update_installed();
        if (r->ok) {
            wchar_t detail[1500];
            _snwprintf(detail, 1500, L"%ls is installed.\n\n%ls", r->name, r->message);
            MessageBoxW(h, detail, APP_NAME, MB_ICONINFORMATION);
        } else if (wcscmp(r->message, L"Downloading the package failed: Cancelled")) {
            MessageBoxW(h, r->message, APP_NAME, MB_ICONWARNING);
        }
        free(r);
        return 0;
    }

    case WM_ACTIVATEAPP:
        if (wp && g_have_echo && !g_job.busy) update_installed();
        return 0;

    case WM_MOUSEMOVE: {
        if (!g_tracking) {
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            g_tracking = true;
        }
        int card;
        Hit hit = hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &card);
        if (hit != g_hot || card != g_hot_card) {
            g_hot = hit;
            g_hot_card = card;
            InvalidateRect(h, NULL, FALSE);
        }
        SetCursor(LoadCursor(NULL, hit == HIT_NONE ? IDC_ARROW : IDC_HAND));
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = false;
        g_hot = HIT_NONE;
        g_hot_card = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) return TRUE;   /* WM_MOUSEMOVE picks the cursor */
        break;
    case WM_LBUTTONDOWN:
        g_press_hit = hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &g_press_card);
        SetCapture(h);
        return 0;
    case WM_LBUTTONUP: {
        /* Act only on a click that also STARTED here. Closing a dialog with a double-click
           delivers the second button-up to whatever window is under the cursor. */
        /* read the press BEFORE releasing capture: ReleaseCapture sends WM_CAPTURECHANGED
           synchronously, which clears it -- done the other way round, every click was dropped */
        Hit pressed = g_press_hit;
        int pressed_card = g_press_card;
        g_press_hit = HIT_NONE;
        if (GetCapture() == h) ReleaseCapture();
        int card;
        Hit hit = hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &card);
        if (hit != HIT_NONE && hit == pressed && card == pressed_card)
            on_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != h) g_press_hit = HIT_NONE;
        return 0;
    case WM_MOUSEWHEEL:
        g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) * dpi(CARD_H) / WHEEL_DELTA;
        clamp_scroll();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_DOWN && g_sel + 1 < g_maps.count) select_index(g_sel + 1);
        else if (wp == VK_UP && g_sel > 0) select_index(g_sel - 1);
        else if (wp == VK_F5) refresh_maps();
        return 0;

    case WM_SIZE:
        clamp_scroll();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = dpi(960);
        mm->ptMinTrackSize.y = dpi(620);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_CLOSE:
        if (g_job.busy && MessageBoxW(h, L"A map is still downloading. Quit anyway?", APP_NAME,
                                      MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        InterlockedExchange(&g_cancel, 1);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd;
    g_inst = inst;
    HANDLE single = CreateMutexW(NULL, TRUE, L"Local\\MapTesterSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"MapTesterMain", NULL);
        if (other) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    SetProcessDPIAware();
    HDC screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    gfx_start();
    config_load();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"MapTesterMain";
    RegisterClassExW(&wc);

    WNDCLASSEXW pc = wc;
    pc.lpfnWndProc = prompt_proc;
    pc.lpszClassName = L"MapTesterPrompt";
    RegisterClassExW(&pc);
    upload_register(inst);

    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = dpi(1180), hgt = dpi(760);
    if (w > work.right - work.left) w = work.right - work.left;
    if (hgt > work.bottom - work.top) hgt = work.bottom - work.top;
    HWND win = CreateWindowExW(0, L"MapTesterMain", APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - hgt) / 2,
                               w, hgt, NULL, NULL, inst, NULL);
    if (!win) return 1;
    ShowWindow(win, show);
    UpdateWindow(win);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        HWND up = upload_window();
        if (up && IsDialogMessageW(up, &msg)) continue;   /* Tab between the upload fields */
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    free_maps(&g_maps);
    gfx_stop();
    CoUninitialize();
    CloseHandle(single);
    return 0;
}
