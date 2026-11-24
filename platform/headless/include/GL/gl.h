#pragma once
#include <stddef.h>
typedef unsigned int GLenum, GLbitfield, GLuint;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef double GLdouble;
typedef unsigned char GLboolean, GLubyte;
typedef signed long int GLintptr;
typedef signed long int GLsizeiptr;
typedef char GLchar;
typedef void GLvoid;
typedef struct _cl_context *GLeglClientBufferEXT;

#define GL_ALWAYS 0x8000u
#define GL_ARRAY_BUFFER 0x8001u
#define GL_BACK 0x8002u
#define GL_BGR 0x8003u
#define GL_BGRA 0x8004u
#define GL_BLEND 0x8005u
#define GL_BYTE 0x8006u
#define GL_CCW 0x8007u
#define GL_CLAMP_TO_BORDER 0x8008u
#define GL_CLAMP_TO_EDGE 0x8009u
#define GL_CLIP_DISTANCE0 0x800au
#define GL_COLOR_ATTACHMENT0 0x800bu
#define GL_COLOR_BUFFER_BIT 0x800cu
#define GL_COMPILE_STATUS 0x800du
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x800eu
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x800fu
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x8010u
#define GL_CULL_FACE 0x8013u
#define GL_DECR 0x8014u
#define GL_DECR_WRAP 0x8015u
#define GL_DEPTH24_STENCIL8 0x8016u
#define GL_DEPTH_ATTACHMENT 0x8017u
#define GL_DEPTH_BUFFER_BIT 0x8018u
#define GL_DEPTH_COMPONENT 0x8019u
#define GL_DEPTH_COMPONENT16 0x801au
#define GL_DEPTH_COMPONENT24 0x801bu
#define GL_DEPTH_COMPONENT32 0x801cu
#define GL_DEPTH_STENCIL 0x801du
#define GL_DEPTH_STENCIL_ATTACHMENT 0x801eu
#define GL_DEPTH_TEST 0x801fu
#define GL_DRAW_FRAMEBUFFER 0x8020u
#define GL_DST_ALPHA 0x8021u
#define GL_DST_COLOR 0x8022u
#define GL_DYNAMIC_DRAW 0x8023u
#define GL_ELEMENT_ARRAY_BUFFER 0x8024u
#define GL_EQUAL 0x8025u
#define GL_FALSE 0u
#define GL_FILL 0x8027u
#define GL_FLOAT 0x8028u
#define GL_FRAGMENT_SHADER 0x8029u
#define GL_FRAMEBUFFER 0x802au
#define GL_FRAMEBUFFER_COMPLETE 0x802bu
#define GL_FRONT 0x802cu
#define GL_FRONT_AND_BACK 0x802du
#define GL_FUNC_ADD 0x802eu
#define GL_FUNC_REVERSE_SUBTRACT 0x802fu
#define GL_FUNC_SUBTRACT 0x8030u
#define GL_GEQUAL 0x8031u
#define GL_GREATER 0x8032u
#define GL_INCR 0x8033u
#define GL_INCR_WRAP 0x8034u
#define GL_INVERT 0x8035u
#define GL_KEEP 0x8036u
#define GL_LEQUAL 0x8037u
#define GL_LESS 0x8038u
#define GL_LINE 0x8039u
#define GL_LINEAR 0x803au
#define GL_LINEAR_MIPMAP_LINEAR 0x803bu
#define GL_LINEAR_MIPMAP_NEAREST 0x803cu
#define GL_LINES 0x803du
#define GL_LINE_STRIP 0x803eu
#define GL_LINK_STATUS 0x803fu
#define GL_MAP_INVALIDATE_RANGE_BIT 0x8040u
#define GL_MAP_UNSYNCHRONIZED_BIT 0x8041u
#define GL_MAP_WRITE_BIT 0x8042u
#define GL_MAX 0x8043u
#define GL_MIN 0x8044u
#define GL_MIRRORED_REPEAT 0x8045u
#define GL_NEAREST 0x8046u
#define GL_NEAREST_MIPMAP_LINEAR 0x8047u
#define GL_NEAREST_MIPMAP_NEAREST 0x8048u
#define GL_NEVER 0x8049u
#define GL_NOTEQUAL 0x804au
#define GL_NO_ERROR 0x804bu
#define GL_NUM_PROGRAM_BINARY_FORMATS 0x804cu
#define GL_ONE 0x804du
#define GL_ONE_MINUS_DST_ALPHA 0x804eu
#define GL_ONE_MINUS_DST_COLOR 0x804fu
#define GL_ONE_MINUS_SRC_ALPHA 0x8050u
#define GL_ONE_MINUS_SRC_COLOR 0x8051u
#define GL_PACK_ALIGNMENT 0x8052u
#define GL_PACK_ROW_LENGTH 0x8053u
#define GL_POINT 0x8054u
#define GL_POINTS 0x8055u
#define GL_POLYGON_OFFSET_FILL 0x8056u
#define GL_PROGRAM_BINARY_LENGTH 0x8057u
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8058u
#define GL_R8 0x8059u
#define GL_READ_FRAMEBUFFER 0x805au
#define GL_RED 0x805bu
#define GL_RENDERBUFFER 0x805cu
#define GL_RENDERER 0x805du
#define GL_REPEAT 0x805eu
#define GL_REPLACE 0x805fu
#define GL_RG 0x8060u
#define GL_RG8 0x8061u
#define GL_RG8_SNORM 0x8062u
#define GL_RGB 0x8063u
#define GL_RGB16 0x8064u
#define GL_RGB565 0x8065u
#define GL_RGB5_A1 0x8066u
#define GL_RGB8 0x8067u
#define GL_RGBA 0x8068u
#define GL_RGBA4 0x8069u
#define GL_RGBA8 0x806au
#define GL_SCISSOR_TEST 0x806bu
#define GL_SHADING_LANGUAGE_VERSION 0x806cu
#define GL_SHORT 0x806du
#define GL_SRC_ALPHA 0x806eu
#define GL_SRC_ALPHA_SATURATE 0x806fu
#define GL_SRC_COLOR 0x8070u
#define GL_STATIC_DRAW 0x8071u
#define GL_STENCIL_BUFFER_BIT 0x8072u
#define GL_STENCIL_TEST 0x8073u
#define GL_STREAM_DRAW 0x8074u
#define GL_TEXTURE0 0x8075u
#define GL_TEXTURE1 0x8076u
#define GL_TEXTURE_2D 0x8077u
#define GL_TEXTURE_BASE_LEVEL 0x8078u
#define GL_TEXTURE_BORDER_COLOR 0x8079u
#define GL_TEXTURE_CUBE_MAP 0x807au
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x807bu
#define GL_TEXTURE_LOD_BIAS 0x807cu
#define GL_TEXTURE_MAG_FILTER 0x807du
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x807eu
#define GL_TEXTURE_MAX_LEVEL 0x807fu
#define GL_TEXTURE_MIN_FILTER 0x8080u
#define GL_TEXTURE_MIN_LOD 0x8081u
#define GL_TEXTURE_SWIZZLE_A 0x8082u
#define GL_TEXTURE_SWIZZLE_B 0x8083u
#define GL_TEXTURE_SWIZZLE_G 0x8084u
#define GL_TEXTURE_SWIZZLE_R 0x8085u
#define GL_TEXTURE_WRAP_R 0x8086u
#define GL_TEXTURE_WRAP_S 0x8087u
#define GL_TEXTURE_WRAP_T 0x8088u
#define GL_TRIANGLES 0x8089u
#define GL_TRIANGLE_FAN 0x808au
#define GL_TRIANGLE_STRIP 0x808bu
#define GL_TRUE 1u
#define GL_VALIDATE_STATUS 0x808du
#define GL_UNPACK_ALIGNMENT 0x808du
#define GL_UNPACK_ROW_LENGTH 0x808eu
#define GL_UNSIGNED_BYTE 0x808fu
#define GL_UNSIGNED_INT 0x8090u
#define GL_UNSIGNED_INT_24_8 0x8091u
#define GL_UNSIGNED_SHORT 0x8092u
#define GL_UNSIGNED_SHORT_1_5_5_5_REV 0x8093u
#define GL_UNSIGNED_SHORT_4_4_4_4_REV 0x8094u
#define GL_UNSIGNED_SHORT_5_6_5 0x8095u
#define GL_VERSION 0x8096u
#define GL_VERTEX_SHADER 0x8097u
#define GL_ZERO 0x8098u

#define GL_ACTIVE_TEXTURE 0x9099u
#define GL_ASCII_COUNT 0x909au
#define GL_ASCII_FIRST 0x909bu
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x909cu
#define GL_CURRENT_PROGRAM 0x909du
#define GL_CYR_COUNT 0x909eu
#define GL_CYR_FIRST 0x909fu
#define GL_EXTENSIONS 0x90a0u
#undef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#define GL_MAJOR_VERSION 0x90a2u
#define GL_MINOR_VERSION 0x90a3u
#define GL_READ_FRAMEBUFFER_BINDING 0x90a4u
#define GL_SAMPLER_BINDING 0x90a5u
#define GL_TEXTURE_BINDING_2D 0x90a6u
#define GL_TEXTURE_BINDING_CUBE_MAP 0x90a7u
#define GL_TOTAL 0x90a8u
#define GL_UNSIGNED_SHORT_4_4_4_4 0x90a9u
#define GL_UNSIGNED_SHORT_5_5_5_1 0x90aau
#define GL_VERTEX_ARRAY_BINDING 0x90abu

#define GL_GREEN 0xa001u
#define GL_BLUE 0xa002u
#define GL_ALPHA 0xa003u
#define GL_STENCIL_INDEX8 0xa018u
#define GL_VENDOR 0xa025u
#define GL_INVALID_ENUM 0xa029u

extern int glActiveTexture();
extern int glAttachShader();
extern int glBindBuffer();
extern int glBindFramebuffer();
extern int glBindRenderbuffer();
extern int glBindSampler();
extern int glBindTexture();
extern int glBindVertexArray();
extern int glBlendEquation();
extern int glBlendFunc();
extern int glBlitFramebuffer();
extern int glBufferData();
extern int glBufferSubData();
extern GLenum glCheckFramebufferStatus(GLenum target);
extern int glClear();
extern int glClearColor();
extern int glClearDepth();
extern int glClearStencil();
extern int glColorMask();
extern int glCompileShader();
extern int glCompressedTexImage2D();
extern GLuint glCreateProgram(void);
extern GLuint glCreateShader(GLenum type);
extern int glCullFace();
extern int glDeleteBuffers();
extern int glDeleteFramebuffers();
extern int glDeleteProgram();
extern int glDeleteRenderbuffers();
extern int glDeleteShader();
extern int glDeleteTextures();
extern int glDepthFunc();
extern int glDepthMask();
extern int glDepthRange();
extern int glDisable();
extern int glDisableVertexAttribArray();
extern int glDrawArrays();
extern int glDrawElements();
extern int glDrawElementsBaseVertex();
extern int glEnable();
extern int glEnableVertexAttribArray();
extern int glFramebufferRenderbuffer();
extern int glFramebufferTexture2D();
extern int glFrontFace();
extern void glGenBuffers(GLsizei n, GLuint *ids);
extern void glGenFramebuffers(GLsizei n, GLuint *ids);
extern void glGenRenderbuffers(GLsizei n, GLuint *ids);
extern void glGenSamplers(GLsizei n, GLuint *ids);
extern void glGenTextures(GLsizei n, GLuint *ids);
extern void glGenVertexArrays(GLsizei n, GLuint *ids);
extern int glGetCompressedTexImage();
extern GLenum glGetError(void);
extern void glGetIntegerv(GLenum pname, GLint *params);
extern void glGetProgramBinary(GLuint prog, GLsizei n, GLsizei *len, GLenum *fmt, void *bin);
extern void glGetProgramInfoLog(GLuint prog, GLsizei n, GLsizei *len, GLchar *log);
extern void glGetProgramiv(GLuint prog, GLenum pname, GLint *params);
extern void glGetShaderInfoLog(GLuint shader, GLsizei n, GLsizei *len, GLchar *log);
extern void glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
extern const GLubyte *glGetString(GLenum name);
extern int glGetTexImage();
extern GLint glGetUniformLocation(GLuint prog, const GLchar *name);
extern int glLinkProgram();
extern void *glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
extern int glPixelStorei();
extern int glPolygonMode();
extern int glPolygonOffset();
extern int glProgramBinary();
extern int glProgramParameteri();
extern int glReadPixels();
extern int glRenderbufferStorage();
extern int glSamplerParameterf();
extern int glSamplerParameterfv();
extern int glSamplerParameteri();
extern int glScissor();
extern int glShaderSource();
extern int glStencilFunc();
extern int glStencilMask();
extern int glStencilOp();
extern int glTexImage2D();
extern int glTexParameteri();
extern int glTexSubImage2D();
extern int glUniform1f();
extern int glUniform1i();
extern int glUniform2f();
extern int glUniform4f();
extern int glUniform4fv();
extern int glUniformMatrix3fv();
extern int glUniformMatrix4fv();
extern GLboolean glUnmapBuffer(GLenum target);
extern int glUseProgram();
extern int glVertexAttribPointer();
extern int glViewport();
