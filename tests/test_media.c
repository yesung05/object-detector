#include "media.h"

#include <stdio.h>

/*
 * FFmpeg 파이프라인의 간단한 통합 테스트입니다.
 * 각 프레임의 왼쪽 위 픽셀을 빨간색으로 바꿔 디코딩 → 콜백 → 인코딩 흐름이
 * 실제 파일 입출력까지 연결되는지 확인합니다.
 */

static int mark_frame(RgbFrame *frame, void *opaque,
                      char *error, size_t error_size) {
    (void)opaque;
    (void)error;
    (void)error_size;
    if (frame->width > 0 && frame->height > 0) {
        frame->data[0] = 255;
        frame->data[1] = 0;
        frame->data[2] = 0;
    }
    return 0;
}

int main(int argc, char **argv) {
    MediaOptions options = {
        .codec = "mpeg4",
        .quality = 23,
        .max_frames = 0
    };
    char error[256] = {0};
    if (argc != 3) {
        fprintf(stderr, "usage: %s INPUT OUTPUT\n", argv[0]);
        return 2;
    }
    if (media_process(argv[1], argv[2], &options, mark_frame, NULL,
                      error, sizeof(error)) != 0) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    return 0;
}
