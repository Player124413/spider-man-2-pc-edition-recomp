// HLE vorbisfile.dll (музыка Music/*.ogg): Core открывает поток ov_open(FILE*, vf) и читает ov_read/ov_time_seek,
// ALAudio спрашивает ov_info/ov_pcm_total/ov_time_total. Декодирует libvorbisfile хоста; гостевой
// OggVorbis_File — только ключ (его поля гость не читает), FILE* гостя читается его же fread/fseek/ftell/fclose
// из msvcr70 (ov_clear закрывает файл, как в оригинале). Соглашение — cdecl.
#include <stdlib.h>
#include <string.h>

#include <vorbis/vorbisfile.h>

#include "runtime/heap.h"
#include "runtime/hle/hle.h"

#define MAX_OV 32
#define SCRATCH 0x10000u

typedef struct {
    u32 vf, file, info, scratch;  // гостевые: OggVorbis_File, FILE*, vorbis_info, буфер для fread
    OggVorbis_File ov;
} stream;

static stream *g_ov[MAX_OV];
static _Thread_local CPU *t_cpu;  // CPU текущего вызова ov_* — для обратных вызовов чтения
static u32 g_fread, g_fseek, g_ftell, g_fclose;

static stream *find(u32 vf)
{
    for (int i = 0; i < MAX_OV; i++)
        if (g_ov[i] && g_ov[i]->vf == vf)
            return g_ov[i];
    return NULL;
}

static size_t cb_read(void *ptr, size_t size, size_t nmemb, void *ds)
{
    stream *s = ds;
    size_t total = size * nmemb, done = 0;
    while (done < total) {
        u32 n = total - done < SCRATCH ? (u32)(total - done) : SCRATCH;
        u32 args[4] = {s->scratch, 1, n, s->file};
        u32 got = rt_guest_call(t_cpu, g_fread, 4, args);
        if (got > n)
            got = 0;
        memcpy((u8 *)ptr + done, g_mem + s->scratch, got);
        done += got;
        if (got < n)
            break;
    }
    return size ? done / size : 0;
}

static int cb_seek(void *ds, ogg_int64_t off, int whence)
{
    stream *s = ds;
    u32 args[3] = {s->file, (u32)(s32)off, (u32)whence};
    return (s32)rt_guest_call(t_cpu, g_fseek, 3, args) ? -1 : 0;
}

static long cb_tell(void *ds)
{
    stream *s = ds;
    return (s32)rt_guest_call(t_cpu, g_ftell, 1, &s->file);
}

static int cb_close(void *ds)
{
    stream *s = ds;
    return (s32)rt_guest_call(t_cpu, g_fclose, 1, &s->file);
}

static void release(stream *s)
{
    for (int i = 0; i < MAX_OV; i++)
        if (g_ov[i] == s)
            g_ov[i] = NULL;
    heap_free(heap_process(), s->scratch);
    if (s->info)
        heap_free(heap_process(), s->info);
    free(s);
}

// ov_open(FILE *f, OggVorbis_File *vf, char *initial, long ibytes)
void hle_vorbisfile_ov_open(CPU *c)
{
    HLE_ENTER("ov_open", 4);
    if (!g_fread) {
        rt_module *crt = rt_module_by_name("msvcr71.dll");
        g_fread = rt_module_export(crt, "fread", 0);
        g_fseek = rt_module_export(crt, "fseek", 0);
        g_ftell = rt_module_export(crt, "ftell", 0);
        g_fclose = rt_module_export(crt, "fclose", 0);
    }
    stream *old = find(ARG(1));
    if (old) {  // гость не вызвал ov_clear — хостовое состояние просто забываем
        ov_clear(&old->ov);
        release(old);
    }
    int slot = -1;
    for (int i = 0; i < MAX_OV && slot < 0; i++)
        if (!g_ov[i])
            slot = i;
    if (slot < 0)
        RET(0, (u32)OV_EFAULT);
    stream *s = calloc(1, sizeof *s);
    s->vf = ARG(1);
    s->file = ARG(0);
    s->scratch = heap_alloc(heap_process(), 0, SCRATCH);
    t_cpu = c;
    ov_callbacks cb = {cb_read, cb_seek, cb_close, cb_tell};
    int r = ov_open_callbacks(s, &s->ov, ARG(2) ? (const char *)(g_mem + ARG(2)) : NULL, (long)(s32)ARG(3), cb);
    if (r < 0) {  // как в libvorbisfile: при ошибке файл не закрывается
        fprintf(stderr, "[vorbis] ov_open failed: %d\n", r);
        release(s);
        RET(0, (u32)r);
    }
    g_ov[slot] = s;
    if (getenv("SNX_LOG_AUDIO")) {
        vorbis_info *vi = ov_info(&s->ov, -1);
        fprintf(stderr, "[vorbis] ov_open: %d Hz x%d, %.1f s\n", (int)vi->rate, vi->channels, ov_time_total(&s->ov, -1));
    }
    RET(0, 0);
}

void hle_vorbisfile_ov_clear(CPU *c)
{
    HLE_ENTER("ov_clear", 1);
    stream *s = find(ARG(0));
    if (s) {
        t_cpu = c;
        ov_clear(&s->ov);
        release(s);
    }
    RET(0, 0);
}

// ov_read(vf, buffer, length, bigendianp, word, sgned, int *bitstream)
void hle_vorbisfile_ov_read(CPU *c)
{
    HLE_ENTER("ov_read", 7);
    stream *s = find(ARG(0));
    if (!s)
        RET(0, (u32)OV_EINVAL);
    t_cpu = c;
    int bs = 0;
    long r = ov_read(&s->ov, (char *)(g_mem + ARG(1)), (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5), &bs);
    if (ARG(6))
        wr32(g_mem, ARG(6), (u32)bs);
    RET(0, (u32)r);
}

// ov_time_seek(vf, double s)
void hle_vorbisfile_ov_time_seek(CPU *c)
{
    HLE_ENTER("ov_time_seek", 3);
    stream *s = find(ARG(0));
    if (!s)
        RET(0, (u32)OV_EINVAL);
    u64 bits = (u64)ARG(1) | (u64)ARG(2) << 32;
    double t;
    memcpy(&t, &bits, 8);
    t_cpu = c;
    RET(0, (u32)ov_time_seek(&s->ov, t));
}

// ov_info(vf, link) -> vorbis_info* (копия в гостевой памяти: version, channels, rate, bitrate_*, codec_setup)
void hle_vorbisfile_ov_info(CPU *c)
{
    HLE_ENTER("ov_info", 2);
    stream *s = find(ARG(0));
    vorbis_info *vi = s ? ov_info(&s->ov, (int)ARG(1)) : NULL;
    if (!vi)
        RET(0, 0);
    if (!s->info)
        s->info = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 32);
    u32 v[7] = {(u32)vi->version, (u32)vi->channels, (u32)vi->rate, (u32)vi->bitrate_upper,
                (u32)vi->bitrate_nominal, (u32)vi->bitrate_lower, (u32)vi->bitrate_window};
    for (int i = 0; i < 7; i++)
        wr32(g_mem, s->info + 4u * (u32)i, v[i]);
    RET(0, s->info);
}

// ov_pcm_total(vf, i) -> ogg_int64_t в edx:eax
void hle_vorbisfile_ov_pcm_total(CPU *c)
{
    HLE_ENTER("ov_pcm_total", 2);
    stream *s = find(ARG(0));
    s64 n = s ? ov_pcm_total(&s->ov, (int)ARG(1)) : OV_EINVAL;
    c->edx = (u32)((u64)n >> 32);
    RET(0, (u32)n);
}

// ov_time_total(vf, i) -> double в ST(0) (gen.HLE_FLOAT_RET)
void hle_vorbisfile_ov_time_total(CPU *c)
{
    HLE_ENTER("ov_time_total", 2);
    stream *s = find(ARG(0));
    c->st_ret = s ? ov_time_total(&s->ov, (int)ARG(1)) : OV_EINVAL;
    RET(0, 0);
}
