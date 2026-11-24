// HLE kernel32: файлы, каталоги, поиск, проекции файлов, ресурсы PE. Пути — через vfs (оверлей для записи).
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/platform.h"
#include "runtime/heap.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/vfs.h"
#include "runtime/vmem.h"

// Позиция файла гостя — своя (pos), каждое обращение к хосту — со своего смещения. Мелкие чтения идут через
// общий кеш блоков (ниже): на Switch каждый read — запрос к файловому сервису, а движок читает пакеты кусками
// по сотне байт и прыгает между таблицами пакета и объектами.
typedef struct rblock rblock;
typedef struct fimage fimage;
typedef struct hfile_s {
    int fd;
    int std;  // 1 — stdin/stdout/stderr хоста (FILE_TYPE_CHAR)
    char path[1024];
    s64 pos;
    rblock *last;  // блок последнего чтения (под g_rb_lock)
    u32 nblk;      // блоков файла в кеше
    u8 *wbuf;      // отложенная запись (wb_flush): wlen байт с позиции wpos
    s64 wpos;
    u32 wlen;
    u32 wsent;     // начало буфера, отданное фоновой записи (ws_post)
    u32 dirty[16][2];  // изменённое в отданном после этого: [от, до) в буфере
    int ndirty;
    int wjob, werr;  // идёт фоновая запись; она не удалась (под g_ws_lock)
    u64 wstreamed;   // байт записано фоном с открытия (для строки в лог)
    struct hfile_s *wprev, *wnext;  // файлы с буфером записи или отложенным открытием (g_wb_files)
    int listed;                     // в g_wb_files
    int lazy;      // открытие отложено (f_open): перезапись существующего файла оверлея
    int closing;   // закрыт гостем, запись и закрытие — в фоне (wc_worker; под g_wb_lock)
    struct hfile_s *cnext;          // очередь фонового закрытия
    fimage *img;    // образ файла в памяти (пакет игры, fimg_open), NULL — нет
    s64 wend;       // конец записанного с открытия (WriteFile): размер файла, открытого с усечением
    u8 trunc;       // открыт с усечением (CREATE_ALWAYS, TRUNCATE_EXISTING, отложенное открытие)
    u8 size_bad;    // размер не выводится из записей (SetEndOfFile, проекция)
    u8 sz_writer;   // учтён писателем в кеше размеров (sz_*)
} hfile;

// ---------------------------------------------------------------- размеры записанных файлов
// FFileManagerWindows::FileSize (game.exe; runtime/overrides_launch.c) перед каждой записью файла открывает его на
// чтение ради размера: на Switch открытие — запросы OpenFile и GetFileTimeStamp (≈1,5 мс на монете бонусного
// уровня). Размер файла, записанного здесь же с усечения и закрытого, известен точно — конец записанного. Он годен,
// пока к пути не открыт писатель и в оверлее ничего не создавалось, не удалялось и не переносилось (vfs_generation).
#define SZ_N 32
static struct {
    char path[1024];
    s64 size;
    u32 gen;
    int writers, known;
} g_sz[SZ_N];
static pthread_mutex_t g_sz_lock = PTHREAD_MUTEX_INITIALIZER;

static int sz_find(const char *path, int create)  // под g_sz_lock; -1 — нет (таблица занята писателями)
{
    int free_i = -1;
    for (int i = 0; i < SZ_N; i++) {
        if (g_sz[i].path[0] && !strcmp(g_sz[i].path, path))
            return i;
        if (!g_sz[i].writers && (free_i < 0 || !g_sz[i].path[0] || (g_sz[free_i].path[0] && !g_sz[i].known)))
            free_i = i;
    }
    if (!create || free_i < 0)
        return -1;
    snprintf(g_sz[free_i].path, sizeof g_sz[free_i].path, "%s", path);
    g_sz[free_i].writers = g_sz[free_i].known = 0;
    return free_i;
}

static void sz_open_writer(hfile *f)
{
    pthread_mutex_lock(&g_sz_lock);
    int i = sz_find(f->path, 1);
    if (i >= 0) {
        g_sz[i].writers++;
        g_sz[i].known = 0;
        f->sz_writer = 1;
    }
    pthread_mutex_unlock(&g_sz_lock);
}

static void sz_close_writer(hfile *f)
{
    if (!f->sz_writer)
        return;
    pthread_mutex_lock(&g_sz_lock);
    int i = sz_find(f->path, 0);
    if (i >= 0 && --g_sz[i].writers <= 0) {
        g_sz[i].writers = 0;
        g_sz[i].known = f->trunc && !f->size_bad;
        g_sz[i].size = f->wend;
        g_sz[i].gen = vfs_generation();
    }
    pthread_mutex_unlock(&g_sz_lock);
    f->sz_writer = 0;
}

// Размер по имени без открытия, как его отдал бы CreateFile на чтение + GetFileSize (-1 — не открылся бы): нет пути,
// каталог, файл, записанный этим слоем. 0 — неизвестно (открыть по-настоящему).
int hle_file_size_known(u32 wname, s64 *size)
{
    char name[1100], rel[1100], host[1100];
    hle_w2utf8(wname, name, sizeof name);
    char kind = 0;
    if (!vfs_rel(name, rel, sizeof rel) || !vfs_find_kind(rel, host, sizeof host, &kind) || kind == 'd') {
        *size = -1;
        return 1;
    }
    if (kind != 'f')
        return 0;
    int ok = 0;
    pthread_mutex_lock(&g_sz_lock);
    int i = sz_find(host, 0);
    if (i >= 0 && g_sz[i].known && !g_sz[i].writers && g_sz[i].gen == vfs_generation()) {
        *size = g_sz[i].size;
        ok = 1;
    }
    pthread_mutex_unlock(&g_sz_lock);
    return ok;
}

// Отложенная запись: WriteFile копятся в буфере файла и уходят на диск одним куском перед любым другим обращением
// к нему (чтение, размер, усечение, проекция, закрытие, FlushFileBuffers), а также при выходе и ловушке
// (hle_file_flush_all). На Switch каждый write — запрос к файловому сервису, а FArchiveFileWriter движка пишет по
// 4 КБ: сохранение игры стояло ≈2,4 с в fsFileWrite. Запись внутрь накопленного (SavePackage возвращается к
// заголовку и таблицам) правит буфер на месте; в буфер влезает сохранение целиком (2,2 МБ) — раньше оно уходило
// кусками, по ≈70 мс на каждый возврат (tests/rt/wfile_test).
#define WBUF (4u << 20)
static hfile *g_wb_files;
static pthread_mutex_t g_wb_lock = PTHREAD_MUTEX_INITIALIZER;

void hle_file_flush_all(void);

static void wb_list(hfile *f)  // под g_wb_lock
{
    static int once;
    if (!once)  // позже atexit(exit_process) из main: идёт раньше него
        once = atexit(hle_file_flush_all) == 0;
    if (f->listed)
        return;
    f->listed = 1;
    f->wprev = NULL;
    f->wnext = g_wb_files;
    if (g_wb_files)
        g_wb_files->wprev = f;
    g_wb_files = f;
}

static void wb_unlist(hfile *f)  // под g_wb_lock
{
    if (!f->listed)
        return;
    f->listed = 0;
    if (f->wprev)
        f->wprev->wnext = f->wnext;
    else
        g_wb_files = f->wnext;
    if (f->wnext)
        f->wnext->wprev = f->wprev;
}

// Перезапись существующего файла оверлея только на запись (CREATE_ALWAYS: ini, лог) открывается при первой
// надобности: на Switch открытие на запись — это mkdir по всему пути, сброс кеша каталогов, создание и усечение,
// десяток запросов к файловому сервису. Записанное до закрытия обычно уходит на диск вместе с открытием в фоне.
static int f_open(hfile *f)
{
    if (!f->lazy) {
        if (f->std || f->fd >= 0)
            return 1;
        errno = EIO;
        return 0;
    }
    f->lazy = 0;
    f->fd = open(f->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f->fd < 0) {
        int err = errno;
        fprintf(stderr, "[file] deferred open of %s failed: %s\n", f->path, strerror(err));
        errno = err;
    }
    return f->fd >= 0;
}

static s64 fimg_size(const fimage *im);

// Размер файла: lseek в конец — один запрос GetSize к fsdev; fstat на Switch тянет ещё тип и метки времени (с
// переводом через сервис времени). Позицию fd чтения и записи ставят сами (lseek перед read/write).
static s64 fd_size(hfile *f)
{
    s64 isz = fimg_size(f->img);
    if (isz >= 0)
        return isz;
    struct stat st;
    if (f->std || f->fd < 0)
        return f->fd >= 0 && fstat(f->fd, &st) == 0 ? (s64)st.st_size : -1;
    return (s64)lseek(f->fd, 0, SEEK_END);
}

static int write_range(int fd, const u8 *p, s64 off, u32 n)
{
    if (fd < 0) {
        errno = EBADF;
        return 0;
    }
    if (lseek(fd, (off_t)off, SEEK_SET) < 0)
        return 0;
    for (u32 done = 0; done < n;) {
        ssize_t r = write(fd, p + done, n - done);
        if (r <= 0) {
            if (r == 0)
                errno = EIO;
            return 0;
        }
        done += (u32)r;
    }
    return 1;
}

// Фоновая запись: накопилось WS_CHUNK (128 КБ) сверх отданного — оно уходит на диск в отдельном потоке, пока гость пишет
// дальше (SavePackage сериализует уровень ≈0,6 с на Switch, а запись 2,1 МБ на SD — ещё ≈90 мс в том же кадре).
// Запись гостя внутрь отданного правит буфер и помечает изменённое (dirty) — wb_flush пишет его заново вместе с
// неотданным хвостом. SavePackage возвращается назад ≈8 раз: по 4 байта (длины ленивых массивов) и заголовок. Фоновая запись — одна на всех; пока она идёт, с дескриптором файла ничего не делается: все
// обращения к нему идут через wb_flush, а тот её дожидается. Мелкие файлы (ini, лог) до порога не дорастают.
#define WS_CHUNK (128u << 10)
static pthread_mutex_t g_ws_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ws_cv = PTHREAD_COND_INITIALIZER;
static hfile *g_ws_file;  // фоновая запись: файл, ...
static int g_ws_fd;
static const u8 *g_ws_src;  // ... откуда, куда и сколько
static s64 g_ws_off;
static u32 g_ws_len;
static int g_ws_thread;  // 1 — поток есть, -1 — не создался

static void *ws_worker(void *arg)
{
    (void)arg;
    rt_platform_thread_background();
    pthread_mutex_lock(&g_ws_lock);
    for (;;) {
        while (!g_ws_file)
            pthread_cond_wait(&g_ws_cv, &g_ws_lock);
        int fd = g_ws_fd;
        const u8 *src = g_ws_src;
        s64 off = g_ws_off;
        u32 n = g_ws_len;
        pthread_mutex_unlock(&g_ws_lock);
        int ok = write_range(fd, src, off, n);
        pthread_mutex_lock(&g_ws_lock);
        hfile *f = g_ws_file;
        g_ws_file = NULL;
        f->wjob = 0;
        f->werr |= !ok;
        f->wstreamed += ok ? n : 0;
        pthread_cond_broadcast(&g_ws_cv);
    }
    return NULL;
}

// отдать фону накопленное сверх wsent, если он свободен (иначе — в следующий раз или в wb_flush)
static void ws_post(hfile *f)
{
    pthread_mutex_lock(&g_ws_lock);
    if (!g_ws_thread) {
        pthread_t t;
        g_ws_thread = pthread_create(&t, NULL, ws_worker, NULL) == 0 ? 1 : -1;
        if (g_ws_thread > 0)
            pthread_detach(t);
    }
    if (g_ws_thread > 0 && !g_ws_file) {
        g_ws_file = f;
        g_ws_fd = f->fd;
        g_ws_src = f->wbuf + f->wsent;
        g_ws_off = f->wpos + f->wsent;
        g_ws_len = f->wlen - f->wsent;
        f->wsent = f->wlen;
        f->wjob = 1;
        pthread_cond_broadcast(&g_ws_cv);
    }
    pthread_mutex_unlock(&g_ws_lock);
}

// изменено [lo, hi) отданного: к соседнему диапазону (ближе 4 КБ) или новым; мест нет — к ближайшему
static void ws_dirty(hfile *f, u32 lo, u32 hi)
{
    int best = -1;
    u32 bd = ~0u;
    for (int i = 0; i < f->ndirty; i++) {
        u32 *d = f->dirty[i];
        u32 gap = hi < d[0] ? d[0] - hi : lo > d[1] ? lo - d[1] : 0;
        if (gap < bd)
            bd = gap, best = i;
    }
    if (best < 0 || (bd >= 4096 && f->ndirty < 16)) {
        f->dirty[f->ndirty][0] = lo;
        f->dirty[f->ndirty++][1] = hi;
        return;
    }
    u32 *d = f->dirty[best];
    d[0] = lo < d[0] ? lo : d[0];
    d[1] = hi > d[1] ? hi : d[1];
}

// Вызывается под g_wb_lock (или из wc_worker для файла с closing == 1, который уже никому не доступен)
static int wb_flush_locked(hfile *f)
{
    if (f->lazy && !f_open(f)) {
        int err = errno;
        f->wlen = f->wsent = 0;
        f->ndirty = 0;
        errno = err;
        return 0;
    }
    if (!f->wsent && !f->wlen)
        return 1;
    pthread_mutex_lock(&g_ws_lock);
    while (f->wjob)
        pthread_cond_wait(&g_ws_cv, &g_ws_lock);
    int ok = !f->werr;
    f->werr = 0;
    pthread_mutex_unlock(&g_ws_lock);
    u32 n = f->wlen, from = f->wsent, redo = 0;
    int err = ok ? 0 : EIO;
    u64 t0 = rt_ticks_ns();
    for (int i = 0; i < f->ndirty; i++) {
        u32 *d = f->dirty[i];
        if (!write_range(f->fd, f->wbuf + d[0], f->wpos + d[0], d[1] - d[0])) {
            ok = 0;
            if (!err)
                err = errno;
        }
        redo += d[1] - d[0];
    }
    f->wlen = f->wsent = 0;
    f->ndirty = 0;
    if (n > from) {
        if (!write_range(f->fd, f->wbuf + from, f->wpos + from, n - from)) {
            ok = 0;
            if (!err)
                err = errno;
        }
    }
    u64 dt = rt_ticks_ns() - t0;
    if (dt > 50000000)  // запись сохранения на Switch: ≈0,7 с на 2,2 МБ — где именно
        fprintf(stderr, "[file] slow write: %u KB at %lld to %s, %.0f ms\n", (n - from + redo) >> 10,
                (long long)(f->wpos + from), f->path, (double)dt / 1e6);
    if (!ok) {
        fprintf(stderr, "[file] deferred write of %u bytes to %s failed\n", n, f->path);
        errno = err ? err : EIO;
        return 0;
    }
    return 1;
}

static int wb_flush(hfile *f)
{
    pthread_mutex_lock(&g_wb_lock);
    int ok = wb_flush_locked(f);
    pthread_mutex_unlock(&g_wb_lock);
    return ok;
}

// Фоновое закрытие: файл, который не писался фоном по ходу (ini, лог — не сохранение), гость закрывает сразу, а
// открытие (если отложено), запись буфера и закрытие идут в отдельном потоке. До конца файл остаётся в g_wb_files
// с closing: обращения по имени (открытие того же пути, размер, удаление, перенос, поиск) и выход его дожидаются.
// SaveConfig переписывает User.ini на каждой монете бонусного уровня.
static pthread_cond_t g_wc_cv = PTHREAD_COND_INITIALIZER;    // есть работа
static pthread_cond_t g_wc_done = PTHREAD_COND_INITIALIZER;  // файл закрыт
static hfile *g_wc_head, *g_wc_tail;
static int g_wc_thread;  // 1 — поток есть, -1 — не создался
static void rb_drop(hfile *f);

static void *wc_worker(void *arg)
{
    (void)arg;
    rt_platform_thread_background();
    pthread_mutex_lock(&g_wb_lock);
    for (;;) {
        while (!g_wc_head)
            pthread_cond_wait(&g_wc_cv, &g_wb_lock);
        hfile *f = g_wc_head;
        if (!(g_wc_head = f->cnext))
            g_wc_tail = NULL;
        pthread_mutex_unlock(&g_wb_lock);
        u32 n = f->wlen;
        u64 t0 = rt_ticks_ns();
        wb_flush_locked(f);
        if (f->fd >= 0)
            close(f->fd);
        double ms = (double)(rt_ticks_ns() - t0) / 1e6;
        if (ms >= 50 || getenv("SNX_LOG_FILES"))  // User.ini на SD Switch — 15–20 мс на каждой монете
            fprintf(stderr, "[file] %s: %u KB written at close in background, %.0f ms\n", f->path, n >> 10, ms);
        rb_drop(f);
        free(f->wbuf);
        pthread_mutex_lock(&g_wb_lock);
        wb_unlist(f);
        free(f);
        pthread_cond_broadcast(&g_wc_done);
    }
    return NULL;
}

static int wc_post(hfile *f)
{
    pthread_mutex_lock(&g_wb_lock);
    if (!g_wc_thread) {
        pthread_t t;
        g_wc_thread = pthread_create(&t, NULL, wc_worker, NULL) == 0 ? 1 : -1;
        if (g_wc_thread > 0)
            pthread_detach(t);
    }
    int ok = g_wc_thread > 0;
    if (ok) {
        wb_list(f);
        f->closing = 1;
        f->cnext = NULL;
        if (g_wc_tail)
            g_wc_tail->cnext = f;
        else
            g_wc_head = f;
        g_wc_tail = f;
        pthread_cond_signal(&g_wc_cv);
    }
    pthread_mutex_unlock(&g_wb_lock);
    return ok;
}

// host == NULL — все файлы; закрываемые фоном — дождаться
static void flush_files(const char *host)
{
    u64 t0 = 0;
    pthread_mutex_lock(&g_wb_lock);
    for (;;) {
        int pending = 0;
        for (hfile *f = g_wb_files; f; f = f->wnext)
            if (!host || !strcmp(f->path, host)) {
                if (f->closing)
                    pending = 1;
                else if (!host || f->wlen || f->lazy)
                    wb_flush_locked(f);
            }
        if (!pending)
            break;
        if (!t0)
            t0 = rt_ticks_ns();
        pthread_cond_wait(&g_wc_done, &g_wb_lock);
    }
    pthread_mutex_unlock(&g_wb_lock);
    double ms = t0 ? (double)(rt_ticks_ns() - t0) / 1e6 : 0;
    if (host && (ms >= 1 || (t0 && getenv("SNX_LOG_FILES"))))  // гость ждал фоновое закрытие этого файла
        fprintf(stderr, "[file] %s: waited %.1f ms for background write\n", host, ms);
}

void hle_file_flush_all(void) { flush_files(NULL); }

// Только файлы с этим путём хоста: открытию по имени нужно то, что записано в него самого. Открытия идут и посреди
// игры (Localize читает .int), и сброс всех буферов писал на SD накопленный лог движка — до 20 мс на открытие
static void flush_path(const char *host) { flush_files(host); }

static _Atomic u64 g_fg_read_ns;  // время последнего чтения гостя с диска (образы пакетов ниже)

// n байт с позиции off (до конца файла); -1 — ошибка
static ssize_t read_at(hfile *f, s64 off, u8 *dst, size_t n)
{
    if (!wb_flush(f))
        return -1;
    static int log = -1;
    if (log < 0)
        log = getenv("SNX_LOG_READS") != NULL;
    if (log)  // поток чтений хоста: tools/probe/reads.py
        fprintf(stderr, "[read] %s %lld %zu\n", f->path, (long long)off, n);
    if (lseek(f->fd, (off_t)off, SEEK_SET) < 0)
        return -1;
    u64 t0 = rt_ticks_ns();
    atomic_store(&g_fg_read_ns, t0);  // поток подкачки образов уступает диск
    size_t got = 0;
    int err = 0;
    while (got < n) {
        ssize_t r = read(f->fd, dst + got, n - got);
        if (r <= 0) {
            err = r < 0 && !got;
            break;
        }
        got += (size_t)r;
    }
    g_hitch.reads++;  // строка [hitch] (d3d8_present)
    g_hitch.read_bytes += got;
    g_hitch.read_ns += rt_ticks_ns() - t0;
    return err ? -1 : (ssize_t)got;
}

// Кеш чтения: блоки по 64 КБ, выровненные в файле, общие на все файлы, вытеснение — давно не нужный. С одним
// буфером на файл до первого кадра читалось 1,3 ГБ ради 83 МБ данных (Engine.u — 431 МБ при размере 1,6 МБ):
// пакет читается из двух-трёх мест вперемешку, и буфер каждый раз заливался заново.
#define RBLK (64u << 10)
#define RBLK_N 384         // 24 МБ на все файлы
#define RBLK_PER_FILE 32   // потоковое чтение большого файла (текстуры, звук) вытесняет только свои блоки
struct rblock {
    hfile *f;  // NULL — свободен
    s64 idx;   // номер блока в файле
    u32 len;   // меньше RBLK — конец файла
    u64 used;  // часы последнего обращения
    u8 *data;
};
static rblock g_rb[RBLK_N];
static u64 g_rb_clock;
static pthread_mutex_t g_rb_lock = PTHREAD_MUTEX_INITIALIZER;

static void rb_free(rblock *b)
{
    b->f->nblk--;
    if (b->f->last == b)
        b->f->last = NULL;
    b->f = NULL;
}

// блок idx файла f (залит с диска при промахе); NULL — ошибка чтения или конец файла
static rblock *rb_get(hfile *f, s64 idx)
{
    rblock *b = f->last;
    if (!b || b->idx != idx) {
        rblock *free_b = NULL, *lru = NULL, *own_lru = NULL;
        b = NULL;
        for (int i = 0; i < RBLK_N && !b; i++) {
            rblock *e = &g_rb[i];
            if (e->f == f && e->idx == idx)
                b = e;
            else if (!e->f)
                free_b = free_b ? free_b : e;
            else {
                if (!lru || e->used < lru->used)
                    lru = e;
                if (e->f == f && (!own_lru || e->used < own_lru->used))
                    own_lru = e;
            }
        }
        if (!b) {
            b = f->nblk >= RBLK_PER_FILE ? own_lru : free_b ? free_b : lru;
            if (b->f)
                rb_free(b);
            if (!b->data && !(b->data = malloc(RBLK)))
                return NULL;
            ssize_t r = read_at(f, idx * RBLK, b->data, RBLK);
            if (r <= 0)
                return NULL;
            b->f = f;
            b->idx = idx;
            b->len = (u32)r;
            f->nblk++;
        }
        f->last = b;
    }
    b->used = ++g_rb_clock;
    return b;
}

// записи и закрытие: блоки файла больше не верны
static void rb_drop(hfile *f)
{
    pthread_mutex_lock(&g_rb_lock);
    for (int i = 0; i < RBLK_N && f->nblk; i++)
        if (g_rb[i].f == f)
            rb_free(&g_rb[i]);
    pthread_mutex_unlock(&g_rb_lock);
}

// ---------------------------------------------------------------- образы пакетов в памяти
// Открытый на чтение файл из каталога игры (пакеты: текстуры, меши, звуки, карта) и read-only файлы
// локализации (.int/.rus, в т.ч. пропатченные в оверлее BumpDialog.int / HpDialog.int) фоновый поток
// дочитывает в память целиком, с начала; чтение гостя из уже прочитанного — копия из памяти. Движок
// подгружает данные пакетов по требованию (TLazyArray: мипы текстур, реплики из AllDialog.uax 111 МБ),
// и на Switch это были фризы посреди игры (строка [hitch]: 0,2–3,5 МБ с SD за 8–260 мс). Поток уступает
// диск: после чтения гостя ждёт затишья. При записи/удалении/переносе в оверлей образ немедленно
// инвалидируется (fimg_invalidate), а поток подкачки закрывает fd до модификации файла (требование fs Switch).
// Бюджет — SNX_FILE_CACHE_MB (по умолчанию 640 МБ: ~1 ГБ окно + ~650 МБ прочее + 640 МБ кеш ≈ 2,3 ГБ < 3,1 ГБ
// в полном режиме Switch; 0 — без образов).
struct fimage {
    fimage *next;
    char path[1024];
    s64 size;
    u8 *data;
    _Atomic s64 loaded;  // байт с начала уже в памяти
    _Atomic int stale;   // инвалидирован записью/удалением: не использовать, освободить при refs==0 && busy==0
    int refs;            // открытых hfile (под g_img_lock)
    int busy;            // поток подкачки читает (под g_img_lock)
    int failed;          // дочитать не вышло: в памяти только начало
    u64 used;            // часы последнего открытия/закрытия (вытеснение)
};
#define FIMG_MIN (256u << 10)        // мелкие пакеты — блочный кеш (для .int/.rus порог 1 Б)
#define FIMG_MAX (128u << 20)        // включает AllDialog.uax (111 МБ реплик)
#define FIMG_OVERLAY_MAX (16u << 20) // верхняя граница для read-only файлов локализации из оверлея
#define FIMG_CHUNK (256u << 10)      // чтение гостя ждёт фоновое не дольше одного куска (≈5 мс)
static fimage *g_imgs;
static u64 g_img_total, g_img_budget, g_img_clock;
static pthread_mutex_t g_img_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_img_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_img_done = PTHREAD_COND_INITIALIZER;
static int g_img_thread;

static s64 fimg_size(const fimage *im)
{
    return (im && !atomic_load_explicit(&im->stale, memory_order_acquire)) ? im->size : -1;
}

static int is_loc_ext(const char *rel)
{
    size_t n = strlen(rel);
    return n >= 4 && (!strcasecmp(rel + n - 4, ".int") || !strcasecmp(rel + n - 4, ".rus"));
}

static int has_open_writer(const char *host)
{
    int found = 0;
    pthread_mutex_lock(&g_wb_lock);
    for (hfile *f = g_wb_files; f; f = f->wnext)
        if (!strcasecmp(f->path, host)) {
            found = 1;
            break;
        }
    pthread_mutex_unlock(&g_wb_lock);
    return found;
}

// Удалить и освободить конкретный образ из g_imgs (под g_img_lock; refs == 0 && busy == 0)
static void fimg_unlink_free(fimage *target)
{
    for (fimage **p = &g_imgs; *p; p = &(*p)->next) {
        if (*p == target) {
            *p = target->next;
            g_img_total -= (u64)target->size;
            free(target->data);
            free(target);
            return;
        }
    }
}

// Инвалидировать образы пути host перед записью/усечением/удалением/переносом: помечает stale и
// дожидается завершения фонового чтения (чтобы fd на Switch fs был закрыт до модификации файла).
static void fimg_invalidate(const char *host)
{
    if (!host || !host[0])
        return;
    pthread_mutex_lock(&g_img_lock);
    for (;;) {
        int busy = 0;
        for (fimage **p = &g_imgs; *p;) {
            fimage *im = *p;
            if (!strcasecmp(im->path, host)) {
                atomic_store_explicit(&im->stale, 1, memory_order_release);
                im->failed = 1;
                if (im->busy) {
                    busy = 1;
                    p = &(*p)->next;
                } else if (!im->refs) {
                    *p = im->next;
                    g_img_total -= (u64)im->size;
                    free(im->data);
                    free(im);
                } else {
                    p = &(*p)->next;
                }
            } else {
                p = &(*p)->next;
            }
        }
        if (!busy)
            break;
        pthread_cond_wait(&g_img_done, &g_img_lock);
    }
    pthread_mutex_unlock(&g_img_lock);
}

static fimage *fimg_next_work(void)
{
    for (fimage *i = g_imgs; i; i = i->next)
        if (atomic_load(&i->loaded) < i->size && !i->failed &&
            !atomic_load_explicit(&i->stale, memory_order_acquire))
            return i;
    return NULL;
}

static void *fimg_worker(void *arg)
{
    (void)arg;
    rt_platform_thread_background();
    pthread_mutex_lock(&g_img_lock);
    for (;;) {
        fimage *im = fimg_next_work();
        if (!im) {
            static u64 reported;
            if (reported != g_img_total) {  // очередь опустела: что в памяти
                int nf = 0;
                for (fimage *i = g_imgs; i; i = i->next)
                    nf++;
                fprintf(stderr, "[file] packages in memory: %d files, %llu MB\n", nf,
                        (unsigned long long)(g_img_total >> 20));
                reported = g_img_total;
            }
            pthread_cond_wait(&g_img_cv, &g_img_lock);
            continue;
        }
        im->busy = 1;
        pthread_mutex_unlock(&g_img_lock);
        int fd = open(im->path, O_RDONLY);
        s64 at = atomic_load(&im->loaded);
        if (fd >= 0 && lseek(fd, (off_t)at, SEEK_SET) < 0) {
            close(fd);
            fd = -1;
        }
        while (fd >= 0 && at < im->size && !atomic_load_explicit(&im->stale, memory_order_acquire)) {
            // гость читал с диска недавно (загрузка) — подождать затишья, но не тормозить инвалидацию
            while (!atomic_load_explicit(&im->stale, memory_order_acquire) &&
                   rt_ticks_ns() - atomic_load(&g_fg_read_ns) < 300000000ull)
                usleep(10000);
            if (atomic_load_explicit(&im->stale, memory_order_acquire))
                break;
            u32 n = im->size - at < FIMG_CHUNK ? (u32)(im->size - at) : FIMG_CHUNK;
            ssize_t r = read(fd, im->data + at, n);
            if (r <= 0)
                break;
            at += r;
            atomic_store_explicit(&im->loaded, at, memory_order_release);
        }
        if (fd >= 0)
            close(fd);
        pthread_mutex_lock(&g_img_lock);
        im->busy = 0;
        int stale = atomic_load_explicit(&im->stale, memory_order_relaxed);
        im->failed = at < im->size || stale;  // в памяти остаётся прочитанное начало
        if (stale && !im->refs)
            fimg_unlink_free(im);
        pthread_cond_broadcast(&g_img_done);
    }
    return NULL;
}

// образ для только что открытого на чтение файла (под g_img_lock); NULL — без образа
static fimage *fimg_open(const char *host, s64 size, s64 min_size, s64 max_size)
{
    if (!g_img_budget) {
        const char *e = getenv("SNX_FILE_CACHE_MB");
        g_img_budget = (u64)(e ? atoi(e) : 640) << 20;
        if (!g_img_budget)
            g_img_budget = 1;  // выключено
    }
    if (size < min_size || size > max_size || (u64)size > g_img_budget)
        return NULL;
    fimage *im = NULL;
    for (fimage *i = g_imgs; i && !im; i = i->next)
        if (!atomic_load_explicit(&i->stale, memory_order_acquire) && i->size == size && !strcasecmp(i->path, host))
            im = i;
    if (!im) {
        while (g_img_total + (u64)size > g_img_budget) {  // вытеснить давно закрытый
            fimage **lru = NULL;
            for (fimage **p = &g_imgs; *p; p = &(*p)->next)
                if (!(*p)->refs && !(*p)->busy && (!lru || (*p)->used < (*lru)->used))
                    lru = p;
            if (!lru)
                return NULL;
            fimage *v = *lru;
            *lru = v->next;
            g_img_total -= (u64)v->size;
            free(v->data);
            free(v);
        }
        im = calloc(1, sizeof *im);
        if (!im || !(im->data = malloc((size_t)size))) {
            free(im);
            return NULL;
        }
        snprintf(im->path, sizeof im->path, "%s", host);
        im->size = size;
        im->next = g_imgs;
        g_imgs = im;
        g_img_total += (u64)size;
        if (!g_img_thread) {
            pthread_t t;
            g_img_thread = pthread_create(&t, NULL, fimg_worker, NULL) == 0 ? 1 : -1;
            if (g_img_thread > 0)
                pthread_detach(t);
        }
        pthread_cond_signal(&g_img_cv);
    }
    im->refs++;
    im->used = ++g_img_clock;
    return im;
}

static u32 buffered_read(hfile *f, u8 *dst, u32 n)
{
    fimage *im = f->img;
    if (im && !atomic_load_explicit(&im->stale, memory_order_acquire) && f->pos >= 0) {  // из образа, если эта часть уже в памяти
        s64 end = f->pos + n > im->size ? im->size : f->pos + n;
        if (end <= atomic_load_explicit(&im->loaded, memory_order_acquire)) {
            u32 k = end > f->pos ? (u32)(end - f->pos) : 0;
            memcpy(dst, im->data + f->pos, k);
            f->pos += k;
            return k;
        }
    }
    if (f->wlen || f->lazy)
        if (!wb_flush(f))
            return 0;
    if (n >= RBLK) {  // крупное — мимо кеша
        ssize_t r = read_at(f, f->pos, dst, n);
        if (r <= 0)
            return 0;
        f->pos += r;
        return (u32)r;
    }
    u32 got = 0;
    pthread_mutex_lock(&g_rb_lock);
    while (got < n) {
        rblock *b = rb_get(f, f->pos / RBLK);
        u32 o = (u32)(f->pos % RBLK);
        if (!b || o >= b->len)
            break;  // конец файла
        u32 k = b->len - o < n - got ? b->len - o : n - got;
        memcpy(dst + got, b->data + o, k);
        got += k;
        f->pos += k;
    }
    pthread_mutex_unlock(&g_rb_lock);
    return got;
}

static u32 g_std[3];  // описатели STD_INPUT/OUTPUT/ERROR

static void close_file_(hfile *f);

static void close_file(void *p)
{
    u64 t0 = rt_ticks_ns();
    close_file_(p);
    __atomic_add_fetch(&rt_file_ns, rt_ticks_ns() - t0, __ATOMIC_RELAXED);
}

static void close_file_(hfile *f)
{
    sz_close_writer(f);  // размер известен сразу: запись может ещё идти в фоне
    // не писался фоном по ходу (не сохранение): запись буфера и закрытие — в фоне
    if (!f->std && (f->wbuf || f->lazy) && !f->wsent && !f->wjob && !f->wstreamed && !f->img && wc_post(f))
        return;
    if (f->listed) {
        pthread_mutex_lock(&g_wb_lock);
        u32 tail = f->wlen - f->wsent;
        for (int i = 0; i < f->ndirty; i++)
            tail += f->dirty[i][1] - f->dirty[i][0];
        u64 t0 = rt_ticks_ns();
        wb_flush_locked(f);
        if (f->wstreamed)  // большой файл (сохранение): сколько ушло фоном и чего стоило закрытие
            fprintf(stderr, "[file] %s: %llu KB written in background, %u KB at close, %.0f ms\n", f->path,
                    (unsigned long long)(f->wstreamed >> 10), tail >> 10, (double)(rt_ticks_ns() - t0) / 1e6);
        wb_unlist(f);
        pthread_mutex_unlock(&g_wb_lock);
        free(f->wbuf);
    } else {
        f_open(f);  // отложенное открытие: CREATE_ALWAYS усекает файл и без записи
    }
    if (!f->std && f->fd >= 0)
        close(f->fd);
    rb_drop(f);
    if (f->img) {
        pthread_mutex_lock(&g_img_lock);
        f->img->refs--;
        f->img->used = ++g_img_clock;
        if (atomic_load_explicit(&f->img->stale, memory_order_relaxed) && !f->img->refs && !f->img->busy)
            fimg_unlink_free(f->img);
        pthread_mutex_unlock(&g_img_lock);
    }
    free(f);
}

static u32 std_handle(int i)
{
    if (!g_std[i]) {
        hfile *f = calloc(1, sizeof *f);
        f->fd = i;
        f->std = 1;
        g_std[i] = hle_obj_new(OBJ_FILE, f);
    }
    return g_std[i];
}

static u32 errno_to_win(int err)
{
    switch (err) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EISDIR: return ERROR_ACCESS_DENIED;
    case EEXIST: return 80;  // ERROR_FILE_EXISTS
    default: return 31;      // ERROR_GEN_FAILURE
    }
}

static u32 last_errno_to_win(void) { return errno_to_win(errno); }

static u64 to_filetime(time_t t) { return (u64)t * 10000000ull + 116444736000000000ull; }

// Имя гостя (A — ANSI cp1252 ≈ Latin-1, W — UTF-16) -> UTF-8
static void name_a(u32 a, char *out, size_t cap)
{
    size_t o = 0;
    for (const u8 *p = (const u8 *)hle_cstr(a); *p && o + 3 < cap; p++) {
        if (*p < 0x80) {
            out[o++] = (char)*p;
        } else {
            out[o++] = (char)(0xC0 | *p >> 6);
            out[o++] = (char)(0x80 | (*p & 0x3F));
        }
    }
    out[o] = 0;
}

static void name_of(u32 a, int wide, char *out, size_t cap)
{
    if (wide)
        hle_w2utf8(a, out, cap);
    else
        name_a(a, out, cap);
}

// UTF-8 -> ANSI гостя (Latin-1, остальное '?')
static u32 put_ansi(u32 dst, u32 cap, const char *s)
{
    u32 n = 0;
    const u8 *p = (const u8 *)s;
    while (*p && n + 1 < cap) {
        u32 ch = *p++;
        if (ch >= 0xC0) {
            int k = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1;
            ch &= 0x3F >> k;
            while (k-- && (*p & 0xC0) == 0x80)
                ch = ch << 6 | (*p++ & 0x3F);
        }
        g_mem[dst + n++] = ch < 0x100 ? (u8)ch : '?';
    }
    if (cap)
        g_mem[dst + n] = 0;
    return n;
}

static u32 put_name(u32 dst, u32 cap, const char *s, int wide) { return wide ? hle_put_w(dst, cap, s) : put_ansi(dst, cap, s); }

// ================================================================ CreateFile и ввод-вывод
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
enum { CREATE_NEW = 1, CREATE_ALWAYS, OPEN_EXISTING, OPEN_ALWAYS, TRUNCATE_EXISTING };

static u32 create_file_(const char *name, u32 access, u32 disp);

static u32 create_file(const char *name, u32 access, u32 disp)
{
    u64 t0 = rt_ticks_ns();
    u32 h = create_file_(name, access, disp);
    __atomic_add_fetch(&rt_file_ns, rt_ticks_ns() - t0, __ATOMIC_RELAXED);
    return h;
}

static u32 create_file_(const char *name, u32 access, u32 disp)
{
    char rel[1100], host[1100];
    if (!vfs_rel(name, rel, sizeof rel)) {
        if (getenv("SNX_LOG_FILES"))
            fprintf(stderr, "[file] outside game root: %s\n", name);
        rt_set_last_error(ERROR_PATH_NOT_FOUND);
        return 0xFFFFFFFFu;
    }
    char kind = 0;  // тип по листингу каталога (vfs.c): stat на Switch — пять-шесть запросов к сервисам
    int exists = vfs_find_kind(rel, host, sizeof host, &kind);
    if (exists)
        flush_path(host);  // с тем, что ещё в буферах открытых файлов
    struct stat st;
    if (exists && kind == '?')
        kind = stat(host, &st) != 0 ? 0 : S_ISDIR(st.st_mode) ? 'd' : S_ISREG(st.st_mode) ? 'f' : '?';
    int is_file = exists && kind == 'f';
    if (exists && kind == 'd') {
        rt_set_last_error(ERROR_ACCESS_DENIED);
        return 0xFFFFFFFFu;
    }
    int write = (access & GENERIC_WRITE) != 0;
    char game[1100];
    if (is_file && disp == CREATE_ALWAYS && write && !(access & GENERIC_READ) &&
        !(vfs_find_game(rel, game, sizeof game) && !strcmp(game, host))) {  // перезапись файла оверлея: f_open
        fimg_invalidate(host);
        hfile *f = calloc(1, sizeof *f);
        f->fd = -1;
        f->lazy = 1;
        f->trunc = 1;
        snprintf(f->path, sizeof f->path, "%s", host);
        sz_open_writer(f);
        pthread_mutex_lock(&g_wb_lock);
        wb_list(f);
        pthread_mutex_unlock(&g_wb_lock);
        if (getenv("SNX_LOG_FILES"))
            fprintf(stderr, "[file] open %s -> %s (w, deferred)\n", name, host);
        u32 h = hle_obj_new(OBJ_FILE, f);
        hle_obj_set_closer(h, close_file);
        rt_set_last_error(ERROR_ALREADY_EXISTS);
        return h;
    }
    int flags;
    if (disp == CREATE_NEW && exists) {
        rt_set_last_error(80);
        return 0xFFFFFFFFu;
    }
    if ((disp == OPEN_EXISTING || disp == TRUNCATE_EXISTING) && !exists) {
        rt_set_last_error(ERROR_FILE_NOT_FOUND);
        return 0xFFFFFFFFu;
    }
    if (write || disp == CREATE_ALWAYS || disp == CREATE_NEW || (disp == OPEN_ALWAYS && !exists)) {
        int keep = disp == OPEN_EXISTING || disp == OPEN_ALWAYS;
        if (!vfs_for_write(rel, host, sizeof host, keep)) {
            rt_set_last_error(ERROR_PATH_NOT_FOUND);
            return 0xFFFFFFFFu;
        }
        fimg_invalidate(host);
        flags = (write ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY) | O_CREAT;
        if (disp == CREATE_ALWAYS || disp == TRUNCATE_EXISTING)
            flags |= O_TRUNC;
    } else {
        flags = O_RDONLY;
    }
    int fd = open(host, flags, 0644);
    if (fd < 0) {
        rt_set_last_error(last_errno_to_win());
        return 0xFFFFFFFFu;
    }
    if (flags & O_CREAT)
        vfs_changed();
    if (getenv("SNX_LOG_FILES"))
        fprintf(stderr, "[file] open %s -> %s (%s)\n", name, host, write ? "rw" : "r");
    hfile *f = calloc(1, sizeof *f);
    f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", host);
    if (flags != O_RDONLY) {
        f->trunc = (flags & O_TRUNC) != 0;
        sz_open_writer(f);
    }
    if (flags == O_RDONLY && is_file) {
        int in_game = vfs_find_game(rel, game, sizeof game) && !strcmp(game, host);
        int is_loc = is_loc_ext(rel);
        // пакет игры (включая AllDialog.uax 111 МБ) или read-only файл локализации (.int/.rus) в оверлее
        if ((in_game || (is_loc && !has_open_writer(host)))) {
            s64 size = fd_size(f);
            if (size >= 0) {
                s64 min_sz = is_loc ? 1 : (s64)FIMG_MIN;
                s64 max_sz = in_game ? (s64)FIMG_MAX : (s64)FIMG_OVERLAY_MAX;
                pthread_mutex_lock(&g_img_lock);
                f->img = fimg_open(host, size, min_sz, max_sz);
                pthread_mutex_unlock(&g_img_lock);
            }
        }
    }
    u32 h = hle_obj_new(OBJ_FILE, f);
    hle_obj_set_closer(h, close_file);
    // CREATE_ALWAYS/OPEN_ALWAYS над существующим файлом: ERROR_ALREADY_EXISTS
    rt_set_last_error((disp == CREATE_ALWAYS || disp == OPEN_ALWAYS) && exists ? ERROR_ALREADY_EXISTS : 0);
    return h;
}

void hle_kernel32_CreateFileA(CPU *c)
{
    HLE_ENTER("CreateFileA", 7);
    char name[1100];
    name_a(ARG(0), name, sizeof name);
    RET(7, create_file(name, ARG(1), ARG(4)));
}

void hle_kernel32_CreateFileW(CPU *c)
{
    HLE_ENTER("CreateFileW", 7);
    char name[1100];
    hle_w2utf8(ARG(0), name, sizeof name);
    RET(7, create_file(name, ARG(1), ARG(4)));
}

void hle_kernel32_ReadFile(CPU *c)
{
    HLE_ENTER("ReadFile", 5);
    hfile *f = hle_obj_get(ARG(0), OBJ_FILE);
    if (ARG(3))
        wr32(g_mem, ARG(3), 0);
    if (!f) {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(5, 0);
    }
    u32 got = 0, n = ARG(2);
    if (f->std)
        while (got < n) {
            ssize_t r = read(f->fd, g_mem + ARG(1) + got, n - got);
            if (r <= 0)
                break;
            got += (u32)r;
        }
    else
        got = buffered_read(f, g_mem + ARG(1), n);
    if (ARG(3))
        wr32(g_mem, ARG(3), got);
    RET(5, 1);
}

void hle_kernel32_WriteFile(CPU *c)
{
    HLE_ENTER("WriteFile", 5);
    hfile *f = hle_obj_get(ARG(0), OBJ_FILE);
    if (ARG(3))
        wr32(g_mem, ARG(3), 0);
    if (!f) {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(5, 0);
    }
    ssize_t r;
    int err = 0;
    if (f->std) {
        r = write(f->fd, g_mem + ARG(1), ARG(2));
        if (r < 0)
            err = errno;
    } else {
        u32 n = ARG(2);
        rb_drop(f);
        pthread_mutex_lock(&g_wb_lock);
        if (!f->lazy && f->fd < 0) {
            // отложенное открытие уже провалилось раньше: не буферизовать в сломанный дескриптор
            pthread_mutex_unlock(&g_wb_lock);
            rt_set_last_error(31);  // ERROR_GEN_FAILURE
            RET(5, 0);
        }
        // внутри накопленного или сразу за ним и влезает — в буфер, иначе накопленное — на диск
        if (f->wlen && !(f->pos >= f->wpos && f->pos <= f->wpos + f->wlen && (u64)(f->pos - f->wpos) + n <= WBUF)) {
            if (!wb_flush_locked(f)) {
                err = errno;
                pthread_mutex_unlock(&g_wb_lock);
                rt_set_last_error(errno_to_win(err));
                RET(5, 0);
            }
        }
        if (n < WBUF && !f->wbuf && (f->wbuf = malloc(WBUF)) != NULL)
            wb_list(f);
        if (n < WBUF && f->wbuf) {
            if (!f->wlen)
                f->wpos = f->pos;
            s64 diff = f->pos - f->wpos;
            if (diff < 0 || (u64)diff + n > WBUF) {
                // защитная проверка инварианта: не допускать выход за границы wbuf ни при каких ошибках выше
                pthread_mutex_unlock(&g_wb_lock);
                rt_set_last_error(31);  // ERROR_GEN_FAILURE
                RET(5, 0);
            }
            u32 at = (u32)diff;
            if (n)
                memcpy(f->wbuf + at, g_mem + ARG(1), n);
            if (at < f->wsent)  // уже отдано фону: при сбросе — заново
                ws_dirty(f, at, at + n < f->wsent ? at + n : f->wsent);
            if (at + n > f->wlen)
                f->wlen = at + n;
            if (f->wlen - f->wsent >= WS_CHUNK) {
                if (!f_open(f)) {
                    err = errno;
                    f->wlen = f->wsent = 0;
                    f->ndirty = 0;
                    pthread_mutex_unlock(&g_wb_lock);
                    rt_set_last_error(errno_to_win(err));
                    RET(5, 0);
                }
                ws_post(f);
            }
            r = (ssize_t)n;
        } else {
            if (!f_open(f) || !write_range(f->fd, g_mem + ARG(1), f->pos, n)) {
                r = -1;
                err = errno;
            } else {
                r = (ssize_t)n;
            }
        }
        if (r > 0) {
            f->pos += r;
            if (f->pos > f->wend)
                f->wend = f->pos;
        }
        pthread_mutex_unlock(&g_wb_lock);
    }
    if (r < 0) {
        rt_set_last_error(errno_to_win(err ? err : EIO));
        RET(5, 0);
    }
    if (ARG(3))
        wr32(g_mem, ARG(3), (u32)r);
    RET(5, 1);
}

void hle_kernel32_SetFilePointer(CPU *c)
{
    HLE_ENTER("SetFilePointer", 4);
    hfile *f = hle_obj_get(ARG(0), OBJ_FILE);
    if (!f || f->std) {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(4, 0xFFFFFFFFu);
    }
    s64 off = (s32)ARG(1);
    if (ARG(2))
        off = (s64)((u64)rd32(g_mem, ARG(2)) << 32 | ARG(1));
    if (ARG(3) == 2)
        wb_flush(f);
    s64 size = ARG(3) == 2 ? fd_size(f) : 0;
    s64 r = ARG(3) == 0 ? off : ARG(3) == 1 ? f->pos + off : size >= 0 ? size + off : -1;
    if (r >= 0)
        f->pos = r;
    if (r < 0) {
        rt_set_last_error(131);  // ERROR_NEGATIVE_SEEK
        RET(4, 0xFFFFFFFFu);
    }
    if (ARG(2))
        wr32(g_mem, ARG(2), (u32)((u64)r >> 32));
    rt_set_last_error(0);
    RET(4, (u32)r);
}

void hle_kernel32_GetFileSize(CPU *c)
{
    HLE_ENTER("GetFileSize", 2);
    hfile *f = hle_obj_get(ARG(0), OBJ_FILE);
    u64 t0 = rt_ticks_ns();
    s64 size = f && wb_flush(f) ? fd_size(f) : -1;
    __atomic_add_fetch(&rt_file_ns, rt_ticks_ns() - t0, __ATOMIC_RELAXED);
    if (size < 0) {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(2, 0xFFFFFFFFu);
    }
    if (ARG(1))
        wr32(g_mem, ARG(1), (u32)((u64)size >> 32));
    RET(2, (u32)size);
}

void hle_kernel32_SetEndOfFile(CPU *c)
{
    HLE_ENTER("SetEndOfFile", 1);
    hfile *f = hle_obj_get(ARG(0), OBJ_FILE);
    if (f) {
        rb_drop(f);
        fimg_invalidate(f->path);
        f->size_bad = 1;
    }
    RET(1, f && wb_flush(f) && ftruncate(f->fd, (off_t)f->pos) == 0);
}

void hle_kernel32_FlushFileBuffers(CPU *c)
{
    HLE_ENTER("FlushFileBuffers", 1);
    hfile *f = hle_obj_get(ARG(0), OBJ_FILE);
    RET(1, !f || f->std || wb_flush(f));
}
void hle_kernel32_LockFile(CPU *c) { HLE_ENTER("LockFile", 5); RET(5, 1); }
void hle_kernel32_UnlockFile(CPU *c) { HLE_ENTER("UnlockFile", 5); RET(5, 1); }
void hle_kernel32_SetFileTime(CPU *c) { HLE_ENTER("SetFileTime", 4); RET(4, 1); }

void hle_kernel32_GetFileType(CPU *c)
{
    HLE_ENTER("GetFileType", 1);
    hfile *f = hle_obj_get(ARG(0), OBJ_FILE);
    RET(1, !f ? 0 : f->std ? 2 /* FILE_TYPE_CHAR */ : 1 /* FILE_TYPE_DISK */);
}

void hle_kernel32_GetFileInformationByHandle(CPU *c)
{
    HLE_ENTER("GetFileInformationByHandle", 2);
    hfile *f = hle_obj_get(ARG(0), OBJ_FILE);
    struct stat st;
    if (!f || !wb_flush(f) || fstat(f->fd, &st) != 0)
        RET(2, 0);
    u32 p = ARG(1);
    memset(g_mem + p, 0, 52);
    wr32(g_mem, p, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
    wr64(g_mem, p + 4, to_filetime(st.st_mtime));
    wr64(g_mem, p + 12, to_filetime(st.st_atime));
    wr64(g_mem, p + 20, to_filetime(st.st_mtime));
    wr32(g_mem, p + 28, 0x1234);  // серийный номер тома
    wr32(g_mem, p + 32, (u32)((u64)st.st_size >> 32));
    wr32(g_mem, p + 36, (u32)st.st_size);
    wr32(g_mem, p + 40, 1);
    wr32(g_mem, p + 44, (u32)((u64)st.st_ino >> 32));
    wr32(g_mem, p + 48, (u32)st.st_ino);
    RET(2, 1);
}

void hle_kernel32_GetStdHandle(CPU *c)
{
    HLE_ENTER("GetStdHandle", 1);
    u32 n = ARG(0);
    RET(1, n == 0xFFFFFFF6u ? std_handle(0) : n == 0xFFFFFFF5u ? std_handle(1) : n == 0xFFFFFFF4u ? std_handle(2) : 0xFFFFFFFFu);
}

void hle_kernel32_SetStdHandle(CPU *c)
{
    HLE_ENTER("SetStdHandle", 2);
    u32 n = ARG(0);
    int i = n == 0xFFFFFFF6u ? 0 : n == 0xFFFFFFF5u ? 1 : n == 0xFFFFFFF4u ? 2 : -1;
    if (i >= 0)
        g_std[i] = ARG(1);
    RET(2, i >= 0);
}

void hle_kernel32_CloseHandle(CPU *c)
{
    HLE_ENTER("CloseHandle", 1);
    u32 h = ARG(0);
    for (int i = 0; i < 3; i++)
        if (g_std[i] == h)
            RET(1, 1);  // стандартные потоки не закрываем
    if (hle_obj_type(h) == OBJ_THREAD)
        RET(1, 1);  // поток живёт, пока не закончится (описатель нужен WaitForSingleObject)
    int ok = hle_obj_close(h);
    if (!ok)
        rt_set_last_error(ERROR_INVALID_HANDLE);
    RET(1, ok);
}

// ================================================================ атрибуты, каталоги
static u32 attrs_of(const char *name, struct stat *st_out)
{
    char rel[1100], host[1100];
    struct stat st;
    if (!vfs_rel(name, rel, sizeof rel) || !vfs_find(rel, host, sizeof host) || stat(host, &st) != 0) {
        rt_set_last_error(ERROR_FILE_NOT_FOUND);
        return 0xFFFFFFFFu;
    }
    if (st_out)
        *st_out = st;
    return S_ISDIR(st.st_mode) ? 0x10 : 0x20;
}

static void get_attrs(CPU *c, int wide)
{
    char name[1100];
    name_of(ARG(0), wide, name, sizeof name);
    c->eax = attrs_of(name, NULL);
    c->esp += 8;
}

void hle_kernel32_GetFileAttributesA(CPU *c) { HLE_ENTER("GetFileAttributesA", 1); get_attrs(c, 0); }
void hle_kernel32_GetFileAttributesW(CPU *c) { HLE_ENTER("GetFileAttributesW", 1); get_attrs(c, 1); }

void hle_kernel32_GetFileAttributesExW(CPU *c)
{
    HLE_ENTER("GetFileAttributesExW", 3);
    hle_file_flush_all();  // размер — с тем, что ещё в буферах открытых файлов
    char name[1100];
    hle_w2utf8(ARG(0), name, sizeof name);
    struct stat st;
    u32 a = attrs_of(name, &st);
    if (a == 0xFFFFFFFFu)
        RET(3, 0);
    u32 p = ARG(2);
    wr32(g_mem, p, a);
    wr64(g_mem, p + 4, to_filetime(st.st_mtime));
    wr64(g_mem, p + 12, to_filetime(st.st_atime));
    wr64(g_mem, p + 20, to_filetime(st.st_mtime));
    wr32(g_mem, p + 28, (u32)((u64)st.st_size >> 32));
    wr32(g_mem, p + 32, (u32)st.st_size);
    RET(3, 1);
}

void hle_kernel32_SetFileAttributesA(CPU *c) { HLE_ENTER("SetFileAttributesA", 2); RET(2, 1); }
void hle_kernel32_SetFileAttributesW(CPU *c) { HLE_ENTER("SetFileAttributesW", 2); RET(2, 1); }

static int path_op(CPU *c, int wide, int op)  // 0 mkdir, 1 rmdir, 2 unlink
{
    hle_file_flush_all();  // по имени — с тем, что ещё в буферах открытых файлов
    char name[1100], rel[1100], host[1100];
    name_of(ARG(0), wide, name, sizeof name);
    if (!vfs_rel(name, rel, sizeof rel)) {
        rt_set_last_error(ERROR_PATH_NOT_FOUND);
        return 0;
    }
    if (op == 0) {
        if (vfs_find(rel, host, sizeof host)) {
            rt_set_last_error(ERROR_ALREADY_EXISTS);
            return 0;
        }
        vfs_for_write(rel, host, sizeof host, 0);
        int ok = mkdir(host, 0755) == 0;
        vfs_changed();
        return ok || (rt_set_last_error(last_errno_to_win()), 0);
    }
    // удалить можно только то, что лежит в оверлее; каталог игры не меняется
    vfs_for_write(rel, host, sizeof host, 0);
    fimg_invalidate(host);
    int r = op == 1 ? rmdir(host) : unlink(host);
    vfs_changed();
    if (r != 0) {
        rt_set_last_error(last_errno_to_win());
        return 0;
    }
    return 1;
}

void hle_kernel32_CreateDirectoryA(CPU *c) { HLE_ENTER("CreateDirectoryA", 2); RET(2, path_op(c, 0, 0)); }
void hle_kernel32_CreateDirectoryW(CPU *c) { HLE_ENTER("CreateDirectoryW", 2); RET(2, path_op(c, 1, 0)); }
void hle_kernel32_RemoveDirectoryA(CPU *c) { HLE_ENTER("RemoveDirectoryA", 1); RET(1, path_op(c, 0, 1)); }
void hle_kernel32_RemoveDirectoryW(CPU *c) { HLE_ENTER("RemoveDirectoryW", 1); RET(1, path_op(c, 1, 1)); }
void hle_kernel32_DeleteFileA(CPU *c) { HLE_ENTER("DeleteFileA", 1); RET(1, path_op(c, 0, 2)); }
void hle_kernel32_DeleteFileW(CPU *c) { HLE_ENTER("DeleteFileW", 1); RET(1, path_op(c, 1, 2)); }

static int move_copy(CPU *c, int wide, int copy)
{
    hle_file_flush_all();  // по имени — с тем, что ещё в буферах открытых файлов
    char a[1100], b[1100], ra[1100], rb[1100], ha[1100], hb[1100];
    name_of(ARG(0), wide, a, sizeof a);
    name_of(ARG(1), wide, b, sizeof b);
    if (!vfs_rel(a, ra, sizeof ra) || !vfs_rel(b, rb, sizeof rb) || !vfs_find(ra, ha, sizeof ha)) {
        rt_set_last_error(ERROR_FILE_NOT_FOUND);
        return 0;
    }
    if (copy && ARG(2) && vfs_find(rb, hb, sizeof hb)) {
        rt_set_last_error(80);
        return 0;
    }
    vfs_for_write(rb, hb, sizeof hb, 0);
    fimg_invalidate(hb);
    if (!copy) {
        char ua[1100];
        vfs_for_write(ra, ua, sizeof ua, 0);
        if (!strcmp(ua, ha)) {
            fimg_invalidate(ha);
            int ok = rename(ha, hb) == 0;
            vfs_changed();
            return ok;
        }
    }
    // копирование (и перенос файла из каталога игры — копией)
    FILE *in = fopen(ha, "rb"), *out = in ? fopen(hb, "wb") : NULL;
    if (!out) {
        if (in)
            fclose(in);
        rt_set_last_error(ERROR_ACCESS_DENIED);
        return 0;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    vfs_changed();
    return 1;
}

void hle_kernel32_MoveFileA(CPU *c) { HLE_ENTER("MoveFileA", 2); RET(2, move_copy(c, 0, 0)); }
void hle_kernel32_MoveFileW(CPU *c) { HLE_ENTER("MoveFileW", 2); RET(2, move_copy(c, 1, 0)); }
void hle_kernel32_CopyFileA(CPU *c) { HLE_ENTER("CopyFileA", 3); RET(3, move_copy(c, 0, 1)); }
void hle_kernel32_CopyFileW(CPU *c) { HLE_ENTER("CopyFileW", 3); RET(3, move_copy(c, 1, 1)); }

void hle_kernel32_GetCurrentDirectoryA(CPU *c)
{
    HLE_ENTER("GetCurrentDirectoryA", 2);
    const char *d = vfs_cwd();
    u32 n = (u32)strlen(d);
    RET(2, ARG(0) <= n ? n + 1 : put_ansi(ARG(1), ARG(0), d));
}

void hle_kernel32_GetCurrentDirectoryW(CPU *c)
{
    HLE_ENTER("GetCurrentDirectoryW", 2);
    const char *d = vfs_cwd();
    u32 n = (u32)strlen(d);
    RET(2, ARG(0) <= n ? n + 1 : hle_put_w(ARG(1), ARG(0), d));
}

void hle_kernel32_SetCurrentDirectoryA(CPU *c)
{
    HLE_ENTER("SetCurrentDirectoryA", 1);
    char name[1100];
    name_a(ARG(0), name, sizeof name);
    RET(1, vfs_set_cwd(name));
}

void hle_kernel32_SetCurrentDirectoryW(CPU *c)
{
    HLE_ENTER("SetCurrentDirectoryW", 1);
    char name[1100];
    hle_w2utf8(ARG(0), name, sizeof name);
    RET(1, vfs_set_cwd(name));
}

static u32 full_path(CPU *c, int wide)
{
    char name[1100], full[1100];
    name_of(ARG(0), wide, name, sizeof name);
    vfs_full(name, full, sizeof full);
    u32 n = (u32)strlen(full);
    if (ARG(1) <= n)
        return n + 1;
    put_name(ARG(2), ARG(1), full, wide);
    if (ARG(3)) {
        const char *b = strrchr(full, '\\');
        u32 off = b ? (u32)(b + 1 - full) : 0;
        wr32(g_mem, ARG(3), ARG(2) + (wide ? 2 * off : off));
    }
    return n;
}

void hle_kernel32_GetFullPathNameA(CPU *c) { HLE_ENTER("GetFullPathNameA", 4); RET(4, full_path(c, 0)); }
void hle_kernel32_GetFullPathNameW(CPU *c) { HLE_ENTER("GetFullPathNameW", 4); RET(4, full_path(c, 1)); }

void hle_kernel32_GetDiskFreeSpaceA(CPU *c)
{
    HLE_ENTER("GetDiskFreeSpaceA", 5);
    const u32 v[4] = {8, 512, 2u << 20, 4u << 20};  // 4 КБ кластеры: 8 ГБ свободно из 16
    for (int i = 0; i < 4; i++)
        if (ARG(i + 1))
            wr32(g_mem, ARG(i + 1), v[i]);
    RET(5, 1);
}

static void disk_free_ex(CPU *c)
{
    const u64 v[3] = {8ull << 30, 16ull << 30, 8ull << 30};
    for (int i = 0; i < 3; i++)
        if (ARG(i + 1))
            wr64(g_mem, ARG(i + 1), v[i]);
}

void hle_kernel32_GetDiskFreeSpaceExA(CPU *c) { HLE_ENTER("GetDiskFreeSpaceExA", 4); disk_free_ex(c); RET(4, 1); }
void hle_kernel32_GetDiskFreeSpaceExW(CPU *c) { HLE_ENTER("GetDiskFreeSpaceExW", 4); disk_free_ex(c); RET(4, 1); }

static u32 drive_type(u32 root, int wide)
{
    u32 ch = !root ? 'C' : wide ? rd16(g_mem, root) : g_mem[root];
    return (ch | 0x20) == 'c' ? 3 /* DRIVE_FIXED */ : 1 /* DRIVE_NO_ROOT_DIR */;
}

void hle_kernel32_GetDriveTypeA(CPU *c) { HLE_ENTER("GetDriveTypeA", 1); RET(1, drive_type(ARG(0), 0)); }
void hle_kernel32_GetDriveTypeW(CPU *c) { HLE_ENTER("GetDriveTypeW", 1); RET(1, drive_type(ARG(0), 1)); }
void hle_kernel32_GetLogicalDrives(CPU *c) { HLE_ENTER("GetLogicalDrives", 0); RET(0, 1u << 2); }

// ================================================================ поиск файлов
typedef struct {
    char *names;
    int count, next;
    char rel_dir[1100], pattern[260];
    int wide;
} hfind;

static void close_find(void *p)
{
    hfind *f = p;
    free(f->names);
    free(f);
}

static int wild_match(const char *pat, const char *s)  // * и ? без учёта регистра; «*.*» совпадает со всем
{
    if (!strcmp(pat, "*.*"))
        return 1;
    while (*pat) {
        if (*pat == '*') {
            pat++;
            for (;; s++) {
                if (wild_match(pat, s))
                    return 1;
                if (!*s)
                    return 0;
            }
        }
        if (!*s)
            return !strcmp(pat, ".") || !strcmp(pat, "*");
        if (*pat != '?' && (*pat | 0x20) != (*s | 0x20) && *pat != *s)
            return 0;
        pat++;
        s++;
    }
    return !*s;
}

static int find_fill(hfind *f, u32 data)
{
    const char *names = f->names;
    for (int i = 0; i < f->next; i++)
        names += strlen(names) + 1;
    for (; f->next < f->count; f->next++, names += strlen(names) + 1) {
        if (!wild_match(f->pattern, names))
            continue;
        char rel[1100], host[1100];
        snprintf(rel, sizeof rel, "%s%s%s", f->rel_dir, f->rel_dir[0] ? "/" : "", names);
        struct stat st = {0};
        if (strcmp(names, ".") && strcmp(names, "..") && (!vfs_find(rel, host, sizeof host) || stat(host, &st) != 0))
            continue;
        int dir = !strcmp(names, ".") || !strcmp(names, "..") || S_ISDIR(st.st_mode);
        memset(g_mem + data, 0, f->wide ? 592 : 320);
        wr32(g_mem, data, dir ? 0x10 : 0x20);
        wr64(g_mem, data + 4, to_filetime(st.st_mtime));
        wr64(g_mem, data + 12, to_filetime(st.st_atime));
        wr64(g_mem, data + 20, to_filetime(st.st_mtime));
        wr32(g_mem, data + 28, (u32)((u64)st.st_size >> 32));
        wr32(g_mem, data + 32, dir ? 0 : (u32)st.st_size);
        put_name(data + 44, 260, names, f->wide);
        f->next++;
        return 1;
    }
    return 0;
}

static u32 find_first(CPU *c, int wide)
{
    hle_file_flush_all();  // по имени — с тем, что ещё в буферах открытых файлов
    char name[1100], rel[1100];
    name_of(ARG(0), wide, name, sizeof name);
    if (!vfs_rel(name, rel, sizeof rel)) {
        rt_set_last_error(ERROR_PATH_NOT_FOUND);
        return 0xFFFFFFFFu;
    }
    hfind *f = calloc(1, sizeof *f);
    f->wide = wide;
    char *slash = strrchr(rel, '/');
    if (slash) {
        *slash = 0;
        snprintf(f->rel_dir, sizeof f->rel_dir, "%s", rel);
        snprintf(f->pattern, sizeof f->pattern, "%s", slash + 1);
    } else {
        snprintf(f->pattern, sizeof f->pattern, "%s", rel);
    }
    f->count = vfs_list(f->rel_dir, &f->names);
    if (f->count < 0 || !find_fill(f, ARG(1))) {
        close_find(f);
        rt_set_last_error(f->count < 0 ? ERROR_PATH_NOT_FOUND : ERROR_FILE_NOT_FOUND);
        return 0xFFFFFFFFu;
    }
    u32 h = hle_obj_new(OBJ_FIND, f);
    hle_obj_set_closer(h, close_find);
    return h;
}

void hle_kernel32_FindFirstFileA(CPU *c) { HLE_ENTER("FindFirstFileA", 2); RET(2, find_first(c, 0)); }
void hle_kernel32_FindFirstFileW(CPU *c) { HLE_ENTER("FindFirstFileW", 2); RET(2, find_first(c, 1)); }

static u32 find_next(CPU *c)
{
    hfind *f = hle_obj_get(ARG(0), OBJ_FIND);
    if (!f) {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (!find_fill(f, ARG(1))) {
        rt_set_last_error(ERROR_NO_MORE_FILES);
        return 0;
    }
    return 1;
}

void hle_kernel32_FindNextFileA(CPU *c) { HLE_ENTER("FindNextFileA", 2); RET(2, find_next(c)); }
void hle_kernel32_FindNextFileW(CPU *c) { HLE_ENTER("FindNextFileW", 2); RET(2, find_next(c)); }
void hle_kernel32_FindClose(CPU *c) { HLE_ENTER("FindClose", 1); RET(1, hle_obj_close(ARG(0))); }

// ================================================================ проекции файлов
typedef struct {
    u32 file, size;
} hmapping;

void hle_kernel32_CreateFileMappingA(CPU *c)
{
    HLE_ENTER("CreateFileMappingA", 6);
    hfile *f = hle_obj_get(ARG(0), OBJ_FILE);
    struct stat st;
    if (!f || !wb_flush(f) || fstat(f->fd, &st) != 0) {
        hle_todo("CreateFileMapping without a file (pagefile-backed)");
        RET(6, 0);
    }
    f->size_bad = 1;  // проекция может менять файл мимо WriteFile
    hmapping *m = calloc(1, sizeof *m);
    m->file = ARG(0);
    m->size = ARG(4) ? ARG(4) : (u32)st.st_size;
    u32 h = hle_obj_new(OBJ_MAPPING, m);
    hle_obj_set_closer(h, free);
    RET(6, h);
}

void hle_kernel32_MapViewOfFile(CPU *c)
{
    HLE_ENTER("MapViewOfFile", 5);
    hmapping *m = hle_obj_get(ARG(0), OBJ_MAPPING);
    hfile *f = m ? hle_obj_get(m->file, OBJ_FILE) : NULL;
    if (!f)
        RET(5, 0);
    u32 off = ARG(2), n = ARG(4) ? ARG(4) : m->size - off;
    u32 a = vm_alloc(0, n ? n : 1, VM_MEM_RESERVE | VM_MEM_COMMIT, 0x04);
    if (a && read_at(f, off, g_mem + a, n) < 0)  // позиция гостя (f->pos) не меняется
        hle_todo("MapViewOfFile: read failed");
    RET(5, a);  // копия содержимого; запись обратно не поддерживается
}

void hle_kernel32_UnmapViewOfFile(CPU *c) { HLE_ENTER("UnmapViewOfFile", 1); RET(1, vm_free(ARG(0), 0, VM_MEM_RELEASE)); }

// ================================================================ ресурсы PE
// HRSRC — гостевой адрес IMAGE_RESOURCE_DATA_ENTRY, HGLOBAL ресурса — адрес данных.
static int res_name_eq(u32 base, u32 entry, u32 name, int wide)
{
    u32 id = rd32(g_mem, entry);
    if (name < 0x10000)
        return !(id & 0x80000000u) && id == name;
    if (!(id & 0x80000000u))
        return 0;
    u32 s = base + (id & 0x7FFFFFFFu);
    u32 len = rd16(g_mem, s);
    char buf[260];
    if (wide)
        hle_w2utf8(name, buf, sizeof buf);
    else
        snprintf(buf, sizeof buf, "%s", hle_cstr(name));
    if (buf[0] == '#')
        return 0;
    if (strlen(buf) != len)
        return 0;
    for (u32 i = 0; i < len; i++) {
        u32 ch = rd16(g_mem, s + 2 + 2 * i);
        if (ch >= 0x80 || (ch | 0x20) != ((u8)buf[i] | 0x20))
            return 0;
    }
    return 1;
}

static u32 res_lookup(u32 base, u32 dir, u32 root, u32 name, int wide, int any)
{
    // «#123» — числовой идентификатор
    if (name >= 0x10000) {
        char buf[16];
        if (wide)
            hle_w2utf8(name, buf, sizeof buf);
        else
            snprintf(buf, sizeof buf, "%s", hle_cstr(name));
        if (buf[0] == '#')
            name = (u32)strtoul(buf + 1, NULL, 10);
    }
    u32 n = rd16(g_mem, dir + 12) + rd16(g_mem, dir + 14);
    for (u32 i = 0; i < n; i++) {
        u32 e = dir + 16 + 8 * i;
        if (any || res_name_eq(root, e, name, wide))
            return rd32(g_mem, e + 4);
    }
    return 0xFFFFFFFFu;
}

u32 hle_find_resource(u32 hmod, u32 name, u32 type, int wide)
{
    if (!hmod)
        hmod = rt_module_main()->desc->base;
    if (!rt_module_by_base(hmod))
        return 0;
    u32 pe = hmod + rd32(g_mem, hmod + 0x3C);
    u32 rva = rd32(g_mem, pe + 24 + 96 + 2 * 8);
    if (!rva)
        return 0;
    u32 root = hmod + rva;
    u32 t = res_lookup(root, root, root, type, wide, 0);
    if (t == 0xFFFFFFFFu || !(t & 0x80000000u))
        return 0;
    u32 nm = res_lookup(root, root + (t & 0x7FFFFFFFu), root, name, wide, 0);
    if (nm == 0xFFFFFFFFu || !(nm & 0x80000000u))
        return 0;
    u32 lang = res_lookup(root, root + (nm & 0x7FFFFFFFu), root, 0, wide, 1);  // первый язык
    if (lang == 0xFFFFFFFFu || (lang & 0x80000000u))
        return 0;
    return root + lang;
}

void hle_kernel32_FindResourceA(CPU *c)
{
    HLE_ENTER("FindResourceA", 3);
    u32 r = hle_find_resource(ARG(0), ARG(1), ARG(2), 0);
    if (!r)
        rt_set_last_error(1814);  // ERROR_RESOURCE_NAME_NOT_FOUND
    RET(3, r);
}

void hle_kernel32_FindResourceW(CPU *c)
{
    HLE_ENTER("FindResourceW", 3);
    u32 r = hle_find_resource(ARG(0), ARG(1), ARG(2), 1);
    if (!r)
        rt_set_last_error(1814);
    RET(3, r);
}

void hle_kernel32_LoadResource(CPU *c)
{
    HLE_ENTER("LoadResource", 2);
    u32 hmod = ARG(0) ? ARG(0) : rt_module_main()->desc->base;
    RET(2, ARG(1) ? hmod + rd32(g_mem, ARG(1)) : 0);
}

void hle_kernel32_LockResource(CPU *c) { HLE_ENTER("LockResource", 1); RET(1, ARG(0)); }
void hle_kernel32_SizeofResource(CPU *c) { HLE_ENTER("SizeofResource", 2); RET(2, ARG(1) ? rd32(g_mem, ARG(1) + 4) : 0); }

// Webhead.exe запрашивает путь/имя через GetPrivateProfileStringW до инициализации
// config-кеша UE2. Ищем по-настоящему .ini пока негде: отдаём default-строку,
// как Windows при отсутствии ключа (alcove: игра дальше читает свой Webhead.ini).
void hle_kernel32_GetPrivateProfileStringA(CPU *c)
{
    HLE_ENTER("GetPrivateProfileStringA", 6);
    u32 def = ARG(2), dst = ARG(3), cap = ARG(4);
    RET(6, def && dst && cap ? hle_put_a(dst, cap, hle_cstr(def)) : 0);
}

void hle_kernel32_GetPrivateProfileStringW(CPU *c)
{
    HLE_ENTER("GetPrivateProfileStringW", 6);
    u32 def = ARG(2), dst = ARG(3), cap = ARG(4), n = 0;
    if (def && dst && cap) {
        while (n + 1 < cap && rd16(g_mem, def + 2 * n)) {
            wr16(g_mem, dst + 2 * n, rd16(g_mem, def + 2 * n));
            n++;
        }
        wr16(g_mem, dst + 2 * n, 0);
    }
    RET(6, n);
}
