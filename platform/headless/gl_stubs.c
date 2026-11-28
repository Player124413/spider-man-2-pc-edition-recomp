// Автоматические no-op заглушки OpenGL для headless-сборки.
#include "GL/gl.h"

int glActiveTexture() { return 0; }
int glAttachShader() { return 0; }
int glBindBuffer() { return 0; }
int glBindFramebuffer() { return 0; }
int glBindRenderbuffer() { return 0; }
int glBindSampler() { return 0; }
int glBindTexture() { return 0; }
int glBindVertexArray() { return 0; }
int glBlendEquation() { return 0; }
int glBlendFunc() { return 0; }
int glBlitFramebuffer() { return 0; }
int glBufferData() { return 0; }
int glBufferSubData() { return 0; }
int glClear() { return 0; }
int glClearColor() { return 0; }
int glClearDepth() { return 0; }
int glClearStencil() { return 0; }
int glColorMask() { return 0; }
int glCompileShader() { return 0; }
int glCompressedTexImage2D() { return 0; }
int glCullFace() { return 0; }
int glDeleteBuffers() { return 0; }
int glDeleteFramebuffers() { return 0; }
int glDeleteProgram() { return 0; }
int glDeleteRenderbuffers() { return 0; }
int glDeleteShader() { return 0; }
int glDeleteTextures() { return 0; }
int glDepthFunc() { return 0; }
int glDepthMask() { return 0; }
int glDepthRange() { return 0; }
int glDisable() { return 0; }
int glDisableVertexAttribArray() { return 0; }
int glDrawArrays() { return 0; }
int glDrawElements() { return 0; }
int glDrawElementsBaseVertex() { return 0; }
int glEnable() { return 0; }
int glEnableVertexAttribArray() { return 0; }
int glFramebufferRenderbuffer() { return 0; }
int glFramebufferTexture2D() { return 0; }
int glFrontFace() { return 0; }
int glGetCompressedTexImage() { return 0; }
int glGetTexImage() { return 0; }
int glLinkProgram() { return 0; }
int glPixelStorei() { return 0; }
int glPolygonMode() { return 0; }
int glPolygonOffset() { return 0; }
int glProgramBinary() { return 0; }
int glProgramParameteri() { return 0; }
int glReadPixels() { return 0; }
int glRenderbufferStorage() { return 0; }
int glSamplerParameterf() { return 0; }
int glSamplerParameterfv() { return 0; }
int glSamplerParameteri() { return 0; }
int glScissor() { return 0; }
int glShaderSource() { return 0; }
int glStencilFunc() { return 0; }
int glStencilMask() { return 0; }
int glStencilOp() { return 0; }
int glTexImage2D() { return 0; }
int glTexParameteri() { return 0; }
int glTexSubImage2D() { return 0; }
int glUniform1f() { return 0; }
int glUniform1i() { return 0; }
int glUniform2f() { return 0; }
int glUniform4f() { return 0; }
int glUniform4fv() { return 0; }
int glUniformMatrix3fv() { return 0; }
int glUniformMatrix4fv() { return 0; }
int glUseProgram() { return 0; }
int glVertexAttribPointer() { return 0; }
int glViewport() { return 0; }

// Заглушки, которые должны возвращать осмысленные значения: D3D8 HLE проверяет статусы
// компиляции/линковки и идентификаторы объектов, поэтому «все нули» ломают инициализацию.
static GLuint g_next_obj = 1;

GLuint glCreateShader(GLenum type) { (void)type; return g_next_obj++; }
GLuint glCreateProgram(void) { return g_next_obj++; }

void glGenBuffers(GLsizei n, GLuint *ids) { for (GLsizei i = 0; i < n && ids; i++) ids[i] = g_next_obj++; }
void glGenFramebuffers(GLsizei n, GLuint *ids) { for (GLsizei i = 0; i < n && ids; i++) ids[i] = g_next_obj++; }
void glGenRenderbuffers(GLsizei n, GLuint *ids) { for (GLsizei i = 0; i < n && ids; i++) ids[i] = g_next_obj++; }
void glGenSamplers(GLsizei n, GLuint *ids) { for (GLsizei i = 0; i < n && ids; i++) ids[i] = g_next_obj++; }
void glGenTextures(GLsizei n, GLuint *ids) { for (GLsizei i = 0; i < n && ids; i++) ids[i] = g_next_obj++; }
void glGenVertexArrays(GLsizei n, GLuint *ids) { for (GLsizei i = 0; i < n && ids; i++) ids[i] = g_next_obj++; }

void glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
    (void)shader;
    if (params) *params = (pname == GL_COMPILE_STATUS) ? GL_TRUE : 0;
}
void glGetProgramiv(GLuint prog, GLenum pname, GLint *params)
{
    (void)prog;
    if (!params) return;
    *params = (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS) ? GL_TRUE : 0;
}
void glGetShaderInfoLog(GLuint s, GLsizei n, GLsizei *len, GLchar *log)
{
    (void)s;
    if (len) *len = 0;
    if (log && n > 0) log[0] = 0;
}
void glGetProgramInfoLog(GLuint p, GLsizei n, GLsizei *len, GLchar *log)
{
    (void)p;
    if (len) *len = 0;
    if (log && n > 0) log[0] = 0;
}
GLint glGetUniformLocation(GLuint prog, const GLchar *name) { (void)prog; (void)name; return 1; }
GLenum glCheckFramebufferStatus(GLenum target) { (void)target; return GL_FRAMEBUFFER_COMPLETE; }
GLenum glGetError(void) { return 0; }
const GLubyte *glGetString(GLenum name) { (void)name; return (const GLubyte *)"headless"; }
void glGetIntegerv(GLenum pname, GLint *params)
{
    (void)pname;
    if (params) *params = 4096;  // максимальный размер текстур/рендербуфера
}
void glGetProgramBinary(GLuint prog, GLsizei n, GLsizei *len, GLenum *fmt, void *bin)
{
    (void)prog; (void)n; (void)fmt; (void)bin;
    if (len) *len = 0;
}
void *glMapBufferRange(GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access)
{
    (void)target; (void)off; (void)access;
    static unsigned char buf[4u << 20];
    return len <= (GLsizeiptr)sizeof(buf) ? buf : NULL;
}
GLboolean glUnmapBuffer(GLenum target) { (void)target; return GL_TRUE; }
