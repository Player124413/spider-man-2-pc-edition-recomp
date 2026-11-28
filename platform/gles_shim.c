// Прослойка OpenGL ES 3.0 под вызовы GL 4.1 слоя D3D8 (см. gles_shim.h). Только Android (SNX_GLES).
#define SNX_GLES_SHIM_IMPL 1
#include "platform/gl.h"

#include <EGL/egl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

gles_caps_t gles_caps;

typedef void (*draw_bv_fn)(GLenum, GLsizei, GLenum, const void *, GLint);
static draw_bv_fn g_draw_bv;

// состояние упаковки пикселей (его читают перекладки форматов: шаг строк источника/приёмника)
static GLint g_unpack_row, g_unpack_align = 4, g_pack_row, g_pack_align = 4;

static int has_ext(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = list; p && (p = strstr(p, name)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0))
            return 1;
    return 0;
}

void gles_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    glGetIntegerv(GL_MAJOR_VERSION, &gles_caps.major);
    glGetIntegerv(GL_MINOR_VERSION, &gles_caps.minor);
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    const char *rnd = (const char *)glGetString(GL_RENDERER);
    snprintf(gles_caps.renderer, sizeof gles_caps.renderer, "%s", rnd ? rnd : "?");
    int es32 = gles_caps.major > 3 || (gles_caps.major == 3 && gles_caps.minor >= 2);
    if (es32)
        g_draw_bv = (draw_bv_fn)eglGetProcAddress("glDrawElementsBaseVertex");
    if (!g_draw_bv && has_ext(ext, "GL_EXT_draw_elements_base_vertex"))
        g_draw_bv = (draw_bv_fn)eglGetProcAddress("glDrawElementsBaseVertexEXT");
    if (!g_draw_bv && has_ext(ext, "GL_OES_draw_elements_base_vertex"))
        g_draw_bv = (draw_bv_fn)eglGetProcAddress("glDrawElementsBaseVertexOES");
    gles_caps.base_vertex = g_draw_bv != NULL;
    gles_caps.clip_distance = has_ext(ext, "GL_EXT_clip_cull_distance");
    gles_caps.s3tc = has_ext(ext, "GL_EXT_texture_compression_s3tc") ||
                     (has_ext(ext, "GL_EXT_texture_compression_dxt1") && has_ext(ext, "GL_ANGLE_texture_compression_dxt3") &&
                      has_ext(ext, "GL_ANGLE_texture_compression_dxt5"));
    gles_caps.border_clamp = es32 || has_ext(ext, "GL_EXT_texture_border_clamp") ||
                             has_ext(ext, "GL_OES_texture_border_clamp");
    GLint nbin = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &nbin);
    gles_caps.program_binary = nbin > 0;
    const char *e = getenv("SNX_TEX16");
    gles_caps.tex16 = e && *e == '1';
    e = getenv("SNX_GLES_S3TC");  // SNX_GLES_S3TC=0 — распаковывать DXT и при расширении (проверка распаковки)
    if (e && *e == '0')
        gles_caps.s3tc = 0;
    e = getenv("SNX_GLES_INVALIDATE");
    gles_caps.invalidate = !(e && *e == '0');
    fprintf(stderr,
            "[gles] ES %d.%d (%s): base vertex %d, clip distance %d, s3tc %d, border clamp %d, program binary %d, "
            "16-bit DXT %d\n",
            gles_caps.major, gles_caps.minor, gles_caps.renderer, gles_caps.base_vertex, gles_caps.clip_distance,
            gles_caps.s3tc, gles_caps.border_clamp, gles_caps.program_binary, gles_caps.tex16);
}

// ---------------------------------------------------------------- временный буфер перекладок
static __thread unsigned char *t_buf;
static __thread size_t t_cap;

static unsigned char *scratch(size_t n)
{
    if (n > t_cap) {
        free(t_buf);
        t_cap = n + (n >> 2);
        t_buf = malloc(t_cap);
        if (!t_buf)
            t_cap = 0;
    }
    return t_buf;
}

static size_t align_up(size_t v, GLint a) { return a > 1 ? (v + (size_t)a - 1) / (size_t)a * (size_t)a : v; }

// перекладки форматов D3D8 -> ES
enum { CV_NONE, CV_BGRA8, CV_BGRX8, CV_BGR8, CV_1555, CV_4444 };

typedef struct {
    GLint internal;
    GLenum format, type;
    int cv, src_bpp, dst_bpp;
} xfmt;

static int map_fmt(GLint internal, GLenum format, GLenum type, xfmt *o)
{
    *o = (xfmt){internal, format, type, CV_NONE, 0, 0};
    if (format == GL_BGRA && type == GL_UNSIGNED_BYTE) {
        *o = (xfmt){GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, internal == GL_RGB8 ? CV_BGRX8 : CV_BGRA8, 4, 4};
    } else if (format == GL_BGR && type == GL_UNSIGNED_BYTE) {
        *o = (xfmt){GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, CV_BGR8, 3, 3};
    } else if (format == GL_BGRA && type == GL_UNSIGNED_SHORT_1_5_5_5_REV) {
        *o = (xfmt){GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, CV_1555, 2, 2};
    } else if (format == GL_BGRA && type == GL_UNSIGNED_SHORT_4_4_4_4_REV) {
        *o = (xfmt){GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, CV_4444, 2, 2};
    } else if (internal == GL_DEPTH_COMPONENT32) {
        // D3DFMT_D32: в ES нет 32-битной целочисленной глубины текстурой; 24 бит хватает (данные — только NULL)
        o->internal = GL_DEPTH_COMPONENT24;
        o->type = GL_UNSIGNED_INT;
    }
    return o->cv != CV_NONE;
}

static void convert_row(int cv, const unsigned char *s, unsigned char *d, int w)
{
    switch (cv) {
    case CV_BGRA8:
        for (int x = 0; x < w; x++, s += 4, d += 4) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = s[3];
        }
        break;
    case CV_BGRX8:
        for (int x = 0; x < w; x++, s += 4, d += 4) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = 255;
        }
        break;
    case CV_BGR8:
        for (int x = 0; x < w; x++, s += 3, d += 3) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
        }
        break;
    case CV_1555:  // A1R5G5B5 -> R5G5B5A1
        for (int x = 0; x < w; x++, s += 2, d += 2) {
            uint16_t v;
            memcpy(&v, s, 2);
            v = (uint16_t)(v << 1 | v >> 15);
            memcpy(d, &v, 2);
        }
        break;
    case CV_4444:  // A4R4G4B4 -> R4G4B4A4
        for (int x = 0; x < w; x++, s += 2, d += 2) {
            uint16_t v;
            memcpy(&v, s, 2);
            v = (uint16_t)(v << 4 | v >> 12);
            memcpy(d, &v, 2);
        }
        break;
    default: break;
    }
}

// данные с учётом UNPACK_ROW_LENGTH/ALIGNMENT -> плотный буфер в формате ES
static const void *convert_upload(const xfmt *f, GLsizei w, GLsizei h, const void *data)
{
    size_t src_row = (size_t)(g_unpack_row ? g_unpack_row : w) * (size_t)f->src_bpp;
    size_t src_pitch = align_up(src_row, g_unpack_align), dst_pitch = (size_t)w * (size_t)f->dst_bpp;
    unsigned char *out = scratch(dst_pitch * (size_t)h);
    if (!out)
        return NULL;
    for (GLsizei y = 0; y < h; y++)
        convert_row(f->cv, (const unsigned char *)data + (size_t)y * src_pitch, out + (size_t)y * dst_pitch, w);
    return out;
}

static void tight_unpack(void)
{
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

static void restore_unpack(void)
{
    glPixelStorei(GL_UNPACK_ROW_LENGTH, g_unpack_row);
    glPixelStorei(GL_UNPACK_ALIGNMENT, g_unpack_align);
}

void snx_glPixelStorei(GLenum pname, GLint v)
{
    switch (pname) {
    case GL_UNPACK_ROW_LENGTH: g_unpack_row = v; break;
    case GL_UNPACK_ALIGNMENT: g_unpack_align = v; break;
    case GL_PACK_ROW_LENGTH: g_pack_row = v; break;
    case GL_PACK_ALIGNMENT: g_pack_align = v; break;
    default: break;
    }
    glPixelStorei(pname, v);
}

// ---------------------------------------------------------------- размеры текстур
// В ES 3.0 нет glGetTexLevelParameteriv: размер уровня 0 запоминается при загрузке (имя текстуры -> w, h; для
// кубических карт все грани одного размера). Таблица с открытой адресацией, только поток с контекстом.
typedef struct {
    GLuint name;
    GLsizei w, h;
} texsize;
static texsize *g_ts;
static size_t g_ts_cap, g_ts_n;

static texsize *ts_slot(GLuint name, int add)
{
    if (add && (g_ts_n + 1) * 2 > g_ts_cap) {
        size_t cap = g_ts_cap ? g_ts_cap * 2 : 1024;
        texsize *n = calloc(cap, sizeof *n);
        if (!n)
            return NULL;
        for (size_t i = 0; i < g_ts_cap; i++)
            if (g_ts[i].name) {
                size_t j = (g_ts[i].name * 2654435761u) & (cap - 1);
                while (n[j].name)
                    j = (j + 1) & (cap - 1);
                n[j] = g_ts[i];
            }
        free(g_ts);
        g_ts = n;
        g_ts_cap = cap;
    }
    if (!g_ts_cap)
        return NULL;
    size_t j = (name * 2654435761u) & (g_ts_cap - 1);
    while (g_ts[j].name && g_ts[j].name != name)
        j = (j + 1) & (g_ts_cap - 1);
    if (!g_ts[j].name) {
        if (!add)
            return NULL;
        g_ts[j].name = name;
        g_ts_n++;
    }
    return &g_ts[j];
}

static void remember_size(GLenum target, GLint level, GLsizei w, GLsizei h)
{
    if (level != 0)
        return;
    GLint tex = 0;
    glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_CUBE_MAP, &tex);
    texsize *t = tex ? ts_slot((GLuint)tex, 1) : NULL;  // имена переиспользуются: запись просто перезаписывается
    if (t) {
        t->w = w;
        t->h = h;
    }
}

void snx_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format,
                      GLenum type, const void *data)
{
    remember_size(target, level, w, h);
    xfmt f;
    if (!map_fmt(internal, format, type, &f) || !data) {
        glTexImage2D(target, level, f.internal, w, h, border, f.format, f.type, data);
        return;
    }
    const void *conv = convert_upload(&f, w, h, data);
    tight_unpack();
    glTexImage2D(target, level, f.internal, w, h, border, f.format, f.type, conv);
    restore_unpack();
}

void snx_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                         GLenum type, const void *data)
{
    xfmt f;
    if (!map_fmt(GL_RGBA8, format, type, &f) || !data) {
        glTexSubImage2D(target, level, x, y, w, h, f.format, f.type, data);
        return;
    }
    const void *conv = convert_upload(&f, w, h, data);
    tight_unpack();
    glTexSubImage2D(target, level, x, y, w, h, f.format, f.type, conv);
    restore_unpack();
}

// ---------------------------------------------------------------- DXT без расширения: распаковка на ЦП
static void rgb565(uint16_t c, unsigned char *o)
{
    unsigned r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    o[0] = (unsigned char)(r << 3 | r >> 2);
    o[1] = (unsigned char)(g << 2 | g >> 4);
    o[2] = (unsigned char)(b << 3 | b >> 2);
    o[3] = 255;
}

// блок цвета (8 байт) -> 16 пикселей RGBA; dxt1 — с режимом «3 цвета + прозрачный»
static void color_block(const unsigned char *b, unsigned char px[16][4], int dxt1)
{
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    unsigned char pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    if (c0 > c1 || !dxt1) {
        for (int i = 0; i < 3; i++) {
            pal[2][i] = (unsigned char)((2 * pal[0][i] + pal[1][i] + 1) / 3);
            pal[3][i] = (unsigned char)((pal[0][i] + 2 * pal[1][i] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++) {
            pal[2][i] = (unsigned char)((pal[0][i] + pal[1][i]) / 2);
            pal[3][i] = 0;
        }
        pal[2][3] = 255;
        pal[3][3] = 0;
    }
    uint32_t idx = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++, idx >>= 2)
        memcpy(px[i], pal[idx & 3], 4);
}

static void alpha_dxt3(const unsigned char *b, unsigned char px[16][4])
{
    for (int i = 0; i < 16; i++) {
        unsigned a = (b[i / 2] >> ((i & 1) * 4)) & 15;
        px[i][3] = (unsigned char)(a << 4 | a);
    }
}

static void alpha_dxt5(const unsigned char *b, unsigned char px[16][4])
{
    unsigned a[8] = {b[0], b[1]};
    if (a[0] > a[1])
        for (int i = 1; i < 7; i++)
            a[i + 1] = ((7 - (unsigned)i) * a[0] + (unsigned)i * a[1] + 3) / 7;
    else {
        for (int i = 1; i < 5; i++)
            a[i + 1] = ((5 - (unsigned)i) * a[0] + (unsigned)i * a[1] + 2) / 5;
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++)
        bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++, bits >>= 3)
        px[i][3] = (unsigned char)a[bits & 7];
}

// уровень DXT -> RGBA8 (w x h, плотно); *has_alpha — есть ли не непрозрачные пиксели
static int decode_dxt(GLenum internal, GLsizei w, GLsizei h, const unsigned char *src, GLsizei size, unsigned char *out,
                      int *has_alpha)
{
    int bs = internal == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT || internal == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ? 8 : 16;
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    if ((size_t)size < (size_t)bw * (size_t)bh * (size_t)bs)
        return -1;
    int alpha = 0;
    unsigned char px[16][4];
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            const unsigned char *b = src + ((size_t)by * (size_t)bw + (size_t)bx) * (size_t)bs;
            if (bs == 8)
                color_block(b, px, internal == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT);
            else {
                color_block(b + 8, px, 0);
                if (internal == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT)
                    alpha_dxt3(b, px);
                else
                    alpha_dxt5(b, px);
            }
            for (int i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h)
                    continue;
                memcpy(out + ((size_t)y * (size_t)w + (size_t)x) * 4, px[i], 4);
                alpha |= px[i][3] != 255;
            }
        }
    *has_alpha = alpha;
    return 0;
}

void snx_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border,
                                GLsizei size, const void *data)
{
    remember_size(target, level, w, h);
    int dxt = internal == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT || internal == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ||
              internal == GL_COMPRESSED_RGBA_S3TC_DXT3_EXT || internal == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    if (!dxt || gles_caps.s3tc) {
        glCompressedTexImage2D(target, level, internal, w, h, border, size, data);
        return;
    }
    size_t n = (size_t)w * (size_t)h;
    unsigned char *rgba = data ? malloc(n * 4) : NULL;
    int alpha = 1;
    if (rgba && decode_dxt(internal, w, h, data, size, rgba, &alpha) != 0) {
        free(rgba);
        rgba = NULL;
    }
    tight_unpack();
    if (!gles_caps.tex16) {
        glTexImage2D(target, level, GL_RGBA8, w, h, border, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    } else {
        // экономия памяти: DXT1 без прозрачности — 565, с ней — 5551, DXT3/5 — 4444 (вдвое меньше RGBA8)
        uint16_t *p = rgba ? (uint16_t *)rgba : NULL;  // поверх того же буфера: 16 бит на месте 32
        int dxt1 = internal == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT || internal == GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
        int mode = dxt1 ? (alpha ? 1 : 0) : 2;
        for (size_t i = 0; p && i < n; i++) {
            const unsigned char *c = rgba + i * 4;
            unsigned r = c[0], g = c[1], b = c[2], a = c[3];
            p[i] = mode == 0   ? (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3)
                   : mode == 1 ? (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >> 7))
                               : (uint16_t)((r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | a >> 4);
        }
        static const GLint in[3] = {GL_RGB565, GL_RGB5_A1, GL_RGBA4};
        static const GLenum fm[3] = {GL_RGB, GL_RGBA, GL_RGBA};
        static const GLenum ty[3] = {GL_UNSIGNED_SHORT_5_6_5, GL_UNSIGNED_SHORT_5_5_5_1, GL_UNSIGNED_SHORT_4_4_4_4};
        if (!data)
            mode = dxt1 ? 1 : 2;
        glTexImage2D(target, level, in[mode], w, h, border, fm[mode], ty[mode], p);
    }
    restore_unpack();
    free(rgba);
}

// ---------------------------------------------------------------- чтение пикселей
// RGBA8 (как читает ES) -> запрошенный формат; d — строка приёмника
static int pack_row(GLenum format, GLenum type, const unsigned char *s, unsigned char *d, int w)
{
    if (format == GL_RGBA && type == GL_UNSIGNED_BYTE)
        memcpy(d, s, (size_t)w * 4);
    else if (format == GL_BGRA && type == GL_UNSIGNED_BYTE)
        for (int x = 0; x < w; x++, s += 4, d += 4) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = s[3];
        }
    else if (format == GL_BGR && type == GL_UNSIGNED_BYTE)
        for (int x = 0; x < w; x++, s += 4, d += 3) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
        }
    else if (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5)
        for (int x = 0; x < w; x++, s += 4, d += 2) {
            uint16_t v = (uint16_t)((s[0] >> 3) << 11 | (s[1] >> 2) << 5 | s[2] >> 3);
            memcpy(d, &v, 2);
        }
    else if (format == GL_BGRA && type == GL_UNSIGNED_SHORT_1_5_5_5_REV)
        for (int x = 0; x < w; x++, s += 4, d += 2) {
            uint16_t v = (uint16_t)((s[3] >> 7) << 15 | (s[0] >> 3) << 10 | (s[1] >> 3) << 5 | s[2] >> 3);
            memcpy(d, &v, 2);
        }
    else if (format == GL_BGRA && type == GL_UNSIGNED_SHORT_4_4_4_4_REV)
        for (int x = 0; x < w; x++, s += 4, d += 2) {
            uint16_t v = (uint16_t)((s[3] >> 4) << 12 | (s[0] >> 4) << 8 | (s[1] >> 4) << 4 | s[2] >> 4);
            memcpy(d, &v, 2);
        }
    else if (format == GL_RED && type == GL_UNSIGNED_BYTE)
        for (int x = 0; x < w; x++, s += 4)
            d[x] = s[0];
    else if (format == GL_RG && (type == GL_UNSIGNED_BYTE || type == GL_BYTE))
        for (int x = 0; x < w; x++, s += 4, d += 2) {
            d[0] = s[0];
            d[1] = s[1];
        }
    else
        return -1;
    return 0;
}

static int bpp_of(GLenum format, GLenum type)
{
    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_1_5_5_5_REV ||
        type == GL_UNSIGNED_SHORT_4_4_4_4_REV || type == GL_UNSIGNED_SHORT)
        return 2;
    switch (format) {
    case GL_RED: return 1;
    case GL_RG: return 2;
    case GL_RGB: case GL_BGR: return 3;
    default: return 4;
    }
}

// из текущего READ_FRAMEBUFFER: всегда RGBA8 (единственное, что ES обязан уметь), затем перекладка
static void read_converted(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *out)
{
    int depth = format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL;
    size_t dst_pitch = align_up((size_t)(g_pack_row ? g_pack_row : w) * (size_t)bpp_of(format, type), g_pack_align);
    size_t row = (size_t)w * (size_t)bpp_of(format, type);
    unsigned char *tmp = depth ? NULL : scratch((size_t)w * (size_t)h * 4);
    if (tmp) {
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
        glPixelStorei(GL_PACK_ROW_LENGTH, g_pack_row);
        glPixelStorei(GL_PACK_ALIGNMENT, g_pack_align);
    }
    static int warned;
    for (GLsizei r = 0; r < h; r++) {
        unsigned char *d = (unsigned char *)out + (size_t)r * dst_pitch;
        if (!tmp || pack_row(format, type, tmp + (size_t)r * (size_t)w * 4, d, w) != 0) {
            memset(d, 0, row);  // глубину ES не читает; неизвестный формат — нули
            if (!warned++)
                fprintf(stderr, "[gles] cannot read pixels in format 0x%x/0x%x: zeros\n", format, type);
        }
    }
}

void snx_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *out)
{
    if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
        glReadPixels(x, y, w, h, format, type, out);
        return;
    }
    read_converted(x, y, w, h, format, type, out);
}

void snx_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void *out)
{
    // уровень текстуры — в цвет временного FBO, оттуда glReadPixels (в ES нет glGetTexImage)
    GLenum bind = target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_CUBE_MAP;
    GLint tex = 0, rfb = 0, w = 0, h = 0;
    glGetIntegerv(bind, &tex);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb);
    texsize *ts = tex ? ts_slot((GLuint)tex, 0) : NULL;
    if (!ts)
        return;
    w = ts->w >> level;
    h = ts->h >> level;
    w = w < 1 ? 1 : w;
    h = h < 1 ? 1 : h;
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, (GLuint)tex, level);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        read_converted(0, 0, w, h, format, type, out);
    else {
        size_t pitch = align_up((size_t)(g_pack_row ? g_pack_row : w) * (size_t)bpp_of(format, type), g_pack_align);
        memset(out, 0, pitch * (size_t)h);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb);
    glDeleteFramebuffers(1, &fb);
}

void snx_glGetCompressedTexImage(GLenum target, GLint level, void *out)
{
    // в ES сжатое не прочитать. D3DDrv перезаписывает заблокированный уровень целиком, старое содержимое ему не
    // нужно [проверить]; размер приёмника неизвестен — не трогаем
    (void)target;
    (void)level;
    (void)out;
    static int warned;
    if (!warned++)
        fprintf(stderr, "[gles] compressed texture readback is not supported in ES: lock keeps old guest memory\n");
}

// ---------------------------------------------------------------- состояние
void snx_glClearDepth(double d) { glClearDepthf((GLfloat)d); }
void snx_glDepthRange(double n, double f) { glDepthRangef((GLfloat)n, (GLfloat)f); }

void snx_glPolygonMode(GLenum face, GLenum mode)
{
    (void)face;
    static int warned;
    if (mode != GL_FILL && !warned++)
        fprintf(stderr, "[gles] wireframe/point fill mode is not available in ES: solid\n");
}

void snx_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint base)
{
    if (g_draw_bv)
        g_draw_bv(mode, count, type, indices, base);
    else {
        // слой D3D8 сам сдвигает указатели атрибутов, когда base != 0 и функции нет (d3d8_ff.c, draw)
        static int warned;
        if (base && !warned++)
            fprintf(stderr, "[gles] glDrawElementsBaseVertex(base %d) without support\n", base);
        glDrawElements(mode, count, type, indices);
    }
}

void snx_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p)
{
    // D3DCOLOR (size GL_BGRA): в ES нет — 4 байта как есть, порядок B,G,R,A переставляет шейдер (ffkey.bgra)
    glVertexAttribPointer(index, size == GL_BGRA ? 4 : size, type, norm, stride, p);
}

static GLint wrap(GLint v) { return v == GL_CLAMP_TO_BORDER && !gles_caps.border_clamp ? GL_CLAMP_TO_EDGE : v; }

void snx_glSamplerParameteri(GLuint s, GLenum pname, GLint v)
{
    if (pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T || pname == GL_TEXTURE_WRAP_R)
        v = wrap(v);
    glSamplerParameteri(s, pname, v);
}

void snx_glSamplerParameterf(GLuint s, GLenum pname, GLfloat v)
{
    if (pname == GL_TEXTURE_LOD_BIAS)
        return;  // в ES только смещением в шейдере
    glSamplerParameterf(s, pname, v);
}

void snx_glSamplerParameterfv(GLuint s, GLenum pname, const GLfloat *v)
{
    if (pname == GL_TEXTURE_BORDER_COLOR && !gles_caps.border_clamp)
        return;
    glSamplerParameterfv(s, pname, v);
}

static int clip_cap(GLenum cap) { return cap >= GL_CLIP_DISTANCE0 && cap < GL_CLIP_DISTANCE0 + 8; }

void snx_glEnable(GLenum cap)
{
    if (clip_cap(cap) && !gles_caps.clip_distance)
        return;  // отсечение — discard в шейдере
    glEnable(cap);
}

void snx_glDisable(GLenum cap)
{
    if (clip_cap(cap) && !gles_caps.clip_distance)
        return;
    glDisable(cap);
}

void snx_glRenderbufferStorage(GLenum target, GLenum internal, GLsizei w, GLsizei h)
{
    if (internal == GL_DEPTH_COMPONENT32)
        internal = GL_DEPTH_COMPONENT24;
    glRenderbufferStorage(target, internal, w, h);
}
