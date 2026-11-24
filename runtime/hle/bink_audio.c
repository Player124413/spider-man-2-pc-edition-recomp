// Декодер звука Bink (вариант DCT — все файлы игры, ревизия 'i'). Пакет кадра — последовательность блоков,
// каждый выровнен на 32 бита; биты читаются младшими вперёд. Блок: 2 бита (пропуск), затем на канал:
// коэффициенты 0 и 1 (float: 5 бит степени, 23 мантиссы, знак), 8-битные индексы шага квантования полос
// (границы — критические частоты слуха), остальные коэффициенты группами (длина группы — 8 или по таблице
// RLE, ширина значений 4 бита, знак отдельным битом), DCT-III длины N и плавный переход первых N/16 сэмплов
// от хвоста предыдущего блока. Выход блока — N − N/16 сэмплов на канал.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/hle/bink.h"

#define MAX_N 2048

static const u16 crit_freq[25] = {100,  200,  300,  400,  510,  630,  770,  920,  1080, 1270, 1480,  1720, 2000,
                                  2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 24500};
static const u8 rle_len[16] = {2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64};

struct bink_audio {
    u32 channels, n, nbits, overlap, nbands;
    u32 bands[26];
    float quant_table[96], root;
    int first;
    float prev[2][MAX_N / 16];
    float coef[2][MAX_N];
    float tw_re[MAX_N], tw_im[MAX_N];      // e^{iπk/2N} для предобработки DCT
    float fft_re[MAX_N / 2], fft_im[MAX_N / 2];  // e^{2πik/N}: поворот нечётной половины и обратное БПФ
    u16 rev[MAX_N / 2];                    // перестановка для БПФ длины N/2
};

typedef struct {
    const u8 *p;
    u32 pos, end;  // в битах
} bits;

// n ≤ 23 бит; за концом пакета — нули. Внутри пакета — одно чтение 8 байт (хост little-endian), у конца — по биту
static inline u32 get(bits *b, u32 n)
{
    u32 pos = b->pos;
    if (pos + n <= b->end && (pos >> 3) + 8 <= (b->end >> 3)) {
        u64 w;
        memcpy(&w, b->p + (pos >> 3), 8);
        b->pos = pos + n;
        return (u32)(w >> (pos & 7)) & (u32)((1ull << n) - 1);
    }
    u32 v = 0;
    for (u32 i = 0; i < n; i++, b->pos++)
        if (b->pos < b->end)
            v |= (u32)((b->p[b->pos >> 3] >> (b->pos & 7)) & 1) << i;
    return v;
}

static float get_float(bits *b)
{
    int power = (int)get(b, 5);
    float f = ldexpf((float)get(b, 23), power - 23);
    return get(b, 1) ? -f : f;
}

bink_audio *bink_audio_new(u32 rate, u32 channels, int dct, int revision)
{
    if (!dct || revision == 'b' || channels < 1 || channels > 2 || !rate) {
        fprintf(stderr, "[bink] audio %u Hz x%u %s rev '%c': not supported\n", rate, channels, dct ? "DCT" : "RDFT",
                revision);
        return NULL;
    }
    bink_audio *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->nbits = rate < 22050 ? 9 : rate < 44100 ? 10 : 11;
    a->n = 1u << a->nbits;
    a->overlap = a->n / 16;
    a->channels = channels;
    a->root = (float)a->n / (sqrtf((float)a->n) * 32768.0f);
    for (int i = 0; i < 96; i++)
        a->quant_table[i] = expf((float)i * 0.15289164787221953823f) * a->root;
    u64 half = ((u64)rate + 1ull) / 2ull;
    for (a->nbands = 1; a->nbands < 25; a->nbands++)
        if (half <= crit_freq[a->nbands - 1])
            break;
    a->bands[0] = 2;
    for (u32 i = 1; i < a->nbands; i++) {
        u32 bi = (u32)(((u64)crit_freq[i - 1] * a->n / half) & ~1ull);
        if (bi < a->bands[i - 1])
            bi = a->bands[i - 1];
        if (bi > a->n)
            bi = a->n;
        a->bands[i] = bi;
    }
    a->bands[a->nbands] = a->n;
    const double pi = 3.14159265358979323846;
    for (u32 k = 0; k < a->n; k++) {
        a->tw_re[k] = (float)cos(pi * k / (2.0 * a->n));
        a->tw_im[k] = (float)sin(pi * k / (2.0 * a->n));
    }
    for (u32 k = 0; k < a->n / 2; k++) {
        a->fft_re[k] = (float)cos(2.0 * pi * k / a->n);
        a->fft_im[k] = (float)sin(2.0 * pi * k / a->n);
        u32 r = 0;
        for (u32 b = 0; b + 1 < a->nbits; b++)
            r |= ((k >> b) & 1) << (a->nbits - 2 - b);
        a->rev[k] = (u16)r;
    }
    a->first = 1;
    return a;
}

void bink_audio_free(bink_audio *a) { free(a); }

void bink_audio_reset(bink_audio *a)
{
    if (a)
        a->first = 1;
}

// x[i] = 2/N · (X0 + Σ_{k≥1} X_k cos(πk(2i+1)/2N)) — обратное DCT-II через обратное БПФ:
// V_k = e^{iπk/2N}(X_k − iX_{N−k}), v = ОБПФ_N(V) (без 1/N), x[2m] = v[m]/N, x[2m+1] = v[N−1−m]/N.
// V эрмитов (V_{N−k} = V̄_k), v вещественный — и считается БПФ длины h = N/2: Z_k = (V_k + V_{k+h}) +
// i·e^{2πik/N}(V_k − V_{k+h}), z = ОБПФ_h(Z), тогда v[2j] = Re z[j], v[2j+1] = Im z[j]. Вдвое меньше бабочек
static void idct(bink_audio *a, float *x)
{
    u32 n = a->n, h = n / 2;
    float re[MAX_N / 2], im[MAX_N / 2];
    x[0] *= 2.0f;
    for (u32 k = 0; k < h; k++) {
        u32 k2 = k + h;
        float xr = x[k], xi = k ? -x[n - k] : 0.0f, yr = x[k2], yi = -x[n - k2];
        float vr = xr * a->tw_re[k] - xi * a->tw_im[k], vi = xr * a->tw_im[k] + xi * a->tw_re[k];
        float ur = yr * a->tw_re[k2] - yi * a->tw_im[k2], ui = yr * a->tw_im[k2] + yi * a->tw_re[k2];
        float dr = vr - ur, di = vi - ui, wr = a->fft_re[k], wi = a->fft_im[k];
        float odr = dr * wr - di * wi, odi = dr * wi + di * wr;  // e^{2πik/N}(V_k − V_{k+h})
        u32 r = a->rev[k];
        re[r] = vr + ur - odi;
        im[r] = vi + ui + odr;
    }
    for (u32 len = 2; len <= h; len <<= 1) {
        u32 half = len / 2, step = n / len;  // e^{2πij/len} = fft[j·N/len]
        for (u32 s = 0; s < h; s += len)
            for (u32 j = 0; j < half; j++) {
                float wr = a->fft_re[j * step], wi = a->fft_im[j * step];
                u32 p = s + j, q = p + half;
                float tr = re[q] * wr - im[q] * wi, ti = re[q] * wi + im[q] * wr;
                re[q] = re[p] - tr;
                im[q] = im[p] - ti;
                re[p] += tr;
                im[p] += ti;
            }
    }
    float scale = 1.0f / (float)n;  // X0 удвоен выше: ОБПФ/N даёт ровно 2/N·(…)
    for (u32 m = 0; m < h; m++) {
        u32 t = n - 1 - m;
        x[2 * m] = (m & 1 ? im[m >> 1] : re[m >> 1]) * scale;
        x[2 * m + 1] = (t & 1 ? im[t >> 1] : re[t >> 1]) * scale;
    }
}

static int decode_block(bink_audio *a, bits *b)
{
    get(b, 2);
    for (u32 ch = 0; ch < a->channels; ch++) {
        float *c = a->coef[ch], quant[25];
        if (b->pos > b->end || b->end - b->pos < 58 + a->nbands * 8)
            return 0;
        c[0] = get_float(b) * a->root;
        c[1] = get_float(b) * a->root;
        for (u32 i = 0; i < a->nbands; i++) {
            u32 v = get(b, 8);
            quant[i] = a->quant_table[v < 95 ? v : 95];
        }
        u32 k = 0, i = 2;
        float q = quant[0];
        while (i < a->n) {
            u32 j = i + (get(b, 1) ? rle_len[get(b, 4)] * 8u : 8u);
            if (j > a->n)
                j = a->n;
            u32 width = get(b, 4);
            if (!width) {
                memset(c + i, 0, (j - i) * sizeof *c);
                i = j;
                while (k < a->nbands && a->bands[k] < i)
                    q = quant[k++];
                continue;
            }
            u32 need_bits = (j - i) * (width + 1);
            if (b->pos <= b->end && need_bits <= b->end - b->pos &&
                ((b->pos + need_bits) >> 3) + 8 <= (b->end >> 3)) {  // группа целиком внутри пакета — без проверок
                u32 pos = b->pos, mask = (1u << width) - 1;
                for (; i < j; i++) {
                    while (k < a->nbands && a->bands[k] <= i)
                        q = quant[k++];
                    u64 w;
                    memcpy(&w, b->p + (pos >> 3), 8);
                    u32 x = (u32)(w >> (pos & 7)), v = x & mask;
                    c[i] = v ? (x >> width & 1 ? -q : q) * (float)v : 0.0f;
                    pos += v ? width + 1 : width;
                }
                b->pos = pos;
            }
            for (; i < j; i++) {
                while (k < a->nbands && a->bands[k] <= i)
                    q = quant[k++];
                u32 v = get(b, width);
                c[i] = v ? (get(b, 1) ? -q : q) * (float)v : 0.0f;
            }
        }
        idct(a, c);
    }
    // плавный переход: веса считаются по чередующимся сэмплам (как у исходного декодера с чередованием каналов)
    u32 count = a->overlap * a->channels;
    for (u32 ch = 0; ch < a->channels; ch++) {
        float *c = a->coef[ch];
        if (!a->first)
            for (u32 i = 0, j = ch; i < a->overlap; i++, j += a->channels)
                c[i] = (a->prev[ch][i] * (float)(count - j) + c[i] * (float)j) / (float)count;
        memcpy(a->prev[ch], c + a->n - a->overlap, a->overlap * sizeof *c);
    }
    a->first = 0;
    return 1;
}

static s16 to_s16(float f)
{
    if (!(f == f))  // NaN из повреждённого потока
        return 0;
    if (f >= 1.0f)
        return 32767;
    if (f <= -1.0f)
        return -32768;
    long v = lrintf(f * 32768.0f);
    return (s16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

u32 bink_audio_decode(bink_audio *a, const u8 *packet, u32 bytes, s16 *out, u32 max)
{
    // Предел 0x0FFFFFFF байт (< 2^31 бит) оставляет запас до UINT32_MAX, чтобы b.pos в decode_block не переполнял u32
    if (!a || !packet || !out || !bytes || !max || bytes > 0x0FFFFFFFu)
        return 0;
    bits b = {packet, 0, bytes * 8u};
    u32 got = 0, per = a->n - a->overlap;
    while (b.pos <= b.end && b.end - b.pos >= 32 && decode_block(a, &b)) {
        for (u32 i = 0; i < per && got + a->channels <= max; i++)
            for (u32 ch = 0; ch < a->channels; ch++)
                out[got++] = to_s16(a->coef[ch][i]);
        if (b.pos > b.end)
            break;
        b.pos = (b.pos + 31u) & ~31u;
    }
    return got;
}
