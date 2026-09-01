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

void rules_update_config(RulesEngine *re, const RulesConfig *config) {
    if (!re || !config) return;
    /* latch 상태(overstay_latched 등)는 건드리지 않고 임계값만 교체합니다.
     * 트랙이 이미 발화 중이어도 다음 evaluate 주기부터 새 값이 적용됩니다. */
    re->config = *config;
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
 * 쓰러짐 판정 (개선 버전):
 *
 * keypoint 있는 경우 (pose 모델):
 *   - bbox 가로 > 세로 × 1.8 (기존 1.2보다 엄격)
 *   - 신뢰 관절(score≥0.4) 3개 이상 + 엉덩이(11·12) 최소 1개 유효해야 std 계산
 *   - 머리·어깨·엉덩이 y 표준편차 / bbox높이 ≤ 0.20 (기존 0.25보다 엄격)
 *
 * keypoint 없는 경우 (detection 전용 모델, 유효 관절 2개 미만, 또는 엉덩이 없음):
 *   - bbox 가로 > 세로 × 2.2 로 훨씬 엄격하게 적용
 *   - 앉아서 팔 벌리거나 숙인 자세(1.2x 수준)는 오감지하지 않음
 *   - 엉덩이 없이 코+어깨만으로 y-std 계산 시 해부학적으로 항상 작은 값이
 *     나와 조건을 거의 항상 통과하므로 이 경로도 폴백으로 처리한다.
 *
 * 공통 가드:
 *   - bbox 세로 < 30px → 너무 작아서 노이즈, 무시
 *   - detection score < 0.30 → 저신뢰 검출, 무시
 */
#define KP_SCORE_THRESH  0.4f
#define FALL_STD_THRESH  0.20f   /* 관절 y 분산 비율 상한 */
#define FALL_RATIO_KP    1.8f    /* keypoint 있을 때 bbox 가로/세로 기준 */
#define FALL_RATIO_NOKP  2.2f    /* keypoint 없을 때 bbox 가로/세로 기준 */
#define FALL_MIN_H       30.0f   /* bbox 최소 세로 (px), 이하는 노이즈 */
#define FALL_MIN_SCORE   0.30f   /* detection 신뢰도 하한 */
static const int FALL_KP_IDX[] = {0, 5, 6, 11, 12};
static const int FALL_KP_COUNT = 5;

static int is_horizontal_pose(const Detection *box) {
    float w = box->x2 - box->x1;
    float h = box->y2 - box->y1;
    int i, valid = 0;
    float y_sum = 0.0f, y_sq = 0.0f, y_mean, variance, std_ratio;

    if (h < FALL_MIN_H)       return 0;  /* 너무 작은 검출은 노이즈 */
    if (box->score < FALL_MIN_SCORE) return 0;  /* 저신뢰 검출 제외 */

    /* keypoint 없거나 부족하면 더 엄격한 bbox 비율 기준 적용 */
    if (box->keypoint_count < YOLO11_NUM_KEYPOINTS) {
        return w > h * FALL_RATIO_NOKP;
    }

    for (i = 0; i < FALL_KP_COUNT; ++i) {
        const Keypoint *kp = &box->kp[FALL_KP_IDX[i]];
        if (kp->score >= KP_SCORE_THRESH) {
            y_sum += kp->y;
            y_sq  += kp->y * kp->y;
            valid++;
        }
    }

    /* 엉덩이(11·12) 중 유효한 관절 수 */
    int hip_valid = (box->kp[11].score >= KP_SCORE_THRESH) +
                    (box->kp[12].score >= KP_SCORE_THRESH);
    /* 유효 관절 3개 미만이거나 엉덩이가 하나도 없으면 엄격한 기준 적용.
     * 엉덩이 없이 코+어깨만으로 y-std를 계산하면 해부학적으로 항상 작은
     * 값이 나와 조건을 거의 항상 통과하기 때문이다. */
    if (valid < 3 || hip_valid == 0) {
        return w > h * FALL_RATIO_NOKP;
    }

    if (w <= h * FALL_RATIO_KP) return 0;  /* bbox 비율 조건 미충족 */

    y_mean   = y_sum / (float)valid;
    variance = y_sq / (float)valid - y_mean * y_mean;
    if (variance < 0.0f) variance = 0.0f;
    std_ratio = (float)sqrt((double)variance) / h;
    return std_ratio <= FALL_STD_THRESH;
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
