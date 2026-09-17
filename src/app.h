/*
 * Map Tester — browse, install and upload custom Echo VR maps.
 *
 * A map is two repacked files:
 *   <echo>\_data\5932408047\rad15\win10\packages\48037dc70b0ecab2_3
 *   <echo>\_data\5932408047\rad15\win10\manifests\48037dc70b0ecab2
 * hosted by the Spark-Bot server's /maps routes (Spark-Bot/maps.js).
 */
#ifndef MAPTESTER_APP_H
#define MAPTESTER_APP_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define APP_NAME        L"Map Tester"
#define APP_VERSION     L"1.0.0"
/* Where Spark-Bot runs. Every copy of the app uses this server; it is not a setting. */
#ifndef DEFAULT_SERVER   /* a test build may point elsewhere with -DDEFAULT_SERVER=... */
#define DEFAULT_SERVER  L"https://spark-bot-production-0645.up.railway.app"
#endif

#define ECHO_DATA_REL   L"_data\\5932408047\\rad15\\win10"
#define PACKAGE_NAME    L"48037dc70b0ecab2_3"
#define MANIFEST_NAME   L"48037dc70b0ecab2"

/* messages worker threads post back to the windows */
#define WM_APP_MAPS       (WM_APP + 1)   /* lParam: MapList* (owned by receiver) or NULL; wParam: error wchar_t* */
#define WM_APP_IMAGE      (WM_APP + 2)   /* lParam: wchar_t* map id (owned) ; wParam: ok */
#define WM_APP_PROGRESS   (WM_APP + 3)   /* lParam: Progress* (owned) */
#define WM_APP_INSTALLED  (WM_APP + 4)   /* lParam: InstallResult* (owned) */
#define WM_APP_UPLOADED   (WM_APP + 5)   /* lParam: UploadResult* (owned) */
#define WM_APP_REMOVED    (WM_APP + 6)   /* lParam: RemoveResult* (owned) */

/* ── util.c ─────────────────────────────────────────────────────────────── */
typedef struct { char *data; size_t len, cap; } Buf;

wchar_t *utf8_to_wide(const char *s);           /* malloc'd, never NULL */
char    *wide_to_utf8(const wchar_t *s);         /* malloc'd, never NULL */
void     buf_append(Buf *b, const char *s, size_t n);
void     buf_appendz(Buf *b, const char *s);
void     buf_json_string(Buf *b, const wchar_t *s); /* appends "escaped" */
void     buf_free(Buf *b);
bool     file_exists(const wchar_t *path);
bool     dir_exists(const wchar_t *path);
int64_t  file_size(const wchar_t *path);         /* -1 if missing */
uint64_t file_time(const wchar_t *path);         /* last write, 0 if missing */
void     path_join(wchar_t *out, size_t cap, const wchar_t *a, const wchar_t *b);
void     format_size(int64_t bytes, wchar_t *out, size_t cap);
bool     local_data_dir(const wchar_t *sub, wchar_t *out, size_t cap); /* %LOCALAPPDATA%\MapTester\sub */
wchar_t *wcsdup_safe(const wchar_t *s);

/* ── json.c ─────────────────────────────────────────────────────────────── */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;
typedef struct JVal {
    JType type;
    char *key;            /* member name inside an object */
    char *str;            /* J_STR, UTF-8 */
    double num;
    bool b;
    struct JVal *child;   /* first element / member */
    struct JVal *next;
} JVal;

JVal       *json_parse(const char *text);        /* NULL on malformed input */
void        json_free(JVal *v);
JVal       *json_get(const JVal *obj, const char *key);
const char *json_str(const JVal *obj, const char *key, const char *def);
double      json_num(const JVal *obj, const char *key, double def);

/* ── net.c ──────────────────────────────────────────────────────────────── */
/* return false to cancel */
typedef bool (*NetProgress)(void *ctx, int64_t done, int64_t total);

typedef struct {
    int status;              /* HTTP status, 0 if the request never completed */
    Buf body;
    wchar_t error[300];      /* readable reason when it failed */
} NetResult;

bool net_request(const wchar_t *method, const wchar_t *url, const wchar_t *headers,
                 const char *body, size_t body_len, NetResult *out);
/* streams to `dest`; fills sha256_hex (65 chars) of what was written */
bool net_download(const wchar_t *url, const wchar_t *dest, char *sha256_hex,
                  NetProgress cb, void *ctx, NetResult *out);
/* PUT/POST a file as the raw body */
bool net_upload(const wchar_t *method, const wchar_t *url, const wchar_t *headers,
                const wchar_t *src, NetProgress cb, void *ctx, NetResult *out);
void net_result_free(NetResult *r);
/* the server's {"error": "..."} if present, else the transport error / status */
void net_describe_error(const NetResult *r, wchar_t *out, size_t cap);

/* ── config.c ───────────────────────────────────────────────────────────── */
typedef struct {
    wchar_t echo_root[MAX_PATH];
    wchar_t server[512];
    wchar_t upload_key[256];
    /* what Map Tester itself last installed, to recognise its own files */
    wchar_t installed_id[64];
    wchar_t installed_name[256];
    int64_t installed_pkg_size, installed_man_size;
    uint64_t installed_pkg_time, installed_man_time;
} Config;

extern Config g_cfg;
void config_load(void);
void config_save(void);

/* The token the server hands back when a map is created is the only proof of who uploaded it (the
   server keeps just its hash), so it is kept here to let the uploader remove the map later. */
void uploads_remember(const wchar_t *id, const wchar_t *token);
bool uploads_token(const wchar_t *id, wchar_t *out, size_t cap);   /* false: not uploaded from this PC */
void uploads_forget(const wchar_t *id);

/* ── install.c ──────────────────────────────────────────────────────────── */
typedef struct {
    wchar_t root[MAX_PATH];        /* ready-at-dawn-echo-arena */
    wchar_t packages[MAX_PATH];
    wchar_t manifests[MAX_PATH];
    wchar_t package[MAX_PATH];     /* packages\48037dc70b0ecab2_3 */
    wchar_t manifest[MAX_PATH];    /* manifests\48037dc70b0ecab2 */
} EchoPaths;

typedef enum { STATE_NO_ECHO, STATE_BASE, STATE_OURS, STATE_OTHER_MOD } InstallState;

bool         echo_paths_from_root(const wchar_t *root, EchoPaths *out);
bool         echo_find(const wchar_t *picked, EchoPaths *out);  /* searches up and down from a pick */
bool         echo_guess(EchoPaths *out);                        /* common install locations */
InstallState install_state(const EchoPaths *p, wchar_t *label, size_t cap);
bool         echo_is_running(void);
/* Backs up whatever is there per the rules in install.c, then moves the two downloaded
   files (already inside the packages / manifests folders) into place. */
bool         install_files(const EchoPaths *p, const wchar_t *pkg_tmp, const wchar_t *man_tmp,
                           wchar_t *report, size_t cap);

typedef enum { BACKUP_NONE, BACKUP_MODDED, BACKUP_BASE } BackupKind;
/* MODDED: _3.bak / .modbak are there (the game was modded before)   BASE: only the .bak manifest */
BackupKind   backup_kind(const EchoPaths *p);
/* puts the backup back in place of the current files (which are removed) */
bool         revert_backup(const EchoPaths *p, wchar_t *report, size_t cap);

/* ── gfx.c ──────────────────────────────────────────────────────────────── */
typedef void *GpImage;

bool     gfx_start(void);
void     gfx_stop(void);
GpImage  gfx_load_image(const wchar_t *path);        /* decoded into memory; file not kept open */
void     gfx_free_image(GpImage img);
bool     gfx_image_size(GpImage img, UINT *w, UINT *h);
/* all ARGB colours are 0xAARRGGBB */
void     gfx_fill_round(HDC dc, int x, int y, int w, int h, int r, DWORD argb);
void     gfx_stroke_round(HDC dc, int x, int y, int w, int h, int r, DWORD argb, float width);
void     gfx_fill_gradient_v(HDC dc, int x, int y, int w, int h, DWORD top, DWORD bottom);
/* draws `img` covering the rect (cropped to fill), clipped to rounded corners */
void     gfx_draw_image_cover(HDC dc, GpImage img, int x, int y, int w, int h, int r);
void     gfx_fill_ellipse(HDC dc, int x, int y, int w, int h, DWORD argb);

/* ── main.c (shared UI helpers) ─────────────────────────────────────────── */
extern HINSTANCE g_inst;
extern HWND g_main;
extern EchoPaths g_echo;
extern bool g_have_echo;
int  dpi(int v);                         /* scale 96-dpi units */
HFONT ui_font(int px, int weight);       /* cached Segoe UI */
bool pick_folder(HWND owner, const wchar_t *title, const wchar_t *start, wchar_t *out, size_t cap);
bool prompt_text(HWND owner, const wchar_t *title, const wchar_t *label, wchar_t *value, size_t cap);
void ui_dark_titlebar(HWND h);
void ui_draw_button(const DRAWITEMSTRUCT *d, bool primary);
void refresh_maps(void);
void select_map_by_id(const wchar_t *id);

/* ── upload.c ───────────────────────────────────────────────────────────── */
void upload_open(HWND owner);
typedef struct { const wchar_t *id, *token, *name, *creator, *gametype, *description; } MapEditInit;
/* the same window, prefilled, saving with PATCH (and a new preview if one is chosen) */
void upload_open_edit(HWND owner, const MapEditInit *init);
bool upload_register(HINSTANCE inst);
HWND upload_window(void);   /* the open upload window, or NULL */

/* ── theme ──────────────────────────────────────────────────────────────── */
#define C_BG        RGB(13, 15, 22)
#define C_PANEL     RGB(20, 23, 33)
#define C_CARD      RGB(27, 31, 44)
#define C_CARD_HOT  RGB(34, 39, 56)
#define C_FIELD     RGB(31, 35, 50)
#define C_BORDER    RGB(44, 50, 70)
#define C_TEXT      RGB(236, 239, 247)
#define C_MUTED     RGB(143, 151, 173)
#define C_FAINT     RGB(96, 104, 126)
#define C_ACCENT    RGB(255, 138, 36)     /* echo orange */
#define C_ACCENT2   RGB(54, 165, 255)     /* echo blue */
#define C_OK        RGB(64, 200, 120)
#define C_BAD       RGB(240, 84, 84)

#define ARGB(a, c)  ((DWORD)(((DWORD)(a) << 24) | ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | (DWORD)GetBValue(c)))

#endif
