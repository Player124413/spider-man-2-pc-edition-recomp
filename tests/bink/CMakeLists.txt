# Декодер звука Bink отдельно от рантайма (сверка с ffmpeg — compare.py)
add_executable(bink_dec bink_dec.c ${PROJECT_SOURCE_DIR}/runtime/hle/bink_audio.c)
target_include_directories(bink_dec PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bink_dec PRIVATE m)
