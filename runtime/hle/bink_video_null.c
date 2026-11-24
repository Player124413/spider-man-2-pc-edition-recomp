// Видео Bink без libavcodec (сборка Android с -DSNX_ANDROID_VIDEO=OFF): ролики идут чёрным кадром со звуком
// (звуковые дорожки декодирует bink_audio.c). Полная сборка — с FFmpeg из tools/android_deps.sh.
#include "runtime/hle/bink.h"

bink_video *bink_video_new(u32 signature, u32 video_flags, u32 w, u32 h)
{
    (void)signature;
    (void)video_flags;
    (void)w;
    (void)h;
    return 0;
}

void bink_video_free(bink_video *v) { (void)v; }
void bink_video_flush(bink_video *v) { (void)v; }

int bink_video_decode(bink_video *v, const u8 *packet, u32 size, int key)
{
    (void)v;
    (void)packet;
    (void)size;
    (void)key;
    return -1;
}

void bink_video_copy(bink_video *v, u8 *dst, u32 pitch, u32 rows, u32 x, u32 y, u32 flags)
{
    (void)v;
    (void)dst;
    (void)pitch;
    (void)rows;
    (void)x;
    (void)y;
    (void)flags;
}

void bink_video_prefetch(bink_video *v, const u8 *packet, u32 size, int key)
{
    (void)v;
    (void)packet;
    (void)size;
    (void)key;
}

void bink_video_stats(bink_video *v, double *dec_ms, double *wait_ms, u32 *frames)
{
    (void)v;
    *dec_ms = *wait_ms = 0;
    *frames = 0;
}
