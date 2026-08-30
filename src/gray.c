#include "gray.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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

/*
 * Y 평면은 픽셀당 1바이트이므로 산술 없이 그대로 옮깁니다.
 * gray_buf_update()와 동일한 중앙 픽셀 샘플링 위치를 사용하여, 두 경로가
 * 같은 좌표를 보도록 맞춥니다(입력 색공간이 달라 값은 완전히 같지 않습니다).
 */
void gray_buf_update_luma(GrayBuf *g, const uint8_t *luma,
                          int width, int height, int stride) {
    int step, y, x, source_y, source_x;
    const uint8_t *row;
    uint8_t *dst;
    if (!g || !g->data || !luma) return;
    step = g->downsample;
    for (y = 0; y < g->height; ++y) {
        source_y = clampi_gray(y * step + step / 2, 0, height - 1);
        row = luma + (size_t)source_y * (size_t)stride;
        dst = g->data + (size_t)y * (size_t)g->width;
        for (x = 0; x < g->width; ++x) {
            source_x = clampi_gray(x * step + step / 2, 0, width - 1);
            dst[x] = row[source_x];
        }
    }
}

void gray_analyze(const GrayBuf *cur, const uint8_t *prev,
                  int motion_gt, int health_ge, GrayStats *out) {
    int n, i;
    unsigned long luma_sum = 0;
    size_t changed_motion = 0;
    size_t changed_health = 0;
    const uint8_t *c;

    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!cur || !cur->data) return;

    c = cur->data;
    n = cur->width * cur->height;

    if (!prev) {
        for (i = 0; i < n; ++i) luma_sum += c[i];
        out->luma_sum = luma_sum;
        out->pixels   = n;
        return;
    }

    /* 한 번의 순회에서 세 통계를 모두 냅니다. 두 임계값 비교는 같은 diff를
     * 재사용하므로 추가 메모리 접근이 없습니다. */
    for (i = 0; i < n; ++i) {
        int v = c[i];
        int diff = v - (int)prev[i];
        if (diff < 0) diff = -diff;
        luma_sum += (unsigned long)v;
        if (diff >  motion_gt) changed_motion++;
        if (diff >= health_ge) changed_health++;
    }

    out->luma_sum       = luma_sum;
    out->pixels         = n;
    out->changed_motion = changed_motion;
    out->changed_health = changed_health;
}
