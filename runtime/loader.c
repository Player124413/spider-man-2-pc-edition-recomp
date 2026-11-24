// Загрузка PE-модулей игры в гостевую память по фиксированной карте адресов.
#include "runtime/loader.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "runtime/rt.h"

static u32 crc32_buf(const u8 *p, size_t n)
{
    static u32 table[256];
    if (!table[1]) {
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static u32 g_next_thunk = RT_THUNK_BASE;

u32 rt_hle_thunk(const char *dll, guest_fn fn)
{
    (void)dll;
    u32 a = g_next_thunk;
    g_next_thunk += RT_THUNK_SIZE;
    memset(g_mem + a, 0xCC, RT_THUNK_SIZE);
    rt_register_function(a, fn);
    return a;
}

// файл целиком; NULL — с errno (EIO — ошибка чтения)
static u8 *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = n >= 0 ? malloc(n ? (size_t)n : 1) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (!buf)
        errno = n < 0 ? EIO : errno ? errno : EIO;
    *size = n > 0 ? (size_t)n : 0;
    return buf;
}

// dir/name; нет такого — то же имя без учёта регистра (Game.exe с ПК, раздел телефона различает регистр)
static u8 *read_module_file(const char *dir, const char *name, char *path, size_t cap, size_t *size)
{
    snprintf(path, cap, "%s/%s", dir, name);
    u8 *f = read_file(path, size);
    if (f || errno != ENOENT)
        return f;
    DIR *d = opendir(dir);
    if (!d) {
        errno = ENOENT;
        return NULL;
    }
    struct dirent *e;
    while ((e = readdir(d)))
        if (!strcasecmp(e->d_name, name) && strcmp(e->d_name, name)) {
            snprintf(path, cap, "%s/%s", dir, e->d_name);
            closedir(d);
            return read_file(path, size);
        }
    closedir(d);
    errno = ENOENT;
    return NULL;
}

static int module_matches(const rt_module_desc *d, const u8 *file, size_t size)
{
    return size == d->file_size && crc32_buf(file, size) == d->crc32;
}

// Копия модулей во внутренней памяти приложения (Android: SNX_MODULE_DIR = files/modules/<версия>, лаунчер).
// На ZTE Blade 20 Smart (Android 9) игра «через некоторое время» перестала запускаться: System/game.exe в папке
// игры (Android/data/…, доступна USB, файловым менеджерам, «очистке» и антивирусам — .exe с ПК им кажется
// мусором или вирусом) пропал. Внутреннюю память другие приложения не видят: после удачной загрузки модуля из
// System порт кладёт туда его копию (запись во временный файл и rename), а без файла в System грузит копию.
static void module_backup(const rt_module_desc *d, const u8 *file, size_t size, const char *bdir)
{
    char dst[1100], tmp[1110];
    snprintf(dst, sizeof dst, "%s/%s", bdir, d->name);
    FILE *f = fopen(dst, "rb");
    if (f) {  // уже есть того же размера — CRC проверится при загрузке копии
        long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
        fclose(f);
        if (n == (long)size)
            return;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "[loader] cannot back up %s to %s: %s\n", d->name, bdir, strerror(errno));
        return;
    }
    int ok = fwrite(file, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    if (ok && rename(tmp, dst) == 0)
        fprintf(stderr, "[loader] %s: backup copy saved to %s\n", d->name, bdir);
    else {
        fprintf(stderr, "[loader] cannot back up %s to %s: %s\n", d->name, bdir, strerror(errno));
        remove(tmp);
    }
}

static u32 le32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u16 le16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }

int rt_load_module(const rt_module_desc *d, const char *dir)
{
    char path[1100];
    size_t size = 0;
    const char *bdir = getenv("SNX_MODULE_DIR");
    if (bdir && !*bdir)
        bdir = NULL;
    u8 *file = read_module_file(dir, d->name, path, sizeof path, &size);
    int err = file ? 0 : errno;
    if (file && module_matches(d, file, size)) {
        if (bdir)
            module_backup(d, file, size, bdir);
    } else {
        if (file)
            fprintf(stderr, "[loader] %s: file differs from the one used for recompilation (size %zu/%u, crc %08x/%08x)\n",
                    d->name, size, d->file_size, crc32_buf(file, size), d->crc32);
        else
            fprintf(stderr, "[loader] cannot read %s: %s\n", path, strerror(err));
        free(file);
        file = NULL;
        if (bdir) {
            char bpath[1100];
            size_t bsize = 0;
            u8 *b = read_module_file(bdir, d->name, bpath, sizeof bpath, &bsize);
            if (b && module_matches(d, b, bsize)) {
                fprintf(stderr, "[loader] %s: using the backup copy %s\n", d->name, bpath);
                file = b;
                size = bsize;
            } else {
                fprintf(stderr, "[loader] %s: no valid backup copy in %s%s\n", d->name, bdir,
                        b ? " (differs)" : "");
                free(b);
            }
        }
        if (!file)
            return -1;
    }
    u32 pe = le32(file + 0x3C);
    u16 nsec = le16(file + pe + 6);
    u16 optsz = le16(file + pe + 20);
    const u8 *opt = file + pe + 24;
    u32 pref = le32(opt + 28);
    u32 hdrsize = le32(opt + 60);
    u32 reloc_rva = le32(opt + 96 + 5 * 8);
    u32 reloc_size = le32(opt + 96 + 5 * 8 + 4);
    u8 *img = g_mem + d->base;
    memset(img, 0, d->size);
    memcpy(img, file, hdrsize);
    const u8 *sec = opt + optsz;
    for (u16 i = 0; i < nsec; i++, sec += 40) {
        u32 vsize = le32(sec + 8), va = le32(sec + 12), rawsize = le32(sec + 16), rawptr = le32(sec + 20);
        u32 n = rawsize < vsize || !vsize ? rawsize : vsize;
        if (va + n > d->size)
            n = d->size - va;
        if (rawptr + n > size)
            n = (u32)size - rawptr;
        memcpy(img + va, file + rawptr, n);
    }
    free(file);
    u32 delta = d->base - pref;
    if (delta && reloc_size) {
        u32 off = 0;
        while (off + 8 <= reloc_size) {
            u32 page = le32(img + reloc_rva + off), bsize = le32(img + reloc_rva + off + 4);
            if (bsize < 8)
                break;
            for (u32 k = 8; k < bsize; k += 2) {
                u16 e = le16(img + reloc_rva + off + k);
                if ((e >> 12) == 3) {
                    u8 *p = img + page + (e & 0xFFF);
                    memcpy(p, &(u32){le32(p) + delta}, 4);
                } else if ((e >> 12) != 0) {
                    fprintf(stderr, "[loader] %s: unsupported reloc type %u\n", d->name, e >> 12);
                    return -1;
                }
            }
            off += bsize;
        }
    } else if (delta) {
        fprintf(stderr, "[loader] %s: needs rebase but has no relocations\n", d->name);
        return -1;
    }
    for (u32 i = 0; i < d->niat; i++) {
        const rt_iat_entry *e = &d->iat[i];
        if (!e->iat_va)
            continue;
        u32 v = e->guest_target ? e->guest_target : rt_hle_thunk(e->dll, e->hle);
        wr32(g_mem, e->iat_va, v);
    }
    rt_register_functions(d->funcs, d->nfuncs);
    return 0;
}
