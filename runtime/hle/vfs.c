// Отображение путей гостя на хост с оверлеем для записи (см. vfs.h).
#include "runtime/hle/vfs.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static char g_game[1024], g_user[1024], g_guest[260], g_cwd[520];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void vfs_init(const char *game_root, const char *user_root, const char *guest_root)
{
    snprintf(g_game, sizeof g_game, "%s", game_root);
    snprintf(g_user, sizeof g_user, "%s", user_root);
    snprintf(g_guest, sizeof g_guest, "%s", guest_root);
    snprintf(g_cwd, sizeof g_cwd, "%s\\System", guest_root);
    mkdir(g_user, 0755);
}

// Полный путь Windows: разделители '\', без «.» и «..», буква диска заглавная.
static void normalize(const char *in, char *out, size_t cap)
{
    char tmp[1100];
    char drive = 0;
    const char *p = in;
    if (((p[0] | 0x20) >= 'a' && (p[0] | 0x20) <= 'z') && p[1] == ':') {
        drive = (char)(p[0] & ~0x20);
        p += 2;
        if (*p != '\\' && *p != '/') {  // "C:file" — относительно текущего каталога диска
            pthread_mutex_lock(&g_lock);
            snprintf(tmp, sizeof tmp, "%s\\%s", g_cwd + 2, p);
            pthread_mutex_unlock(&g_lock);
            p = NULL;
        }
    } else if (*p == '\\' || *p == '/') {
        drive = g_cwd[0];
    } else {
        pthread_mutex_lock(&g_lock);
        drive = g_cwd[0];
        snprintf(tmp, sizeof tmp, "%s\\%s", g_cwd + 2, p);
        pthread_mutex_unlock(&g_lock);
        p = NULL;
    }
    if (p)
        snprintf(tmp, sizeof tmp, "%s", p);
    // разбор компонентов
    const char *comp[128];
    size_t len[128];
    int n = 0;
    for (char *s = tmp; *s;) {
        while (*s == '\\' || *s == '/')
            s++;
        if (!*s)
            break;
        char *e = s;
        while (*e && *e != '\\' && *e != '/')
            e++;
        size_t l = (size_t)(e - s);
        if (l == 1 && s[0] == '.') {
        } else if (l == 2 && s[0] == '.' && s[1] == '.') {
            if (n)
                n--;
        } else if (n < 128) {
            comp[n] = s;
            len[n++] = l;
        }
        s = e;
    }
    size_t o = 0;
    if (cap < 4)
        return;
    out[o++] = drive ? drive : 'C';
    out[o++] = ':';
    for (int i = 0; i < n && o + len[i] + 2 < cap; i++) {
        out[o++] = '\\';
        memcpy(out + o, comp[i], len[i]);
        o += len[i];
    }
    if (o == 2)
        out[o++] = '\\';
    out[o] = 0;
}

int vfs_rel(const char *in, char *rel, size_t cap)
{
    char full[1100];
    normalize(in, full, sizeof full);
    size_t gl = strlen(g_guest);
    if (strncasecmp(full, g_guest, gl) || (full[gl] && full[gl] != '\\'))
        return 0;
    const char *r = full + gl + (full[gl] == '\\');
    size_t i = 0;
    for (; r[i] && i + 1 < cap; i++)
        rel[i] = r[i] == '\\' ? '/' : r[i];
    rel[i] = 0;
    return 1;
}

// ---------------------------------------------------------------- кеш каталогов
// Листинги каталогов хоста: на Switch каждый stat/opendir/readdir — запрос к файловому сервису, а поиск без учёта
// регистра шёл на каждый CreateFile (≈2 % потока гостя). Каталог игры не меняется; листинги оверлея
// сбрасываются при любом изменении в нём (vfs_changed).
// Тип записи берётся из readdir (d_type; devkitA64 newlib заполняет его из st_mode fsdev): на Switch stat — это
// пять-шесть запросов к сервисам (тип, метка времени с переводом через сервис времени, открытие, размер, закрытие).
typedef struct dcache {
    struct dcache *next;
    char *path;   // каталог хоста
    char *names;  // записи подряд: тип ('d', 'f', '?' — неизвестен) и имя с '\0'; NULL — каталога нет
    size_t len;
} dcache;
#define DC_BUCKETS 1024
static dcache *g_dc[DC_BUCKETS];
static pthread_mutex_t g_dc_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t dc_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static dcache *dc_load(const char *path)
{
    dcache *e = calloc(1, sizeof *e);
    e->path = strdup(path);
    DIR *d = opendir(path);
    if (!d)
        return e;
    size_t capb = 256;
    e->names = malloc(capb);
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name) + 2;
        if (e->len + l > capb) {
            capb = (e->len + l) * 2;
            e->names = realloc(e->names, capb);
        }
        e->names[e->len] = de->d_type == DT_DIR ? 'd' : de->d_type == DT_REG ? 'f' : '?';
        memcpy(e->names + e->len + 1, de->d_name, l - 1);
        e->len += l;
    }
    closedir(d);
    return e;
}

// имя в каталоге dir без учёта регистра -> настоящее имя в out и тип ('d', 'f', '?'); 0 — нет такого (или каталога)
static char dc_lookup(const char *dir, const char *name, char *out, size_t cap)
{
    uint32_t b = dc_hash(dir) % DC_BUCKETS;
    char found = 0;
    pthread_mutex_lock(&g_dc_lock);
    dcache *e = g_dc[b];
    while (e && strcmp(e->path, dir))
        e = e->next;
    if (!e) {
        e = dc_load(dir);
        e->next = g_dc[b];
        g_dc[b] = e;
    }
    for (size_t o = 0; e->names && o < e->len; o += strlen(e->names + o + 1) + 2)
        if (!strcasecmp(e->names + o + 1, name)) {
            snprintf(out, cap, "%s", e->names + o + 1);
            found = e->names[o];
            break;
        }
    pthread_mutex_unlock(&g_dc_lock);
    return found;
}

static uint32_t g_gen;  // vfs_changed

uint32_t vfs_generation(void) { return __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE); }

void vfs_changed(void)
{
    __atomic_add_fetch(&g_gen, 1, __ATOMIC_RELEASE);
    size_t ul = strlen(g_user);
    pthread_mutex_lock(&g_dc_lock);
    for (int b = 0; b < DC_BUCKETS; b++)
        for (dcache **pe = &g_dc[b]; *pe;) {
            dcache *e = *pe;
            if (!strncmp(e->path, g_user, ul) && (!e->path[ul] || e->path[ul] == '/')) {
                *pe = e->next;
                free(e->path);
                free(e->names);
                free(e);
            } else
                pe = &e->next;
        }
    pthread_mutex_unlock(&g_dc_lock);
}

// Сопоставить rel под root без учёта регистра. Возвращает число разрешённых компонентов; out — путь хоста
// (неразрешённый хвост дописан как есть). *complete = 1, если разрешён весь путь; *kind — тип последнего
// компонента ('d', 'f', '?'), если разрешён весь путь.
static int resolve_ci(const char *root, const char *rel, char *out, size_t cap, int *complete, char *kind)
{
    size_t o = (size_t)snprintf(out, cap, "%s", root);
    const char *s = rel;
    int ok = 1;
    char k = 'd';  // пустой rel — сам корень
    while (*s) {
        const char *e = strchr(s, '/');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        char name[260];
        snprintf(name, sizeof name, "%.*s", (int)l, s);
        if (!ok || !(k = dc_lookup(out, name, name, sizeof name)))
            ok = 0;
        o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, "/%s", name);
        s += l;
        if (*s == '/')
            s++;
    }
    *complete = ok;
    if (kind)
        *kind = ok ? k : 0;
    return ok;
}

int vfs_find_kind(const char *rel, char *host, size_t cap, char *kind)
{
    int complete;
    if (resolve_ci(g_user, rel, host, cap, &complete, kind) && complete)
        return 1;
    return resolve_ci(g_game, rel, host, cap, &complete, kind) && complete;
}

int vfs_find(const char *rel, char *host, size_t cap) { return vfs_find_kind(rel, host, cap, NULL); }

int vfs_find_game(const char *rel, char *host, size_t cap)
{
    int complete;
    return resolve_ci(g_game, rel, host, cap, &complete, NULL) && complete;
}

static int mkdirs(const char *path)  // каталоги на пути к файлу
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    return 0;
}

static int copy_file(const char *from, const char *to)
{
    FILE *a = fopen(from, "rb");
    if (!a)
        return -1;
    FILE *b = fopen(to, "wb");
    if (!b) {
        fclose(a);
        return -1;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        fwrite(buf, 1, n, b);
    fclose(a);
    fclose(b);
    return 0;
}

int vfs_for_write(const char *rel, char *host, size_t cap, int copy_existing)
{
    int complete;
    resolve_ci(g_user, rel, host, cap, &complete, NULL);
    int r = mkdirs(host) == 0;
    if (r && copy_existing && !complete) {
        char src[1100];
        if (resolve_ci(g_game, rel, src, sizeof src, &complete, NULL) && complete) {
            struct stat st;
            if (stat(src, &st) == 0 && S_ISREG(st.st_mode))
                copy_file(src, host);
        }
    }
    vfs_changed();
    return r;
}

const char *vfs_cwd(void) { return g_cwd; }

int vfs_set_cwd(const char *in)
{
    char full[1100], rel[1100], host[1100];
    normalize(in, full, sizeof full);
    if (vfs_rel(full, rel, sizeof rel)) {
        struct stat st;
        if (!rel[0] ? 0 : !vfs_find(rel, host, sizeof host) || stat(host, &st) != 0 || !S_ISDIR(st.st_mode))
            return 0;
    }
    pthread_mutex_lock(&g_lock);
    snprintf(g_cwd, sizeof g_cwd, "%s", full);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

void vfs_full(const char *in, char *out, size_t cap) { normalize(in, out, cap); }

static int add_unique(char **buf, size_t *len, size_t *capb, int n, const char *name)
{
    for (size_t o = 0; o < *len; o += strlen(*buf + o) + 1)
        if (!strcasecmp(*buf + o, name))
            return n;
    size_t l = strlen(name) + 1;
    if (*len + l > *capb) {
        *capb = (*len + l) * 2;
        *buf = realloc(*buf, *capb);
    }
    memcpy(*buf + *len, name, l);
    *len += l;
    return n + 1;
}

int vfs_list(const char *rel_dir, char **names)
{
    char host[1100];
    const char *roots[2] = {g_user, g_game};
    char *buf = NULL;
    size_t len = 0, capb = 0;
    int n = 0, any = 0;
    n = add_unique(&buf, &len, &capb, n, ".");
    n = add_unique(&buf, &len, &capb, n, "..");
    for (int r = 0; r < 2; r++) {
        int complete;
        if (!resolve_ci(roots[r], rel_dir, host, sizeof host, &complete, NULL) || !complete)
            continue;
        DIR *d = opendir(host);
        if (!d)
            continue;
        any = 1;
        struct dirent *de;
        while ((de = readdir(d)))
            if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
                n = add_unique(&buf, &len, &capb, n, de->d_name);
        closedir(d);
    }
    if (!any) {
        free(buf);
        return -1;
    }
    *names = buf;
    return n;
}
