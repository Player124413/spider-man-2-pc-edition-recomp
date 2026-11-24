// Общие помощники HLE: трассировка, строки гостя, таблица объектов ядра.
#include "runtime/hle/hle.h"

#include <pthread.h>
#include <stdlib.h>

int g_hle_trace;
hle_hitch_counters g_hitch;
void (*hle_frame_hook)(int frame);

void hle_trace(CPU *c, const char *name, int nargs)
{
    fprintf(stderr, "[hle] %s(", name);
    for (int i = 0; i < nargs; i++)
        fprintf(stderr, "%s%08x", i ? ", " : "", ARG(i));
    fprintf(stderr, ") ret=%08x\n", rd32(g_mem, c->esp));
}

void hle_todo(const char *what)
{
    // список уже сообщённого — короткий, линейного поиска хватает
    static const char *seen[256];
    static int n;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < n; i++)
        if (seen[i] == what || !strcmp(seen[i], what)) {
            pthread_mutex_unlock(&lock);
            return;
        }
    if (n < 256)
        seen[n++] = what;
    pthread_mutex_unlock(&lock);
    fprintf(stderr, "[hle] TODO: %s\n", what);
}

u32 hle_dyn_thunk(hle_dyn_fn *table, const char *dll, const char *name)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    u32 r = 0;
    pthread_mutex_lock(&lock);
    for (hle_dyn_fn *d = table; d->name && !r; d++)
        if (!strcmp(d->name, name)) {
            if (!d->thunk)
                d->thunk = rt_hle_thunk(dll, d->fn);
            r = d->thunk;
        }
    pthread_mutex_unlock(&lock);
    return r;
}

const char *hle_cstr(u32 a) { return a ? (const char *)(g_mem + a) : ""; }

size_t hle_wlen(u32 a)
{
    size_t n = 0;
    if (a)
        while (rd16(g_mem, a + 2 * (u32)n))
            n++;
    return n;
}

void hle_w2utf8(u32 w, char *out, size_t cap)
{
    size_t o = 0;
    for (u32 i = 0; w; i++) {
        u32 ch = rd16(g_mem, w + 2 * i);
        if (ch >= 0xD800 && ch < 0xDC00) {
            u32 lo = rd16(g_mem, w + 2 * (i + 1));
            if (lo >= 0xDC00 && lo < 0xE000) {
                ch = 0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (!ch)
            break;
        char b[4];
        size_t k;
        if (ch < 0x80) { b[0] = (char)ch; k = 1; }
        else if (ch < 0x800) { b[0] = (char)(0xC0 | ch >> 6); b[1] = (char)(0x80 | (ch & 0x3F)); k = 2; }
        else if (ch < 0x10000) { b[0] = (char)(0xE0 | ch >> 12); b[1] = (char)(0x80 | (ch >> 6 & 0x3F)); b[2] = (char)(0x80 | (ch & 0x3F)); k = 3; }
        else { b[0] = (char)(0xF0 | ch >> 18); b[1] = (char)(0x80 | (ch >> 12 & 0x3F)); b[2] = (char)(0x80 | (ch >> 6 & 0x3F)); b[3] = (char)(0x80 | (ch & 0x3F)); k = 4; }
        if (o + k + 1 > cap)
            break;
        memcpy(out + o, b, k);
        o += k;
    }
    if (cap)
        out[o] = 0;
}

u32 hle_put_a(u32 dst, u32 cap, const char *s)
{
    u32 n = (u32)strlen(s);
    if (!cap)
        return n;
    if (n >= cap)
        n = cap - 1;
    memcpy(g_mem + dst, s, n);
    g_mem[dst + n] = 0;
    return n;
}

u32 hle_put_w(u32 dst, u32 cap, const char *s)
{
    u32 n = 0;
    const u8 *p = (const u8 *)s;
    while (*p && n + 1 < cap) {
        u32 ch = *p++;
        if (ch >= 0xC0) {  // UTF-8 -> кодовая точка (BMP; остальное — '?')
            int k = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1;
            ch &= 0x3F >> k;
            while (k-- && (*p & 0xC0) == 0x80)
                ch = ch << 6 | (*p++ & 0x3F);
            if (ch > 0xFFFF)
                ch = '?';
        }
        wr16(g_mem, dst + 2 * n++, (u16)ch);
    }
    if (cap)
        wr16(g_mem, dst + 2 * n, 0);
    return n;
}

// ---------------------------------------------------------------- объекты ядра
#define MAX_OBJS 4096
struct hle_obj {
    int type;
    void *data;
    void (*closer)(void *);
};
static struct hle_obj g_objs[MAX_OBJS];
static pthread_mutex_t g_objs_lock = PTHREAD_MUTEX_INITIALIZER;

static int obj_index(u32 h) { return h >= 0x100 && !(h & 3) && (h - 0x100) / 4 < MAX_OBJS ? (int)((h - 0x100) / 4) : -1; }

u32 hle_obj_new(int type, void *data)
{
    pthread_mutex_lock(&g_objs_lock);
    u32 h = 0;
    for (int i = 0; i < MAX_OBJS; i++)
        if (g_objs[i].type == OBJ_NONE) {
            g_objs[i] = (struct hle_obj){type, data, NULL};
            h = 0x100u + 4u * (u32)i;
            break;
        }
    pthread_mutex_unlock(&g_objs_lock);
    return h;
}

void *hle_obj_get(u32 h, int type)
{
    int i = obj_index(h);
    return i >= 0 && g_objs[i].type == type ? g_objs[i].data : NULL;
}

int hle_obj_type(u32 h)
{
    int i = obj_index(h);
    return i >= 0 ? g_objs[i].type : OBJ_NONE;
}

void hle_obj_set_closer(u32 h, void (*closer)(void *))
{
    int i = obj_index(h);
    if (i >= 0)
        g_objs[i].closer = closer;
}

int hle_obj_close(u32 h)
{
    pthread_mutex_lock(&g_objs_lock);
    int i = obj_index(h);
    struct hle_obj o = {0};
    if (i >= 0 && g_objs[i].type != OBJ_NONE) {
        o = g_objs[i];
        g_objs[i].type = OBJ_NONE;
    }
    pthread_mutex_unlock(&g_objs_lock);
    if (o.type == OBJ_NONE)
        return 0;
    if (o.closer)
        o.closer(o.data);
    return 1;
}
