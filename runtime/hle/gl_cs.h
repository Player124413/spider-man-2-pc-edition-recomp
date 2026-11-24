// Поток рендера (command stream): вызовы GL слоя D3D8 записываются в кольцевой буфер и исполняются отдельным
// потоком на своём ядре — поток гостя не ждёт драйвер GL. Включён по умолчанию (d3d8_device_create;
// SNX_RENDER_THREAD=0 — выключить, тогда обёртки зовут GL напрямую).
// Подключается через d3d8_int.h: функции gl* слоя D3D8 подменяются макросами на cs_gl*. Данные по указателям
// (буферы, униформы, текстуры) копируются в очередь; функции с результатом (glGet*, glCreate*, чтение
// пикселей) ждут поток рендера; имена glGen* берутся из запаса, заготовленного потоком рендера.
// Код, который должен исполниться в потоке рендера целиком (создание программ, проверки), — cs_sync/cs_async:
// внутри них вызовы GL идут напрямую.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "platform/gl.h"

#ifdef __cplusplus
extern "C" {
#endif

int cs_enabled(void);                                         // поток рендера запущен
int cs_on_render_thread(void);                                // вызов из потока рендера (для профайлера)
extern void (*cs_render_thread_hook)(void);                   // зовётся в начале потока рендера (профайлер)
int cs_start(int core);                                       // запустить поток рендера на ядре core; 0 — успех
void cs_stop(void);                                           // дренировать очередь и корректно остановить поток рендера
void cs_sync(void (*fn)(void *arg), void *arg);               // fn(arg) в потоке рендера, дождаться
void cs_async(void (*fn)(void *arg), const void *arg, size_t size);  // fn(копия arg) в потоке рендера
typedef void (*cs_cmd_fn)(void *payload);
// Компактная команда верхнего уровня (D3D8): заголовок 16 байт, payload выровнен на 16 байт.
// В однопоточном режиме вызывает fn(payload) сразу без копирования.
void cs_cmd_push(cs_cmd_fn fn, const void *payload, size_t payload_size);
// Команда с переменным хвостом данных (копируется в кольцо или в кучу при > BIG_DATA);
// в payload по смещениям ptr_field_off и heap_field_off записываются указатели на данные и блок malloc.
// В однопоточном режиме fn получает исходный hdr (вызывающий заранее ставит ptr = data, heap = NULL).
void cs_cmd_push_var(cs_cmd_fn fn, const void *hdr, size_t hdr_size, const void *data, size_t data_size,
                     size_t ptr_field_off, size_t heap_field_off);
// Позиция записи (Core 1) и исполненный хвост (Core 2 -> Core 1 acquire) для трекинга слотов динамических буферов.
uint64_t cs_wr_pos(void);
uint64_t cs_tail_pos(void);
void cs_finish(void);                                         // дождаться пустой очереди
void cs_swap(void);                                           // показать кадр (plat_swap) и ограничить отставание
// Накопленное с прошлого вызова: записей и байт в очереди, ожиданий cs_sync, время ожидания потоком гостя,
// время работы потока рендера (с показом кадров) и отдельно показа (plat_swap: vsync и ожидание GPU)
typedef struct {
    unsigned long long recs, bytes, syncs, wait_ns, busy_ns, swap_ns;
} cs_stats;
void cs_stats_take(cs_stats *out);
unsigned long long cs_wait_total_ns(void);  // поток гостя ждал поток рендера, с начала работы
// Запись в часть буфера без ожидания GPU (glMapBufferRange UNSYNCHRONIZED в потоке рендера, данные — копией
// в очереди): для D3DLOCK_NOOVERWRITE, когда приложение обещает не трогать то, что рисуется
void cs_BufferWriteUnsync(GLenum target, GLintptr off, GLsizeiptr size, const void *data);
// glTexImage2D с известным размером данных (копия в очередь вместо ожидания)
void cs_TexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLenum format, GLenum type,
                   const void *data, size_t size);
// glTexSubImage2D из строк по row байт с шагом pitch: в очередь копируются только сами строки, плотно
// (GL_UNPACK_ROW_LENGTH у вызывающего — 0)
void cs_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                      GLenum type, const void *data, size_t row, size_t pitch);

// ---------------------------------------------------------------- обёртки
#define CS_GL_ASYNC(F)                                                                                     \
    F(1, ActiveTexture, GLenum)                                                                            \
    F(2, AttachShader, GLuint, GLuint)                                                                     \
    F(2, BindBuffer, GLenum, GLuint)                                                                       \
    F(2, BindFramebuffer, GLenum, GLuint)                                                                  \
    F(2, BindRenderbuffer, GLenum, GLuint)                                                                 \
    F(2, BindSampler, GLuint, GLuint)                                                                      \
    F(2, BindTexture, GLenum, GLuint)                                                                      \
    F(1, BindVertexArray, GLuint)                                                                          \
    F(1, BlendEquation, GLenum)                                                                            \
    F(2, BlendFunc, GLenum, GLenum)                                                                        \
    F(10, BlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)     \
    F(1, Clear, GLbitfield)                                                                                \
    F(4, ClearColor, GLfloat, GLfloat, GLfloat, GLfloat)                                                   \
    F(1, ClearDepth, GLdouble)                                                                             \
    F(1, ClearStencil, GLint)                                                                              \
    F(4, ColorMask, GLboolean, GLboolean, GLboolean, GLboolean)                                            \
    F(1, CompileShader, GLuint)                                                                            \
    F(1, CullFace, GLenum)                                                                                 \
    F(1, DeleteProgram, GLuint)                                                                            \
    F(1, DeleteShader, GLuint)                                                                             \
    F(1, DepthFunc, GLenum)                                                                                \
    F(1, DepthMask, GLboolean)                                                                             \
    F(2, DepthRange, GLdouble, GLdouble)                                                                   \
    F(1, Disable, GLenum)                                                                                  \
    F(1, DisableVertexAttribArray, GLuint)                                                                 \
    F(3, DrawArrays, GLenum, GLint, GLsizei)                                                               \
    F(4, DrawElements, GLenum, GLsizei, GLenum, const void *)                                              \
    F(5, DrawElementsBaseVertex, GLenum, GLsizei, GLenum, const void *, GLint)                             \
    F(1, Enable, GLenum)                                                                                   \
    F(1, EnableVertexAttribArray, GLuint)                                                                  \
    F(4, FramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint)                                          \
    F(5, FramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)                                      \
    F(1, FrontFace, GLenum)                                                                                \
    F(1, LinkProgram, GLuint)                                                                              \
    F(2, PixelStorei, GLenum, GLint)                                                                       \
    F(2, PolygonMode, GLenum, GLenum)                                                                      \
    F(2, PolygonOffset, GLfloat, GLfloat)                                                                  \
    F(4, RenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei)                                            \
    F(3, SamplerParameterf, GLuint, GLenum, GLfloat)                                                       \
    F(3, SamplerParameteri, GLuint, GLenum, GLint)                                                         \
    F(4, Scissor, GLint, GLint, GLsizei, GLsizei)                                                          \
    F(3, StencilFunc, GLenum, GLint, GLuint)                                                               \
    F(1, StencilMask, GLuint)                                                                              \
    F(3, StencilOp, GLenum, GLenum, GLenum)                                                                \
    F(3, TexParameteri, GLenum, GLenum, GLint)                                                             \
    F(2, Uniform1f, GLint, GLfloat)                                                                        \
    F(2, Uniform1i, GLint, GLint)                                                                          \
    F(3, Uniform2f, GLint, GLfloat, GLfloat)                                                               \
    F(5, Uniform4f, GLint, GLfloat, GLfloat, GLfloat, GLfloat)                                             \
    F(1, UseProgram, GLuint)                                                                               \
    F(6, VertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)                     \
    F(4, Viewport, GLint, GLint, GLsizei, GLsizei)

// с результатом: ждут поток рендера
#define CS_GL_SYNC_RET(F)                                                                                  \
    F(1, GLenum, CheckFramebufferStatus, GLenum)                                                           \
    F(0, GLuint, CreateProgram, void)                                                                      \
    F(1, GLuint, CreateShader, GLenum)                                                                     \
    F(0, GLenum, GetError, void)                                                                           \
    F(1, const GLubyte *, GetString, GLenum)                                                               \
    F(2, GLint, GetUniformLocation, GLuint, const GLchar *)                                                \
    F(4, void *, MapBufferRange, GLenum, GLintptr, GLsizeiptr, GLbitfield)                                 \
    F(1, GLboolean, UnmapBuffer, GLenum)

// без результата, но с выходными указателями или строками: ждут поток рендера
#define CS_GL_SYNC_VOID(F)                                                                                 \
    F(2, GenVertexArrays, GLsizei, GLuint *)                                                               \
    F(2, GenSamplers, GLsizei, GLuint *)                                                                   \
    F(3, GetShaderiv, GLuint, GLenum, GLint *)                                                             \
    F(4, GetShaderInfoLog, GLuint, GLsizei, GLsizei *, GLchar *)                                           \
    F(3, GetProgramiv, GLuint, GLenum, GLint *)                                                            \
    F(4, GetProgramInfoLog, GLuint, GLsizei, GLsizei *, GLchar *)                                          \
    F(4, ShaderSource, GLuint, GLsizei, const GLchar *const *, const GLint *)                              \
    F(7, ReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)                               \
    F(5, GetTexImage, GLenum, GLint, GLenum, GLenum, void *)                                               \
    F(3, GetCompressedTexImage, GLenum, GLint, void *)

// имена в списках — без префикса gl: на Switch gl* — макросы glad, и имя, переданное через макрос, раскрылось бы
#define CS_PROTO_ASYNC(n, name, ...) void cs_gl##name(__VA_ARGS__);
#define CS_PROTO_RET(n, ret, name, ...) ret cs_gl##name(__VA_ARGS__);
#define CS_PROTO_VOID(n, name, ...) void cs_gl##name(__VA_ARGS__);
CS_GL_ASYNC(CS_PROTO_ASYNC)
CS_GL_SYNC_RET(CS_PROTO_RET)
CS_GL_SYNC_VOID(CS_PROTO_VOID)

// данные по указателю копируются; имена glGen* — из запаса
void cs_glUniform4fv(GLint loc, GLsizei count, const GLfloat *v);
void cs_glUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v);
void cs_glSamplerParameterfv(GLuint s, GLenum pname, const GLfloat *v);  // только 4-компонентные (цвет края)
void cs_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void cs_glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data);
void cs_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border,
                               GLsizei size, const void *data);
void cs_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border,
                     GLenum format, GLenum type, const void *data);  // с данными — ждёт поток рендера
void cs_glGenTextures(GLsizei n, GLuint *out);
void cs_glGenBuffers(GLsizei n, GLuint *out);
void cs_glGenFramebuffers(GLsizei n, GLuint *out);
void cs_glGenRenderbuffers(GLsizei n, GLuint *out);
void cs_glDeleteTextures(GLsizei n, const GLuint *names);
void cs_glDeleteBuffers(GLsizei n, const GLuint *names);
void cs_glDeleteFramebuffers(GLsizei n, const GLuint *names);
void cs_glDeleteRenderbuffers(GLsizei n, const GLuint *names);

#if !defined(GL_CS_IMPL) && !defined(GL_CS_NO_WRAP)
// clang-format off
#undef glActiveTexture
#define glActiveTexture cs_glActiveTexture
#undef glAttachShader
#define glAttachShader cs_glAttachShader
#undef glBindBuffer
#define glBindBuffer cs_glBindBuffer
#undef glBindFramebuffer
#define glBindFramebuffer cs_glBindFramebuffer
#undef glBindRenderbuffer
#define glBindRenderbuffer cs_glBindRenderbuffer
#undef glBindSampler
#define glBindSampler cs_glBindSampler
#undef glBindTexture
#define glBindTexture cs_glBindTexture
#undef glBindVertexArray
#define glBindVertexArray cs_glBindVertexArray
#undef glBlendEquation
#define glBlendEquation cs_glBlendEquation
#undef glBlendFunc
#define glBlendFunc cs_glBlendFunc
#undef glBlitFramebuffer
#define glBlitFramebuffer cs_glBlitFramebuffer
#undef glClear
#define glClear cs_glClear
#undef glClearColor
#define glClearColor cs_glClearColor
#undef glClearDepth
#define glClearDepth cs_glClearDepth
#undef glClearStencil
#define glClearStencil cs_glClearStencil
#undef glColorMask
#define glColorMask cs_glColorMask
#undef glCompileShader
#define glCompileShader cs_glCompileShader
#undef glCullFace
#define glCullFace cs_glCullFace
#undef glDeleteProgram
#define glDeleteProgram cs_glDeleteProgram
#undef glDeleteShader
#define glDeleteShader cs_glDeleteShader
#undef glDepthFunc
#define glDepthFunc cs_glDepthFunc
#undef glDepthMask
#define glDepthMask cs_glDepthMask
#undef glDepthRange
#define glDepthRange cs_glDepthRange
#undef glDisable
#define glDisable cs_glDisable
#undef glDisableVertexAttribArray
#define glDisableVertexAttribArray cs_glDisableVertexAttribArray
#undef glDrawArrays
#define glDrawArrays cs_glDrawArrays
#undef glDrawElements
#define glDrawElements cs_glDrawElements
#undef glDrawElementsBaseVertex
#define glDrawElementsBaseVertex cs_glDrawElementsBaseVertex
#undef glEnable
#define glEnable cs_glEnable
#undef glEnableVertexAttribArray
#define glEnableVertexAttribArray cs_glEnableVertexAttribArray
#undef glFramebufferRenderbuffer
#define glFramebufferRenderbuffer cs_glFramebufferRenderbuffer
#undef glFramebufferTexture2D
#define glFramebufferTexture2D cs_glFramebufferTexture2D
#undef glFrontFace
#define glFrontFace cs_glFrontFace
#undef glLinkProgram
#define glLinkProgram cs_glLinkProgram
#undef glPixelStorei
#define glPixelStorei cs_glPixelStorei
#undef glPolygonMode
#define glPolygonMode cs_glPolygonMode
#undef glPolygonOffset
#define glPolygonOffset cs_glPolygonOffset
#undef glRenderbufferStorage
#define glRenderbufferStorage cs_glRenderbufferStorage
#undef glSamplerParameterf
#define glSamplerParameterf cs_glSamplerParameterf
#undef glSamplerParameteri
#define glSamplerParameteri cs_glSamplerParameteri
#undef glScissor
#define glScissor cs_glScissor
#undef glStencilFunc
#define glStencilFunc cs_glStencilFunc
#undef glStencilMask
#define glStencilMask cs_glStencilMask
#undef glStencilOp
#define glStencilOp cs_glStencilOp
#undef glTexParameteri
#define glTexParameteri cs_glTexParameteri
#undef glUniform1f
#define glUniform1f cs_glUniform1f
#undef glUniform1i
#define glUniform1i cs_glUniform1i
#undef glUniform2f
#define glUniform2f cs_glUniform2f
#undef glUniform4f
#define glUniform4f cs_glUniform4f
#undef glUseProgram
#define glUseProgram cs_glUseProgram
#undef glVertexAttribPointer
#define glVertexAttribPointer cs_glVertexAttribPointer
#undef glViewport
#define glViewport cs_glViewport
#undef glCheckFramebufferStatus
#define glCheckFramebufferStatus cs_glCheckFramebufferStatus
#undef glCreateProgram
#define glCreateProgram cs_glCreateProgram
#undef glCreateShader
#define glCreateShader cs_glCreateShader
#undef glGetError
#define glGetError cs_glGetError
#undef glGetString
#define glGetString cs_glGetString
#undef glGetUniformLocation
#define glGetUniformLocation cs_glGetUniformLocation
#undef glMapBufferRange
#define glMapBufferRange cs_glMapBufferRange
#undef glUnmapBuffer
#define glUnmapBuffer cs_glUnmapBuffer
#undef glGenVertexArrays
#define glGenVertexArrays cs_glGenVertexArrays
#undef glGenSamplers
#define glGenSamplers cs_glGenSamplers
#undef glGetShaderiv
#define glGetShaderiv cs_glGetShaderiv
#undef glGetShaderInfoLog
#define glGetShaderInfoLog cs_glGetShaderInfoLog
#undef glGetProgramiv
#define glGetProgramiv cs_glGetProgramiv
#undef glGetProgramInfoLog
#define glGetProgramInfoLog cs_glGetProgramInfoLog
#undef glShaderSource
#define glShaderSource cs_glShaderSource
#undef glReadPixels
#define glReadPixels cs_glReadPixels
#undef glGetTexImage
#define glGetTexImage cs_glGetTexImage
#undef glGetCompressedTexImage
#define glGetCompressedTexImage cs_glGetCompressedTexImage
#undef glUniform4fv
#define glUniform4fv cs_glUniform4fv
#undef glUniformMatrix4fv
#define glUniformMatrix4fv cs_glUniformMatrix4fv
#undef glSamplerParameterfv
#define glSamplerParameterfv cs_glSamplerParameterfv
#undef glBufferData
#define glBufferData cs_glBufferData
#undef glBufferSubData
#define glBufferSubData cs_glBufferSubData
#undef glCompressedTexImage2D
#define glCompressedTexImage2D cs_glCompressedTexImage2D
#undef glTexImage2D
#define glTexImage2D cs_glTexImage2D
#undef glGenTextures
#define glGenTextures cs_glGenTextures
#undef glGenBuffers
#define glGenBuffers cs_glGenBuffers
#undef glGenFramebuffers
#define glGenFramebuffers cs_glGenFramebuffers
#undef glGenRenderbuffers
#define glGenRenderbuffers cs_glGenRenderbuffers
#undef glDeleteTextures
#define glDeleteTextures cs_glDeleteTextures
#undef glDeleteBuffers
#define glDeleteBuffers cs_glDeleteBuffers
#undef glDeleteFramebuffers
#define glDeleteFramebuffers cs_glDeleteFramebuffers
#undef glDeleteRenderbuffers
#define glDeleteRenderbuffers cs_glDeleteRenderbuffers
// clang-format on
#endif

#ifdef __cplusplus
}
#endif
