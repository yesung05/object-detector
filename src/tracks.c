#include "tracks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 내부 유틸리티 ─────────────────────────────────────────────────── */

/* 두 박스의 IoU (Intersection over Union)를 계산합니다. */
static float iou(const Detection *a, const Detection *b) {
    float ix1 = a->x1 > b->x1 ? a->x1 : b->x1;
    float iy1 = a->y1 > b->y1 ? a->y1 : b->y1;
    float ix2 = a->x2 < b->x2 ? a->x2 : b->x2;
    float iy2 = a->y2 < b->y2 ? a->y2 : b->y2;
    float inter_w = ix2 - ix1;
    float inter_h = iy2 - iy1;
    float inter_area, area_a, area_b, union_area;
    if (inter_w <= 0.0f || inter_h <= 0.0f) return 0.0f;
    inter_area = inter_w * inter_h;
    area_a = (a->x2 - a->x1) * (a->y2 - a->y1);
    area_b = (b->x2 - b->x1) * (b->y2 - b->y1);
    union_area = area_a + area_b - inter_area;
    if (union_area <= 0.0f) return 0.0f;
    return inter_area / union_area;
}

/*
 * bbox 상체(상단 60%) 영역에서 H·S 채널 히스토그램을 추출합니다.
 * V(명도)는 조명 변화에 취약해 제외합니다.
 *
 * hist[0..15]  : H 빈 (22.5° 간격, 0~360°)
 * hist[16..31] : S 빈 (16단계, 0~1)
 * 각 절반의 합이 1이 되도록 픽셀 수로 정규화합니다.
 */
static void compute_appear_hist(
    const uint8_t *rgb, int img_w, int img_h, int img_stride,
    const Detection *det, float *hist)
{
    int x1 = (int)det->x1;
    int y1 = (int)det->y1;
    int x2 = (int)det->x2;
    /* 다리·바닥은 의류 식별에 노이즈가 되므로 상단 60%만 씁니다. */
    int y2 = (int)(det->y1 + (det->y2 - det->y1) * 0.6f);
    int x, y, count = 0;

    memset(hist, 0, APPEAR_HIST_BINS * sizeof(float));

    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > img_w) x2 = img_w;
    if (y2 > img_h) y2 = img_h;
    if (x1 >= x2 || y1 >= y2) return;

    for (y = y1; y < y2; ++y) {
        const uint8_t *row = rgb + (size_t)y * img_stride;
        for (x = x1; x < x2; ++x) {
            float r = row[x * 3 + 0] * (1.0f / 255.0f);
            float g = row[x * 3 + 1] * (1.0f / 255.0f);
            float b = row[x * 3 + 2] * (1.0f / 255.0f);
            float max_c = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float min_c = r < g ? (r < b ? r : b) : (g < b ? g : b);
            float delta = max_c - min_c;
            float h = 0.0f;
            float s = (max_c > 1e-6f) ? (delta / max_c) : 0.0f;
            int h_bin, s_bin;

            if (delta > 1e-6f) {
                float inv_d = 1.0f / delta;
                if (max_c == r)      h = 60.0f * ((g - b) * inv_d);
                else if (max_c == g) h = 60.0f * ((b - r) * inv_d + 2.0f);
                else                 h = 60.0f * ((r - g) * inv_d + 4.0f);
                if (h < 0.0f) h += 360.0f;
                if (h >= 360.0f) h -= 360.0f;
            }

            h_bin = (int)(h * (16.0f / 360.0f));
            s_bin = (int)(s * 16.0f);
            if (h_bin > 15) h_bin = 15;
            if (s_bin > 15) s_bin = 15;

            hist[h_bin]      += 1.0f;
            hist[16 + s_bin] += 1.0f;
            count++;
        }
    }

    /* H 절반과 S 절반을 각각 정규화합니다 (각 절반의 합 = 1). */
    if (count > 0) {
        float inv = 1.0f / (float)count;
        int k;
        for (k = 0; k < APPEAR_HIST_BINS; ++k)
            hist[k] *= inv;
    }
}

/*
 * 히스토그램 교차값 (Histogram Intersection) 유사도를 반환합니다.
 * H 절반·S 절반 각각의 교차값을 구해 평균하므로 반환값은 [0, 1]입니다.
 * 1에 가까울수록 외관이 유사합니다.
 */
static float hist_similarity(const float *a, const float *b) {
    float h_sim = 0.0f, s_sim = 0.0f;
    int i;
    for (i = 0;  i < 16; ++i) h_sim += a[i] < b[i] ? a[i] : b[i];
    for (i = 16; i < 32; ++i) s_sim += a[i] < b[i] ? a[i] : b[i];
    return (h_sim + s_sim) * 0.5f;
}

/* ── 공개 API ─────────────────────────────────────────────────────── */

int tracks_init(TrackList *tl, size_t capacity, float iou_threshold,
                int max_misses, double limbo_seconds, float appear_threshold,
                char *error, size_t error_size) {
    Track *items;
    if (!tl || capacity == 0) {
        if (error) snprintf(error, error_size, "tracks_init: invalid args");
        return -1;
    }
    items = (Track *)calloc(capacity, sizeof(Track));
    if (!items) {
        if (error) snprintf(error, error_size, "tracks_init: out of memory");
        return -1;
    }
    tl->items            = items;
    tl->count            = 0;
    tl->capacity         = capacity;
    tl->next_id          = 1;
    tl->iou_threshold    = iou_threshold;
    tl->max_misses       = max_misses;
    tl->limbo_seconds    = limbo_seconds;
    tl->appear_threshold = appear_threshold;
    return 0;
}

void tracks_destroy(TrackList *tl) {
    if (!tl) return;
    free(tl->items);
    tl->items    = NULL;
    tl->count    = 0;
    tl->capacity = 0;
}

/*
 * Phase 1 : 활성 트랙 ↔ detection IoU 그리디 매칭
 * Phase 2 : 미매칭 detection → limbo 트랙과 HSV 히스토그램 비교 (Re-ID)
 * Phase 3 : 여전히 미매칭 detection → 신규 트랙 생성
 *
 * 트랙 수명:
 *   active=1, misses=0        : 정상 추적 (녹색)
 *   active=1, misses=1..max   : 단기 미감지 (주황)
 *   active=0, now<limbo_exp   : limbo — 보이지 않지만 Re-ID 대기 중
 *   active=0, now>=limbo_exp  : 만료 — 슬롯 재사용 가능
 *
 * 체류 시간 정책:
 *   IoU 매칭 성공 시: dwell += now - last_seen (연속 체류 누적)
 *   Re-ID 부활 시  : dwell 그대로 유지 (limbo 기간은 제외)
 */
void tracks_update(TrackList *tl, const DetectionList *detections,
                   const uint8_t *rgb, int img_w, int img_h, int img_stride,
                   double now) {
    size_t i, j, k;
    int    matched_det[1024];
    size_t det_count;

    if (!tl || !detections) return;
    det_count = detections->count;
    if (det_count > 1024) det_count = 1024;
    memset(matched_det, 0, det_count * sizeof(matched_det[0]));

    /* ── Phase 1: 활성 트랙 IoU 매칭 ──────────────────────────────── */
    for (i = 0; i < tl->count; ++i) {
        Track *t = &tl->items[i];
        float best_iou = 0.0f;
        int   best_j   = -1;
        if (!t->active) continue;
        for (j = 0; j < det_count; ++j) {
            if (matched_det[j]) continue;
            {
                float v = iou(&t->box, &detections->items[j]);
                if (v > best_iou) { best_iou = v; best_j = (int)j; }
            }
        }
        if (best_j >= 0 && best_iou >= tl->iou_threshold) {
            matched_det[best_j] = 1;
            t->dwell_seconds   += now - t->last_seen;
            t->last_seen        = now;
            t->box              = detections->items[best_j];
            t->misses           = 0;
            /* 외관 히스토그램을 EMA로 갱신합니다 (α=0.15).
             * 빠른 갱신은 잘못된 수렴을 일으키므로 느린 α를 씁니다. */
            if (rgb) {
                float new_hist[APPEAR_HIST_BINS];
                compute_appear_hist(rgb, img_w, img_h, img_stride,
                                    &t->box, new_hist);
                if (!t->appear_valid) {
                    memcpy(t->appear_hist, new_hist, sizeof(new_hist));
                    t->appear_valid = 1;
                } else {
                    int kk;
                    for (kk = 0; kk < APPEAR_HIST_BINS; ++kk)
                        t->appear_hist[kk] = 0.85f * t->appear_hist[kk]
                                           + 0.15f * new_hist[kk];
                }
            }
        } else {
            t->misses++;
            if (t->misses > tl->max_misses) {
                t->active           = 0;
                /* limbo 시작. 이 기간 동안 Re-ID로 부활할 수 있습니다. */
                t->limbo_expired_at = now + tl->limbo_seconds;
            }
        }
    }

    /* ── Phase 2 + 3: 미매칭 detection 처리 ──────────────────────── */
    for (j = 0; j < det_count; ++j) {
        float  cur_hist[APPEAR_HIST_BINS];
        int    hist_computed = 0;
        Track *best_limbo    = NULL;
        float  best_sim      = 0.0f;

        if (matched_det[j]) continue;

        /* Re-ID: limbo 트랙과 히스토그램 비교 */
        if (rgb) {
            compute_appear_hist(rgb, img_w, img_h, img_stride,
                                &detections->items[j], cur_hist);
            hist_computed = 1;

            for (k = 0; k < tl->count; ++k) {
                Track *t = &tl->items[k];
                float  sim;
                /* active 트랙은 Phase 1에서 이미 처리됐습니다. */
                if (t->active) continue;
                /* limbo 기간이 끝났거나 히스토그램이 없으면 건너뜁니다. */
                if (t->limbo_expired_at <= now) continue;
                if (!t->appear_valid) continue;

                sim = hist_similarity(t->appear_hist, cur_hist);
                if (sim > best_sim) { best_sim = sim; best_limbo = t; }
            }
        }

        if (best_limbo && best_sim >= tl->appear_threshold) {
            /* 동일 인물 — 트랙 부활.
             * limbo 기간은 체류 시간에 포함하지 않으므로 dwell은 갱신하지 않습니다.
             * 다음 IoU 매칭 때부터 now - last_seen이 정상 간격으로 누적됩니다. */
            best_limbo->active           = 1;
            best_limbo->misses           = 0;
            best_limbo->limbo_expired_at = 0.0;
            best_limbo->box              = detections->items[j];
            best_limbo->last_seen        = now;
            {
                int kk;
                for (kk = 0; kk < APPEAR_HIST_BINS; ++kk)
                    best_limbo->appear_hist[kk] = 0.85f * best_limbo->appear_hist[kk]
                                                + 0.15f * cur_hist[kk];
            }
        } else {
            /* 신규 인물 — 슬롯을 확보합니다. 우선순위:
             *   1) 만료된 limbo 슬롯 (limbo_expired_at 지남)
             *   2) 배열 끝 빈 공간
             *   3) limbo 중 가장 오래 전에 떠난 트랙 강제 퇴출
             *      (최근에 떠난 사람이 돌아올 확률이 더 높으므로 오래된 쪽 제거) */
            Track *slot = NULL;
            for (k = 0; k < tl->count; ++k) {
                if (!tl->items[k].active &&
                    tl->items[k].limbo_expired_at <= now) {
                    slot = &tl->items[k];
                    break;
                }
            }
            if (!slot && tl->count < tl->capacity)
                slot = &tl->items[tl->count++];
            if (!slot) {
                /* 모든 슬롯이 limbo — 가장 오래 전에 떠난 트랙을 퇴출합니다. */
                Track *oldest = NULL;
                for (k = 0; k < tl->count; ++k) {
                    Track *t = &tl->items[k];
                    if (!t->active && t->limbo_expired_at > now) {
                        if (!oldest || t->last_seen < oldest->last_seen)
                            oldest = t;
                    }
                }
                if (oldest) {
                    fprintf(stderr,
                            "tracks: limbo full — evicting track #%d"
                            " (last_seen %.0fs ago)\n",
                            oldest->id, now - oldest->last_seen);
                    slot = oldest;
                }
            }
            if (!slot) continue;  /* active 트랙만 가득 찬 경우 — 무시 */

            memset(slot, 0, sizeof(*slot));
            slot->id          = tl->next_id++;
            slot->active      = 1;
            slot->first_seen  = now;
            slot->last_seen   = now;
            slot->order       = TRACK_UNORDERED;
            slot->box         = detections->items[j];
            if (hist_computed) {
                memcpy(slot->appear_hist, cur_hist, sizeof(slot->appear_hist));
                slot->appear_valid = 1;
            }
        }
    }
}

void tracks_mark_ordered(TrackList *tl, int track_id) {
    size_t i;
    if (!tl) return;
    for (i = 0; i < tl->count; ++i) {
        if (tl->items[i].active && tl->items[i].id == track_id)
            tl->items[i].order = TRACK_ORDERED;
    }
}

int tracks_limbo_count(const TrackList *tl, double now) {
    size_t i;
    int n = 0;
    if (!tl) return 0;
    for (i = 0; i < tl->count; ++i) {
        const Track *t = &tl->items[i];
        if (!t->active && t->limbo_expired_at > now) n++;
    }
    return n;
}
