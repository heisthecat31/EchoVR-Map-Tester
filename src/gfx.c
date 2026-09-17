/* Anti-aliased shapes and images through the GDI+ flat API (plain C, no C++ headers). */
#include "app.h"

#include <objidl.h>
#include <stdlib.h>
#include <string.h>

typedef int GpStatus;
typedef void GpGraphics, GpPath, GpBrush, GpPen, GpBitmap;
typedef struct { UINT32 version; void *debug_cb; BOOL no_bg_thread; BOOL no_ext_codecs; } GdiplusStartupInput;
typedef struct { int X, Y; } GpPoint;

GpStatus WINAPI GdiplusStartup(ULONG_PTR *token, const GdiplusStartupInput *in, void *out);
void     WINAPI GdiplusShutdown(ULONG_PTR token);
GpStatus WINAPI GdipLoadImageFromStream(IStream *s, GpImage *img);
GpStatus WINAPI GdipDisposeImage(GpImage img);
GpStatus WINAPI GdipGetImageWidth(GpImage img, UINT *w);
GpStatus WINAPI GdipGetImageHeight(GpImage img, UINT *h);
GpStatus WINAPI GdipCreateBitmapFromScan0(INT w, INT h, INT stride, INT fmt, BYTE *scan0, GpBitmap **bmp);
GpStatus WINAPI GdipGetImageGraphicsContext(GpImage img, GpGraphics **g);
GpStatus WINAPI GdipCreateFromHDC(HDC dc, GpGraphics **g);
GpStatus WINAPI GdipDeleteGraphics(GpGraphics *g);
GpStatus WINAPI GdipSetSmoothingMode(GpGraphics *g, int mode);
GpStatus WINAPI GdipSetInterpolationMode(GpGraphics *g, int mode);
GpStatus WINAPI GdipSetPixelOffsetMode(GpGraphics *g, int mode);
GpStatus WINAPI GdipCreatePath(int fill_mode, GpPath **p);
GpStatus WINAPI GdipDeletePath(GpPath *p);
GpStatus WINAPI GdipAddPathArc(GpPath *p, float x, float y, float w, float h, float start, float sweep);
GpStatus WINAPI GdipAddPathRectangle(GpPath *p, float x, float y, float w, float h);
GpStatus WINAPI GdipClosePathFigure(GpPath *p);
GpStatus WINAPI GdipCreateSolidFill(DWORD argb, GpBrush **b);
GpStatus WINAPI GdipCreateLineBrushI(const GpPoint *p1, const GpPoint *p2, DWORD c1, DWORD c2, int wrap, GpBrush **b);
GpStatus WINAPI GdipDeleteBrush(GpBrush *b);
GpStatus WINAPI GdipCreatePen1(DWORD argb, float width, int unit, GpPen **p);
GpStatus WINAPI GdipDeletePen(GpPen *p);
GpStatus WINAPI GdipFillPath(GpGraphics *g, GpBrush *b, GpPath *p);
GpStatus WINAPI GdipDrawPath(GpGraphics *g, GpPen *pen, GpPath *p);
GpStatus WINAPI GdipFillEllipseI(GpGraphics *g, GpBrush *b, int x, int y, int w, int h);
GpStatus WINAPI GdipSetClipPath(GpGraphics *g, GpPath *p, int combine);
GpStatus WINAPI GdipDrawImageRectRectI(GpGraphics *g, GpImage img, int dx, int dy, int dw, int dh,
                                       int sx, int sy, int sw, int sh, int unit, void *attrs,
                                       void *cb, void *cb_data);

#define PixelFormat32bppPARGB 0x000E200B
#define UnitPixel 2

static ULONG_PTR token;

bool gfx_start(void)
{
    GdiplusStartupInput in = { 1, NULL, FALSE, FALSE };
    return GdiplusStartup(&token, &in, NULL) == 0;
}

void gfx_stop(void)
{
    if (token) GdiplusShutdown(token);
    token = 0;
}

GpImage gfx_load_image(const wchar_t *path)
{
    /* Read the bytes ourselves and copy the decoded pixels into a bitmap we own: GDI+ keeps a
       file-backed image's file locked, and a PARGB bitmap draws much faster when scaled. */
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(f, NULL);
    HGLOBAL mem = size && size < (64u << 20) ? GlobalAlloc(GMEM_MOVEABLE, size) : NULL;
    bool read_ok = false;
    if (mem) {
        void *p = GlobalLock(mem);
        DWORD got = 0;
        read_ok = p && ReadFile(f, p, size, &got, NULL) && got == size;
        GlobalUnlock(mem);
    }
    CloseHandle(f);
    if (!read_ok) { if (mem) GlobalFree(mem); return NULL; }

    IStream *stream = NULL;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) { GlobalFree(mem); return NULL; }
    GpImage src = NULL, out = NULL;
    if (GdipLoadImageFromStream(stream, &src) == 0) {
        UINT w = 0, h = 0;
        GdipGetImageWidth(src, &w);
        GdipGetImageHeight(src, &h);
        GpBitmap *bmp = NULL;
        if (w && h && GdipCreateBitmapFromScan0((INT)w, (INT)h, 0, PixelFormat32bppPARGB, NULL, &bmp) == 0) {
            GpGraphics *g = NULL;
            if (GdipGetImageGraphicsContext(bmp, &g) == 0) {
                GdipDrawImageRectRectI(g, src, 0, 0, (int)w, (int)h, 0, 0, (int)w, (int)h, UnitPixel, NULL, NULL, NULL);
                GdipDeleteGraphics(g);
                out = bmp;
            } else {
                GdipDisposeImage(bmp);
            }
        }
        GdipDisposeImage(src);
    }
    stream->lpVtbl->Release(stream);   /* frees `mem` too */
    return out;
}

void gfx_free_image(GpImage img)
{
    if (img) GdipDisposeImage(img);
}

bool gfx_image_size(GpImage img, UINT *w, UINT *h)
{
    return img && GdipGetImageWidth(img, w) == 0 && GdipGetImageHeight(img, h) == 0;
}

static GpGraphics *begin(HDC dc)
{
    GpGraphics *g = NULL;
    if (GdipCreateFromHDC(dc, &g) != 0) return NULL;
    GdipSetSmoothingMode(g, 4);        /* anti-alias */
    GdipSetPixelOffsetMode(g, 2);      /* high quality */
    return g;
}

static GpPath *round_path(float x, float y, float w, float h, float r)
{
    GpPath *p = NULL;
    GdipCreatePath(0, &p);
    if (!p) return NULL;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0.5f) {
        GdipAddPathRectangle(p, x, y, w, h);
        return p;
    }
    float d = r * 2;
    GdipAddPathArc(p, x, y, d, d, 180, 90);
    GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(p);
    return p;
}

void gfx_fill_round(HDC dc, int x, int y, int w, int h, int r, DWORD argb)
{
    GpGraphics *g = begin(dc);
    if (!g) return;
    GpPath *p = round_path((float)x, (float)y, (float)w, (float)h, (float)r);
    GpBrush *b = NULL;
    GdipCreateSolidFill(argb, &b);
    if (p && b) GdipFillPath(g, b, p);
    if (b) GdipDeleteBrush(b);
    if (p) GdipDeletePath(p);
    GdipDeleteGraphics(g);
}

void gfx_stroke_round(HDC dc, int x, int y, int w, int h, int r, DWORD argb, float width)
{
    GpGraphics *g = begin(dc);
    if (!g) return;
    float half = width / 2;
    GpPath *p = round_path(x + half, y + half, w - width, h - width, (float)r);
    GpPen *pen = NULL;
    GdipCreatePen1(argb, width, UnitPixel, &pen);
    if (p && pen) GdipDrawPath(g, pen, p);
    if (pen) GdipDeletePen(pen);
    if (p) GdipDeletePath(p);
    GdipDeleteGraphics(g);
}

void gfx_fill_gradient_v(HDC dc, int x, int y, int w, int h, DWORD top, DWORD bottom)
{
    GpGraphics *g = begin(dc);
    if (!g) return;
    GpPoint a = { x, y - 1 }, b = { x, y + h + 1 };
    GpBrush *br = NULL;
    GpPath *p = round_path((float)x, (float)y, (float)w, (float)h, 0);
    GdipCreateLineBrushI(&a, &b, top, bottom, 3, &br);
    if (br && p) GdipFillPath(g, br, p);
    if (br) GdipDeleteBrush(br);
    if (p) GdipDeletePath(p);
    GdipDeleteGraphics(g);
}

void gfx_draw_image_cover(HDC dc, GpImage img, int x, int y, int w, int h, int r)
{
    UINT iw, ih;
    if (!gfx_image_size(img, &iw, &ih) || w <= 0 || h <= 0) return;
    GpGraphics *g = begin(dc);
    if (!g) return;
    GdipSetInterpolationMode(g, 7);    /* high quality bicubic */
    GpPath *p = round_path((float)x, (float)y, (float)w, (float)h, (float)r);
    if (p) GdipSetClipPath(g, p, 0);
    /* crop the source to the destination's aspect so the picture fills without stretching */
    double want = (double)w / h, have = (double)iw / ih;
    int sx = 0, sy = 0, sw = (int)iw, sh = (int)ih;
    if (have > want) { sw = (int)(ih * want); sx = ((int)iw - sw) / 2; }
    else             { sh = (int)(iw / want); sy = ((int)ih - sh) / 2; }
    GdipDrawImageRectRectI(g, img, x, y, w, h, sx, sy, sw, sh, UnitPixel, NULL, NULL, NULL);
    if (p) GdipDeletePath(p);
    GdipDeleteGraphics(g);
}

void gfx_fill_ellipse(HDC dc, int x, int y, int w, int h, DWORD argb)
{
    GpGraphics *g = begin(dc);
    if (!g) return;
    GpBrush *b = NULL;
    GdipCreateSolidFill(argb, &b);
    if (b) { GdipFillEllipseI(g, b, x, y, w, h); GdipDeleteBrush(b); }
    GdipDeleteGraphics(g);
}
