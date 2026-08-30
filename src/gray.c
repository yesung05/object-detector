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
                  int motion_gt, int health_ge,
                  int block_min_changed,
                  GrayStats *out, MotionMap *map) {
    int w, h, x, y;
    unsigned long luma_sum = 0;
    size_t changed_motion = 0;
    size_t changed_health = 0;
    /* 블록 행 하나만큼의 카운터. 전체 블록 배열을 스택에 잡지 않기 위해
     * 블록 행이 끝날 때마다 비트로 접어 넣고 재사용합니다. */
    uint16_t row_counts[GRAY_MAX_BLOCKS_X];
    int blocks_x = 0, blocks_y = 0, cur_brow = -1;

    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (map) memset(map, 0, sizeof(*map));
    if (!cur || !cur->data) return;

    w = cur->width;
    h = cur->height;

    if (map) {
        blocks_x = (w + GRAY_BLOCK_SIZE - 1) / GRAY_BLOCK_SIZE;
        blocks_y = (h + GRAY_BLOCK_SIZE - 1) / GRAY_BLOCK_SIZE;
        /* 상한을 넘으면 지도만 포기하고 통계는 계속 냅니다.
         * 지도가 없으면 호출자는 보수적으로 추론을 실행하면 되므로
         * 오작동이 아니라 최적화 미적용으로 떨어집니다. */
        if (blocks_x > GRAY_MAX_BLOCKS_X ||
            blocks_x * blocks_y > GRAY_MAX_BLOCKS) {
            map = NULL;
        } else {
            map->blocks_x = blocks_x;
            map->blocks_y = blocks_y;
            memset(row_counts, 0, sizeof(row_counts));
        }
    }

    if (!prev) {
        int n = w * h;
        int i;
        for (i = 0; i < n; ++i) luma_sum += cur->data[i];
        out->luma_sum = luma_sum;
        out->pixels   = n;
        return;
    }

    /* 한 번의 순회에서 luma 합, 두 종류의 변화 픽셀 수, 블록 지도를 모두 냅니다. */
    for (y = 0; y < h; ++y) {
        const uint8_t *c = cur->data + (size_t)y * (size_t)w;
        const uint8_t *p = prev      + (size_t)y * (size_t)w;
        int brow = y / GRAY_BLOCK_SIZE;

        if (map && brow != cur_brow) {
            /* 이전 블록 행을 비트로 접어 넣고 카운터를 초기화합니다. */
            if (cur_brow >= 0) {
                for (x = 0; x < blocks_x; ++x) {
                    if (row_counts[x] >= (uint16_t)block_min_changed) {
                        int idx = cur_brow * blocks_x + x;
                        map->bits[idx >> 5] |= 1u << (idx & 31);
                        map->changed_blocks++;
                    }
                }
            }
            memset(row_counts, 0, sizeof(row_counts));
            cur_brow = brow;
        }

        for (x = 0; x < w; ++x) {
            int v = c[x];
            int diff = v - (int)p[x];
            if (diff < 0) diff = -diff;
            luma_sum += (unsigned long)v;
            if (diff >  motion_gt) {
                changed_motion++;
                if (map) row_counts[x / GRAY_BLOCK_SIZE]++;
            }
            if (diff >= health_ge) changed_health++;
        }
    }

    /* 마지막 블록 행 마무리 */
    if (map && cur_brow >= 0) {
        for (x = 0; x < blocks_x; ++x) {
            if (row_counts[x] >= (uint16_t)block_min_changed) {
                int idx = cur_brow * blocks_x + x;
                map->bits[idx >> 5] |= 1u << (idx & 31);
                map->changed_blocks++;
            }
        }
    }

    out->luma_sum       = luma_sum;
    out->pixels         = w * h;
    out->changed_motion = changed_motion;
    out->changed_health = changed_health;
}

int gray_blocks_outside(const MotionMap *map, const GrayRect *boxes,
                        int box_count, int downsample, int margin_blocks) {
    int bx, by, i, outside = 0;
    if (!map || map->blocks_x <= 0 || downsample < 1) return 0;
    if (map->changed_blocks == 0) return 0;

    for (by = 0; by < map->blocks_y; ++by) {
        for (bx = 0; bx < map->blocks_x; ++bx) {
            int idx = by * map->blocks_x + bx;
            int covered = 0;
            if (!motion_map_get(map, idx)) continue;
            for (i = 0; i < box_count; ++i) {
                /* 박스를 그레이 블록 좌표로 옮깁니다.
                 * 원본 픽셀 → 그레이 픽셀(/downsample) → 블록(/BLOCK_SIZE) */
                float scale = (float)(downsample * GRAY_BLOCK_SIZE);
                int b_x1 = (int)(boxes[i].x1 / scale) - margin_blocks;
                int b_y1 = (int)(boxes[i].y1 / scale) - margin_blocks;
                int b_x2 = (int)(boxes[i].x2 / scale) + margin_blocks;
                int b_y2 = (int)(boxes[i].y2 / scale) + margin_blocks;
                if (bx >= b_x1 && bx <= b_x2 && by >= b_y1 && by <= b_y2) {
                    covered = 1;
                    break;
                }
            }
            if (!covered) outside++;
        }
    }
    return outside;
}
