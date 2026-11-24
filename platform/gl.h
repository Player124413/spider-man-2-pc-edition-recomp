// Единая точка подключения OpenGL: mac — системный GL 4.1 core, Linux — системный GL с прототипами,
// Switch — mesa через glad, Android — OpenGL ES 3.0 (с прослойкой platform/gles_shim.h: недостающие в ES вызовы
// и форматы D3D8 — BGRA, DXT без расширения и т. п.). Общий диалект шейдеров: GLSL `#version 410 core`; на ES —
// `#version 300 es` (SNX_GLSL_HEADER + SNX_GLSL_PRECISION: между ними можно вставить строки #extension).
#pragma once

#if defined(__SWITCH__)
#include <glad/glad.h>
#define SNX_GL_NEEDS_LOADER 1
#elif defined(__APPLE__)
#include <OpenGL/gl3.h>
#define SNX_GL_NEEDS_LOADER 0
#elif defined(__ANDROID__)  // раньше __linux__: на Android он тоже определён
#define SNX_GLES 1
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#define SNX_GL_NEEDS_LOADER 0
#elif defined(__linux__)
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#endif
#include <GL/gl.h>
#include <GL/glext.h>
#define SNX_GL_NEEDS_LOADER 0
#else
#error "unsupported platform"
#endif

#ifdef SNX_GLES
#define SNX_GL_MAJOR 3
#define SNX_GL_MINOR 0
#define SNX_GLSL_HEADER "#version 300 es\n"
#define SNX_IS_GLES 1
#define SNX_GLSL_PRECISION "precision highp float;\nprecision highp int;\n"
#include "platform/gles_shim.h"
#else
#define SNX_GL_MAJOR 4
#define SNX_GL_MINOR 1
#define SNX_GLSL_HEADER "#version 410 core\n"
#define SNX_IS_GLES 0
#define SNX_GLSL_PRECISION ""
#endif
