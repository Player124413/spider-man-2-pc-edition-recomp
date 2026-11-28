// HLE DirectDraw: game.exe создаёт IDirectDraw только чтобы узнать объём видеопамяти (IDirectDraw7::GetAvailableVidMem).
#include "runtime/hle/com.h"
#include "runtime/hle/d3d8.h"

static u32 g_vt_dd, g_vt_dd7;

static int is_ddraw_iid(u32 iid)
{
    return com_iid_eq(iid, 0x15e65ec0, 0x3b9c, 0x11d2, 0xb92f00609797ea5bull)      // IID_IDirectDraw7
        || com_iid_eq(iid, 0x9c59509a, 0x39bd, 0x11d1, 0x8c4a00c04fd930c5ull)      // IID_IDirectDraw4
        || com_iid_eq(iid, 0xb3a6f3e0, 0x2b43, 0x11cf, 0xa2de00aa00b93356ull)      // IID_IDirectDraw2
        || com_iid_eq(iid, 0x6c14db80, 0xa733, 0x11ce, 0xa52100208b2fe0b4ull)      // IID_IDirectDraw
        || com_iid_eq(iid, 0x00000000, 0x0000, 0x0000, 0xc000000000000046ull);     // IID_IUnknown
}

// QueryInterface(this, riid, ppv): любая версия интерфейса — объект с vtable IDirectDraw7 (v2/v4/v7 совпадают
// по раскладке до GetAvailableVidMem включительно)
static void dd_QueryInterface(CPU *c)
{
    u32 iid = ARG(1), out = ARG(2);
    if (!is_ddraw_iid(iid)) {
        char s[64];
        com_iid_str(iid, s, sizeof s);
        fprintf(stderr, "[ddraw] QueryInterface %s -> E_NOINTERFACE\n", s);
        wr32(g_mem, out, 0);
        RET(3, E_NOINTERFACE);
    }
    wr32(g_mem, out, com_create(g_vt_dd7, NULL, NULL));
    RET(3, S_OK);
}

// GetAvailableVidMem(this, DDSCAPS2*, total*, free*)
static void dd7_GetAvailableVidMem(CPU *c)
{
    if (ARG(2))
        wr32(g_mem, ARG(2), d3d8_vidmem_total());
    if (ARG(3))
        wr32(g_mem, ARG(3), d3d8_vidmem_total() - 8u * 1024 * 1024);
    RET(4, S_OK);
}

static void dd_SetCooperativeLevel(CPU *c) { RET(3, S_OK); }

static void init_vtables(void)
{
    if (g_vt_dd)
        return;
    guest_fn dd[23] = {dd_QueryInterface, com_AddRef, com_Release};
    dd[0x50 / 4] = dd_SetCooperativeLevel;
    g_vt_dd = com_vtable("IDirectDraw", dd, 23);
    guest_fn dd7[30] = {dd_QueryInterface, com_AddRef, com_Release};
    dd7[0x50 / 4] = dd_SetCooperativeLevel;
    dd7[0x5C / 4] = dd7_GetAvailableVidMem;
    g_vt_dd7 = com_vtable("IDirectDraw7", dd7, 30);
}

// DirectDrawCreate(GUID *driver, IDirectDraw **out, IUnknown *outer)
void hle_ddraw_DirectDrawCreate(CPU *c)
{
    HLE_ENTER("DirectDrawCreate", 3);
    init_vtables();
    wr32(g_mem, ARG(1), com_create(g_vt_dd, NULL, NULL));
    RET(3, S_OK);
}
