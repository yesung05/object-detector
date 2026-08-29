#include "tracks.h"

#include <stdlib.h>
#include <string.h>

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

int tracks_init(TrackList *tl, size_t capacity, float iou_threshold,
                int max_misses, char *error, size_t error_size) {
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
    tl->items         = items;
    tl->count         = 0;
    tl->capacity      = capacity;
    tl->next_id       = 1;
    tl->iou_threshold = iou_threshold;
    tl->max_misses    = max_misses;
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
 * 그리디 IoU 매칭 (O(M×N)):
 * 1. 각 기존 트랙에 대해 IoU가 가장 높은 detection을 찾습니다.
 * 2. iou_threshold 이상이면 매칭 → box, last_seen, dwell 갱신, misses=0.
 * 3. 매칭 없는 트랙: misses++, max_misses 초과 시 active=0.
 * 4. 매칭 없는 detection: 용량이 남으면 새 트랙 생성.
 */
void tracks_update(TrackList *tl, const DetectionList *detections, double now) {
    size_t i, j;
    /* 어떤 detection 이 이미 매칭됐는지 기록합니다. */
    int matched_det[1024];
    size_t det_count;

    if (!tl || !detections) return;
    det_count = detections->count;
    if (det_count > 1024) det_count = 1024;
    memset(matched_det, 0, det_count * sizeof(matched_det[0]));

    /* 기존 트랙을 매칭합니다. */
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
            t->box          = detections->items[best_j];
            t->dwell_seconds += now - t->last_seen;
            t->last_seen    = now;
            t->misses       = 0;
        } else {
            t->misses++;
            if (t->misses > tl->max_misses) t->active = 0;
        }
    }

    /* 매칭되지 않은 detection → 새 트랙 생성 */
    for (j = 0; j < det_count; ++j) {
        Track *slot = NULL;
        size_t k;
        if (matched_det[j]) continue;
        /* 비활성 슬롯 재사용 */
        for (k = 0; k < tl->count; ++k) {
            if (!tl->items[k].active) { slot = &tl->items[k]; break; }
        }
        /* 슬롯이 없으면 용량 끝에 추가 */
        if (!slot && tl->count < tl->capacity) {
            slot = &tl->items[tl->count++];
        }
        if (!slot) continue;  /* 용량 초과 — 새 트랙 무시 */
        memset(slot, 0, sizeof(*slot));
        slot->id          = tl->next_id++;
        slot->active      = 1;
        slot->first_seen  = now;
        slot->last_seen   = now;
        slot->dwell_seconds = 0.0;
        slot->order       = TRACK_UNORDERED;
        slot->box         = detections->items[j];
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
