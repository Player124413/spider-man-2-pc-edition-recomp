// Headless-запуск рекомпилированного Webhead.exe: те же 12 модулей, что и в game-процессе,
// но без SDL/OK: VFS берёт игры из SNX_GAME, настройки — из SNX_USERDATA.
// playing-цикл работает до timeout (это ожидаемо: игра не завершается сама).
#include <stdio.h>
#include <stdlib.h>

#include "runtime/process.h"
#include "runtime/rt.h"

extern const rt_module_desc mod_msvcr71, mod_core, mod_fire, mod_webhead, mod_webhead_exe;
extern const rt_module_desc mod_window, mod_windrv, mod_engine, mod_d3ddrv, mod_gui;
extern const rt_module_desc mod_gameplay, mod_alaudio;

static const rt_module_desc *const mods[] = {
    &mod_msvcr71, &mod_core, &mod_fire, &mod_webhead, &mod_webhead_exe, &mod_window,
    &mod_windrv, &mod_engine, &mod_d3ddrv, &mod_gui, &mod_gameplay, &mod_alaudio, NULL,
};

int main(void)
{
    extern int g_hle_trace;
    g_hle_trace = getenv("SNX_HLE_TRACE") != NULL;
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *game = getenv("SNX_GAME");
    const char *user = getenv("SNX_USERDATA");
    if (!game) game = "references/coolgame";
    if (!user) user = "build/userdata-smoke";

    if (rt_process_init(mods, game, user, "C:\\Spider-Man2", "") != 0) {
        fprintf(stderr, "HEADLESS FAIL: rt_process_init\n");
        return 1;
    }
    if (rt_module_attach(rt_module_main()) != 0) {
        fprintf(stderr, "HEADLESS FAIL: rt_module_attach\n");
        return 2;
    }
    puts("HEADLESS OK: modules attached, entering Webhead.exe");
    u32 rc = rt_guest_call(&rt_thread_current()->cpu, rt_module_main()->desc->entry, 0, NULL);
    fprintf(stderr, "HEADLESS OK: entry returned %u\n", rc);
    return 0;
}
