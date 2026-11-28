/* Минимальная проверка рекомпиляции: грузим все 12 переведённых модулей Spider-Man 2
 * в гостевую память и вызываем реальный сгенерированный код из Core.dll.
 *
 * Сборка: tools/build_smoke.sh. Никаких DLL/EXE игры при линковке не используется —
 * только C, полученный из них рекомпилятором, и рантайм shrekNX.
 */
#include <stdio.h>
#include <string.h>

#include "runtime/heap.h"
#include "runtime/process.h"
#include "runtime/rt.h"

extern const rt_module_desc mod_msvcr71, mod_core, mod_fire, mod_webhead, mod_webhead_exe;
extern const rt_module_desc mod_window, mod_windrv, mod_engine, mod_d3ddrv, mod_gui;
extern const rt_module_desc mod_gameplay, mod_alaudio;

static const rt_module_desc *const mods[] = {
    &mod_msvcr71, &mod_core, &mod_fire, &mod_webhead, &mod_webhead_exe, &mod_window,
    &mod_windrv, &mod_engine, &mod_d3ddrv, &mod_gui, &mod_gameplay, &mod_alaudio, NULL,
};

int main(int argc, char **argv)
{
    const char *game = argc > 1 ? argv[1] : "references/coolgame";
    const char *user = argc > 2 ? argv[2] : "build/userdata-smoke";

    if (rt_process_init(mods, game, user, "C:\\Spider-Man2", "") != 0) {
        fprintf(stderr, "SMOKE FAIL: rt_process_init\n");
        return 1;
    }
    printf("SMOKE: загружено %zu рекомпилированных модулей\n", sizeof mods / sizeof mods[0] - 1);

    rt_module *core = rt_module_by_name("Core");
    u32 strlen_w = core ? rt_module_export(core, "?appStrlen@@YAHPBG@Z", 0) : 0;
    u32 memcmp = core ? rt_module_export(core, "?appMemcmp@@YAHPBX0H@Z", 0) : 0;
    if (!strlen_w || !memcmp) {
        fprintf(stderr, "SMOKE FAIL: экспорты Core.dll не найдены\n");
        return 2;
    }

    u32 heap = heap_process();
    u32 str = heap_alloc(heap, HEAP_ZERO_MEMORY, 64);
    u32 a = heap_alloc(heap, HEAP_ZERO_MEMORY, 16);
    u32 b = heap_alloc(heap, HEAP_ZERO_MEMORY, 16);
    if (!str || !a || !b) {
        fprintf(stderr, "SMOKE FAIL: heap_alloc\n");
        return 3;
    }
    static const u16 text[] = { 'S','p','i','d','e','r','-','M','a','n',' ','2', 0 };
    for (u32 i = 0; i < sizeof text / sizeof text[0]; i++)
        wr16(g_mem, str + 2 * i, text[i]);
    for (u32 i = 0; i < 8; i++)
        g_mem[a + i] = g_mem[b + i] = (u8)i;
    g_mem[b + 3] = 0x7F;  // 4-й байт отличается: первые 3 совпадают

    CPU *c = &rt_thread_current()->cpu;
    u32 args[3] = { str, 0, 0 };
    u32 got = rt_guest_call(c, strlen_w, 1, args);
    printf("SMOKE: Core!appStrlen(L\"Spider-Man 2\") = %u (ожидается 12)\n", got);
    args[0] = a; args[1] = b; args[2] = 3;
    u32 eq = rt_guest_call(c, memcmp, 3, args);
    printf("SMOKE: Core!appMemcmp(buf1, buf2, 3) = %d (ожидается 0)\n", (int)eq);
    args[2] = 4;
    u32 ne = rt_guest_call(c, memcmp, 3, args);
    printf("SMOKE: Core!appMemcmp(buf1, buf2, 4) = %d (ожидается -1)\n", (int)ne);

    if (got != 12 || eq != 0 || ne != 0xFFFFFFFFu) {
        fprintf(stderr, "SMOKE FAIL: результат вызова сгенерированного кода неверен\n");
        return 4;
    }
    puts("SMOKE OK");
    return 0;
}
