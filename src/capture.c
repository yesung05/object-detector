#include "capture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

/*
 * stb_image_write 를 이 번역 단위 전용(static)으로 한 벌 더 구현합니다.
 * stream.c 의 구현은 Windows 전용 빌드에만 들어가 Linux/macOS 에서는 쓸 수 없고,
 * static 이므로 Windows 에서 stream.c 와 심볼이 충돌하지 않습니다.
 * 쓰지 않는 PNG/BMP 인코더 경고는 외부 코드라 여기서만 끕니다.
 */
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4505 4244 4456 4457 4245 4100)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO   /* 파일 쓰기 오류를 직접 확인하려고 콜백 경로만 씁니다 */
#include "../third_party/stb/stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static int make_dir(const char *path) {
#if defined(_WIN32)
    int r = _mkdir(path);
#else
    int r = mkdir(path, 0755);
#endif
    return (r == 0 || errno == EEXIST) ? 0 : -1;
}

int capture_init(EventCapture *cap, const char *dir, int quality,
                 char *error, size_t error_size) {
    if (!cap || !dir || !dir[0]) {
        if (error) snprintf(error, error_size, "capture_init: invalid args");
        return -1;
    }
    memset(cap, 0, sizeof(*cap));
    if (strlen(dir) >= sizeof(cap->dir)) {
        if (error) snprintf(error, error_size, "capture_init: path too long");
        return -1;
    }
    if (make_dir(dir) != 0) {
        if (error)
            snprintf(error, error_size, "capture_init: mkdir %s failed: %s",
                     dir, strerror(errno));
        return -1;
    }
    strcpy(cap->dir, dir);
    cap->quality = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
    return 0;
}

void capture_destroy(EventCapture *cap) {
    if (!cap) return;
    free(cap->buf);
    cap->buf = NULL;
    cap->buf_size = 0;
}

typedef struct {
    FILE *file;
    int   failed;
} JpegFileSink;

static void jpeg_to_file(void *context, void *data, int size) {
    JpegFileSink *sink = (JpegFileSink *)context;
    if (sink->failed || size <= 0) return;
    if (fwrite(data, 1, (size_t)size, sink->file) != (size_t)size)
        sink->failed = 1;
}

int capture_save(EventCapture *cap, const char *kind, int track_id,
                 const uint8_t *rgb, int width, int height, int stride,
                 const Detection *box, int privacy_mask,
                 char *path_out, size_t path_size,
                 char *error, size_t error_size) {
    char stamp[32];
    char path[700];
    char tmp[710];
    time_t raw = time(NULL);
    struct tm tmv;
    size_t row_bytes, need;
    JpegFileSink sink;
    int y, ok;

    if (path_out && path_size) path_out[0] = '\0';
    if (!cap || !cap->dir[0] || !kind || !rgb || width <= 0 || height <= 0 ||
        stride < width * 3) {
        if (error) snprintf(error, error_size, "capture_save: invalid args");
        return -1;
    }

#if defined(_WIN32)
    localtime_s(&tmv, &raw);
#else
    localtime_r(&raw, &tmv);
#endif
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
    snprintf(path, sizeof(path), "%s/%s_%s_track%d.jpg", cap->dir, kind, stamp,
             track_id);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (path_out && path_size) snprintf(path_out, path_size, "%s", path);

    row_bytes = (size_t)width * 3;
    need = row_bytes * (size_t)height;
    if (need > cap->buf_size) {
        uint8_t *nb = (uint8_t *)realloc(cap->buf, need);
        if (!nb) {
            if (error) snprintf(error, error_size, "capture_save: out of memory");
            return -1;
        }
        cap->buf = nb;
        cap->buf_size = need;
    }
    if (privacy_mask) {
        /* main.c 의 보호 모드 화면과 비슷한 어두운 회색. 박스 선이 잘 보이는 밝기입니다. */
        memset(cap->buf, 0x30, need);
    } else {
        for (y = 0; y < height; ++y)
            memcpy(cap->buf + (size_t)y * row_bytes, rgb + (size_t)y * (size_t)stride,
                   row_bytes);
    }

    if (box) {
        /* draw_detections 는 DetectionList 를 받으므로 박스 하나짜리 목록을 빌려
         * 만듭니다. items 는 호출자의 box 를 가리킬 뿐 소유하지 않습니다. */
        DetectionList one;
        one.items = (Detection *)box;
        one.count = 1;
        one.capacity = 1;
        draw_detections(cap->buf, width, height, (int)row_bytes, &one);
    }

    /* 임시 파일로 끝까지 쓴 뒤 이름을 바꿔, 대시보드 등이 반쯤 쓴 JPEG 를 읽지 않게 합니다. */
    sink.file = fopen(tmp, "wb");
    if (!sink.file) {
        if (error)
            snprintf(error, error_size, "capture_save: open %s failed: %s",
                     tmp, strerror(errno));
        return -1;
    }
    sink.failed = 0;
    ok = stbi_write_jpg_to_func(jpeg_to_file, &sink, width, height, 3, cap->buf,
                                cap->quality);
    if (fclose(sink.file) != 0) sink.failed = 1;
    if (!ok || sink.failed) {
        remove(tmp);
        if (error) snprintf(error, error_size, "capture_save: write %s failed", tmp);
        return -1;
    }
    remove(path);  /* Windows rename() 은 대상이 있으면 실패 */
    if (rename(tmp, path) != 0) {
        remove(tmp);
        if (error)
            snprintf(error, error_size, "capture_save: rename to %s failed", path);
        return -1;
    }
    return 0;
}
