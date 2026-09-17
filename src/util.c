#include "app.h"

#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

wchar_t *utf8_to_wide(const char *s)
{
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (n > 0 ? n : 1));
    if (!w) abort();
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
    return w;
}

char *wide_to_utf8(const wchar_t *s)
{
    if (!s) s = L"";
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *u = (char *)malloc(n > 0 ? n : 1);
    if (!u) abort();
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, s, -1, u, n, NULL, NULL)) u[0] = 0;
    return u;
}

wchar_t *wcsdup_safe(const wchar_t *s)
{
    size_t n = wcslen(s ? s : L"") + 1;
    wchar_t *d = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (!d) abort();
    memcpy(d, s ? s : L"", n * sizeof(wchar_t));
    return d;
}

void buf_append(Buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *d = (char *)realloc(b->data, cap);
        if (!d) abort();
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

void buf_appendz(Buf *b, const char *s) { buf_append(b, s, strlen(s)); }

void buf_json_string(Buf *b, const wchar_t *s)
{
    char *u = wide_to_utf8(s);
    buf_append(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)u; *p; p++) {
        char esc[8];
        switch (*p) {
        case '"':  buf_append(b, "\\\"", 2); break;
        case '\\': buf_append(b, "\\\\", 2); break;
        case '\n': buf_append(b, "\\n", 2); break;
        case '\r': break;                          /* edit controls give CRLF */
        case '\t': buf_append(b, "\\t", 2); break;
        default:
            if (*p < 0x20) {
                snprintf(esc, sizeof esc, "\\u%04x", *p);
                buf_appendz(b, esc);
            } else {
                buf_append(b, (const char *)p, 1);
            }
        }
    }
    buf_append(b, "\"", 1);
    free(u);
}

void buf_free(Buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

bool file_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t file_size(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d) ||
        (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return -1;
    return ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

uint64_t file_time(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) return 0;
    return ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
}

void path_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b)
{
    size_t n = wcslen(a);
    bool slash = n && (a[n - 1] == L'\\' || a[n - 1] == L'/');
    _snwprintf(out, cap, slash ? L"%ls%ls" : L"%ls\\%ls", a, b);
    out[cap - 1] = 0;
}

void format_size(int64_t bytes, wchar_t *out, size_t cap)
{
    if (bytes < 0) bytes = 0;
    if (bytes >= (int64_t)1 << 30)
        _snwprintf(out, cap, L"%.2f GB", bytes / 1073741824.0);
    else if (bytes >= 1 << 20)
        _snwprintf(out, cap, L"%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1 << 10)
        _snwprintf(out, cap, L"%.0f KB", bytes / 1024.0);
    else
        _snwprintf(out, cap, L"%lld B", (long long)bytes);
    out[cap - 1] = 0;
}

bool local_data_dir(const wchar_t *sub, wchar_t *out, size_t cap)
{
    wchar_t base[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, base)))
        return false;
    wchar_t app[MAX_PATH];
    path_join(app, MAX_PATH, base, L"MapTester");
    CreateDirectoryW(app, NULL);
    if (sub && *sub) {
        path_join(out, cap, app, sub);
        CreateDirectoryW(out, NULL);
    } else {
        _snwprintf(out, cap, L"%ls", app);
        out[cap - 1] = 0;
    }
    return dir_exists(out);
}
