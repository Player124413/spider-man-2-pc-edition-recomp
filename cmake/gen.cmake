# Сгенерированный рекомпилятором C (локально, из файлов игры; в git не входит).
# SNX_VARIANT: пусто — английская версия (build/gen), rus — русская локализация Spider-Man 2.
# В копии coolgame русская локализация лежит в System/*.int (Startup.int в UTF-16 с кириллицей), а PE-модули
# те же самые, поэтому отдельный build/gen-rus не нужен: если его нет, берём общий build/gen.
set(SNX_VARIANT "" CACHE STRING "Версия игры: пусто — английская, rus — русская локализация")
if(SNX_VARIANT)
  set(snx_gen_default "${PROJECT_SOURCE_DIR}/build/gen-${SNX_VARIANT}")
  if(NOT EXISTS "${snx_gen_default}" AND EXISTS "${PROJECT_SOURCE_DIR}/build/gen")
    message(STATUS "SNX_VARIANT=${SNX_VARIANT}: отдельного gen нет, используем общий build/gen (та же PE-копия)")
    set(snx_gen_default "${PROJECT_SOURCE_DIR}/build/gen")
  endif()
else()
  set(snx_gen_default "${PROJECT_SOURCE_DIR}/build/gen")
endif()
set(SNX_GEN_DIR "${snx_gen_default}" CACHE PATH "Каталог с выводом recomp")
option(SNX_TRACE "Кольцевой буфер входов в функции (отладка)" OFF)
# Регистры гостя закреплены за x19–x27 (runtime/gen_rt.h, abi.h): глобальные регистровые переменные есть только в
# GCC на ARM64 — clang Apple их не умеет, поэтому мак (и дифф-тесты) — без этого, с регистрами через CPU.
if(NINTENDO_SWITCH)
  option(SNX_PIN "Регистры гостя в регистрах хоста (ARM64, GCC)" ON)
elseif(UNIX AND NOT APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$" AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
  option(SNX_PIN "Регистры гостя в регистрах хоста (ARM64, GCC)" OFF)
endif()
if(SNX_PIN)
  add_compile_definitions(SNX_PIN)
endif()
if(NINTENDO_SWITCH)
  # Сборка по профилю GCC (AGENTS.md, «PGO»): gen — со счётчиками (snx_boot --pgo-dump S пишет *.gcda в
  # sdmc:/switch/shrek2/pgo), use — по профилю из SNX_PGO_DIR (tools/pgo_fetch.py). Имена файлов профиля — путь
  # объектника от каталога сборки, поэтому годится профиль из другого каталога (build/switch-pgo -> build/switch).
  # Профиль — для того же сгенерированного C: после recomp функции с другим графом переходов идут без профиля
  set(SNX_PGO "" CACHE STRING "Сборка по профилю GCC: пусто, gen или use")
  set(SNX_PGO_DIR "${PROJECT_SOURCE_DIR}/build/pgo" CACHE PATH "Каталог профиля (*.gcda) для SNX_PGO=use")
  if(SNX_PGO STREQUAL "gen")
    add_compile_options(-fprofile-generate=/switch/shrek2/pgo -fprofile-prefix-path=${CMAKE_BINARY_DIR}
                        -fprofile-update=single)
    add_link_options(-fprofile-generate)
    add_compile_definitions(SNX_PGO_GEN)
  elseif(SNX_PGO STREQUAL "use")
    # холодное (не исполнявшееся в прогоне) — в .text.unlikely, на размер: горячий код плотнее в кеше инструкций.
    # Без -fprofile-partial-training: с ним нули считаются неизвестностью и выносится в разы меньше; код других
    # уровней, которого не было в прогоне, добирается прогонами там (счётчики складываются)
    add_compile_options(-fprofile-use=${SNX_PGO_DIR} -fprofile-prefix-path=${CMAKE_BINARY_DIR}
                        -freorder-blocks-and-partition -fprofile-correction -Wno-missing-profile
                        -Wno-coverage-mismatch)
    set(CMAKE_C_COMPILER_LAUNCHER "${PROJECT_SOURCE_DIR}/tools/cc_partition.sh")  # файлы, где вынос не собирается
  elseif(SNX_PGO)
    message(FATAL_ERROR "SNX_PGO: пусто, gen или use")
  endif()
endif()
set(SNX_GEN_FLAGS $<$<BOOL:${SNX_TRACE}>:-DSNX_TRACE> -Wno-unused-label -Wno-parentheses-equality -Wno-unused-variable
    -Wno-unused-but-set-variable -Wno-unused-function -fno-strict-aliasing -fno-math-errno -fno-trapping-math)
if(SNX_PIN)
  # CPU *c закреплён в x28 (runtime/gen_rt.h): -ffixed-x28 гарантирует сохранность x28 во всех функциях гостя
  list(APPEND SNX_GEN_FLAGS -ffixed-x28)
endif()

# snx_gen_module(<stem>) -> цель snx_gen_<stem>, если каталог существует
function(snx_gen_module stem)
  set(dir "${SNX_GEN_DIR}/${stem}")
  if(NOT EXISTS "${dir}")
    return()
  endif()
  file(GLOB srcs CONFIGURE_DEPENDS "${dir}/*.c")
  add_library(snx_gen_${stem} STATIC ${srcs})
  target_include_directories(snx_gen_${stem} PRIVATE ${PROJECT_SOURCE_DIR})
  target_compile_options(snx_gen_${stem} PRIVATE ${SNX_GEN_FLAGS} $<$<CONFIG:Debug>:-O1>)
endfunction()

set(SNX_GEN_STEMS msvcr71 core engine d3ddrv windrv window fire gui gameplay webhead webhead_exe alaudio)
foreach(s ${SNX_GEN_STEMS})
  snx_gen_module(${s})
endforeach()
if(EXISTS "${SNX_GEN_DIR}/hle_stubs.c")
  add_library(snx_gen_hle_stubs STATIC "${SNX_GEN_DIR}/hle_stubs.c")
  target_include_directories(snx_gen_hle_stubs PRIVATE ${PROJECT_SOURCE_DIR})
endif()
