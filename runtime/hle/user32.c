// HLE user32: классы и окна, очереди сообщений по потокам, SendMessage/PostMessage с вызовом гостевых
// WndProc, диалоги из ресурсов PE, курсор/фокус/захват, метрики экрана. Вывода на экран здесь нет:
// единственное настоящее окно (видовое окно WinDrv) связывается с SDL позже (Ф7/Ф8); заставка и прочие
// окна существуют только как объекты.
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>

#include "runtime/heap.h"
#include "runtime/rt.h"
#include "runtime/hle/d3d8.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/user32.h"

#define MAX_WIN 1024
#define MAX_CLASS 128
#define HWND_BASE 0x00020000u
#define HWND_DESKTOP_ 0x00010010u

enum {
    WM_CREATE = 0x01, WM_DESTROY = 0x02, WM_MOVE = 0x03, WM_SIZE = 0x05, WM_ACTIVATE = 0x06, WM_SETFOCUS = 0x07,
    WM_KILLFOCUS = 0x08, WM_ENABLE = 0x0A, WM_SETTEXT = 0x0C, WM_GETTEXT = 0x0D, WM_GETTEXTLENGTH = 0x0E,
    WM_PAINT = 0x0F, WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_SHOWWINDOW = 0x18, WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82,
    WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105,
    WM_SYSCHAR = 0x106, WM_INITDIALOG = 0x110, WM_COMMAND = 0x111, WM_TIMER = 0x113, WM_ACTIVATEAPP = 0x1C,
    WM_NCACTIVATE = 0x86,
};

typedef struct {
    int used;
    char name[128];
    u32 atom, proc, style, extra, hinst, wide, cursor, icon, brush;
} wclass;

typedef struct {
    int used;
    u32 cls, proc, dlgproc, style, exstyle, parent, owner, id, hinst, menu, tid, userdata;
    s32 x, y, w, h;
    int visible, enabled, wide, is_dialog, ended;
    int need_paint;  // окно «недействительно»: при пустой очереди GetMessage/PeekMessage отдают WM_PAINT
    u32 dlg_result, dlg_msgresult, dlg_user;
    char text[256];
    u32 extra[32];
    struct { char name[64]; u32 atom, value; } props[8];
} win;

static wclass g_cls[MAX_CLASS];
static win g_win[MAX_WIN];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 g_next_atom = 0xC000;
static u32 g_focus, g_active, g_capture, g_foreground;
static s32 g_cursor_x = 640, g_cursor_y = 360, g_show_cursor;
static s32 g_screen_w = 1280, g_screen_h = 720;
static u8 g_keys[256];

void user32_set_screen(int w, int h)
{
    g_screen_w = w;
    g_screen_h = h;
}

void user32_screen(int *w, int *h)
{
    *w = g_screen_w;
    *h = g_screen_h;
}

static win *W(u32 h)
{
    if (h < HWND_BASE || (h - HWND_BASE) % 4 || (h - HWND_BASE) / 4 >= MAX_WIN)
        return NULL;
    win *w = &g_win[(h - HWND_BASE) / 4];
    return w->used ? w : NULL;
}

static u32 hwnd_of(const win *w) { return HWND_BASE + 4u * (u32)(w - g_win); }

// Строка гостя (A или W) -> UTF-8; атом/ordinal (< 0x10000) — "#n"
static void gstr(u32 p, int wide, char *out, size_t cap)
{
    if (p < 0x10000) {
        snprintf(out, cap, "#%u", p);
        return;
    }
    if (wide)
        hle_w2utf8(p, out, cap);
    else
        snprintf(out, cap, "%s", hle_cstr(p));
}

// ================================================================ очереди сообщений
#define QLEN 512
typedef struct {
    u32 tid;
    u32 msg[QLEN][7];  // hwnd, message, wParam, lParam, time, pt.x, pt.y
    int head, count;
    int quit;
    u32 quit_code;
    pthread_cond_t cv;
} mqueue;

static mqueue g_q[64];

static mqueue *queue_of(u32 tid)
{
    for (int i = 0; i < 64; i++)
        if (g_q[i].tid == tid)
            return &g_q[i];
    for (int i = 0; i < 64; i++)
        if (!g_q[i].tid) {
            g_q[i].tid = tid;
            pthread_cond_init(&g_q[i].cv, NULL);
            return &g_q[i];
        }
    return &g_q[0];
}

static u32 now_ms(void) { return (u32)(rt_ticks_ns() / 1000000u); }

static void post(u32 tid, u32 hwnd, u32 msg, u32 wp, u32 lp)
{
    mqueue *q = queue_of(tid);
    if (q->count == QLEN) {
        hle_todo("message queue overflow");
        return;
    }
    u32 *m = q->msg[(q->head + q->count++) % QLEN];
    m[0] = hwnd;
    m[1] = msg;
    m[2] = wp;
    m[3] = lp;
    m[4] = now_ms();
    m[5] = (u32)g_cursor_x;
    m[6] = (u32)g_cursor_y;
    pthread_cond_broadcast(&q->cv);
}

void user32_post(u32 hwnd, u32 msg, u32 wp, u32 lp)
{
    pthread_mutex_lock(&g_lock);
    win *w = W(hwnd);
    if (w)
        post(w->tid, hwnd, msg, wp, lp);
    pthread_mutex_unlock(&g_lock);
}

// Взять сообщение из очереди текущего потока: 1 — есть, 0 — нет (и wait == 0)
static int take(u32 out_msg, u32 hwnd, u32 lo, u32 hi, int remove, int wait)
{
    u32 tid = rt_thread_current()->tid;
    pthread_mutex_lock(&g_lock);
    mqueue *q = queue_of(tid);
    for (;;) {
        for (int k = 0; k < q->count; k++) {
            u32 *m = q->msg[(q->head + k) % QLEN];
            if (hwnd && m[0] != hwnd)
                continue;
            if ((lo || hi) && (m[1] < lo || m[1] > hi))
                continue;
            memcpy(g_mem + out_msg, m, 28);
            if (remove) {
                for (int j = k; j > 0; j--)
                    memcpy(q->msg[(q->head + j) % QLEN], q->msg[(q->head + j - 1) % QLEN], 28);
                q->head = (q->head + 1) % QLEN;
                q->count--;
            }
            pthread_mutex_unlock(&g_lock);
            return 1;
        }
        // как в Windows: после отправленных и посланных сообщений — WM_PAINT недействительных окон потока
        if (!(lo || hi) || (WM_PAINT >= lo && WM_PAINT <= hi))
            for (int i = 0; i < MAX_WIN; i++) {
                win *w = &g_win[i];
                if (!w->used || !w->need_paint || !w->visible || w->tid != tid)
                    continue;
                u32 h = hwnd_of(w);
                if (hwnd && h != hwnd && w->parent != hwnd)
                    continue;
                u32 m[7] = {h, WM_PAINT, 0, 0, now_ms(), (u32)g_cursor_x, (u32)g_cursor_y};
                memcpy(g_mem + out_msg, m, 28);
                pthread_mutex_unlock(&g_lock);
                return 1;
            }
        if (q->quit) {
            u32 m[7] = {0, WM_QUIT, q->quit_code, 0, now_ms(), 0, 0};
            memcpy(g_mem + out_msg, m, 28);
            if (remove)
                q->quit = 0;
            pthread_mutex_unlock(&g_lock);
            return 1;
        }
        if (!wait)
            break;
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += 50000000;
        if (dl.tv_nsec >= 1000000000) {
            dl.tv_sec++;
            dl.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&q->cv, &g_lock, &dl);
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

// ================================================================ вызов оконных процедур
static u32 def_window_proc(CPU *c, u32 hwnd, u32 msg, u32 wp, u32 lp);
static u32 set_focus(CPU *c, u32 hwnd);

static u32 call_proc(CPU *c, u32 proc, u32 hwnd, u32 msg, u32 wp, u32 lp)
{
    u32 args[4] = {hwnd, msg, wp, lp};
    return rt_guest_call(c, proc, 4, args);
}

// DefDlgProc: процедура диалога возвращает TRUE — обработано (результат в DWL_MSGRESULT)
static u32 dialog_dispatch(CPU *c, win *w, u32 hwnd, u32 msg, u32 wp, u32 lp)
{
    u32 r = w->dlgproc ? call_proc(c, w->dlgproc, hwnd, msg, wp, lp) : 0;
    if (msg == WM_INITDIALOG)
        return r;
    if (r)
        return W(hwnd) ? W(hwnd)->dlg_msgresult : 0;
    if (msg == WM_CLOSE)
        return 0;  // диалог закрывается своей процедурой (EndDialog/DestroyWindow)
    return def_window_proc(c, hwnd, msg, wp, lp);
}

// STM_SETIMAGE(IMAGE_BITMAP): так game.exe ставит заставку загрузки (Logo.bmp) — её рисует d3d8_splash
static void static_image(u32 hbm)
{
    u32 bits, bpp, stride;
    s32 bw, bh;
    if (gdi32_bitmap(hbm, &bits, &bw, &bh, &bpp, &stride) && bpp == 32 && bh > 0)
        d3d8_splash(g_mem + bits, bw, bh, stride);
}

u32 user32_send(CPU *c, u32 hwnd, u32 msg, u32 wp, u32 lp)
{
    win *w = W(hwnd);
    if (!w)
        return 0;
    if (msg == 0x172 /* STM_SETIMAGE */ && wp == 0 /* IMAGE_BITMAP */ && lp)
        static_image(lp);
    if (w->is_dialog && !w->proc)
        return dialog_dispatch(c, w, hwnd, msg, wp, lp);
    if (!w->proc)
        return def_window_proc(c, hwnd, msg, wp, lp);
    return call_proc(c, w->proc, hwnd, msg, wp, lp);
}

static void destroy_window(CPU *c, u32 hwnd)
{
    win *w = W(hwnd);
    if (!w)
        return;
    for (int i = 0; i < MAX_WIN; i++)  // сначала дочерние
        if (g_win[i].used && g_win[i].parent == hwnd)
            destroy_window(c, hwnd_of(&g_win[i]));
    user32_send(c, hwnd, WM_DESTROY, 0, 0);
    user32_send(c, hwnd, 0x82 /* WM_NCDESTROY */, 0, 0);
    if (g_focus == hwnd)
        g_focus = 0;
    if (g_active == hwnd)
        g_active = 0;
    if (g_capture == hwnd)
        g_capture = 0;
    w = W(hwnd);
    if (w)
        w->used = 0;
}

static u32 def_window_proc(CPU *c, u32 hwnd, u32 msg, u32 wp, u32 lp)
{
    win *w = W(hwnd);
    (void)wp;
    switch (msg) {
    case WM_NCCREATE:
        return 1;
    case WM_CLOSE:
        destroy_window(c, hwnd);
        return 0;
    case WM_SETTEXT:
        if (w)
            gstr(lp, w->wide, w->text, sizeof w->text);
        return 1;
    case WM_GETTEXTLENGTH:
        return w ? (u32)strlen(w->text) : 0;
    case WM_GETTEXT:
        return w ? (w->wide ? hle_put_w(lp, wp, w->text) : hle_put_a(lp, wp, w->text)) : 0;
    case WM_PAINT:
        if (w)
            w->need_paint = 0;
        return 0;
    case WM_ACTIVATE:
        if ((wp & 0xFFFF) && w)
            set_focus(c, hwnd);
        return 0;
    case WM_NCACTIVATE:
        return 1;
    default:
        return 0;
    }
}

// ================================================================ классы
// Процедура системных классов (BUTTON, EDIT, …): поведение DefWindowProc, элементов управления на экране нет
// EM_REPLACESEL в EDIT — так лог-окно движка (WLog) дописывает строки лога: дублируем их в stderr.
static void sys_wndproc(CPU *c)
{
    u32 hwnd = ARG(0), msg = ARG(1);
    if (msg == 0xC2 /* EM_REPLACESEL */ && ARG(3) && !getenv("SNX_QUIET_LOG")) {
        win *w = W(hwnd);
        char text[2048];
        gstr(ARG(3), w ? w->wide : 1, text, sizeof text);
        size_t n = strlen(text);
        while (n && (text[n - 1] == '\n' || text[n - 1] == '\r'))
            text[--n] = 0;
        if (n)
            fprintf(stderr, "[log] %s\n", text);
        RET(4, 0);
    }
    RET(4, def_window_proc(c, hwnd, msg, ARG(2), ARG(3)));
}

static void system_classes(void)
{
    static const char *const names[] = {
        "BUTTON", "EDIT", "STATIC", "LISTBOX", "COMBOBOX", "COMBOLBOX", "SCROLLBAR", "MDICLIENT", "#32770",
        "RichEdit", "RICHEDIT", "RichEdit20A", "RichEdit20W", "msctls_trackbar32", "msctls_progress32",
        "msctls_updown32", "SysListView32", "SysTreeView32", "SysTabControl32", "ToolbarWindow32",
        "msctls_statusbar32", "tooltips_class32", "SysHeader32", "ReBarWindow32", "ComboBoxEx32",
    };
    u32 proc = rt_hle_thunk("user32.dll", sys_wndproc);
    for (size_t i = 0; i < sizeof names / sizeof names[0] && i < MAX_CLASS; i++) {
        wclass *k = &g_cls[i];
        k->used = 1;
        snprintf(k->name, sizeof k->name, "%s", names[i]);
        k->proc = proc;
        k->style = 0x8 | 0x3;  // CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW
        k->wide = 1;
        k->atom = g_next_atom++;
    }
}

static wclass *find_class(const char *name)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, system_classes);
    for (int i = 0; i < MAX_CLASS; i++)
        if (g_cls[i].used && !strcasecmp(g_cls[i].name, name))
            return &g_cls[i];
    if (name[0] == '#') {
        u32 atom = (u32)strtoul(name + 1, NULL, 10);
        for (int i = 0; i < MAX_CLASS; i++)
            if (g_cls[i].used && g_cls[i].atom == atom)
                return &g_cls[i];
    }
    return NULL;
}

static u32 register_class(CPU *c, int wide)
{
    u32 p = ARG(0);
    char name[128];
    gstr(rd32(g_mem, p + 40), wide, name, sizeof name);
    pthread_mutex_lock(&g_lock);
    wclass *k = find_class(name);
    if (!k)
        for (int i = 0; i < MAX_CLASS && !k; i++)
            if (!g_cls[i].used)
                k = &g_cls[i];
    u32 atom = 0;
    if (k) {
        k->used = 1;
        snprintf(k->name, sizeof k->name, "%s", name);
        k->style = rd32(g_mem, p + 4);
        k->proc = rd32(g_mem, p + 8);
        k->extra = rd32(g_mem, p + 16);
        k->hinst = rd32(g_mem, p + 20);
        k->icon = rd32(g_mem, p + 24);
        k->cursor = rd32(g_mem, p + 28);
        k->brush = rd32(g_mem, p + 32);
        k->wide = (u32)wide;
        k->atom = k->atom ? k->atom : g_next_atom++;
        atom = k->atom;
    }
    pthread_mutex_unlock(&g_lock);
    return atom;
}

void hle_user32_RegisterClassExA(CPU *c) { HLE_ENTER("RegisterClassExA", 1); RET(1, register_class(c, 0)); }
void hle_user32_RegisterClassExW(CPU *c) { HLE_ENTER("RegisterClassExW", 1); RET(1, register_class(c, 1)); }

static u32 class_info(CPU *c, int wide)
{
    char name[128];
    gstr(ARG(1), wide, name, sizeof name);
    wclass *k = find_class(name);
    if (!k) {
        rt_set_last_error(1411);  // ERROR_CLASS_DOES_NOT_EXIST
        return 0;
    }
    u32 p = ARG(2);
    wr32(g_mem, p + 4, k->style);
    wr32(g_mem, p + 8, k->proc);
    wr32(g_mem, p + 12, 0);
    wr32(g_mem, p + 16, k->extra);
    wr32(g_mem, p + 20, k->hinst);
    wr32(g_mem, p + 24, k->icon);
    wr32(g_mem, p + 28, k->cursor);
    wr32(g_mem, p + 32, k->brush);
    return k->atom;
}

void hle_user32_GetClassInfoExA(CPU *c) { HLE_ENTER("GetClassInfoExA", 3); RET(3, class_info(c, 0)); }
void hle_user32_GetClassInfoExW(CPU *c) { HLE_ENTER("GetClassInfoExW", 3); RET(3, class_info(c, 1)); }

// ================================================================ создание окон
static win *new_window(void)
{
    for (int i = 0; i < MAX_WIN; i++)
        if (!g_win[i].used) {
            memset(&g_win[i], 0, sizeof g_win[i]);
            g_win[i].used = 1;
            g_win[i].enabled = 1;
            g_win[i].tid = rt_thread_current()->tid;
            return &g_win[i];
        }
    return NULL;
}

static u32 create_window(CPU *c, int wide, u32 exstyle, u32 cls_p, u32 title_p, u32 style, s32 x, s32 y, s32 cx,
                         s32 cy, u32 parent, u32 menu, u32 hinst, u32 param)
{
    char cname[128];
    gstr(cls_p, wide, cname, sizeof cname);
    pthread_mutex_lock(&g_lock);
    wclass *k = find_class(cname);
    win *w = k ? new_window() : NULL;
    if (!w) {
        pthread_mutex_unlock(&g_lock);
        fprintf(stderr, "[user32] CreateWindowEx: class '%s' %s\n", cname, k ? "(no free slots)" : "not registered");
        rt_set_last_error(1407);  // ERROR_CANNOT_FIND_WND_CLASS
        return 0;
    }
    if ((u32)x == 0x80000000u)
        x = y = 0;
    if ((u32)cx == 0x80000000u) {
        cx = g_screen_w;
        cy = g_screen_h;
    }
    w->cls = (u32)(k - g_cls);
    w->proc = k->proc;
    w->style = style;
    w->exstyle = exstyle;
    w->parent = (style & 0x40000000u) ? parent : 0;  // WS_CHILD
    w->owner = (style & 0x40000000u) ? 0 : parent;
    w->id = (style & 0x40000000u) ? menu : 0;
    w->menu = (style & 0x40000000u) ? 0 : menu;
    w->hinst = hinst;
    w->x = x;
    w->y = y;
    w->w = cx;
    w->h = cy;
    w->wide = wide;
    if (title_p)
        gstr(title_p, wide, w->text, sizeof w->text);
    u32 hwnd = hwnd_of(w);
    pthread_mutex_unlock(&g_lock);
    if (getenv("SNX_LOG_WINDOWS"))
        fprintf(stderr, "[user32] CreateWindowEx '%s' '%s' style %08x %dx%d -> %08x\n", cname, w->text, style, cx, cy, hwnd);
    // CREATESTRUCT на гостевом стеке
    u32 sp = c->esp;
    c->esp -= 64;
    u32 cs = c->esp;
    u32 v[12] = {param, hinst, menu, parent, (u32)cy, (u32)cx, (u32)y, (u32)x, style, title_p, cls_p, exstyle};
    memcpy(g_mem + cs, v, sizeof v);
    u32 ok = user32_send(c, hwnd, WM_NCCREATE, 0, cs);
    if (ok && user32_send(c, hwnd, WM_CREATE, 0, cs) != 0xFFFFFFFFu) {
        c->esp = sp;
        if (W(hwnd) && (style & 0x10000000u)) {  // WS_VISIBLE
            W(hwnd)->visible = 1;
            W(hwnd)->need_paint = 1;
            user32_send(c, hwnd, WM_SHOWWINDOW, 1, 0);
        }
        return W(hwnd) ? hwnd : 0;
    }
    c->esp = sp;
    destroy_window(c, hwnd);
    return 0;
}

void hle_user32_CreateWindowExA(CPU *c)
{
    HLE_ENTER("CreateWindowExA", 12);
    RET(12, create_window(c, 0, ARG(0), ARG(1), ARG(2), ARG(3), (s32)ARG(4), (s32)ARG(5), (s32)ARG(6), (s32)ARG(7),
                          ARG(8), ARG(9), ARG(10), ARG(11)));
}

void hle_user32_CreateWindowExW(CPU *c)
{
    HLE_ENTER("CreateWindowExW", 12);
    RET(12, create_window(c, 1, ARG(0), ARG(1), ARG(2), ARG(3), (s32)ARG(4), (s32)ARG(5), (s32)ARG(6), (s32)ARG(7),
                          ARG(8), ARG(9), ARG(10), ARG(11)));
}

void hle_user32_DestroyWindow(CPU *c)
{
    HLE_ENTER("DestroyWindow", 1);
    int ok = W(ARG(0)) != NULL;
    destroy_window(c, ARG(0));
    RET(1, ok);
}

// ================================================================ диалоги
static u32 skip_sz_or_ord(u32 p)
{
    u32 w = rd16(g_mem, p);
    if (w == 0)
        return p + 2;
    if (w == 0xFFFF)
        return p + 4;
    while (rd16(g_mem, p))
        p += 2;
    return p + 2;
}

// Окно диалога и дочерние элементы (по шаблону DLGTEMPLATE/DLGTEMPLATEEX); WM_INITDIALOG.
static u32 create_dialog(CPU *c, u32 hinst, u32 tmpl, u32 parent, u32 dlgproc, u32 param, int wide)
{
    int ex = rd16(g_mem, tmpl) == 1 && rd16(g_mem, tmpl + 2) == 0xFFFF;
    u32 style = rd32(g_mem, tmpl + (ex ? 12 : 0));
    u32 n = rd16(g_mem, tmpl + (ex ? 16 : 8));
    u32 p = tmpl + (ex ? 18 : 10);
    s32 cx = (s16)rd16(g_mem, p + 4), cy = (s16)rd16(g_mem, p + 6);
    p = skip_sz_or_ord(p + 8);  // меню
    p = skip_sz_or_ord(p);      // класс
    u32 title = p;
    p = skip_sz_or_ord(p);
    if (style & 0x40) {  // DS_SETFONT
        p += ex ? 6 : 2;
        p = skip_sz_or_ord(p);
    }
    pthread_mutex_lock(&g_lock);
    win *w = new_window();
    if (!w) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    w->is_dialog = 1;
    w->dlgproc = dlgproc;
    w->style = style;
    w->parent = (style & 0x40000000u) ? parent : 0;
    w->owner = (style & 0x40000000u) ? 0 : parent;
    w->hinst = hinst;
    w->w = cx * 2;
    w->h = cy * 2;
    w->wide = wide;
    hle_w2utf8(rd16(g_mem, title) == 0xFFFF ? 0 : title, w->text, sizeof w->text);
    u32 hwnd = hwnd_of(w);
    for (u32 i = 0; i < n; i++) {
        p = (p + 3) & ~3u;
        u32 istyle = rd32(g_mem, p + (ex ? 8 : 0));
        u32 q = p + (ex ? 12 : 8);
        s32 ix = (s16)rd16(g_mem, q), iy = (s16)rd16(g_mem, q + 2), icx = (s16)rd16(g_mem, q + 4), icy = (s16)rd16(g_mem, q + 6);
        u32 id = ex ? rd32(g_mem, q + 8) : rd16(g_mem, q + 8);
        q += ex ? 12 : 10;
        q = skip_sz_or_ord(q);  // класс
        u32 text = q;
        q = skip_sz_or_ord(q);
        q += 2 + rd16(g_mem, q);  // дополнительные данные
        p = q;
        win *ch = new_window();
        if (!ch)
            break;
        ch->style = istyle;
        ch->parent = hwnd;
        ch->id = id;
        ch->hinst = hinst;
        ch->x = ix * 2;
        ch->y = iy * 2;
        ch->w = icx * 2;
        ch->h = icy * 2;
        ch->visible = (istyle & 0x10000000u) != 0;
        ch->wide = wide;
        if (rd16(g_mem, text) != 0xFFFF)
            hle_w2utf8(text, ch->text, sizeof ch->text);
    }
    pthread_mutex_unlock(&g_lock);
    if (getenv("SNX_LOG_WINDOWS"))
        fprintf(stderr, "[user32] dialog '%s' %u items -> %08x\n", w->text, n, hwnd);
    dialog_dispatch(c, W(hwnd), hwnd, WM_INITDIALOG, 0, param);
    if (W(hwnd) && (style & 0x10000000u))
        W(hwnd)->visible = 1;
    return W(hwnd) ? hwnd : 0;
}

static u32 dialog_from_resource(CPU *c, int wide, int modal)
{
    u32 hinst = ARG(0) ? ARG(0) : rt_module_main()->desc->base;
    u32 r = hle_find_resource(hinst, ARG(1), 5 /* RT_DIALOG */, wide);
    if (!r) {
        fprintf(stderr, "[user32] dialog resource %08x not found in %08x\n", ARG(1), hinst);
        return modal ? 0xFFFFFFFFu : 0;
    }
    u32 hwnd = create_dialog(c, hinst, hle_resource_data(hinst, r), ARG(2), ARG(3), ARG(4), wide);
    if (!modal)
        return hwnd;
    // модальный диалог: окна на экране нет, ждём EndDialog из процедуры (обработка сообщений очереди)
    fprintf(stderr, "[user32] modal dialog '%s' — no UI, running its message loop\n", W(hwnd) ? W(hwnd)->text : "?");
    u32 press = 0;
    if (W(hwnd) && user32_modal_hook) {
        static char texts[8192];
        size_t len = 0;
        texts[0] = 0;
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAX_WIN; i++)
            if (g_win[i].used && g_win[i].parent == hwnd && g_win[i].text[0] && len + 2 < sizeof texts)
                len += (size_t)snprintf(texts + len, sizeof texts - len, "%s\n", g_win[i].text);
        char title[sizeof W(hwnd)->text];
        snprintf(title, sizeof title, "%s", W(hwnd)->text);
        pthread_mutex_unlock(&g_lock);
        press = user32_modal_hook(title, texts);
    }
    if (press && W(hwnd)) {
        user32_send(c, hwnd, 0x111 /* WM_COMMAND */, press, 0);
        if (W(hwnd) && !W(hwnd)->ended) {
            W(hwnd)->ended = 1;
            W(hwnd)->dlg_result = press;
        }
    }
    u32 msg = (c->esp -= 32);
    while (W(hwnd) && !W(hwnd)->ended) {
        if (take(msg, 0, 0, 0, 1, 1)) {
            u32 h = rd32(g_mem, msg);
            if (W(h))
                user32_send(c, h, rd32(g_mem, msg + 4), rd32(g_mem, msg + 8), rd32(g_mem, msg + 12));
        }
    }
    c->esp += 32;
    u32 res = W(hwnd) ? W(hwnd)->dlg_result : 0;
    destroy_window(c, hwnd);
    return res;
}

void hle_user32_CreateDialogParamA(CPU *c) { HLE_ENTER("CreateDialogParamA", 5); RET(5, dialog_from_resource(c, 0, 0)); }
void hle_user32_CreateDialogParamW(CPU *c) { HLE_ENTER("CreateDialogParamW", 5); RET(5, dialog_from_resource(c, 1, 0)); }
void hle_user32_DialogBoxParamA(CPU *c) { HLE_ENTER("DialogBoxParamA", 5); RET(5, dialog_from_resource(c, 0, 1)); }
void hle_user32_DialogBoxParamW(CPU *c) { HLE_ENTER("DialogBoxParamW", 5); RET(5, dialog_from_resource(c, 1, 1)); }

void hle_user32_CreateDialogIndirectParamA(CPU *c)
{
    HLE_ENTER("CreateDialogIndirectParamA", 5);
    RET(5, create_dialog(c, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), 0));
}

void hle_user32_EndDialog(CPU *c)
{
    HLE_ENTER("EndDialog", 2);
    win *w = W(ARG(0));
    if (w) {
        w->ended = 1;
        w->dlg_result = ARG(1);
        w->visible = 0;
    }
    RET(2, w != NULL);
}

void hle_user32_GetDlgItem(CPU *c)
{
    HLE_ENTER("GetDlgItem", 2);
    for (int i = 0; i < MAX_WIN; i++)
        if (g_win[i].used && g_win[i].parent == ARG(0) && g_win[i].id == ARG(1))
            RET(2, hwnd_of(&g_win[i]));
    RET(2, 0);
}

void hle_user32_GetDlgCtrlID(CPU *c) { HLE_ENTER("GetDlgCtrlID", 1); RET(1, W(ARG(0)) ? W(ARG(0))->id : 0); }

// ================================================================ сообщения
void hle_user32_SendMessageA(CPU *c) { HLE_ENTER("SendMessageA", 4); RET(4, user32_send(c, ARG(0), ARG(1), ARG(2), ARG(3))); }
void hle_user32_SendMessageW(CPU *c) { HLE_ENTER("SendMessageW", 4); RET(4, user32_send(c, ARG(0), ARG(1), ARG(2), ARG(3))); }

static u32 post_message(CPU *c)
{
    u32 hwnd = ARG(0);
    if (!hwnd) {  // сообщение потоку
        pthread_mutex_lock(&g_lock);
        post(rt_thread_current()->tid, 0, ARG(1), ARG(2), ARG(3));
        pthread_mutex_unlock(&g_lock);
        return 1;
    }
    if (!W(hwnd))
        return 0;
    user32_post(hwnd, ARG(1), ARG(2), ARG(3));
    return 1;
}

void hle_user32_PostMessageA(CPU *c) { HLE_ENTER("PostMessageA", 4); RET(4, post_message(c)); }
void hle_user32_PostMessageW(CPU *c) { HLE_ENTER("PostMessageW", 4); RET(4, post_message(c)); }

void hle_user32_PostQuitMessage(CPU *c)
{
    HLE_ENTER("PostQuitMessage", 1);
    pthread_mutex_lock(&g_lock);
    mqueue *q = queue_of(rt_thread_current()->tid);
    q->quit = 1;
    q->quit_code = ARG(0);
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&g_lock);
    RET(1, 0);
}

// Хук хоста: перед выборкой сообщений главного потока (события SDL -> очередь)
void (*user32_pump_hook)(void);
u32 (*user32_modal_hook)(const char *title, const char *texts);

static void peek_message(CPU *c)
{
    if (user32_pump_hook)
        user32_pump_hook();
    RET(5, take(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4) & 1, 0));
}

void hle_user32_PeekMessageA(CPU *c) { HLE_ENTER("PeekMessageA", 5); peek_message(c); }
void hle_user32_PeekMessageW(CPU *c) { HLE_ENTER("PeekMessageW", 5); peek_message(c); }

void hle_user32_GetMessageW(CPU *c)
{
    HLE_ENTER("GetMessageW", 4);
    if (user32_pump_hook)
        user32_pump_hook();
    while (!take(ARG(0), ARG(1), ARG(2), ARG(3), 1, 1))
        if (user32_pump_hook)
            user32_pump_hook();
    RET(4, rd32(g_mem, ARG(0) + 4) != WM_QUIT);
}

static void dispatch_message(CPU *c)
{
    u32 m = ARG(0), hwnd = rd32(g_mem, m), msg = rd32(g_mem, m + 4);
    u32 r = 0;
    if (msg == WM_TIMER && rd32(g_mem, m + 12))  // TIMERPROC
        r = call_proc(c, rd32(g_mem, m + 12), hwnd, msg, rd32(g_mem, m + 8), rd32(g_mem, m + 16));
    else if (W(hwnd))
        r = user32_send(c, hwnd, msg, rd32(g_mem, m + 8), rd32(g_mem, m + 12));
    RET(1, r);
}

void hle_user32_DispatchMessageA(CPU *c) { HLE_ENTER("DispatchMessageA", 1); dispatch_message(c); }
void hle_user32_DispatchMessageW(CPU *c) { HLE_ENTER("DispatchMessageW", 1); dispatch_message(c); }

void hle_user32_TranslateMessage(CPU *c)
{
    HLE_ENTER("TranslateMessage", 1);
    u32 m = ARG(0), msg = rd32(g_mem, m + 4), vk = rd32(g_mem, m + 8);
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        u32 ch = user32_vk_to_char(vk, (g_keys[0x10] & 0x80) != 0);
        if (ch)
            user32_post(rd32(g_mem, m), msg == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, ch, rd32(g_mem, m + 12));
        RET(1, 1);
    }
    RET(1, 0);
}

void hle_user32_GetMessageTime(CPU *c) { HLE_ENTER("GetMessageTime", 0); RET(0, now_ms()); }
void hle_user32_DefWindowProcA(CPU *c) { HLE_ENTER("DefWindowProcA", 4); RET(4, def_window_proc(c, ARG(0), ARG(1), ARG(2), ARG(3))); }
void hle_user32_DefWindowProcW(CPU *c) { HLE_ENTER("DefWindowProcW", 4); RET(4, def_window_proc(c, ARG(0), ARG(1), ARG(2), ARG(3))); }
void hle_user32_DefMDIChildProcA(CPU *c) { HLE_ENTER("DefMDIChildProcA", 4); RET(4, def_window_proc(c, ARG(0), ARG(1), ARG(2), ARG(3))); }
void hle_user32_DefMDIChildProcW(CPU *c) { HLE_ENTER("DefMDIChildProcW", 4); RET(4, def_window_proc(c, ARG(0), ARG(1), ARG(2), ARG(3))); }

static void call_window_proc(CPU *c)
{
    u32 proc = ARG(0);
    if (!proc || rt_lookup(proc) == NULL)
        RET(5, def_window_proc(c, ARG(1), ARG(2), ARG(3), ARG(4)));
    RET(5, call_proc(c, proc, ARG(1), ARG(2), ARG(3), ARG(4)));
}

void hle_user32_CallWindowProcA(CPU *c) { HLE_ENTER("CallWindowProcA", 5); call_window_proc(c); }
void hle_user32_CallWindowProcW(CPU *c) { HLE_ENTER("CallWindowProcW", 5); call_window_proc(c); }

static u32 g_next_msg = 0xC000;
void hle_user32_RegisterWindowMessageA(CPU *c) { HLE_ENTER("RegisterWindowMessageA", 1); RET(1, __atomic_fetch_add(&g_next_msg, 1, __ATOMIC_RELAXED)); }
void hle_user32_RegisterWindowMessageW(CPU *c) { HLE_ENTER("RegisterWindowMessageW", 1); RET(1, __atomic_fetch_add(&g_next_msg, 1, __ATOMIC_RELAXED)); }

// ================================================================ свойства окна
static u32 window_long(u32 hwnd, s32 idx, int set, u32 val)
{
    win *w = W(hwnd);
    if (!w)
        return 0;
    u32 *f = NULL;
    switch (idx) {
    case -4: f = &w->proc; break;      // GWL_WNDPROC
    case -6: f = &w->hinst; break;     // GWL_HINSTANCE
    case -8: f = &w->owner; break;     // GWL_HWNDPARENT
    case -12: f = &w->id; break;       // GWL_ID
    case -16: f = &w->style; break;    // GWL_STYLE
    case -20: f = &w->exstyle; break;  // GWL_EXSTYLE
    case -21: f = &w->userdata; break; // GWL_USERDATA
    default:
        if (w->is_dialog && idx == 0)
            f = &w->dlg_msgresult;     // DWL_MSGRESULT
        else if (w->is_dialog && idx == 4)
            f = &w->dlgproc;           // DWL_DLGPROC
        else if (w->is_dialog && idx == 8)
            f = &w->dlg_user;          // DWL_USER
        else if (idx >= 0 && idx / 4 < 32)
            f = &w->extra[idx / 4];
    }
    if (!f)
        return 0;
    u32 old = *f;
    if (set)
        *f = val;
    return old;
}

void hle_user32_GetWindowLongA(CPU *c) { HLE_ENTER("GetWindowLongA", 2); RET(2, window_long(ARG(0), (s32)ARG(1), 0, 0)); }
void hle_user32_GetWindowLongW(CPU *c) { HLE_ENTER("GetWindowLongW", 2); RET(2, window_long(ARG(0), (s32)ARG(1), 0, 0)); }
void hle_user32_SetWindowLongA(CPU *c) { HLE_ENTER("SetWindowLongA", 3); RET(3, window_long(ARG(0), (s32)ARG(1), 1, ARG(2))); }
void hle_user32_SetWindowLongW(CPU *c) { HLE_ENTER("SetWindowLongW", 3); RET(3, window_long(ARG(0), (s32)ARG(1), 1, ARG(2))); }

static u32 prop(CPU *c, int wide, int op)  // 0 set, 1 get, 2 remove
{
    win *w = W(ARG(0));
    if (!w)
        return 0;
    char name[64] = "";
    u32 atom = ARG(1) < 0x10000 ? ARG(1) : 0;
    if (!atom)
        gstr(ARG(1), wide, name, sizeof name);
    for (int i = 0; i < 8; i++) {
        if (w->props[i].name[0] || w->props[i].atom) {
            if (atom ? w->props[i].atom != atom : strcasecmp(w->props[i].name, name))
                continue;
            u32 v = w->props[i].value;
            if (op == 0)
                w->props[i].value = ARG(2);
            if (op == 2)
                memset(&w->props[i], 0, sizeof w->props[i]);
            return op == 0 ? 1 : v;
        }
    }
    if (op != 0)
        return 0;
    for (int i = 0; i < 8; i++)
        if (!w->props[i].name[0] && !w->props[i].atom) {
            snprintf(w->props[i].name, sizeof w->props[i].name, "%s", name);
            w->props[i].atom = atom;
            w->props[i].value = ARG(2);
            return 1;
        }
    return 0;
}

void hle_user32_SetPropA(CPU *c) { HLE_ENTER("SetPropA", 3); RET(3, prop(c, 0, 0)); }
void hle_user32_SetPropW(CPU *c) { HLE_ENTER("SetPropW", 3); RET(3, prop(c, 1, 0)); }
void hle_user32_RemovePropA(CPU *c) { HLE_ENTER("RemovePropA", 2); RET(2, prop(c, 0, 2)); }
void hle_user32_RemovePropW(CPU *c) { HLE_ENTER("RemovePropW", 2); RET(2, prop(c, 1, 2)); }

void hle_user32_GetWindowTextA(CPU *c) { HLE_ENTER("GetWindowTextA", 3); RET(3, W(ARG(0)) ? hle_put_a(ARG(1), ARG(2), W(ARG(0))->text) : 0); }
void hle_user32_GetParent(CPU *c) { HLE_ENTER("GetParent", 1); RET(1, W(ARG(0)) ? (W(ARG(0))->parent ? W(ARG(0))->parent : W(ARG(0))->owner) : 0); }

void hle_user32_SetParent(CPU *c)
{
    HLE_ENTER("SetParent", 2);
    win *w = W(ARG(0));
    u32 old = w ? w->parent : 0;
    if (w)
        w->parent = ARG(1);
    RET(2, old);
}

void hle_user32_GetWindow(CPU *c)
{
    HLE_ENTER("GetWindow", 2);
    win *w = W(ARG(0));
    if (!w)
        RET(2, 0);
    if (ARG(1) == 4)  // GW_OWNER
        RET(2, w->owner);
    if (ARG(1) == 5)  // GW_CHILD
        for (int i = 0; i < MAX_WIN; i++)
            if (g_win[i].used && g_win[i].parent == ARG(0))
                RET(2, hwnd_of(&g_win[i]));
    if (ARG(1) == 2)  // GW_HWNDNEXT
        for (int i = (int)(w - g_win) + 1; i < MAX_WIN; i++)
            if (g_win[i].used && g_win[i].parent == w->parent)
                RET(2, hwnd_of(&g_win[i]));
    RET(2, 0);
}

void hle_user32_EnumChildWindows(CPU *c)
{
    HLE_ENTER("EnumChildWindows", 3);
    u32 parent = ARG(0), proc = ARG(1), lp = ARG(2);
    for (int i = 0; i < MAX_WIN; i++)
        if (g_win[i].used && g_win[i].parent == parent) {
            u32 args[2] = {hwnd_of(&g_win[i]), lp};
            if (!rt_guest_call(c, proc, 2, args))
                break;
        }
    RET(3, 1);
}

void hle_user32_IsWindow(CPU *c) { HLE_ENTER("IsWindow", 1); RET(1, W(ARG(0)) != NULL || ARG(0) == HWND_DESKTOP_); }
void hle_user32_IsWindowEnabled(CPU *c) { HLE_ENTER("IsWindowEnabled", 1); RET(1, W(ARG(0)) && W(ARG(0))->enabled); }
void hle_user32_IsIconic(CPU *c) { HLE_ENTER("IsIconic", 1); RET(1, 0); }
void hle_user32_IsZoomed(CPU *c) { HLE_ENTER("IsZoomed", 1); RET(1, 0); }

void hle_user32_EnableWindow(CPU *c)
{
    HLE_ENTER("EnableWindow", 2);
    win *w = W(ARG(0));
    int was = w ? !w->enabled : 0;
    if (w)
        w->enabled = ARG(1) != 0;
    RET(2, was);
}

void hle_user32_GetWindowThreadProcessId(CPU *c)
{
    HLE_ENTER("GetWindowThreadProcessId", 2);
    if (ARG(1))
        wr32(g_mem, ARG(1), 0x100);
    RET(2, W(ARG(0)) ? W(ARG(0))->tid : rt_thread_current()->tid);
}

// ================================================================ геометрия и показ
static void put_rect(u32 p, s32 l, s32 t, s32 r, s32 b)
{
    wr32(g_mem, p, (u32)l);
    wr32(g_mem, p + 4, (u32)t);
    wr32(g_mem, p + 8, (u32)r);
    wr32(g_mem, p + 12, (u32)b);
}

static void origin(u32 hwnd, s32 *x, s32 *y)  // экранные координаты клиентской области
{
    *x = *y = 0;
    for (win *w = W(hwnd); w; w = W(w->parent)) {
        *x += w->x;
        *y += w->y;
    }
}

void hle_user32_GetClientRect(CPU *c)
{
    HLE_ENTER("GetClientRect", 2);
    win *w = W(ARG(0));
    if (ARG(0) == HWND_DESKTOP_ || !w)
        put_rect(ARG(1), 0, 0, g_screen_w, g_screen_h);
    else
        put_rect(ARG(1), 0, 0, w->w, w->h);
    RET(2, w != NULL || ARG(0) == HWND_DESKTOP_);
}

int user32_client_size(u32 hwnd, int *w, int *h)
{
    win *x = W(hwnd);
    *w = x ? x->w : g_screen_w;
    *h = x ? x->h : g_screen_h;
    return x != NULL;
}

u32 user32_focus(void) { return g_focus ? g_focus : g_active; }

void hle_user32_GetWindowRect(CPU *c)
{
    HLE_ENTER("GetWindowRect", 2);
    win *w = W(ARG(0));
    if (!w) {
        put_rect(ARG(1), 0, 0, g_screen_w, g_screen_h);
        RET(2, ARG(0) == HWND_DESKTOP_);
    }
    s32 x, y;
    origin(ARG(0), &x, &y);
    put_rect(ARG(1), x, y, x + w->w, y + w->h);
    RET(2, 1);
}

void hle_user32_AdjustWindowRect(CPU *c) { HLE_ENTER("AdjustWindowRect", 3); RET(3, 1); }  // окна без рамки

static void move_window(CPU *c, u32 hwnd, s32 x, s32 y, s32 cx, s32 cy, int move, int size)
{
    win *w = W(hwnd);
    if (!w)
        return;
    if (move) {
        w->x = x;
        w->y = y;
    }
    if (size) {
        w->need_paint |= w->w != cx || w->h != cy;
        w->w = cx;
        w->h = cy;
    }
    if (move)
        user32_send(c, hwnd, WM_MOVE, 0, (u32)(x & 0xFFFF) | (u32)y << 16);
    if (size)
        user32_send(c, hwnd, WM_SIZE, 0, (u32)(cx & 0xFFFF) | (u32)cy << 16);
}

// Активация окна верхнего уровня, как при щелчке/SetForegroundWindow: WM_ACTIVATEAPP (первый раз),
// WM_NCACTIVATE, WM_ACTIVATE (DefWindowProc ставит фокус -> WM_SETFOCUS)
static int g_app_active;

static void activate(CPU *c, u32 hwnd)
{
    u32 old = g_active;
    g_foreground = hwnd;
    if (old == hwnd)
        return;
    if (old && W(old)) {
        user32_send(c, old, WM_NCACTIVATE, 0, 0);
        user32_send(c, old, WM_ACTIVATE, 0, hwnd);
    }
    g_active = hwnd;
    if (!g_app_active) {
        g_app_active = 1;
        user32_send(c, hwnd, WM_ACTIVATEAPP, 1, 0);
    }
    user32_send(c, hwnd, WM_NCACTIVATE, 1, 0);
    user32_send(c, hwnd, WM_ACTIVATE, 1, old);
}

void hle_user32_SetWindowPos(CPU *c)
{
    HLE_ENTER("SetWindowPos", 7);
    u32 f = ARG(6);
    move_window(c, ARG(0), (s32)ARG(2), (s32)ARG(3), (s32)ARG(4), (s32)ARG(5), !(f & 2), !(f & 1));
    win *w = W(ARG(0));
    if (w && (f & 0x40))
        w->visible = 1;
    if (w && (f & 0x80))
        w->visible = 0;
    RET(7, w != NULL);
}

void hle_user32_MoveWindow(CPU *c)
{
    HLE_ENTER("MoveWindow", 6);
    move_window(c, ARG(0), (s32)ARG(1), (s32)ARG(2), (s32)ARG(3), (s32)ARG(4), 1, 1);
    RET(6, W(ARG(0)) != NULL);
}

void hle_user32_ShowWindow(CPU *c)
{
    HLE_ENTER("ShowWindow", 2);
    win *w = W(ARG(0));
    if (!w)
        RET(2, 0);
    int was = w->visible, show = ARG(1) != 0;
    w->visible = show;
    if (was != show)
        user32_send(c, ARG(0), WM_SHOWWINDOW, (u32)show, 0);
    if (show && !was) {
        w->need_paint = 1;
        user32_send(c, ARG(0), WM_SIZE, 0, (u32)(w->w & 0xFFFF) | (u32)w->h << 16);
        if (ARG(1) != 4 && ARG(1) != 8 && !(w->style & 0x40000000u))  // SW_SHOWNOACTIVATE, SW_SHOWNA; не дочернее
            activate(c, ARG(0));
    }
    RET(2, was);
}

void hle_user32_UpdateWindow(CPU *c)
{
    HLE_ENTER("UpdateWindow", 1);
    win *w = W(ARG(0));
    if (w && w->need_paint && w->visible)
        user32_send(c, ARG(0), WM_PAINT, 0, 0);
    RET(1, w != NULL);
}

static void set_need_paint(u32 hwnd, int v)
{
    if (!hwnd) {  // все окна
        for (int i = 0; i < MAX_WIN; i++)
            if (g_win[i].used)
                g_win[i].need_paint = v;
    } else if (W(hwnd))
        W(hwnd)->need_paint = v;
}

void hle_user32_InvalidateRect(CPU *c) { HLE_ENTER("InvalidateRect", 3); set_need_paint(ARG(0), 1); RET(3, 1); }
void hle_user32_ValidateRect(CPU *c) { HLE_ENTER("ValidateRect", 2); set_need_paint(ARG(0), 0); RET(2, 1); }

void hle_user32_GetUpdateRect(CPU *c)
{
    HLE_ENTER("GetUpdateRect", 3);
    if (ARG(1))
        put_rect(ARG(1), 0, 0, 0, 0);
    RET(3, 0);
}

void hle_user32_BeginPaint(CPU *c)
{
    HLE_ENTER("BeginPaint", 2);
    u32 ps = ARG(1);
    memset(g_mem + ps, 0, 64);
    u32 dc = gdi32_window_dc(ARG(0));
    wr32(g_mem, ps, dc);
    win *w = W(ARG(0));
    if (w)
        w->need_paint = 0;
    put_rect(ps + 8, 0, 0, w ? w->w : 0, w ? w->h : 0);
    RET(2, dc);
}

void hle_user32_EndPaint(CPU *c) { HLE_ENTER("EndPaint", 2); RET(2, 1); }
void hle_user32_BringWindowToTop(CPU *c) { HLE_ENTER("BringWindowToTop", 1); RET(1, 1); }
void hle_user32_BeginDeferWindowPos(CPU *c) { HLE_ENTER("BeginDeferWindowPos", 1); RET(1, 0x7FF0); }
void hle_user32_EndDeferWindowPos(CPU *c) { HLE_ENTER("EndDeferWindowPos", 1); RET(1, 1); }

static void map_points(u32 from, u32 to, u32 pts, u32 n)
{
    s32 fx, fy, tx, ty;
    origin(from, &fx, &fy);
    origin(to, &tx, &ty);
    for (u32 i = 0; i < n; i++) {
        wr32(g_mem, pts + 8 * i, rd32(g_mem, pts + 8 * i) + (u32)(fx - tx));
        wr32(g_mem, pts + 8 * i + 4, rd32(g_mem, pts + 8 * i + 4) + (u32)(fy - ty));
    }
}

void hle_user32_ClientToScreen(CPU *c) { HLE_ENTER("ClientToScreen", 2); map_points(ARG(0), 0, ARG(1), 1); RET(2, 1); }
void hle_user32_ScreenToClient(CPU *c) { HLE_ENTER("ScreenToClient", 2); map_points(0, ARG(0), ARG(1), 1); RET(2, 1); }
void hle_user32_MapWindowPoints(CPU *c) { HLE_ENTER("MapWindowPoints", 4); map_points(ARG(0), ARG(1), ARG(2), ARG(3)); RET(4, 0); }

void hle_user32_WindowFromPoint(CPU *c)
{
    HLE_ENTER("WindowFromPoint", 2);
    RET(2, g_active ? g_active : HWND_DESKTOP_);
}

// ================================================================ фокус, захват, курсор, клавиатура
// Webhead.exe проверяет FindWindowExA перед запуском («игра уже запущена?»). В порте
// сторонних окон нет — всегда нет совпадения; добавил для Spider-Man 2.
void hle_user32_FindWindowA(CPU *c) { HLE_ENTER("FindWindowA", 2); RET(2, 0); }
void hle_user32_FindWindowW(CPU *c) { HLE_ENTER("FindWindowW", 2); RET(2, 0); }
void hle_user32_FindWindowExA(CPU *c) { HLE_ENTER("FindWindowExA", 4); RET(4, 0); }
void hle_user32_FindWindowExW(CPU *c) { HLE_ENTER("FindWindowExW", 4); RET(4, 0); }

void hle_user32_GetDesktopWindow(CPU *c) { HLE_ENTER("GetDesktopWindow", 0); RET(0, HWND_DESKTOP_); }
void hle_user32_GetActiveWindow(CPU *c) { HLE_ENTER("GetActiveWindow", 0); RET(0, g_active); }
void hle_user32_GetForegroundWindow(CPU *c) { HLE_ENTER("GetForegroundWindow", 0); RET(0, g_foreground); }
void hle_user32_GetFocus(CPU *c) { HLE_ENTER("GetFocus", 0); RET(0, g_focus); }
void hle_user32_GetCapture(CPU *c) { HLE_ENTER("GetCapture", 0); RET(0, g_capture); }

void hle_user32_SetActiveWindow(CPU *c)
{
    HLE_ENTER("SetActiveWindow", 1);
    u32 old = g_active;
    if (W(ARG(0)))
        activate(c, ARG(0));
    RET(1, old);
}

void hle_user32_SetForegroundWindow(CPU *c)
{
    HLE_ENTER("SetForegroundWindow", 1);
    if (W(ARG(0)))
        activate(c, ARG(0));
    RET(1, W(ARG(0)) != NULL);
}

static u32 set_focus(CPU *c, u32 hwnd)
{
    u32 old = g_focus;
    g_focus = hwnd;
    if (old != g_focus) {
        if (old && W(old))
            user32_send(c, old, WM_KILLFOCUS, g_focus, 0);
        if (g_focus && W(g_focus))
            user32_send(c, g_focus, WM_SETFOCUS, old, 0);
    }
    return old;
}

void hle_user32_SetFocus(CPU *c) { HLE_ENTER("SetFocus", 1); RET(1, set_focus(c, ARG(0))); }

void hle_user32_SetCapture(CPU *c)
{
    HLE_ENTER("SetCapture", 1);
    u32 old = g_capture;
    g_capture = ARG(0);
    RET(1, old);
}

void hle_user32_ReleaseCapture(CPU *c) { HLE_ENTER("ReleaseCapture", 0); g_capture = 0; RET(0, 1); }
void hle_user32_AttachThreadInput(CPU *c) { HLE_ENTER("AttachThreadInput", 3); RET(3, 1); }

void user32_set_cursor_pos(int x, int y)
{
    g_cursor_x = x;
    g_cursor_y = y;
}

void hle_user32_GetCursorPos(CPU *c)
{
    HLE_ENTER("GetCursorPos", 1);
    wr32(g_mem, ARG(0), (u32)g_cursor_x);
    wr32(g_mem, ARG(0) + 4, (u32)g_cursor_y);
    RET(1, 1);
}

void hle_user32_SetCursorPos(CPU *c)
{
    HLE_ENTER("SetCursorPos", 2);
    g_cursor_x = (s32)ARG(0);
    g_cursor_y = (s32)ARG(1);
    RET(2, 1);
}

void hle_user32_ShowCursor(CPU *c)
{
    HLE_ENTER("ShowCursor", 1);
    g_show_cursor += ARG(0) ? 1 : -1;
    RET(1, (u32)g_show_cursor);
}

static u32 g_cursor;
void hle_user32_SetCursor(CPU *c) { HLE_ENTER("SetCursor", 1); u32 o = g_cursor; g_cursor = ARG(0); RET(1, o); }
void hle_user32_GetCursor(CPU *c) { HLE_ENTER("GetCursor", 0); RET(0, g_cursor); }
void hle_user32_ClipCursor(CPU *c) { HLE_ENTER("ClipCursor", 1); RET(1, 1); }
void hle_user32_LoadCursorA(CPU *c) { HLE_ENTER("LoadCursorA", 2); RET(2, 0x00030000u | (ARG(1) & 0xFFFF)); }
void hle_user32_LoadCursorW(CPU *c) { HLE_ENTER("LoadCursorW", 2); RET(2, 0x00030000u | (ARG(1) & 0xFFFF)); }
void hle_user32_LoadIconA(CPU *c) { HLE_ENTER("LoadIconA", 2); RET(2, 0x00031000u | (ARG(1) & 0xFFF)); }
void hle_user32_LoadIconW(CPU *c) { HLE_ENTER("LoadIconW", 2); RET(2, 0x00031000u | (ARG(1) & 0xFFF)); }

void hle_user32_LoadImageA(CPU *c)
{
    HLE_ENTER("LoadImageA", 6);
    hle_todo("LoadImageA (returns a fake handle)");
    RET(6, 0x00032000u | (ARG(1) & 0xFFF));
}

void user32_set_key(u32 vk, int down)
{
    if (vk < 256)
        g_keys[vk] = down ? (u8)(0x80 | (g_keys[vk] & 1) ^ 1) : (u8)(g_keys[vk] & 1);
}

void hle_user32_GetKeyState(CPU *c) { HLE_ENTER("GetKeyState", 1); RET(1, ARG(0) < 256 ? (u32)(s16)(s8)g_keys[ARG(0)] : 0); }

void hle_user32_SetKeyboardState(CPU *c)
{
    HLE_ENTER("SetKeyboardState", 1);
    memcpy(g_keys, g_mem + ARG(0), 256);
    RET(1, 1);
}

u32 user32_vk_to_char(u32 vk, int shift)
{
    if (vk >= 'A' && vk <= 'Z')
        return shift ? vk : vk + 32;
    if (vk >= '0' && vk <= '9')
        return shift ? (u32)")!@#$%^&*("[vk - '0'] : vk;
    switch (vk) {
    case 0x20: return ' ';
    case 0x0D: return '\r';
    case 0x08: return 8;
    case 0x09: return 9;
    case 0x1B: return 27;
    case 0xBA: return shift ? ':' : ';';
    case 0xBB: return shift ? '+' : '=';
    case 0xBC: return shift ? '<' : ',';
    case 0xBD: return shift ? '_' : '-';
    case 0xBE: return shift ? '>' : '.';
    case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`';
    case 0xDB: return shift ? '{' : '[';
    case 0xDC: return shift ? '|' : '\\';
    case 0xDD: return shift ? '}' : ']';
    case 0xDE: return shift ? '"' : '\'';
    default: return 0;
    }
}

void hle_user32_MapVirtualKeyW(CPU *c)
{
    HLE_ENTER("MapVirtualKeyW", 2);
    u32 code = ARG(0), type = ARG(1);
    RET(2, type == 2 ? user32_vk_to_char(code, 0) : code);  // MAPVK_VK_TO_CHAR; скан-коды не моделируются
}

void hle_user32_GetKeyNameTextW(CPU *c)
{
    HLE_ENTER("GetKeyNameTextW", 3);
    char name[16];
    snprintf(name, sizeof name, "Key%u", (ARG(0) >> 16) & 0xFF);
    RET(3, hle_put_w(ARG(1), ARG(2), name));
}

void hle_user32_RegisterHotKey(CPU *c) { HLE_ENTER("RegisterHotKey", 4); RET(4, 1); }
void hle_user32_UnregisterHotKey(CPU *c) { HLE_ENTER("UnregisterHotKey", 2); RET(2, 1); }

// ================================================================ система
void hle_user32_GetSystemMetrics(CPU *c)
{
    HLE_ENTER("GetSystemMetrics", 1);
    switch (ARG(0)) {
    case 0: case 16: case 78: RET(1, (u32)g_screen_w);  // SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN
    case 1: case 17: case 79: RET(1, (u32)g_screen_h);
    case 76: case 77: RET(1, 0);                         // SM_X/YVIRTUALSCREEN
    case 80: RET(1, 1);                                  // SM_CMONITORS
    case 4: RET(1, 19);                                  // SM_CYCAPTION
    case 5: case 6: RET(1, 1);                           // SM_CXBORDER
    case 7: case 8: case 32: case 33: RET(1, 4);         // SM_CXDLGFRAME, SM_CXFRAME
    case 11: case 12: RET(1, 32);                        // SM_CXICON
    case 13: case 14: RET(1, 32);                        // SM_CXCURSOR
    case 15: RET(1, 19);                                 // SM_CYMENU
    case 19: RET(1, 1);                                  // SM_MOUSEPRESENT
    case 43: RET(1, 3);                                  // SM_CMOUSEBUTTONS
    default:
        if (g_hle_trace)
            fprintf(stderr, "[user32] GetSystemMetrics(%u) -> 0\n", ARG(0));
        RET(1, 0);
    }
}

void hle_user32_SystemParametersInfoA(CPU *c)
{
    HLE_ENTER("SystemParametersInfoA", 4);
    if (ARG(0) == 0x30 && ARG(2))  // SPI_GETWORKAREA
        put_rect(ARG(2), 0, 0, g_screen_w, g_screen_h);
    else if (ARG(2) && (ARG(0) == 0x0A || ARG(0) == 0x16 || ARG(0) == 0x68))  // SPI_GETKEYBOARDSPEED/DELAY/…
        wr32(g_mem, ARG(2), 0);
    RET(4, 1);
}

void hle_user32_SystemParametersInfoW(CPU *c)
{
    HLE_ENTER("SystemParametersInfoW", 4);
    hle_user32_SystemParametersInfoA(c);  // та же логика
}

void hle_user32_GetSysColor(CPU *c) { HLE_ENTER("GetSysColor", 1); RET(1, ARG(0) == 5 ? 0xFFFFFF : 0xC0C0C0); }
void hle_user32_GetSysColorBrush(CPU *c) { HLE_ENTER("GetSysColorBrush", 1); RET(1, gdi32_stock(0)); }

void hle_user32_MessageBeep(CPU *c) { HLE_ENTER("MessageBeep", 1); RET(1, 1); }

static void message_box(CPU *c, int wide)
{
    char text[2048], cap[256];
    gstr(ARG(1), wide, text, sizeof text);
    gstr(ARG(2), wide, cap, sizeof cap);
    fprintf(stderr, "[user32] MessageBox \"%s\": %s\n", cap, text);
    u32 type = ARG(3) & 0xF;
    // MB_OK -> IDOK, MB_OKCANCEL -> IDOK, MB_YESNO(CANCEL) -> IDYES, MB_RETRYCANCEL -> IDCANCEL
    RET(4, type == 4 || type == 3 ? 6 : type == 5 ? 2 : 1);
}

void hle_user32_MessageBoxA(CPU *c) { HLE_ENTER("MessageBoxA", 4); message_box(c, 0); }
void hle_user32_MessageBoxW(CPU *c) { HLE_ENTER("MessageBoxW", 4); message_box(c, 1); }

// ================================================================ меню, буфер обмена, отрисовка элементов (редактор)
void hle_user32_CreatePopupMenu(CPU *c) { HLE_ENTER("CreatePopupMenu", 0); RET(0, 0x00034000u); }
void hle_user32_DestroyMenu(CPU *c) { HLE_ENTER("DestroyMenu", 1); RET(1, 1); }
void hle_user32_GetMenu(CPU *c) { HLE_ENTER("GetMenu", 1); RET(1, W(ARG(0)) ? W(ARG(0))->menu : 0); }
void hle_user32_SetMenu(CPU *c) { HLE_ENTER("SetMenu", 2); if (W(ARG(0))) W(ARG(0))->menu = ARG(1); RET(2, 1); }
void hle_user32_LoadMenuA(CPU *c) { HLE_ENTER("LoadMenuA", 2); RET(2, 0x00034000u | (ARG(1) & 0xFFF)); }
void hle_user32_LoadMenuW(CPU *c) { HLE_ENTER("LoadMenuW", 2); RET(2, 0x00034000u | (ARG(1) & 0xFFF)); }
void hle_user32_GetSubMenu(CPU *c) { HLE_ENTER("GetSubMenu", 2); RET(2, 0); }
void hle_user32_GetMenuItemCount(CPU *c) { HLE_ENTER("GetMenuItemCount", 1); RET(1, 0); }
void hle_user32_GetMenuItemInfoA(CPU *c) { HLE_ENTER("GetMenuItemInfoA", 4); RET(4, 0); }
void hle_user32_GetMenuItemInfoW(CPU *c) { HLE_ENTER("GetMenuItemInfoW", 4); RET(4, 0); }
void hle_user32_SetMenuItemInfoA(CPU *c) { HLE_ENTER("SetMenuItemInfoA", 4); RET(4, 1); }
void hle_user32_SetMenuItemInfoW(CPU *c) { HLE_ENTER("SetMenuItemInfoW", 4); RET(4, 1); }
void hle_user32_InsertMenuItemA(CPU *c) { HLE_ENTER("InsertMenuItemA", 4); RET(4, 1); }
void hle_user32_GetMenuState(CPU *c) { HLE_ENTER("GetMenuState", 3); RET(3, 0); }
void hle_user32_TrackPopupMenu(CPU *c) { HLE_ENTER("TrackPopupMenu", 7); RET(7, 0); }
void hle_user32_OpenClipboard(CPU *c) { HLE_ENTER("OpenClipboard", 1); RET(1, 1); }
void hle_user32_CloseClipboard(CPU *c) { HLE_ENTER("CloseClipboard", 0); RET(0, 1); }
void hle_user32_EmptyClipboard(CPU *c) { HLE_ENTER("EmptyClipboard", 0); RET(0, 1); }
void hle_user32_GetClipboardData(CPU *c) { HLE_ENTER("GetClipboardData", 1); RET(1, 0); }
void hle_user32_SetClipboardData(CPU *c) { HLE_ENTER("SetClipboardData", 2); RET(2, ARG(1)); }
void hle_user32_DrawEdge(CPU *c) { HLE_ENTER("DrawEdge", 4); RET(4, 1); }
void hle_user32_DrawFocusRect(CPU *c) { HLE_ENTER("DrawFocusRect", 2); RET(2, 1); }
void hle_user32_FillRect(CPU *c) { HLE_ENTER("FillRect", 3); RET(3, 1); }
void hle_user32_DrawTextA(CPU *c) { HLE_ENTER("DrawTextA", 5); RET(5, 16); }
void hle_user32_DrawTextW(CPU *c) { HLE_ENTER("DrawTextW", 5); RET(5, 16); }
void hle_user32_DrawTextExA(CPU *c) { HLE_ENTER("DrawTextExA", 6); RET(6, 16); }
void hle_user32_DrawTextExW(CPU *c) { HLE_ENTER("DrawTextExW", 6); RET(6, 16); }

void hle_user32_GetDC(CPU *c) { HLE_ENTER("GetDC", 1); RET(1, gdi32_window_dc(ARG(0))); }
void hle_user32_ReleaseDC(CPU *c) { HLE_ENTER("ReleaseDC", 2); RET(2, 1); }
