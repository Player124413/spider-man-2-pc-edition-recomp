// Проверка декодера звука Bink: bink_dec <файл> [смещение BIK в файле] [дорожка] > pcm.s16le
// Пишет PCM дорожки (s16, каналы с чередованием); в stderr — формат и сумма заявленных размеров PCM кадров
// против декодированного. Эталон — ffmpeg: tests/bink/compare.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/hle/bink.h"

static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.bik [offset] [track] > out.s16le\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return perror(argv[1]), 1;
    long off = argc > 2 ? strtol(argv[2], NULL, 0) : 0;
    u32 track = argc > 3 ? (u32)atoi(argv[3]) : 0;
    u8 hdr[8];
    fseek(f, off, SEEK_SET);
    if (fread(hdr, 1, 8, f) != 8 || memcmp(hdr, "BIK", 3))
        return fprintf(stderr, "not Bink\n"), 1;
    u32 size = le32(hdr + 4) + 8;
    u8 *d = malloc(size);
    fseek(f, off, SEEK_SET);
    if (fread(d, 1, size, f) != size)
        return fprintf(stderr, "short file\n"), 1;
    fclose(f);
    u32 frames = le32(d + 8), ntracks = le32(d + 40);
    if (track >= ntracks)
        return fprintf(stderr, "no track %u (%u tracks)\n", track, ntracks), 1;
    u32 o = 44 + ntracks * 4 + track * 4;
    u32 rate = (u32)d[o] | (u32)d[o + 1] << 8, flags = (u32)d[o + 2] | (u32)d[o + 3] << 8;
    u32 ch = flags & 0x2000 ? 2 : 1;
    const u8 *offs = d + 44 + ntracks * 12;
    fprintf(stderr, "rev %c, %u frames, %u Hz x%u, flags %04x\n", d[3], frames, rate, ch, flags);
    bink_audio *a = bink_audio_new(rate, ch, (flags & 0x1000) != 0, d[3]);
    if (!a)
        return 1;
    s16 *buf = malloc(1 << 22);
    u64 reported = 0, decoded = 0;
    for (u32 i = 0; i < frames; i++) {
        u32 p = le32(offs + 4 * i) & ~1u;
        for (u32 t = 0; t < ntracks; t++) {
            u32 n = le32(d + p);
            if (t == track && n >= 4) {
                reported += le32(d + p + 4);
                u32 got = bink_audio_decode(a, d + p + 8, n - 4, buf, 1 << 21);
                decoded += got * 2;
                fwrite(buf, 2, got, stdout);
                if (getenv("BINK_CUM"))
                    fprintf(stderr, "f%u rep %llu dec %llu\n", i, (unsigned long long)reported, (unsigned long long)decoded);
            }
            p += 4 + n;
        }
    }
    fprintf(stderr, "PCM bytes: reported %llu, decoded %llu\n", (unsigned long long)reported,
            (unsigned long long)decoded);
    bink_audio_free(a);
    return 0;
}
