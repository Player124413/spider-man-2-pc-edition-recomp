// Слабые определения по умолчанию (NULL) для указателей на оригинальные функции гостя ovr_orig_*.
// Вынесены в отдельный TU намеренно: в runtime/overrides_{core,crt,engine}.c объявлены только чистые
// `extern void (*const ovr_orig_*)(CPU *c);`, иначе GCC при `-O3 -fvisibility=hidden` (Switch) сворачивает
// `const __attribute__((weak)) = NULL` в том же TU в константу NULL и вырезает вызовы RT_ENTER_GEN!
// В snx_boot сильные определения из build/gen/*/*_table.c перекрывают эти слабые заглушки; в автономных
// тестах (tests/rt/*) и дифф-тестах отдельных модулей (difftest_*) они позволяют линковаться без чужих таблиц.
#include <stddef.h>

#include "runtime/abi.h"

// Замены, которые мы НЕ включаем в recomp/overrides.py для Spider-Man 2 (они-specific
// для Shrek 2), тем не менее остаются в runtime/overrides_*.c — дать им нулевые
// слабые original-указатели, чтобы автономные сборки линковались.
void (*const ovr_orig_ovr_core_collect_garbage)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_core_struct_link)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_core_save_config)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_core_load_config)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_core_objit_next)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_engine_camera_matrices)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_engine_save_game)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_engine_flush)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_launch_file_write)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_launch_file_read)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_launch_file_size)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_launch_cfg_get_string)(CPU *c) __attribute__((weak)) = NULL;

void (*const ovr_orig_ovr_core_box_add_vec)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_core_vec_safe_normal)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_core_vec_normalize)(CPU *c) __attribute__((weak)) = NULL;

void (*const ovr_orig_ovr_crt_qsort)(CPU *c) __attribute__((weak)) = NULL;

void (*const ovr_orig_ovr_engine_box_check)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_engine_sphere_check)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_engine_skin_verts)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_engine_sprite_fill_vb)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_engine_shadow_get)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_engine_shadow_draw_tile)(CPU *c) __attribute__((weak)) = NULL;

void (*const ovr_orig_ovr_d3ddrv_ehvec_ctor)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_d3ddrv_set_mat_pass)(CPU *c) __attribute__((weak)) = NULL;
void (*const ovr_orig_ovr_d3ddrv_commit_state)(CPU *c) __attribute__((weak)) = NULL;
