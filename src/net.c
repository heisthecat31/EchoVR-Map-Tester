/* HTTP(S) over WinHTTP: small requests, streamed downloads, streamed uploads. */
#include "app.h"

#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

#define CHUNK (256 * 1024)

typedef struct {
    HINTERNET session, connect, request;
} Conn;

static void set_error(NetResult *r, const wchar_t *what)
{
    DWORD code = GetLastError();
    wchar_t sys[200] = L"";
    if (code >= 12000 && code < 13000) {
        switch (code) {
        case 12002: wcscpy(sys, L"the server took too long to answer"); break;
        case 12007: wcscpy(sys, L"the server name could not be resolved"); break;
        case 12029: wcscpy(sys, L"could not connect to the server"); break;
        case 12030: wcscpy(sys, L"the connection was dropped"); break;
        case 12175: wcscpy(sys, L"a secure (HTTPS) connection could not be made"); break;
        default: _snwprintf(sys, 200, L"network error %lu", (unsigned long)code);
        }
    } else if (code) {
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0,
                       sys, 200, NULL);
        size_t n = wcslen(sys);
        while (n && (sys[n - 1] == L'\n' || sys[n - 1] == L'\r' || sys[n - 1] == L'.')) sys[--n] = 0;
    }
    _snwprintf(r->error, 300, sys[0] ? L"%ls: %ls" : L"%ls", what, sys);
    r->error[299] = 0;
}

static void conn_close(Conn *c)
{
    if (c->request) WinHttpCloseHandle(c->request);
    if (c->connect) WinHttpCloseHandle(c->connect);
    if (c->session) WinHttpCloseHandle(c->session);
    memset(c, 0, sizeof *c);
}

/* opens session + connection + request for `url` */
static bool conn_open(Conn *c, const wchar_t *method, const wchar_t *url, NetResult *r)
{
    memset(c, 0, sizeof *c);
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        _snwprintf(r->error, 300, L"Bad server address: %ls", url);
        return false;
    }
    wchar_t ua[64];
    _snwprintf(ua, 64, L"MapTester/%ls", APP_VERSION);
    c->session = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c->session)   /* AUTOMATIC_PROXY needs Windows 8.1+ */
        c->session = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c->session) { set_error(r, L"Could not start networking"); return false; }
    /* resolve, connect, send, receive */
    WinHttpSetTimeouts(c->session, 15000, 15000, 120000, 120000);
    c->connect = WinHttpConnect(c->session, host, uc.nPort, 0);
    if (!c->connect) { set_error(r, L"Could not reach the server"); conn_close(c); return false; }
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    c->request = WinHttpOpenRequest(c->connect, method, path, NULL, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!c->request) { set_error(r, L"Could not create the request"); conn_close(c); return false; }
    return true;
}

static int status_code(HINTERNET req)
{
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    return (int)status;
}

static int64_t content_length(HINTERNET req)
{
    wchar_t text[32];
    DWORD size = sizeof text;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                             text, &size, WINHTTP_NO_HEADER_INDEX))
        return -1;
    return _wcstoi64(text, NULL, 10);
}

static bool read_body(HINTERNET req, Buf *body, NetResult *r)
{
    char tmp[8192];
    buf_append(body, "", 0);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(req, tmp, sizeof tmp, &got)) { set_error(r, L"Reading the reply failed"); return false; }
        if (!got) return true;
        buf_append(body, tmp, got);
        if (body->len > 64 * 1024 * 1024) { wcscpy(r->error, L"The reply is too large"); return false; }
    }
}

bool net_request(const wchar_t *method, const wchar_t *url, const wchar_t *headers,
                 const char *body, size_t body_len, NetResult *out)
{
    memset(out, 0, sizeof *out);
    Conn c;
    if (!conn_open(&c, method, url, out)) return false;
    bool ok = WinHttpSendRequest(c.request, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                                 headers ? (DWORD)-1L : 0, (LPVOID)body, (DWORD)body_len,
                                 (DWORD)body_len, 0) &&
              WinHttpReceiveResponse(c.request, NULL);
    if (!ok) {
        set_error(out, L"Could not reach the server");
    } else {
        out->status = status_code(c.request);
        ok = read_body(c.request, &out->body, out);
    }
    conn_close(&c);
    return ok && out->status >= 200 && out->status < 300;
}

typedef struct { BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE hash; } Sha;

static bool sha_open(Sha *s)
{
    memset(s, 0, sizeof *s);
    if (BCryptOpenAlgorithmProvider(&s->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return false;
    if (BCryptCreateHash(s->alg, &s->hash, NULL, 0, NULL, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(s->alg, 0);
        return false;
    }
    return true;
}

static void sha_close(Sha *s, char *hex)
{
    unsigned char d[32];
    if (hex) {
        hex[0] = 0;
        if (BCryptFinishHash(s->hash, d, 32, 0) == 0)
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    }
    if (s->hash) BCryptDestroyHash(s->hash);
    if (s->alg) BCryptCloseAlgorithmProvider(s->alg, 0);
}

bool net_download(const wchar_t *url, const wchar_t *dest, char *sha256_hex,
                  NetProgress cb, void *ctx, NetResult *out)
{
    memset(out, 0, sizeof *out);
    Conn c;
    if (!conn_open(&c, L"GET", url, out)) return false;
    if (!WinHttpSendRequest(c.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) ||
        !WinHttpReceiveResponse(c.request, NULL)) {
        set_error(out, L"Could not reach the server");
        conn_close(&c);
        return false;
    }
    out->status = status_code(c.request);
    if (out->status != 200) {
        read_body(c.request, &out->body, out);
        conn_close(&c);
        return false;
    }
    int64_t total = content_length(c.request);
    HANDLE f = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        set_error(out, L"Could not create the download file");
        conn_close(&c);
        return false;
    }
    Sha sha;
    bool hashing = sha_open(&sha);
    char *chunk = (char *)malloc(CHUNK);
    int64_t done = 0;
    bool ok = chunk != NULL;
    while (ok) {
        DWORD got = 0, wrote = 0;
        if (!WinHttpReadData(c.request, chunk, CHUNK, &got)) { set_error(out, L"The download was interrupted"); ok = false; break; }
        if (!got) break;
        if (!WriteFile(f, chunk, got, &wrote, NULL) || wrote != got) { set_error(out, L"Writing to disk failed"); ok = false; break; }
        if (hashing) BCryptHashData(sha.hash, (PUCHAR)chunk, got, 0);
        done += got;
        if (cb && !cb(ctx, done, total)) { wcscpy(out->error, L"Cancelled"); ok = false; break; }
    }
    if (ok && total >= 0 && done != total) {
        _snwprintf(out->error, 300, L"The download ended early (%lld of %lld bytes)",
                   (long long)done, (long long)total);
        ok = false;
    }
    free(chunk);
    CloseHandle(f);
    if (hashing) sha_close(&sha, sha256_hex);
    else if (sha256_hex) sha256_hex[0] = 0;
    if (!ok) DeleteFileW(dest);
    conn_close(&c);
    return ok;
}

bool net_upload(const wchar_t *method, const wchar_t *url, const wchar_t *headers,
                const wchar_t *src, NetProgress cb, void *ctx, NetResult *out)
{
    memset(out, 0, sizeof *out);
    int64_t total = file_size(src);
    if (total < 0) { _snwprintf(out->error, 300, L"Cannot read %ls", src); return false; }
    if (total >= 0xFFFFFFFFLL) { wcscpy(out->error, L"Files of 4 GB or more cannot be uploaded"); return false; }
    HANDLE f = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) { set_error(out, L"Could not open the file"); return false; }
    Conn c;
    if (!conn_open(&c, method, url, out)) { CloseHandle(f); return false; }
    bool ok = WinHttpSendRequest(c.request, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                                 headers ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0,
                                 (DWORD)total, 0);
    if (!ok) set_error(out, L"Could not reach the server");
    char *chunk = (char *)malloc(CHUNK);
    int64_t done = 0;
    ok = ok && chunk;
    while (ok && done < total) {
        DWORD got = 0, sent = 0;
        if (!ReadFile(f, chunk, CHUNK, &got, NULL) || !got) { set_error(out, L"Reading the file failed"); ok = false; break; }
        if (!WinHttpWriteData(c.request, chunk, got, &sent) || sent != got) {
            set_error(out, L"The upload was interrupted");
            ok = false;
            break;
        }
        done += got;
        if (cb && !cb(ctx, done, total)) { wcscpy(out->error, L"Cancelled"); ok = false; break; }
    }
    free(chunk);
    CloseHandle(f);
    if (ok) {
        if (!WinHttpReceiveResponse(c.request, NULL)) {
            set_error(out, L"The server did not answer");
            ok = false;
        } else {
            out->status = status_code(c.request);
            read_body(c.request, &out->body, out);
            ok = out->status >= 200 && out->status < 300;
        }
    }
    conn_close(&c);
    return ok;
}

void net_result_free(NetResult *r) { buf_free(&r->body); }

void net_describe_error(const NetResult *r, wchar_t *out, size_t cap)
{
    if (r->body.data && r->status) {
        JVal *j = json_parse(r->body.data);
        const char *msg = json_str(j, "error", NULL);
        if (msg) {
            wchar_t *w = utf8_to_wide(msg);
            _snwprintf(out, cap, L"%ls", w);
            out[cap - 1] = 0;
            free(w);
            json_free(j);
            return;
        }
        json_free(j);
    }
    if (r->error[0])
        _snwprintf(out, cap, L"%ls", r->error);
    else if (r->status)
        _snwprintf(out, cap, L"The server answered HTTP %d", r->status);
    else
        _snwprintf(out, cap, L"Unknown network error");
    out[cap - 1] = 0;
}
