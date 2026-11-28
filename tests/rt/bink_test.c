// Синтетический тест парсера контейнера Bink и декодера звука (runtime/hle/bink.c, bink_audio.c):
// API bink_audio_* (частоты, каналы, сброс, тишина, ненулевой DC, усечённые и случайные пакеты)
// и HLE binkw32 (минимальные валидные BIKi из памяти и файла, а также повреждённые/усечённые заголовки).
// Запуск: bink_test <временный каталог>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime/abi.h"
#include "runtime/heap.h"
#include "runtime/hle/bink.h"
#include "runtime/hle/vfs.h"
#include "runtime/rt.h"
#include "runtime/vmem.h"

void hle_binkw32__BinkOpen_8(CPU *c);
void hle_binkw32__BinkClose_4(CPU *c);
void hle_binkw32__BinkOpenTrack_8(CPU *c);
void hle_binkw32__BinkCloseTrack_4(CPU *c);
void hle_binkw32__BinkGetTrackData_8(CPU *c);
void hle_binkw32__BinkWait_4(CPU *c);
void hle_binkw32__BinkDoFrame_4(CPU *c);
void hle_binkw32__BinkNextFrame_4(CPU *c);
void hle_binkw32__BinkCopyToBuffer_28(CPU *c);
void hle_binkw32__BinkGoto_12(CPU *c);
void hle_binkw32__BinkPause_8(CPU *c);
void hle_binkw32__BinkGetSummary_8(CPU *c);

#define BINKFROMMEMORY 0x04000000u
#define STACK 0x00200000u
#define G_NAME 0x00300000u
#define G_BUF 0x00400000u
#define G_PCM 0x00500000u
#define G_SUM 0x00600000u

static int g_fails;
static CPU g_cpu;

#define CHECK(cond, ...)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            if (g_fails++ < 32) {                                                                                    \
                printf("FAIL (%s:%d): ", __FILE__, __LINE__);                                                        \
                printf(__VA_ARGS__);                                                                                 \
                printf("\n");                                                                                        \
            }                                                                                                        \
        }                                                                                                            \
    } while (0)

static u32 hle_call(void (*fn)(CPU *), int n, const u32 *args)
{
    g_cpu.esp = STACK - 4u * (u32)(n + 1);
    wr32(g_mem, g_cpu.esp, 0xDEADBEEFu);
    for (int i = 0; i < n; i++)
        wr32(g_mem, g_cpu.esp + 4u + 4u * (u32)i, args[i]);
    fn(&g_cpu);
    CHECK(g_cpu.esp == STACK, "дисбаланс стека после вызова HLE: esp=%08x", g_cpu.esp);
    return g_cpu.eax;
}

static void put_le32(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
}

static void put_le16(u8 *p, u16 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
}

// Запись одного значения в битовый поток LSB-first (как читает bink_audio.c)
typedef struct {
    u8 *buf;
    u32 pos, cap_bits;
} bitw;

static void bw_put(bitw *w, u32 val, u32 nbits)
{
    for (u32 i = 0; i < nbits && w->pos < w->cap_bits; i++, w->pos++)
        if ((val >> i) & 1u)
            w->buf[w->pos >> 3] |= (u8)(1u << (w->pos & 7));
}

static u32 nbands_for(u32 rate)
{
    static const u16 crit_freq[25] = {100,  200,  300,  400,  510,  630,  770,  920,  1080, 1270, 1480,  1720, 2000,
                                      2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 24500};
    u32 half = (rate + 1) / 2, nb = 1;
    for (; nb < 25; nb++)
        if (half <= crit_freq[nb - 1])
            break;
    return nb;
}

// Собрать валидный выровненный на 32 бита DCT-пакет из одного блока (все полосы width=0 через RLE;
// если dc_mant != 0 — ненулевой коэффициент c[0]). Возвращает размер пакета в байтах.
static u32 build_dct_block(u8 *pkt, u32 cap, u32 rate, u32 channels, u32 dc_power, u32 dc_mant)
{
    memset(pkt, 0, cap);
    u32 nbits = rate < 22050 ? 9 : rate < 44100 ? 10 : 11;
    u32 n = 1u << nbits;
    u32 nb = nbands_for(rate);
    bitw bw = {pkt, 0, cap * 8};
    bw_put(&bw, 0, 2);  // 2 бита пропуска
    for (u32 ch = 0; ch < channels; ch++) {
        // c[0]: степень (5), мантисса (23), знак (1)
        bw_put(&bw, dc_power, 5);
        bw_put(&bw, dc_mant, 23);
        bw_put(&bw, 0, 1);
        // c[1] = 0
        bw_put(&bw, 0, 29);
        // индексы квантования полос (nb * 8 бит)
        for (u32 i = 0; i < nb; i++)
            bw_put(&bw, 0, 8);
        // группы коэффициентов 2..n: RLE длиной 64*8 = 512 (индекс 15), ширина 0
        for (u32 i = 2; i < n; i += 512) {
            bw_put(&bw, 1, 1);   // RLE
            bw_put(&bw, 15, 4);  // rle_len[15] = 64 -> 512 коэффициентов
            bw_put(&bw, 0, 4);   // width = 0 (нули)
        }
    }
    return ((bw.pos + 31u) & ~31u) / 8u;
}

static void test_bink_audio_api(void)
{
    // Неподдерживаемые параметры
    CHECK(bink_audio_new(44100, 2, 0, 'i') == NULL, "RDFT не поддержан");
    CHECK(bink_audio_new(44100, 2, 1, 'b') == NULL, "ревизия 'b' не поддержана");
    CHECK(bink_audio_new(44100, 0, 1, 'i') == NULL, "0 каналов");
    CHECK(bink_audio_new(44100, 3, 1, 'i') == NULL, "3 канала");
    CHECK(bink_audio_new(0, 1, 1, 'i') == NULL, "частота 0");

    // Безопасность при NULL
    s16 dummy[16] = {0};
    bink_audio_free(NULL);
    bink_audio_reset(NULL);
    CHECK(bink_audio_decode(NULL, (const u8 *)"\0\0\0\0", 4, dummy, 16) == 0, "decode(NULL)");

    // Все три ветки длины окна (nbits = 9, 10, 11) для моно и стерео
    static const u32 rates[3] = {11025, 22050, 44100};
    static const u32 ns[3] = {512, 1024, 2048};
    s16 out[8192];
    for (int ri = 0; ri < 3; ri++) {
        for (u32 ch = 1; ch <= 2; ch++) {
            bink_audio *a = bink_audio_new(rates[ri], ch, 1, 'i');
            CHECK(a != NULL, "bink_audio_new(%u, %u)", rates[ri], ch);
            if (!a)
                continue;
            u32 per = ns[ri] - ns[ri] / 16;

            // Пустой и усечённые пакеты короче заголовка блока
            u8 tiny[12] = {0xFF, 0xAA, 0x55, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
            CHECK(bink_audio_decode(a, tiny, 0, out, 8192) == 0, "пустой пакет");
            CHECK(bink_audio_decode(a, tiny, 1, out, 8192) == 0, "усечённый пакет 1 байт");
            CHECK(bink_audio_decode(a, tiny, 7, out, 8192) == 0, "усечённый пакет 7 байт");
            CHECK(bink_audio_decode(a, tiny, 12, out, 8192) == 0, "усечённый пакет 12 байт");

            // Валидный нулевой блок DCT -> тишина и проверка ограничения max с канарейкой
            u8 pkt[256];
            u32 pkt_len = build_dct_block(pkt, sizeof pkt, rates[ri], ch, 0, 0);
            s16 cap_buf[32];
            for (int k = 0; k < 32; k++)
                cap_buf[k] = 0x5A5A;
            u32 got_cap = bink_audio_decode(a, pkt, pkt_len, cap_buf, 10);
            CHECK(got_cap <= 10, "переполнение буфера max=10: got=%u", got_cap);
            CHECK(cap_buf[10] == 0x5A5A && cap_buf[31] == 0x5A5A, "запись за границу max");

            // Полный нулевой блок: выход равен per*ch нулевых сэмплов
            bink_audio_reset(a);
            u32 got = bink_audio_decode(a, pkt, pkt_len, out, per * ch);
            CHECK(got == per * ch, "нулевой пакет %uHz x%u: got=%u want=%u", rates[ri], ch, got, per * ch);
            int nonz = 0;
            for (u32 k = 0; k < got; k++)
                if (out[k] != 0)
                    nonz = 1;
            CHECK(!nonz, "нулевой DCT-пакет дал ненулевые сэмплы");
            bink_audio_free(a);
        }
    }

    // Проверка ненулевого DC-коэффициента, перекрытия двух блоков и сброса (bink_audio_reset)
    bink_audio *a = bink_audio_new(22050, 1, 1, 'i');
    CHECK(a != NULL, "22050 моно");
    if (a) {
        u8 pkt[256];
        u32 pkt_len = build_dct_block(pkt, sizeof pkt, 22050, 1, 20, 0x400000u);
        s16 first_run[960], second_run[960], after_reset[960];
        u32 g1 = bink_audio_decode(a, pkt, pkt_len, first_run, 960);
        CHECK(g1 == 960, "DC блок 1: got=%u", g1);
        int any_nz = 0;
        for (u32 k = 0; k < g1; k++)
            if (first_run[k] != 0)
                any_nz = 1;
        CHECK(any_nz, "ненулевой DC-пакет дал только нули");
        // Второй блок подряд использует хвост перекрытия (!first), после reset — снова идентичен first_run
        u32 g2 = bink_audio_decode(a, pkt, pkt_len, second_run, 960);
        CHECK(g2 == 960, "DC блок 2: got=%u", g2);
        bink_audio_reset(a);
        u32 g3 = bink_audio_decode(a, pkt, pkt_len, after_reset, 960);
        CHECK(g3 == 960 && memcmp(first_run, after_reset, sizeof first_run) == 0,
              "после bink_audio_reset выход должен совпадать с первым блоком");

        // Многоблочный пакет (2 блока подряд в одном пакете)
        u8 two[512];
        memcpy(two, pkt, pkt_len);
        memcpy(two + pkt_len, pkt, pkt_len);
        bink_audio_reset(a);
        u32 g_two = bink_audio_decode(a, two, pkt_len * 2, out, 2048);
        CHECK(g_two == 1920, "двухблочный пакет: got=%u want=1920", g_two);
        bink_audio_free(a);
    }
}

// Собрать минимальный валидный BIKi в buf (2 кадра, 1 аудиодорожка 22050 Гц моно DCT)
static u32 build_valid_bik(u8 *buf, u32 w, u32 h, u32 fps, u32 fpsdiv, u16 track_flags)
{
    u8 dct_pkt[128];
    u32 apkt_payload = build_dct_block(dct_pkt, sizeof dct_pkt, 22050, 1, 0, 0);
    const u32 frames = 2, ntracks = 1;
    const u32 hdr_sz = 44 + ntracks * 12 + (frames + 1) * 4;  // 44 + 12 + 12 = 68
    const u32 apkt_bytes = 4 + apkt_payload;                  // pcm_bytes + DCT
    const u32 frame_sz = 4 + apkt_bytes;                      // длина аудиопакета + тело
    const u32 total = hdr_sz + frames * frame_sz;
    memset(buf, 0, total);
    memcpy(buf, "BIKi", 4);
    put_le32(buf + 4, total - 8);
    put_le32(buf + 8, frames);
    put_le32(buf + 12, frame_sz);
    put_le32(buf + 20, w);
    put_le32(buf + 24, h);
    put_le32(buf + 28, fps);
    put_le32(buf + 32, fpsdiv);
    put_le32(buf + 36, 0);        // video_flags
    put_le32(buf + 40, ntracks);
    // Метаданные дорожки 0
    put_le32(buf + 44, 2048);         // maxsize
    put_le16(buf + 48, 22050);        // rate
    put_le16(buf + 50, track_flags);  // flags (0x4000=16-bit, 0x1000=DCT)
    put_le32(buf + 52, 0);            // track id
    // Таблица смещений кадров (frames + 1)
    u32 f0 = hdr_sz, f1 = hdr_sz + frame_sz, f2 = total;
    put_le32(buf + 56, f0 | 1u);  // ключевой кадр 0
    put_le32(buf + 60, f1);       // кадр 1
    put_le32(buf + 64, f2);       // конец файла
    // Аудиопакеты кадров 0 и 1
    for (u32 f = 0; f < frames; f++) {
        u8 *fp = buf + hdr_sz + f * frame_sz;
        put_le32(fp, apkt_bytes);
        put_le32(fp + 4, 1920);  // заявлено 1920 байт PCM
        memcpy(fp + 8, dct_pkt, apkt_payload);
    }
    return total;
}

static void write_file_bytes(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "не удалось создать файл %s", path);
    if (f) {
        CHECK(fwrite(data, 1, n, f) == n, "запись в %s", path);
        fclose(f);
    }
}

static void test_bink_container_hle(const char *tmp_dir)
{
    char game[1024], user[1024], movies[1100], fpath[1200];
    snprintf(game, sizeof game, "%s/game", tmp_dir);
    snprintf(user, sizeof user, "%s/user", tmp_dir);
    snprintf(movies, sizeof movies, "%s/Movies", game);
    mkdir(tmp_dir, 0755);
    mkdir(game, 0755);
    mkdir(user, 0755);
    mkdir(movies, 0755);

    // Записываем тестовые .bik заранее до vfs_init (vfs кеширует каталог игры)
    u8 valid_bik[1024];
    u32 valid_sz = build_valid_bik(valid_bik, 0, 0, 30, 1, 0x5000);
    snprintf(fpath, sizeof fpath, "%s/valid.bik", movies);
    write_file_bytes(fpath, valid_bik, valid_sz);

    // Ролик с настоящими размерами кадра и очень быстрыми кадрами (fps=1000, кадр 1 мс): на нём проверяется
    // расписание BinkWait — если игра не успевает, кадры пропускаются, а не растягивают ролик.
    u8 pacing_bik[1024];
    u32 pacing_sz = build_valid_bik(pacing_bik, 8, 8, 1000, 1, 0x5000);
    snprintf(fpath, sizeof fpath, "%s/pacing.bik", movies);
    write_file_bytes(fpath, pacing_bik, pacing_sz);

    // Повреждённые файлы в Movies/
    u8 short_bik[20];
    memset(short_bik, 0, sizeof short_bik);
    memcpy(short_bik, "BIKi", 4);
    put_le32(short_bik + 4, 12);
    snprintf(fpath, sizeof fpath, "%s/short.bik", movies);
    write_file_bytes(fpath, short_bik, sizeof short_bik);

    u8 bad_magic[64];
    memset(bad_magic, 0, sizeof bad_magic);
    memcpy(bad_magic, "NOPE", 4);
    snprintf(fpath, sizeof fpath, "%s/badmagic.bik", movies);
    write_file_bytes(fpath, bad_magic, sizeof bad_magic);

    u8 zero_frames[64];
    memset(zero_frames, 0, sizeof zero_frames);
    memcpy(zero_frames, "BIKi", 4);
    put_le32(zero_frames + 4, sizeof zero_frames - 8);
    snprintf(fpath, sizeof fpath, "%s/zeroframes.bik", movies);
    write_file_bytes(fpath, zero_frames, sizeof zero_frames);

    u8 many_tracks[128];
    memset(many_tracks, 0, sizeof many_tracks);
    memcpy(many_tracks, "BIKi", 4);
    put_le32(many_tracks + 4, sizeof many_tracks - 8);
    put_le32(many_tracks + 8, 1);
    put_le32(many_tracks + 40, 5);  // > 4 дорожек
    snprintf(fpath, sizeof fpath, "%s/manytracks.bik", movies);
    write_file_bytes(fpath, many_tracks, sizeof many_tracks);

    u8 trunc_offs[60];
    memset(trunc_offs, 0, sizeof trunc_offs);
    memcpy(trunc_offs, "BIKi", 4);
    put_le32(trunc_offs + 4, sizeof trunc_offs - 8);
    put_le32(trunc_offs + 8, 10);  // нужно 44 + 44 = 88 > 60
    snprintf(fpath, sizeof fpath, "%s/truncoffs.bik", movies);
    write_file_bytes(fpath, trunc_offs, sizeof trunc_offs);

    vfs_init(game, user, "C:\\Shrek2");

    // 1. Валидный BIKi из памяти (BINKFROMMEMORY): 16-бит дорожка
    memcpy(g_mem + G_BUF, valid_bik, valid_sz);
    u32 a_open[2] = {G_BUF, BINKFROMMEMORY};
    u32 b = hle_call(hle_binkw32__BinkOpen_8, 2, a_open);
    CHECK(b != 0, "BinkOpen(BINKFROMMEMORY) валидного BIKi вернул 0");
    if (b) {
        CHECK(rd32(g_mem, b + 8) == 2 && rd32(g_mem, b + 12) == 1 && rd32(g_mem, b + 20) == 30 &&
                  rd32(g_mem, b + 24) == 1,
              "поля BINK: frames=%u frameNum=%u fps=%u/%u", rd32(g_mem, b + 8), rd32(g_mem, b + 12),
              rd32(g_mem, b + 20), rd32(g_mem, b + 24));

        u32 a_sum[2] = {b, G_SUM};
        hle_call(hle_binkw32__BinkGetSummary_8, 2, a_sum);
        CHECK(rd32(g_mem, G_SUM + 8) == 66 && rd32(g_mem, G_SUM + 32) == 2 && rd32(g_mem, G_SUM + 36) == 1,
              "BINKSUMMARY: time=%u frames=%u played=%u", rd32(g_mem, G_SUM + 8), rd32(g_mem, G_SUM + 32),
              rd32(g_mem, G_SUM + 36));

        u32 a_tr0[2] = {b, 0}, a_tr1[2] = {b, 1};
        u32 tr = hle_call(hle_binkw32__BinkOpenTrack_8, 2, a_tr0);
        CHECK(tr != 0, "BinkOpenTrack(0) вернул 0");
        CHECK(hle_call(hle_binkw32__BinkOpenTrack_8, 2, a_tr1) == 0, "BinkOpenTrack(1) должен вернуть 0");
        if (tr) {
            CHECK(rd32(g_mem, tr) == 22050 && rd32(g_mem, tr + 4) == 16 && rd32(g_mem, tr + 8) == 1 &&
                      rd32(g_mem, tr + 12) >= 1920,
                  "BINKTRACK: rate=%u bits=%u ch=%u max=%u", rd32(g_mem, tr), rd32(g_mem, tr + 4), rd32(g_mem, tr + 8),
                  rd32(g_mem, tr + 12));
            memset(g_mem + G_PCM, 0xCC, 2048);
            u32 a_td[2] = {tr, G_PCM};
            u32 got = hle_call(hle_binkw32__BinkGetTrackData_8, 2, a_td);
            CHECK(got == 1920, "BinkGetTrackData вернул %u вместо 1920", got);
            CHECK(g_mem[G_PCM] == 0 && g_mem[G_PCM + 1919] == 0, "PCM тишины не нулевой");
            u32 a_ct[1] = {tr};
            hle_call(hle_binkw32__BinkCloseTrack_4, 1, a_ct);
        }

        // Навигация по кадрам (NextFrame с заворотом и Goto)
        u32 a_b[1] = {b};
        hle_call(hle_binkw32__BinkNextFrame_4, 1, a_b);
        CHECK(rd32(g_mem, b + 12) == 2 && rd32(g_mem, b + 16) == 1, "после 1-го NextFrame");
        hle_call(hle_binkw32__BinkNextFrame_4, 1, a_b);
        CHECK(rd32(g_mem, b + 12) == 1 && rd32(g_mem, b + 16) == 2, "заворот после 2-го NextFrame");
        u32 a_gt[3] = {b, 2, 0};
        hle_call(hle_binkw32__BinkGoto_12, 3, a_gt);
        CHECK(rd32(g_mem, b + 12) == 2, "BinkGoto(2)");
        u32 a_gt_bad[3] = {b, 99, 0};
        hle_call(hle_binkw32__BinkGoto_12, 3, a_gt_bad);
        CHECK(rd32(g_mem, b + 12) == 2, "BinkGoto вне диапазона не должен менять кадр");
        u32 a_p[2] = {b, 1};
        CHECK(hle_call(hle_binkw32__BinkPause_8, 2, a_p) == 1, "BinkPause(1)");
        hle_call(hle_binkw32__BinkClose_4, 1, a_b);
    }

    // 2. Валидный BIKi с 8-битной дорожкой (проверка ветки bits8 -> 0x80 тишина) и fps=0/fpsdiv=0 (без деления на 0)
    u8 bik8[1024];
    u32 sz8 = build_valid_bik(bik8, 0, 0, 0, 0, 0x1000);  // 8-бит, DCT
    memcpy(g_mem + G_BUF, bik8, sz8);
    u32 b8 = hle_call(hle_binkw32__BinkOpen_8, 2, a_open);
    CHECK(b8 != 0, "BinkOpen 8-бит fps=0");
    if (b8) {
        u32 a_sum[2] = {b8, G_SUM};
        hle_call(hle_binkw32__BinkGetSummary_8, 2, a_sum);
        u32 a_tr0[2] = {b8, 0};
        u32 tr8 = hle_call(hle_binkw32__BinkOpenTrack_8, 2, a_tr0);
        CHECK(tr8 != 0 && rd32(g_mem, tr8 + 4) == 8, "8-бит BINKTRACK");
        if (tr8) {
            u32 a_td[2] = {tr8, G_PCM};
            CHECK(hle_call(hle_binkw32__BinkGetTrackData_8, 2, a_td) == 1920, "8-бит GetTrackData");
        }
        u32 a_b[1] = {b8};
        hle_call(hle_binkw32__BinkClose_4, 1, a_b);
    }

    // 3. Валидный BIKi через VFS (файл на диске)
    strcpy((char *)g_mem + G_NAME, "C:\\Shrek2\\Movies\\valid.bik");
    u32 a_file[2] = {G_NAME, 0};
    u32 bf = hle_call(hle_binkw32__BinkOpen_8, 2, a_file);
    CHECK(bf != 0, "BinkOpen из файла valid.bik вернул 0");
    if (bf) {
        u32 a_tr0[2] = {bf, 0};
        u32 trf = hle_call(hle_binkw32__BinkOpenTrack_8, 2, a_tr0);
        CHECK(trf != 0, "BinkOpenTrack для файла");
        if (trf) {
            u32 a_td[2] = {trf, G_PCM};
            CHECK(hle_call(hle_binkw32__BinkGetTrackData_8, 2, a_td) == 1920, "GetTrackData из файла");
        }
        u32 a_b[1] = {bf};
        hle_call(hle_binkw32__BinkClose_4, 1, a_b);
    }

    // 3b. Расписание кадров (BinkWait): ролик идёт по часам, отставшие кадры пропускаются. Раньше «ждать»
    // отсчитывалось от предыдущего BinkCopyToBuffer, поэтому на телефоне ролик играл медленнее реального времени.
    strcpy((char *)g_mem + G_NAME, "C:\\Shrek2\\Movies\\pacing.bik");
    u32 bp = hle_call(hle_binkw32__BinkOpen_8, 2, a_file);
    CHECK(bp != 0, "BinkOpen pacing.bik вернул 0");
    if (bp) {
        u32 a_b[1] = {bp};
        // Первый вызов заводит расписание и просит подождать: кадр 0 ещё показывается.
        CHECK(hle_call(hle_binkw32__BinkWait_4, 1, a_b) == 1, "BinkWait на старте ролика должен вернуть 1");
        CHECK(rd32(g_mem, bp + 12) == 1, "BinkWait без BinkNextFrame не должен двигать кадр до времени");
        usleep(30000);  // 30 кадров ролика (по 1 мс) — телефон «не успел»
        CHECK(hle_call(hle_binkw32__BinkWait_4, 1, a_b) == 0, "BinkWait после паузы должен вернуть 0");
        CHECK(rd32(g_mem, bp + 12) == 2, "отставшие кадры ролика должны пропускаться (FrameNum=%u)",
              rd32(g_mem, bp + 12));
        hle_call(hle_binkw32__BinkClose_4, 1, a_b);
    }

    // 4. Повреждённые/усечённые файлы и отсутствие файла
    static const char *const bad_files[] = {
        "C:\\Shrek2\\Movies\\missing.bik",
        "C:\\Shrek2\\Movies\\short.bik",
        "C:\\Shrek2\\Movies\\badmagic.bik",
        "C:\\Shrek2\\Movies\\zeroframes.bik",
        "C:\\Shrek2\\Movies\\manytracks.bik",
        "C:\\Shrek2\\Movies\\truncoffs.bik",
    };
    for (size_t i = 0; i < sizeof bad_files / sizeof bad_files[0]; i++) {
        strcpy((char *)g_mem + G_NAME, bad_files[i]);
        CHECK(hle_call(hle_binkw32__BinkOpen_8, 2, a_file) == 0, "битый файл %s должен вернуть 0", bad_files[i]);
    }

    // 5. BINKFROMMEMORY с огромным заявленным размером (> 256 МБ) и неверной сигнатурой
    memcpy(g_mem + G_BUF, "BIKi", 4);
    put_le32(g_mem + G_BUF + 4, 0x20000000u);
    CHECK(hle_call(hle_binkw32__BinkOpen_8, 2, a_open) == 0, "BINKFROMMEMORY > 256 МБ должен вернуть 0");
    memcpy(g_mem + G_BUF, "BAD!", 4);
    put_le32(g_mem + G_BUF + 4, 64);
    CHECK(hle_call(hle_binkw32__BinkOpen_8, 2, a_open) == 0, "BINKFROMMEMORY с плохой сигнатурой должен вернуть 0");

    // 6. BIKi со смещением кадра за концом файла: безопасное открытие/отказ без падения и утечек
    u8 oob_offs[1024];
    u32 oob_sz = build_valid_bik(oob_offs, 0, 0, 30, 1, 0x5000);
    put_le32(oob_offs + 56, 999999u | 1u);
    put_le32(oob_offs + 60, 1000100u);
    memcpy(g_mem + G_BUF, oob_offs, oob_sz);
    u32 boob = hle_call(hle_binkw32__BinkOpen_8, 2, a_open);
    if (boob) {
        u32 a_tr0[2] = {boob, 0};
        u32 tro = hle_call(hle_binkw32__BinkOpenTrack_8, 2, a_tr0);
        if (tro) {
            u32 a_td[2] = {tro, G_PCM};
            CHECK(hle_call(hle_binkw32__BinkGetTrackData_8, 2, a_td) == 0, "выход смещения кадра за файл");
        }
        u32 a_b[1] = {boob};
        hle_call(hle_binkw32__BinkClose_4, 1, a_b);
    }

    // 7. Безопасность при нулевых дескрипторах
    u32 z1[1] = {0}, z2[2] = {0, G_PCM}, z7[7] = {0, G_PCM, 64, 16, 0, 0, 3};
    hle_call(hle_binkw32__BinkDoFrame_4, 1, z1);
    hle_call(hle_binkw32__BinkNextFrame_4, 1, z1);
    hle_call(hle_binkw32__BinkCopyToBuffer_28, 7, z7);
    CHECK(hle_call(hle_binkw32__BinkGetTrackData_8, 2, z2) == 0, "GetTrackData(0)");
    hle_call(hle_binkw32__BinkClose_4, 1, z1);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s <tmp_dir>\n", argv[0]);
        return 2;
    }
    if (rt_mem_init() != 0) {
        printf("rt_mem_init failed\n");
        return 2;
    }
    vm_init();
    heap_init();

    test_bink_audio_api();
    test_bink_container_hle(argv[1]);

    printf("bink_test: %d failed\n", g_fails);
    return g_fails != 0;
}
