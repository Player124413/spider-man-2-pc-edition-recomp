// Отражение объектов UnrealScript гостя (см. uobj.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "runtime/game/uobj.h"
#include "runtime/process.h"
#include "runtime/vmem.h"

static u32 g_names, g_objects;  // гостевые адреса TArray: FName::Names, UObject::GObjObjects

int uo_init(void)
{
    if (g_objects)
        return 1;
    rt_module *core = rt_module_by_name("Core");
    if (!core)
        return 0;
    g_names = rt_module_export(core, "?Names@FName@@0V?$TArray@PAUFNameEntry@@@@A", 0);
    g_objects = rt_module_export(core, "?GObjObjects@UObject@@0V?$TArray@PAVUObject@@@@A", 0);
    return g_objects != 0;
}

const char *uo_name(u32 obj)
{
    static _Thread_local char buf[64];
    buf[0] = 0;
    if (!obj || !g_names)
        return buf;
    u32 idx = rd32(g_mem, obj + UO_NAME);
    if (idx >= rd32(g_mem, g_names + 4))
        return buf;
    u32 e = rd32(g_mem, rd32(g_mem, g_names) + 4 * idx);
    if (!e)
        return buf;
    for (u32 i = 0; i < sizeof buf - 1; i++) {
        u16 ch = rd16(g_mem, e + 0xC + 2 * i);
        buf[i] = (char)(ch < 0x80 ? ch : '?');
        if (!ch)
            break;
    }
    buf[sizeof buf - 1] = 0;
    return buf;
}

int uo_name_is(u32 obj, const char *name) { return obj && !strcasecmp(uo_name(obj), name); }
u32 uo_class(u32 obj) { return obj ? rd32(g_mem, obj + UO_CLASS) : 0; }
u32 uo_outer(u32 obj) { return obj ? rd32(g_mem, obj + UO_OUTER) : 0; }
u32 uo_count(void) { return g_objects ? rd32(g_mem, g_objects + 4) : 0; }
u32 uo_at(u32 i) { return i < uo_count() ? rd32(g_mem, rd32(g_mem, g_objects) + 4 * i) : 0; }

// v — живой объект: память зафиксирована, и GObjObjects[v->Index] == v
static int uo_valid(u32 v)
{
    vm_info mi;
    if (v < 0x10000 || (v & 3) || !vm_query(v, &mi) || mi.state != VM_MEM_COMMIT || v + 0x28 > mi.base + mi.size)
        return 0;
    return uo_at(rd32(g_mem, v + 4)) == v;
}

// Раскладка полей этой сборки (разведка SNX_UO_DUMP): UField — SuperField +0x2C, Next +0x30; UStruct — Children
// +0x40; UProperty — ArrayDim +0x38, ElementSize +0x3C, Offset +0x4C; UBoolProperty — BitMask +0x70.
#define UF_SUPER 0x2Cu
#define UF_NEXT 0x30u
#define US_CHILDREN 0x40u
#define UP_DIM 0x38u
#define UP_OFFSET 0x4Cu
#define UBP_MASK 0x70u

static int is_property(u32 f)
{
    const char *cn = uo_name(uo_class(f));
    size_t n = strlen(cn);
    return n > 8 && !strcmp(cn + n - 8, "Property");
}

// индекс FName по строке (без учёта регистра ASCII); -1 — такого имени нет
static s32 fname(const char *name)
{
    u32 n = g_names ? rd32(g_mem, g_names + 4) : 0, data = g_names ? rd32(g_mem, g_names) : 0;
    for (u32 i = 0; i < n; i++) {
        u32 e = rd32(g_mem, data + 4 * i), k = 0;
        if (!e)
            continue;
        for (;; k++) {
            u16 ch = rd16(g_mem, e + 0xC + 2 * k);
            unsigned a = (unsigned char)name[k];
            if ((ch | 0x20u) != (a | 0x20u) || (ch && !a) || (!ch && a))
                break;
            if (!ch)
                return (s32)i;
        }
    }
    return -1;
}

u32 uo_find_class(const char *name)
{
    s32 idx = fname(name), cls = fname("Class");
    for (u32 i = 0, n = idx >= 0 && cls >= 0 ? uo_count() : 0; i < n; i++) {
        u32 o = uo_at(i);
        if (o && rd32(g_mem, o + UO_NAME) == (u32)idx && rd32(g_mem, uo_class(o) + UO_NAME) == (u32)cls)
            return o;
    }
    return 0;
}

int uo_is_a_class(u32 obj, u32 cls)
{
    for (u32 c = uo_class(obj), k = 0; c && k < 64; c = rd32(g_mem, c + UF_SUPER), k++)
        if (c == cls)
            return 1;
    return 0;
}

int uo_is_a(u32 obj, const char *class_name)
{
    for (u32 c = uo_class(obj), k = 0; c && k < 64; c = rd32(g_mem, c + UF_SUPER), k++)
        if (uo_name_is(c, class_name))
            return 1;
    return 0;
}

int uo_find_prop(u32 cls, const char *name, uo_prop *out)
{
    for (u32 s = cls, k = 0; s && k < 64; s = rd32(g_mem, s + UF_SUPER), k++)
        for (u32 f = rd32(g_mem, s + US_CHILDREN), j = 0; f && j < 4096; f = rd32(g_mem, f + UF_NEXT), j++)
            if (uo_name_is(f, name) && is_property(f)) {
                out->prop = f;
                out->offset = rd32(g_mem, f + UP_OFFSET);
                out->bitmask = uo_name_is(uo_class(f), "BoolProperty") ? rd32(g_mem, f + UBP_MASK) : 0;
                return 1;
            }
    return 0;
}

u32 uo_get_u32(u32 obj, const char *name)
{
    uo_prop p;
    return obj && uo_find_prop(uo_class(obj), name, &p) ? rd32(g_mem, obj + p.offset) : 0;
}

int uo_get_bool(u32 obj, const char *name)
{
    uo_prop p;
    if (!obj || !uo_find_prop(uo_class(obj), name, &p) || !p.bitmask)
        return -1;
    return (rd32(g_mem, obj + p.offset) & p.bitmask) != 0;
}

int uo_set_bool(u32 obj, const char *name, int v)
{
    uo_prop p;
    if (!obj || !uo_find_prop(uo_class(obj), name, &p) || !p.bitmask)
        return 0;
    u32 w = rd32(g_mem, obj + p.offset);
    wr32(g_mem, obj + p.offset, v ? w | p.bitmask : w & ~p.bitmask);
    return 1;
}

// ---------------------------------------------------------------- вызовы гостя
// Регистры гостя сохраняются и восстанавливаются целиком; буфер (параметры, строки) — на стеке гостя ниже его
// текущей вершины, вызовы кладут свои аргументы ниже буфера.
typedef struct {
    CPU *c, save;
    u32 buf;
} gcall;

static int gcall_begin(gcall *g, u32 size)
{
    GuestThread *t = rt_thread_current();
    if (!t || size > 0x1000)
        return 0;
    g->c = &t->cpu;
    g->save = *g->c;
    g->buf = (g->save.esp - size - 0x40) & ~15u;
    g->c->esp = g->buf - 0x10;
    memset(g_mem + g->buf, 0, size);
    return 1;
}

static void gcall_end(gcall *g) { *g->c = g->save; }

static u32 gcall_this(gcall *g, u32 fn, u32 self, int n, const u32 *args)
{
    g->c->ecx = self;
    return rt_guest_call(g->c, fn, n, args);
}

static u32 core_export(const char *name)
{
    rt_module *core = rt_module_by_name("Core");
    return core ? rt_module_export(core, name, 0) : 0;
}

int uo_call(u32 obj, const char *func, const u32 *parms, int n, u32 *ret)
{
    static u32 find_fn, process_fn;
    static struct {
        const char *name;
        s32 idx;
    } cache[32];
    if (!obj || n < 0 || n > 16)
        return 0;
    if (!find_fn) {
        find_fn = core_export("?FindFunction@UObject@@QAEPAVUFunction@@VFName@@H@Z");
        process_fn = core_export("?ProcessEvent@UObject@@UAEXPAVUFunction@@PAX1@Z");
        if (!find_fn || !process_fn)
            return 0;
    }
    s32 idx = -1;
    int k = 0;
    while (k < 32 && cache[k].name && strcmp(cache[k].name, func))
        k++;
    if (k < 32 && cache[k].name)
        idx = cache[k].idx;
    else if ((idx = fname(func)) >= 0 && k < 32) {
        cache[k].name = func;
        cache[k].idx = idx;
    }
    gcall g;
    if (idx < 0 || !gcall_begin(&g, 0x100))
        return 0;
    u32 a[3] = {(u32)idx, 0, 0}, fn = gcall_this(&g, find_fn, obj, 2, a);
    if (fn) {
        for (int i = 0; i < n; i++)
            wr32(g_mem, g.buf + 4 * (u32)i, parms[i]);
        a[0] = fn;
        a[1] = g.buf;
        a[2] = 0;
        gcall_this(&g, process_fn, obj, 3, a);
        if (ret)
            *ret = rd32(g_mem, g.buf + 4 * (u32)n);
    }
    gcall_end(&g);
    return fn != 0;
}

// UTF-8 -> UTF-16 (BMP); -> длина в символах, не больше cap
static size_t utf16_of(const char *s, u16 *out, size_t cap)
{
    size_t k = 0;
    for (const u8 *p = (const u8 *)s; *p && k < cap;) {
        if (*p < 0x80)
            out[k++] = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) {
            out[k++] = (u16)((p[0] & 0x1F) << 6 | (p[1] & 0x3F));
            p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            out[k++] = (u16)((p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F));
            p += 3;
        } else
            p++;
    }
    return k;
}

int uo_set_str(u32 obj, const char *name, const char *s)
{
    static u32 assign_fn;
    uo_prop p;
    u16 w[256];
    size_t len = utf16_of(s, w, sizeof w / sizeof w[0]);
    if (!obj || !uo_find_prop(uo_class(obj), name, &p))
        return 0;
    u32 str = obj + p.offset, data = rd32(g_mem, str), num = rd32(g_mem, str + 4);
    if (!len && (!num || (data && num == 1 && !rd16(g_mem, data))))  // уже пустая строка
        return 1;
    if (data && num == len + 1) {  // уже такая строка
        size_t i = 0;
        while (i < len && rd16(g_mem, data + 2 * (u32)i) == w[i])
            i++;
        if (i == len)
            return 1;
    }
    if (!assign_fn && !(assign_fn = core_export("??4FString@@QAEAAV0@PBG@Z")))
        return 0;
    gcall g;
    if (!gcall_begin(&g, 2 * (u32)len + 2))
        return 0;
    for (size_t i = 0; i < len; i++)
        wr16(g_mem, g.buf + 2 * (u32)i, w[i]);
    u32 a[1] = {g.buf};
    gcall_this(&g, assign_fn, str, 1, a);
    gcall_end(&g);
    return 1;
}

void uo_dump(u32 obj)
{
    char cn[64];
    snprintf(cn, sizeof cn, "%s", uo_name(uo_class(obj)));
    fprintf(stderr, "[uobj] %08x %s (%s)\n", obj, uo_name(obj), cn);
    for (u32 s = uo_class(obj), k = 0; s && k < 64; s = rd32(g_mem, s + UF_SUPER), k++) {
        char sn[64];
        snprintf(sn, sizeof sn, "%s", uo_name(s));
        for (u32 f = rd32(g_mem, s + US_CHILDREN), j = 0; f && j < 4096; f = rd32(g_mem, f + UF_NEXT), j++) {
            if (!is_property(f))
                continue;
            u32 off = rd32(g_mem, f + UP_OFFSET), v = rd32(g_mem, obj + off), dim = rd32(g_mem, f + UP_DIM);
            char pc[64];
            snprintf(pc, sizeof pc, "%s", uo_name(uo_class(f)));
            fprintf(stderr, "  %s.%s +%03x %s%s = %08x", sn, uo_name(f), off, pc, dim > 1 ? "[]" : "", v);
            if (!strcmp(pc, "BoolProperty"))
                fprintf(stderr, " -> %d", (v & rd32(g_mem, f + UBP_MASK)) != 0);
            else if (!strcmp(pc, "ObjectProperty") && uo_valid(v))
                fprintf(stderr, " -> %s", uo_name(v));
            else if (!strcmp(pc, "FloatProperty"))
                fprintf(stderr, " -> %g", rdf32(g_mem, obj + off));
            else if (!strcmp(pc, "StrProperty") && v && rd32(g_mem, obj + off + 4)) {
                fprintf(stderr, " -> \"");
                for (u32 i = 0; i < 60; i++) {
                    u16 ch = rd16(g_mem, v + 2 * i);
                    if (!ch)
                        break;
                    fputc(ch < 0x80 ? ch : '?', stderr);
                }
                fputc('"', stderr);
            } else if (!strcmp(pc, "ArrayProperty"))
                fprintf(stderr, " -> num %u", rd32(g_mem, obj + off + 4));
            fputc('\n', stderr);
        }
    }
}

// дерево элементов меню: имя, класс, видимость, ввод, состояние, рамка, подпись; дети — Controls и MyPanel
static void tree(u32 comp, int depth)
{
    if (!comp || depth > 8)
        return;
    char nm[64], cn[64], cap[64] = "";
    snprintf(nm, sizeof nm, "%s", uo_name(comp));
    snprintf(cn, sizeof cn, "%s", uo_name(uo_class(comp)));
    uo_prop b, c;
    float r[4] = {0};
    if (uo_find_prop(uo_class(comp), "Bounds", &b))
        for (int i = 0; i < 4; i++)
            r[i] = rdf32(g_mem, comp + b.offset + 4 * (u32)i);
    if (uo_find_prop(uo_class(comp), "Caption", &c) && rd32(g_mem, comp + c.offset)) {
        u32 str = rd32(g_mem, comp + c.offset);
        for (u32 i = 0; i < sizeof cap - 1; i++) {
            u16 ch = rd16(g_mem, str + 2 * i);
            cap[i] = (char)(ch < 0x80 ? ch : '?');
            if (!ch)
                break;
        }
        cap[sizeof cap - 1] = 0;
    }
    int vis = uo_get_bool(comp, "bVisible"), inp = uo_get_bool(comp, "bAcceptsInput");
    uo_prop ms;
    int state = uo_find_prop(uo_class(comp), "MenuState", &ms) ? g_mem[comp + ms.offset] : -1;
    fprintf(stderr, "[tree] %*s%s (%s) vis %d inp %d st %d [%.0f %.0f %.0f %.0f] \"%s\"\n", depth * 2, "", nm, cn, vis,
            inp, state, r[0], r[1], r[2], r[3], cap);
    uo_prop p;
    if (uo_find_prop(uo_class(comp), "Controls", &p)) {
        u32 arr = rd32(g_mem, comp + p.offset), num = rd32(g_mem, comp + p.offset + 4);
        for (u32 i = 0; i < num && i < 200; i++)
            tree(rd32(g_mem, arr + 4 * i), depth + 1);
    }
    if (uo_find_prop(uo_class(comp), "MyPanel", &p))
        tree(rd32(g_mem, comp + p.offset), depth + 1);
}

void uo_explore(void)
{
    if (!uo_init())
        return;
    const char *what = getenv("SNX_UO_CLASS");
    for (u32 i = 0, n = uo_count(); i < n; i++) {
        u32 o = uo_at(i);
        if (!o || !uo_name_is(uo_class(o), what ? what : "ShGUIController"))
            continue;
        if (getenv("SNX_UO_ALL"))
            uo_dump(o);
        uo_prop ms;
        if (uo_find_prop(uo_class(o), "MenuStack", &ms)) {
            u32 arr = rd32(g_mem, o + ms.offset), num = rd32(g_mem, o + ms.offset + 4);
            for (u32 k = 0; k < num && k < 16; k++) {
                u32 pg = rd32(g_mem, arr + 4 * k);
                char cn[64];
                snprintf(cn, sizeof cn, "%s", uo_name(uo_class(pg)));
                fprintf(stderr, "[uobj] MenuStack[%u] %s (%s)\n", k, uo_name(pg), cn);
            }
        }
        u32 page = uo_get_u32(o, "ActivePage");
        if (what || !page)
            continue;
        tree(page, 0);
        if (!getenv("SNX_UO_ALL"))
            continue;
        uo_dump(page);
        uo_prop p;
        if (!uo_find_prop(uo_class(page), "Controls", &p))
            continue;
        u32 arr = rd32(g_mem, page + p.offset), num = rd32(g_mem, page + p.offset + 4);
        for (u32 k = 0; k < num && getenv("SNX_UO_ALL"); k++)
            uo_dump(rd32(g_mem, arr + 4 * k));
        for (u32 k = 0; k < num; k++) {
            u32 ctl = rd32(g_mem, arr + 4 * k);
            uo_prop b;
            float x = 0, y = 0, w = 0, h = 0;
            if (uo_find_prop(uo_class(ctl), "Bounds", &b)) {
                x = rdf32(g_mem, ctl + b.offset);
                y = rdf32(g_mem, ctl + b.offset + 4);
                w = rdf32(g_mem, ctl + b.offset + 8);
                h = rdf32(g_mem, ctl + b.offset + 12);
            }
            char cn[64], nm[64];
            snprintf(cn, sizeof cn, "%s", uo_name(uo_class(ctl)));
            snprintf(nm, sizeof nm, "%s", uo_name(ctl));
            int vis = uo_get_bool(ctl, "bVisible"), inp = uo_get_bool(ctl, "bAcceptsInput");
            fprintf(stderr, "  ctl %2u %-28s %-20s vis %d inp %d bounds %.0f %.0f %.0f %.0f\n", k, nm, cn, vis, inp, x, y, w, h);
        }
    }
}
