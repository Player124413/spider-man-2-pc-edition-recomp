// Синтетический тест INI-парсера и записи (host/boot/config.c: boot_ini_set / boot_ini_get):
// переводы строк CRLF и LF, новые секции и ключи, регистронезависимость, отсутствие лишней перезаписи,
// пустой файл, пустые значения и усечение буфера.
// Запуск: config_test <временный каталог>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "host/boot/config.h"

static int g_fails;

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

static void write_str(const char *path, const char *s)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "создание %s", path);
    if (f) {
        size_t n = strlen(s);
        CHECK(fwrite(s, 1, n, f) == n, "запись %s", path);
        fclose(f);
    }
}

static char *read_str(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf) {
        size_t got = fread(buf, 1, (size_t)n, f);
        buf[got] = 0;
    }
    fclose(f);
    return buf;
}

static void test_missing_file(const char *dir)
{
    char path[1100], val[64];
    snprintf(path, sizeof path, "%s/no_such.ini", dir);
    remove(path);
    CHECK(boot_ini_get(path, "Sec", "Key", val, sizeof val) == 0, "get отсутствующего файла");
    CHECK(boot_ini_set(path, "Sec", "Key", "1") == -1, "set отсутствующего файла");
}

static void test_crlf_and_no_rewrite(const char *dir)
{
    char path[1100], val[64];
    snprintf(path, sizeof path, "%s/crlf.ini", dir);
    const char *orig =
        "[Alpha]\r\n"
        "FirstKey=One\r\n"
        "FirstKey2=Two\r\n"
        "\r\n"
        "[Beta]\r\n"
        "Flag=True\r\n";
    write_str(path, orig);

    // Чтение без учёта регистра и проверка границ имён ключей/секций
    CHECK(boot_ini_get(path, "alpha", "firstkey", val, sizeof val) == 1 && strcmp(val, "One") == 0,
          "get alpha/firstkey: \"%s\"", val);
    CHECK(boot_ini_get(path, "ALPHA", "FIRSTKEY2", val, sizeof val) == 1 && strcmp(val, "Two") == 0,
          "get ALPHA/FIRSTKEY2: \"%s\"", val);
    CHECK(boot_ini_get(path, "Alpha", "First", val, sizeof val) == 0, "префикс ключа не должен совпадать");
    CHECK(boot_ini_get(path, "Alpha", "Flag", val, sizeof val) == 0, "ключ из другой секции");
    CHECK(boot_ini_get(path, "beta", "flag", val, sizeof val) == 1 && strcmp(val, "True") == 0, "get beta/flag");

    // Без лишней перезаписи при совпадении значения
    struct stat st0, st1;
    CHECK(stat(path, &st0) == 0, "stat до set");
    CHECK(boot_ini_set(path, "ALPHA", "firstkey", "One") == 0, "повторный set того же значения должен вернуть 0");
    CHECK(stat(path, &st1) == 0 && st0.st_ino == st1.st_ino && st0.st_size == st1.st_size,
          "файл не должен перезаписываться при неизменном значении");
    char *raw = read_str(path);
    CHECK(raw && strcmp(raw, orig) == 0, "содержимое изменилось после холостого set");
    free(raw);

    // Изменение существующего ключа: сохраняет регистр имени ключа и CRLF
    CHECK(boot_ini_set(path, "alpha", "firstkey", "Updated") == 1, "обновление FirstKey");
    raw = read_str(path);
    CHECK(raw && strstr(raw, "FirstKey=Updated\r\n") != NULL && strstr(raw, "FirstKey2=Two\r\n") != NULL,
          "после обновления ключа: %s", raw ? raw : "(null)");
    free(raw);

    // Вставка нового ключа в существующую первую секцию (до пустой строки и [Beta])
    CHECK(boot_ini_set(path, "Alpha", "AddedKey", "123") == 1, "добавление AddedKey в [Alpha]");
    CHECK(boot_ini_get(path, "Alpha", "AddedKey", val, sizeof val) == 1 && strcmp(val, "123") == 0, "чтение AddedKey");
    CHECK(boot_ini_get(path, "Beta", "AddedKey", val, sizeof val) == 0, "AddedKey не должен попасть в [Beta]");
    raw = read_str(path);
    CHECK(raw && strstr(raw, "FirstKey2=Two\r\nAddedKey=123\r\n\r\n[Beta]\r\n") != NULL,
          "позиция вставки в [Alpha] с CRLF: %s", raw ? raw : "(null)");
    free(raw);

    // Добавление новой секции в конец с CRLF
    CHECK(boot_ini_set(path, "Gamma", "Mode", "Fast") == 1, "добавление новой секции [Gamma]");
    CHECK(boot_ini_get(path, "gamma", "mode", val, sizeof val) == 1 && strcmp(val, "Fast") == 0, "чтение [Gamma]");
    raw = read_str(path);
    CHECK(raw && strstr(raw, "[Gamma]\r\nMode=Fast\r\n") != NULL, "формат новой секции CRLF: %s", raw ? raw : "(null)");
    free(raw);
}

static void test_lf_and_no_trailing_newline(const char *dir)
{
    char path[1100], val[64];
    snprintf(path, sizeof path, "%s/lf.ini", dir);
    // Файл только с LF и без завершающего перевода строки
    write_str(path, "[Main]\nA=1");

    // Добавление ключа в последнюю секцию без завершающего \n — должно сохранить LF и не добавлять \r
    CHECK(boot_ini_set(path, "main", "B", "2") == 1, "добавление B в LF-файл без \\n");
    char *raw = read_str(path);
    CHECK(raw && strchr(raw, '\r') == NULL && strcmp(raw, "[Main]\nA=1\nB=2\n") == 0,
          "LF-файл после вставки B: \"%s\"", raw ? raw : "(null)");
    free(raw);

    // Снова убираем завершающий \n и добавляем новую секцию
    write_str(path, "[Main]\nA=1");
    CHECK(boot_ini_set(path, "Next", "C", "3") == 1, "добавление [Next] в LF-файл без \\n");
    raw = read_str(path);
    CHECK(raw && strchr(raw, '\r') == NULL && strcmp(raw, "[Main]\nA=1\n[Next]\nC=3\n") == 0,
          "LF-файл после новой секции: \"%s\"", raw ? raw : "(null)");
    free(raw);

    CHECK(boot_ini_get(path, "Main", "A", val, sizeof val) == 1 && strcmp(val, "1") == 0, "get A");
    CHECK(boot_ini_get(path, "Next", "C", val, sizeof val) == 1 && strcmp(val, "3") == 0, "get C");
}

static void test_empty_special_and_truncation(const char *dir)
{
    char path[1100], val[64];
    snprintf(path, sizeof path, "%s/empty.ini", dir);
    write_str(path, "");

    CHECK(boot_ini_get(path, "Sec", "K", val, sizeof val) == 0, "get из пустого файла");
    CHECK(boot_ini_set(path, "Sec", "EmptyVal", "") == 1, "запись пустого значения");
    CHECK(boot_ini_get(path, "Sec", "EmptyVal", val, sizeof val) == 1 && strcmp(val, "") == 0, "чтение пустого значения");
    CHECK(boot_ini_set(path, "Sec", "EmptyVal", "") == 0, "повтор пустого значения -> 0");

    // Значение со знаком '=' и проверка усечения выходного буфера
    CHECK(boot_ini_set(path, "Sec", "Expr", "Axis aBaseY Invert=-1") == 1, "запись значения с '='");
    CHECK(boot_ini_get(path, "Sec", "Expr", val, sizeof val) == 1 && strcmp(val, "Axis aBaseY Invert=-1") == 0,
          "чтение значения с '=': \"%s\"", val);

    char small[5];
    memset(small, 0x5A, sizeof small);
    CHECK(boot_ini_get(path, "Sec", "Expr", small, sizeof small) == 1 && strcmp(small, "Axis") == 0,
          "усечение буфера cap=5: \"%s\"", small);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s <tmp_dir>\n", argv[0]);
        return 2;
    }
    mkdir(argv[1], 0755);
    test_missing_file(argv[1]);
    test_crlf_and_no_rewrite(argv[1]);
    test_lf_and_no_trailing_newline(argv[1]);
    test_empty_special_and_truncation(argv[1]);
    printf("config_test: %d failed\n", g_fails);
    return g_fails != 0;
}
