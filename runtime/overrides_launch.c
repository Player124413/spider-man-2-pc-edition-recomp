// Нативные замены функций game.exe (таблица — recomp/overrides.py). Отдельно от overrides_core.c: на них ссылается
// только таблица game.exe, и харнессы дифф-тестов других модулей собираются без неё.
#include <stdio.h>
#include <stdlib.h>

#include "runtime/abi.h"
#include "runtime/ini_cache.h"
#include "runtime/rt.h"

// ---------------------------------------------------------------- запись файла (game.exe)
// FArchiveFileWriter::Serialize(V, Length): копия в буфер 4 КБ архива, полный буфер — виртуальный Flush (WriteFile).
// Сохранение пишет так каждое поле по 1–4 байта; помещается в буфер — копия здесь же, иначе — оригинал.
#define FW_POS 0x48u
#define FW_COUNT 0x4Cu
#define FW_BUF 0x50u
#define FW_BUF_SIZE 0x1000u

extern void (*const ovr_orig_ovr_launch_file_write)(CPU *c);

void ovr_launch_file_write(CPU *c)
{
    u8 *m = g_mem;
    u32 ar = c->ecx, v = rd32(m, c->esp + 4);
    s32 len = (s32)rd32(m, c->esp + 8), cnt = (s32)rd32(m, ar + FW_COUNT);
    if (len < 0 || cnt < 0 || len > (s32)FW_BUF_SIZE - cnt) {
        RT_ENTER_GEN(c, ovr_orig_ovr_launch_file_write);  // оригинал — код гостя
        return;
    }
    wr32(m, ar + FW_POS, rd32(m, ar + FW_POS) + (u32)len);
    memmove(m + ar + FW_BUF + (u32)cnt, m + v, (u32)len);
    wr32(m, ar + FW_COUNT, (u32)(cnt + len));
    c->esp += 12;  // thiscall, два аргумента: ret 8
}

// ---------------------------------------------------------------- чтение файла (game.exe)
// FArchiveFileReader::Serialize(V, Length) (game.exe!0x10908610): копия из буфера 1 КБ архива; когда данных в
// буфере не хватает — виртуальный Precache (game.exe!0x10908550, ReadFile) или прямое чтение ReadFile (>= 1 КБ)
// в оригинале. При загрузке пакетов UE2 читает так каждое поле по 1–4 байта; всё, что помещается в буфер,
// копируется здесь без вызовов гостя. SNX_AR_CHECK=1 — каждое попадание сверяется с оригиналом.
#define FR_POS 0x4Cu
#define FR_BUF_BASE 0x50u
#define FR_BUF_COUNT 0x54u
#define FR_BUF 0x58u
#define FR_BUF_SIZE 0x400u

extern void (*const ovr_orig_ovr_launch_file_read)(CPU *c);

static inline int ar_check(void)  // SNX_AR_CHECK=1: сверка чтения архива с оригиналом
{
    static int v = -1;
    if (v < 0)
        v = getenv("SNX_AR_CHECK") != NULL;
    return v;
}

void ovr_launch_file_read(CPU *c)
{
    u8 *m = g_mem;
    u32 ar = c->ecx, v = rd32(m, c->esp + 4);
    s32 len = (s32)rd32(m, c->esp + 8);
    if (len <= 0) {
        if (ar_check()) {
            u32 sp = c->esp;
            RT_ENTER_GEN(c, ovr_orig_ovr_launch_file_read);  // оригинал — код гостя
            if (c->esp != sp + 12)
                fprintf(stderr, "[launch] SNX_AR_CHECK: FileReader::Serialize(len=%d) esp mismatch\n", len);
            return;
        }
        c->esp += 12;  // thiscall, два аргумента: ret 8
        return;
    }
    if (ar > RT_WINDOW - (FR_BUF + FR_BUF_SIZE) || v > RT_WINDOW || (u64)len > (u64)RT_WINDOW - v) {
        RT_ENTER_GEN(c, ovr_orig_ovr_launch_file_read);  // вне окна гостя — оригинал
        return;
    }
    s64 pos = (s32)rd32(m, ar + FR_POS);
    s64 base = (s32)rd32(m, ar + FR_BUF_BASE);
    s64 cnt = (s32)rd32(m, ar + FR_BUF_COUNT);
    s64 off = pos - base;
    if (off < 0 || cnt < 0 || cnt > (s64)FR_BUF_SIZE || (s64)len > cnt - off) {
        RT_ENTER_GEN(c, ovr_orig_ovr_launch_file_read);  // буфер пуст или не хватает байт — оригинал
        return;
    }
    if (ar_check()) {
        u8 want[FR_BUF_SIZE];
        memcpy(want, m + ar + FR_BUF + (u32)off, (u32)len);
        u32 want_pos = (u32)(pos + len), sp = c->esp;
        RT_ENTER_GEN(c, ovr_orig_ovr_launch_file_read);  // оригинал — код гостя
        u32 got_pos = rd32(m, ar + FR_POS);
        if (c->esp != sp + 12 || got_pos != want_pos || memcmp(m + v, want, (u32)len) != 0)
            fprintf(stderr, "[launch] SNX_AR_CHECK: FileReader::Serialize mismatch (pos %u vs %u, len %d)\n",
                    want_pos, got_pos, len);
        return;
    }
    memmove(m + v, m + ar + FR_BUF + (u32)off, (u32)len);
    u32 new_pos = (u32)(pos + len);
    wr32(m, ar + FR_POS, new_pos);
    c->eax = new_pos;  // как в оригинале (webhead.exe!0x40xxxxx)
    c->esp += 12;      // thiscall, два аргумента: ret 8
}

// ---------------------------------------------------------------- размер файла (game.exe)
// FFileManagerWindows::FileSize(Filename): CreateFile на чтение, GetFileSize, CloseHandle. CreateFileWriter зовёт её
// перед каждой записью (SaveConfig — на каждой монете бонусного уровня), а открытие на Switch — запросы к файловому
// сервису (≈1,5 мс). Без открытия отвечает слой файлов (rt_file_size_hook — hle_file_size_known из
// runtime/hle/kernel32_file.c, ставит host/boot/main.c): нет файла, каталог, файл, записанный этим слоем и
// закрытый. Иначе и без крючка (харнессы дифф-тестов) — оригинал. SNX_CFG_CHECK=1 — сверка с оригиналом.
int (*rt_file_size_hook)(u32 wname, s64 *size);

extern void (*const ovr_orig_ovr_launch_file_size)(CPU *c);

void ovr_launch_file_size(CPU *c)
{
    s64 size;
    u32 name = rd32(g_mem, c->esp + 4);
    if (!rt_file_size_hook || !rt_file_size_hook(name, &size)) {
        RT_ENTER_GEN(c, ovr_orig_ovr_launch_file_size);  // оригинал — код гостя
        return;
    }
    u32 r = size < 0 ? 0xFFFFFFFFu : (u32)size;  // INT из GetFileSize; нет файла — -1
    if (cfg_check()) {
        u32 sp = c->esp;
        RT_ENTER_GEN(c, ovr_orig_ovr_launch_file_size);  // оригинал — код гостя
        if (c->eax != r)
            fprintf(stderr, "[launch] SNX_CFG_CHECK: FileSize %d, original %d\n", (s32)r, (s32)c->eax);
        c->esp = sp;
    }
    c->eax = r;
    c->esp += 8;  // thiscall, один аргумент: ret 4
}

// ---------------------------------------------------------------- чтение кеша ini (game.exe)
// FConfigCacheIni::GetString(Section, Key, Value, Size, Filename): Find(Filename) — копия имени в буфер, «.ini» в
// конец, User.ini/System.ini — пути из кеша, поиск файла; затем секция и ключ — каждый через временный FString
// (выделение и освобождение памяти) и хеш строки; значение — appStrncpy (wcsncpy: остаток буфера — нули). Перечитывание
// конфига в SaveConfig (overrides_core.c) зовёт её сотни раз. Здесь то же по памяти кеша без вызовов гостя: файлы и секции — ключи
// TMap (уникальны), ключ в секции — TMultiMap: Find отдаёт пару с наибольшим индексом (цепочка хеша идёт от новых
// к старым). Сравнение — без учёта регистра ASCII (appStricmp — _wcsicmp); не-ASCII, где строки расходятся, файла
// нет в кеше (Find прочёл бы его с диска), Size <= 0 — оригинал. SNX_CFG_CHECK=1 — каждый ответ сверяется с ним.
extern void (*const ovr_orig_ovr_launch_cfg_get_string)(CPU *c);

static inline u16 fold(u16 ch) { return ch >= 'A' && ch <= 'Z' ? (u16)(ch + 32) : ch; }

// строка гостя (0 — "") и строка хоста: 0 — равны без учёта регистра, 1 — нет, -1 — не-ASCII там, где расходятся
static int cmp_gh(u32 a, const u16 *b)
{
    for (u32 i = 0;; i++) {
        u16 x = a ? rd16(g_mem, a + 2 * i) : 0, y = b[i];
        if (x != y) {
            if (x >= 0x80 || y >= 0x80)
                return -1;
            if (fold(x) != fold(y))
                return 1;
        }
        if (!x)
            return 0;
    }
}

static int cmp_gg(u32 a, u32 b)
{
    for (u32 i = 0;; i++) {
        u16 x = a ? rd16(g_mem, a + 2 * i) : 0, y = b ? rd16(g_mem, b + 2 * i) : 0;
        if (x != y) {
            if (x >= 0x80 || y >= 0x80)
                return -1;
            if (fold(x) != fold(y))
                return 1;
        }
        if (!x)
            return 0;
    }
}

// 1 — найдено (*val — строка значения гостя, 0 — ""), 0 — нет, -1 — решает оригинал
static int cfg_lookup(u32 self, u32 sec, u32 key, u32 fname, u32 *val)
{
    static const u16 ext[] = {'.', 'i', 'n', 'i', 0}, user[] = {'U', 's', 'e', 'r', '.', 'i', 'n', 'i', 0},
                     sys[] = {'S', 'y', 's', 't', 'e', 'm', '.', 'i', 'n', 'i', 0};
    u16 name[512];
    u32 src = fname ? fname : fstr_data(self + CFG_SYSTEM_INI), n = 0;
    for (; n < 500 && (name[n] = src ? rd16(g_mem, src + 2 * n) : 0); n++)
        ;
    if (n >= 500)
        return -1;
    if (n < 5 || (name[n - 4] != '.' && name[n - 5] != '.'))
        memcpy(name + n, ext, sizeof ext);
    u32 alias = 0;
    for (u32 i = 0; name[i]; i++)
        if (name[i] >= 0x80)
            return -1;
    // appStricmp(Filename, "User.ini"/"System.ini") — строки ASCII, имя тоже
    int is_user = 1, is_sys = 1;
    for (u32 i = 0; is_user && (name[i] || user[i]); i++)
        is_user = fold(name[i]) == fold(user[i]);
    for (u32 i = 0; is_sys && (name[i] || sys[i]); i++)
        is_sys = fold(name[i]) == fold(sys[i]);
    if (is_user)
        alias = fstr_data(self + CFG_USER_INI);
    else if (is_sys)
        alias = fstr_data(self + CFG_SYSTEM_INI);
    if (is_user || is_sys) {
        for (n = 0; n < 500 && (name[n] = alias ? rd16(g_mem, alias + 2 * n) : 0); n++)
            ;
        if (n >= 500)
            return -1;
    }
    u32 files = rd32(g_mem, self + CFG_FILES), nf = rd32(g_mem, self + CFG_FILES + TARRAY_NUM), file = 0;
    for (u32 i = 0; i < nf && !file; i++) {
        int d = cmp_gh(fstr_data(files + i * CFG_FILE_SIZE + 4), name);
        if (d < 0)
            return -1;
        if (!d)
            file = files + i * CFG_FILE_SIZE;
    }
    if (!file)
        return -1;  // Find прочёл бы файл с диска
    u32 secs = rd32(g_mem, file + CFG_FILE_SECS), ns = rd32(g_mem, file + CFG_FILE_SECS + TARRAY_NUM), s = 0;
    for (u32 j = 0; j < ns && !s; j++) {
        int d = cmp_gg(fstr_data(secs + j * CFG_SEC_SIZE + 4), sec);
        if (d < 0)
            return -1;
        if (!d)
            s = secs + j * CFG_SEC_SIZE;
    }
    if (!s)
        return 0;
    u32 kv = rd32(g_mem, s + CFG_SEC_PAIRS), nkv = rd32(g_mem, s + CFG_SEC_PAIRS + TARRAY_NUM);
    for (u32 k = nkv; k-- > 0;) {
        int d = cmp_gg(fstr_data(kv + k * CFG_KV_SIZE + 4), key);
        if (d < 0)
            return -1;
        if (!d) {
            *val = fstr_data(kv + k * CFG_KV_SIZE + 0x10);
            return 1;
        }
    }
    return 0;
}

void ovr_launch_cfg_get_string(CPU *c)
{
    u32 self = c->ecx, sec = rd32(g_mem, c->esp + 4), key = rd32(g_mem, c->esp + 8), out = rd32(g_mem, c->esp + 12);
    s32 size = (s32)rd32(g_mem, c->esp + 16);
    u32 fname = rd32(g_mem, c->esp + 20), val = 0;
    int r = size > 0 && size <= 0x100000 ? cfg_lookup(self, sec, key, fname, &val) : -1;
    if (r < 0 || cfg_check()) {
        RT_ENTER_GEN(c, ovr_orig_ovr_launch_cfg_get_string);  // оригинал — код гостя
        if (r < 0)
            return;
        int bad = (u32)r != c->eax;
        for (s32 i = 0, end = 0; !bad && r && i < size; i++) {  // wcsncpy: строка, затем нули; последний — ноль
            u16 want = end || i == size - 1 ? 0 : val ? rd16(g_mem, val + 2 * (u32)i) : 0;
            end |= !want;
            bad = rd16(g_mem, out + 2 * (u32)i) != want;
        }
        if (bad || (!r && rd16(g_mem, out)))
            fprintf(stderr, "[core] SNX_CFG_CHECK: GetString mismatch (native %d, game %u)\n", r, c->eax);
        return;
    }
    if (r) {  // wcsncpy(out, val, size), out[size - 1] = 0
        u32 i = 0;
        for (u16 ch; val && i < (u32)size - 1 && (ch = rd16(g_mem, val + 2 * i)); i++)
            wr16(g_mem, out + 2 * i, ch);
        memset(g_mem + out + 2 * i, 0, 2 * ((u32)size - i));
    } else
        wr16(g_mem, out, 0);
    c->eax = (u32)r;
    c->esp += 24;  // thiscall, пять аргументов: ret 20
}
