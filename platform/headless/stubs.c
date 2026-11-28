// Заглушки подсистем, которые в headless-режиме не нужны (графика, звук, поток рендера).
// Реальные реализации — в runtime/hle/{gl_cs,d3d8*,openal,bink_video}.c и platform/window.c.
#include <stddef.h>
#include "runtime/abi.h"
#include "runtime/hle/hle.h"

void openal_shutdown(void) { }
hle_dyn_fn hle_openal_fns[] = {{NULL, NULL, 0}};

// FreeType в headless-сборке недоступен: все функции возвращают ошибку, и gdi32.c
// использует встроенную моноширинную аппроксимацию размеров текста.
#include "ft_stub.h"
FT_Error FT_Init_FreeType(FT_Library *l) { *l = NULL; return 1; }
FT_Error FT_Done_FreeType(FT_Library l) { (void)l; return 0; }
FT_Error FT_New_Face(FT_Library l, const char *p, FT_Long i, FT_Face *f)
{ (void)l; (void)p; (void)i; *f = NULL; return 1; }
FT_Error FT_New_Memory_Face(FT_Library l, const FT_Byte *p, FT_Long n, FT_Long i, FT_Face *f)
{ (void)l; (void)p; (void)n; (void)i; *f = NULL; return 1; }
FT_Error FT_Done_Face(FT_Face f) { (void)f; return 0; }
FT_Error FT_Request_Size(FT_Face f, FT_Size_Request r) { (void)f; (void)r; return 1; }
FT_UInt FT_Get_Char_Index(FT_Face f, FT_ULong c) { (void)f; (void)c; return 0; }
FT_Error FT_Load_Glyph(FT_Face f, FT_UInt g, FT_Int32 fl) { (void)f; (void)g; (void)fl; return 1; }
FT_Error FT_Render_Glyph(FT_GlyphSlot s, FT_Render_Mode m) { (void)s; (void)m; return 1; }
FT_Error FT_Outline_Embolden(FT_Outline *o, FT_Pos p) { (void)o; (void)p; return 1; }
FT_Long FT_MulFix(FT_Long a, FT_Long b) { return (FT_Long)(((long long)a * b) >> 16); }

// Видео Bink требует libavcodec — в headless-режиме кадры не декодируются,
// но структура BINK и тайминги ролика работают как в оригинале.
#include "runtime/hle/bink.h"
bink_video *bink_video_new(u32 sig, u32 flags, u32 w, u32 h)
{ (void)sig; (void)flags; (void)w; (void)h; return NULL; }
void bink_video_free(bink_video *v) { (void)v; }
void bink_video_flush(bink_video *v) { (void)v; }
int bink_video_decode(bink_video *v, const u8 *p, u32 n, int key)
{ (void)v; (void)p; (void)n; (void)key; return 0; }
void bink_video_prefetch(bink_video *v, const u8 *p, u32 n, int key)
{ (void)v; (void)p; (void)n; (void)key; }
void bink_video_stats(bink_video *v, double *d, double *w, u32 *n)
{ (void)v; *d = *w = 0; *n = 0; }
void bink_video_copy(bink_video *v, u8 *dst, u32 pitch, u32 rows, u32 x, u32 y, u32 flags)
{ (void)v; (void)dst; (void)pitch; (void)rows; (void)x; (void)y; (void)flags; }

// Платформенные вызовы окна/GL и Menю — headless-заглушки.
#include "platform/window.h"
#include "runtime/game/menu.h"
#include "runtime/game/rumble.h"
#include "runtime/hle/overlay.h"
int plat_window_open(int w, int h, int fullscreen, const char *title)
{ (void)w; (void)h; (void)fullscreen; (void)title; return 0; }
int plat_gl_bind(void) { return 0; }
void plat_gl_unbind(void) { }
void plat_drawable_size(int *w, int *h) { *w = 640; *h = 480; }
void plat_swap(void) { }
int plat_vsync_paced(void) { return 0; }
int game_menu_open(int *focus) { if (focus) *focus = 0; return 0; }
void game_menu_element(int at_point, float x, float y) { (void)at_point; (void)x; (void)y; }
int game_menu_element_result(float *x, float *y) { (void)x; (void)y; return 2; }
void game_menu_frame(unsigned w, unsigned h) { (void)w; (void)h; }
void game_menu_draw_overlay(int w, int h, float dx, float dy, float dw, float dh)
{ (void)w; (void)h; (void)dx; (void)dy; (void)dw; (void)dh; }
void game_rumble_frame(void) { }
void ov_begin(void) { }
void ov_flush(u32 fbo, int w, int h) { (void)fbo; (void)w; (void)h; }
