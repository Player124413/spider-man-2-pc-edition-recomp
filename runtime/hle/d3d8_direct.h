// Прямой C-интерфейс HLE Direct3D 8 для нативных оверрайдов D3DDrv/Engine:
// обновляет теневое состояние устройства (или активный StateBlock) и отправляет
// компактные команды D3D8 в поток рендера gl_cs напрямую — без COM-трамплинов,
// x86-стека и лишнего refcount-шума при неизменных ресурсах.
#pragma once

#include "runtime/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

struct d3ddev;

struct d3ddev *d3d8_dev_from_guest(u32 guest_ptr);
void d3d8_direct_set_rs(struct d3ddev *d, u32 state, u32 val);
void d3d8_direct_set_tss(struct d3ddev *d, u32 stage, u32 type, u32 val);
void d3d8_direct_set_transform(struct d3ddev *d, u32 type, const float *mat16);
void d3d8_direct_set_material(struct d3ddev *d, const void *mat68);
void d3d8_direct_set_light(struct d3ddev *d, u32 idx, const void *light104);
void d3d8_direct_light_enable(struct d3ddev *d, u32 idx, u32 enable);
void d3d8_direct_set_texture(struct d3ddev *d, u32 stage, u32 tex_guest_ptr);
void d3d8_direct_set_stream(struct d3ddev *d, u32 idx, u32 vb_guest_ptr, u32 stride);
void d3d8_direct_set_indices(struct d3ddev *d, u32 ib_guest_ptr, u32 base_vertex);
void d3d8_direct_set_vs(struct d3ddev *d, u32 handle);
void d3d8_direct_set_ps(struct d3ddev *d, u32 handle);
void d3d8_direct_set_vs_const(struct d3ddev *d, u32 reg, const float *data, u32 count);
void d3d8_direct_set_ps_const(struct d3ddev *d, u32 reg, const float *data, u32 count);
void d3d8_direct_set_viewport(struct d3ddev *d, const void *vp24);
void d3d8_direct_set_clip_plane(struct d3ddev *d, u32 idx, const float *plane4);

// Однопроходное GPU-размытие теней-проекторов (9 тапов 3x3 со смещением step_px, вес 28/255).
// src_tex_guest — IDirect3DTexture8* (или IDirect3DSurface8*), dst_surf_guest — IDirect3DSurface8* (0 -> текущий RT).
// Возвращает 1, если команда поставлена в gl_cs, или 0 (нужен откат на оригинальный путь).
int d3d8_direct_shadow_blur(struct d3ddev *d, u32 src_tex_guest, u32 dst_surf_guest, float step_px);

#ifdef __cplusplus
}
#endif
