// Фаза 0 / спайк S3: SDL2-окно + GL 4.1 core + GLSL 410 + DXT1-текстура на mac и Switch.
// Запуск: snx_hello [--frames N]. Код выхода 0, если самопроверка прошла.
#include <SDL.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform/gl.h"
#include "platform/log.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif

static const char *vs_src = SNX_GLSL_HEADER SNX_GLSL_PRECISION
    "layout(location=0) in vec2 pos;\n"
    "out vec2 uv;\n"
    "void main(){ uv = pos*0.5+0.5; gl_Position = vec4(pos,0.0,1.0); }\n";
static const char *fs_src = SNX_GLSL_HEADER SNX_GLSL_PRECISION
    "in vec2 uv; uniform sampler2D tex; uniform float t;\n"
    "out vec4 color;\n"
    "void main(){ color = texture(tex, uv) * vec4(1.0, 0.5+0.5*sin(t), 1.0, 1.0); }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        SNX_LOG("shader compile failed: %s", log);
        exit(2);
    }
    return s;
}

static int has_ext(const char *name)
{
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++)
        if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, i), name))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
#ifdef __SWITCH__
    socketInitializeDefault();
    nxlinkStdio();
#endif
    int max_frames = 0;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--frames"))
            max_frames = atoi(argv[i + 1]);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, SNX_GL_MAJOR);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, SNX_GL_MINOR);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("webheadNX hello", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1280, 720, SDL_WINDOW_OPENGL);
    if (!win) {
        SDL_Log("SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        SDL_Log("SDL_GL_CreateContext: %s", SDL_GetError());
        return 1;
    }
#if SNX_GL_NEEDS_LOADER
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        SDL_Log("gladLoadGL failed");
        return 1;
    }
#endif
    SDL_GL_SetSwapInterval(1);
    SDL_JoystickOpen(0);

    SNX_LOG("GL_VERSION  %s", glGetString(GL_VERSION));
    SNX_LOG("GL_RENDERER %s", glGetString(GL_RENDERER));
    SNX_LOG("GLSL        %s", glGetString(GL_SHADING_LANGUAGE_VERSION));
    int s3tc = has_ext("GL_EXT_texture_compression_s3tc");
    SNX_LOG("S3TC        %s", s3tc ? "yes" : "NO");

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vs_src));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs_src));
    glLinkProgram(prog);

    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    // DXT1-блок 4x4: color0 = чистый красный (RGB565 0xF800), все индексы 0.
    static const uint8_t dxt1_red[8] = {0x00, 0xF8, 0x00, 0x00, 0, 0, 0, 0};
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (s3tc)
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, 4, 4, 0, sizeof dxt1_red, dxt1_red);
    SNX_LOG("glGetError after setup: 0x%x", glGetError());

    int frame = 0, running = 1, check_ok = -1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                running = 0;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
                running = 0;
            if (ev.type == SDL_JOYBUTTONDOWN && ev.jbutton.button == 10) // «+» на Switch
                running = 0;
        }
        int w, h;
        SDL_GL_GetDrawableSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.3f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog);
        glUniform1f(glGetUniformLocation(prog, "t"), frame == 0 ? 3.14159265f / 2 : frame / 30.0f);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        if (frame == 0) {
            uint8_t px[4] = {0};
            glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            check_ok = s3tc && px[0] > 240 && px[1] < 16 && px[2] < 16;
            SNX_LOG("center pixel rgba=%u,%u,%u,%u -> DXT1 check %s", px[0], px[1], px[2], px[3],
                    check_ok ? "PASS" : "FAIL");
        }
        SDL_GL_SwapWindow(win);
        if (max_frames && ++frame >= max_frames)
            running = 0;
        else if (!max_frames)
            frame++;
    }
    SNX_LOG("frames rendered: %d", frame);
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
#ifdef __SWITCH__
    socketExit();
#endif
    return check_ok == 1 ? 0 : 3;
}
