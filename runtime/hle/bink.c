// HLE binkw32 (RAD Bink 1.x): ролики (Movies/*.bik) и звук в формате Bink внутри пакетов (диалоги — Engine
// перегоняет их в WAV через BinkGetTrackData). Контейнер разбирается полностью: кадры, частота, дорожки,
// размеры PCM каждого кадра (первый dword аудиопакета). Файловые ролики читаются потоково (в памяти только
// заголовок, таблица смещений и переиспользуемый буфер одного кадра); BINKFROMMEMORY держит копию буфера.
// Декодирование звука — bink_audio.c (если нет — тишина той же длины; у звуков в пакетах весь PCM — в кадре 0,
// у роликов — покадрово, декодированное совпадает с заявленным до последнего кадра, где хвост блока лишний).
// Видео — bink_video.c (libavcodec): BinkDoFrame декодирует текущий кадр (пропущенные — догоняются от ключевого),
// BinkCopyToBuffer переводит его в формат поверхности; время и номера кадров — как у SDK.
// Структуры SDK в гостевой памяти: BINK (Width, Height, Frames, FrameNum (с 1), LastFrameNum, FrameRate,
// FrameRateDiv, ...), BINKTRACK (Frequency, Bits, Channels, MaxSize, bink, ...), BINKSUMMARY.
#include <stdio.h>
#include <stdlib.h>

#include "runtime/heap.h"
#include "runtime/hle/bink.h"
#include "runtime/hle/d3d8.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/vfs.h"
#include "runtime/rt.h"

#define BINKFROMMEMORY 0x04000000u
#define MAX_BINK 64
#define MAX_BINK_MEM (256u << 20)
#define MAX_BINK_DIM 16384u

typedef struct {
    u32 obj;               // гостевой BINK
    u8 *data;              // копия данных (только для BINKFROMMEMORY)
    FILE *fp;              // открытый файл (потоковый режим, иначе NULL)
    u8 *frame_buf;         // переиспользуемый буфер одного кадра (растёт до max_frame_size)
    u32 frame_cap;         // выделенная ёмкость frame_buf
    u32 frame_loaded;      // индекс кадра в frame_buf (~0u — нет)
    u32 frame_len;         // длина кадра в frame_buf
    u32 size;              // полный размер файла/буфера
    u32 sig;               // первые 4 байта ("BIKi")
    u32 video_flags;       // dword заголовка по смещению 36
    u32 frames, w, h, fps, fpsdiv, ntracks;
    u32 rate[4], flags[4], maxsize[4];
    u32 audio_req[4];  // сколько раз игра забрала PCM дорожки (BinkGetTrackData): видно в журнале при закрытии
    u32 *offs;             // frames + 1 смещений
    u8 *key;               // ключевые кадры (бит 0 смещения)
    u32 frame;             // текущий, с 0
    u32 tracks[4];         // гостевые BINKTRACK
    bink_audio *dec[4];
    u32 dec_next[4];       // кадр, который декодер ждёт следующим (иначе — сброс хвоста перекрытия)
    bink_video *vid;
    u32 vid_frame;         // последний декодированный кадр видео (~0u — нет)
    int movie;             // ролик из файла (Movies/*.bik), не звук в пакете
    int menu_movie;        // заставка главного меню/логотипов, а не игровая сцена
    u32 do_frame;          // кадр последнего BinkDoFrame (~0u — не было)
    u64 tick_ns;           // последний вызов игры по этому ролику (кадр, звук, копия)
    u64 stats_ns, copy_ns; // статистика (movie_stats): начало отрезка, время перевода кадров
    u32 copy_n;
} bink;

static bink *g_bink[MAX_BINK];

// Звук роликов, которые порт рисует сам (реализация ниже, перед BinkDoFrame)
#ifdef SNX_HEADLESS
static void movie_audio_pump(bink *b) { (void)b; }
static void movie_audio_stop(bink *b) { (void)b; }
#else
static void movie_audio_pump(bink *b);
static void movie_audio_stop(bink *b);
#endif

static bink *by_obj(u32 obj)
{
    if (!obj)
        return NULL;
    for (int i = 0; i < MAX_BINK; i++)
        if (g_bink[i] && g_bink[i]->obj == obj)
            return g_bink[i];
    return NULL;
}

static bink *by_track(u32 tr, int *index)
{
    if (!tr)
        return NULL;
    for (int i = 0; i < MAX_BINK; i++)
        for (int t = 0; g_bink[i] && t < 4; t++)
            if (g_bink[i]->tracks[t] == tr) {
                *index = t;
                return g_bink[i];
            }
    return NULL;
}

static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static void sync_obj(bink *b)
{
    wr32(g_mem, b->obj + 12, b->frame + 1);  // FrameNum
}

static void bink_destroy(bink *b)
{
    if (!b)
        return;
    movie_audio_stop(b);  // ролик закрылся — очередь звука порта тоже
    for (int t = 0; t < 4; t++) {
        if (b->tracks[t])
            heap_free(heap_process(), b->tracks[t]);
        bink_audio_free(b->dec[t]);
    }
    if (b->obj)
        heap_free(heap_process(), b->obj);
    bink_video_free(b->vid);
    if (b->fp)
        fclose(b->fp);
    free(b->frame_buf);
    free(b->key);
    free(b->offs);
    free(b->data);
    free(b);
}

// Разбор заголовка (44 байта), метаданных дорожек и таблицы смещений кадров.
// Источник — либо буфер в памяти (d != NULL, fp == NULL), либо поток FILE* (fp != NULL, позиция 44).
// При ошибке закрывает fp и освобождает d, если bink не создан.
static bink *parse(const u8 hdr[44], u8 *d, FILE *fp, u32 size)
{
    if (size < 44 || memcmp(hdr, "BIK", 3) != 0)
        return NULL;
    u32 frames = le32(hdr + 8);
    u32 ntracks = le32(hdr + 40);
    if (ntracks > 4 || frames == 0)
        return NULL;
    u64 tracks_bytes = (u64)ntracks * 12ull;
    u64 offs_bytes = ((u64)frames + 1ull) * 4ull;
    u64 hdr_total = 44ull + tracks_bytes + offs_bytes;
    if (hdr_total > (u64)size)
        return NULL;

    bink *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->data = d;
    b->fp = fp;
    b->frame_loaded = ~0u;
    b->vid_frame = ~0u;
    b->size = size;
    b->sig = le32(hdr);
    b->frames = frames;
    b->w = le32(hdr + 20);
    b->h = le32(hdr + 24);
    b->fps = le32(hdr + 28);
    b->fpsdiv = le32(hdr + 32);
    b->video_flags = le32(hdr + 36);
    b->ntracks = ntracks;
    if (!b->fpsdiv)
        b->fpsdiv = 1;

    u8 tbuf[48];
    const u8 *tp = NULL;
    if (tracks_bytes > 0) {
        if (d) {
            tp = d + 44;
        } else {
            if (fread(tbuf, 1, (size_t)tracks_bytes, fp) != (size_t)tracks_bytes)
                goto fail;
            tp = tbuf;
        }
        u32 o = 0;
        for (u32 t = 0; t < ntracks; t++, o += 4)
            b->maxsize[t] = le32(tp + o);
        for (u32 t = 0; t < ntracks; t++, o += 4) {
            b->rate[t] = (u32)tp[o] | (u32)tp[o + 1] << 8;
            b->flags[t] = (u32)tp[o + 2] | (u32)tp[o + 3] << 8;
        }
    }

    b->offs = malloc((size_t)offs_bytes);
    b->key = malloc((size_t)frames + 1u);
    if (!b->offs || !b->key)
        goto fail;

    if (d) {
        const u8 *op = d + 44u + (u32)tracks_bytes;
        for (u32 i = 0; i <= frames; i++) {
            u32 raw = le32(op + (size_t)i * 4u);
            b->offs[i] = raw & ~1u;
            b->key[i] = (u8)(raw & 1u);
        }
    } else {
        // читаем таблицу смещений прямо в b->offs и переводим из LE на месте
        u8 *raw_buf = (u8 *)b->offs;
        if (fread(raw_buf, 1, (size_t)offs_bytes, fp) != (size_t)offs_bytes)
            goto fail;
        for (u32 i = 0; i <= frames; i++) {
            u32 raw = le32(raw_buf + (size_t)i * 4u);
            b->offs[i] = raw & ~1u;
            b->key[i] = (u8)(raw & 1u);
        }
    }
    b->key[0] = 1;

    // проверяем границы и монотонность смещений кадров
    if (b->offs[0] < (u32)hdr_total || b->offs[0] > size)
        goto fail;
    for (u32 i = 1; i <= frames; i++) {
        if (b->offs[i] < b->offs[i - 1] || b->offs[i] > size)
            goto fail;
    }
    return b;

fail:
    // d и fp освободит вызывающий при возврате NULL
    b->data = NULL;
    b->fp = NULL;
    bink_destroy(b);
    return NULL;
}

// Указатель на сырые байты кадра f и его длина (*out_len). В потоковом режиме читает кадр в frame_buf.
static const u8 *frame_data(bink *b, u32 f, u32 *out_len)
{
    if (!b || f >= b->frames)
        return NULL;
    u32 start = b->offs[f], end = b->offs[f + 1];
    if (start > end || end > b->size)
        return NULL;
    u32 len = end - start;
    if (b->data) {
        *out_len = len;
        return b->data + start;
    }
    if (!b->fp)
        return NULL;
    if (b->frame_loaded == f) {
        *out_len = b->frame_len;
        return b->frame_buf ? b->frame_buf : (const u8 *)"";
    }
    if (len > b->frame_cap) {
        u8 *nb = realloc(b->frame_buf, len);
        if (!nb)
            return NULL;
        b->frame_buf = nb;
        b->frame_cap = len;
    }
    if (len > 0) {
        if (fseek(b->fp, (long)start, SEEK_SET) != 0 || fread(b->frame_buf, 1, len, b->fp) != len) {
            b->frame_loaded = ~0u;
            b->frame_len = 0;
            return NULL;
        }
    }
    b->frame_loaded = f;
    b->frame_len = len;
    *out_len = len;
    return b->frame_buf ? b->frame_buf : (const u8 *)"";
}

// Аудиопакет дорожки t в кадре f: указатель и размер (NULL — нет)
static const u8 *audio_packet(bink *b, u32 f, u32 t, u32 *len)
{
    if (!b || t >= b->ntracks)
        return NULL;
    u32 flen;
    const u8 *fd = frame_data(b, f, &flen);
    if (!fd)
        return NULL;
    u32 off = 0;
    for (u32 i = 0; i < b->ntracks; i++) {
        if (flen - off < 4u)
            return NULL;
        u32 n = le32(fd + off);
        off += 4u;
        if (n > flen - off)
            return NULL;
        if (i == t) {
            *len = n;
            return fd + off;
        }
        off += n;
    }
    return NULL;
}

// Видеопакет кадра f: всё после аудиопакетов дорожек
static const u8 *video_packet(bink *b, u32 f, u32 *len)
{
    if (!b)
        return NULL;
    u32 flen;
    const u8 *fd = frame_data(b, f, &flen);
    if (!fd)
        return NULL;
    u32 off = 0;
    for (u32 i = 0; i < b->ntracks; i++) {
        if (flen - off < 4u)
            return NULL;
        u32 n = le32(fd + off);
        off += 4u;
        if (n > flen - off)
            return NULL;
        off += n;
    }
    *len = flen - off;
    return fd + off;
}

// Наибольший заявленный размер PCM дорожки t по всем кадрам (для потокового файла читает только заголовки аудио)
static u32 scan_track_maxsize(bink *b, u32 t)
{
    u32 mx = b->maxsize[t];
    if (b->data) {
        for (u32 f = 0; f < b->frames; f++) {
            u32 n;
            const u8 *p = audio_packet(b, f, t, &n);
            if (p && n >= 4 && le32(p) > mx)
                mx = le32(p);
        }
        return mx;
    }
    if (!b->fp)
        return mx;
    for (u32 f = 0; f < b->frames; f++) {
        u32 start = b->offs[f], end = b->offs[f + 1];
        if (start >= end || end > b->size)
            continue;
        u32 rem = end - start;
        if (fseek(b->fp, (long)start, SEEK_SET) != 0)
            break;
        for (u32 i = 0; i <= t; i++) {
            if (rem < 4u)
                break;
            u8 buf[8];
            if (i == t) {
                u32 want = rem >= 8u ? 8u : 4u;
                if (fread(buf, 1, want, b->fp) != want)
                    break;
                u32 n = le32(buf);
                rem -= 4u;
                if (n <= rem && n >= 4u && want == 8u) {
                    u32 pcm = le32(buf + 4);
                    if (pcm > mx)
                        mx = pcm;
                }
            } else {
                if (fread(buf, 1, 4, b->fp) != 4)
                    break;
                u32 n = le32(buf);
                rem -= 4u;
                if (n > rem)
                    break;
                rem -= n;
                if (n > 0 && fseek(b->fp, (long)n, SEEK_CUR) != 0)
                    break;
            }
        }
    }
    return mx;
}

// Раз в 2 с, пока ролик идёт: скорость декодирования и перевода кадра в snx.log
static void movie_stats(bink *b)
{
    u64 now = rt_ticks_ns();
    if (!b->movie || !b->vid)
        return;
    if (!b->stats_ns) {
        b->stats_ns = now;
        return;
    }
    if (now - b->stats_ns < 2000000000ull)
        return;
    double dec, wait;
    u32 n;
    bink_video_stats(b->vid, &dec, &wait, &n);
    fprintf(stderr, "[bink] movie %ux%u: %u frames in %.1f s, decode %.1f ms/frame (waited %.1f), copy %.1f ms\n",
            b->w, b->h, n, (double)(now - b->stats_ns) / 1e9, dec, wait,
            b->copy_n ? (double)b->copy_ns / 1e6 / (double)b->copy_n : 0.0);
    b->stats_ns = now;
    b->copy_ns = 0;
    b->copy_n = 0;
}

// Декодировать кадр f; если он не следующий — с ближайшего ключевого (или с последнего декодированного)
static void video_decode_to(bink *b, u32 f)
{
    if (!b->vid || f >= b->frames || f == b->vid_frame)
        return;
    u32 start = f;
    while (start > 0 && !b->key[start] && start != b->vid_frame + 1)
        start--;
    if (start != b->vid_frame + 1)
        bink_video_flush(b->vid);
    for (u32 i = start; i <= f; i++) {
        u32 n;
        const u8 *p = video_packet(b, i, &n);
        if (p && n)
            bink_video_decode(b->vid, p, n, b->key[i]);
    }
    b->vid_frame = f;
    if (f + 1 < b->frames) {  // следующий кадр — в фоне, пока игра выводит этот
        u32 n;
        const u8 *p = video_packet(b, f + 1, &n);
        if (p && n)
            bink_video_prefetch(b->vid, p, n, b->key[f + 1]);
    }
    movie_stats(b);
}

// Логотипы/главное меню: эти ролики не игровые сцены, пока они идут экранное управление можно спрятать
// и передавать касания в меню. Обычные внутриигровые ролики так помечать нельзя — во время них кнопки нужны.
static int is_menu_movie(const char *name)
{
    const char *base = name ? strrchr(name, '\\') : NULL;
    if (!base) base = name ? strrchr(name, '/') : NULL;
    base = base ? base + 1 : name;
    if (!base) return 0;
    return !strcasecmp(base, "Marvel.bik") || !strcasecmp(base, "ACTIVISN.bik") ||
           !strcasecmp(base, "ESRB.bik");
}

// BinkOpen(name, flags): имя файла гостя или (BINKFROMMEMORY) указатель на данные
void hle_binkw32__BinkOpen_8(CPU *c)
{
    HLE_ENTER("BinkOpen", 2);
    u32 src = ARG(0), flags = ARG(1);
    u8 *d = NULL;
    FILE *fp = NULL;
    u32 size = 0;
    u8 hdr[44];
    int menu_movie = 0;
    if (flags & BINKFROMMEMORY) {
        if (src < 0x10000u || src > RT_WINDOW - 44u)
            RET(2, 0);
        if (memcmp(g_mem + src, "BIK", 3) != 0)
            RET(2, 0);
        u32 body = le32(g_mem + src + 4);
        if (body < 44u - 8u || body > MAX_BINK_MEM - 8u)
            RET(2, 0);
        size = body + 8u;
        if ((u64)src + size > RT_WINDOW)
            RET(2, 0);
        d = malloc(size);
        if (!d)
            RET(2, 0);
        memcpy(d, g_mem + src, size);
        memcpy(hdr, d, 44);
    } else {
        char rel[512], host[1024];
        const char *name = hle_cstr(src);
        if (!name[0] || !vfs_rel(name, rel, sizeof rel) || !vfs_find(rel, host, sizeof host)) {
            fprintf(stderr, "[bink] BinkOpen(%s): not found\n", name);
            RET(2, 0);
        }
        fp = fopen(host, "rb");
        if (!fp)
            RET(2, 0);
        if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            RET(2, 0);
        }
        long fsz = ftell(fp);
        if (fsz < 44 || (unsigned long)fsz > 0xFFFFFFFFul || fseek(fp, 0, SEEK_SET) != 0) {
            fclose(fp);
            RET(2, 0);
        }
        size = (u32)fsz;
        if (fread(hdr, 1, 44, fp) != 44) {
            fclose(fp);
            RET(2, 0);
        }
        fprintf(stderr, "[bink] BinkOpen(%s) %u bytes (stream)\n", name, size);
        menu_movie = is_menu_movie(name);
    }
    int from_file = !(flags & BINKFROMMEMORY);
    int slot = -1;
    for (int i = 0; i < MAX_BINK && slot < 0; i++)
        if (!g_bink[i])
            slot = i;
    bink *b = slot >= 0 ? parse(hdr, d, fp, size) : NULL;
    if (!b) {
        if (fp)
            fclose(fp);
        free(d);
        RET(2, 0);
    }
    b->obj = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 0x200);
    if (!b->obj) {
        bink_destroy(b);
        RET(2, 0);
    }
    u32 o = b->obj;
    wr32(g_mem, o + 0, b->w);
    wr32(g_mem, o + 4, b->h);
    wr32(g_mem, o + 8, b->frames);
    wr32(g_mem, o + 16, 0);  // LastFrameNum
    wr32(g_mem, o + 20, b->fps);
    wr32(g_mem, o + 24, b->fpsdiv);
    wr32(g_mem, o + 32, flags);
    wr32(g_mem, o + 40, size);
    g_bink[slot] = b;
    b->do_frame = ~0u;
    b->movie = from_file && b->w && b->h;
    b->menu_movie = b->movie && menu_movie;
    b->tick_ns = rt_ticks_ns();
    if (b->movie) {
        fprintf(stderr, "[bink] movie %ux%u, %u frames, audio tracks: %u\n", b->w, b->h, b->frames, b->ntracks);
        for (u32 t = 0; t < b->ntracks; t++)
            fprintf(stderr, "[bink]   track %u: %u Hz, %s, %s, max %u bytes/packet\n", t, b->rate[t],
                    (b->flags[t] & 0x2000) ? "stereo" : "mono", (b->flags[t] & 0x4000) ? "16 bit" : "8 bit",
                    b->maxsize[t]);
    }
    if (b->w > 0 && b->h > 0 && b->w <= MAX_BINK_DIM && b->h <= MAX_BINK_DIM) {
        b->vid = bink_video_new(b->sig, b->video_flags, b->w, b->h);
        if (!b->vid)
            fprintf(stderr, "[bink] no video decoder: frames will be black\n");
    }
    sync_obj(b);
    RET(2, b->obj);
}

void hle_binkw32__BinkClose_4(CPU *c)
{
    HLE_ENTER("BinkClose", 1);
    u32 obj = ARG(0);
    for (int i = 0; i < MAX_BINK; i++) {
        bink *b = g_bink[i];
        if (b && b->obj == obj) {
            if (b->movie) {  // видно, брала ли игра звук ролика: 0 запросов — игра его не проигрывает
                u32 req = 0;
                for (u32 t = 0; t < b->ntracks; t++)
                    req += b->audio_req[t];
                fprintf(stderr, "[bink] movie closed: %u audio frame(s) taken by the game\n", req);
            }
            g_bink[i] = NULL;
            bink_destroy(b);
            break;
        }
    }
    RET(1, 0);
}

// BinkOpenTrack(bink, trackindex) -> BINKTRACK: Frequency, Bits, Channels, MaxSize, bink, sndcomp, trackindex
void hle_binkw32__BinkOpenTrack_8(CPU *c)
{
    HLE_ENTER("BinkOpenTrack", 2);
    bink *b = by_obj(ARG(0));
    u32 t = ARG(1);
    if (!b || t >= b->ntracks)
        RET(2, 0);
    if (!b->tracks[t]) {
        u32 tr = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 32);
        if (!tr)
            RET(2, 0);
        u32 ch = (b->flags[t] & 0x2000) ? 2 : 1, bits = (b->flags[t] & 0x4000) ? 16 : 8;
        wr32(g_mem, tr, b->rate[t]);
        wr32(g_mem, tr + 4, bits);
        wr32(g_mem, tr + 8, ch);
        // MaxSize: наибольший PCM кадра (заголовок даёт его не всегда точно — проверим все кадры)
        u32 mx = scan_track_maxsize(b, t);
        wr32(g_mem, tr + 12, mx);
        wr32(g_mem, tr + 16, b->obj);
        wr32(g_mem, tr + 24, t);
        b->tracks[t] = tr;
        b->dec[t] = bink_audio_new(b->rate[t], ch, (b->flags[t] & 0x1000) != 0, (int)(b->sig >> 24));
        fprintf(stderr, "[bink] open track %u: %u Hz x%u x%u bit, max %s%u bytes, decoder %s\n", t, b->rate[t], ch,
                bits, "", mx, b->dec[t] ? "ok" : "NO (тишина)");
        b->audio_req[t] = 0;
    }
    RET(2, b->tracks[t]);
}

void hle_binkw32__BinkCloseTrack_4(CPU *c)
{
    HLE_ENTER("BinkCloseTrack", 1);
    RET(1, 0);  // освобождается вместе с BINK
}

// BinkGetTrackData(track, dest): PCM текущего кадра; байт записано
void hle_binkw32__BinkGetTrackData_8(CPU *c)
{
    HLE_ENTER("BinkGetTrackData", 2);
    int t;
    u32 tr = ARG(0), dst = ARG(1);
    bink *b = by_track(tr, &t);
    u32 n;
    const u8 *p = b ? audio_packet(b, b->frame, (u32)t, &n) : NULL;
    if (!p || n < 4)
        RET(2, 0);
    u32 bytes = le32(p);
    u32 cap = rd32(g_mem, tr + 12);
    if (bytes > cap)
        bytes = cap;
    if (bytes == 0 || dst < 0x10000u || dst >= RT_WINDOW || (u64)dst + bytes > RT_WINDOW)
        RET(2, 0);
    b->tick_ns = rt_ticks_ns();
    if (b->frame != b->dec_next[t])
        bink_audio_reset(b->dec[t]);
    b->dec_next[t] = b->frame + 1;
    u32 got = 0, bits8 = rd32(g_mem, tr + 4) == 8;
    if (bits8) {  // 8 бит без знака
        s16 *tmp = malloc((size_t)bytes * 2u + 2u);
        if (!tmp)
            RET(2, 0);
        got = bink_audio_decode(b->dec[t], p + 4, n - 4, tmp, bytes);
        for (u32 i = 0; i < got; i++)
            g_mem[dst + i] = (u8)((tmp[i] >> 8) + 128);
        free(tmp);
    } else if (dst & 1u) {
        // невыровненный адрес гостя: декодируем во временный буфер s16 без UB
        u32 samples = bytes / 2u;
        s16 *tmp = samples ? malloc((size_t)samples * 2u) : NULL;
        if (samples && !tmp)
            RET(2, 0);
        if (tmp) {
            got = bink_audio_decode(b->dec[t], p + 4, n - 4, tmp, samples) * 2u;
            if (got)
                memcpy(g_mem + dst, tmp, got);
            free(tmp);
        }
    } else {
        got = bink_audio_decode(b->dec[t], p + 4, n - 4, (s16 *)(g_mem + dst), bytes / 2u) * 2u;
    }
    if (got < bytes)
        memset(g_mem + dst + got, bits8 ? 0x80 : 0, bytes - got);
    if (b->audio_req[t]++ < 2 || b->audio_req[t] % 300 == 0)
        fprintf(stderr, "[bink] track %u frame %u: game took %u bytes PCM, decoded %u, requests %u\n", t, b->frame,
                bytes, got, b->audio_req[t]);
    RET(2, bytes);
}

// ── Звук ролика, который порт рисует сам ─────────────────────────────────────────────────────────────
// На ПК звук ролика играет сам Bink через DirectAudio, а игра (Engine) PCM роликов не забирает вовсе — в
// журнале у таких роликов «movie closed: 0 audio frame(s) taken by the game». DirectSound в порте нет, поэтому
// дорожку ролика декодирует сам порт (bink_audio.c) и отдаёт в очередь SDL: OpenAL занят звуком игры, а SDL
// уже подключён хостом. Звук идёт за кадрами, которые игра уже прошла (BinkNextFrame), а не своим таймером:
// на паузе, при медленном кадре и при перемотке он не разъезжается с картинкой.
#ifndef SNX_HEADLESS
#include <SDL.h>

static SDL_AudioDeviceID g_mov_dev;
static SDL_AudioStream *g_mov_stream;
static bink_audio *g_mov_dec;
static u32 g_mov_owner, g_mov_track, g_mov_next, g_mov_out_rate, g_mov_need;
static int g_mov_failed;
static int g_mov_started;  // в очередь этого ролика уже что-то положено
static u32 g_mov_dry;      // сколько раз очередь успевала опустеть (треск/провалы) — видно в журнале
// Насколько кадров звук может уйти вперёд картинки. Это запас против «медленных» кадров ролика: раньше звук
// брался только за уже пройденные кадры, и при кадре дольше буфера очередь пустела — слышно треском.
#define MOV_LOOKAHEAD 12u
static float g_mov_gain = 1.0f;
static s16 g_mov_pcm[0x12000];  // 147456 байт: больше самого большого аудиопакета Bink (145920)

static void movie_audio_stop(bink *b);

static void movie_audio_reset(void)
{
    if (g_mov_stream)
        SDL_FreeAudioStream(g_mov_stream);
    g_mov_stream = NULL;
    bink_audio_free(g_mov_dec);
    g_mov_dec = NULL;
    g_mov_owner = 0;
    g_mov_started = 0;
    if (g_mov_dev)
        SDL_ClearQueuedAudio(g_mov_dev);
}

static int movie_audio_wanted(void)
{
    const char *e = getenv("SNX_MOVIE_SOUND");  // 0 — без звука роликов (для сравнения в журнале)
    return !(e && *e == '0');
}

static int movie_audio_open(void)
{
    if (g_mov_dev)
        return 1;
    if (g_mov_failed)
        return 0;
    if (!movie_audio_wanted()) {
        g_mov_failed = 1;
        return 0;
    }
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        g_mov_failed = 1;
        fprintf(stderr, "[bink] movie sound: SDL audio not available (%s)\n", SDL_GetError());
        return 0;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    SDL_zero(have);
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 2048;
    g_mov_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!g_mov_dev) {
        g_mov_failed = 1;
        fprintf(stderr, "[bink] movie sound: cannot open audio device (%s)\n", SDL_GetError());
        return 0;
    }
    SDL_PauseAudioDevice(g_mov_dev, 0);
    g_mov_out_rate = have.freq ? (u32)have.freq : (u32)want.freq;
    g_mov_need = g_mov_out_rate * 2u;  // ~500 мс стерео 16 бит: ролик может «задуматься» на кадре — звук не рвётся
    const char *vol = getenv("SNX_MOVIE_VOL");  // громкость роликов, доля от исходной
    if (vol && atof(vol) > 0.0)
        g_mov_gain = (float)atof(vol) > 2.0f ? 2.0f : (float)atof(vol);
    fprintf(stderr, "[bink] movie sound: SDL %u Hz stereo, %u ms queue, gain %.2f\n", g_mov_out_rate,
            g_mov_need / (g_mov_out_rate / 250u), (double)g_mov_gain);
    return 1;
}

static int movie_audio_begin(bink *b)
{
    if (!movie_audio_open())
        return 0;
    if (g_mov_owner == b->obj && g_mov_dec && g_mov_stream)
        return 1;
    movie_audio_reset();
    u32 ch = (b->flags[0] & 0x2000) ? 2u : 1u;
    g_mov_dec = bink_audio_new(b->rate[0], ch, (b->flags[0] & 0x1000) != 0, (int)(b->sig >> 24));
    if (!g_mov_dec)
        return 0;
    g_mov_stream = SDL_NewAudioStream(AUDIO_S16SYS, (Uint8)ch, (int)b->rate[0], AUDIO_S16SYS, 2, (int)g_mov_out_rate);
    if (!g_mov_stream) {
        bink_audio_free(g_mov_dec);
        g_mov_dec = NULL;
        return 0;
    }
    g_mov_owner = b->obj;
    g_mov_track = 0;
    g_mov_next = 0;
    g_mov_dry = 0;
    fprintf(stderr, "[bink] movie sound: track 0 %u Hz x%u by the port -> SDL %u Hz x2\n", b->rate[0], ch,
            g_mov_out_rate);
    return 1;
}

// Держит очередь заполненной до g_mov_need, но не забегая вперёд кадров, которые игра уже прошла.
static void movie_audio_pump(bink *b)
{
    if (!b || !b->movie || !b->ntracks)
        return;
    for (u32 t = 0; t < b->ntracks; t++)
        if (b->audio_req[t])  // игру звук берёт — второй раз не играем
            return;
    if (!movie_audio_begin(b))
        return;
    // Перемотка (BinkGoto) и конец ролика (кадры пошли с начала). Обычное упреждение сбросом НЕ считается:
    // здесь была ошибка первой версии — звук разрешалось брать только за уже пройденные кадры, поэтому сразу
    // после BinkNextFrame «следующий кадр» оказывался впереди текущего, и каждый вызов отрисовки глушил
    // декодер вместе с очередью. Отсюда и треск: декодер терял плавный переход между блоками.
    if (g_mov_next > b->frame + MOV_LOOKAHEAD || b->frame > g_mov_next + 90u) {
        bink_audio_reset(g_mov_dec);
        SDL_ClearQueuedAudio(g_mov_dev);
        g_mov_next = b->frame;
        g_mov_started = 0;
    }
    u32 queued = SDL_GetQueuedAudioSize(g_mov_dev);
    if (g_mov_started && !queued && g_mov_next < b->frames && g_mov_next >= 2u)
        g_mov_dry++;  // очередь успела опустеть — в журнале видно, если провалы ещё остались
    u32 limit = b->frames ? b->frames - 1u : 0;  // дальше последнего кадра звука нет
    if (b->frame + MOV_LOOKAHEAD < limit)
        limit = b->frame + MOV_LOOKAHEAD;
    while (queued < g_mov_need && g_mov_next <= limit) {
        u32 f = g_mov_next;
        u32 n = 0;
        const u8 *p = audio_packet(b, f, g_mov_track, &n);
        if (!p || n < 5u) {
            g_mov_next = f + 1u;
            continue;
        }
        // Кадр звука длится ровно столько, сколько заявлено в его заголовке (первый dword): декодер может дать
        // меньше (и лишний хвост блока в последнем кадре) — добиваем/отрезаем тишиной, иначе часы звука уезжают
        // от картинки и раз в несколько секунд очередь пустеет.
        u32 decl = le32(p);
        u32 want = (b->flags[g_mov_track] & 0x4000) ? decl / 2u : decl;  // 16 бит — decl это байты, 8 бит — отсчёты
        if (want > (u32)(sizeof g_mov_pcm / 2u))
            want = (u32)(sizeof g_mov_pcm / 2u);
        u32 got = want ? bink_audio_decode(g_mov_dec, p + 4, n - 4u, g_mov_pcm, want) : 0;
        g_mov_next = f + 1u;
        if (f < 2u)  // видно в журнале: совпадает ли декодированное с заявленным размером кадра
            fprintf(stderr, "[bink] movie sound: frame %u declares %u (%s), decoded %u samples\n", f, decl,
                    (b->flags[g_mov_track] & 0x4000) ? "bytes, 16 bit" : "samples, 8 bit", got);
        if (!want)
            continue;
        if (got < want)
            memset(g_mov_pcm + got, 0, (size_t)(want - got) * sizeof *g_mov_pcm);
        if (g_mov_gain != 1.0f)
            for (u32 i = 0; i < want; i++)
                g_mov_pcm[i] = (s16)(g_mov_pcm[i] * g_mov_gain);
        if (SDL_AudioStreamPut(g_mov_stream, g_mov_pcm, (int)(want * 2u)) != 0)
            break;
        int avail;
        while ((avail = SDL_AudioStreamAvailable(g_mov_stream)) > 0) {
            u8 out[4096];
            int k = SDL_AudioStreamGet(g_mov_stream, out, avail > (int)sizeof out ? (int)sizeof out : avail);
            if (k <= 0)
                break;
            if (SDL_QueueAudio(g_mov_dev, out, (Uint32)k) != 0)
                break;
        }
        g_mov_started = 1;
        queued = SDL_GetQueuedAudioSize(g_mov_dev);
    }
}

static void movie_audio_stop(bink *b)
{
    if (!b || !g_mov_dev || g_mov_owner != b->obj)
        return;
    fprintf(stderr, "[bink] movie sound: %u dry spell(s) in the queue\n", g_mov_dry);
    movie_audio_reset();
}
#endif  // SNX_HEADLESS

void hle_binkw32__BinkDoFrame_4(CPU *c)
{
    HLE_ENTER("BinkDoFrame", 1);
    bink *b = by_obj(ARG(0));
    if (b) {
        video_decode_to(b, b->frame);
        b->do_frame = b->frame;
        b->tick_ns = rt_ticks_ns();
    }
    RET(1, 0);
}

void hle_binkw32__BinkNextFrame_4(CPU *c)
{
    HLE_ENTER("BinkNextFrame", 1);
    bink *b = by_obj(ARG(0));
    if (b) {
        b->tick_ns = rt_ticks_ns();
        wr32(g_mem, b->obj + 16, b->frame + 1);  // LastFrameNum
        b->frame = b->frame + 1 < b->frames ? b->frame + 1 : 0;  // как SDK: после последнего — первый
        sync_obj(b);
        movie_audio_pump(b);  // кадр прошёл — очередь звука ролика пополняется только что пройденными кадрами
    }
    RET(1, 0);
}

// BinkWait(bink): TRUE, пока не пришло время следующего кадра. Без этой функции игра (Engine/D3DDrv) ловит HLE-trap
// на первом же настоящем ролике: с 132-байтовыми LFS-указателями вызывалась только BinkOpen, а BinkWait — нет.
// Не блокируем поток — только сообщаем игре, пора ли звать BinkNextFrame; в остальном — как обычный Bink SDK.
void hle_binkw32__BinkWait_4(CPU *c)
{
    HLE_ENTER("BinkWait", 1);
    bink *b = by_obj(ARG(0));
    int wait = 0;
    if (b && b->fps) {
        u64 period = (u64)b->fpsdiv * 1000000000ull / b->fps;  // длительность кадра, нс
        if (period && rt_ticks_ns() - b->tick_ns < period)
            wait = 1;
    }
    RET(1, wait);
}

// BinkCopyToBuffer(bink, dest, destpitch, destheight, destx, desty, flags): последний декодированный кадр
// (без декодера — чёрный)
void hle_binkw32__BinkCopyToBuffer_28(CPU *c)
{
    HLE_ENTER("BinkCopyToBuffer", 7);
    bink *b = by_obj(ARG(0));
    u32 dst = ARG(1), pitch = ARG(2), dst_h = ARG(3), x0 = ARG(4), y0 = ARG(5), flags = ARG(6);
    if (b && ((s32)x0 < 0 || (s32)y0 < 0)) {
        // ролик больше кадра, игра поставила его левее/выше начала: не писать мимо буфера (покажет
        // bink_movie_overlay)
        static int warned;
        if (!warned++)
            fprintf(stderr, "[bink] BinkCopyToBuffer at %d,%d: outside the frame, skipped\n", (s32)x0, (s32)y0);
    } else if (b && dst >= 0x10000u && dst < RT_WINDOW && pitch > 0 && b->w > 0 && b->h > 0) {
        b->tick_ns = rt_ticks_ns();
        u32 rows = dst_h < b->h ? dst_h : b->h;
        if (rows > 0) {
            u32 type = flags & 15u;
            u32 bpp = (type == 1 || type == 2) ? 3u : (type >= 7 && type <= 12) ? 2u : 4u;
            u64 last_row = (u64)y0 + (u64)(rows - 1u);
            if (b->vid && b->vid_frame != ~0u) {
                u64 row_end = ((u64)x0 + (u64)b->w) * bpp;
                if (last_row <= RT_WINDOW / pitch && (u64)dst + last_row * pitch + row_end <= RT_WINDOW) {
                    u64 t0 = rt_ticks_ns();
                    bink_video_copy(b->vid, g_mem + dst, pitch, rows, x0, y0, flags);
                    b->copy_ns += rt_ticks_ns() - t0;
                    b->copy_n++;
                    d3d8_movie_frame(dst, pitch, x0, y0, b->w, rows);
                }
            } else {
                if (last_row < RT_WINDOW / pitch && (u64)dst + (last_row + 1u) * pitch <= RT_WINDOW)
                    for (u32 y = 0; y < rows; y++)
                        memset(g_mem + dst + ((u64)y0 + y) * pitch, 0, pitch);
            }
        }
    }
    RET(7, 0);
}

void hle_binkw32__BinkGoto_12(CPU *c)
{
    HLE_ENTER("BinkGoto", 3);
    bink *b = by_obj(ARG(0));
    if (b && ARG(1) >= 1 && ARG(1) <= b->frames) {
        b->frame = ARG(1) - 1;
        sync_obj(b);
    }
    RET(3, 0);
}

void hle_binkw32__BinkPause_8(CPU *c)
{
    HLE_ENTER("BinkPause", 2);
    RET(2, ARG(1));
}

// BINKSUMMARY: Width, Height, TotalTime (мс), FileFrameRate, FileFrameRateDiv, FrameRate, FrameRateDiv,
// TotalOpenTime, TotalFrames, TotalPlayedFrames, ...
void hle_binkw32__BinkGetSummary_8(CPU *c)
{
    HLE_ENTER("BinkGetSummary", 2);
    bink *b = by_obj(ARG(0));
    u32 s = ARG(1);
    if (s < 0x10000u || (u64)s + 0x60u > RT_WINDOW)
        RET(2, 0);
    memset(g_mem + s, 0, 0x60);
    if (b) {
        wr32(g_mem, s, b->w);
        wr32(g_mem, s + 4, b->h);
        wr32(g_mem, s + 8, (u32)((u64)b->frames * b->fpsdiv * 1000 / (b->fps ? b->fps : 1)));
        wr32(g_mem, s + 12, b->fps);
        wr32(g_mem, s + 16, b->fpsdiv);
        wr32(g_mem, s + 20, b->fps);
        wr32(g_mem, s + 24, b->fpsdiv);
        wr32(g_mem, s + 32, b->frames);
        wr32(g_mem, s + 36, b->frame + 1);
    }
    RET(2, 0);
}

// Активная заставка главного меню/логотипов — признак меню, когда GUI-меню движок не открыл.
// Игровые ролики (сцены, диалоги) сюда не входят: во время них экранные кнопки должны оставаться.
int bink_menu_movie_active(void)
{
    u64 now = rt_ticks_ns();
    for (int i = 0; i < MAX_BINK; i++) {
        bink *b = g_bink[i];
        if (b && b->menu_movie && now - b->tick_ns < 250000000ull)
            return 1;
    }
    return 0;
}

// Ролик, который игра крутит (вызовы по нему не старше 250 мс), — для показа самим портом, когда его кадр не попадает
// в задний буфер (d3d8_present). Игра (D3DDrv) копирует кадр Bink 1:1 в середину заднего буфера и только если он
// туда помещается: ролики игры — 800x600 и 640x480, а кадр порта на телефоне — экран x «Разрешение» (на 20:9 при
// 50 % — 1200x540, на 720p — 780x360). Тогда звук идёт, а экран чёрный. Кадр — RGBA (R,G,B,255), строки сверху
// вниз; буфер живёт до следующего вызова.
int bink_movie_overlay(const u8 **rgba, u32 *w, u32 *h)
{
    static u8 *buf;
    static u32 buf_size, last_obj, last_frame = ~0u;
    u64 now = rt_ticks_ns();
    bink *m = NULL;
    for (int i = 0; i < MAX_BINK; i++) {
        bink *b = g_bink[i];
        if (b && b->movie && b->vid && now - b->tick_ns < 250000000ull && (!m || b->tick_ns > m->tick_ns))
            m = b;
    }
    if (!m)
        return 0;
    u32 f = m->do_frame != ~0u ? m->do_frame : m->frame;
    movie_audio_pump(m);  // игра зовёт только BinkDoFrame — звук ролика тоже должен идти
    video_decode_to(m, f);
    if (m->vid_frame == ~0u)
        return 0;
    u32 need = m->w * m->h * 4;
    if (need > buf_size) {
        u8 *nb = realloc(buf, need);
        if (!nb)
            return 0;
        buf = nb;
        buf_size = need;
        last_obj = 0;
    }
    if (m->obj != last_obj || m->vid_frame != last_frame) {
        if (m->obj != last_obj)
            memset(buf, 0, need);  // кадр декодера мог быть меньше заголовка
        u64 t0 = rt_ticks_ns();
        bink_video_copy(m->vid, buf, m->w * 4, m->h, 0, 0, 4 /* BINKSURFACE32R: R,G,B, альфа 255 */);
        m->copy_ns += rt_ticks_ns() - t0;
        m->copy_n++;
        last_obj = m->obj;
        last_frame = m->vid_frame;
    }
    *rgba = buf;
    *w = m->w;
    *h = m->h;
    return 1;
}

// Замена системного звукодрайвера Bink и глобальный выключатель звука: в headless-сборке звук
// идёт через bink_audio.c, поэтому достаточно вернуть «предыдущее» состояние.
void hle_binkw32__BinkSetSoundSystem_8(CPU *c)
{
    HLE_ENTER("BinkSetSoundSystem", 2);
    RET(2, 0);
}

void hle_binkw32__BinkSetSoundOnOff_8(CPU *c)
{
    HLE_ENTER("BinkSetSoundOnOff", 2);
    RET(2, 1);
}
