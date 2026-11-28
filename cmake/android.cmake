# Android (arm64-v8a): библиотеки собираются из исходников вместе с игрой. Исходники кладёт
# tools/android_deps.sh в SNX_ANDROID_DEPS (по умолчанию build/android-deps): SDL2, openal-soft, ogg, vorbis,
# freetype и, если собран, FFmpeg (только декодер Bink; без него ролики — чёрный кадр со звуком).
# Сборку запускает Gradle (android/app/build.gradle -> externalNativeBuild) или tools/build.sh android.
set(SNX_PLATFORM android)
if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a" AND NOT ANDROID_ABI STREQUAL "arm64-v8a")
  # окно гостя — резерв 4 ГБ адресов (runtime/rt.c): нужен 64-битный процесс; x86_64 — только для эмулятора
  if(NOT ANDROID_ABI STREQUAL "x86_64")
    message(FATAL_ERROR "Android: поддерживаются только arm64-v8a (и x86_64 для эмулятора)")
  endif()
endif()
set(SNX_ANDROID_DEPS "${PROJECT_SOURCE_DIR}/build/android-deps" CACHE PATH "Исходники библиотек (tools/android_deps.sh)")
set(deps "${SNX_ANDROID_DEPS}")
foreach(d SDL openal-soft ogg vorbis freetype)
  if(NOT EXISTS "${deps}/${d}")
    message(FATAL_ERROR "Нет ${deps}/${d}: запусти tools/android_deps.sh")
  endif()
endforeach()

# тяжёлые для ЦП телефона настройки по умолчанию: -O2 (как Release), без проверок стека и PLT для своих вызовов
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-fno-stack-protector -fno-plt $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>)
add_link_options(-Wl,--gc-sections -Wl,-z,max-page-size=16384)  # 16 КБ страниц (Android 15+)

# ---------------------------------------------------------------- SDL2 (libSDL2.so: её грузит SDLActivity)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
add_subdirectory("${deps}/SDL" "${CMAKE_BINARY_DIR}/deps/SDL" EXCLUDE_FROM_ALL)
add_library(snx_sdl2 INTERFACE)
target_link_libraries(snx_sdl2 INTERFACE SDL2::SDL2 GLESv3 EGL android log)

# ---------------------------------------------------------------- OpenAL Soft (статически; вывод — AAudio/OpenSL)
set(LIBTYPE STATIC CACHE STRING "" FORCE)
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EMBED_HRTF_DATA ON CACHE BOOL "" FORCE)  # 160 КБ; при OFF в 1.25.2 — пустой add_dependencies
set(ALSOFT_BACKEND_OBOE OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_OPENSL ON CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_WAVE OFF CACHE BOOL "" FORCE)
set(ALSOFT_ENABLE_MODULES OFF CACHE BOOL "" FORCE)  # модули C++20 не нужны (и требуют свежий Ninja)
add_subdirectory("${deps}/openal-soft" "${CMAKE_BINARY_DIR}/deps/openal" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------- ogg + vorbis + vorbisfile (своим списком файлов)
set(ogg "${deps}/ogg")
set(vorbis "${deps}/vorbis")
file(WRITE "${CMAKE_BINARY_DIR}/deps/ogg_inc/ogg/config_types.h"
     "#pragma once\n#include <stdint.h>\ntypedef int16_t ogg_int16_t;\ntypedef uint16_t ogg_uint16_t;\n"
     "typedef int32_t ogg_int32_t;\ntypedef uint32_t ogg_uint32_t;\ntypedef int64_t ogg_int64_t;\n"
     "typedef uint64_t ogg_uint64_t;\n")
set(vorbis_srcs mdct smallft block envelope window lsp lpc analysis synthesis psy info floor1 floor0 res0
    mapping0 registry codebook sharedbook lookup bitrate vorbisfile)
list(TRANSFORM vorbis_srcs PREPEND "${vorbis}/lib/")
list(TRANSFORM vorbis_srcs APPEND ".c")
add_library(snx_vorbis STATIC "${ogg}/src/bitwise.c" "${ogg}/src/framing.c" ${vorbis_srcs})
target_include_directories(snx_vorbis PUBLIC "${CMAKE_BINARY_DIR}/deps/ogg_inc" "${ogg}/include" "${vorbis}/include"
                           PRIVATE "${vorbis}/lib")
target_compile_options(snx_vorbis PRIVATE -O3 -w)

# ---------------------------------------------------------------- FreeType (надписи слоя интерфейса, GDI)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
add_subdirectory("${deps}/freetype" "${CMAKE_BINARY_DIR}/deps/freetype" EXCLUDE_FROM_ALL)
add_library(snx_font INTERFACE)
target_link_libraries(snx_font INTERFACE freetype)

# ---------------------------------------------------------------- FFmpeg (декодер Bink), собранный заранее
set(ffmpeg "${deps}/ffmpeg/${ANDROID_ABI}")
if(EXISTS "${ffmpeg}/lib/libavcodec.a")
  set(snx_video_default ON)
else()
  set(snx_video_default OFF)
endif()
option(SNX_ANDROID_VIDEO "Ролики Bink через FFmpeg (tools/android_deps.sh --ffmpeg)" ${snx_video_default})
add_library(snx_audio INTERFACE)
if(TARGET OpenAL::OpenAL)
  target_link_libraries(snx_audio INTERFACE OpenAL::OpenAL snx_vorbis)
else()
  target_link_libraries(snx_audio INTERFACE OpenAL snx_vorbis)
endif()
target_compile_definitions(snx_audio INTERFACE AL_LIBTYPE_STATIC)
if(SNX_ANDROID_VIDEO)
  target_include_directories(snx_audio INTERFACE "${ffmpeg}/include")
  target_link_libraries(snx_audio INTERFACE "${ffmpeg}/lib/libavcodec.a" "${ffmpeg}/lib/libavutil.a")
else()
  message(WARNING "FFmpeg для Android не найден (${ffmpeg}): ролики будут без изображения")
endif()
