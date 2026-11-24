// HLE мелких DLL без поведения в порте: comctl32, comdlg32, imm32 (IME выключен), ifc23 (Immersion force feedback —
// устройства нет). binkw32 — bink.c.
#include <stdlib.h>

#include "runtime/hle/hle.h"

// Хук хоста для вибрации геймпада (регистрируется в host/boot/input.c; в тестах без хоста остаётся NULL).
void (*hle_rumble_hook)(u16 low, u16 high, u32 ms);

// ================================================================ comctl32, comdlg32
void hle_comctl32_ord17(CPU *c) { HLE_ENTER("InitCommonControls", 0); RET(0, 0); }
void hle_comdlg32_ChooseColorA(CPU *c) { HLE_ENTER("ChooseColorA", 1); RET(1, 0); }        // отмена
void hle_comdlg32_GetSaveFileNameA(CPU *c) { HLE_ENTER("GetSaveFileNameA", 1); RET(1, 0); }

// ================================================================ imm32
// IMM выключен, как на английской Windows XP без восточноазиатских языков: контекстов нет, ImmCreateContext
// отказывает — WinDrv сбрасывает флаг IME у окна (иначе каждый тик пишет в лог «Creating IME context.»).
void hle_imm32_ImmGetContext(CPU *c) { HLE_ENTER("ImmGetContext", 1); RET(1, 0); }
void hle_imm32_ImmReleaseContext(CPU *c) { HLE_ENTER("ImmReleaseContext", 2); RET(2, 1); }
void hle_imm32_ImmAssociateContext(CPU *c) { HLE_ENTER("ImmAssociateContext", 2); RET(2, 0); }
void hle_imm32_ImmCreateContext(CPU *c) { HLE_ENTER("ImmCreateContext", 0); RET(0, 0); }
void hle_imm32_ImmDestroyContext(CPU *c) { HLE_ENTER("ImmDestroyContext", 1); RET(1, 1); }
void hle_imm32_ImmGetCompositionStringW(CPU *c) { HLE_ENTER("ImmGetCompositionStringW", 4); RET(4, 0); }

// ================================================================ ifc23 (C++: методы thiscall — this в ecx, аргументы снимает вызываемый)
// Проверка локальной копии игры (read-only): каталога ForceFeedback/ и файлов .ifr в Shrek 2 нет; Engine.dll
// содержит штатный код UE2 со ссылками на ../ForceFeedback/{other,ifeel}.ifr, а скрипты SHGame.u ForceFeedback
// не вызывают (в Engine.u остались базовые вызовы ClientPlayForceFeedback("Damage") из Pawn.PlayHit /
// PlayerController.Dead). CreateDevice возвращает NULL, чтобы движок не разыменовывал фиктивный vtable устройства;
// при прямых вызовах Start/Stop логируем эффект и передаём импульс в хостовый хук вибрации [проверить].
void hle_ifc23__CreateDevice_CImmDevice__SAPAV1_PAUHINSTANCE____PAUHWND_____Z(CPU *c)
{
    HLE_ENTER("CImmDevice::CreateDevice", 2);
    RET(0, 0);  // аргументы снимает вызывающий
}
void hle_ifc23___0CImmProject__QAE_XZ(CPU *c) { HLE_ENTER("CImmProject::CImmProject", 0); RET(0, c->ecx); }
void hle_ifc23___1CImmProject__QAE_XZ(CPU *c) { HLE_ENTER("CImmProject::~CImmProject", 0); RET(0, 0); }
void hle_ifc23__OpenFile_CImmProject__QAEHPBDPAVCImmDevice___Z(CPU *c)
{
    HLE_ENTER("CImmProject::OpenFile", 2);
    if (g_hle_trace || getenv("SNX_LOG_RUMBLE"))
        fprintf(stderr, "[ifc23] OpenFile(\"%s\")\n", hle_cstr(ARG(0)));
    RET(2, 0);
}
void hle_ifc23__Start_CImmProject__QAEHPBDKKPAVCImmDevice___Z(CPU *c)
{
    HLE_ENTER("CImmProject::Start", 4);
    const char *name = hle_cstr(ARG(0));
    fprintf(stderr, "[ifc23] Start(\"%s\", iter=%u, flags=0x%x)\n", name, ARG(1), ARG(2));
    if (hle_rumble_hook)
        hle_rumble_hook(0x6000u, 0xA000u, 180u);
    RET(4, 0);
}
void hle_ifc23__Stop_CImmProject__QAEHPBD_Z(CPU *c)
{
    HLE_ENTER("CImmProject::Stop", 1);
    const char *name = hle_cstr(ARG(0));
    fprintf(stderr, "[ifc23] Stop(\"%s\")\n", name);
    if (hle_rumble_hook)
        hle_rumble_hook(0, 0, 0);
    RET(1, 0);
}
void hle_ifc23__GetNumEffects_CImmProject__QAEHXZ(CPU *c) { HLE_ENTER("CImmProject::GetNumEffects", 0); RET(0, 0); }
void hle_ifc23__GetEffectName_CImmProject__QAEPBDH_Z(CPU *c) { HLE_ENTER("CImmProject::GetEffectName", 1); RET(1, 0); }
void hle_ifc23__GetEffectType_CImmProject__QAEKPBD_Z(CPU *c) { HLE_ENTER("CImmProject::GetEffectType", 1); RET(1, 0); }
void hle_ifc23__CreateEffectByIndex_CImmProject__QAEPAVCImmCompoundEffect__HPAVCImmDevice__K_Z(CPU *c)
{
    HLE_ENTER("CImmProject::CreateEffectByIndex", 3);
    RET(3, 0);
}
