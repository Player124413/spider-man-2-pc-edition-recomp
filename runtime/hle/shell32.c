// HLE最小 shell32: Webhead.exe ищет «Мои документы» для каталога сохранений.
// Реальных ярлыков/оболочек в порте нет: описатель папки — синтетический,
// SHGetPathFromIDList возвращает путь в каталог игры.
#include <stdio.h>
#include <string.h>

#include "runtime/hle/hle.h"
#include "runtime/process.h"

#define PIDL_MAGIC 0x00070000u

void hle_shell32_SHGetSpecialFolderLocation(CPU *c)
{
    HLE_ENTER("SHGetSpecialFolderLocation", 3);
    int folder = (int)ARG(1);
    if (ARG(2))
        wr32(g_mem, ARG(2), PIDL_MAGIC | (u32)(folder & 0xFFFF));
    RET(3, 0);  // S_OK
}

static const char *folder_path(u32 pidl, char *out, size_t cap)
{
    int folder = (int)(pidl & 0xFFFF);
    const char *root = rt_guest_root();
    // CSIDL_PERSONAL (5) — «Мои документы», CSIDL_APPDATA (26) — настройки.
    if (folder == 5 || folder == 26)
        snprintf(out, cap, "%s\\Saves", root);
    else
        snprintf(out, cap, "%s", root);
    return out;
}

void hle_shell32_SHGetPathFromIDListA(CPU *c)
{
    HLE_ENTER("SHGetPathFromIDListA", 2);
    char path[600];
    u32 dst = ARG(0), pidl = ARG(1);
    if (!dst || !(pidl & PIDL_MAGIC)) {
        RET(2, 0);
        return;
    }
    RET(2, hle_put_a(dst, 260, folder_path(pidl, path, sizeof path)) != 0);
}

void hle_shell32_SHGetPathFromIDListW(CPU *c)
{
    HLE_ENTER("SHGetPathFromIDListW", 2);
    char path[600];
    u32 dst = ARG(0), pidl = ARG(1), n = 0;
    if (!dst || !(pidl & PIDL_MAGIC)) {
        RET(2, 0);
        return;
    }
    folder_path(pidl, path, sizeof path);
    for (; n < 259 && path[n]; n++)
        wr16(g_mem, dst + 2 * n, (u8)path[n]);
    wr16(g_mem, dst + 2 * n, 0);
    RET(2, 1);
}

