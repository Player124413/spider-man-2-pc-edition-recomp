// Настройки порта в ini (host/boot/config.c) для файлов в UTF-16LE с BOM, как их пишет игра, если в тексте есть
// символ вне ANSI (русская версия). Сценарии: DefUser.ini в UTF-16 — User.ini остаётся UTF-16 (чётная длина, BOM,
// кириллица цела) и получает привязки геймпада Joy*; User.ini прежних сборок (UTF-16 + 8-битный хвост
// «\n[Engine.Input]\nJoy1=Fire», нечётная длина — игра читает его пустым) — хвост отрезан, привязки в UTF-16;
// 8-битный ini остаётся 8-битным. Каталоги игры и пользователя — заглушки VFS в <каталог>/game и <каталог>/user.
// Запуск: ini_test <пустой каталог>
#define _GNU_SOURCE  // memmem
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef unsigned char u8;

#include "host/boot/config.h"
#include "runtime/game/menu.h"
#include "runtime/hle/vfs.h"

static char g_dir[512];
static int g_fail;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                 \
            fputc('\n', stderr);                          \
            g_fail = 1;                                   \
        }                                                 \
    } while (0)

static int exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

int vfs_find(const char *rel, char *host, size_t cap)
{
    snprintf(host, cap, "%s/user/%s", g_dir, rel);
    if (exists(host))
        return 1;
    snprintf(host, cap, "%s/game/%s", g_dir, rel);
    return exists(host);
}

int vfs_find_game(const char *rel, char *host, size_t cap)
{
    snprintf(host, cap, "%s/game/%s", g_dir, rel);
    return exists(host);
}

int vfs_for_write(const char *rel, char *host, size_t cap, int copy_existing)
{
    (void)copy_existing;  // game.ini игры в тестах нет
    snprintf(host, cap, "%s/user/%s", g_dir, rel);
    return 1;
}

void vfs_changed(void) {}
void game_menu_set_language(const char *lang) { (void)lang; }

static void put(const char *path, const void *p, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n)
        fprintf(stderr, "cannot write %s\n", path), exit(2);
    fclose(f);
}

static char *get(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc(*n + 1);
    if (fread(p, 1, *n, f) != *n)
        *n = 0;
    fclose(f);
    return p;
}

// UTF-8 -> UTF-16LE с BOM (только BMP)
static size_t wide(const char *s, char *out)
{
    size_t o = 0;
    out[o++] = (char)0xFF;
    out[o++] = (char)0xFE;
    for (size_t i = 0; s[i];) {
        unsigned u = (unsigned char)s[i++];
        if (u >= 0xE0) {
            u = (u & 0x0F) << 12 | ((unsigned char)s[i] & 0x3F) << 6 | ((unsigned char)s[i + 1] & 0x3F);
            i += 2;
        } else if (u >= 0xC0) {
            u = (u & 0x1F) << 6 | ((unsigned char)s[i++] & 0x3F);
        }
        out[o++] = (char)(u & 0xFF);
        out[o++] = (char)(u >> 8);
    }
    return o;
}

// есть ли в UTF-16 (после BOM) подстрока ASCII
static int wide_has(const char *w, size_t n, const char *sub)
{
    size_t k = strlen(sub);
    for (size_t i = 2; i + 2 * k <= n; i += 2) {
        size_t j = 0;
        while (j < k && w[i + 2 * j] == sub[j] && w[i + 2 * j + 1] == 0)
            j++;
        if (j == k)
            return 1;
    }
    return 0;
}

static const char *const DEFUSER =
    "[Engine.Input]\r\n"
    "Aliases[0]=(Command=\"Button bFire | Fire\",Alias=Fire)\r\n"
    "W=MoveForward\r\n"
    "LeftMouse=Fire\r\n"
    "\r\n"
    "[DefaultPlayer]\r\n"
    "Name=Шрек\r\n";

static void defuser_wide(const char *user_ini, const char *game_ini, char *buf)
{
    char path[700];
    snprintf(path, sizeof path, "%s/game/System/DefUser.ini", g_dir);
    put(path, buf, wide(DEFUSER, buf));
    remove(user_ini);
    remove(game_ini);
    put(game_ini, "[Engine.Engine]\r\nLanguage=rus\r\n", 31);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: ini_test <dir>\n");
        return 2;
    }
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    char path[700], user_ini[700], game_ini[700];
    mkdir(g_dir, 0755);
    snprintf(path, sizeof path, "%s/game", g_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/game/System", g_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/user", g_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/user/System", g_dir);
    mkdir(path, 0755);
    snprintf(user_ini, sizeof user_ini, "%s/user/System/User.ini", g_dir);
    snprintf(game_ini, sizeof game_ini, "%s/user/System/Webhead.ini", g_dir);  // Spider-Man 2, не Shrek 2
    static char buf[1 << 16];
    size_t n = 0;
    char *u, v[256];

    // 1. первый запуск, DefUser.ini в UTF-16: User.ini — его копия в UTF-16 с привязками геймпада
    defuser_wide(user_ini, game_ini, buf);
    boot_config_apply(1280, 720);
    u = get(user_ini, &n);
    CHECK(u && n >= 2 && (u8)u[0] == 0xFF && (u8)u[1] == 0xFE, "User.ini lost BOM");
    CHECK(u && !(n & 1), "User.ini odd length %zu", n);
    CHECK(u && wide_has(u, n, "Joy1=Fire") && wide_has(u, n, "JoyX=Axis aStrafe"), "no Joy bindings in UTF-16");
    CHECK(u && wide_has(u, n, "Aliases[0]=") && wide_has(u, n, "W=MoveForward"), "DefUser bindings lost");
    CHECK(u && memmem(u, n, "\x28\x04\x40\x04\x35\x04\x3a\x04", 8), "Cyrillic name damaged");
    CHECK(boot_ini_get(user_ini, "Engine.Input", "Joy2", v, sizeof v) && !strcmp(v, "Jump"), "Joy2 = '%s'", v);
    CHECK(boot_ini_get(user_ini, "DefaultPlayer", "Name", v, sizeof v) && !strcmp(v, "Шрек"), "Name = '%s'", v);
    free(u);

    // 2. второй запуск: ничего не меняется
    u = get(user_ini, &n);
    size_t n1 = n;
    boot_config_apply(1280, 720);
    char *u2 = get(user_ini, &n);
    CHECK(u && u2 && n == n1 && !memcmp(u, u2, n), "second start changed User.ini");
    free(u);
    free(u2);

    // 3. User.ini прежней сборки: UTF-16 (игра сохранила) + 8-битный хвост с привязками, длина нечётная
    size_t wl = wide(DEFUSER, buf);
    const char *tail = "\n[Engine.Input]\nJoy1=Fire\nJoy10=EscHandler\n";
    memcpy(buf + wl, tail, strlen(tail));
    put(user_ini, buf, wl + strlen(tail));
    CHECK((wl + strlen(tail)) & 1, "test setup: length must be odd");
    boot_config_apply(1280, 720);
    u = get(user_ini, &n);
    CHECK(u && !(n & 1) && (u8)u[0] == 0xFF, "damaged User.ini not repaired (len %zu)", n);
    CHECK(u && !memmem(u, n, "\n[Engine", 9) && !memmem(u, n, "Joy1=Fire", 9), "8-bit tail left in User.ini");
    CHECK(u && wide_has(u, n, "Joy1=Fire") && wide_has(u, n, "Joy10=EscHandler"), "Joy bindings not in UTF-16");
    free(u);

    // 4. 8-битный ini остаётся 8-битным
    put(user_ini, "[Engine.Input]\r\nW=MoveForward\r\n", 31);
    boot_config_apply(1280, 720);
    u = get(user_ini, &n);
    CHECK(u && n > 2 && (u8)u[0] == '[' && memmem(u, n, "Joy1=Fire\r\n", 11), "8-bit User.ini changed encoding");
    free(u);
    u = get(game_ini, &n);
    CHECK(u && u[0] == '[' && memmem(u, n, "UseJoystick=True", 16), "game.ini without UseJoystick");
    free(u);

    // 5. game.ini в UTF-16: ключи — в UTF-16
    put(game_ini, buf, wide("[Engine.Engine]\r\nLanguage=rus\r\n[WinDrv.WindowsClient]\r\nUseJoystick=False\r\n", buf));
    boot_config_apply(1280, 720);
    u = get(game_ini, &n);
    CHECK(u && (u8)u[0] == 0xFF && !(n & 1) && wide_has(u, n, "UseJoystick=True") &&
              wide_has(u, n, "FullscreenViewportX=1280") && !wide_has(u, n, "UseJoystick=False"),
          "game.ini UTF-16 not updated");
    free(u);

    // 6. читы (SNX_CHEATS=1, лаунчер Android): команды на NumPad2-9; без переменной — не пишутся; режим и меню
    // разработчика прежней сборки (NumPad0/NumPad1) стираются
    {
        static const char seed[] = "[Engine.Input]\r\nW=MoveForward\r\nNumPad2=Duck\r\nNumPad0=ToggleDebugMode\r\n"
                                   "NumPad1=ShowDevMenu\r\n";
        put(user_ini, seed, sizeof seed - 1);
    }
    boot_config_apply(1280, 720);
    CHECK(!boot_ini_get(user_ini, "Engine.Input", "NumPad1", v, sizeof v) || !v[0], "NumPad1 not cleared: '%s'", v);
    CHECK(!boot_ini_get(user_ini, "Engine.Input", "NumPad0", v, sizeof v) || !v[0], "NumPad0 not cleared: '%s'", v);
    CHECK(boot_ini_get(user_ini, "Engine.Input", "NumPad2", v, sizeof v) && !strcmp(v, "Duck"), "cheats without SNX_CHEATS");
    setenv("SNX_CHEATS", "1", 1);
    boot_config_apply(1280, 720);
    unsetenv("SNX_CHEATS");
    CHECK(!boot_ini_get(user_ini, "Engine.Input", "NumPad1", v, sizeof v) || !v[0], "NumPad1 = '%s'", v);
    CHECK(boot_ini_get(user_ini, "Engine.Input", "NumPad2", v, sizeof v) && !strcmp(v, "Ghost"), "NumPad2 = '%s'", v);
    CHECK(boot_ini_get(user_ini, "Engine.Input", "NumPad6", v, sizeof v) && !strcmp(v, "AddCoins 100"), "NumPad6 = '%s'", v);
    CHECK(boot_ini_get(user_ini, "Engine.Input", "W", v, sizeof v) && !strcmp(v, "MoveForward"), "W = '%s'", v);

    // 7. размер кадра на телефоне (boot_render_size, SNX_ASPECT): 21:9 во весь экран, с пределом 16:9 и 4:3 —
    // уже экрана; экран уже предела не трогается; масштаб и минимум 360 строк после предела
    int w = 2520, h = 1080;
    CHECK(boot_render_size(&w, &h, 1.0f, NULL) == 0 && w == 2520 && h == 1080, "full: %dx%d", w, h);
    w = 2520, h = 1080;
    CHECK(boot_render_size(&w, &h, 1.0f, "full") == 0 && w == 2520, "full: %dx%d", w, h);
    w = 2520, h = 1080;
    float lim = boot_render_size(&w, &h, 1.0f, "16:9");
    CHECK(lim > 1.77f && lim < 1.78f && w == 1920 && h == 1080, "16:9: %dx%d", w, h);
    w = 2400, h = 1080;
    CHECK(boot_render_size(&w, &h, 0.5f, "4:3") > 0 && w == 720 && h == 540, "4:3 50%%: %dx%d", w, h);
    w = 1920, h = 1080;
    boot_render_size(&w, &h, 1.0f, "21:9");
    CHECK(w == 1920 && h == 1080, "16:9 screen, 21:9 limit: %dx%d", w, h);
    w = 2400, h = 1080;
    boot_render_size(&w, &h, 0.25f, "4:3");
    CHECK(h == 360 && w == 480, "min 360 after limit: %dx%d", w, h);
    w = 2400, h = 1080;
    CHECK(boot_render_size(&w, &h, 1.0f, "abc") == 0 && w == 2400, "garbage aspect: %dx%d", w, h);

    puts(g_fail ? "ini_test: FAIL" : "ini_test: OK");
    return g_fail;
}
