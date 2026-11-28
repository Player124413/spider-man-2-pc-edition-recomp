// Загрузчик модулей (runtime/loader.c): копия модулей во внутренней памяти (SNX_MODULE_DIR) и имя без учёта
// регистра. На ZTE Blade 20 Smart System/game.exe из папки игры через некоторое время пропадал (очистка или
// антивирус) — игра больше не запускалась: «[loader] cannot read …/System/game.exe».
// Запуск: loader_test <пустой рабочий каталог>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "runtime/loader.h"
#include "runtime/rt.h"

static int g_fail;
#define CHECK(c, ...)                                         \
    do {                                                      \
        if (!(c)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                     \
            fprintf(stderr, "\n");                            \
            g_fail = 1;                                       \
        }                                                     \
    } while (0)

static u32 crc32_buf(const u8 *p, size_t n)
{
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

enum { BASE = 0x00400000u, PE_SIZE = 0x400 };
static u8 g_pe[PE_SIZE];

// минимальный PE: заголовки без секций, база = BASE (без релокаций), метка в конце файла
static void make_pe(void)
{
    memset(g_pe, 0, sizeof g_pe);
    g_pe[0] = 'M', g_pe[1] = 'Z';
    u32 pe = 0x40;
    memcpy(g_pe + 0x3C, &pe, 4);
    memcpy(g_pe + pe, "PE\0\0", 4);
    u16 nsec = 0, optsz = 224;
    memcpy(g_pe + pe + 6, &nsec, 2);
    memcpy(g_pe + pe + 20, &optsz, 2);
    u8 *opt = g_pe + pe + 24;
    u32 base = BASE, hdr = 0x200;
    memcpy(opt + 28, &base, 4);
    memcpy(opt + 60, &hdr, 4);
    memcpy(g_pe + 0x100, "shrekNX loader test", 19);
}

static void put(const char *path, const u8 *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        exit(2);
    }
    fclose(f);
}

static long fsize(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: loader_test <dir>\n");
        return 2;
    }
    char sys[1024], bak[1024], exe[1100], up[1100], bexe[1100];
    snprintf(sys, sizeof sys, "%s/System", argv[1]);
    snprintf(bak, sizeof bak, "%s/modules", argv[1]);
    mkdir(argv[1], 0755);
    mkdir(sys, 0755);
    mkdir(bak, 0755);
    snprintf(exe, sizeof exe, "%s/game.exe", sys);
    snprintf(up, sizeof up, "%s/GAME.EXE", sys);
    snprintf(bexe, sizeof bexe, "%s/game.exe", bak);
    remove(exe), remove(up), remove(bexe);

    g_mem = calloc(1, BASE + 0x10000);
    make_pe();
    rt_module_desc d = {"game.exe", BASE, BASE, 0x1000, crc32_buf(g_pe, PE_SIZE), PE_SIZE, 0, 0, NULL, 0, NULL, 0,
                        NULL, 0, 0};

    // 1. файл в System — грузится, копия сохраняется
    put(exe, g_pe, PE_SIZE);
    setenv("SNX_MODULE_DIR", bak, 1);
    CHECK(rt_load_module(&d, sys) == 0, "load from System");
    CHECK(!memcmp(g_mem + BASE + 0x100, "shrekNX loader test", 19), "image header copied");
    CHECK(fsize(bexe) == PE_SIZE, "backup saved (%ld)", fsize(bexe));

    // 2. файл из System пропал — копия
    remove(exe);
    memset(g_mem + BASE, 0, 0x1000);
    CHECK(rt_load_module(&d, sys) == 0, "load from backup when System/game.exe is missing");
    CHECK(!memcmp(g_mem + BASE + 0x100, "shrekNX loader test", 19), "image from backup");

    // 3. файл в System испорчен — копия
    u8 bad[PE_SIZE];
    memcpy(bad, g_pe, PE_SIZE);
    bad[0x300] ^= 0xFF;
    put(exe, bad, PE_SIZE);
    CHECK(rt_load_module(&d, sys) == 0, "load from backup when System/game.exe differs");

    // 4. без копии: другое написание имени (GAME.EXE) находится
    remove(exe);
    remove(bexe);
    unsetenv("SNX_MODULE_DIR");
    put(up, g_pe, PE_SIZE);
    CHECK(rt_load_module(&d, sys) == 0, "case-insensitive name");

    // 5. нет ни файла, ни копии — ошибка
    remove(up);
    setenv("SNX_MODULE_DIR", bak, 1);
    CHECK(rt_load_module(&d, sys) != 0, "no file and no backup must fail");

    remove(bexe);
    free(g_mem);
    g_mem = NULL;
    if (g_fail)
        return 1;
    printf("loader_test: ok\n");
    return 0;
}
