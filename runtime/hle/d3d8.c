// HLE Direct3D 8: IDirect3D8 (адаптер, режимы, форматы, caps). Профиль — GeForce4 Ti 4600 (NV25, 128 МБ):
// HW T&L, vs.1.1, ps.1.3, DXT1-5, 4 текстуры за проход — карта «высокой детализации» 2004 года, которой нет
// в VideoCard.ini (значит, без ограничений). Устройство — d3d8_dev.c, отрисовка — d3d8_ff.c.
#include <stdio.h>
#include <string.h>

#include "runtime/hle/com.h"
#include "runtime/hle/d3d8.h"
#include "runtime/hle/d3d8_int.h"
#include "runtime/hle/user32.h"

u32 d3d8_vidmem_total(void) { return 128u * 1024 * 1024; }

static void wrf(u32 a, float f)
{
    u32 v;
    memcpy(&v, &f, 4);
    wr32(g_mem, a, v);
}

// ================================================================ режимы экрана
static const u16 g_modes[][2] = {
    {640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 960},
    {1280, 1024}, {1600, 1200}, {1920, 1080},
};
#define NMODES (int)(sizeof g_modes / sizeof g_modes[0])
static const u32 g_mode_fmts[2] = {FMT_X8R8G8B8, FMT_R5G6B5};

// Список режимов = таблица + размер экрана порта (user32_screen: на телефоне — экран x доля разрешения, например
// 1200x540 на 20:9), если его нет в таблице; по возрастанию ширины, затем высоты. Полноэкранный D3DRenderDevice
// берёт из списка ближайший к FullscreenViewportX/Y режим: без размера экрана в списке кадр на 20:9–21:9
// становился 16:9 (1280x720 / 1920x1080) с чёрными полосами по бокам даже в режиме «Весь экран».
// Возвращает число режимов одного формата; mode_at — i-й из них.
static int mode_list(u16 out[NMODES + 1][2])
{
    int sw, sh, n = 0, extra = 1;
    user32_screen(&sw, &sh);
    for (int i = 0; i < NMODES; i++)
        if (g_modes[i][0] == sw && g_modes[i][1] == sh)
            extra = 0;
    if (sw <= 0 || sh <= 0 || sw > 0xFFFF || sh > 0xFFFF)
        extra = 0;
    for (int i = 0; i < NMODES; i++) {
        if (extra && (sw < g_modes[i][0] || (sw == g_modes[i][0] && sh < g_modes[i][1]))) {
            out[n][0] = (u16)sw;
            out[n++][1] = (u16)sh;
            extra = 0;
        }
        out[n][0] = g_modes[i][0];
        out[n++][1] = g_modes[i][1];
    }
    if (extra) {
        out[n][0] = (u16)sw;
        out[n++][1] = (u16)sh;
    }
    static int logged;
    if (!logged && n > NMODES) {
        logged = 1;
        fprintf(stderr, "[d3d8] display modes: + screen %dx%d\n", sw, sh);
    }
    return n;
}

static void put_mode(u32 p, u32 w, u32 h, u32 fmt)
{
    wr32(g_mem, p, w);
    wr32(g_mem, p + 4, h);
    wr32(g_mem, p + 8, 60);
    wr32(g_mem, p + 12, fmt);
}

static int is_display_fmt(u32 f) { return f == FMT_X8R8G8B8 || f == FMT_R5G6B5; }

// ================================================================ caps
void d3d8_fill_caps(u32 p)
{
    memset(g_mem + p, 0, 0xD4);
    wr32(g_mem, p + 0x00, 1);            // DeviceType: HAL
    wr32(g_mem, p + 0x08, 0x00020000);   // Caps: READ_SCANLINE
    wr32(g_mem, p + 0x0C, 0x200A0000);   // Caps2: FULLSCREENGAMMA | CANRENDERWINDOWED | DYNAMICTEXTURES
    wr32(g_mem, p + 0x10, 0x20);         // Caps3: ALPHA_FULLSCREEN_FLIP_OR_DISCARD
    wr32(g_mem, p + 0x14, 0x80000003);   // PresentationIntervals: IMMEDIATE | ONE | TWO
    wr32(g_mem, p + 0x18, 3);            // CursorCaps: COLOR | LOWRES
    wr32(g_mem, p + 0x1C, 0x001BBEF0);   // DevCaps: …, HWTRANSFORMANDLIGHT, HWRASTERIZATION, PUREDEVICE
    wr32(g_mem, p + 0x20, 0x00000CF2);   // PrimitiveMiscCaps: MASKZ, CULL*, COLORWRITEENABLE, TSSARGTEMP, BLENDOP
    wr32(g_mem, p + 0x24, 0x00736191);   // RasterCaps: DITHER, ZTEST, FOGVERTEX/TABLE/RANGE, LODBIAS, ZBIAS, ANISO, W/ZFOG
    wr32(g_mem, p + 0x28, 0xFF);         // ZCmpCaps
    wr32(g_mem, p + 0x2C, 0x1FFF);       // SrcBlendCaps
    wr32(g_mem, p + 0x30, 0x1FFF);       // DestBlendCaps
    wr32(g_mem, p + 0x34, 0xFF);         // AlphaCmpCaps
    wr32(g_mem, p + 0x38, 0x00084208);   // ShadeCaps: gouraud color/specular/alpha/fog
    wr32(g_mem, p + 0x3C, 0x0007ED47);   // TextureCaps: POW2+NONPOW2CONDITIONAL, ALPHA, PROJECTED, CUBE/VOLUME, MIP*
    wr32(g_mem, p + 0x40, 0x07030700);   // TextureFilterCaps: point/linear/aniso min/mag, point/linear mip
    wr32(g_mem, p + 0x44, 0x03030300);   // CubeTextureFilterCaps
    wr32(g_mem, p + 0x48, 0x03030300);   // VolumeTextureFilterCaps
    wr32(g_mem, p + 0x4C, 0x3F);         // TextureAddressCaps: WRAP MIRROR CLAMP BORDER INDEPENDENTUV MIRRORONCE
    wr32(g_mem, p + 0x50, 0x3F);         // VolumeTextureAddressCaps
    wr32(g_mem, p + 0x54, 0x1F);         // LineCaps
    wr32(g_mem, p + 0x58, 4096);         // MaxTextureWidth
    wr32(g_mem, p + 0x5C, 4096);         // MaxTextureHeight
    wr32(g_mem, p + 0x60, 512);          // MaxVolumeExtent
    wr32(g_mem, p + 0x64, 8192);         // MaxTextureRepeat
    wr32(g_mem, p + 0x68, 4096);         // MaxTextureAspectRatio
    wr32(g_mem, p + 0x6C, 8);            // MaxAnisotropy
    wrf(p + 0x70, 1e10f);                // MaxVertexW
    wrf(p + 0x74, -1e8f);                // GuardBandLeft
    wrf(p + 0x78, -1e8f);                // GuardBandTop
    wrf(p + 0x7C, 1e8f);                 // GuardBandRight
    wrf(p + 0x80, 1e8f);                 // GuardBandBottom
    wrf(p + 0x84, 0.0f);                 // ExtentsAdjust
    wr32(g_mem, p + 0x88, 0xFF);         // StencilCaps
    wr32(g_mem, p + 0x8C, 0x00180008);   // FVFCaps: 8 текстурных координат, DONOTSTRIPELEMENTS, PSIZE
    wr32(g_mem, p + 0x90, 0x03FEFFFF);   // TextureOpCaps: всё, кроме PREMODULATE
    wr32(g_mem, p + 0x94, 8);            // MaxTextureBlendStages
    wr32(g_mem, p + 0x98, 4);            // MaxSimultaneousTextures
    wr32(g_mem, p + 0x9C, 0x7B);         // VertexProcessingCaps: TEXGEN MATERIALSOURCE7 DIR/POS LIGHTS LOCALVIEWER TWEENING
    wr32(g_mem, p + 0xA0, 8);            // MaxActiveLights
    wr32(g_mem, p + 0xA4, 6);            // MaxUserClipPlanes
    wr32(g_mem, p + 0xA8, 4);            // MaxVertexBlendMatrices
    wr32(g_mem, p + 0xAC, 0);            // MaxVertexBlendMatrixIndex
    wrf(p + 0xB0, 64.0f);                // MaxPointSize
    wr32(g_mem, p + 0xB4, 0xFFFFF);      // MaxPrimitiveCount
    wr32(g_mem, p + 0xB8, 0xFFFFF);      // MaxVertexIndex
    wr32(g_mem, p + 0xBC, 16);           // MaxStreams
    wr32(g_mem, p + 0xC0, 255);          // MaxStreamStride
    wr32(g_mem, p + 0xC4, 0xFFFE0101);   // VertexShaderVersion 1.1
    wr32(g_mem, p + 0xC8, 96);           // MaxVertexShaderConst
    wr32(g_mem, p + 0xCC, 0xFFFF0103);   // PixelShaderVersion 1.3
    wrf(p + 0xD0, 1.0f);                 // MaxPixelShaderValue
}

// ================================================================ IDirect3D8
static u32 g_vt_d3d8;

static int is_d3d8_iid(u32 iid)
{
    return com_iid_eq(iid, 0x1dd9e8da, 0x1c77, 0x4d40, 0xb0cf98fefdff9512ull)  // IID_IDirect3D8
        || com_iid_eq(iid, 0x00000000, 0x0000, 0x0000, 0xc000000000000046ull); // IID_IUnknown
}

static void d3d_QueryInterface(CPU *c)
{
    if (!is_d3d8_iid(ARG(1))) {
        wr32(g_mem, ARG(2), 0);
        RET(3, E_NOINTERFACE);
    }
    com_addref(ARG(0));
    wr32(g_mem, ARG(2), ARG(0));
    RET(3, S_OK);
}

static void d3d_GetAdapterCount(CPU *c) { RET(1, 1); }

// GetAdapterIdentifier(this, adapter, flags, D3DADAPTER_IDENTIFIER8*)
static void d3d_GetAdapterIdentifier(CPU *c)
{
    if (ARG(1) != 0)
        RET(4, D3DERR_INVALIDCALL);
    u32 p = ARG(3);
    memset(g_mem + p, 0, 0x42C);
    strcpy((char *)g_mem + p, "nv4_disp.dll");
    strcpy((char *)g_mem + p + 512, "NVIDIA GeForce4 Ti 4600");
    wr32(g_mem, p + 1024, (10u << 16) | 6177u);  // DriverVersion 6.14.10.6177
    wr32(g_mem, p + 1028, (6u << 16) | 14u);
    wr32(g_mem, p + 1032, 0x10DE);               // VendorId
    wr32(g_mem, p + 1036, 0x0250);               // DeviceId
    wr32(g_mem, p + 1040, 0);                    // SubSysId
    wr32(g_mem, p + 1044, 0xA3);                 // Revision
    static const u8 guid[16] = {0x10, 0xDE, 0x02, 0x50, 0x4E, 0x56, 0x32, 0x35, 0x53, 0x4E, 0x58, 0x00, 0x00, 0x00, 0x00, 0x01};
    memcpy(g_mem + p + 1048, guid, 16);
    wr32(g_mem, p + 1064, (ARG(2) & 2) ? 0 : 1); // WHQLLevel (D3DENUM_NO_WHQL_LEVEL -> 0)
    RET(4, D3D_OK);
}

static void d3d_GetAdapterModeCount(CPU *c)
{
    u16 l[NMODES + 1][2];
    RET(2, ARG(1) == 0 ? (u32)(mode_list(l) * 2) : 0);
}

// EnumAdapterModes(this, adapter, mode, D3DDISPLAYMODE*): сначала все 32-битные, потом 16-битные
static void d3d_EnumAdapterModes(CPU *c)
{
    u16 l[NMODES + 1][2];
    u32 m = ARG(2), n = (u32)mode_list(l);
    if (ARG(1) != 0 || m >= n * 2)
        RET(4, D3DERR_INVALIDCALL);
    put_mode(ARG(3), l[m % n][0], l[m % n][1], g_mode_fmts[m / n]);
    RET(4, D3D_OK);
}

static void d3d_GetAdapterDisplayMode(CPU *c)
{
    int w, h;
    user32_screen(&w, &h);
    put_mode(ARG(2), (u32)w, (u32)h, FMT_X8R8G8B8);
    RET(3, D3D_OK);
}

// CheckDeviceType(this, adapter, devtype, displayfmt, backbufferfmt, windowed)
static void d3d_CheckDeviceType(CPU *c)
{
    u32 disp = ARG(3), bb = ARG(4);
    int ok = ARG(1) == 0 && ARG(2) == 1 && is_display_fmt(disp)
          && (bb == disp || (disp == FMT_X8R8G8B8 && bb == FMT_A8R8G8B8));
    RET(6, ok ? D3D_OK : D3DERR_NOTAVAILABLE);
}

static int texture_fmt(u32 f)
{
    switch (f) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5: case FMT_A4R4G4B4:
    case FMT_A8: case FMT_L8: case FMT_A8L8: case FMT_V8U8:
    case FMT_DXT1: case FMT_DXT2: case FMT_DXT3: case FMT_DXT4: case FMT_DXT5:
        return 1;
    default:
        return 0;
    }
}

// CheckDeviceFormat(this, adapter, devtype, adapterfmt, usage, rtype, checkfmt)
static void d3d_CheckDeviceFormat(CPU *c)
{
    u32 usage = ARG(4), rtype = ARG(5), f = ARG(6);
    int ok;
    if (usage & USAGE_DEPTHSTENCIL)
        ok = f == FMT_D16 || f == FMT_D24S8 || f == FMT_D24X8 || f == FMT_D16_LOCKABLE;
    else if (usage & USAGE_RENDERTARGET)
        ok = f == FMT_A8R8G8B8 || f == FMT_X8R8G8B8 || f == FMT_R5G6B5;
    else if (rtype == RTYPE_VOLUMETEXTURE)
        ok = texture_fmt(f) && f != FMT_V8U8;
    else
        ok = texture_fmt(f) || (rtype == RTYPE_SURFACE && (f == FMT_D16 || f == FMT_D24S8 || f == FMT_D24X8));
    ok = ok && ARG(1) == 0 && is_display_fmt(ARG(3));
    if (g_hle_trace)
        fprintf(stderr, "[d3d8] CheckDeviceFormat usage %x rtype %u fmt %08x -> %s\n", usage, rtype, f, ok ? "ok" : "no");
    RET(7, ok ? D3D_OK : D3DERR_NOTAVAILABLE);
}

// CheckDeviceMultiSampleType(this, adapter, devtype, fmt, windowed, type): 2x/4x — есть
static void d3d_CheckDeviceMultiSampleType(CPU *c)
{
    u32 t = ARG(5);
    RET(6, t == 0 || t == 2 || t == 4 ? D3D_OK : D3DERR_NOTAVAILABLE);
}

// CheckDepthStencilMatch(this, adapter, devtype, adapterfmt, rtfmt, dsfmt)
static void d3d_CheckDepthStencilMatch(CPU *c) { RET(6, D3D_OK); }

static void d3d_GetDeviceCaps(CPU *c)
{
    if (ARG(1) != 0 || ARG(2) != 1)
        RET(4, D3DERR_INVALIDCALL);
    d3d8_fill_caps(ARG(3));
    RET(4, D3D_OK);
}

static void d3d_GetAdapterMonitor(CPU *c) { RET(2, ARG(1) == 0 ? 0x00010020u : 0); }

// CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppDevice)
static void d3d_CreateDevice(CPU *c)
{
    fprintf(stderr, "[d3d8] CreateDevice adapter %u type %u hwnd %08x flags %08x\n", ARG(1), ARG(2), ARG(3), ARG(4));
    d3d8_set_d3d_object(ARG(0));
    u32 dev = ARG(1) == 0 ? d3d8_device_create(c, ARG(3), ARG(5)) : 0;
    wr32(g_mem, ARG(6), dev);
    RET(7, dev ? D3D_OK : D3DERR_NOTAVAILABLE);
}

static void init_vtables(void)
{
    if (g_vt_d3d8)
        return;
    guest_fn m[16] = {
        d3d_QueryInterface, com_AddRef, com_Release, NULL /* RegisterSoftwareDevice */,
        d3d_GetAdapterCount, d3d_GetAdapterIdentifier, d3d_GetAdapterModeCount, d3d_EnumAdapterModes,
        d3d_GetAdapterDisplayMode, d3d_CheckDeviceType, d3d_CheckDeviceFormat, d3d_CheckDeviceMultiSampleType,
        d3d_CheckDepthStencilMatch, d3d_GetDeviceCaps, d3d_GetAdapterMonitor, d3d_CreateDevice,
    };
    g_vt_d3d8 = com_vtable("IDirect3D8", m, 16);
}

void hle_d3d8_Direct3DCreate8(CPU *c)
{
    HLE_ENTER("Direct3DCreate8", 1);
    if (ARG(0) != 220)
        fprintf(stderr, "[d3d8] Direct3DCreate8: SDK version %u\n", ARG(0));
    init_vtables();
    RET(1, com_create_st(g_vt_d3d8, NULL, NULL));
}
