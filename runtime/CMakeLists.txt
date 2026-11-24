add_library(snx_runtime STATIC rt.c loader.c overrides_crt.c overrides_core.c overrides_engine.c overrides_launch.c ovr_orig_weak.c script_fix.c vmem.c heap.c process.c hle/vfs.c)
if(NINTENDO_SWITCH)
  target_sources(snx_runtime PRIVATE ${PROJECT_SOURCE_DIR}/platform/switch/mem.c)
endif()
target_include_directories(snx_runtime PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(snx_runtime PUBLIC m)

# HLE Win32: объектная библиотека — сильные определения всегда попадают в сборку и перекрывают слабые
# заглушки build/gen/hle_stubs.c. Харнесс дифф-тестов её не линкует (там импорты — ловушки, как в unicorn).
add_library(snx_hle OBJECT hle/hle.c hle/kernel32.c hle/kernel32_file.c hle/advapi32.c hle/user32.c hle/gdi32.c
            hle/com.c hle/ddraw.c hle/d3d8.c hle/d3d8_dev.c hle/d3d8_res.c hle/d3d8_ff.c hle/d3d8_ps.c hle/gl_cs.c
            hle/misc.c hle/shell32.c hle/wsock32.c hle/dinput8.c hle/overlay.c game/uobj.c game/menu.c game/rumble.c hle/bink.c hle/bink_audio.c hle/openal.c hle/vorbisfile.c
            ${PROJECT_SOURCE_DIR}/platform/window.c)
if(ANDROID)
  # OpenGL ES 3.0 вместо GL 4.1 core (platform/gles_shim.c), потоки на больших ядрах (platform/android/thread.c)
  target_sources(snx_hle PRIVATE ${PROJECT_SOURCE_DIR}/platform/gles_shim.c ${PROJECT_SOURCE_DIR}/platform/android/thread.c)
  if(SNX_ANDROID_VIDEO)
    target_sources(snx_hle PRIVATE hle/bink_video.c)
  else()
    target_sources(snx_hle PRIVATE hle/bink_video_null.c)
  endif()
else()
  target_sources(snx_hle PRIVATE hle/bink_video.c)
endif()
target_include_directories(snx_hle PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(snx_hle PUBLIC snx_sdl2 snx_audio snx_font)  # D3D8 -> GL, окно SDL; OpenAL, vorbisfile; FreeType
