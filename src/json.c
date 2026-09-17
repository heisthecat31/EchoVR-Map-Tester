/* A small JSON reader -- enough for the server's replies (objects, arrays, strings, numbers). */
#include "app.h"

#include <stdlib.h>
#include <string.h>

typedef struct { const char *p; int depth; } Parser;

static JVal *parse_value(Parser *ps);

static JVal *new_val(JType t)
{
    JVal *v = (JVal *)calloc(1, sizeof(JVal));
    if (!v) abort();
    v->type = t;
    return v;
}

static void skip_ws(Parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') ps->p++;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(p[i]);
        if (h < 0) return false;
        v = v << 4 | (unsigned)h;
    }
    *out = v;
    return true;
}

static void put_utf8(Buf *b, unsigned cp)
{
    char t[4];
    if (cp < 0x80) { t[0] = (char)cp; buf_append(b, t, 1); }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | cp >> 6); t[1] = (char)(0x80 | (cp & 0x3F)); buf_append(b, t, 2); }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | cp >> 12); t[1] = (char)(0x80 | (cp >> 6 & 0x3F)); t[2] = (char)(0x80 | (cp & 0x3F)); buf_append(b, t, 3); }
    else { t[0] = (char)(0xF0 | cp >> 18); t[1] = (char)(0x80 | (cp >> 12 & 0x3F)); t[2] = (char)(0x80 | (cp >> 6 & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F)); buf_append(b, t, 4); }
}

static char *parse_string(Parser *ps)
{
    if (*ps->p != '"') return NULL;
    ps->p++;
    Buf b = {0};
    buf_append(&b, "", 0);
    for (;;) {
        char c = *ps->p;
        if (!c) { buf_free(&b); return NULL; }
        ps->p++;
        if (c == '"') break;
        if (c != '\\') { buf_append(&b, &c, 1); continue; }
        char e = *ps->p++;
        switch (e) {
        case '"': buf_append(&b, "\"", 1); break;
        case '\\': buf_append(&b, "\\", 1); break;
        case '/': buf_append(&b, "/", 1); break;
        case 'b': buf_append(&b, "\b", 1); break;
        case 'f': buf_append(&b, "\f", 1); break;
        case 'n': buf_append(&b, "\n", 1); break;
        case 'r': buf_append(&b, "\r", 1); break;
        case 't': buf_append(&b, "\t", 1); break;
        case 'u': {
            unsigned cp;
            if (!read_hex4(ps->p, &cp)) { buf_free(&b); return NULL; }
            ps->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned lo;
                if (read_hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ps->p += 6;
                }
            }
            put_utf8(&b, cp);
            break;
        }
        default: buf_free(&b); return NULL;
        }
    }
    return b.data;
}

static JVal *parse_container(Parser *ps, bool object)
{
    char close = object ? '}' : ']';
    JVal *v = new_val(object ? J_OBJ : J_ARR);
    JVal **tail = &v->child;
    ps->p++;
    skip_ws(ps);
    if (*ps->p == close) { ps->p++; return v; }
    for (;;) {
        char *key = NULL;
        skip_ws(ps);
        if (object) {
            key = parse_string(ps);
            if (!key) goto fail;
            skip_ws(ps);
            if (*ps->p != ':') { free(key); goto fail; }
            ps->p++;
        }
        JVal *item = parse_value(ps);
        if (!item) { free(key); goto fail; }
        item->key = key;
        *tail = item;
        tail = &item->next;
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == close) { ps->p++; return v; }
        goto fail;
    }
fail:
    json_free(v);
    return NULL;
}

static JVal *parse_value(Parser *ps)
{
    if (++ps->depth > 64) return NULL;
    skip_ws(ps);
    JVal *v = NULL;
    char c = *ps->p;
    if (c == '{' || c == '[') {
        v = parse_container(ps, c == '{');
    } else if (c == '"') {
        char *s = parse_string(ps);
        if (s) { v = new_val(J_STR); v->str = s; }
    } else if (!strncmp(ps->p, "true", 4)) {
        v = new_val(J_BOOL); v->b = true; ps->p += 4;
    } else if (!strncmp(ps->p, "false", 5)) {
        v = new_val(J_BOOL); ps->p += 5;
    } else if (!strncmp(ps->p, "null", 4)) {
        v = new_val(J_NULL); ps->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        double d = strtod(ps->p, &end);
        if (end != ps->p) { v = new_val(J_NUM); v->num = d; ps->p = end; }
    }
    ps->depth--;
    return v;
}

JVal *json_parse(const char *text)
{
    if (!text) return NULL;
    Parser ps = { text, 0 };
    JVal *v = parse_value(&ps);
    if (!v) return NULL;
    skip_ws(&ps);
    if (*ps.p) { json_free(v); return NULL; }
    return v;
}

void json_free(JVal *v)
{
    while (v) {
        JVal *next = v->next;
        json_free(v->child);
        free(v->key);
        free(v->str);
        free(v);
        v = next;
    }
}

JVal *json_get(const JVal *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ) return NULL;
    for (JVal *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

const char *json_str(const JVal *obj, const char *key, const char *def)
{
    JVal *v = json_get(obj, key);
    return v && v->type == J_STR ? v->str : def;
}

double json_num(const JVal *obj, const char *key, double def)
{
    JVal *v = json_get(obj, key);
    return v && v->type == J_NUM ? v->num : def;
}
