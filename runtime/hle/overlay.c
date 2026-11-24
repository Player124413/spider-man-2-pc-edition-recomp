// Слой интерфейса порта поверх кадра (см. overlay.h). Атлас R8 1024x1024: вверху — формы значков (круг,
// кольцо, «таблетка», крестовина; сглаживание по расстоянию до края), ниже — глифы ASCII и кириллицы
// (U+0400..U+045F, включая Ё/ё) кеглем 48. Значок кнопки — светлая форма с тёмной надписью поверх, как на Switch.
#include "runtime/hle/overlay.h"

#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "platform/gl.h"
// Все вызовы GL в этом файле выполняются внутри x_draw() (через cs_async в потоке рендера) — идём в GL напрямую
#define GL_CS_IMPL
#include "runtime/hle/gl_cs.h"

#define ATLAS 1024
#define FONT_PX 48
#define MAX_VERTS (6 * 4096)

#define GL_ASCII_FIRST 32u
#define GL_ASCII_COUNT 95u  // ASCII 32..126
#define GL_CYR_FIRST 0x0400u
#define GL_CYR_COUNT 96u    // кириллица U+0400..U+045F (включая Ё/ё)
#define GL_TOTAL (GL_ASCII_COUNT + GL_CYR_COUNT)

typedef struct {
    float u0, v0, u1, v1;  // в атласе (доли)
    float w, h, bx, by;    // размер глифа, смещение от пера вправо и от базовой линии вверх (пиксели кегля 48)
    float adv;
    u8 valid;
} ov_glyph;

typedef struct {
    float x, y, u, v;
    u32 c;
} ov_vert;

typedef struct {
    u32 fbo;
    int w, h, n;
} ov_batch;  // за ним — n вершин

_Static_assert(sizeof(ov_batch) == 16 && sizeof(ov_vert) == 20, "ожидаемая плотная раскладка ov_batch/ov_vert");

enum { SH_WHITE, SH_DISC, SH_RING, SH_PILL, SH_CROSS, SH_N };
static const int g_sh_rect[SH_N][4] = {  // x, y, w, h в атласе
    {0, 0, 4, 4}, {8, 0, 96, 96}, {112, 0, 96, 96}, {216, 0, 176, 96}, {400, 0, 96, 96},
};

static u8 *g_atlas;
static int g_state;  // 0 — не готов, 1 — атлас есть, -1 — не вышло
static int g_have_font;
static ov_glyph g_gl[GL_TOTAL];
static float g_ascent, g_cap;  // высота над базовой линией строки и заглавных (пиксели кегля 48)
static struct {
    ov_batch hdr;
    ov_vert v[MAX_VERTS];
} g_batch;
static int g_nv;

typedef void (*ov_draw_hook_fn)(int w, int h);
static _Atomic ov_draw_hook_fn g_draw_hook;

// ---------------------------------------------------------------- UTF-8 и индекс глифа
static int glyph_idx(u32 cp)
{
    if (cp >= GL_ASCII_FIRST && cp < GL_ASCII_FIRST + GL_ASCII_COUNT)
        return (int)(cp - GL_ASCII_FIRST);
    if (cp >= GL_CYR_FIRST && cp < GL_CYR_FIRST + GL_CYR_COUNT)
        return (int)(GL_ASCII_COUNT + (cp - GL_CYR_FIRST));
    if (cp == 0x00A0u)
        return (int)(' ' - GL_ASCII_FIRST);
    if (cp == 0x2013u || cp == 0x2014u)
        return (int)('-' - GL_ASCII_FIRST);
    return -1;
}

// Следующий кодовый пункт UTF-8; некорректные байты пропускаются безопасно без чтения за '\0'.
static u32 utf8_next(const char **ps)
{
    const u8 *p = (const u8 *)*ps;
    u8 b0 = p[0];
    if (b0 < 0x80u) {
        *ps = (const char *)(p + 1);
        return b0;
    }
    if ((b0 & 0xE0u) == 0xC0u && (p[1] & 0xC0u) == 0x80u) {
        u32 cp = ((u32)(b0 & 0x1Fu) << 6) | (u32)(p[1] & 0x3Fu);
        *ps = (const char *)(p + 2);
        return cp >= 0x80u ? cp : 0xFFFDu;
    }
    if ((b0 & 0xF0u) == 0xE0u && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) {
        u32 cp = ((u32)(b0 & 0x0Fu) << 12) | ((u32)(p[1] & 0x3Fu) << 6) | (u32)(p[2] & 0x3Fu);
        *ps = (const char *)(p + 3);
        return (cp >= 0x800u && !(cp >= 0xD800u && cp <= 0xDFFFu)) ? cp : 0xFFFDu;
    }
    if ((b0 & 0xF8u) == 0xF0u && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u && (p[3] & 0xC0u) == 0x80u) {
        u32 cp = ((u32)(b0 & 0x07u) << 18) | ((u32)(p[1] & 0x3Fu) << 12) | ((u32)(p[2] & 0x3Fu) << 6) |
                 (u32)(p[3] & 0x3Fu);
        *ps = (const char *)(p + 4);
        return (cp >= 0x10000u && cp <= 0x10FFFFu) ? cp : 0xFFFDu;
    }
    *ps = (const char *)(p + 1);
    return 0xFFFDu;
}

// ---------------------------------------------------------------- атлас
static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// форма по расстоянию до края d (пиксели, < 0 — внутри)
static void shape(int id)
{
    const int *r = g_sh_rect[id];
    for (int y = 0; y < r[3]; y++)
        for (int x = 0; x < r[2]; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f, d = -1.0f;
            float cx = (float)r[2] / 2, cy = (float)r[3] / 2, R = (float)r[3] / 2 - 2;
            switch (id) {
            case SH_DISC: d = hypotf(px - cx, py - cy) - R; break;
            case SH_RING: d = fabsf(hypotf(px - cx, py - cy) - (R - 5)) - 5; break;
            case SH_PILL: {  // прямоугольник со скруглением на всю высоту
                float hx = (float)r[2] / 2 - 2 - R, qx = fabsf(px - cx) - hx;
                d = (qx > 0 ? hypotf(qx, py - cy) : fabsf(py - cy)) - R;
                break;
            }
            case SH_CROSS: {  // два скруглённых бруска
                float a = R, b = R * 0.36f, rr = b * 0.5f;
                float dx1 = fabsf(px - cx) - (a - rr), dy1 = fabsf(py - cy) - (b - rr);
                float dx2 = fabsf(px - cx) - (b - rr), dy2 = fabsf(py - cy) - (a - rr);
                float d1 = hypotf(fmaxf(dx1, 0), fmaxf(dy1, 0)) + fminf(fmaxf(dx1, dy1), 0) - rr;
                float d2 = hypotf(fmaxf(dx2, 0), fmaxf(dy2, 0)) + fminf(fmaxf(dx2, dy2), 0) - rr;
                d = fminf(d1, d2);
                break;
            }
            default: break;
            }
            g_atlas[(r[1] + y) * ATLAS + r[0] + x] = (u8)(clamp01(0.5f - d) * 255.0f + 0.5f);
        }
}

static FT_Face open_font(FT_Library ft, int *pl_inited)
{
    FT_Face face = NULL;
    const char *env = getenv("SNX_FONT");
    if (env && !FT_New_Face(ft, env, 0, &face))
        return face;
#ifdef __SWITCH__
    // Системный шрифт Switch PlSharedFontType_Standard уже включает кириллицу [проверить на железе Switch]
    PlFontData fd;
    if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
        *pl_inited = 1;
        if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard)) &&
            !FT_New_Memory_Face(ft, (const FT_Byte *)fd.address, (FT_Long)fd.size, 0, &face))
            return face;
        plExit();
        *pl_inited = 0;
    }
#else
    (void)pl_inited;
#ifdef __APPLE__
    // Предпочитаем системные шрифты macOS с полной кириллицей (Arial Rounded Bold содержит только латиницу)
    static const char *const paths[] = {
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/SFCompactRounded.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf",
    };
#elif defined(__ANDROID__)
    // системные шрифты Android разных лет: Roboto (5+, на 12+ — переменный и RobotoStatic), Droid Sans (старые)
    static const char *const paths[] = {"/system/fonts/Roboto-Bold.ttf", "/system/fonts/RobotoStatic-Regular.ttf",
                                        "/system/fonts/Roboto-Regular.ttf", "/system/fonts/DroidSans-Bold.ttf",
                                        "/system/fonts/DroidSans.ttf", "/system/fonts/NotoSans-Regular.ttf"};
#else
    static const char *const paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
    };
#endif
    FT_Face fallback = NULL;
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        FT_Face cand = NULL;
        if (FT_New_Face(ft, paths[i], 0, &cand))
            continue;
        if (FT_Get_Char_Index(cand, 0x0410u) != 0) {
            if (fallback)
                FT_Done_Face(fallback);
            return cand;
        }
        if (!fallback)
            fallback = cand;
        else
            FT_Done_Face(cand);
    }
    return fallback;
#endif
    return NULL;
}

static void pack_glyph(FT_Face face, u32 cp, int idx, int *px, int *py, int *row)
{
    if (idx < 0 || idx >= (int)GL_TOTAL)
        return;
    if (cp != ' ' && !FT_Get_Char_Index(face, (FT_ULong)cp))
        return;
    if (FT_Load_Char(face, (FT_ULong)cp, FT_LOAD_RENDER))
        return;
    FT_GlyphSlot g = face->glyph;
    int w = (int)g->bitmap.width, h = (int)g->bitmap.rows;
    if (w < 0 || h < 0 || w + 2 > ATLAS || h + 2 > ATLAS)
        return;
    if (*px + w + 2 > ATLAS) {
        *px = 0;
        *py += *row + 2;
        *row = 0;
    }
    if (*py + h > ATLAS)
        return;
    for (int y = 0; y < h; y++) {
        const u8 *src = g->bitmap.pitch >= 0 ? g->bitmap.buffer + (size_t)y * (size_t)g->bitmap.pitch
                                             : g->bitmap.buffer + (size_t)(h - 1 - y) * (size_t)(-g->bitmap.pitch);
        memcpy(g_atlas + (size_t)(*py + y) * ATLAS + (size_t)*px, src, (size_t)w);
    }
    ov_glyph *o = &g_gl[idx];
    o->u0 = (float)*px / ATLAS;
    o->v0 = (float)*py / ATLAS;
    o->u1 = (float)(*px + w) / ATLAS;
    o->v1 = (float)(*py + h) / ATLAS;
    o->w = (float)w;
    o->h = (float)h;
    o->bx = (float)g->bitmap_left;
    o->by = (float)g->bitmap_top;
    o->adv = (float)(g->advance.x >> 6);
    o->valid = 1;
    *px += w + 2;
    if (h > *row)
        *row = h;
}

static void build_atlas(void)
{
    g_atlas = calloc(ATLAS, ATLAS);
    if (!g_atlas) {
        g_state = -1;
        return;
    }
    for (int i = 0; i < 4 * 4; i++)
        g_atlas[(i / 4) * ATLAS + i % 4] = 255;
    for (int s = SH_DISC; s < SH_N; s++)
        shape(s);
    FT_Library ft = NULL;
    FT_Face face = NULL;
    int ft_ok = !FT_Init_FreeType(&ft), pl_inited = 0;
    if (ft_ok && (face = open_font(ft, &pl_inited)) && !FT_Set_Pixel_Sizes(face, 0, FONT_PX)) {
        g_ascent = (float)(face->size->metrics.ascender >> 6);
        int px = 0, py = 104, row = 0;
        for (u32 cp = GL_ASCII_FIRST; cp < GL_ASCII_FIRST + GL_ASCII_COUNT; cp++)
            pack_glyph(face, cp, glyph_idx(cp), &px, &py, &row);
        for (u32 cp = GL_CYR_FIRST; cp < GL_CYR_FIRST + GL_CYR_COUNT; cp++)
            pack_glyph(face, cp, glyph_idx(cp), &px, &py, &row);
        int hi = glyph_idx('H');
        g_cap = (hi >= 0 && g_gl[hi].by > 0) ? g_gl[hi].by : g_ascent * 0.7f;
        g_have_font = 1;
        fprintf(stderr, "[overlay] font %s %s (atlas used y=%d/%d)\n", face->family_name ? face->family_name : "?",
                face->style_name ? face->style_name : "", py + row, ATLAS);
    } else
        fprintf(stderr, "[overlay] no font: icons without letters, no text\n");
    if (face)
        FT_Done_Face(face);
    if (ft_ok)
        FT_Done_FreeType(ft);
#ifdef __SWITCH__
    if (pl_inited)
        plExit();
#endif
    g_state = 1;
}

static void ensure_atlas(void)
{
    if (!g_state)
        build_atlas();
}

// ---------------------------------------------------------------- список
static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, u32 c)
{
    if (g_nv + 6 > MAX_VERTS)
        return;
    ov_vert *v = g_batch.v + g_nv;
    v[0] = (ov_vert){x0, y0, u0, v0, c};
    v[1] = (ov_vert){x1, y0, u1, v0, c};
    v[2] = (ov_vert){x0, y1, u0, v1, c};
    v[3] = v[1];
    v[4] = (ov_vert){x1, y1, u1, v1, c};
    v[5] = v[2];
    g_nv += 6;
}

static void sprite(int id, float x, float y, float w, float h, u32 c)
{
    const int *r = g_sh_rect[id];
    if (id == SH_WHITE)  // середина сплошного квадрата
        quad(x, y, x + w, y + h, 2.0f / ATLAS, 2.0f / ATLAS, 2.0f / ATLAS, 2.0f / ATLAS, c);
    else
        quad(x, y, x + w, y + h, (float)r[0] / ATLAS, (float)r[1] / ATLAS, (float)(r[0] + r[2]) / ATLAS,
             (float)(r[1] + r[3]) / ATLAS, c);
}

void ov_begin(void)
{
    ensure_atlas();
    g_nv = 0;
}

void ov_rect(float x, float y, float w, float h, u32 rgba)
{
    ensure_atlas();
    if (g_state > 0)
        sprite(SH_WHITE, x, y, w, h, rgba);
}

float ov_text_width(float h, const char *s)
{
    if (!s)
        return 0;
    ensure_atlas();
    float k = h / FONT_PX, w = 0;
    while (g_have_font && *s) {
        int idx = glyph_idx(utf8_next(&s));
        if (idx >= 0 && g_gl[idx].valid)
            w += g_gl[idx].adv * k;
    }
    return w;
}

float ov_text(float x, float y, float h, u32 rgba, const char *s)
{
    if (!s)
        return 0;
    ensure_atlas();
    if (g_state <= 0 || !g_have_font)
        return 0;
    float k = h / FONT_PX, pen = x, base = y + g_ascent * k;
    while (*s) {
        int idx = glyph_idx(utf8_next(&s));
        if (idx < 0 || !g_gl[idx].valid)
            continue;
        const ov_glyph *g = &g_gl[idx];
        if (g->w > 0)
            quad(pen + g->bx * k, base - g->by * k, pen + (g->bx + g->w) * k, base + (g->h - g->by) * k, g->u0, g->v0,
                 g->u1, g->v1, rgba);
        pen += g->adv * k;
    }
    return pen - x;
}

// надпись по центру (cx, cy): по ширине и по высоте заглавных
static void text_centered(float cx, float cy, float h, u32 rgba, const char *s)
{
    float k = h / FONT_PX, base = cy + g_cap * k / 2;
    ov_text(cx - ov_text_width(h, s) / 2, base - g_ascent * k, h, rgba, s);
}

static const struct {
    const char *label;
    float w;  // ширина в долях высоты
} g_btn[OV_BUTTON_N] = {
    [OV_A] = {"A", 1}, [OV_B] = {"B", 1}, [OV_X] = {"X", 1}, [OV_Y] = {"Y", 1}, [OV_L] = {"L", 1.6f},
    [OV_R] = {"R", 1.6f}, [OV_ZL] = {"ZL", 1.9f}, [OV_ZR] = {"ZR", 1.9f}, [OV_PLUS] = {"", 1}, [OV_MINUS] = {"", 1},
    [OV_LSTICK] = {"L", 1}, [OV_RSTICK] = {"R", 1}, [OV_DPAD] = {"", 1}, [OV_DPAD_UP] = {"", 1},
    [OV_DPAD_DOWN] = {"", 1}, [OV_DPAD_LEFT] = {"", 1}, [OV_DPAD_RIGHT] = {"", 1},
};

float ov_icon_width(float h, ov_button b) { return b < OV_BUTTON_N ? g_btn[b].w * h : 0; }

float ov_icon(float x, float y, float h, ov_button b)
{
    ensure_atlas();
    if (g_state <= 0 || b >= OV_BUTTON_N)
        return 0;
    const u32 face = OV_RGBA(240, 240, 240, 255), ink = OV_RGBA(30, 30, 30, 255), accent = OV_RGBA(255, 210, 40, 255);
    float w = g_btn[b].w * h, cx = x + w / 2, cy = y + h / 2;
    switch (b) {
    case OV_L: case OV_R: case OV_ZL: case OV_ZR:
        sprite(SH_PILL, x, y, w, h, face);
        text_centered(cx, cy, h * 0.6f, ink, g_btn[b].label);
        break;
    case OV_PLUS: case OV_MINUS:
        sprite(SH_DISC, x, y, h, h, face);
        ov_rect(cx - h * 0.25f, cy - h * 0.06f, h * 0.5f, h * 0.12f, ink);
        if (b == OV_PLUS)
            ov_rect(cx - h * 0.06f, cy - h * 0.25f, h * 0.12f, h * 0.5f, ink);
        break;
    case OV_LSTICK: case OV_RSTICK:
        sprite(SH_DISC, x, y, h, h, face);
        sprite(SH_RING, x + h * 0.14f, y + h * 0.14f, h * 0.72f, h * 0.72f, ink);
        text_centered(cx, cy, h * 0.42f, ink, g_btn[b].label);
        break;
    case OV_DPAD: case OV_DPAD_UP: case OV_DPAD_DOWN: case OV_DPAD_LEFT: case OV_DPAD_RIGHT: {
        sprite(SH_CROSS, x, y, h, h, face);
        static const float off[5][2] = {{0, 0}, {0, -0.3f}, {0, 0.3f}, {-0.3f, 0}, {0.3f, 0}};
        const float *o = off[b - OV_DPAD];
        if (b != OV_DPAD)
            sprite(SH_DISC, cx + o[0] * h - h * 0.11f, cy + o[1] * h - h * 0.11f, h * 0.22f, h * 0.22f, accent);
        break;
    }
    default:
        sprite(SH_DISC, x, y, h, h, face);
        text_centered(cx, cy, h * 0.62f, ink, g_btn[b].label);
        break;
    }
    return w;
}

// ---------------------------------------------------------------- рисование (поток рендера)
static GLuint g_prog, g_vao, g_vbo, g_tex;
static GLint g_loc_size, g_loc_flip_y;
static int g_gl_inited;

static GLuint shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    const char *srcs[3] = {SNX_GLSL_HEADER, SNX_GLSL_PRECISION, src};
    glShaderSource(s, 3, srcs, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[overlay] shader: %s\n", log);
    }
    return s;
}

static void gl_init(void)
{
    static const char vs[] = "layout(location = 0) in vec2 p;\n"
                             "layout(location = 1) in vec2 t;\n"
                             "layout(location = 2) in vec4 c;\n"
                             "uniform vec2 size;\n"
                             "uniform float flip_y;\n"
                             "out vec2 vt;\n"
                             "out vec4 vc;\n"
                             "void main() { vt = t; vc = c;\n"
                             "  gl_Position = vec4(p.x / size.x * 2.0 - 1.0, (p.y / size.y * 2.0 - 1.0) * flip_y, 0.0, 1.0); }\n";
    static const char fs[] = "in vec2 vt;\n"
                             "in vec4 vc;\n"
                             "uniform sampler2D tex;\n"
                             "out vec4 o;\n"
                             "void main() { o = vec4(vc.rgb, vc.a * texture(tex, vt).r); }\n";
    g_prog = glCreateProgram();
    GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    glAttachShader(g_prog, v);
    glAttachShader(g_prog, f);
    glLinkProgram(g_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    g_loc_size = glGetUniformLocation(g_prog, "size");
    g_loc_flip_y = glGetUniformLocation(g_prog, "flip_y");
    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ov_vert), (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ov_vert), (void *)8);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ov_vert), (void *)16);
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    GLint unpack_al = 4, unpack_row = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack_al);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpack_row);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS, ATLAS, 0, GL_RED, GL_UNSIGNED_BYTE, g_atlas);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpack_al);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, unpack_row);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(g_atlas);
    g_atlas = NULL;
}

// Сразу после ov_flush в d3d8_present перебиндивается DRAW_FRAMEBUFFER=0, сбрасываются scissor/colorMask/clearColor
// и инвалидируется кеш состояния кадра (g_applied_gen = 0xFFFFFFFFu), поэтому 22 glGet*/glIsEnabled на каждый кадр
// не нужны. Сохраняем только привязки VAO/программы/текстуры и явно выключаем смешивание, чтобы не мешать выводу кадра.
static void x_draw(void *arg)
{
    const ov_batch *b = arg;
    GLint prog, vao, active, tex, samp;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_SAMPLER_BINDING, &samp);

    if (!g_gl_inited) {
        gl_init();
        g_gl_inited = 1;
    }
    if (g_prog) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, b->fbo);
        glViewport(0, 0, b->w, b->h);
        glUseProgram(g_prog);
        glUniform2f(g_loc_size, (float)b->w, (float)b->h);
        // FBO игры хранится сверху вниз (flip_y=+1), окно GL (fbo=0) — снизу вверх (flip_y=-1)
        glUniform1f(g_loc_flip_y, b->fbo == 0 ? -1.0f : 1.0f);
        glBindVertexArray(g_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(ov_vert) * (size_t)b->n), b + 1, GL_STREAM_DRAW);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glBindSampler(0, 0);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBlendEquation(GL_FUNC_ADD);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDrawArrays(GL_TRIANGLES, 0, b->n);
        glDisable(GL_BLEND);
    }

    glBindSampler(0, (GLuint)samp);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glActiveTexture((GLenum)active);
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
}

void ov_set_draw_hook(void (*fn)(int w, int h))
{
    atomic_store_explicit(&g_draw_hook, fn, memory_order_release);
}

void ov_flush(u32 fbo, int w, int h)
{
    ov_draw_hook_fn hook = atomic_load_explicit(&g_draw_hook, memory_order_acquire);
    if (hook)
        hook(w, h);
    if (g_state <= 0 || !g_nv)
        return;
    g_batch.hdr = (ov_batch){fbo, w, h, g_nv};
    size_t size = sizeof(ov_batch) + sizeof(ov_vert) * (size_t)g_nv;
    cs_async(x_draw, &g_batch, size);
    g_nv = 0;
}
