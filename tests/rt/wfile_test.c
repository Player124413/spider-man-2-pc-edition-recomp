// Файлы гостя (runtime/hle/kernel32_file.c) против модели в памяти. Запись: буфер, правка накопленного на месте,
// сброс перед чтением/размером/закрытием, фоновая запись большого файла — случайные позиции (возвраты назад — как SavePackage к заголовку), записи
// мелкие и больше буфера, чтения посреди записи, размер, закрытие и повторное открытие. Перезапись существующего
// файла (ini): открытие и запись откладываются, закрытие — фоном; открытие того же пути, размер по имени, удаление
// сразу после закрытия видят новое содержимое. Чтение пакета игры: образ
// в памяти, который фоновый поток дочитывает, пока идут чтения (из образа и с диска вперемешку), и повторное
// открытие с готовым образом. Сами функции HLE — через стек гостя, как их зовёт сгенерированный код.
// Запуск: build/mac/tests/rt/wfile_test <пустой каталог>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "runtime/abi.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/vfs.h"
#include "runtime/rt.h"

void hle_kernel32_CreateFileA(CPU *c);
void hle_kernel32_WriteFile(CPU *c);
void hle_kernel32_ReadFile(CPU *c);
void hle_kernel32_SetFilePointer(CPU *c);
void hle_kernel32_GetFileSize(CPU *c);
void hle_kernel32_CloseHandle(CPU *c);
void hle_kernel32_GetFileAttributesExW(CPU *c);
void hle_kernel32_DeleteFileA(CPU *c);

#define STACK 0x00200000u  // стек вызовов (адреса гостя)
#define NAME 0x00300000u
#define IO 0x00400000u      // буфер данных гостя
#define OUT 0x00380000u     // *lpNumberOfBytes
#define MAXF (12u << 20)    // больше буфера записи (4 МБ)

static CPU cpu;
static u8 *model;
static u32 model_len;
static int fails;
static u64 rng = 0x9E3779B97F4A7C15ull;

static u32 rnd(u32 n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (u32)(rng % n);
}

static u32 call(void (*fn)(CPU *), int n, const u32 *args)
{
    cpu.esp = STACK - 4u * (u32)(n + 1);
    wr32(g_mem, cpu.esp, 0xDEADBEEFu);  // адрес возврата
    for (int i = 0; i < n; i++)
        wr32(g_mem, cpu.esp + 4u + 4u * (u32)i, args[i]);
    fn(&cpu);
    if (cpu.esp != STACK)
        printf("stack imbalance after call: %08x\n", cpu.esp), fails++;
    return cpu.eax;
}

static u32 open_file(u32 disp)
{
    u32 a[7] = {NAME, 0xC0000000u /* GENERIC_READ|WRITE */, 0, 0, disp, 0x80, 0};
    return call(hle_kernel32_CreateFileA, 7, a);
}

static void seek(u32 h, u32 pos)
{
    u32 a[4] = {h, pos, 0, 0};
    if (call(hle_kernel32_SetFilePointer, 4, a) != pos)
        printf("seek %u failed\n", pos), fails++;
}

static void write_at(u32 h, u32 pos, u32 n)
{
    seek(h, pos);
    for (u32 i = 0; i < n; i++)
        g_mem[IO + i] = (u8)rnd(256);
    u32 a[5] = {h, IO, n, OUT, 0};
    if (!call(hle_kernel32_WriteFile, 5, a) || rd32(g_mem, OUT) != n)
        printf("write %u at %u failed\n", n, pos), fails++;
    memcpy(model + pos, g_mem + IO, n);
    if (pos + n > model_len)
        model_len = pos + n;
}

static void check_read(u32 h, u32 pos, u32 n, const char *what)
{
    seek(h, pos);
    memset(g_mem + IO, 0xCD, n);
    u32 a[5] = {h, IO, n, OUT, 0};
    u32 want = pos >= model_len ? 0 : (pos + n > model_len ? model_len - pos : n);
    if (!call(hle_kernel32_ReadFile, 5, a) || rd32(g_mem, OUT) != want || memcmp(g_mem + IO, model + pos, want)) {
        if (fails++ < 10)
            printf("%s: read %u at %u: got %u bytes, want %u\n", what, n, pos, rd32(g_mem, OUT), want);
    }
}

static void check_size(u32 h)
{
    u32 a[2] = {h, 0};
    u32 sz = call(hle_kernel32_GetFileSize, 2, a);
    if (sz != model_len && fails++ < 10)
        printf("size %u, want %u\n", sz, model_len);
}

int main(int argc, char **argv)
{
    if (argc < 2 || rt_mem_init() != 0)
        return 2;
    char game[1024];
    snprintf(game, sizeof game, "%s/game", argv[1]);
    mkdir(argv[1], 0755);
    mkdir(game, 0755);
    char tex[1100];  // каталог игры неизменен (vfs кеширует его листинги): каталог пакета — заранее
    snprintf(tex, sizeof tex, "%s/Textures", game);
    mkdir(tex, 0755);
    vfs_init(game, argv[1], "C:\\Shrek2");
    strcpy((char *)g_mem + NAME, "C:\\Shrek2\\System\\Save.tmp");
    model = calloc(MAXF, 1);
    int ops = 0;
    for (int round = 0; round < 40; round++) {
        u32 h = open_file(round % 4 ? 4 /* OPEN_ALWAYS */ : 2 /* CREATE_ALWAYS */);
        if (h == 0xFFFFFFFFu) {
            printf("open failed\n");
            return 1;
        }
        if (round % 4 == 0)
            model_len = 0;
        for (int k = 0; k < 300; k++, ops++) {
            u32 kind = rnd(100), end = model_len;
            if (kind < 55) {  // дальше подряд, как FArchiveFileWriter
                u32 n = rnd(10) ? 1 + rnd(4096) : 1 + rnd(64 << 10);
                if (end + n < MAXF)
                    write_at(h, end, n);
            } else if (kind < 80) {  // назад внутрь записанного (заголовок, таблицы)
                u32 n = 1 + rnd(256);
                if (end > n)
                    write_at(h, rnd(end - n), n);
            } else if (kind < 83) {  // крупная запись, больше буфера
                u32 n = (4u << 20) + rnd(1u << 20);
                u32 pos = rnd(end + 1);
                if (pos + n < MAXF)
                    write_at(h, pos, n);
            } else if (kind < 86) {  // за конец с дырой
                u32 pos = end + 1 + rnd(5000), n = 1 + rnd(3000);
                if (pos + n < MAXF) {
                    memset(model + end, 0, pos - end);  // дыра читается нулями
                    write_at(h, pos, n);
                }
            } else if (kind < 95) {
                u32 pos = rnd(end + 100);
                check_read(h, pos, 1 + rnd(8192), "mid");
            } else {
                check_size(h);
            }
        }
        check_size(h);
        check_read(h, 0, model_len, "before close");
        u32 a[1] = {h};
        call(hle_kernel32_CloseHandle, 1, a);
        hle_file_flush_all();  // закрытие могло уйти в фон
        // с диска после закрытия
        char path[1100];
        snprintf(path, sizeof path, "%s/System/Save.tmp", argv[1]);
        FILE *f = fopen(path, "rb");
        u8 *disk = malloc(MAXF);
        size_t got = f ? fread(disk, 1, MAXF, f) : 0;
        if (f)
            fclose(f);
        if (got != model_len || memcmp(disk, model, model_len)) {
            if (fails++ < 10)
                printf("round %d: file on disk %zu bytes, want %u%s\n", round, got, model_len,
                       got == model_len ? " (content differs)" : "");
        }
        free(disk);
    }
    // как SavePackage: 2,2 МБ подряд мелкими кусками (уходят на диск фоном), затем заголовок
    // заново — внутрь уже записанного фоном
    {
        u32 h = open_file(2 /* CREATE_ALWAYS */);
        model_len = 0;
        while (model_len < (2200u << 10)) {
            write_at(h, model_len, 1 + rnd(16));
            ops++;
        }
        write_at(h, 0, 200);
        write_at(h, 300u << 10, 64);
        u32 a[1] = {h};
        call(hle_kernel32_CloseHandle, 1, a);
        hle_file_flush_all();
        char path[1100];
        snprintf(path, sizeof path, "%s/System/Save.tmp", argv[1]);
        FILE *f = fopen(path, "rb");
        u8 *disk = malloc(MAXF);
        size_t got = f ? fread(disk, 1, MAXF, f) : 0;
        if (f)
            fclose(f);
        if ((got != model_len || memcmp(disk, model, model_len)) && fails++ < 10)
            printf("save-like: file on disk %zu bytes, want %u\n", got, model_len);
        free(disk);
    }
    // перезапись ini (SaveConfig на каждой монете): только запись, CREATE_ALWAYS поверх существующего файла
    {
        char path[1100];
        snprintf(path, sizeof path, "%s/System/User.ini", argv[1]);
        FILE *pf0 = fopen(path, "wb");
        fputs("[Old]\r\nKey=1\r\n", pf0);
        fclose(pf0);
        strcpy((char *)g_mem + NAME, "C:\\Shrek2\\System\\User.ini");
        const char *wname = "C:\\Shrek2\\System\\User.ini";
        u32 wn = NAME + 0x200;
        for (u32 i = 0; i <= strlen(wname); i++)
            wr16(g_mem, wn + 2 * i, (u8)wname[i]);
        u32 hold = 0xFFFFFFFFu;  // открытый на запись, пока читается
        for (int round = 0; round < 300; round++, ops++) {
            u32 aw[7] = {NAME, 0x40000000u /* GENERIC_WRITE */, 0, 0, 2 /* CREATE_ALWAYS */, 0x80, 0};
            u32 h = call(hle_kernel32_CreateFileA, 7, aw);
            if (h == 0xFFFFFFFFu) {
                printf("ini round %d: rewrite open failed\n", round);
                return 1;
            }
            model_len = 0;
            u32 kind = rnd(10);
            if (kind == 0) {  // читатель при открытом писателе: CREATE_ALWAYS уже усёк файл
                u32 ar[7] = {NAME, 0x80000000u, 1, 0, 3 /* OPEN_EXISTING */, 0x80, 0};
                hold = call(hle_kernel32_CreateFileA, 7, ar);
                check_read(hold, 0, 100, "ini while writer open");
                u32 a1[1] = {hold};
                call(hle_kernel32_CloseHandle, 1, a1);
            }
            if (kind != 1)  // 1 — пустая перезапись
                for (u32 left = 1 + rnd(12000); left;) {  // кусками, как FArchiveFileWriter
                    u32 n = left < 4096 ? left : 4096;
                    write_at(h, model_len, n);
                    left -= n;
                }
            s64 known;
            if (hle_file_size_known(wn, &known) && fails++ < 10)  // писатель открыт: размер неизвестен
                printf("ini round %d: size known with a writer open\n", round);
            u32 a[1] = {h};
            call(hle_kernel32_CloseHandle, 1, a);
            // записан с усечения через этот слой: размер без открытия (FileSize, overrides_launch.c), запись может идти фоном
            if ((!hle_file_size_known(wn, &known) || known != model_len) && fails++ < 10)
                printf("ini round %d: known size %lld, want %u\n", round, (long long)known, model_len);
            if (kind < 5) {  // сразу открыть заново и прочитать
                u32 ar[7] = {NAME, 0x80000000u, 1, 0, 3, 0x80, 0};
                u32 r = call(hle_kernel32_CreateFileA, 7, ar);
                check_size(r);
                check_read(r, 0, model_len + 10, "ini reopened");
                u32 a1[1] = {r};
                call(hle_kernel32_CloseHandle, 1, a1);
            } else if (kind < 7) {  // размер по имени
                u32 ax[3] = {wn, 0, IO};
                if (!call(hle_kernel32_GetFileAttributesExW, 3, ax) || rd32(g_mem, IO + 32) != model_len)
                    if (fails++ < 10)
                        printf("ini round %d: size by name %u, want %u\n", round, rd32(g_mem, IO + 32), model_len);
            }
        }
        hle_file_flush_all();
        // кеш размеров: после изменения оверлея — неизвестно; нет файла и каталог — -1 по листингу
        s64 known;
        vfs_changed();
        if (hle_file_size_known(wn, &known) && fails++ < 10)
            printf("ini: size known after vfs_changed\n");
        const char *names[] = {"C:\\Shrek2\\System\\NoSuch.ini", "C:\\Shrek2\\SYSTEM", "D:\\Other\\User.ini"};
        for (int i = 0; i < 3; i++) {
            u32 w = NAME + 0x400;
            for (u32 j = 0; j <= strlen(names[i]); j++)
                wr16(g_mem, w + 2 * j, (u8)names[i][j]);
            if ((!hle_file_size_known(w, &known) || known != -1) && fails++ < 10)
                printf("ini: %s: want -1 without opening\n", names[i]);
        }
        FILE *f = fopen(path, "rb");
        u8 *disk = malloc(MAXF);
        size_t got = f ? fread(disk, 1, MAXF, f) : 0;
        if (f)
            fclose(f);
        if ((got != model_len || memcmp(disk, model, model_len)) && fails++ < 10)
            printf("ini: file on disk %zu bytes, want %u\n", got, model_len);
        free(disk);
        // удаление сразу после фонового закрытия
        u32 aw[7] = {NAME, 0x40000000u, 0, 0, 2, 0x80, 0};
        u32 h = call(hle_kernel32_CreateFileA, 7, aw);
        write_at(h, 0, 5000);
        u32 a[1] = {h};
        call(hle_kernel32_CloseHandle, 1, a);
        u32 ad[1] = {NAME};
        if (!call(hle_kernel32_DeleteFileA, 1, ad) && fails++ < 10)
            printf("ini: delete after close failed\n");
        u32 ar[7] = {NAME, 0x80000000u, 1, 0, 3, 0x80, 0};
        if (call(hle_kernel32_CreateFileA, 7, ar) != 0xFFFFFFFFu && fails++ < 10)
            printf("ini: file still there after delete\n");
        hle_file_flush_all();
        if (!access(path, F_OK) && fails++ < 10)
            printf("ini: file on disk after delete\n");
    }
    // чтение пакета из каталога игры: образ в памяти
    char pkg[1100];
    snprintf(pkg, sizeof pkg, "%s/Textures", game);
    mkdir(pkg, 0755);
    snprintf(pkg, sizeof pkg, "%s/Textures/Big.utx", game);
    model_len = (7u << 20) + 12345;
    for (u32 i = 0; i < model_len; i++)
        model[i] = (u8)rnd(256);
    FILE *pf = fopen(pkg, "wb");
    fwrite(model, 1, model_len, pf);
    fclose(pf);
    strcpy((char *)g_mem + NAME, "C:\\Shrek2\\Textures\\Big.utx");
    for (int round = 0; round < 3; round++) {
        if (round == 1)
            usleep(1500000);  // затишье: поток подкачки дочитывает образ
        unsigned long long reads0 = g_hitch.reads;
        u32 a7[7] = {NAME, 0x80000000u /* GENERIC_READ */, 1, 0, 3 /* OPEN_EXISTING */, 0x80, 0};
        u32 h = call(hle_kernel32_CreateFileA, 7, a7);
        if (h == 0xFFFFFFFFu) {
            printf("open package failed\n");
            return 1;
        }
        for (int k = 0; k < 3000; k++, ops++) {
            u32 n = rnd(4) ? 1 + rnd(2048) : 1 + rnd(300 << 10);
            check_read(h, rnd(model_len + 1000), n, "package");
        }
        check_read(h, 0, model_len, "package whole");
        check_size(h);  // размер — lseek в конец (fd_size), позиция чтения после него та же
        u32 ae[4] = {h, 0, 0, 2 /* FILE_END */};
        if (call(hle_kernel32_SetFilePointer, 4, ae) != model_len && fails++ < 10)
            printf("package: FILE_END seek, want %u\n", model_len);
        check_read(h, 1000, 5000, "package after size");
        u32 a[1] = {h};
        call(hle_kernel32_CloseHandle, 1, a);
        const char *cm = getenv("SNX_FILE_CACHE_MB");  // 0 — без образов: проверяются только данные
        if (round && !(cm && !atoi(cm)) && g_hitch.reads != reads0 && fails++ < 10)  // образ готов — с диска ничего
            printf("package round %d: %llu disk reads with a ready image\n", round, g_hitch.reads - reads0);
    }
    // регрессия ошибки отложенного открытия: wb_flush провалился -> WriteFile должен вернуть 0 без записи за пределы wbuf
    {
        char fpath[1100];
        snprintf(fpath, sizeof fpath, "%s/System/FailLazy.sav", argv[1]);
        FILE *fp = fopen(fpath, "wb");
        if (fp) {
            fputs("seed", fp);
            fclose(fp);
        }
        vfs_changed();
        strcpy((char *)g_mem + NAME, "C:\\Shrek2\\System\\FailLazy.sav");
        u32 aw[7] = {NAME, 0x40000000u /* GENERIC_WRITE */, 0, 0, 2 /* CREATE_ALWAYS */, 0x80, 0};
        u32 h = call(hle_kernel32_CreateFileA, 7, aw);
        if (h == 0xFFFFFFFFu) {
            printf("fail-lazy: deferred open handle failed\n");
            fails++;
        } else {
            // пишем со смещением > 0 в wbuf (lazy == 1), затем подменяем файл каталогом — f_open провалится
            seek(h, 4096);
            u32 w1[5] = {h, IO, 1024, OUT, 0};
            if (!call(hle_kernel32_WriteFile, 5, w1) || rd32(g_mem, OUT) != 1024)
                printf("fail-lazy: initial buffered write failed\n"), fails++;
            unlink(fpath);
            mkdir(fpath, 0755);
            // возврат назад (pos < wpos): раньше wb_flush возвращал 0, не сбрасывал wlen, а at уходил в переполнение
            seek(h, 0);
            wr32(g_mem, OUT, 0xCAFEu);
            u32 w2[5] = {h, IO, 32, OUT, 0};
            if (call(hle_kernel32_WriteFile, 5, w2) != 0 || rd32(g_mem, OUT) != 0)
                printf("fail-lazy: write after seek-back must fail cleanly (out=%u)\n", rd32(g_mem, OUT)), fails++;
            // повторная запись в сломанный дескриптор (> WBUF и < WBUF) также должна вернуть 0
            u32 w3[5] = {h, IO, (5u << 20), OUT, 0};
            if (call(hle_kernel32_WriteFile, 5, w3) != 0 || rd32(g_mem, OUT) != 0)
                printf("fail-lazy: large write after failed open must fail\n"), fails++;
            u32 ac[1] = {h};
            call(hle_kernel32_CloseHandle, 1, ac);
            hle_file_flush_all();
            rmdir(fpath);
            vfs_changed();
            ops += 3;
        }
        // второй сценарий: провал wb_flush при записи > WBUF в отложенный файл
        snprintf(fpath, sizeof fpath, "%s/System/FailLazy2.sav", argv[1]);
        fp = fopen(fpath, "wb");
        if (fp) {
            fputs("seed2", fp);
            fclose(fp);
        }
        vfs_changed();
        strcpy((char *)g_mem + NAME, "C:\\Shrek2\\System\\FailLazy2.sav");
        h = call(hle_kernel32_CreateFileA, 7, aw);
        if (h != 0xFFFFFFFFu) {
            u32 w1[5] = {h, IO, 512, OUT, 0};
            call(hle_kernel32_WriteFile, 5, w1);
            unlink(fpath);
            mkdir(fpath, 0755);
            u32 wbig[5] = {h, IO, (5u << 20), OUT, 0};
            if (call(hle_kernel32_WriteFile, 5, wbig) != 0 || rd32(g_mem, OUT) != 0)
                printf("fail-lazy2: >WBUF write after failed open must fail\n"), fails++;
            u32 ac[1] = {h};
            call(hle_kernel32_CloseHandle, 1, ac);
            hle_file_flush_all();
            rmdir(fpath);
            vfs_changed();
            ops += 2;
        }
        // валидный сценарий: запись с позиции >= 4 МБ, затем возврат назад (pos < wpos) и запись > WBUF
        strcpy((char *)g_mem + NAME, "C:\\Shrek2\\System\\Save.tmp");
        h = open_file(2 /* CREATE_ALWAYS */);
        if (h != 0xFFFFFFFFu) {
            model_len = 0;
            memset(model, 0, 5u << 20);
            write_at(h, 5u << 20, 4096);
            write_at(h, 0, 64);
            write_at(h, 1024, (5u << 20) + 123);
            check_size(h);
            check_read(h, 0, model_len, "seekback+large");
            u32 ac[1] = {h};
            call(hle_kernel32_CloseHandle, 1, ac);
            hle_file_flush_all();
            ops += 5;
        }
    }
    // кеширование read-only файла локализации (.int) в оверлее + инвалидация при записи и удалении
    {
        char ovp[1100];
        snprintf(ovp, sizeof ovp, "%s/System/TestDialog.int", argv[1]);
        u32 len1 = 300u << 10;
        for (u32 i = 0; i < len1; i++)
            model[i] = (u8)rnd(256);
        model_len = len1;
        FILE *fp = fopen(ovp, "wb");
        if (fp) {
            fwrite(model, 1, len1, fp);
            fclose(fp);
        }
        vfs_changed();
        strcpy((char *)g_mem + NAME, "C:\\Shrek2\\System\\TestDialog.int");
        const char *cm = getenv("SNX_FILE_CACHE_MB");
        int cache_on = !(cm && !atoi(cm));
        // 1) первый проход открывает read-only и запускает фоновую подкачку fimage
        u32 ar[7] = {NAME, 0x80000000u /* GENERIC_READ */, 1, 0, 3 /* OPEN_EXISTING */, 0x80, 0};
        u32 h = call(hle_kernel32_CreateFileA, 7, ar);
        if (h == 0xFFFFFFFFu) {
            printf("overlay .int: initial open failed\n");
            fails++;
        } else {
            check_size(h);
            check_read(h, 0, len1, "overlay .int first");
            u32 ac[1] = {h};
            call(hle_kernel32_CloseHandle, 1, ac);
            usleep(600000);  // затишье: fimg_worker дочитывает 300 КБ в RAM
            unsigned long long reads0 = g_hitch.reads;
            h = call(hle_kernel32_CreateFileA, 7, ar);
            if (h == 0xFFFFFFFFu) {
                printf("overlay .int: cached open failed\n");
                fails++;
            } else {
                check_size(h);
                check_read(h, 0, len1, "overlay .int cached");
                check_read(h, 4096, 8192, "overlay .int cached mid");
                u32 ac2[1] = {h};
                call(hle_kernel32_CloseHandle, 1, ac2);
                if (cache_on && g_hitch.reads != reads0 && fails++ < 10)
                    printf("overlay .int: %llu disk reads with a ready image\n", g_hitch.reads - reads0);
            }
            // 2) перезапись через отложенный путь (GENERIC_WRITE + CREATE_ALWAYS) обязана инвалидировать fimage
            u32 aw[7] = {NAME, 0x40000000u /* GENERIC_WRITE */, 0, 0, 2 /* CREATE_ALWAYS */, 0x80, 0};
            u32 hw = call(hle_kernel32_CreateFileA, 7, aw);
            if (hw == 0xFFFFFFFFu) {
                printf("overlay .int: rewrite open failed\n");
                fails++;
            } else {
                model_len = 0;
                write_at(hw, 0, 4096);
                write_at(hw, 4096, 12345);
                u32 acw[1] = {hw};
                call(hle_kernel32_CloseHandle, 1, acw);
                u32 hr = call(hle_kernel32_CreateFileA, 7, ar);
                if (hr == 0xFFFFFFFFu) {
                    printf("overlay .int: reopen after rewrite failed\n");
                    fails++;
                } else {
                    check_size(hr);
                    check_read(hr, 0, model_len, "overlay .int after rewrite");
                    u32 acr[1] = {hr};
                    call(hle_kernel32_CloseHandle, 1, acr);
                }
            }
            // 3) перезапись через GENERIC_READ|WRITE и последующее удаление + новое содержимое
            u32 hrw = open_file(2 /* CREATE_ALWAYS */);
            if (hrw != 0xFFFFFFFFu) {
                model_len = 0;
                write_at(hrw, 0, 7777);
                u32 acrw[1] = {hrw};
                call(hle_kernel32_CloseHandle, 1, acrw);
                hle_file_flush_all();
                u32 hr2 = call(hle_kernel32_CreateFileA, 7, ar);
                if (hr2 != 0xFFFFFFFFu) {
                    check_size(hr2);
                    check_read(hr2, 0, model_len, "overlay .int after rw");
                    u32 acr2[1] = {hr2};
                    call(hle_kernel32_CloseHandle, 1, acr2);
                } else
                    fails++;
            } else
                fails++;
            u32 ad[1] = {NAME};
            if (!call(hle_kernel32_DeleteFileA, 1, ad) && fails++ < 10)
                printf("overlay .int: delete failed\n");
            if (call(hle_kernel32_CreateFileA, 7, ar) != 0xFFFFFFFFu && fails++ < 10)
                printf("overlay .int: still openable after delete\n");
            ops += 8;
        }
    }
    // тип записи — из листинга каталога (vfs.c, без stat): каталог открыть нельзя (open(O_RDONLY) каталога на
    // POSIX прошёл бы), имя без учёта регистра. GetLastError без потока гостя не хранится.
    strcpy((char *)g_mem + NAME, "C:\\Shrek2\\textures");
    u32 ad7[7] = {NAME, 0x80000000u, 1, 0, 3, 0x80, 0};
    if (call(hle_kernel32_CreateFileA, 7, ad7) != 0xFFFFFFFFu && fails++ < 10)
        printf("directory: open must fail\n");
    printf("wfile_test: %d operations, %d failed\n", ops, fails);
    return fails != 0;
}
