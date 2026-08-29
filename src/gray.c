#include "gray.h"

#include <stddef.h>
#include <stdlib.h>

static int clampi_gray(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int gray_buf_init(GrayBuf *g, int source_width, int source_height, int downsample) {
    int w, h;
    uint8_t *buf;
    if (!g || source_width <= 0 || source_height <= 0 || downsample < 1)
        return -1;
    w = (source_width  + downsample - 1) / downsample;
    h = (source_height + downsample - 1) / downsample;
    buf = (uint8_t *)malloc((size_t)w * (size_t)h);
    if (!buf) return -1;
    g->data      = buf;
    g->width     = w;
    g->height    = h;
    g->downsample = downsample;
    return 0;
}

void gray_buf_destroy(GrayBuf *g) {
    if (!g) return;
    free(g->data);
    g->data = NULL;
}

/*
 * 고정소수점 luma: (77R + 150G + 29B + 128) >> 8
 * tracker.c의 make_gray()와 동일한 공식이므로 두 모듈이 같은 결과를 냅니다.
 * 포인트 샘플링: 각 다운샘플 셀의 중앙 픽셀 한 개만 읽어 처리 비용을 최소화합니다.
 */
void gray_buf_update(GrayBuf *g, const uint8_t *rgb,
                     int width, int height, int stride) {
    int step, y, x, source_y, source_x;
    const uint8_t *pixel;
    if (!g || !g->data || !rgb) return;
    step = g->downsample;
    for (y = 0; y < g->height; ++y) {
        source_y = clampi_gray(y * step + step / 2, 0, height - 1);
        for (x = 0; x < g->width; ++x) {
            source_x = clampi_gray(x * step + step / 2, 0, width - 1);
            pixel = rgb + source_y * stride + source_x * 3;
            g->data[y * g->width + x] =
                (uint8_t)((77u * pixel[0] + 150u * pixel[1] +
                           29u * pixel[2] + 128u) >> 8);
        }
    }
}
