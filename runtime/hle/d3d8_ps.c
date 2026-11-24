// HLE Direct3D 8: пиксельные шейдеры ps.1.1–1.4 (байткод) -> тело GLSL. D3DDrv собирает свои шейдеры
// (смешивание слоёв террейна) встроенным ассемблером D3DX8; нужное подмножество: def, tex/texld,
// texcoord/texcrd, mov, add, sub, mul, mad, dp3, dp4, lrp, cnd, cmp, texkill, модификаторы и маски.
// Интерфейс тела (d3d8_ff.c): входы v0, v1, tc0..tc5, сэмплеры u_tex0..5, константы u_psc[8]; результат — cur.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/hle/d3d8_int.h"

#define MAX_PS 64
#define MAX_PS_TOKENS 4096u

typedef struct {
    char *glsl;
    u32 *tokens;
    u32 ntok;
    u64 hash;
} ps_obj;

// Раздельные таблицы дескрипторов для Core 1 (g_ps) и Core 2 (r_ps): освобождение ps_obj идёт через gl_cs,
// поэтому поток рендера всегда читает неизменяемую копию без гонок при DeletePixelShader.
static ps_obj *g_ps[MAX_PS];
static ps_obj *r_ps[MAX_PS];

typedef struct {
    u32 idx;
    ps_obj *obj;
} cmd_ps_t;

static void x_ps_set(void *p)
{
    const cmd_ps_t *q = p;
    r_ps[q->idx] = q->obj;
}

static void x_ps_del(void *p)
{
    const cmd_ps_t *q = p;
    if (r_ps[q->idx] == q->obj)
        r_ps[q->idx] = NULL;
    if (q->obj) {
        free(q->obj->glsl);
        free(q->obj->tokens);
        free(q->obj);
    }
}

typedef struct {
    char *p;
    size_t n, cap;
} sb;

static void out(sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(sb *b, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        if (!b->p) {
            b->cap = 4096;
            b->p = malloc(b->cap);
            if (!b->p)
                return;
        }
        va_start(ap, fmt);
        int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (w >= 0 && b->n + (size_t)w < b->cap) {
            b->n += (size_t)w;
            return;
        }
        b->cap *= 2;
        char *np = realloc(b->p, b->cap);
        if (!np)
            return;
        b->p = np;
    }
}

// Гарантированный float-литерал GLSL 410 core (всегда с точкой или экспонентой, без NaN/Inf)
static void fmt_float(char *o, size_t cap, float v)
{
    if (!isfinite(v))
        v = 0.0f;
    snprintf(o, cap, "%.8g", (double)v);
    if (!strchr(o, '.') && !strchr(o, 'e') && !strchr(o, 'E')) {
        size_t len = strlen(o);
        if (len + 2 < cap) {
            o[len] = '.';
            o[len + 1] = '0';
            o[len + 2] = 0;
        }
    }
}

static u64 fnv64(const void *p, size_t n)
{
    const u8 *b = p;
    u64 h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 1099511628211ull;
    return h ? h : 1ull;  // 0 зарезервирован под фиксированный конвейер без PS
}

enum { R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_TEXTURE = 3 };

static void reg_name(u32 tok, int ver, char *o, size_t cap)
{
    u32 type = (tok >> 28) & 7, n = tok & 0x7FF;
    switch (type) {
    case R_TEMP: snprintf(o, cap, "r%u", n < 6 ? n : 0); break;
    case R_INPUT: snprintf(o, cap, "v%u", n < 2 ? n : 0); break;
    case R_CONST: snprintf(o, cap, "c%u", n < 8 ? n : 0); break;
    case R_TEXTURE: snprintf(o, cap, ver >= 0x104 ? "tc%u" : "t%u", n < 6 ? n : 0); break;
    default: snprintf(o, cap, "r0");
    }
}

static void src_expr(u32 tok, int ver, char *o, size_t cap)
{
    char r[16], v[64];
    reg_name(tok, ver, r, sizeof r);
    u32 sw = (tok >> 16) & 0xFF;
    if (sw == 0xE4)
        snprintf(v, sizeof v, "%s", r);
    else
        snprintf(v, sizeof v, "%s.%c%c%c%c", r, "xyzw"[sw & 3], "xyzw"[(sw >> 2) & 3], "xyzw"[(sw >> 4) & 3],
                 "xyzw"[(sw >> 6) & 3]);
    switch ((tok >> 24) & 0xF) {
    case 1: snprintf(o, cap, "(-%s)", v); break;
    case 2: snprintf(o, cap, "(%s - 0.5)", v); break;
    case 3: snprintf(o, cap, "(0.5 - %s)", v); break;
    case 4: snprintf(o, cap, "(2.0 * (%s - 0.5))", v); break;
    case 5: snprintf(o, cap, "(-2.0 * (%s - 0.5))", v); break;
    case 6: snprintf(o, cap, "(1.0 - %s)", v); break;
    case 7: snprintf(o, cap, "(2.0 * %s)", v); break;
    case 8: snprintf(o, cap, "(-2.0 * %s)", v); break;
    case 11: snprintf(o, cap, "abs(%s)", v); break;
    default: snprintf(o, cap, "%s", v);
    }
}

// Запись результата: сдвиг (_x2/_x4/_x8/_d2/_d4/_d8), насыщение, маска
static void dst_write(sb *b, u32 tok, int ver, const char *expr)
{
    char r[16], mask[8] = ".", e[288];
    reg_name(tok, ver, r, sizeof r);
    u32 wm = (tok >> 16) & 0xF, mod = (tok >> 20) & 0xF;
    int shift = (int)((tok >> 24) & 0xF);
    if (shift >= 8)
        shift -= 16;
    if (shift) {
        float scale = shift > 0 ? (float)(1 << shift) : 1.0f / (float)(1 << -shift);
        char sf[32];
        fmt_float(sf, sizeof sf, scale);
        snprintf(e, sizeof e, "(%s) * %s", expr, sf);
    } else
        snprintf(e, sizeof e, "%s", expr);
    const char *lo = (mod & 1) ? "0.0" : "-8.0", *hi = (mod & 1) ? "1.0" : "8.0";
    if (wm == 0xF || wm == 0)
        out(b, "  %s = clamp(%s, %s, %s);\n", r, e, lo, hi);
    else {
        int k = 1;
        for (int i = 0; i < 4; i++)
            if (wm & (1u << i))
                mask[k++] = "xyzw"[i];
        mask[k] = 0;
        out(b, "  %s%s = clamp(vec4(%s)%s, %s, %s);\n", r, mask, e, mask, lo, hi);
    }
}

char *d3d8_ps_translate(const u32 *t, u32 max, u32 *out_ntok, u64 *out_hash)
{
    if (!t || !max || (t[0] & 0xFFFF0000u) != 0xFFFF0000u) {
        fprintf(stderr, "[d3d8] pixel shader: bad version token %08x\n", (t && max) ? t[0] : 0);
        return NULL;
    }
    int ver = (int)(t[0] & 0xFFFFu);
    sb b = {0};
    out(&b, "  // ps.%d.%d\n", ver >> 8, ver & 0xFF);
    for (int i = 0; i < 6; i++)
        out(&b, "  vec4 r%d = vec4(0.0), t%d = vec4(0.0);\n", i, i);
    for (int i = 0; i < 8; i++)
        out(&b, "  vec4 c%d = u_psc[%d];\n", i, i);
    u32 i = 1, end_tok = 0;
    while (i < max) {
        u32 tok = t[i], op = tok & 0xFFFFu;
        if (tok == 0x0000FFFFu) {
            end_tok = i + 1;
            break;
        }
        if (op == 0xFFFEu) {  // комментарий
            u32 skip = 1u + ((tok >> 16) & 0x7FFFu);
            if (skip > max - i) {
                fprintf(stderr, "[d3d8] pixel shader: truncated comment token\n");
                free(b.p);
                return NULL;
            }
            i += skip;
            continue;
        }
        if (op == 0xFFFDu || op == 0u) {  // phase, nop
            i++;
            continue;
        }
        u32 n = 0;  // число токенов операндов за кодом операции
        switch (op) {
        case 81: n = 5; break;                         // def c#, x, y, z, w
        case 66: case 64: n = ver >= 0x104 ? 2 : 1; break;
        case 65: n = 1; break;                         // texkill
        case 1: n = 2; break;                          // mov
        case 2: case 3: case 5: case 8: case 9: n = 3; break;
        case 4: case 18: case 80: case 88: n = 4; break;
        default:
            fprintf(stderr, "[d3d8] pixel shader ps.%d.%d: opcode %u not supported\n", ver >> 8, ver & 0xFF, op);
            free(b.p);
            return NULL;
        }
        if (max - i < 1u + n) {
            fprintf(stderr, "[d3d8] pixel shader ps.%d.%d: truncated instruction %u\n", ver >> 8, ver & 0xFF, op);
            free(b.p);
            return NULL;
        }
        const u32 *a = &t[i + 1];
        char s0[96], s1[96], s2[96], e[512];
        switch (op) {
        case 81: {  // def c#, x, y, z, w
            float f[4];
            memcpy(f, a + 1, 16);
            char r[16], f0[32], f1[32], f2[32], f3[32];
            reg_name(a[0], ver, r, sizeof r);
            fmt_float(f0, sizeof f0, f[0]);
            fmt_float(f1, sizeof f1, f[1]);
            fmt_float(f2, sizeof f2, f[2]);
            fmt_float(f3, sizeof f3, f[3]);
            out(&b, "  %s = vec4(%s, %s, %s, %s);\n", r, f0, f1, f2, f3);
            break;
        }
        case 66: {  // tex / texld
            u32 st = a[0] & 0x7FFu;
            if (st >= 6)
                st = 0;
            if (ver >= 0x104) {  // texld rN, tM|rM: сэмплер N
                char src[16];
                reg_name(a[1], ver, src, sizeof src);
                snprintf(e, sizeof e, "texture(u_tex%u, %s.xy)", st, src);
            } else {
                snprintf(e, sizeof e, "texture(u_tex%u, tc%u.xy)", st, st);
            }
            dst_write(&b, a[0], ver, e);
            break;
        }
        case 64:  // texcoord / texcrd
            if (ver >= 0x104) {
                src_expr(a[1], ver, s0, sizeof s0);
                dst_write(&b, a[0], ver, s0);
            } else {
                u32 st = a[0] & 0x7FFu;
                if (st >= 6)
                    st = 0;
                snprintf(e, sizeof e, "clamp(tc%u, 0.0, 1.0)", st);
                dst_write(&b, a[0], ver, e);
            }
            break;
        case 65: {  // texkill
            char r[16];
            reg_name(a[0], ver, r, sizeof r);
            out(&b, "  if (any(lessThan(%s.xyz, vec3(0.0)))) discard;\n", r);
            break;
        }
        case 1:
            src_expr(a[1], ver, s0, sizeof s0);
            dst_write(&b, a[0], ver, s0);
            break;
        case 2: case 3: case 5: case 8: case 9:
            src_expr(a[1], ver, s0, sizeof s0);
            src_expr(a[2], ver, s1, sizeof s1);
            if (op == 2)
                snprintf(e, sizeof e, "%s + %s", s0, s1);
            else if (op == 3)
                snprintf(e, sizeof e, "%s - %s", s0, s1);
            else if (op == 5)
                snprintf(e, sizeof e, "%s * %s", s0, s1);
            else if (op == 8)
                snprintf(e, sizeof e, "vec4(dot(vec3(%s), vec3(%s)))", s0, s1);
            else
                snprintf(e, sizeof e, "vec4(dot(vec4(%s), vec4(%s)))", s0, s1);
            dst_write(&b, a[0], ver, e);
            break;
        case 4: case 18: case 80: case 88:
            src_expr(a[1], ver, s0, sizeof s0);
            src_expr(a[2], ver, s1, sizeof s1);
            src_expr(a[3], ver, s2, sizeof s2);
            if (op == 4)
                snprintf(e, sizeof e, "%s * %s + %s", s0, s1, s2);
            else if (op == 18)
                snprintf(e, sizeof e, "mix(%s, %s, %s)", s2, s1, s0);
            else if (op == 80 && ver >= 0x104)  // cnd: src0 > 0.5 ? src1 : src2 (ps.1.4 — покомпонентно)
                snprintf(e, sizeof e, "mix(%s, %s, vec4(greaterThan(vec4(%s), vec4(0.5))))", s2, s1, s0);
            else if (op == 80)  // ps.1.1–1.3: по альфе src0
                snprintf(e, sizeof e, "(vec4(%s).a > 0.5 ? vec4(%s) : vec4(%s))", s0, s1, s2);
            else  // cmp: src0 >= 0 ? src1 : src2
                snprintf(e, sizeof e, "mix(%s, %s, vec4(greaterThanEqual(vec4(%s), vec4(0.0))))", s2, s1, s0);
            dst_write(&b, a[0], ver, e);
            break;
        }
        i += 1u + n;
    }
    if (!end_tok)
        end_tok = i;
    out(&b, "  cur = r0;\n");
    if (out_ntok)
        *out_ntok = end_tok;
    if (out_hash)
        *out_hash = fnv64(t, (size_t)end_tok * sizeof(u32));
    return b.p;
}

u32 d3d8_ps_create(const u32 *t, u32 max)
{
    u32 ntok = 0;
    u64 hash = 0;
    char *glsl = d3d8_ps_translate(t, max, &ntok, &hash);
    if (!glsl)
        return 0;
    for (u32 h = 0; h < MAX_PS; h++) {
        if (!g_ps[h]) {
            ps_obj *obj = calloc(1, sizeof *obj);
            if (!obj) {
                free(glsl);
                return 0;
            }
            obj->glsl = glsl;
            obj->hash = hash;
            if (ntok && ntok <= MAX_PS_TOKENS) {
                obj->tokens = malloc((size_t)ntok * sizeof(u32));
                if (obj->tokens) {
                    memcpy(obj->tokens, t, (size_t)ntok * sizeof(u32));
                    obj->ntok = ntok;
                }
            }
            g_ps[h] = obj;
            cmd_ps_t cmd = {h, obj};
            cs_cmd_push(x_ps_set, &cmd, sizeof cmd);
            return h + 1;
        }
    }
    free(glsl);
    return 0;
}

void d3d8_ps_delete(u32 h)
{
    if (h && h <= MAX_PS && g_ps[h - 1]) {
        ps_obj *obj = g_ps[h - 1];
        g_ps[h - 1] = NULL;
        cmd_ps_t cmd = {h - 1, obj};
        cs_cmd_push(x_ps_del, &cmd, sizeof cmd);
    }
}

const char *d3d8_ps_glsl(u32 h) { return h && h <= MAX_PS && g_ps[h - 1] ? g_ps[h - 1]->glsl : NULL; }
u64 d3d8_ps_hash(u32 h) { return h && h <= MAX_PS && g_ps[h - 1] ? g_ps[h - 1]->hash : 0; }

const u32 *d3d8_ps_tokens(u32 h, u32 *out_ntok)
{
    if (!h || h > MAX_PS || !g_ps[h - 1] || !g_ps[h - 1]->tokens) {
        if (out_ntok)
            *out_ntok = 0;
        return NULL;
    }
    if (out_ntok)
        *out_ntok = g_ps[h - 1]->ntok;
    return g_ps[h - 1]->tokens;
}

const char *d3d8_r_ps_glsl(u32 h) { return h && h <= MAX_PS && r_ps[h - 1] ? r_ps[h - 1]->glsl : NULL; }
u64 d3d8_r_ps_hash(u32 h) { return h && h <= MAX_PS && r_ps[h - 1] ? r_ps[h - 1]->hash : 0; }

const u32 *d3d8_r_ps_tokens(u32 h, u32 *out_ntok)
{
    if (!h || h > MAX_PS || !r_ps[h - 1] || !r_ps[h - 1]->tokens) {
        if (out_ntok)
            *out_ntok = 0;
        return NULL;
    }
    if (out_ntok)
        *out_ntok = r_ps[h - 1]->ntok;
    return r_ps[h - 1]->tokens;
}
