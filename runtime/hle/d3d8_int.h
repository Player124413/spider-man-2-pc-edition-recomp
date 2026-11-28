// HLE Direct3D 8 поверх OpenGL 4.1 core: общее для d3d8*.c (форматы, ресурсы, состояние устройства).
// Соглашения отрисовки: все изображения GL хранятся «сверху вниз», как в D3D (строка 0 — верх): текстуры
// загружаются как есть, рендер идёт в FBO с перевёрнутой осью y клипа, и только Present переворачивает кадр
// при копировании в окно. Поэтому координаты окна GL численно равны экранным D3D (+0,5 пикселя: центры
// пикселей D3D целые), а D3D-«по часовой» — это GL-«против часовой».
#pragma once
#include "platform/gl.h"
#include "runtime/hle/com.h"
#include "runtime/hle/gl_cs.h"  // вызовы GL — через поток рендера (если включён)

#ifdef __cplusplus
extern "C" {
#endif

#define D3D_OK 0u
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017Cu

enum {
    FMT_UNKNOWN = 0, FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24,
    FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26, FMT_A8 = 28, FMT_X4R4G4B4 = 30, FMT_P8 = 41, FMT_L8 = 50, FMT_A8L8 = 51,
    FMT_V8U8 = 60, FMT_D16_LOCKABLE = 70, FMT_D32 = 71, FMT_D15S1 = 73, FMT_D24S8 = 75, FMT_D24X8 = 77,
    FMT_D24X4S4 = 79, FMT_D16 = 80, FMT_VERTEXDATA = 100, FMT_INDEX16 = 101, FMT_INDEX32 = 102,
};
#define FOURCC(a, b, c, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c) << 16 | (u32)(d) << 24)
#define FMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define FMT_DXT2 FOURCC('D', 'X', 'T', '2')
#define FMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define FMT_DXT4 FOURCC('D', 'X', 'T', '4')
#define FMT_DXT5 FOURCC('D', 'X', 'T', '5')

#define USAGE_RENDERTARGET 1u
#define USAGE_DEPTHSTENCIL 2u
#define USAGE_WRITEONLY 8u
#define USAGE_DYNAMIC 0x200u
enum { POOL_DEFAULT = 0, POOL_MANAGED = 1, POOL_SYSTEMMEM = 2, POOL_SCRATCH = 3 };
enum { RTYPE_SURFACE = 1, RTYPE_VOLUME, RTYPE_TEXTURE, RTYPE_VOLUMETEXTURE, RTYPE_CUBETEXTURE, RTYPE_VERTEXBUFFER,
       RTYPE_INDEXBUFFER };
#define LOCK_READONLY 0x10u
#define LOCK_NOOVERWRITE 0x1000u
#define LOCK_DISCARD 0x2000u

// ---------------------------------------------------------------- форматы
typedef struct {
    u32 fmt;
    GLenum internal, format, type;  // format == 0 — сжатый (internal — формат S3TC)
    u8 bpp;                         // байт на пиксель; у сжатых — байт на блок 4x4
    u8 depth, stencil;              // формат глубины/трафарета
    GLint swizzle[4];               // GL_RED.. / GL_ZERO / GL_ONE (0 — без перестановки)
} d3dfmt;
const d3dfmt *d3d8_format(u32 fmt);      // NULL — формат не поддержан
u32 d3d8_pitch(const d3dfmt *f, u32 w);  // байт в строке (строке блоков у сжатых)
u32 d3d8_rows(const d3dfmt *f, u32 h);   // строк (строк блоков)

// ---------------------------------------------------------------- ресурсы
typedef struct d3dtex d3dtex;
typedef struct d3dsurf {
    u32 obj;               // гостевой COM-объект
    d3dtex *tex;           // контейнер (уровень текстуры) или NULL
    int face, level;
    u32 w, h, fmt, usage, pool;
    GLuint rb;             // свой renderbuffer (глубина/трафарет вне текстур)
    GLuint color_tex;      // своя 2D-текстура для цветовых целей рендера и заднего буфера (чтение в пост-проходе Present)
    u32 mem, pitch;        // гостевая память блокировки (у SYSTEMMEM-поверхностей — постоянная)
    u32 rb_mem;            // память блокировки цели рендера: заводится один раз (ReduceMouseLag — каждый кадр)
    int locked, own_mem;
    u32 lock_flags, lock_rect[4];
    u32 movie[4], has_movie; // кадр ролика в залоченной цели (x, y, w, h): растянуть при Unlock
    int movie_mode;        // прошлый Unlock был кадром ролика: Lock не читает цель из GPU
    GLuint movie_tex;      // постоянная текстура кадра ролика
    u32 movie_tw, movie_th;
    int backbuffer;        // неявный задний буфер устройства
} d3dsurf;

struct d3dtex {
    u32 obj;
    int cube;              // 0 — 2D, 1 — кубическая
    u32 w, h, levels, fmt, usage, pool;
    GLuint gl;
    d3dsurf **surf;        // [грань * levels + уровень]
    u32 lod;
    u32 filled;            // битовая маска загруженных уровней грани 0 (для дампа/отладки)
};

#define DYN_SLOTS 4  // кольцевые слоты гостевой памяти для zero-copy динамических VB/IB

typedef struct d3dbuf {
    u32 obj;
    int index;               // 0 — вершинный, 1 — индексный
    u32 size, usage, fvf, fmt, pool;
    u32 mem;                 // основная гостевая копия (static / fallback для копии в кольцо)
    u32 zmem[DYN_SLOTS];     // кольцевые слоты гостевой памяти (Core 2 читает напрямую без копии в g_ring)
    u64 zfence[DYN_SLOTS];   // позиция g_wr последней команды заливки из этого слота
    int nzslots;             // выделенных zero-copy слотов (0..DYN_SLOTS)
    int cur_zslot;           // активный zero-copy слот (-1 — используется b->mem)
    u32 cur_zhi;             // верхняя граница [0, cur_zhi), уже отправленная в gl_cs из cur_zslot
    int locked;              // сейчас заблокирован гостем
    int lock_zc;             // 1 — текущий Lock отдан в zmem[cur_zslot] (zero-copy)
    u32 lock_off, lock_size, lock_flags;
    GLuint gl;
} d3dbuf;

d3dtex *d3d8_tex_create(u32 w, u32 h, u32 levels, u32 usage, u32 fmt, u32 pool, int cube);
d3dsurf *d3d8_surf_create(u32 w, u32 h, u32 fmt, u32 usage, u32 pool);  // самостоятельная поверхность
d3dbuf *d3d8_buf_create(int index, u32 size, u32 usage, u32 fvf_or_fmt, u32 pool);
d3dtex *d3d8_tex_from(u32 obj);    // гостевой объект -> текстура (NULL — не текстура)
d3dsurf *d3d8_surf_from(u32 obj);
d3dbuf *d3d8_buf_from(u32 obj);
void d3d8_buf_sync(d3dbuf *b);     // сбросить незакрытый Lock (если вызван Draw без Unlock)
// Выбор слота zero-copy для LOCK_DISCARD/LOCK_NOOVERWRITE по хвосту очереди tail (-1 -> fallback в g_ring)
int d3d8_dyn_slot_pick(int nzslots, int cur_zslot, u32 cur_zhi, const u64 *zfence, uint64_t tail, u32 off, u32 flags);
void d3d8_res_init(u32 device_obj);  // vtables ресурсов; GetDevice возвращает device_obj
// Прикрепить поверхность к FBO (уровень текстуры, color_tex цели рендера или renderbuffer)
void d3d8_fbo_attach(GLenum fb_target, GLenum attachment, const d3dsurf *s);
// CopyRects, GetFrontBuffer и UpdateTexture (поверх структур d3dsurf/d3dtex)
u32 d3d8_copy_rects(d3dsurf *src, d3dsurf *dst, u32 src_rects_va, u32 count, u32 dst_pts_va);
u32 d3d8_copy_surface(d3dsurf *src, d3dsurf *dst);  // копировать перекрывающуюся область (0,0)-(min w, min h)
u32 d3d8_update_texture(d3dtex *src, d3dtex *dst);

// ---------------------------------------------------------------- устройство
#define MAX_STAGES 8
#define MAX_STREAMS 16
#define MAX_LIGHTS 8

// Биты устаревшего фиксированного состояния GL (применяются побитно в apply_state)
enum {
    GLD_VP = 1u << 0,        // glViewport, glDepthRange
    GLD_SCISSOR = 1u << 1,   // glDisable(GL_SCISSOR_TEST)
    GLD_DEPTH = 1u << 2,     // GL_DEPTH_TEST, glDepthFunc, glDepthMask
    GLD_BLEND = 1u << 3,     // GL_BLEND, glBlendFunc, glBlendEquation
    GLD_CULL = 1u << 4,      // glFrontFace, GL_CULL_FACE, glCullFace
    GLD_STENCIL = 1u << 5,   // GL_STENCIL_TEST, glStencilFunc/Op/Mask
    GLD_COLORMASK = 1u << 6, // glColorMask
    GLD_POLY = 1u << 7,      // glPolygonMode, GL_POLYGON_OFFSET_FILL/PolygonOffset
    GLD_CLIP = 1u << 8,      // GL_CLIP_DISTANCE0..5
    GLD_ALL = 0x1FFu,
};

typedef struct d3d_sb d3d_sb;

typedef struct {
    u32 type;
    float diffuse[4], specular[4], ambient[4], pos[3], dir[3];
    float range, falloff, att0, att1, att2, theta, phi;
} d3dlight;  // D3DLIGHT8, 104 байта

typedef struct { u8 stream, reg, type, offset; } velem;  // reg — D3DVSDE_*, type — D3DVSDT_*
typedef struct {
    int n, rhw;
    velem e[16];
} vdecl;

// Состояние устройства, принадлежащее ИСКЛЮЧИТЕЛЬНО потоку рендера (Core 2, или Core 1 при SNX_RENDER_THREAD=0).
// Поток гостя (Core 1) его не читает и не пишет — обновляется только через команды D3D8 в gl_cs.
typedef struct d3d_rstate {
    u32 rs[256];
    u32 tss[MAX_STAGES][32];
    float view[16], proj[16], world[16], texm[MAX_STAGES][16];
    float material[17];
    d3dlight lights[MAX_LIGHTS];
    u8 light_on[MAX_LIGHTS];
    u32 vp[4];
    float vpz[2];
    float clip[6][4];
    d3dtex *tex[MAX_STAGES];
    d3dbuf *stream[MAX_STREAMS];
    u32 stride[MAX_STREAMS];
    d3dbuf *indices;
    u32 base_vertex;
    u32 vs, ps;
    float psc[8][4];
    d3dsurf *rt, *ds;
    // гранулярные поколения групп состояния: программа догружает только изменившиеся униформы
    u32 gen_xf, gen_view, gen_texm, gen_vp, gen_clip, gen_light, gen_mat, gen_const, gen_psc, gen_bump;
    u32 gl_dirty;          // битовая маска GLD_* фиксированного состояния GL
    u32 sampler_dirty;
    int key_dirty, decl_dirty, decl_ok;
    vdecl cached_decl;
    // GL
    GLuint vao, up_vbo, up_ibo, fbo, blit_fbo;
    d3dsurf *fbo_rt, *fbo_ds;  // что сейчас прикреплено к fbo
    GLuint samplers[MAX_STAGES];
    int frame;
    u32 draws;
} d3d_rstate;

typedef struct d3ddev {
    u32 obj;
    // Теневое состояние потока гостя (Core 1): Get*, StateBlock, дедупликация перед отправкой в gl_cs
    u32 rs[256];
    u32 tss[MAX_STAGES][32];
    float view[16], proj[16], world[16], texm[MAX_STAGES][16];
    float material[17];
    d3dlight lights[MAX_LIGHTS];
    u8 light_on[MAX_LIGHTS], light_set[MAX_LIGHTS];
    u32 vp[4];             // X, Y, Width, Height
    float vpz[2];          // MinZ, MaxZ
    float clip[6][4];
    d3dtex *tex[MAX_STAGES];
    d3dbuf *stream[MAX_STREAMS];
    u32 stride[MAX_STREAMS];
    d3dbuf *indices;
    u32 base_vertex;
    u32 vs, ps;            // FVF или дескриптор шейдера; дескриптор пиксельного шейдера
    float vsc[96][4], psc[8][4];
    d3dsurf *rt, *ds;      // текущие цели
    d3dsurf *backbuffer, *autodepth;
    u32 bb_w, bb_h, bb_fmt, ds_fmt, windowed;
    u32 hwnd;
    // таблица гаммы (SetGammaRamp/GetGammaRamp: 3 канала x 256 значений 0..65535)
    u16 gamma[3][256];
    int gamma_dirty;
    // блоки состояний (CreateStateBlock / BeginStateBlock..EndStateBlock)
    d3d_sb **sb;
    u32 sb_cap;
    d3d_sb *rec_sb;
    int frame;
    u32 draws;  // за текущий кадр (для лога/статистики на Core 1)
    // Состояние потока рендера (Core 2)
    d3d_rstate r;
} d3ddev;

d3ddev *d3d8_dev(void);  // единственное устройство (или NULL)
void d3d8_set_d3d_object(u32 obj);                  // IDirect3D8 для GetDirect3D
u32 d3d8_device_create(CPU *c, u32 hwnd, u32 pp);  // гостевой объект или 0
void d3d8_decl_from_fvf(u32 fvf, vdecl *out);      // разбор FVF в vdecl (чистая функция, безопасна на Core 2)
#define D3D8_VS_HANDLE_BASE 0xF0000001u
#define D3D8_MAX_VS 512

// d3d8_ff.c: команды D3D8 (Core 1 -> gl_cs -> Core 2) и отрисовка
void d3d8_cmd_init_gl(d3ddev *d);
void d3d8_cmd_reset_defaults(d3ddev *d, int has_autodepth, u32 bb_w, u32 bb_h);
void d3d8_cmd_unbind_all(d3ddev *d);
void d3d8_cmd_rs(d3ddev *d, u32 state, u32 val);
void d3d8_cmd_tss(d3ddev *d, u32 stage, u32 type, u32 val);
void d3d8_cmd_xf(d3ddev *d, u32 state, const float mat[16]);
void d3d8_cmd_vp(d3ddev *d, const u32 vp[4], const float vpz[2]);
void d3d8_cmd_mat(d3ddev *d, const float mat[17]);
void d3d8_cmd_light(d3ddev *d, u32 idx, const d3dlight *l);
void d3d8_cmd_light_enable(d3ddev *d, u32 idx, u8 on);
void d3d8_cmd_clip(d3ddev *d, u32 idx, const float plane[4]);
void d3d8_cmd_tex(d3ddev *d, u32 stage, d3dtex *t);
void d3d8_cmd_stream(d3ddev *d, u32 n, d3dbuf *b, u32 stride);
void d3d8_cmd_indices(d3ddev *d, d3dbuf *b, u32 base_vertex);
void d3d8_cmd_vs(d3ddev *d, u32 vs);
void d3d8_cmd_vs_set(u32 idx, const vdecl *decl);
void d3d8_cmd_vs_del(u32 idx);
void d3d8_cmd_ps(d3ddev *d, u32 ps);
void d3d8_cmd_psc(d3ddev *d, u32 reg, u32 count, const float data[][4]);
void d3d8_cmd_set_rt(d3ddev *d, d3dsurf *rt, d3dsurf *ds, int reset_vp);
void d3d8_cmd_clear(d3ddev *d, u32 flags, u32 color, float z, u32 stencil, u32 count, const s32 *rects);
void d3d8_cmd_draw(d3ddev *d, u32 prim, u32 count, int indexed, u32 first, u32 min_index, u32 num_vertices);
void d3d8_cmd_draw_up(d3ddev *d, u32 prim, u32 count, int indexed, u32 min_index, u32 num_vertices,
                      const void *up_vertices, u32 up_stride, const void *up_indices, u32 up_index_fmt);
void d3d8_cmd_shadow_blur(d3ddev *d, d3dtex *src_tex, d3dsurf *src_surf, d3dsurf *dst_surf, float step_px);
void d3d8_bind_target(d3ddev *d);  // восстановить FBO текущих rt/ds на Core 2
extern u64 g_movie_drawn_ns;  // когда кадр ролика Bink последний раз залит в цель игрой (d3d8_res.c write_movie)
extern u64 g_movie_overlay_ns;  // когда порт последний раз сам нарисовал кадр ролика (d3d8_ff.c movie_overlay)
void d3d8_present(d3ddev *d);
void d3d8_prog_preload(void);  // программы прошлых запусков — в фоновую сборку на Core 2 (d3d8_ff.c)
int d3d8_dumping(const d3ddev *d);  // кадр SNX_DUMP_DRAWS: печатать состояние отрисовок

// Чистые функции обновления d3d_rstate (используются на Core 2 и в синтетическом тесте d3d8_cs_test)
void d3d8_r_apply_rs(d3d_rstate *r, u32 state, u32 val);
void d3d8_r_apply_tss(d3d_rstate *r, u32 stage, u32 type, u32 val);
void d3d8_r_apply_xf(d3d_rstate *r, u32 state, const float mat[16]);
void d3d8_r_apply_tex(d3d_rstate *r, u32 stage, d3dtex *t);

// d3d8_ps.c: пиксельные шейдеры ps.1.x
u32 d3d8_ps_create(const u32 *tokens, u32 max_tokens);  // дескриптор (>0) или 0
void d3d8_ps_delete(u32 handle);
const char *d3d8_ps_glsl(u32 handle);  // тело main() фрагментного шейдера (Core 1 / тесты)
u64 d3d8_ps_hash(u32 handle);          // 64-битный хеш байткода (0 — нет)
const u32 *d3d8_ps_tokens(u32 handle, u32 *out_ntok);  // исходный байткод и число токенов
// Доступ со стороны потока рендера (Core 2): таблица обновляется через gl_cs, освобождение отложено
const char *d3d8_r_ps_glsl(u32 handle);
u64 d3d8_r_ps_hash(u32 handle);
const u32 *d3d8_r_ps_tokens(u32 handle, u32 *out_ntok);
// Трансляция байткода ps.1.x в тело GLSL без создания дескриптора (для предзагрузки из SnxPrograms.bin)
char *d3d8_ps_translate(const u32 *tokens, u32 max_tokens, u32 *out_ntok, u64 *out_hash);

#ifdef __cplusplus
}
#endif
