// Тест D3D8-уровневого command stream (без окна и GL-контекста):
//   1. Однопоточный COM (com_create_st без атомиков) и обычный COM (com_create).
//   2. Поток рендера gl_cs: cs_cmd_push, cs_cmd_push_var (внутри кольца и > BIG_DATA),
//      позиции cs_wr_pos / cs_tail_pos, отложенное освобождение, прямой режим (direct).
//   3. Гранулярные флаги d3d_rstate (d3d8_r_apply_rs / tss / xf / tex) и key_dirty.
//   4. Жизненный цикл кольцевых zero-copy слотов динамических VB/IB (d3d8_dyn_slot_pick).
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/heap.h"
#include "runtime/hle/com.h"
#include "runtime/hle/d3d8_direct.h"
#include "runtime/hle/d3d8_int.h"
#include "runtime/hle/gl_cs.h"
#include "runtime/rt.h"
#include "runtime/vmem.h"

static int g_fails;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "FAIL (%s:%d): %s\n", __FILE__, __LINE__, (msg));   \
            g_fails++;                                                          \
        }                                                                       \
    } while (0)

// ---------------------------------------------------------------- 1. COM (st vs atomic)
static int g_com_destroyed;
static void on_com_destroy(void *p)
{
    int *tag = p;
    if (tag && *tag == 0x1234)
        g_com_destroyed++;
}

static void test_com_st(void)
{
    guest_fn vt[3] = {NULL, com_AddRef, com_Release};
    u32 vtbl = com_vtable("TestCOM", vt, 3);
    int tag_st = 0x1234, tag_mt = 0x1234;
    u32 ost = com_create_st(vtbl, &tag_st, on_com_destroy);
    u32 omt = com_create(vtbl, &tag_mt, on_com_destroy);
    CHECK(ost != 0 && omt != 0, "com_create_st / com_create вернули валидные объекты");
    CHECK(com_host(ost) == &tag_st, "com_host(ost) совпадает");
    CHECK(com_addref(ost) == 2, "st addref -> 2");
    CHECK(com_addref(ost) == 3, "st addref -> 3");
    CHECK(com_release(ost) == 2, "st release -> 2");
    CHECK(com_release(ost) == 1, "st release -> 1");
    CHECK(g_com_destroyed == 0, "destroy не вызван до 0");
    CHECK(com_release(ost) == 0, "st release -> 0");
    CHECK(g_com_destroyed == 1, "st destroy вызван ровно 1 раз");
    CHECK(com_host(ost) == NULL, "после release до 0 com_host == NULL");
    CHECK(com_addref(omt) == 2 && com_release(omt) == 1 && com_release(omt) == 0, "atomic COM refcount");
    CHECK(g_com_destroyed == 2, "atomic destroy вызван ровно 1 раз");
}

// ---------------------------------------------------------------- 2. gl_cs SPSC команды и отложенное освобождение
typedef struct {
    u32 seq;
    u64 val;
} seq_cmd;

static u32 g_expect_seq;
static u64 g_sum;
static int g_seq_bad;

static void x_seq(void *p)
{
    const seq_cmd *q = p;
    if (q->seq != g_expect_seq)
        g_seq_bad++;
    g_expect_seq++;
    g_sum += q->val;
}

typedef struct {
    u32 len;
    u8 seed;
    const u8 *data;
    void *heap;
} var_cmd;

static int g_var_runs, g_var_bad;
static void x_var(void *p)
{
    const var_cmd *q = p;
    g_var_runs++;
    if (!q->data) {
        g_var_bad++;
        free(q->heap);
        return;
    }
    for (u32 i = 0; i < q->len; i++) {
        u8 want = (u8)(q->seed + (i & 0xFFu));
        if (q->data[i] != want) {
            g_var_bad++;
            break;
        }
    }
    free(q->heap);
}

typedef struct {
    u32 magic;
    int uses;
} deferred_obj;

static int g_def_bad, g_def_freed;
static void x_use_obj(void *p)
{
    deferred_obj *o = *(deferred_obj **)p;
    if (o->magic != 0xA5A55A5Au)
        g_def_bad++;
    o->uses++;
}
static void x_free_obj(void *p)
{
    deferred_obj *o = *(deferred_obj **)p;
    if (o->magic != 0xA5A55A5Au || o->uses != 100)
        g_def_bad++;
    o->magic = 0;
    free(o);
    g_def_freed++;
}

static void test_cs_ring(void)
{
    CHECK(cs_start(0) == 0, "cs_start(0) успешен");
    CHECK(cs_enabled() == 1, "cs_enabled() == 1");

    // 5000 компактных команд подряд: строгий FIFO и верный payload
    g_expect_seq = 0;
    g_sum = 0;
    g_seq_bad = 0;
    u64 want_sum = 0;
    for (u32 i = 0; i < 5000; i++) {
        seq_cmd c = {i, (u64)i * 3u + 7u};
        want_sum += c.val;
        cs_cmd_push(x_seq, &c, sizeof c);
    }
    uint64_t wr_after_seq = cs_wr_pos();
    CHECK(wr_after_seq > 0, "cs_wr_pos продвинулся");

    // Переменные данные: мелкие (в кольце) и крупные (> BIG_DATA = 4 МБ -> malloc)
    static const u32 sizes[] = {16u, 123u, 65536u, (5u << 20) + 17u};
    g_var_runs = 0;
    g_var_bad = 0;
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        u32 n = sizes[k];
        u8 *buf = malloc(n);
        u8 seed = (u8)(13u + k * 29u);
        for (u32 i = 0; i < n; i++)
            buf[i] = (u8)(seed + (i & 0xFFu));
        var_cmd vc = {n, seed, buf, NULL};
        cs_cmd_push_var(x_var, &vc, sizeof vc, buf, n, offsetof(var_cmd, data), offsetof(var_cmd, heap));
        // Сразу портим исходный буфер на Core 1 — Core 2 должен видеть собственную копию
        memset(buf, 0xEE, n);
        free(buf);
    }

    // Отложенное освобождение ресурса после 100 использований
    deferred_obj *obj = calloc(1, sizeof *obj);
    obj->magic = 0xA5A55A5Au;
    g_def_bad = 0;
    g_def_freed = 0;
    for (int i = 0; i < 100; i++)
        cs_cmd_push(x_use_obj, &obj, sizeof obj);
    cs_cmd_push(x_free_obj, &obj, sizeof obj);

    cs_finish();
    CHECK(cs_tail_pos() >= wr_after_seq, "cs_tail_pos >= cs_wr_pos после cs_finish");
    CHECK(g_expect_seq == 5000 && g_seq_bad == 0 && g_sum == want_sum, "5000 команд cs_cmd_push в точном FIFO");
    CHECK(g_var_runs == 4 && g_var_bad == 0, "cs_cmd_push_var (кольцо + >BIG_DATA) без искажений");
    CHECK(g_def_bad == 0 && g_def_freed == 1, "отложенное освобождение прошло после всех использований");

    cs_stop();
    CHECK(cs_enabled() == 0, "cs_enabled() == 0 после cs_stop");

    // Прямой (однопоточный) режим после cs_stop
    seq_cmd c = {5000, 42};
    cs_cmd_push(x_seq, &c, sizeof c);
    CHECK(g_expect_seq == 5001 && g_sum == want_sum + 42, "direct-режим cs_cmd_push исполняет синхронно");
}

// ---------------------------------------------------------------- 3. Гранулярные флаги d3d_rstate
static void test_rstate_dirty(void)
{
    d3d_rstate r;
    memset(&r, 0, sizeof r);

    // Blend RS (27) -> только GLD_BLEND, без key_dirty
    d3d8_r_apply_rs(&r, 27, 1);
    CHECK(r.gl_dirty == GLD_BLEND && r.key_dirty == 0, "RS_ALPHABLENDENABLE -> GLD_BLEND, key_dirty=0");
    r.gl_dirty = 0;
    // Повторное то же значение — ничего не помечается
    d3d8_r_apply_rs(&r, 27, 1);
    CHECK(r.gl_dirty == 0 && r.key_dirty == 0, "дубликат RS не ставит dirty");

    // AlphaTest RS (15) -> key_dirty=1, gl_dirty=0
    d3d8_r_apply_rs(&r, 15, 1);
    CHECK(r.key_dirty == 1 && r.gl_dirty == 0, "RS_ALPHATESTENABLE -> key_dirty=1");
    r.key_dirty = 0;

    // ClipPlane RS (152) -> GLD_CLIP и key_dirty=1
    d3d8_r_apply_rs(&r, 152, 0x3);
    CHECK((r.gl_dirty & GLD_CLIP) != 0 && r.key_dirty == 1, "RS_CLIPPLANEENABLE -> GLD_CLIP + key_dirty");
    r.gl_dirty = 0;
    r.key_dirty = 0;

    // Fog color RS (34) -> gen_const++, без key_dirty
    u32 gc = r.gen_const;
    d3d8_r_apply_rs(&r, 34, 0xFF112233u);
    CHECK(r.gen_const == gc + 1 && r.key_dirty == 0, "RS_FOGCOLOR -> gen_const++");

    // TSS: сэмплер (16) -> sampler_dirty, без key_dirty; COLOROP (1) -> key_dirty; bump (7) -> gen_bump++
    d3d8_r_apply_tss(&r, 2, 16, 2);
    CHECK(r.sampler_dirty == (1u << 2) && r.key_dirty == 0, "TSS_MAGFILTER -> sampler_dirty");
    r.sampler_dirty = 0;
    d3d8_r_apply_tss(&r, 0, 1, 4);
    CHECK(r.key_dirty == 1 && r.sampler_dirty == 0, "TSS_COLOROP -> key_dirty");
    r.key_dirty = 0;
    u32 gb = r.gen_bump;
    d3d8_r_apply_tss(&r, 1, 7, 0x3F800000u);
    CHECK(r.gen_bump == gb + 1 && r.key_dirty == 0, "TSS_BUMPENVMAT00 -> gen_bump++");

    // Текстуры: NULL -> 2D (key_dirty=1), 2D_a -> 2D_b (key_dirty=0!), 2D_b -> cube (key_dirty=1)
    d3dtex t2a = {.cube = 0}, t2b = {.cube = 0}, tcube = {.cube = 1};
    d3d8_r_apply_tex(&r, 0, &t2a);
    CHECK(r.key_dirty == 1 && r.tex[0] == &t2a, "tex NULL -> 2D ставит key_dirty");
    r.key_dirty = 0;
    d3d8_r_apply_tex(&r, 0, &t2b);
    CHECK(r.key_dirty == 0 && r.tex[0] == &t2b, "tex 2D -> 2D НЕ ставит key_dirty");
    d3d8_r_apply_tex(&r, 0, &tcube);
    CHECK(r.key_dirty == 1 && r.tex[0] == &tcube, "tex 2D -> cube ставит key_dirty");
    r.key_dirty = 0;
    d3d8_r_apply_tex(&r, 0, NULL);
    CHECK(r.key_dirty == 1 && r.tex[0] == NULL, "tex cube -> NULL ставит key_dirty");

    // Матрицы: view -> gen_xf + gen_view, world -> gen_xf, texm -> gen_texm (все без key_dirty)
    r.key_dirty = 0;
    float m[16] = {1};
    u32 gxf = r.gen_xf, gv = r.gen_view, gt = r.gen_texm;
    d3d8_r_apply_xf(&r, 2, m);
    CHECK(r.gen_xf == gxf + 1 && r.gen_view == gv + 1 && r.key_dirty == 0, "view -> gen_xf + gen_view");
    d3d8_r_apply_xf(&r, 256, m);
    CHECK(r.gen_xf == gxf + 2 && r.gen_view == gv + 1 && r.key_dirty == 0, "world -> gen_xf");
    d3d8_r_apply_xf(&r, 16, m);
    CHECK(r.gen_texm == gt + 1 && r.key_dirty == 0, "texm0 -> gen_texm");
}

// ---------------------------------------------------------------- 4. Слоты zero-copy динамических VB/IB
static void test_dyn_slots(void)
{
    u64 fence[DYN_SLOTS] = {0};
    int cur = -1;
    u32 zhi = 0;

    // 4 подряд DISCARD занимают слоты 0, 1, 2, 3
    for (int i = 0; i < DYN_SLOTS; i++) {
        int k = d3d8_dyn_slot_pick(DYN_SLOTS, cur, zhi, fence, 0, 0, LOCK_DISCARD);
        CHECK(k == i, "последовательный выбор свободного zero-copy слота");
        cur = k;
        fence[k] = (u64)(i + 1) * 100u;
        zhi = 256;
    }

    // 5-й DISCARD при tail < всех fence -> безопасный fallback (-1)
    CHECK(d3d8_dyn_slot_pick(DYN_SLOTS, cur, zhi, fence, 50, 0, LOCK_DISCARD) == -1,
          "все слоты заняты -> fallback -1");

    // Core 2 прошёл fence[0] = 100 -> слот 0 снова свободен
    int k0 = d3d8_dyn_slot_pick(DYN_SLOTS, cur, zhi, fence, 100, 0, LOCK_DISCARD);
    CHECK(k0 == 0, "после tail >= fence[0] переиспользуется слот 0");
    cur = k0;
    fence[0] = 500;
    zhi = 512;

    // Монотонный NOOVERWRITE (off >= zhi) -> остаётся в слоте 0 без копирования
    CHECK(d3d8_dyn_slot_pick(DYN_SLOTS, cur, zhi, fence, 100, 512, LOCK_NOOVERWRITE) == 0,
          "монотонный NOOVERWRITE -> zero-copy в том же слоте");

    // Немонотонный NOOVERWRITE (off < zhi) при ещё не пройденном fence -> fallback (-1)
    CHECK(d3d8_dyn_slot_pick(DYN_SLOTS, cur, zhi, fence, 100, 256, LOCK_NOOVERWRITE) == -1,
          "перекрывающий NOOVERWRITE при активном fence -> fallback -1");

    // После прохождения fence[0] = 500 -> безопасно писать с любого смещения
    CHECK(d3d8_dyn_slot_pick(DYN_SLOTS, cur, zhi, fence, 500, 256, LOCK_NOOVERWRITE) == 0,
          "NOOVERWRITE после прохождения fence -> zero-copy");
}

// ---------------------------------------------------------------- 5. Прямой C-интерфейс d3d8_direct.h
static void test_d3d8_direct(void)
{
    CHECK(cs_start(0) == 0, "direct: cs_start(0) успешен");

    guest_fn vt[3] = {NULL, com_AddRef, com_Release};
    u32 vtbl = com_vtable("FakeDev", vt, 3);
    d3ddev *d = calloc(1, sizeof *d);
    d->obj = com_create_st(vtbl, d, NULL);
    CHECK(d3d8_dev_from_guest(0) == NULL, "d3d8_dev_from_guest(0) == NULL");
    CHECK(d3d8_dev_from_guest(d->obj) == d, "d3d8_dev_from_guest возвращает d3ddev");

    // 1) Сеттеры состояния -> обновление d (Core 1) + доставка на Core 2 (d->r) + дедупликация
    d3d8_direct_set_rs(d, 27, 1);
    d3d8_direct_set_tss(d, 0, 1, 4);
    float mat[16] = {1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 3, 0, 4, 5, 6, 1};
    d3d8_direct_set_transform(d, 256, mat);
    float m68[17];
    for (int i = 0; i < 17; i++)
        m68[i] = (float)(i + 1) * 0.25f;
    d3d8_direct_set_material(d, m68);
    d3dlight lt = {0};
    lt.type = 3;
    lt.diffuse[0] = 0.5f;
    lt.dir[2] = 1.0f;
    d3d8_direct_set_light(d, 1, &lt);
    d3d8_direct_light_enable(d, 1, 1);
    d3d8_direct_set_vs(d, 0x102u);
    d3d8_direct_set_ps(d, 7u);
    float vsc[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    d3d8_direct_set_vs_const(d, 4, vsc, 2);
    float psc[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    d3d8_direct_set_ps_const(d, 2, psc, 2);
    struct { u32 x, y, w, h; float minz, maxz; } vp = {10, 20, 1280, 720, 0.0f, 1.0f};
    d3d8_direct_set_viewport(d, &vp);
    float clip[4] = {0.0f, 1.0f, 0.0f, -5.0f};
    d3d8_direct_set_clip_plane(d, 2, clip);
    d3d8_direct_set_texture(d, 0, 0);
    d3d8_direct_set_stream(d, 0, 0, 0);
    d3d8_direct_set_indices(d, 0, 0);

    cs_finish();
    CHECK(d->rs[27] == 1 && d->r.rs[27] == 1, "direct_set_rs дошёл до Core 2");
    CHECK(d->tss[0][1] == 4 && d->r.tss[0][1] == 4, "direct_set_tss дошёл до Core 2");
    CHECK(!memcmp(d->r.world, mat, 64), "direct_set_transform(WORLD) дошёл до Core 2");
    CHECK(!memcmp(d->r.material, m68, 68), "direct_set_material дошёл до Core 2");
    CHECK(d->r.lights[1].type == 3 && d->r.light_on[1] == 1, "direct_set_light + light_enable дошли до Core 2");
    CHECK(d->r.vs == 0x102u && d->r.ps == 7u, "direct_set_vs / direct_set_ps дошли до Core 2");
    CHECK(!memcmp(d->vsc[4], vsc, 32), "direct_set_vs_const записал d->vsc");
    CHECK(!memcmp(d->r.psc[2], psc, 32), "direct_set_ps_const дошёл до Core 2");
    CHECK(d->r.vp[2] == 1280 && d->r.vp[3] == 720 && d->r.vpz[1] == 1.0f, "direct_set_viewport дошёл до Core 2");
    CHECK(!memcmp(d->r.clip[2], clip, 16), "direct_set_clip_plane дошёл до Core 2");

    // 2) Повторные те же вызовы полностью отбрасываются (wr_pos не растёт)
    uint64_t wr0 = cs_wr_pos();
    d3d8_direct_set_rs(d, 27, 1);
    d3d8_direct_set_tss(d, 0, 1, 4);
    d3d8_direct_set_transform(d, 256, mat);
    d3d8_direct_set_material(d, m68);
    d3d8_direct_set_light(d, 1, &lt);
    d3d8_direct_light_enable(d, 1, 1);
    d3d8_direct_set_vs(d, 0x102u);
    d3d8_direct_set_ps(d, 7u);
    d3d8_direct_set_ps_const(d, 2, psc, 2);
    d3d8_direct_set_viewport(d, &vp);
    d3d8_direct_set_clip_plane(d, 2, clip);
    d3d8_direct_set_texture(d, 0, 0);
    d3d8_direct_set_stream(d, 0, 0, 0);
    d3d8_direct_set_indices(d, 0, 0);
    CHECK(cs_wr_pos() == wr0, "дубликаты d3d8_direct_set_* не шлют команды в gl_cs");

    // 3) Предохранители d3d8_direct_shadow_blur без GL-контекста (неверные аргументы -> 0)
    CHECK(d3d8_direct_shadow_blur(NULL, 0, 0, 1.5f) == 0, "shadow_blur(NULL) -> 0");
    CHECK(d3d8_direct_shadow_blur(d, 0, 0, 1.5f) == 0, "shadow_blur(src=0) -> 0");
    CHECK(d3d8_direct_shadow_blur(d, d->obj, 0, 1.5f) == 0, "shadow_blur(non-tex) -> 0");
    CHECK(d3d8_direct_shadow_blur(d, d->obj, 0, -1.0f) == 0, "shadow_blur(step<0) -> 0");
    CHECK(d->draws == 0, "отклонённый shadow_blur не увеличивает d->draws");

    cs_stop();
    com_release(d->obj);
    free(d);
}

int main(void)
{
    if (rt_mem_init() != 0) {
        fprintf(stderr, "rt_mem_init failed\n");
        return 2;
    }
    vm_init();
    heap_init();
    test_com_st();
    test_cs_ring();
    test_rstate_dirty();
    test_dyn_slots();
    test_d3d8_direct();
    printf("d3d8_cs_test: %s (%d failed)\n", g_fails ? "FAIL" : "OK", g_fails);
    return g_fails != 0;
}
