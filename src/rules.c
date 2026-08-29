#include "rules.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const RulesConfig DEFAULT_RULES = {3600.0, 300.0, 5.0, 0, 0, 0, 0, 0};

int rules_init(RulesEngine *re, size_t capacity, const RulesConfig *config,
               char *error, size_t error_size) {
    TrackRuleState *states;
    size_t i;
    if (!re || capacity == 0) {
        if (error) snprintf(error, error_size, "rules_init: invalid args");
        return -1;
    }
    states = (TrackRuleState *)calloc(capacity, sizeof(TrackRuleState));
    if (!states) {
        if (error) snprintf(error, error_size, "rules_init: out of memory");
        return -1;
    }
    for (i = 0; i < capacity; ++i) states[i].track_id = -1;
    re->states   = states;
    re->capacity = capacity;
    re->config   = config ? *config : DEFAULT_RULES;
    return 0;
}

void rules_destroy(RulesEngine *re) {
    if (!re) return;
    free(re->states);
    re->states   = NULL;
    re->capacity = 0;
}

/* track_id 에 해당하는 슬롯을 반환합니다. 없으면 빈 슬롯에 할당합니다. */
static TrackRuleState *get_state(RulesEngine *re, int track_id) {
    size_t idx = (size_t)(track_id < 0 ? 0 : track_id) % re->capacity;
    size_t i;
    /* 해당 슬롯이 같은 ID면 반환 */
    if (re->states[idx].track_id == track_id) return &re->states[idx];
    /* 선형 탐색 (capacity가 작아 빠름) */
    for (i = 0; i < re->capacity; ++i) {
        if (re->states[i].track_id == track_id) return &re->states[i];
    }
    /* 빈 슬롯에 할당 */
    for (i = 0; i < re->capacity; ++i) {
        if (re->states[i].track_id == -1) {
            memset(&re->states[i], 0, sizeof(re->states[i]));
            re->states[i].track_id = track_id;
            return &re->states[i];
        }
    }
    /* 용량 초과: idx 슬롯 강제 재사용 */
    memset(&re->states[idx], 0, sizeof(re->states[idx]));
    re->states[idx].track_id = track_id;
    return &re->states[idx];
}

/* 비활성 트랙의 latch 슬롯을 해제합니다. */
static void release_state(RulesEngine *re, int track_id) {
    size_t i;
    for (i = 0; i < re->capacity; ++i) {
        if (re->states[i].track_id == track_id) {
            re->states[i].track_id = -1;
            return;
        }
    }
}

/*
 * 쓰러짐 판정:
 * - 머리(0), 왼어깨(5), 오른어깨(6), 왼엉덩이(11), 오른엉덩이(12) 중
 *   score >= 0.4 인 keypoint의 y 좌표 표준편차를 bbox 높이로 나눕니다.
 *   이 비율이 0.25 이하이면서 bbox 가로 > 세로*1.2 이면 "수평 자세"입니다.
 * - keypoint가 없으면 bbox 가로/세로 비율만으로 판정합니다.
 */
#define KP_SCORE_THRESH 0.4f
static const int FALL_KP_IDX[] = {0, 5, 6, 11, 12};
static const int FALL_KP_COUNT = 5;

static int is_horizontal_pose(const Detection *box) {
    float w = box->x2 - box->x1;
    float h = box->y2 - box->y1;
    int i, valid = 0;
    float y_sum = 0.0f, y_sq = 0.0f, y_mean, variance, std_ratio;

    if (h <= 0.0f) return 0;
    if (w <= h * 1.2f) return 0;  /* bbox가 세로 우세 → 서 있음 */

    if (box->keypoint_count < YOLO11_NUM_KEYPOINTS)
        return 1;  /* keypoint 없으면 bbox 비율만으로 판정 */

    for (i = 0; i < FALL_KP_COUNT; ++i) {
        const Keypoint *kp = &box->kp[FALL_KP_IDX[i]];
        if (kp->score >= KP_SCORE_THRESH) {
            y_sum += kp->y;
            y_sq  += kp->y * kp->y;
            valid++;
        }
    }
    if (valid < 2) return 1;  /* keypoint 부족 → bbox 비율로 대체 */

    y_mean   = y_sum / (float)valid;
    variance = y_sq / (float)valid - y_mean * y_mean;
    if (variance < 0.0f) variance = 0.0f;
    std_ratio = (float)sqrt((double)variance) / h;
    return std_ratio <= 0.25f;
}

void rules_evaluate(RulesEngine *re, TrackList *tl, double now, EventLog *elog) {
    size_t i;
    char msg[256];

    if (!re || !tl) return;

    for (i = 0; i < tl->count; ++i) {
        Track *t = &tl->items[i];
        TrackRuleState *s;

        if (!t->active) {
            release_state(re, t->id);
            continue;
        }

        s = get_state(re, t->id);

        /* ROI 키오스크 체크: 박스 중심이 ROI 안에 있으면 ORDERED 로 전환 */
        if (re->config.roi_kiosk_set && t->order == TRACK_UNORDERED) {
            float cx = (t->box.x1 + t->box.x2) * 0.5f;
            float cy = (t->box.y1 + t->box.y2) * 0.5f;
            if (cx >= re->config.roi_kiosk_x &&
                cx <= re->config.roi_kiosk_x + re->config.roi_kiosk_w &&
                cy >= re->config.roi_kiosk_y &&
                cy <= re->config.roi_kiosk_y + re->config.roi_kiosk_h) {
                t->order = TRACK_ORDERED;
                snprintf(msg, sizeof(msg), "track=%d roi=kiosk", t->id);
                event_log_write(elog, LOG_INFO, "rules", msg);
            }
        }

        /* ── 초과 체류 ── */
        if (t->dwell_seconds > re->config.dwell_limit_seconds) {
            if (!s->overstay_latched) {
                s->overstay_latched = 1;
                snprintf(msg, sizeof(msg),
                         "overstay track=%d dwell=%.0fs limit=%.0fs",
                         t->id, t->dwell_seconds,
                         re->config.dwell_limit_seconds);
                event_log_write(elog, LOG_WARN, "rules", msg);
            }
        } else {
            s->overstay_latched = 0;
        }

        /* ── 미주문 착석 ── */
        if (t->order == TRACK_UNORDERED &&
            t->dwell_seconds > re->config.unordered_grace_seconds) {
            if (!s->unordered_latched) {
                s->unordered_latched = 1;
                snprintf(msg, sizeof(msg),
                         "unordered_seated track=%d dwell=%.0fs grace=%.0fs",
                         t->id, t->dwell_seconds,
                         re->config.unordered_grace_seconds);
                event_log_write(elog, LOG_WARN, "rules", msg);
            }
        } else if (t->order == TRACK_ORDERED) {
            s->unordered_latched = 0;
        }

        /* ── 쓰러짐 ── */
        if (is_horizontal_pose(&t->box)) {
            if (s->fall_start <= 0.0) s->fall_start = now;
            if (!s->fall_latched &&
                (now - s->fall_start) >= re->config.fall_hold_seconds) {
                s->fall_latched = 1;
                snprintf(msg, sizeof(msg),
                         "person_fallen track=%d hold=%.1fs",
                         t->id, now - s->fall_start);
                event_log_write(elog, LOG_ERROR, "rules", msg);
            }
        } else {
            s->fall_start  = 0.0;
            s->fall_latched = 0;
        }
    }
}
