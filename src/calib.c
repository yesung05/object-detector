#include "calib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 게이트(main.c gray_analyze 호출)와 같은 픽셀 차이 기준을 써야
 * "학습에서 흔들림"과 "게이트에서 변화"가 같은 의미가 됩니다. */
#define CALIB_MOTION_GT 8

/*
 * 한 표본에서 이 비율 이상의 블록이 변했다면 국소 흔들림이 아니라 조명 on/off,
 * 자동 노출, 햇빛 변화 같은 화면 전체 변화입니다. 그런 표본을 세면 모든 블록의
 * 비율이 함께 올라가 멀쩡한 구역까지 무시 후보가 되므로 통째로 버립니다.
 */
#define CALIB_GLOBAL_CHANGE_RATIO 0.5

/* 사람 박스 주변을 몇 블록 더 가릴지. 그림자·팔 움직임이 박스 밖으로 번지는 폭입니다. */
#define CALIB_PEOPLE_MARGIN_BLOCKS 1

/*
 * 블록 판정에 필요한 최소 관측 수. 사람에게 오래 가려진 블록은 표본이 적어
 * 비율이 요동칩니다(관측 2회 중 1회 변화 = 50%). 전체 표본의 1/4 이상,
 * 그리고 절대 20회(0.5초 간격이면 10초) 이상 관측된 블록만 판정합니다.
 */
#define CALIB_MIN_OBSERVED_ABS 20
#define CALIB_MIN_OBSERVED_DIV 4

/*
 * 이중 임계값(hysteresis) 확장. noise_ratio 이상인 블록이 씨앗이 되고, 그 씨앗에
 * 이어진 이웃은 noise_ratio / 3 이상이면 같은 구역에 편입합니다.
 *
 * 흔들림은 가장자리로 갈수록 드물어집니다 — 달력이 끝까지 휘는 순간에만 닿는
 * 블록은 10~20% 만 변합니다. 단일 임계값이면 그 가장자리가 빠져, 흔들림이
 * 끝에 닿을 때마다 YOLO 가 깨어납니다(합성 영상 실측: 추론 357 → 143회에서 멈춤).
 * 씨앗에 이어진 경우에만 낮은 기준을 쓰므로, 홀로 가끔 변하는 블록은 여전히 제외됩니다.
 */
#define CALIB_GROW_RATIO_DIV 3.0

int calib_active(const MotionCalib *c) {
    return c && c->snapshot != NULL;
}

int calib_start(MotionCalib *c, const GrayBuf *g, int frame_width,
                int frame_height, const CalibOptions *opt, double now,
                char *error, size_t error_size) {
    int bx, by;
    if (!c || !g || !g->data || !opt || frame_width <= 0 || frame_height <= 0) {
        if (error) snprintf(error, error_size, "calib_start: invalid args");
        return -1;
    }
    /* gray_analyze 와 같은 격자 계산이어야 블록 인덱스가 맞습니다. */
    bx = (g->width + GRAY_BLOCK_SIZE - 1) / GRAY_BLOCK_SIZE;
    by = (g->height + GRAY_BLOCK_SIZE - 1) / GRAY_BLOCK_SIZE;
    if (bx > GRAY_MAX_BLOCKS_X || bx * by > GRAY_MAX_BLOCKS) {
        if (error)
            snprintf(error, error_size,
                     "calib_start: 블록 격자 %dx%d 가 상한 초과", bx, by);
        return -1;
    }

    calib_destroy(c);
    memset(c, 0, sizeof(*c));
    c->snapshot = (uint8_t *)malloc((size_t)g->width * (size_t)g->height);
    if (!c->snapshot) {
        if (error) snprintf(error, error_size, "calib_start: out of memory");
        return -1;
    }
    c->opt          = *opt;
    c->gray_width   = g->width;
    c->gray_height  = g->height;
    c->downsample   = g->downsample;
    c->frame_width  = frame_width;
    c->frame_height = frame_height;
    c->blocks_x     = bx;
    c->blocks_y     = by;
    c->started      = now;
    c->last_sample  = now;
    return 0;
}

void calib_destroy(MotionCalib *c) {
    if (!c) return;
    free(c->snapshot);
    c->snapshot = NULL;
    c->snapshot_ready = 0;
}

int calib_elapsed_done(const MotionCalib *c, double now) {
    return calib_active(c) && (now - c->started) >= c->opt.duration_seconds;
}

/* 원본 좌표 사각형이 덮는 블록 범위를 margin 만큼 넓혀 격자 안으로 자릅니다. */
static void rect_to_blocks(const MotionCalib *c, const GrayRect *r, int margin,
                           int *x1, int *y1, int *x2, int *y2) {
    float scale = (float)(c->downsample * GRAY_BLOCK_SIZE);
    *x1 = (int)(r->x1 / scale) - margin;
    *y1 = (int)(r->y1 / scale) - margin;
    *x2 = (int)(r->x2 / scale) + margin;
    *y2 = (int)(r->y2 / scale) + margin;
    if (*x1 < 0) *x1 = 0;
    if (*y1 < 0) *y1 = 0;
    if (*x2 > c->blocks_x - 1) *x2 = c->blocks_x - 1;
    if (*y2 > c->blocks_y - 1) *y2 = c->blocks_y - 1;
}

static void mark_rects(const MotionCalib *c, const GrayRect *rects, int count,
                       int margin, uint8_t *mask) {
    int i, x, y;
    for (i = 0; i < count; ++i) {
        int x1, y1, x2, y2;
        rect_to_blocks(c, &rects[i], margin, &x1, &y1, &x2, &y2);
        for (y = y1; y <= y2; ++y)
            for (x = x1; x <= x2; ++x)
                mask[y * c->blocks_x + x] = 1;
    }
}

void calib_accumulate(MotionCalib *c, const MotionMap *map,
                      const GrayRect *people, int people_count) {
    uint8_t occluded[GRAY_MAX_BLOCKS];
    int total, i;
    if (!calib_active(c) || !map) return;
    if (map->blocks_x != c->blocks_x || map->blocks_y != c->blocks_y) return;
    total = c->blocks_x * c->blocks_y;

    if ((double)map->changed_blocks > CALIB_GLOBAL_CHANGE_RATIO * (double)total) {
        c->skipped_global++;
        return;
    }

    memset(occluded, 0, (size_t)total);
    if (people && people_count > 0)
        mark_rects(c, people, people_count, CALIB_PEOPLE_MARGIN_BLOCKS, occluded);

    for (i = 0; i < total; ++i) {
        if (occluded[i]) continue;
        c->observed[i]++;
        if (motion_map_get(map, i)) c->changed[i]++;
    }
    c->samples++;
}

int calib_tick(MotionCalib *c, const GrayBuf *cur, const GrayRect *people,
               int people_count, int block_min_changed, double now) {
    int sampled = 0;
    if (!calib_active(c) || !cur || !cur->data) return 0;
    if (cur->width != c->gray_width || cur->height != c->gray_height) return 0;
    if (c->snapshot_ready && (now - c->last_sample) < c->opt.sample_seconds)
        return 0;

    if (c->snapshot_ready) {
        GrayStats stats;
        MotionMap map;
        /* health_ge=256: 카메라 헬스 집계는 여기서 필요 없어 도달 불가 값을 줍니다. */
        gray_analyze(cur, c->snapshot, NULL, CALIB_MOTION_GT, 256,
                     block_min_changed, &stats, &map);
        calib_accumulate(c, &map, people, people_count);
        sampled = 1;
    }
    memcpy(c->snapshot, cur->data, (size_t)c->gray_width * (size_t)c->gray_height);
    c->snapshot_ready = 1;
    c->last_sample = now;
    return sampled;
}

static int rects_overlap(const GrayRect *a, const GrayRect *b) {
    /* 경계만 맞닿은 경우는 겹침으로 보지 않습니다. 보호 블록을 뺀 뒤 바로 옆
     * 블록으로 만든 사각형은 보호 구역과 경계를 공유할 수 있기 때문입니다. */
    return a->x1 < b->x2 && b->x1 < a->x2 && a->y1 < b->y2 && b->y1 < a->y2;
}

/* out 을 blocks 내림차순으로 유지하며 상위 max_out 개만 남깁니다. */
static void keep_top(CalibRegion *out, int *count, int max_out,
                     const CalibRegion *r) {
    int pos;
    if (*count < max_out) {
        pos = (*count)++;
    } else {
        if (max_out <= 0 || r->blocks <= out[max_out - 1].blocks) return;
        pos = max_out - 1;
    }
    while (pos > 0 && out[pos - 1].blocks < r->blocks) {
        out[pos] = out[pos - 1];
        --pos;
    }
    out[pos] = *r;
}

int calib_extract(const MotionCalib *c, const GrayRect *protect,
                  int protect_count, CalibRegion *out, int max_out,
                  int *total_found) {
    uint8_t flagged[GRAY_MAX_BLOCKS];   /* 씨앗: noise_ratio 이상 */
    uint8_t growable[GRAY_MAX_BLOCKS];  /* 확장 후보: noise_ratio / 3 이상 */
    uint8_t guarded[GRAY_MAX_BLOCKS];
    /* 명시적 스택 flood fill. 재귀는 최악 1024 단계라 스택 깊이를 예측하기 어렵고,
     * 블록 수 상한이 정해져 있어 고정 배열 하나로 충분합니다. */
    int stack[GRAY_MAX_BLOCKS];
    uint32_t min_obs;
    int total, i, written = 0, found = 0;
    float scale;

    if (total_found) *total_found = 0;
    if (!c || c->blocks_x <= 0 || !out || max_out <= 0) return 0;
    total = c->blocks_x * c->blocks_y;
    scale = (float)(c->downsample * GRAY_BLOCK_SIZE);

    min_obs = (uint32_t)(c->samples / CALIB_MIN_OBSERVED_DIV);
    if (min_obs < CALIB_MIN_OBSERVED_ABS) min_obs = CALIB_MIN_OBSERVED_ABS;

    memset(guarded, 0, (size_t)total);
    if (protect && protect_count > 0)
        mark_rects(c, protect, protect_count, 0, guarded);

    for (i = 0; i < total; ++i) {
        double ratio;
        flagged[i] = 0;
        growable[i] = 0;
        if (guarded[i] || c->observed[i] < min_obs) continue;
        ratio = (double)c->changed[i] / (double)c->observed[i];
        if (ratio >= (double)c->opt.noise_ratio) flagged[i] = 1;
        if (ratio >= (double)c->opt.noise_ratio / CALIB_GROW_RATIO_DIV) growable[i] = 1;
    }

    for (i = 0; i < total; ++i) {
        int top = 0, blocks = 0;
        int bx1, by1, bx2, by2, j;
        float ratio_sum = 0.0f;
        CalibRegion r;
        if (!flagged[i]) continue;

        bx1 = bx2 = i % c->blocks_x;
        by1 = by2 = i / c->blocks_x;
        /* 방문 표시는 growable 로 합니다. 씨앗은 모두 growable 이므로
         * 같은 구역에 속한 다른 씨앗도 여기서 함께 소비됩니다. */
        flagged[i] = 0;
        growable[i] = 0;
        stack[top++] = i;
        while (top > 0) {
            int idx = stack[--top];
            int x = idx % c->blocks_x, y = idx / c->blocks_x;
            int nb[4];
            int k;
            blocks++;
            ratio_sum += (float)c->changed[idx] / (float)c->observed[idx];
            if (x < bx1) bx1 = x;
            if (x > bx2) bx2 = x;
            if (y < by1) by1 = y;
            if (y > by2) by2 = y;
            /* 4-이웃: 대각선까지 묶으면 떨어진 두 흔들림이 하나의 큰 사각형이 됩니다. */
            nb[0] = x > 0               ? idx - 1           : -1;
            nb[1] = x < c->blocks_x - 1 ? idx + 1           : -1;
            nb[2] = y > 0               ? idx - c->blocks_x : -1;
            nb[3] = y < c->blocks_y - 1 ? idx + c->blocks_x : -1;
            for (k = 0; k < 4; ++k) {
                if (nb[k] >= 0 && growable[nb[k]]) {
                    flagged[nb[k]] = 0;
                    growable[nb[k]] = 0;
                    stack[top++] = nb[k];
                }
            }
        }

        memset(&r, 0, sizeof(r));
        r.blocks     = blocks;
        r.mean_ratio = ratio_sum / (float)blocks;
        r.rect.x1 = (float)bx1 * scale;
        r.rect.y1 = (float)by1 * scale;
        r.rect.x2 = (float)(bx2 + 1) * scale;
        r.rect.y2 = (float)(by2 + 1) * scale;
        if (r.rect.x2 > (float)c->frame_width)  r.rect.x2 = (float)c->frame_width;
        if (r.rect.y2 > (float)c->frame_height) r.rect.y2 = (float)c->frame_height;

        /* 무시 구역은 사각형 전체이므로 판정도 블록 수가 아니라 사각형 면적으로 합니다. */
        if ((double)((bx2 - bx1 + 1) * (by2 - by1 + 1)) >
            (double)c->opt.max_area_ratio * (double)total) {
            r.status = CALIB_REGION_TOO_LARGE;
        } else {
            r.status = CALIB_REGION_OK;
            for (j = 0; j < protect_count; ++j) {
                if (rects_overlap(&r.rect, &protect[j])) {
                    r.status = CALIB_REGION_PROTECTED;
                    break;
                }
            }
        }
        found++;
        keep_top(out, &written, max_out, &r);
    }

    if (total_found) *total_found = found;
    return written;
}

const char *calib_region_status_name(CalibRegionStatus status) {
    switch (status) {
    case CALIB_REGION_OK:        return "ok";
    case CALIB_REGION_TOO_LARGE: return "too_large";
    case CALIB_REGION_PROTECTED: return "protected";
    }
    return "unknown";
}
