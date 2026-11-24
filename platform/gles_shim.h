// Прослойка OpenGL ES 3.0 (Android) под вызовы GL 4.1 слоя D3D8. Подключается из platform/gl.h, только с SNX_GLES.
// Вызовы, которых в ES нет или которые там принимают меньше форматов, подменяются макросами на snx_gl* (их тела —
// platform/gles_shim.c): форматы D3D8 (BGRA, BGR, 1555/4444 «REV») перекладываются на ЦП в порядок ES, DXT без
// расширения S3TC распаковывается (в 16 бит на пиксель при SNX_TEX16=1), чтение текстур — через FBO и glReadPixels,
// ClearDepth/DepthRange — их варианты f, PolygonMode/LOD_BIAS — пропускаются, CLAMP_TO_BORDER и плоскости отсечения —
// по расширениям. Макросы раскрываются и внутри gl_cs.c (там обёртки cs_gl* зовут gl*), поэтому прослойка работает в
// том потоке, где исполняется GL: в потоке рендера или, без него, в потоке гостя.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- типы и перечисления GL, которых нет в заголовках ES
typedef double GLdouble;  // обёртки gl_cs.h (ClearDepth, DepthRange) — с сигнатурами настольного GL
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_BGR
#define GL_BGR 0x80E0
#endif
#ifndef GL_UNSIGNED_SHORT_4_4_4_4_REV
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8365
#endif
#ifndef GL_UNSIGNED_SHORT_1_5_5_5_REV
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8366
#endif
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812D
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif
#ifndef GL_TEXTURE_LOD_BIAS
#define GL_TEXTURE_LOD_BIAS 0x8501
#endif
#ifndef GL_POINT
#define GL_POINT 0x1B00
#define GL_LINE 0x1B01
#define GL_FILL 0x1B02
#endif
#ifndef GL_DEPTH_COMPONENT32
#define GL_DEPTH_COMPONENT32 0x81A7
#endif
#ifndef GL_CLIP_DISTANCE0
#define GL_CLIP_DISTANCE0 0x3000
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

// ---------------------------------------------------------------- возможности контекста
typedef struct {
    int major, minor;     // версия ES
    int base_vertex;      // glDrawElementsBaseVertex: ES 3.2, EXT/OES_draw_elements_base_vertex
    int clip_distance;    // GL_EXT_clip_cull_distance (иначе плоскости отсечения — discard в шейдере)
    int s3tc;             // DXT1/3/5 сжатыми (GL_EXT_texture_compression_s3tc и родственные)
    int border_clamp;     // CLAMP_TO_BORDER: ES 3.2, EXT/OES_texture_border_clamp
    int program_binary;   // glProgramBinary с хотя бы одним форматом
    int tex16;            // распакованный DXT — в 16 бит на пиксель (SNX_TEX16=1: экономия памяти)
    int invalidate;       // glInvalidateFramebuffer глубины после кадра (тайловые GPU; SNX_GLES_INVALIDATE=0 — нет)
    char renderer[128];
} gles_caps_t;
extern gles_caps_t gles_caps;

// после того как контекст стал текущим (plat_gl_bind): версия, расширения, функции
void gles_init(void);

// ---------------------------------------------------------------- подменённые вызовы
void snx_glPixelStorei(GLenum pname, GLint v);
void snx_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format,
                      GLenum type, const void *data);
void snx_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                         GLenum type, const void *data);
void snx_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border,
                                GLsizei size, const void *data);
void snx_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void *out);
void snx_glGetCompressedTexImage(GLenum target, GLint level, void *out);
void snx_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *out);
void snx_glClearDepth(double d);
void snx_glDepthRange(double n, double f);
void snx_glPolygonMode(GLenum face, GLenum mode);
void snx_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint base);
void snx_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void *p);
void snx_glSamplerParameteri(GLuint s, GLenum pname, GLint v);
void snx_glSamplerParameterf(GLuint s, GLenum pname, GLfloat v);
void snx_glSamplerParameterfv(GLuint s, GLenum pname, const GLfloat *v);
void snx_glEnable(GLenum cap);
void snx_glDisable(GLenum cap);
void snx_glRenderbufferStorage(GLenum target, GLenum internal, GLsizei w, GLsizei h);

#ifndef SNX_GLES_SHIM_IMPL
#define glPixelStorei snx_glPixelStorei
#define glTexImage2D snx_glTexImage2D
#define glTexSubImage2D snx_glTexSubImage2D
#define glCompressedTexImage2D snx_glCompressedTexImage2D
#define glGetTexImage snx_glGetTexImage
#define glGetCompressedTexImage snx_glGetCompressedTexImage
#define glReadPixels snx_glReadPixels
#define glClearDepth snx_glClearDepth
#define glDepthRange snx_glDepthRange
#define glPolygonMode snx_glPolygonMode
#define glDrawElementsBaseVertex snx_glDrawElementsBaseVertex
#define glVertexAttribPointer snx_glVertexAttribPointer
#define glSamplerParameteri snx_glSamplerParameteri
#define glSamplerParameterf snx_glSamplerParameterf
#define glSamplerParameterfv snx_glSamplerParameterfv
#define glEnable snx_glEnable
#define glDisable snx_glDisable
#define glRenderbufferStorage snx_glRenderbufferStorage
#endif

#ifdef __cplusplus
}
#endif
