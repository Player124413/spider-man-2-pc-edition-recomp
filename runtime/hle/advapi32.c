// HLE advapi32 (имя пользователя, реестр), winmm (таймер), ole32 (GUID), shell32.
// Реестр — плоское хранилище «ключ -> значения» в памяти с сохранением в <user>/registry.txt.
#include <pthread.h>
#include <stdlib.h>
#include <strings.h>

#include "runtime/hle/hle.h"
#include "runtime/hle/vfs.h"

// ================================================================ имя пользователя
void hle_advapi32_GetUserNameA(CPU *c)
{
    HLE_ENTER("GetUserNameA", 2);
    u32 cap = rd32(g_mem, ARG(1));
    if (cap < 7) {
        wr32(g_mem, ARG(1), 7);
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(2, 0);
    }
    wr32(g_mem, ARG(1), hle_put_a(ARG(0), cap, "Player") + 1);
    RET(2, 1);
}

void hle_advapi32_GetUserNameW(CPU *c)
{
    HLE_ENTER("GetUserNameW", 2);
    u32 cap = rd32(g_mem, ARG(1));
    if (cap < 7) {
        wr32(g_mem, ARG(1), 7);
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(2, 0);
    }
    wr32(g_mem, ARG(1), hle_put_w(ARG(0), cap, "Player") + 1);
    RET(2, 1);
}

// ================================================================ реестр
typedef struct {
    char key[256], name[128];
    u32 type, size;
    u8 data[512];
} reg_value;

#define MAX_VALUES 512
#define MAX_KEYS 128
static reg_value g_vals[MAX_VALUES];
static int g_nvals;
static char g_keys[MAX_KEYS][256];  // открытые/созданные ключи; описатель = 0x80000100 + индекс
static int g_nkeys;
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_reg_loaded;

static const char *root_name(u32 h)
{
    switch (h) {
    case 0x80000000u: return "HKCR";
    case 0x80000001u: return "HKCU";
    case 0x80000002u: return "HKLM";
    case 0x80000003u: return "HKU";
    default: return NULL;
    }
}

static int key_path(u32 parent, const char *sub, char *out, size_t cap)
{
    const char *base = root_name(parent);
    if (!base) {
        u32 i = parent - 0x80000100u;
        if (i >= (u32)g_nkeys)
            return 0;
        base = g_keys[i];
    }
    snprintf(out, cap, "%s%s%s", base, sub && *sub ? "\\" : "", sub ? sub : "");
    for (char *p = out; *p; p++)
        if (*p == '/')
            *p = '\\';
    return 1;
}

static int reg_file_read(char *out, size_t cap)
{
    // Чтение без vfs_for_write, чтобы не сбрасывать кеш каталогов VFS (vfs_changed) при запросах реестра.
    return vfs_find("registry.txt", out, cap);
}

static int reg_file_write(char *out, size_t cap)
{
    return vfs_for_write("registry.txt", out, cap, 0);
}

static void reg_load(void)
{
    if (g_reg_loaded)
        return;
    g_reg_loaded = 1;
    char path[1100];
    if (!reg_file_read(path, sizeof path))
        return;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[2048];
    while (fgets(line, sizeof line, f) && g_nvals < MAX_VALUES) {
        reg_value *v = &g_vals[g_nvals];
        char hex[1100] = "";
        if (sscanf(line, "%255[^\t]\t%127[^\t]\t%u\t%1099s", v->key, v->name, &v->type, hex) < 3)
            continue;
        if (!strcmp(v->name, "@"))
            v->name[0] = 0;
        v->size = 0;
        for (const char *p = hex; p[0] && p[1] && v->size < sizeof v->data; p += 2) {
            unsigned b = 0;
            if (sscanf(p, "%2x", &b) != 1)
                break;
            v->data[v->size++] = (u8)b;
        }
        g_nvals++;
    }
    fclose(f);
}

static void reg_save(void)
{
    char path[1100], tmp[1120];
    if (!reg_file_write(path, sizeof path))
        return;
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof tmp)
        return;
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    for (int i = 0; i < g_nvals; i++) {
        reg_value *v = &g_vals[i];
        fprintf(f, "%s\t%s\t%u\t", v->key, v->name[0] ? v->name : "@", v->type);
        for (u32 k = 0; k < v->size; k++)
            fprintf(f, "%02x", v->data[k]);
        fprintf(f, "\n");
    }
    int err = ferror(f) != 0;
    if (fclose(f) != 0)
        err = 1;
    if (err) {
        remove(tmp);
        return;
    }
    // Атомарная замена на POSIX; если ФС (например, Switch fsdev) не даёт переименовать поверх существующего
    // файла, удаляем старый и повторяем rename.
    if (rename(tmp, path) != 0) {
        remove(path);
        if (rename(tmp, path) != 0) {
            remove(tmp);
            return;
        }
    }
    vfs_changed();
}

static int key_exists(const char *path)
{
    size_t n = strlen(path);
    for (int i = 0; i < g_nvals; i++)
        if (!strncasecmp(g_vals[i].key, path, n) && (!g_vals[i].key[n] || g_vals[i].key[n] == '\\'))
            return 1;
    for (int i = 0; i < g_nkeys; i++)
        if (!strcasecmp(g_keys[i], path))
            return 1;
    return 0;
}

static u32 key_handle(const char *path)
{
    for (int i = 0; i < g_nkeys; i++)
        if (!strcasecmp(g_keys[i], path))
            return 0x80000100u + (u32)i;
    if (g_nkeys == MAX_KEYS)
        return 0;
    snprintf(g_keys[g_nkeys], sizeof g_keys[0], "%s", path);
    return 0x80000100u + (u32)g_nkeys++;
}

static u32 reg_open(u32 parent, const char *sub, u32 out, int create)
{
    char path[256];
    pthread_mutex_lock(&g_reg_lock);
    reg_load();
    u32 r = ERROR_FILE_NOT_FOUND;
    if (key_path(parent, sub, path, sizeof path) && (create || key_exists(path) || !sub || !*sub)) {
        u32 h = key_handle(path);
        if (h) {
            wr32(g_mem, out, h);
            r = ERROR_SUCCESS;
        }
    }
    pthread_mutex_unlock(&g_reg_lock);
    if (getenv("SNX_LOG_FILES"))
        fprintf(stderr, "[reg] %s %s\\%s -> %u\n", create ? "create" : "open", root_name(parent) ? root_name(parent) : "?",
                sub ? sub : "", r);
    return r;
}

void hle_advapi32_RegOpenKeyA(CPU *c) { HLE_ENTER("RegOpenKeyA", 3); RET(3, reg_open(ARG(0), hle_cstr(ARG(1)), ARG(2), 0)); }
void hle_advapi32_RegOpenKeyExA(CPU *c) { HLE_ENTER("RegOpenKeyExA", 5); RET(5, reg_open(ARG(0), hle_cstr(ARG(1)), ARG(4), 0)); }

void hle_advapi32_RegCreateKeyExA(CPU *c)
{
    HLE_ENTER("RegCreateKeyExA", 9);
    u32 r = reg_open(ARG(0), hle_cstr(ARG(1)), ARG(7), 1);
    if (ARG(8))
        wr32(g_mem, ARG(8), 1);  // REG_CREATED_NEW_KEY
    RET(9, r);
}

void hle_advapi32_RegCloseKey(CPU *c) { HLE_ENTER("RegCloseKey", 1); RET(1, ERROR_SUCCESS); }

void hle_advapi32_RegQueryValueExA(CPU *c)
{
    HLE_ENTER("RegQueryValueExA", 6);
    char path[256];
    const char *name = hle_cstr(ARG(1));
    pthread_mutex_lock(&g_reg_lock);
    reg_load();
    u32 r = ERROR_FILE_NOT_FOUND;
    if (key_path(ARG(0), NULL, path, sizeof path))
        for (int i = 0; i < g_nvals; i++) {
            reg_value *v = &g_vals[i];
            if (strcasecmp(v->key, path) || strcasecmp(v->name, name))
                continue;
            if (ARG(3))
                wr32(g_mem, ARG(3), v->type);
            u32 cap = ARG(5) ? rd32(g_mem, ARG(5)) : 0;
            if (ARG(5))
                wr32(g_mem, ARG(5), v->size);
            r = ERROR_SUCCESS;
            if (ARG(4)) {
                if (cap < v->size)
                    r = 234;  // ERROR_MORE_DATA
                else
                    memcpy(g_mem + ARG(4), v->data, v->size);
            }
            break;
        }
    pthread_mutex_unlock(&g_reg_lock);
    if (getenv("SNX_LOG_FILES"))
        fprintf(stderr, "[reg] query %s\\%s -> %u\n", path, name, r);
    RET(6, r);
}

void hle_advapi32_RegSetValueExA(CPU *c)
{
    HLE_ENTER("RegSetValueExA", 6);
    char path[256];
    const char *name = hle_cstr(ARG(1));
    pthread_mutex_lock(&g_reg_lock);
    reg_load();
    u32 r = ERROR_INVALID_HANDLE;
    if (key_path(ARG(0), NULL, path, sizeof path)) {
        reg_value *v = NULL;
        for (int i = 0; i < g_nvals; i++)
            if (!strcasecmp(g_vals[i].key, path) && !strcasecmp(g_vals[i].name, name))
                v = &g_vals[i];
        if (!v && g_nvals < MAX_VALUES)
            v = &g_vals[g_nvals++];
        if (v) {
            snprintf(v->key, sizeof v->key, "%s", path);
            snprintf(v->name, sizeof v->name, "%s", name);
            v->type = ARG(3);
            v->size = ARG(5) < sizeof v->data ? ARG(5) : (u32)sizeof v->data;
            memcpy(v->data, g_mem + ARG(4), v->size);
            reg_save();
            r = ERROR_SUCCESS;
        }
    }
    pthread_mutex_unlock(&g_reg_lock);
    RET(6, r);
}

// ================================================================ winmm
void hle_winmm_timeGetTime(CPU *c) { HLE_ENTER("timeGetTime", 0); RET(0, (u32)(rt_guest_ticks_ns() / 1000000u)); }
void hle_winmm_timeBeginPeriod(CPU *c) { HLE_ENTER("timeBeginPeriod", 1); RET(1, 0); }
void hle_winmm_timeEndPeriod(CPU *c) { HLE_ENTER("timeEndPeriod", 1); RET(1, 0); }

// ================================================================ ole32, shell32
void hle_ole32_CoCreateGuid(CPU *c)
{
    HLE_ENTER("CoCreateGuid", 1);
    for (int i = 0; i < 16; i++)
        g_mem[ARG(0) + (u32)i] = (u8)(rand() >> 7);
    g_mem[ARG(0) + 7] = (u8)((g_mem[ARG(0) + 7] & 0x0F) | 0x40);  // версия 4
    RET(1, 0);
}

void hle_shell32_ShellExecuteA(CPU *c)
{
    HLE_ENTER("ShellExecuteA", 6);
    fprintf(stderr, "[hle] ShellExecuteA(%s, %s) ignored\n", hle_cstr(ARG(1)), hle_cstr(ARG(2)));
    RET(6, 2);  // <= 32 — ошибка (ERROR_FILE_NOT_FOUND)
}

void hle_shell32_ShellExecuteW(CPU *c)
{
    HLE_ENTER("ShellExecuteW", 6);
    char op[64], file[520];
    hle_w2utf8(ARG(1), op, sizeof op);
    hle_w2utf8(ARG(2), file, sizeof file);
    fprintf(stderr, "[hle] ShellExecuteW(%s, %s) ignored\n", op, file);
    RET(6, 2);
}

void hle_shell32_Shell_NotifyIconA(CPU *c) { HLE_ENTER("Shell_NotifyIconA", 2); RET(2, 1); }
