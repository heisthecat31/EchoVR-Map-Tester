/* Upload window: details + the two map files + a preview image, streamed to the server. */
#include "app.h"

#include <commdlg.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define ID_NAME     201
#define ID_CREATOR  202
#define ID_CUSTOM   203
#define ID_DESC     204
#define ID_PKG      211
#define ID_MAN      212
#define ID_IMG      213
#define ID_UPLOAD   220
#define ID_CANCEL   221

#define W_WIDTH   640
#define W_HEIGHT  800
#define M         28          /* side margin */

static const wchar_t *TYPES[] = { L"Arena", L"Combat", L"Social", L"Other" };
#define NTYPES 4

typedef struct {
    HWND hwnd, name, creator, custom, desc, pkg_btn, man_btn, img_btn, upload, cancel;
    int type;                         /* index into TYPES, -1 none */
    int hot_type;
    int pressed_type;                 /* chip the left button went down on, -1 none */
    wchar_t files[3][MAX_PATH];       /* package, manifest, image */
    GpImage thumb;
    bool busy;
    int64_t done, total;
    wchar_t phase[160];
    wchar_t status[600];
    COLORREF status_color;
    /* editing an existing map instead of uploading a new one */
    bool edit;
    wchar_t edit_id[64], edit_token[128];
} Upload;

static Upload *g_up;

typedef struct {
    wchar_t server[512], key[256];
    wchar_t *name, *creator, *type, *desc;
    wchar_t files[3][MAX_PATH];
    int64_t sizes[3], base;           /* base = bytes of earlier parts, for overall progress */
    uint64_t last_post;
    const wchar_t *phase;
    bool edit;                        /* PATCH details (+ replace the image if files[2] is set) */
    wchar_t id[64], token[128];
} UploadJob;

typedef struct { bool ok; wchar_t message[700]; wchar_t id[64]; wchar_t token[128]; } UploadResult;
typedef struct { int64_t done, total; wchar_t phase[160]; } UpProgress;

/* ── layout ──────────────────────────────────────────────────────────── */
static int row_y(int i)   /* top of each labelled field */
{
    static const int ys[] = { 92, 170, 248, 336, 482, 558, 634 };
    return ys[i];
}

static RECT chip_rect(int i)
{
    int w = (W_WIDTH - 2 * M - 3 * 10) / NTYPES;
    RECT r = { dpi(M + i * (w + 10)), dpi(row_y(2) + 24), dpi(M + i * (w + 10) + w), dpi(row_y(2) + 64) };
    return r;
}

static void field_box(HDC dc, int y, int h)
{
    gfx_fill_round(dc, dpi(M), dpi(y), dpi(W_WIDTH - 2 * M), dpi(h), dpi(10), ARGB(255, C_FIELD));
    gfx_stroke_round(dc, dpi(M), dpi(y), dpi(W_WIDTH - 2 * M), dpi(h), dpi(10), ARGB(255, C_BORDER), 1.0f);
}

static void label(HDC dc, const wchar_t *text, const wchar_t *hint, int y)
{
    HGDIOBJ old = SelectObject(dc, ui_font(14, FW_SEMIBOLD));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_TEXT);
    RECT r = { dpi(M), dpi(y), dpi(W_WIDTH - M), dpi(y + 20) };
    DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if (hint) {
        RECT c = { 0, 0, 0, 0 };
        DrawTextW(dc, text, -1, &c, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, ui_font(13, FW_NORMAL));
        SetTextColor(dc, C_FAINT);
        r.left += c.right + dpi(10);
        DrawTextW(dc, hint, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, old);
}

static void text_at(HDC dc, const wchar_t *s, RECT r, HFONT f, COLORREF c, UINT flags)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void paint_file_row(HDC dc, Upload *u, int i, int y, int h)
{
    field_box(dc, y, h);
    const wchar_t *path = u->files[i];
    int text_left = M + 16;
    if (i == 2 && u->thumb) {
        gfx_draw_image_cover(dc, u->thumb, dpi(M + 8), dpi(y + 8), dpi((h - 16) * 16 / 9), dpi(h - 16), dpi(6));
        text_left = M + 8 + (h - 16) * 16 / 9 + 14;
    }
    RECT name = { dpi(text_left), dpi(y + 10), dpi(W_WIDTH - M - 130), dpi(y + h / 2 + 2) };
    RECT info = { dpi(text_left), dpi(y + h / 2 + 2), dpi(W_WIDTH - M - 130), dpi(y + h - 10) };
    if (!path[0]) {
        RECT r = { dpi(text_left), dpi(y), dpi(W_WIDTH - M - 130), dpi(y + h) };
        text_at(dc, u->edit ? L"Keep the current preview" : L"No file chosen", r, ui_font(14, FW_NORMAL), C_FAINT,
                DT_SINGLELINE | DT_VCENTER);
        return;
    }
    const wchar_t *base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    text_at(dc, base, name, ui_font(14, FW_SEMIBOLD), C_TEXT, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    wchar_t size[32], line[400];
    format_size(file_size(path), size, 32);
    const wchar_t *want = i == 0 ? PACKAGE_NAME : i == 1 ? MANIFEST_NAME : NULL;
    bool named = !want || !_wcsicmp(base, want);
    _snwprintf(line, 400, named ? L"%ls" : L"%ls   ·   usually named %ls", size, want ? want : L"");
    text_at(dc, line, info, ui_font(13, FW_NORMAL), named ? C_MUTED : C_ACCENT, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

static void paint(HWND h, Upload *u)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(C_PANEL);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    gfx_fill_round(dc, dpi(M), dpi(26), dpi(6), dpi(40), dpi(3), ARGB(255, C_ACCENT2));
    RECT t = { dpi(M + 18), dpi(20), rc.right - dpi(M), dpi(48) };
    text_at(dc, u->edit ? L"Edit map details" : L"Upload a map", t, ui_font(24, FW_BOLD), C_TEXT,
            DT_SINGLELINE | DT_VCENTER);
    RECT st = { dpi(M + 18), dpi(48), rc.right - dpi(M), dpi(70) };
    text_at(dc, u->edit ? L"Changes show for everyone as soon as you save."
                        : L"Share your repacked map so anyone can install it with one click.",
            st, ui_font(13, FW_NORMAL), C_MUTED, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    label(dc, L"Map name", NULL, row_y(0));
    field_box(dc, row_y(0) + 24, 42);
    label(dc, L"Creator", L"your name or tag", row_y(1));
    field_box(dc, row_y(1) + 24, 42);

    label(dc, L"Game type", NULL, row_y(2));
    for (int i = 0; i < NTYPES; i++) {
        RECT r = chip_rect(i);
        bool on = u->type == i, hot = u->hot_type == i;
        gfx_fill_round(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, dpi(10),
                       ARGB(255, on ? RGB(58, 42, 26) : hot ? C_CARD_HOT : C_FIELD));
        gfx_stroke_round(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, dpi(10),
                         ARGB(255, on ? C_ACCENT : C_BORDER), on ? 2.0f : 1.0f);
        text_at(dc, TYPES[i], r, ui_font(14, on ? FW_BOLD : FW_SEMIBOLD), on ? C_ACCENT : C_TEXT,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (u->type == NTYPES - 1) field_box(dc, row_y(2) + 72, 38);

    label(dc, L"Description", L"what it is, how to play it", row_y(3));
    field_box(dc, row_y(3) + 24, 112);

    if (u->edit) {
        /* the files are what people installed: changing them is a new map, not an edit */
        label(dc, L"Map files", NULL, row_y(4));
        int y = row_y(4) + 24, hgt = row_y(6) - 8 - y;
        gfx_fill_round(dc, dpi(M), dpi(y), dpi(W_WIDTH - 2 * M), dpi(hgt), dpi(10), ARGB(255, RGB(24, 27, 38)));
        gfx_stroke_round(dc, dpi(M), dpi(y), dpi(W_WIDTH - 2 * M), dpi(hgt), dpi(10), ARGB(255, C_BORDER), 1.0f);
        RECT r = { dpi(M + 16), dpi(y), dpi(W_WIDTH - M - 16), dpi(y + hgt) };
        text_at(dc, L"The package and manifest can't be changed after upload — people may already have them "
                    L"installed. To ship new files, upload them as a new map.",
                r, ui_font(13, FW_NORMAL), C_MUTED, DT_WORDBREAK | DT_VCENTER | DT_EDITCONTROL);
    } else {
        label(dc, L"Map package", PACKAGE_NAME L"  →  packages", row_y(4));
        paint_file_row(dc, u, 0, row_y(4) + 24, 50);
        label(dc, L"Manifest", MANIFEST_NAME L"  →  manifests", row_y(5));
        paint_file_row(dc, u, 1, row_y(5) + 24, 50);
    }
    label(dc, L"Preview image", u->edit ? L"optional: choose one to replace the current preview"
                                        : L"PNG or JPG screenshot from in game", row_y(6));
    paint_file_row(dc, u, 2, row_y(6) + 24, 64);

    /* progress + status above the buttons */
    int py = W_HEIGHT - 96;
    if (u->busy) {
        int full = W_WIDTH - 2 * M;
        gfx_fill_round(dc, dpi(M), dpi(py), dpi(full), dpi(6), dpi(3), ARGB(255, C_FIELD));
        int w = u->total > 0 ? (int)(dpi(full) * u->done / u->total) : 0;
        if (w > dpi(6)) gfx_fill_round(dc, dpi(M), dpi(py), w, dpi(6), dpi(3), ARGB(255, C_ACCENT2));
        wchar_t a[32], b[32], line[300];
        format_size(u->done, a, 32);
        format_size(u->total, b, 32);
        _snwprintf(line, 300, L"%ls   %ls of %ls", u->phase, a, b);
        RECT r = { dpi(M), dpi(py + 10), rc.right - dpi(M), dpi(py + 30) };
        text_at(dc, line, r, ui_font(13, FW_NORMAL), C_MUTED, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    } else if (u->status[0]) {
        RECT r = { dpi(M), dpi(py - 4), rc.right - dpi(M), dpi(py + 30) };
        text_at(dc, u->status, r, ui_font(13, FW_NORMAL), u->status_color, DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);
    }

    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

/* ── controls ────────────────────────────────────────────────────────── */
static HWND edit(HWND parent, int id, int x, int y, int w, int h, DWORD style, int limit, const wchar_t *cue)
{
    HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                             dpi(x), dpi(y), dpi(w), dpi(h), parent, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(e, WM_SETFONT, (WPARAM)ui_font(15, FW_NORMAL), TRUE);
    SendMessageW(e, EM_LIMITTEXT, limit, 0);
    if (cue) SendMessageW(e, 0x1501 /* EM_SETCUEBANNER */, TRUE, (LPARAM)cue);
    return e;
}

static HWND button(HWND parent, int id, const wchar_t *text, int x, int y, int w, int h)
{
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             dpi(x), dpi(y), dpi(w), dpi(h), parent, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)ui_font(15, FW_SEMIBOLD), TRUE);
    return b;
}

static void set_busy(Upload *u, bool busy)
{
    u->busy = busy;
    EnableWindow(u->name, !busy);
    EnableWindow(u->creator, !busy);
    EnableWindow(u->custom, !busy);
    EnableWindow(u->desc, !busy);
    EnableWindow(u->pkg_btn, !busy);
    EnableWindow(u->man_btn, !busy);
    EnableWindow(u->img_btn, !busy);
    EnableWindow(u->upload, !busy);
    SetWindowTextW(u->upload, u->edit ? (busy ? L"Saving…" : L"Save changes") : (busy ? L"Uploading…" : L"Upload Map"));
    InvalidateRect(u->hwnd, NULL, FALSE);
}

static void choose_file(Upload *u, int which)
{
    wchar_t path[MAX_PATH];
    wcscpy(path, u->files[which]);
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = u->hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (which == 0) {
        ofn.lpstrTitle = L"Choose the map package (" PACKAGE_NAME L")";
        ofn.lpstrFilter = L"Map package (" PACKAGE_NAME L")\0" PACKAGE_NAME L"*\0All files\0*.*\0";
    } else if (which == 1) {
        ofn.lpstrTitle = L"Choose the manifest (" MANIFEST_NAME L")";
        ofn.lpstrFilter = L"Manifest (" MANIFEST_NAME L")\0" MANIFEST_NAME L"*\0All files\0*.*\0";
    } else {
        ofn.lpstrTitle = L"Choose a preview image";
        ofn.lpstrFilter = L"Images (PNG, JPG)\0*.png;*.jpg;*.jpeg\0";
    }
    if (!GetOpenFileNameW(&ofn)) return;
    if (which == 2) {
        GpImage img = gfx_load_image(path);
        if (!img) {
            MessageBoxW(u->hwnd, L"That image couldn't be opened. Use a PNG or JPG.", APP_NAME, MB_ICONWARNING);
            return;
        }
        gfx_free_image(u->thumb);
        u->thumb = img;
    }
    wcscpy(u->files[which], path);
    u->status[0] = 0;
    InvalidateRect(u->hwnd, NULL, FALSE);
}

/* ── the upload itself ───────────────────────────────────────────────── */
static bool up_progress(void *ctx, int64_t done, int64_t total)
{
    (void)total;
    UploadJob *job = (UploadJob *)ctx;
    uint64_t now = GetTickCount64();
    if (now - job->last_post < 80) return true;
    job->last_post = now;
    UpProgress *p = (UpProgress *)calloc(1, sizeof(UpProgress));
    p->done = job->base + done;
    p->total = job->sizes[0] + job->sizes[1] + job->sizes[2];
    _snwprintf(p->phase, 160, L"%ls", job->phase);
    HWND target = g_up ? g_up->hwnd : NULL;
    if (!target || !PostMessageW(target, WM_APP_PROGRESS, 0, (LPARAM)p)) free(p);
    return true;
}

static void header_lines(wchar_t *out, size_t cap, const wchar_t *type, const wchar_t *token, const wchar_t *key)
{
    _snwprintf(out, cap, L"Content-Type: %ls\r\n", type);
    if (token && *token) {
        size_t n = wcslen(out);
        _snwprintf(out + n, cap - n, L"X-Map-Token: %ls\r\n", token);
    }
    if (key && *key) {
        size_t n = wcslen(out);
        _snwprintf(out + n, cap - n, L"X-Upload-Key: %ls\r\n", key);
    }
    out[cap - 1] = 0;
}

/* edit mode: PATCH the details, then replace the preview if a new one was chosen */
static DWORD WINAPI edit_thread(LPVOID arg)
{
    UploadJob *job = (UploadJob *)arg;
    UploadResult *res = (UploadResult *)calloc(1, sizeof(UploadResult));
    wchar_t url[700], headers[600], why[400] = L"";
    NetResult r;

    Buf body = {0};
    buf_appendz(&body, "{\"name\":");
    buf_json_string(&body, job->name);
    buf_appendz(&body, ",\"creator\":");
    buf_json_string(&body, job->creator);
    buf_appendz(&body, ",\"gametype\":");
    buf_json_string(&body, job->type);
    buf_appendz(&body, ",\"description\":");
    buf_json_string(&body, job->desc);
    buf_appendz(&body, "}");
    _snwprintf(url, 700, L"%ls/maps/%ls", job->server, job->id);
    header_lines(headers, 600, L"application/json", job->token, NULL);
    job->phase = L"Saving details…";
    up_progress(job, 0, 0);
    bool ok = net_request(L"PATCH", url, headers, body.data, body.len, &r);
    buf_free(&body);
    if (!ok) net_describe_error(&r, why, 400);
    net_result_free(&r);

    if (ok && job->files[2][0]) {
        job->phase = L"Uploading new preview image…";
        job->last_post = 0;
        _snwprintf(url, 700, L"%ls/maps/%ls/image", job->server, job->id);
        header_lines(headers, 600, L"application/octet-stream", job->token, NULL);
        ok = net_upload(L"PUT", url, headers, job->files[2], up_progress, job, &r);
        if (!ok) {
            wchar_t e[300];
            net_describe_error(&r, e, 300);
            _snwprintf(why, 400, L"the details were saved, but the new image failed: %ls", e);
        }
        net_result_free(&r);
    }

    res->ok = ok;
    _snwprintf(res->id, 64, L"%ls", job->id);
    if (ok) _snwprintf(res->message, 700, L"“%ls” is updated.", job->name);
    else _snwprintf(res->message, 700, L"Saving failed — %ls", why);
    SecureZeroMemory(job->token, sizeof job->token);
    free(job->name); free(job->creator); free(job->type); free(job->desc);
    free(job);
    HWND target = g_up ? g_up->hwnd : NULL;
    if (!target || !PostMessageW(target, WM_APP_UPLOADED, 1, (LPARAM)res)) free(res);
    return 0;
}

static DWORD WINAPI upload_thread(LPVOID arg)
{
    UploadJob *job = (UploadJob *)arg;
    UploadResult *res = (UploadResult *)calloc(1, sizeof(UploadResult));
    wchar_t url[700], headers[600], why[400], id[64] = L"", token[128] = L"";
    NetResult r;

    /* 1. create the draft */
    Buf body = {0};
    buf_appendz(&body, "{\"name\":");
    buf_json_string(&body, job->name);
    buf_appendz(&body, ",\"creator\":");
    buf_json_string(&body, job->creator);
    buf_appendz(&body, ",\"gametype\":");
    buf_json_string(&body, job->type);
    buf_appendz(&body, ",\"description\":");
    buf_json_string(&body, job->desc);
    buf_appendz(&body, "}");
    _snwprintf(url, 700, L"%ls/maps", job->server);
    header_lines(headers, 600, L"application/json", NULL, job->key);
    job->phase = L"Creating the map…";
    up_progress(job, 0, 0);
    bool ok = net_request(L"POST", url, headers, body.data, body.len, &r);
    buf_free(&body);
    if (ok) {
        JVal *j = json_parse(r.body.data);
        const char *jid = json_str(j, "id", ""), *jtok = json_str(j, "token", "");
        wchar_t *wid = utf8_to_wide(jid), *wtok = utf8_to_wide(jtok);
        _snwprintf(id, 64, L"%ls", wid);
        _snwprintf(token, 128, L"%ls", wtok);
        free(wid);
        free(wtok);
        json_free(j);
        ok = id[0] && token[0];
        if (!ok) wcscpy(why, L"the server's reply was not understood");
    } else {
        net_describe_error(&r, why, 400);
    }
    net_result_free(&r);

    /* 2. the three files */
    static const wchar_t *parts[3] = { L"package", L"manifest", L"image" };
    static const wchar_t *phases[3] = { L"Uploading map package…", L"Uploading manifest…", L"Uploading preview image…" };
    for (int i = 0; ok && i < 3; i++) {
        job->phase = phases[i];
        job->last_post = 0;
        _snwprintf(url, 700, L"%ls/maps/%ls/%ls", job->server, id, parts[i]);
        header_lines(headers, 600, L"application/octet-stream", token, NULL);
        ok = net_upload(L"PUT", url, headers, job->files[i], up_progress, job, &r);
        if (!ok) {
            wchar_t e[300];
            net_describe_error(&r, e, 300);
            _snwprintf(why, 400, L"%ls: %ls", parts[i], e);
        }
        net_result_free(&r);
        job->base += job->sizes[i];
    }

    /* 3. publish */
    if (ok) {
        job->phase = L"Publishing…";
        job->last_post = 0;
        up_progress(job, 0, 0);
        _snwprintf(url, 700, L"%ls/maps/%ls/publish", job->server, id);
        header_lines(headers, 600, L"application/json", token, NULL);
        ok = net_request(L"POST", url, headers, "{}", 2, &r);
        if (!ok) net_describe_error(&r, why, 400);
        net_result_free(&r);
    }

    if (ok) {
        res->ok = true;
        _snwprintf(res->id, 64, L"%ls", id);
        _snwprintf(res->token, 128, L"%ls", token);
        _snwprintf(res->message, 700, L"“%ls” is live.\n\nYou can remove it any time: select it and click Remove map.", job->name);
    } else {
        if (id[0]) {   /* don't leave a half-uploaded draft behind */
            _snwprintf(url, 700, L"%ls/maps/%ls", job->server, id);
            header_lines(headers, 600, L"application/json", token, NULL);
            net_request(L"DELETE", url, headers, NULL, 0, &r);
            net_result_free(&r);
        }
        _snwprintf(res->message, 700, L"Upload failed — %ls", why);
    }
    free(job->name); free(job->creator); free(job->type); free(job->desc);
    free(job);
    HWND target = g_up ? g_up->hwnd : NULL;
    if (!target || !PostMessageW(target, WM_APP_UPLOADED, 0, (LPARAM)res)) free(res);
    return 0;
}

static wchar_t *get_text(HWND e)
{
    int n = GetWindowTextLengthW(e);
    wchar_t *s = (wchar_t *)calloc(n + 1, sizeof(wchar_t));
    GetWindowTextW(e, s, n + 1);
    /* trim */
    wchar_t *start = s;
    while (*start == L' ' || *start == L'\t' || *start == L'\r' || *start == L'\n') start++;
    size_t len = wcslen(start);
    while (len && (start[len - 1] == L' ' || start[len - 1] == L'\t' || start[len - 1] == L'\r' || start[len - 1] == L'\n'))
        start[--len] = 0;
    memmove(s, start, (len + 1) * sizeof(wchar_t));
    return s;
}

static void fail_field(Upload *u, const wchar_t *msg, HWND focus)
{
    _snwprintf(u->status, 600, L"%ls", msg);
    u->status_color = C_BAD;
    InvalidateRect(u->hwnd, NULL, FALSE);
    if (focus) SetFocus(focus);
}

static void start_upload(Upload *u)
{
    if (u->busy) return;
    wchar_t *name = get_text(u->name), *creator = get_text(u->creator), *desc = get_text(u->desc);
    wchar_t *type = u->type == NTYPES - 1 ? get_text(u->custom) : wcsdup_safe(u->type >= 0 ? TYPES[u->type] : L"");
    const wchar_t *problem = NULL;
    HWND focus = NULL;
    if (!name[0]) { problem = L"Give the map a name."; focus = u->name; }
    else if (!creator[0]) { problem = L"Add who made the map."; focus = u->creator; }
    else if (u->type < 0) problem = L"Pick a game type.";
    else if (!type[0]) { problem = L"Type the custom game type."; focus = u->custom; }
    else if (!u->edit && (!u->files[0][0] || !file_exists(u->files[0]))) problem = L"Choose the map package file (" PACKAGE_NAME L").";
    else if (!u->edit && (!u->files[1][0] || !file_exists(u->files[1]))) problem = L"Choose the manifest file (" MANIFEST_NAME L").";
    else if (!u->edit && (!u->files[2][0] || !file_exists(u->files[2]))) problem = L"Choose a preview image.";
    else if (u->edit && u->files[2][0] && !file_exists(u->files[2])) problem = L"The new preview image can't be found.";
    if (problem) {
        fail_field(u, problem, focus);
        free(name); free(creator); free(desc); free(type);
        return;
    }
    const wchar_t *pb = wcsrchr(u->files[0], L'\\'), *mb = wcsrchr(u->files[1], L'\\');
    pb = pb ? pb + 1 : u->files[0];
    mb = mb ? mb + 1 : u->files[1];
    if (!u->edit && (_wcsicmp(pb, PACKAGE_NAME) || _wcsicmp(mb, MANIFEST_NAME))) {
        wchar_t msg[700];
        _snwprintf(msg, 700, L"The files are usually named\n  %ls\n  %ls\n\nYou picked\n  %ls\n  %ls\n\n"
                             L"They are installed under the right names either way. Upload anyway?",
                   PACKAGE_NAME, MANIFEST_NAME, pb, mb);
        if (MessageBoxW(u->hwnd, msg, APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) {
            free(name); free(creator); free(desc); free(type);
            return;
        }
    }

    UploadJob *job = (UploadJob *)calloc(1, sizeof(UploadJob));
    _snwprintf(job->server, 512, L"%ls", g_cfg.server);
    _snwprintf(job->key, 256, L"%ls", g_cfg.upload_key);
    job->name = name; job->creator = creator; job->type = type; job->desc = desc;
    for (int i = 0; i < 3; i++) {
        if (u->edit && i < 2) continue;             /* an edit never sends map files */
        wcscpy(job->files[i], u->files[i]);
        job->sizes[i] = u->files[i][0] ? file_size(u->files[i]) : 0;
        if (job->sizes[i] < 0) job->sizes[i] = 0;
    }
    job->edit = u->edit;
    wcscpy(job->id, u->edit_id);
    wcscpy(job->token, u->edit_token);
    u->done = 0;
    u->total = job->sizes[0] + job->sizes[1] + job->sizes[2];
    wcscpy(u->phase, L"Starting…");
    u->status[0] = 0;
    HANDLE t = CreateThread(NULL, 0, u->edit ? edit_thread : upload_thread, job, 0, NULL);
    if (!t) {
        free(name); free(creator); free(desc); free(type); free(job);
        fail_field(u, u->edit ? L"Could not start saving." : L"Could not start the upload.", NULL);
        return;
    }
    CloseHandle(t);
    set_busy(u, true);
}

/* ── window ──────────────────────────────────────────────────────────── */
static LRESULT CALLBACK upload_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Upload *u = (Upload *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        u = (Upload *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)u);
        u->hwnd = h;
        ui_dark_titlebar(h);
        int fw = W_WIDTH - 2 * M - 28;
        u->name = edit(h, ID_NAME, M + 14, row_y(0) + 35, fw, 22, ES_AUTOHSCROLL, 64, L"e.g. Neon Arena");
        u->creator = edit(h, ID_CREATOR, M + 14, row_y(1) + 35, fw, 22, ES_AUTOHSCROLL, 64, L"e.g. heisthecat");
        u->custom = edit(h, ID_CUSTOM, M + 14, row_y(2) + 81, fw, 22, ES_AUTOHSCROLL, 32, L"Custom game type");
        ShowWindow(u->custom, SW_HIDE);
        u->desc = edit(h, ID_DESC, M + 14, row_y(3) + 32, fw, 96,
                       ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 2000,
                       NULL);
        SetWindowTheme(u->desc, L"DarkMode_Explorer", NULL);
        u->pkg_btn = button(h, ID_PKG, L"Browse…", W_WIDTH - M - 118, row_y(4) + 32, 106, 34);
        u->man_btn = button(h, ID_MAN, L"Browse…", W_WIDTH - M - 118, row_y(5) + 32, 106, 34);
        u->img_btn = button(h, ID_IMG, L"Browse…", W_WIDTH - M - 118, row_y(6) + 39, 106, 34);
        u->cancel = button(h, ID_CANCEL, L"Close", W_WIDTH - M - 150 - 12 - 170, W_HEIGHT - 54, 150, 44);
        u->upload = button(h, ID_UPLOAD, L"Upload Map", W_WIDTH - M - 170, W_HEIGHT - 54, 170, 44);
        SetFocus(u->name);
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        static HBRUSH field;
        if (!field) field = CreateSolidBrush(C_FIELD);
        SetTextColor((HDC)wp, IsWindowEnabled((HWND)lp) ? C_TEXT : C_MUTED);
        SetBkColor((HDC)wp, C_FIELD);
        return (LRESULT)field;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *d = (const DRAWITEMSTRUCT *)lp;
        ui_draw_button(d, d->CtlID == ID_UPLOAD);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_PKG: choose_file(u, 0); break;
        case ID_MAN: choose_file(u, 1); break;
        case ID_IMG: choose_file(u, 2); break;
        case ID_UPLOAD: start_upload(u); break;
        case ID_CANCEL: SendMessageW(h, WM_CLOSE, 0, 0); break;
        }
        return 0;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), hot = -1;
        for (int i = 0; i < NTYPES; i++) {
            RECT r = chip_rect(i);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) hot = i;
        }
        if (hot != u->hot_type) {
            u->hot_type = hot;
            InvalidateRect(h, NULL, FALSE);
        }
        SetCursor(LoadCursor(NULL, hot >= 0 && !u->busy ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (HWND)wp == h) return TRUE;
        break;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        u->pressed_type = -1;
        for (int i = 0; i < NTYPES; i++) {
            RECT r = chip_rect(i);
            if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) u->pressed_type = i;
        }
        SetCapture(h);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != h) u->pressed_type = -1;
        return 0;
    case WM_LBUTTONUP: {
        /* ⛔ Only a click that also STARTED on the chip. Double-clicking a file in the Browse
           dialog closes it on the second press, and that press's button-up then arrives here
           -- it landed on "Social" and silently replaced the type that had been picked.
           The press is read before ReleaseCapture, whose WM_CAPTURECHANGED clears it. */
        int pressed = u->pressed_type;
        u->pressed_type = -1;
        if (GetCapture() == h) ReleaseCapture();
        if (u->busy || pressed < 0) return 0;
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        for (int i = 0; i < NTYPES; i++) {
            RECT r = chip_rect(i);
            if (i == pressed && x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
                u->type = i;
                ShowWindow(u->custom, i == NTYPES - 1 ? SW_SHOW : SW_HIDE);
                if (i == NTYPES - 1) SetFocus(u->custom);
                InvalidateRect(h, NULL, FALSE);
            }
        }
        return 0;
    }
    case WM_APP_PROGRESS: {
        UpProgress *p = (UpProgress *)lp;
        u->done = p->done;
        u->total = p->total;
        wcscpy(u->phase, p->phase);
        free(p);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_APP_UPLOADED: {
        UploadResult *r = (UploadResult *)lp;
        set_busy(u, false);
        if (r->ok) {
            wchar_t id[64];
            wcscpy(id, r->id);
            if (!wp) uploads_remember(r->id, r->token);   /* a new upload: lets this PC edit/remove it later */
            MessageBoxW(h, r->message, APP_NAME, MB_ICONINFORMATION);
            free(r);
            DestroyWindow(h);
            select_map_by_id(id);
            return 0;
        }
        _snwprintf(u->status, 600, L"%ls", r->message);
        u->status_color = C_BAD;
        free(r);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(h, u);
        return 0;
    case WM_CLOSE:
        if (u->busy) {
            MessageBoxW(h, L"The upload is still running. Wait for it to finish.", APP_NAME, MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        EnableWindow(g_main, TRUE);
        SetActiveWindow(g_main);
        gfx_free_image(u->thumb);
        free(u);
        g_up = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool upload_register(HINSTANCE inst)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = upload_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"MapTesterUpload";
    return RegisterClassExW(&wc) != 0;
}

HWND upload_window(void) { return g_up ? g_up->hwnd : NULL; }

static HWND open_window(HWND owner, Upload *u)
{
    if (g_up) {
        SetForegroundWindow(g_up->hwnd);
        free(u);
        return NULL;
    }
    u->hot_type = -1;
    u->pressed_type = -1;
    g_up = u;
    RECT o;
    GetWindowRect(owner, &o);
    RECT adj = { 0, 0, dpi(W_WIDTH), dpi(W_HEIGHT) };
    AdjustWindowRectEx(&adj, WS_CAPTION | WS_SYSMENU, FALSE, 0);
    int ww = adj.right - adj.left, wh = adj.bottom - adj.top;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = o.left + (o.right - o.left - ww) / 2, y = o.top + (o.bottom - o.top - wh) / 2;
    if (y < work.top) y = work.top;
    HWND h = CreateWindowExW(0, L"MapTesterUpload", u->edit ? L"Edit Map — " APP_NAME : L"Upload Map — " APP_NAME,
                             WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, x, y, ww, wh, owner, NULL, g_inst, u);
    if (!h) {
        free(u);
        g_up = NULL;
        return NULL;
    }
    return h;
}

void upload_open(HWND owner)
{
    Upload *u = (Upload *)calloc(1, sizeof(Upload));
    u->type = -1;
    HWND h = open_window(owner, u);
    if (!h) return;
    EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
}

void upload_open_edit(HWND owner, const MapEditInit *init)
{
    Upload *u = (Upload *)calloc(1, sizeof(Upload));
    u->type = -1;
    u->edit = true;
    _snwprintf(u->edit_id, 64, L"%ls", init->id);
    _snwprintf(u->edit_token, 128, L"%ls", init->token);
    HWND h = open_window(owner, u);
    if (!h) return;
    SetWindowTextW(u->name, init->name);
    SetWindowTextW(u->creator, init->creator);
    /* the server keeps LF; an edit control only breaks lines on CRLF */
    size_t n = wcslen(init->description);
    wchar_t *desc = (wchar_t *)calloc(n * 2 + 1, sizeof(wchar_t));
    for (size_t i = 0, k = 0; i < n; i++) {
        if (init->description[i] == L'\n' && (i == 0 || init->description[i - 1] != L'\r')) desc[k++] = L'\r';
        desc[k++] = init->description[i];
    }
    SetWindowTextW(u->desc, desc);
    free(desc);
    for (int i = 0; i < NTYPES - 1; i++)
        if (!_wcsicmp(init->gametype, TYPES[i])) u->type = i;
    if (u->type < 0 && init->gametype[0]) {
        u->type = NTYPES - 1;
        SetWindowTextW(u->custom, init->gametype);
        ShowWindow(u->custom, SW_SHOW);
    }
    ShowWindow(u->pkg_btn, SW_HIDE);
    ShowWindow(u->man_btn, SW_HIDE);
    SetWindowTextW(u->upload, L"Save changes");
    EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
}
