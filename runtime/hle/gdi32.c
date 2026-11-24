// HLE gdi32: объекты GDI (контексты, битмапы и DIB-секции с битами в гостевой памяти, кисти, перья, шрифты,
// стоковые объекты) и вывод без экрана. Игра рисует через D3D; GDI ей нужен для заставки, диалогов и шрифтов:
// локализованные версии (русская — SAPFont.rus: Tahoma) не берут готовые шрифты из SH_Fonts.utx, а растеризуют
// TrueType через GDI — WinDrv рисует символ TextOutW в 8-битный DIB и копирует пиксели в страницу UFont. Поэтому
// замер (GetTextExtentPoint32) и вывод текста (TextOutW, PatBlt) в битмапы гостя — настоящие, через FreeType
// (шрифт по имени: на маке — системный Tahoma/Arial, на Switch — системный шрифт консоли). BitBlt и т. п. — без
// вывода.
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#ifdef __SWITCH__
#include <switch.h>
#endif

#include "runtime/heap.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/user32.h"

#define MAX_GDI 2048
#define GDI_BASE 0x00050000u

enum { G_NONE, G_DC, G_BITMAP, G_BRUSH, G_PEN, G_FONT, G_PALETTE };

typedef struct {
    int type, stock;
    // DC
    u32 hwnd, bitmap, brush, pen, font, text_color, bk_color, bk_mode;
    s32 cur_x, cur_y;
    int window_dc;
    // битмап: биты в гостевой памяти, строки снизу вверх (DIB) или сверху вниз (height < 0)
    s32 w, h;
    u32 bpp, stride, bits, bits_owned, dib;
    u8 bih[40];  // BITMAPINFOHEADER DIB-секции (GetObject -> DIBSECTION)
    u32 *pal;    // палитра DIB-секции с 1/4/8 бит (0x00RRGGBB), npal записей
    u32 npal;
    // кисть/перо
    u32 color, style, width;
    // шрифт
    s32 font_h;
    u32 font_weight, font_quality;
    char face[32];
    FT_Face ft;  // открывается при первом замере/выводе (font_face)
} gdiobj;

static gdiobj g_gdi[MAX_GDI];
static pthread_mutex_t g_gdi_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 g_stock[20];

static gdiobj *G(u32 h, int type)
{
    if (h < GDI_BASE || (h - GDI_BASE) % 4 || (h - GDI_BASE) / 4 >= MAX_GDI)
        return NULL;
    gdiobj *o = &g_gdi[(h - GDI_BASE) / 4];
    return o->type != G_NONE && (!type || o->type == type) ? o : NULL;
}

static u32 gdi_new(int type, gdiobj **out)
{
    pthread_mutex_lock(&g_gdi_lock);
    for (int i = 1; i < MAX_GDI; i++)
        if (g_gdi[i].type == G_NONE) {
            memset(&g_gdi[i], 0, sizeof g_gdi[i]);
            g_gdi[i].type = type;
            pthread_mutex_unlock(&g_gdi_lock);
            *out = &g_gdi[i];
            return GDI_BASE + 4u * (u32)i;
        }
    pthread_mutex_unlock(&g_gdi_lock);
    hle_todo("gdi32: out of GDI handles");
    *out = NULL;
    return 0;
}

// Стоковые объекты: 0-5 кисти (WHITE, LTGRAY, GRAY, DKGRAY, BLACK, NULL), 6-8 перья (WHITE, BLACK, NULL),
// 10-17 шрифты, 15 DEFAULT_PALETTE, 18 DC_BRUSH, 19 DC_PEN.
static void stock_create(void)
{
    static const u32 gray[6] = {0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000, 0};
    for (int i = 0; i < 20; i++) {
        gdiobj *o;
        int type = i <= 5 || i == 18 ? G_BRUSH : i <= 8 || i == 19 ? G_PEN : i == 15 ? G_PALETTE : G_FONT;
        if (i == 9)
            continue;
        g_stock[i] = gdi_new(type, &o);
        o->stock = 1;
        o->color = i <= 5 ? gray[i] : i == 6 ? 0xFFFFFF : 0;
        o->style = i == 5 || i == 8 ? 5 /* NULL */ : 0;
        if (type == G_FONT) {
            o->font_h = 16;
            o->font_weight = 400;
            snprintf(o->face, sizeof o->face, "System");
        }
    }
}

static void stock_init(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, stock_create);
}

u32 gdi32_stock(int i)
{
    stock_init();
    return i >= 0 && i < 20 ? g_stock[i] : 0;
}

static u32 new_dc(u32 hwnd, int window_dc)
{
    stock_init();
    gdiobj *d;
    u32 h = gdi_new(G_DC, &d);
    if (!d)
        return 0;
    d->hwnd = hwnd;
    d->window_dc = window_dc;
    d->brush = g_stock[0];
    d->pen = g_stock[7];
    d->font = g_stock[13];  // SYSTEM_FONT
    d->bk_color = 0xFFFFFF;
    d->bk_mode = 2;  // OPAQUE
    return h;
}

u32 gdi32_window_dc(u32 hwnd)
{
    pthread_mutex_lock(&g_gdi_lock);
    for (int i = 1; i < MAX_GDI; i++)
        if (g_gdi[i].type == G_DC && g_gdi[i].window_dc && g_gdi[i].hwnd == hwnd) {
            pthread_mutex_unlock(&g_gdi_lock);
            return GDI_BASE + 4u * (u32)i;
        }
    pthread_mutex_unlock(&g_gdi_lock);
    return new_dc(hwnd, 1);
}

// ================================================================ контексты
void hle_gdi32_CreateCompatibleDC(CPU *c) { HLE_ENTER("CreateCompatibleDC", 1); RET(1, new_dc(0, 0)); }

void hle_gdi32_DeleteDC(CPU *c)
{
    HLE_ENTER("DeleteDC", 1);
    gdiobj *d = G(ARG(0), G_DC);
    if (d && !d->window_dc)
        d->type = G_NONE;
    RET(1, d != NULL);
}

void hle_gdi32_GetDeviceCaps(CPU *c)
{
    HLE_ENTER("GetDeviceCaps", 2);
    int w, h;
    user32_screen(&w, &h);
    switch (ARG(1)) {
    case 2: RET(2, 1);               // TECHNOLOGY: DT_RASDISPLAY
    case 4: RET(2, (u32)w / 4);      // HORZSIZE, мм при 96 dpi ≈ 0.26 мм/пиксель
    case 6: RET(2, (u32)h / 4);      // VERTSIZE
    case 8: case 118: RET(2, (u32)w);  // HORZRES, DESKTOPHORZRES
    case 10: case 117: RET(2, (u32)h); // VERTRES, DESKTOPVERTRES
    case 12: RET(2, 32);             // BITSPIXEL
    case 14: RET(2, 1);              // PLANES
    case 24: RET(2, 0xFFFFFFFFu);    // NUMCOLORS: больше 256
    case 38: RET(2, 0x7E99);         // RASTERCAPS: BITBLT, BITMAP64, DI_BITMAP, DIBTODEV, STRETCHBLT, STRETCHDIB, …
    case 88: case 90: RET(2, 96);    // LOGPIXELSX/Y
    case 104: RET(2, 0);             // SIZEPALETTE
    case 108: RET(2, 24);            // COLORRES
    case 116: RET(2, 60);            // VREFRESH
    default:
        if (g_hle_trace)
            fprintf(stderr, "[gdi32] GetDeviceCaps(%u) -> 0\n", ARG(1));
        RET(2, 0);
    }
}

void hle_gdi32_SetDeviceGammaRamp(CPU *c) { HLE_ENTER("SetDeviceGammaRamp", 2); RET(2, 1); }

static u32 swap_field(u32 *f, u32 v)
{
    u32 old = *f;
    *f = v;
    return old;
}

void hle_gdi32_SetBkColor(CPU *c) { HLE_ENTER("SetBkColor", 2); gdiobj *d = G(ARG(0), G_DC); RET(2, d ? swap_field(&d->bk_color, ARG(1)) : 0xFFFFFFFFu); }
void hle_gdi32_SetTextColor(CPU *c) { HLE_ENTER("SetTextColor", 2); gdiobj *d = G(ARG(0), G_DC); RET(2, d ? swap_field(&d->text_color, ARG(1)) : 0xFFFFFFFFu); }
void hle_gdi32_SetBkMode(CPU *c) { HLE_ENTER("SetBkMode", 2); gdiobj *d = G(ARG(0), G_DC); RET(2, d ? swap_field(&d->bk_mode, ARG(1)) : 0); }

void hle_gdi32_SelectObject(CPU *c)
{
    HLE_ENTER("SelectObject", 2);
    gdiobj *d = G(ARG(0), G_DC), *o = G(ARG(1), 0);
    if (!d || !o)
        RET(2, 0);
    switch (o->type) {
    case G_BITMAP: RET(2, swap_field(&d->bitmap, ARG(1)));
    case G_BRUSH: RET(2, swap_field(&d->brush, ARG(1)));
    case G_PEN: RET(2, swap_field(&d->pen, ARG(1)));
    case G_FONT: RET(2, swap_field(&d->font, ARG(1)));
    default: RET(2, 0);
    }
}

void hle_gdi32_DeleteObject(CPU *c)
{
    HLE_ENTER("DeleteObject", 1);
    gdiobj *o = G(ARG(0), 0);
    if (!o || o->type == G_DC)
        RET(1, 0);
    if (!o->stock) {
        if (o->bits_owned)
            heap_free(heap_process(), o->bits);
        free(o->pal);
        if (o->ft)
            FT_Done_Face(o->ft);
        o->type = G_NONE;
    }
    RET(1, 1);
}

void hle_gdi32_GetStockObject(CPU *c) { HLE_ENTER("GetStockObject", 1); RET(1, gdi32_stock((int)ARG(0))); }

// ================================================================ битмапы
int gdi32_bitmap(u32 hbm, u32 *bits, s32 *w, s32 *h, u32 *bpp, u32 *stride)
{
    gdiobj *b = G(hbm, G_BITMAP);
    if (!b)
        return 0;
    *bits = b->bits;
    *w = b->w;
    *h = b->h;
    *bpp = b->bpp;
    *stride = b->stride;
    return 1;
}

static u32 new_bitmap(s32 w, s32 h, u32 bpp, gdiobj **out)
{
    gdiobj *b;
    u32 hb = gdi_new(G_BITMAP, &b);
    if (!b)
        return 0;
    b->w = w;
    b->h = h;
    b->bpp = bpp ? bpp : 32;
    b->stride = (((u32)(w < 0 ? -w : w) * b->bpp + 31) / 32) * 4;
    u32 size = b->stride * (u32)(h < 0 ? -h : h);
    b->bits = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, size ? size : 4);
    b->bits_owned = 1;
    if (!b->bits) {
        b->type = G_NONE;
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    *out = b;
    return hb;
}

// CreateDIBitmap(hdc, BITMAPINFOHEADER*, CBM_INIT, bits, BITMAPINFO*, usage): совместимый (32 бит) битмап
// с преобразованием DIB 1/4/8/16/24/32 бит в BGRX.
void hle_gdi32_CreateDIBitmap(CPU *c)
{
    HLE_ENTER("CreateDIBitmap", 6);
    u32 hdr = ARG(1), init = ARG(2), src = ARG(3), bmi = ARG(4), usage = ARG(5);
    s32 w = (s32)rd32(g_mem, hdr + 4), h = (s32)rd32(g_mem, hdr + 8);
    gdiobj *b;
    u32 hb = new_bitmap(w, h < 0 ? -h : h, 32, &b);
    if (!hb)
        RET(6, 0);
    if ((init & 4) && src && bmi) {  // CBM_INIT
        u32 sbpp = rd16(g_mem, bmi + 14), comp = rd32(g_mem, bmi + 16);
        u32 pal = bmi + rd32(g_mem, bmi);
        u32 sstride = (((u32)w * sbpp + 31) / 32) * 4;
        u32 rows = (u32)(h < 0 ? -h : h);
        if (comp != 0 && !(comp == 3 && (sbpp == 16 || sbpp == 32)))
            hle_todo("CreateDIBitmap: compressed DIB");
        for (u32 y = 0; y < rows; y++) {
            // и источник, и битмап — снизу вверх, если h > 0; результат храним снизу вверх
            u32 sy = h < 0 ? rows - 1 - y : y;
            u32 s = src + sy * sstride, d = b->bits + y * b->stride;
            for (u32 x = 0; x < (u32)w; x++) {
                u32 px = 0;
                switch (sbpp) {
                case 1: case 4: case 8: {
                    u32 bit = x * sbpp, idx = (g_mem[s + bit / 8] >> (8 - sbpp - bit % 8)) & ((1u << sbpp) - 1);
                    px = usage == 1 ? 0 : rd32(g_mem, pal + 4 * idx);
                    break;
                }
                case 16: {
                    u32 v = rd16(g_mem, s + 2 * x);
                    px = ((v >> 10 & 31) * 255 / 31) << 16 | ((v >> 5 & 31) * 255 / 31) << 8 | (v & 31) * 255 / 31;
                    break;
                }
                case 24: px = g_mem[s + 3 * x] | (u32)g_mem[s + 3 * x + 1] << 8 | (u32)g_mem[s + 3 * x + 2] << 16; break;
                case 32: px = rd32(g_mem, s + 4 * x); break;
                }
                wr32(g_mem, d + 4 * x, px & 0xFFFFFF);
            }
        }
    }
    RET(6, hb);
}

// CreateDIBSection(hdc, BITMAPINFO*, usage, void **bits, hSection, offset)
void hle_gdi32_CreateDIBSection(CPU *c)
{
    HLE_ENTER("CreateDIBSection", 6);
    u32 bmi = ARG(1);
    if (ARG(4))
        hle_todo("CreateDIBSection over a file mapping");
    s32 w = (s32)rd32(g_mem, bmi + 4), h = (s32)rd32(g_mem, bmi + 8);
    gdiobj *b;
    u32 hb = new_bitmap(w, h, rd16(g_mem, bmi + 14), &b);
    if (!hb)
        RET(6, 0);
    b->dib = 1;
    memcpy(b->bih, g_mem + bmi, 40);
    if (b->bpp <= 8) {  // палитра (DIB_RGB_COLORS): RGBQUAD сразу за заголовком
        u32 used = rd32(g_mem, bmi + 32), n = used && used < (1u << b->bpp) ? used : 1u << b->bpp;
        if ((b->pal = calloc(n, 4)))
            for (u32 i = 0; i < n; i++)
                b->pal[i] = ARG(2) == 0 ? rd32(g_mem, bmi + rd32(g_mem, bmi) + 4 * i) & 0xFFFFFF : i * 0x010101u;
        b->npal = b->pal ? n : 0;
    }
    if (ARG(3))
        wr32(g_mem, ARG(3), b->bits);
    RET(6, hb);
}

void hle_gdi32_CreateCompatibleBitmap(CPU *c)
{
    HLE_ENTER("CreateCompatibleBitmap", 3);
    gdiobj *b;
    RET(3, new_bitmap((s32)ARG(1), (s32)ARG(2), 32, &b));
}

// GetObjectA: BITMAP (24 байта) / DIBSECTION (84), LOGPEN, LOGBRUSH, LOGFONTA
void hle_gdi32_GetObjectA(CPU *c)
{
    HLE_ENTER("GetObjectA", 3);
    gdiobj *o = G(ARG(0), 0);
    u32 cap = ARG(1), p = ARG(2);
    if (!o)
        RET(3, 0);
    u8 buf[84] = {0};
    u32 n = 0;
    switch (o->type) {
    case G_BITMAP: {
        u32 v[6] = {0, (u32)o->w, (u32)(o->h < 0 ? -o->h : o->h), o->stride, 1u | o->bpp << 16, o->dib ? o->bits : 0};
        memcpy(buf, v, 24);
        n = 24;
        if (o->dib && cap >= 84) {
            memcpy(buf + 24, o->bih, 40);
            n = 84;
        }
        break;
    }
    case G_PEN: {
        u32 v[4] = {o->style, o->width, 0, o->color};
        memcpy(buf, v, 16);
        n = 16;
        break;
    }
    case G_BRUSH: {
        u32 v[3] = {o->style == 5 ? 1u : 0u, o->color, 0};
        memcpy(buf, v, 12);
        n = 12;
        break;
    }
    case G_FONT: {
        u32 v[5] = {(u32)o->font_h, 0, 0, 0, o->font_weight};
        memcpy(buf, v, 20);
        memcpy(buf + 28, o->face, strlen(o->face));
        n = 60;
        break;
    }
    default:
        RET(3, 0);
    }
    if (!p)
        RET(3, n);
    if (cap < n)
        n = cap;
    memcpy(g_mem + p, buf, n);
    RET(3, n);
}

// ================================================================ кисти, перья, шрифты
void hle_gdi32_CreateSolidBrush(CPU *c)
{
    HLE_ENTER("CreateSolidBrush", 1);
    gdiobj *o;
    u32 h = gdi_new(G_BRUSH, &o);
    if (o)
        o->color = ARG(0);
    RET(1, h);
}

void hle_gdi32_CreatePen(CPU *c)
{
    HLE_ENTER("CreatePen", 3);
    gdiobj *o;
    u32 h = gdi_new(G_PEN, &o);
    if (o) {
        o->style = ARG(0);
        o->width = ARG(1);
        o->color = ARG(2);
    }
    RET(3, h);
}

static u32 new_font(s32 height, u32 weight, u32 quality, const char *face)
{
    gdiobj *o;
    u32 h = gdi_new(G_FONT, &o);
    if (o) {
        o->font_h = height ? height : 16;
        o->font_weight = weight ? weight : 400;
        o->font_quality = quality;
        snprintf(o->face, sizeof o->face, "%s", face);
    }
    return h;
}

// CreateFont(height, width, escapement, orientation, weight, italic, underline, strike, charset, outprec,
//            clipprec, quality, pitch, face) — 14 аргументов
void hle_gdi32_CreateFontA(CPU *c) { HLE_ENTER("CreateFontA", 14); RET(14, new_font((s32)ARG(0), ARG(4), ARG(11), hle_cstr(ARG(13)))); }

void hle_gdi32_CreateFontW(CPU *c)
{
    HLE_ENTER("CreateFontW", 14);
    char face[64] = "";
    if (ARG(13))
        hle_w2utf8(ARG(13), face, sizeof face);
    RET(14, new_font((s32)ARG(0), ARG(4), ARG(11), face));
}

void hle_gdi32_CreateFontIndirectA(CPU *c)
{
    HLE_ENTER("CreateFontIndirectA", 1);
    u32 lf = ARG(0);
    char face[33];
    memcpy(face, g_mem + lf + 28, 32);
    face[32] = 0;
    RET(1, new_font((s32)rd32(g_mem, lf), rd32(g_mem, lf + 16), g_mem[lf + 26], face));
}

// ---------------------------------------------------------------- TrueType через FreeType
static FT_Library g_ft;
static pthread_mutex_t g_ft_lock = PTHREAD_MUTEX_INITIALIZER;  // FreeType не потокобезопасен

// файл шрифта для имени GDI: SNX_GDI_FONT; на маке — системные Tahoma/Arial (жирный — отдельным файлом), на
// Switch — системный шрифт (жирность — утолщением контура, font_face); *synth_bold — нужно ли утолщать
static FT_Face open_face(const char *name, int bold, int *synth_bold)
{
    FT_Face f = NULL;
    *synth_bold = 0;
    const char *env = getenv("SNX_GDI_FONT");
    if (env && !FT_New_Face(g_ft, env, 0, &f)) {
        *synth_bold = bold;
        return f;
    }
#ifdef __SWITCH__
    (void)name;
    PlFontData fd;
    if (R_SUCCEEDED(plInitialize(PlServiceType_User)) && R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard)) &&
        !FT_New_Memory_Face(g_ft, (const FT_Byte *)fd.address, (FT_Long)fd.size, 0, &f)) {
        *synth_bold = bold;
        return f;
    }
#elif defined(__ANDROID__)
    // Tahoma/Arial в Android нет: Roboto (с кириллицей — для русской версии), на старых — Droid Sans. Свой шрифт
    // (например, Tahoma с ПК) — SNX_GDI_FONT, его ставит лаунчер, если файл лежит в папке игры
    (void)name;
    static const char *const reg[] = {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf",
                                      "/system/fonts/DroidSans.ttf", "/system/fonts/NotoSans-Regular.ttf"};
    static const char *const bld[] = {"/system/fonts/Roboto-Bold.ttf", "/system/fonts/DroidSans-Bold.ttf"};
    if (bold)
        for (size_t i = 0; i < sizeof bld / sizeof bld[0]; i++)
            if (!FT_New_Face(g_ft, bld[i], 0, &f))
                return f;
    for (size_t i = 0; i < sizeof reg / sizeof reg[0]; i++)
        if (!FT_New_Face(g_ft, reg[i], 0, &f)) {
            *synth_bold = bold;
            return f;
        }
#else
    static const char dir[] = "/System/Library/Fonts/Supplemental/";
    const char *files[2][2] = {{"Arial.ttf", "Arial Bold.ttf"}, {"Tahoma.ttf", "Tahoma Bold.ttf"}};
    int tahoma = !strcasecmp(name, "Tahoma") || !strcasecmp(name, "Verdana");
    char path[256];
    snprintf(path, sizeof path, "%s%s", dir, files[tahoma][bold]);
    if (!FT_New_Face(g_ft, path, 0, &f))
        return f;
    snprintf(path, sizeof path, "%s%s", dir, files[tahoma][0]);
    if (!FT_New_Face(g_ft, path, 0, &f)) {
        *synth_bold = bold;
        return f;
    }
    if (!FT_New_Face(g_ft, "/System/Library/Fonts/Helvetica.ttc", 0, &f)) {
        *synth_bold = bold;
        return f;
    }
#endif
    return NULL;
}

// FT_Face шрифта (под g_ft_lock) с размером по высоте GDI: < 0 — кегль (em), > 0 — высота ячейки
static FT_Face font_face(gdiobj *f, int *synth_bold)
{
    if (!g_ft && FT_Init_FreeType(&g_ft))
        return NULL;
    int bold = f->font_weight >= 600;
    if (!f->ft) {
        f->ft = open_face(f->face, bold, synth_bold);
        if (!f->ft)
            return NULL;
        FT_Size_RequestRec req = {f->font_h < 0 ? FT_SIZE_REQUEST_TYPE_NOMINAL : FT_SIZE_REQUEST_TYPE_CELL, 0,
                                  (FT_Long)(f->font_h < 0 ? -f->font_h : f->font_h) << 6, 0, 0};
        FT_Request_Size(f->ft, &req);
        f->font_quality |= (u32)*synth_bold << 8;  // запомнить, что жирность синтетическая
    }
    *synth_bold = (f->font_quality >> 8) & 1;
    return f->ft;
}

// глиф символа ch в слот (с утолщением, если нужно); 0 — нет
static int load_glyph(FT_Face ft, u32 ch, int synth_bold, int mono)
{
    FT_UInt gi = FT_Get_Char_Index(ft, ch);
    if (FT_Load_Glyph(ft, gi, FT_LOAD_NO_BITMAP | (mono ? FT_LOAD_TARGET_MONO : FT_LOAD_TARGET_NORMAL)))
        return 0;
    if (synth_bold && ft->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        FT_Pos s = FT_MulFix(ft->units_per_EM, ft->size->metrics.y_scale) / 24;
        FT_Outline_Embolden(&ft->glyph->outline, s);
        ft->glyph->metrics.horiAdvance += s;
        ft->glyph->advance.x += s;
    }
    return 1;
}

static u32 cell_height(FT_Face ft) { return (u32)((ft->size->metrics.ascender - ft->size->metrics.descender + 63) >> 6); }

// Размер строки: сумма продвижений глифов и высота ячейки шрифта (tmHeight); нет шрифта — моноширинное приближение
static void text_extent(u32 hdc, u32 str, u32 len, int wide, u32 size)
{
    gdiobj *d = G(hdc, G_DC), *f = d ? G(d->font, G_FONT) : NULL;
    s32 fh = f ? f->font_h : 16;
    u32 cx = len * (u32)((fh < 0 ? -fh : fh) / 2 > 0 ? (fh < 0 ? -fh : fh) / 2 : 1), cy = (u32)(fh < 0 ? -fh : fh);
    int sb;
    pthread_mutex_lock(&g_ft_lock);
    FT_Face ft = f ? font_face(f, &sb) : NULL;
    if (ft) {
        FT_Pos adv = 0;
        for (u32 i = 0; i < len; i++)
            if (load_glyph(ft, wide ? rd16(g_mem, str + 2 * i) : g_mem[str + i], sb, (f->font_quality & 0xFF) == 3))
                adv += (ft->glyph->advance.x + 32) >> 6;
        cx = (u32)adv;
        cy = cell_height(ft);
    }
    pthread_mutex_unlock(&g_ft_lock);
    wr32(g_mem, size, cx);
    wr32(g_mem, size + 4, cy);
}

void hle_gdi32_GetTextExtentPoint32A(CPU *c) { HLE_ENTER("GetTextExtentPoint32A", 4); text_extent(ARG(0), ARG(1), ARG(2), 0, ARG(3)); RET(4, 1); }
void hle_gdi32_GetTextExtentPoint32W(CPU *c) { HLE_ENTER("GetTextExtentPoint32W", 4); text_extent(ARG(0), ARG(1), ARG(2), 1, ARG(3)); RET(4, 1); }

// ---------------------------------------------------------------- пиксели битмапа гостя
// цвет COLORREF (0x00BBGGRR) -> значение пикселя битмапа (индекс ближайшего цвета палитры для 1/4/8 бит)
static u32 px_value(const gdiobj *b, u32 cr)
{
    u32 r = cr & 0xFF, g = cr >> 8 & 0xFF, bl = cr >> 16 & 0xFF;
    if (b->bpp > 8)
        return r << 16 | g << 8 | bl;
    u32 best = 0, best_d = 0xFFFFFFFFu;
    for (u32 i = 0; i < b->npal; i++) {
        s32 dr = (s32)(b->pal[i] >> 16 & 0xFF) - (s32)r, dg = (s32)(b->pal[i] >> 8 & 0xFF) - (s32)g, db = (s32)(b->pal[i] & 0xFF) - (s32)bl;
        u32 dist = (u32)(dr * dr + dg * dg + db * db);
        if (dist < best_d) {
            best_d = dist;
            best = i;
        }
    }
    return best;
}

static void put_px(const gdiobj *b, s32 x, s32 y, u32 v)
{
    s32 w = b->w < 0 ? -b->w : b->w, h = b->h < 0 ? -b->h : b->h;
    if (x < 0 || y < 0 || x >= w || y >= h)
        return;
    u32 row = b->bits + b->stride * (u32)(b->h < 0 ? y : h - 1 - y);  // DIB с h > 0 — снизу вверх
    switch (b->bpp) {
    case 32: wr32(g_mem, row + 4 * (u32)x, v); break;
    case 24:
        g_mem[row + 3 * (u32)x] = (u8)v;
        g_mem[row + 3 * (u32)x + 1] = (u8)(v >> 8);
        g_mem[row + 3 * (u32)x + 2] = (u8)(v >> 16);
        break;
    case 16: wr16(g_mem, row + 2 * (u32)x, (u16)((v >> 19 & 31) << 10 | (v >> 11 & 31) << 5 | (v >> 3 & 31))); break;
    case 8: g_mem[row + (u32)x] = (u8)v; break;
    case 4: {
        u8 *p = &g_mem[row + (u32)x / 2];
        *p = (u8)(x & 1 ? (*p & 0xF0) | (v & 15) : (*p & 0x0F) | (v & 15) << 4);
        break;
    }
    case 1: {
        u8 *p = &g_mem[row + (u32)x / 8], bit = (u8)(0x80 >> (x & 7));
        *p = (u8)(v ? *p | bit : *p & ~bit);
        break;
    }
    default: break;
    }
}

static void fill(const gdiobj *b, s32 x, s32 y, s32 w, s32 h, u32 v)
{
    for (s32 j = 0; j < h; j++)
        for (s32 i = 0; i < w; i++)
            put_px(b, x + i, y + j, v);
}

// ================================================================ рисование
void hle_gdi32_BitBlt(CPU *c) { HLE_ENTER("BitBlt", 9); RET(9, 1); }
void hle_gdi32_StretchBlt(CPU *c) { HLE_ENTER("StretchBlt", 11); RET(11, 1); }

// PatBlt(hdc, x, y, w, h, rop): PATCOPY — кистью DC, BLACKNESS/WHITENESS
void hle_gdi32_PatBlt(CPU *c)
{
    HLE_ENTER("PatBlt", 6);
    gdiobj *d = G(ARG(0), G_DC), *b = d ? G(d->bitmap, G_BITMAP) : NULL, *br = d ? G(d->brush, G_BRUSH) : NULL;
    u32 rop = ARG(5);
    if (b && (rop == 0x00F00021u || rop == 0x00000042u || rop == 0x00FF0062u)) {
        u32 cr = rop == 0x42u ? 0 : rop == 0x00FF0062u ? 0xFFFFFF : br ? br->color : 0xFFFFFF;
        if (rop != 0x00F00021u || !br || br->style != 5)
            fill(b, (s32)ARG(1), (s32)ARG(2), (s32)ARG(3), (s32)ARG(4), px_value(b, cr));
    }
    RET(6, 1);
}

// TextOutW(hdc, x, y, str, n): выравнивание TA_TOP|TA_LEFT (верх ячейки в y), фон — цветом фона при OPAQUE;
// NONANTIALIASED_QUALITY — однобитные глифы (как на ПК), иначе пиксель с покрытием от половины
static void text_out(u32 hdc, s32 x, s32 y, u32 str, u32 n, int wide)
{
    gdiobj *d = G(hdc, G_DC), *b = d ? G(d->bitmap, G_BITMAP) : NULL, *f = d ? G(d->font, G_FONT) : NULL;
    if (!b || !f || !b->bits)
        return;
    int sb, mono = (f->font_quality & 0xFF) == 3;
    pthread_mutex_lock(&g_ft_lock);
    FT_Face ft = font_face(f, &sb);
    if (ft) {
        u32 fg = px_value(b, d->text_color), bg = px_value(b, d->bk_color);
        s32 base = y + (s32)((ft->size->metrics.ascender + 32) >> 6), pen = x;
        for (u32 i = 0; i < n; i++) {
            if (!load_glyph(ft, wide ? rd16(g_mem, str + 2 * i) : g_mem[str + i], sb, mono) ||
                FT_Render_Glyph(ft->glyph, mono ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL))
                continue;
            FT_GlyphSlot g = ft->glyph;
            s32 adv = (s32)((g->advance.x + 32) >> 6);
            if (d->bk_mode == 2)  // OPAQUE
                fill(b, pen, y, adv, (s32)cell_height(ft), bg);
            const FT_Bitmap *bm = &g->bitmap;
            for (u32 r = 0; r < bm->rows; r++)
                for (u32 col = 0; col < bm->width; col++) {
                    const u8 *row = bm->buffer + (s32)r * bm->pitch;
                    int on = bm->pixel_mode == FT_PIXEL_MODE_MONO ? row[col / 8] >> (7 - col % 8) & 1 : row[col] >= 128;
                    if (on)
                        put_px(b, pen + g->bitmap_left + (s32)col, base - g->bitmap_top + (s32)r, fg);
                }
            pen += adv;
        }
    }
    pthread_mutex_unlock(&g_ft_lock);
}

void hle_gdi32_TextOutW(CPU *c)
{
    HLE_ENTER("TextOutW", 5);
    text_out(ARG(0), (s32)ARG(1), (s32)ARG(2), ARG(3), ARG(4), 1);
    RET(5, 1);
}

void hle_gdi32_ExtTextOutA(CPU *c) { HLE_ENTER("ExtTextOutA", 8); RET(8, 1); }
void hle_gdi32_Rectangle(CPU *c) { HLE_ENTER("Rectangle", 5); RET(5, 1); }

void hle_gdi32_MoveToEx(CPU *c)
{
    HLE_ENTER("MoveToEx", 4);
    gdiobj *d = G(ARG(0), G_DC);
    if (!d)
        RET(4, 0);
    if (ARG(3)) {
        wr32(g_mem, ARG(3), (u32)d->cur_x);
        wr32(g_mem, ARG(3) + 4, (u32)d->cur_y);
    }
    d->cur_x = (s32)ARG(1);
    d->cur_y = (s32)ARG(2);
    RET(4, 1);
}

void hle_gdi32_LineTo(CPU *c)
{
    HLE_ENTER("LineTo", 3);
    gdiobj *d = G(ARG(0), G_DC);
    if (d) {
        d->cur_x = (s32)ARG(1);
        d->cur_y = (s32)ARG(2);
    }
    RET(3, d != NULL);
}
