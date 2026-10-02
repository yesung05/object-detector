#include "rules.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Tier 2 클래스 분류 헬퍼 ── */
static int is_animal(int id)   { return id == OBJ_CAT || id == OBJ_DOG; }
static int is_food(int id)     { return id >= OBJ_FOOD_FIRST && id <= OBJ_FOOD_LAST; }

/*
 * 두 Detection bbox 간의 IoU를 계산합니다.
 * rules.c 내부에서만 사용하는 별도 구현으로, postprocess.c의
 * intersection_over_union()와 독립적입니다.
 */
static float obj_iou(const Detection *a, const Detection *b) {
    float left   = a->x1 > b->x1 ? a->x1 : b->x1;
    float top    = a->y1 > b->y1 ? a->y1 : b->y1;
    float right  = a->x2 < b->x2 ? a->x2 : b->x2;
    float bottom = a->y2 < b->y2 ? a->y2 : b->y2;
    float inter_w = right - left;
    float inter_h = bottom - top;
    float inter, area_a, area_b, denom;
    if (inter_w <= 0.0f || inter_h <= 0.0f) return 0.0f;
    inter  = inter_w * inter_h;
    area_a = (a->x2 - a->x1) * (a->y2 - a->y1);
    area_b = (b->x2 - b->x1) * (b->y2 - b->y1);
    denom  = area_a + area_b - inter;
    return denom > 0.0f ? inter / denom : 0.0f;
}

static const RulesConfig DEFAULT_RULES = {
    3600.0,  /* dwell_limit_seconds */
    300.0,   /* unordered_grace_seconds */
    5.0,     /* fall_hold_seconds */
    /* 사선 화각(30-45°)에서는 누운 사람의 bbox 비율이 정수직 화각보다 낮음.
       실험값: kp 경로 1.3, nokp 경로 1.6. 대시보드 설정으로 현장 조정 가능 */
    1.3f,    /* fall_aspect_ratio_kp */
    1.6f,    /* fall_aspect_ratio_nokp */
    0, 0, 0, 0, /* roi_kiosk_{x,y,w,h} */
    0,       /* roi_kiosk_set */
    0.15f,   /* animal_iou_threshold */
    1,       /* no_cup_margin */
    1,       /* vanish_enabled */
    5.0,     /* vanish_hold_seconds */
    3.0,     /* vanish_min_dwell_seconds */
    0.35f,   /* vanish_min_score */
    0.08f,   /* vanish_edge_margin */
    0,       /* vanish_require_residue */
    0,       /* roi_anchor_foot */
    3,       /* object_confirm_count */
    5,       /* object_confirm_window */
    0,       /* max_occupancy (비활성) */
    10.0,    /* max_occupancy_hold_seconds */
    300.0,   /* still_seconds */
    0.10f,   /* still_motion_threshold */
};

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
    memset(re,0,sizeof(*re));
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

void rules_set_frame_size(RulesEngine *re, int width, int height) {
    if (!re) return;
    re->frame_width  = width;
    re->frame_height = height;
}

void rules_update_config(RulesEngine *re, const RulesConfig *config) {
    if (!re || !config) return;
    /* latch 상태(overstay_latched 등)는 건드리지 않고 임계값만 교체합니다.
     * 트랙이 이미 발화 중이어도 다음 evaluate 주기부터 새 값이 적용됩니다. */
    re->config = *config;
}

void rules_shift_time(RulesEngine *re, double gap) {
    size_t i;
    if (!re || !re->states || gap <= 0.0) return;
    re->occ_start = 0; /* 공백 동안의 인원은 알 수 없으므로 초과 지속 시간을 이어 세지 않습니다 */
    for (i = 0; i < re->capacity; ++i) {
        re->states[i].order_near_start=re->states[i].order_near_last=0;
        re->states[i].still_start=0; /* 화면을 못 본 시간은 "움직임 없음"의 증거가 아닙니다 */
        /* A camera gap is missing evidence, not observed posture duration. */
        re->states[i].fall_start=0;re->states[i].fall_samples=0;
        re->states[i].upright_start=0;re->states[i].upright_samples=0;
        if(re->states[i].upright_last>0)re->states[i].upright_last+=gap;
        if(re->states[i].fall_last_observation>0)re->states[i].fall_last_observation+=gap;
    }
}

/* 발화한 쓰러짐을 캡처 대기 목록에 올립니다. 넘치면 캡처만 생략합니다(로그는 이미 남음). */
static void note_fall(RulesEngine *re, const Track *t) {
    if (re->fall_fired_count >= RULES_MAX_FIRED) return;
    re->fall_fired_track[re->fall_fired_count] = t->id;
    re->fall_fired_box[re->fall_fired_count]   = t->box;
    re->fall_fired_count++;
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
#define FALL_EDGE_PX      2.0f   /* 이 거리 안에서 화면 경계에 닿으면 박스가 잘린 것으로 봄 */
/* 코가 어깨보다 어깨너비의 이 배수만큼 위에 있으면 상체가 서 있다고 봅니다.
 * 똑바로 앉거나 선 자세는 0.5~0.8 수준, 누우면 0 근처가 됩니다. */
#define FALL_UPRIGHT_DROP 0.35f
static const int FALL_KP_IDX[] = {0, 5, 6, 11, 12};
static const int FALL_KP_COUNT = 5;

/*
 * 코가 어깨보다 충분히 위에 있으면 상체가 서 있는 것입니다. 누우면 둘의 y 가 비슷해집니다.
 * 어깨 너비로 정규화해 카메라와의 거리·해상도에 무관하게 만듭니다.
 *
 * 반환 1=상체 서 있음, 0=상체가 수평에 가까움, -1=관절이 부족해 판단 불가.
 */
static int upper_body_upright(const Detection *b) {
    const Keypoint *nose = &b->kp[0], *ls = &b->kp[5], *rs = &b->kp[6];
    float shoulder_y, shoulder_w, drop;
    if (nose->score < KP_SCORE_THRESH ||
        ls->score   < KP_SCORE_THRESH ||
        rs->score   < KP_SCORE_THRESH) return -1;
    shoulder_y = (ls->y + rs->y) * 0.5f;
    shoulder_w = ls->x > rs->x ? ls->x - rs->x : rs->x - ls->x;
    if (shoulder_w < 1.0f) return -1;          /* 정면 아닌 각도 — 너비로 정규화 불가 */
    drop = (shoulder_y - nose->y) / shoulder_w;
    return drop > FALL_UPRIGHT_DROP;
}

/* 테스트에서 직접 부를 수 있도록 노출합니다 — 쓰러짐 오탐은 영상 없이 재현·검증되어야 합니다. */
int is_horizontal_pose_for_test(const Detection *box, float ratio_kp, float ratio_nokp,
                                int frame_w, int frame_h);

static int is_horizontal_pose(const Detection *box,
                               float ratio_kp, float ratio_nokp,
                               int frame_w, int frame_h) {
    float w = box->x2 - box->x1;
    float h = box->y2 - box->y1;
    int i, valid = 0;
    float y_sum = 0.0f, y_sq = 0.0f, y_mean, variance, std_ratio;

    if (h < FALL_MIN_H)       return 0;  /* 너무 작은 검출은 노이즈 */
    if (box->score < FALL_MIN_SCORE) return 0;  /* 저신뢰 검출 제외 */

    /*
     * 박스가 화면 경계에 닿으면 실제 크기를 알 수 없습니다. 특히 아래가 잘리면 세로가
     * 짧게 측정되어 가로/세로 비가 부풀어 오릅니다.
     *
     * 실측 사례: 카메라 가까이 앉아 팔을 뻗은 사람의 박스가 1045x470(비율 2.22)으로
     * 잡혔습니다. 다리가 화면 밖이라 엉덩이 관절도 없어 nokp 폴백(임계 2.2)을 타고
     * 간발의 차로 "쓰러짐"이 발화했습니다. 스켈레톤은 명백히 앉은 자세였는데도요.
     *
     * 이때는 비율을 믿지 않고 상체 자세만으로 판단합니다. 판단이 안 되면 발화하지
     * 않습니다 — 응급 알림은 틀리는 순간 신뢰를 잃습니다.
     */
    if (frame_w > 0 && frame_h > 0 &&
        (box->x1 <= FALL_EDGE_PX || box->y1 <= FALL_EDGE_PX ||
         box->x2 >= (float)frame_w - FALL_EDGE_PX ||
         box->y2 >= (float)frame_h - FALL_EDGE_PX)) {
        return upper_body_upright(box) == 0;
    }

    /* keypoint 없거나 부족하면 더 엄격한 bbox 비율 기준 적용 */
    if (box->keypoint_count < YOLO11_NUM_KEYPOINTS) {
        return w > h * ratio_nokp;
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
        return w > h * ratio_nokp;
    }

    if (w <= h * ratio_kp) return 0;  /* bbox 비율 조건 미충족 */

    y_mean   = y_sum / (float)valid;
    variance = y_sq / (float)valid - y_mean * y_mean;
    if (variance < 0.0f) variance = 0.0f;
    std_ratio = (float)sqrt((double)variance) / h;
    return std_ratio <= FALL_STD_THRESH;
}

int is_horizontal_pose_for_test(const Detection *box, float ratio_kp, float ratio_nokp,
                                int frame_w, int frame_h) {
    return is_horizontal_pose(box, ratio_kp, ratio_nokp, frame_w, frame_h);
}

/*
 * 물체 감지 결과(DetectionList)와 현재 사람 트랙(TrackList)을 조합하여
 * Tier 2 룰을 평가합니다. Tier 1의 track_id 기반 latch와 달리
 * 물체 룰은 프레임 단위로 평가하며 RulesEngine 내 단일 래치 집합을 씁니다.
 *
 * 래치는 조건이 해소된 다음 호출 때 해제됩니다(0으로 초기화된 슬롯 재사용).
 * 물체 룰 전용 슬롯으로 track_id=-2 예약 슬롯을 사용합니다.
 */
/*
 * 관측 한 번을 기록하고 상태 전이를 알려 줍니다.
 * 반환: 1=이번에 확정(발화), -1=이번에 해제, 0=변화 없음.
 *
 * K-of-N 비트열을 쓴 이유: Tier 2 관측 간격이 길어(기본 90프레임) 연속 조건은 손에 가려진
 * 한 번의 누락만으로 리셋됩니다. 카운터 대신 비트열이면 "최근 N번 중 K번"을 popcount 하나로
 * 판정하고, 해제도 "N번 모두 안 보임"으로 같은 마스크에서 나와 확정·해제가 대칭입니다.
 */
static int obj_rule_observe(RulesEngine *re, ObjRuleState *r, int seen, double now) {
    int k = re->config.object_confirm_count;
    int n = re->config.object_confirm_window;
    unsigned mask, h;
    int hits = 0;

    if (k <= 1) { k = 1; n = 1; }                    /* 필터 없음 = 기존 동작 */
    else {
        if (n < k) n = k;
        if (n > 16) n = 16;
        if (k > n) k = n;
    }
    mask = (1u << n) - 1u;
    r->history = ((r->history << 1) | (seen ? 1u : 0u)) & 0xFFFFu;
    h = r->history & mask;
    while (h) { hits += (int)(h & 1u); h >>= 1; }

    if (!r->latched && hits >= k) {
        r->latched = 1;
        r->since = now;
        return 1;
    }
    if (r->latched && hits == 0) {
        r->latched = 0;
        return -1;
    }
    return 0;
}

/* 확정 시점에만 한 줄 남깁니다. hits 를 같이 적는 이유: 오탐 조사 때 "몇 번 중 몇 번 보였나"가
 * 임계값을 조정하는 유일한 근거입니다. */
static void obj_rule_log(RulesEngine *re, EventLog *elog, const char *name,
                         const char *detail, const ObjRuleState *r) {
    char msg[192];
    unsigned h = r->history;
    int hits = 0, n = re->config.object_confirm_window;
    if (re->config.object_confirm_count <= 1) n = 1;
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    h &= (1u << n) - 1u;
    while (h) { hits += (int)(h & 1u); h >>= 1; }
    snprintf(msg, sizeof(msg), "%s %s hits=%d/%d", name, detail, hits, n);
    event_log_write(elog, LOG_WARN, "rules", msg);
}

void rules_evaluate_objects(RulesEngine *re, const DetectionList *objs,
                             const TrackList *tl, double now, EventLog *elog) {
    size_t i, j;
    int found_bottle = 0, found_food = 0;
    int animal_on_chair = 0, animal_on_table = 0;
    int cup_count = 0, active_persons = 0;
    char detail[96];
    float iou_thresh;
    int margin;

    if (!re || !objs || !tl) return;

    iou_thresh = re->config.animal_iou_threshold > 0.0f
                 ? re->config.animal_iou_threshold : 0.15f;
    margin = re->config.no_cup_margin > 0 ? re->config.no_cup_margin : 1;

    /* 1패스: 물체 목록 분류 */
    for (i = 0; i < objs->count; ++i) {
        const Detection *obj = &objs->items[i];
        int id = obj->class_id;

        if (id == OBJ_BOTTLE)      { found_bottle = 1; }
        else if (is_food(id))      { found_food   = 1; }
        else if (id == OBJ_CUP)   { cup_count++; }

        /* 동물-가구 IoU 체크 */
        if (is_animal(id)) {
            for (j = 0; j < objs->count; ++j) {
                const Detection *furn = &objs->items[j];
                if (furn->class_id == OBJ_CHAIR &&
                    obj_iou(obj, furn) >= iou_thresh) {
                    animal_on_chair = 1;
                }
                if (furn->class_id == OBJ_DININGTABLE &&
                    obj_iou(obj, furn) >= iou_thresh) {
                    animal_on_table = 1;
                }
            }
        }
    }

    /* 활성 사람 수 집계 */
    for (i = 0; i < tl->count; ++i) {
        if (tl->items[i].active) active_persons++;
    }

    /* 룰마다 "이번 관측에서 조건이 성립했는가"만 넘기고 확정·해제는 한 함수가 맡습니다. */
    if (obj_rule_observe(re, &re->obj_rule[OBJ_RULE_DRINK], found_bottle, now) == 1) {
        snprintf(detail, sizeof(detail), "cups=%d", cup_count);
        obj_rule_log(re, elog, "external_drink", detail, &re->obj_rule[OBJ_RULE_DRINK]);
    }
    if (obj_rule_observe(re, &re->obj_rule[OBJ_RULE_FOOD], found_food, now) == 1)
        obj_rule_log(re, elog, "external_food", "", &re->obj_rule[OBJ_RULE_FOOD]);
    if (obj_rule_observe(re, &re->obj_rule[OBJ_RULE_ANIMAL_CHAIR], animal_on_chair, now) == 1) {
        snprintf(detail, sizeof(detail), "iou_thresh=%.2f", iou_thresh);
        obj_rule_log(re, elog, "animal_on_chair", detail, &re->obj_rule[OBJ_RULE_ANIMAL_CHAIR]);
    }
    if (obj_rule_observe(re, &re->obj_rule[OBJ_RULE_ANIMAL_TABLE], animal_on_table, now) == 1) {
        snprintf(detail, sizeof(detail), "iou_thresh=%.2f", iou_thresh);
        obj_rule_log(re, elog, "animal_on_table", detail, &re->obj_rule[OBJ_RULE_ANIMAL_TABLE]);
    }
    if (obj_rule_observe(re, &re->obj_rule[OBJ_RULE_NO_CUP],
                         active_persons > cup_count + margin, now) == 1) {
        snprintf(detail, sizeof(detail), "persons=%d cups=%d margin=%d",
                 active_persons, cup_count, margin);
        obj_rule_log(re, elog, "no_cup_seated", detail, &re->obj_rule[OBJ_RULE_NO_CUP]);
    }
}

int rules_check_vanish(RulesEngine *re, Track *t, const VanishEvidence *ev,
                       double now, EventLog *elog) {
    char msg[256];
    double gone;
    float cx, cy;

    if (!re || !t || !ev || !re->config.vanish_enabled) return 0;

    /* 다시 보이면 래치를 풀고 해소를 남깁니다. 재등장했는데 경고가 남아 있으면
     * 점주가 이미 지나간 상황을 계속 보게 됩니다. */
    if (t->active && t->misses == 0) {
        if (t->vanish_warned) {
            snprintf(msg, sizeof(msg), "person_unaccounted_cleared track=%d", t->id);
            event_log_write(elog, LOG_INFO, "rules", msg);
        }
        t->vanish_warned    = 0;
        t->vanish_escalated = 0;
        return 0;
    }

    gone = now - t->last_seen;
    if (gone < re->config.vanish_hold_seconds) return 0;

    /* 깜빡이던 트랙이나 원래 흐릿하던 탐지가 사라진 것은 정보가 아닙니다.
     * 여기를 막지 않으면 잡동사니를 사람으로 잘못 잡았다가 놓칠 때마다 경고가 납니다. */
    if (t->dwell_seconds < re->config.vanish_min_dwell_seconds) return 0;
    if (t->box.score < re->config.vanish_min_score) return 0;

    /*
     * 어디서 사라졌는지에 따라 판단이 완전히 달라집니다.
     *
     *   출입문 근처   문이 열렸으면 나간 것이고, 닫힌 채였으면 나갈 수 없었다는 강한 신호입니다.
     *   다른 가장자리 화각 밖으로 걸어 나갔을 수 있어 구분이 불가능합니다. 문이 닫혀 있어도
     *                 마찬가지입니다 — 카메라가 매장 전체를 덮지 못하면 문을 쓰지 않고도
     *                 시야에서 사라질 수 있습니다. 그 자리에 뭔가 남아 있을 때만 말합니다.
     *   화면 안쪽     나갈 곳이 없는데 사라졌습니다. 가장 강한 신호입니다.
     */
    if (ev->near_door) {
        if (ev->door_can_exit == 1) return 0;                  /* 문이 열렸다 — 정상 퇴장 */
        /* 문 상태를 모르면 나간 것인지 알 수 없으므로 잔류 흔적을 요구합니다. */
        if (ev->door_can_exit < 0 && ev->residue_at_spot != 1) return 0;
    } else if (ev->near_edge) {
        if (ev->residue_at_spot != 1) return 0;
    } else if (re->config.vanish_require_residue && ev->residue_at_spot != 1) {
        /* 칸막이·기둥이 많은 매장에서는 화면 안쪽에서도 사람이 정상적으로 자주 가려집니다.
         * 그런 배치에서는 이 옵션으로 잔류 흔적을 항상 요구하게 둡니다. */
        return 0;
    }

    cx = (t->box.x1 + t->box.x2) * 0.5f;
    cy = (t->box.y1 + t->box.y2) * 0.5f;

    if (!t->vanish_warned) {
        t->vanish_warned = 1;
        snprintf(msg, sizeof(msg),
                 "person_unaccounted track=%d last=%.0f,%.0f score=%.2f dwell=%.0fs "
                 "gone=%.0fs where=%s door=%s residue=%s",
                 t->id, cx, cy, t->box.score, t->dwell_seconds, gone,
                 ev->near_door ? "door" : ev->near_edge ? "frame_edge" : "interior",
                 ev->door_can_exit == 1 ? "opened" :
                 ev->door_can_exit == 0 ? "closed" : "unknown",
                 ev->residue_at_spot == 1 ? "yes" :
                 ev->residue_at_spot == 0 ? "no" : "unknown");
        event_log_write(elog, LOG_WARN, "rules", msg);
        return 1;
    }

    /* 그 자리에 움직이지 않는 것이 남아 있으면 사람일 가능성이 큽니다.
     * 칸막이 뒤로 이동한 경우는 그 바닥이 기준으로 돌아오므로 여기서 갈립니다.
     * 이 조건이 없으면 가구가 많은 매장에서 경고가 쏟아집니다. */
    if (!t->vanish_escalated && ev->residue_at_spot == 1) {
        t->vanish_escalated = 1;
        snprintf(msg, sizeof(msg),
                 "person_unaccounted_residue track=%d last=%.0f,%.0f gone=%.0fs "
                 "— 소실 지점에 움직이지 않는 것이 남아 있습니다",
                 t->id, cx, cy, gone);
        event_log_write(elog, LOG_ERROR, "rules", msg);
        return 2;
    }
    return 0;
}

/* A wide box or drifting head patch is not independent evidence of a person.
 * Require both shoulders/hips inside the current box and a non-degenerate torso.
 * 1 = upright torso; 2 = horizontal torso; 0 = insufficient evidence. */
static int fall_torso_pose(const Detection *b) {
    const int ids[4]={5,6,11,12};
    float w=b->x2-b->x1,h=b->y2-b->y1,sx,sy,hx,hy,dx,dy;
    int i;
    if(b->keypoint_count<13 || w<=0 || h<FALL_MIN_H)return 0;
    for(i=0;i<4;i++) {
        const Keypoint *k=&b->kp[ids[i]];
        if(!isfinite(k->x)||!isfinite(k->y)||!isfinite(k->score)||k->score<.5f ||
           k->x<b->x1-w*.1f || k->x>b->x2+w*.1f ||
           k->y<b->y1-h*.1f || k->y>b->y2+h*.1f)return 0;
    }
    sx=(b->kp[5].x+b->kp[6].x)*.5f;sy=(b->kp[5].y+b->kp[6].y)*.5f;
    hx=(b->kp[11].x+b->kp[12].x)*.5f;hy=(b->kp[11].y+b->kp[12].y)*.5f;
    dx=fabsf(hx-sx);dy=hy-sy;
    if(dy>h*.15f && dy>dx*1.2f)return 1;
    if(dx>w*.15f && fabsf(dy)<dx*.6f)return 2;
    return 0;
}

/*
 * 장시간 무동작 판정용 헬퍼.
 *
 * "선 자세" 기준을 무릎-엉덩이 y 차이로 둔 이유: pose==1(상체 직립)은 앉은 사람도 포함해서
 * 그것만으로는 독서하며 오래 앉아 있는 정상 손님과 구분되지 않습니다. 서 있으면 허벅지가
 * 수직이라 무릎이 엉덩이보다 확연히 아래에 찍히고, 앉으면 허벅지가 카메라 쪽으로 눕습니다.
 * 애매한 경우(관절 부족 등)는 대상에서 빼서 놓치는 쪽으로 틀립니다 — 이 룰의 오탐은
 * 점주 신뢰를 갉아먹지만 미탐은 쓰러짐·소실 룰이 이미 일부 메웁니다.
 */
#define STILL_STAND_DROP 0.22f  /* 무릎이 엉덩이보다 박스 높이의 이 비율 이상 아래 */

static int standing_legs(const Detection *b) {
    float h = b->y2 - b->y1, hip = 0.0f, knee = 0.0f;
    int hn = 0, kn = 0, k;
    if (b->keypoint_count < 15 || h < FALL_MIN_H) return 0;
    for (k = 11; k <= 12; ++k)
        if (b->kp[k].score >= KP_SCORE_THRESH) { hip  += b->kp[k].y; hn++; }
    for (k = 13; k <= 14; ++k)
        if (b->kp[k].score >= KP_SCORE_THRESH) { knee += b->kp[k].y; kn++; }
    if (!hn || !kn) return 0;
    return (knee / (float)kn - hip / (float)hn) > h * STILL_STAND_DROP;
}

/* [0]=박스 중심, [1..5]=FALL_KP_IDX 관절. 반환: 유효한 점의 비트마스크. */
static int still_points(const Detection *b, float *x, float *y) {
    int i, mask = 1;
    x[0] = (b->x1 + b->x2) * 0.5f;
    y[0] = (b->y1 + b->y2) * 0.5f;
    for (i = 0; i < FALL_KP_COUNT; ++i) {
        int idx = FALL_KP_IDX[i];
        if (idx < b->keypoint_count && b->kp[idx].score >= KP_SCORE_THRESH) {
            x[i + 1] = b->kp[idx].x;
            y[i + 1] = b->kp[idx].y;
            mask |= 1 << (i + 1);
        }
    }
    return mask;
}

static void still_reset(TrackRuleState *s) {
    s->still_start = 0.0;
    s->still_latched = 0;
    s->still_valid_mask = 0;
}

/* 새 관측이 들어올 때만 호출합니다 (rules_evaluate 의 fall 게이트 통과 후).
 * pose: fall_torso_pose 결과 1=상체 직립, 2=수평. */
static void still_update(RulesEngine *re, const Track *t, TrackRuleState *s,
                         int pose, double now, EventLog *elog) {
    const Detection *b = &t->box;
    float x[6] = {0}, y[6] = {0}, scale, thr;
    int mask, common, i, moved = 0;
    char msg[160];

    if (re->config.still_seconds <= 0.0) { still_reset(s); return; }
    if (!(pose == 2 || (pose == 1 && standing_legs(b)))) { still_reset(s); return; }

    /* 정규화 기준을 가로·세로 중 큰 쪽으로 둔 이유: 누운 사람은 박스 높이가 작아
     * 같은 떨림이 훨씬 큰 비율로 보입니다. */
    scale = (b->x2 - b->x1) > (b->y2 - b->y1) ? (b->x2 - b->x1) : (b->y2 - b->y1);
    if (scale < FALL_MIN_H) { still_reset(s); return; }
    thr = re->config.still_motion_threshold > 0.0f ? re->config.still_motion_threshold : 0.10f;

    mask = still_points(b, x, y);
    if (s->still_start <= 0.0) {
        memcpy(s->still_ax, x, sizeof(x));
        memcpy(s->still_ay, y, sizeof(y));
        s->still_valid_mask = mask;
        s->still_start = now;
        return;
    }

    common = mask & s->still_valid_mask;
    for (i = 0; i < 6; ++i) {
        float dx, dy;
        if (!(common & (1 << i))) continue;
        dx = x[i] - s->still_ax[i];
        dy = y[i] - s->still_ay[i];
        if ((float)sqrt((double)(dx * dx + dy * dy)) > scale * thr) { moved = 1; break; }
    }
    if (moved) {
        /* 움직였으면 지금 자세를 새 기준으로 삼고 처음부터 다시 셉니다. 이미 발화했다면 해제. */
        memcpy(s->still_ax, x, sizeof(x));
        memcpy(s->still_ay, y, sizeof(y));
        s->still_valid_mask = mask;
        s->still_start = now;
        s->still_latched = 0;
        return;
    }

    if (!s->still_latched && now - s->still_start >= re->config.still_seconds) {
        s->still_latched = 1;
        snprintf(msg, sizeof(msg),
                 "person_motionless track=%d still=%.0fs limit=%.0fs posture=%s motion_thr=%.2f",
                 t->id, now - s->still_start, re->config.still_seconds,
                 pose == 2 ? "lying" : "standing", thr);
        event_log_write(elog, LOG_WARN, "rules", msg);
    }
}

void rules_evaluate(RulesEngine *re, TrackList *tl, double now, EventLog *elog) {
    size_t i;
    char msg[256];

    if (!re || !tl) return;
    re->fall_fired_count = 0;

    /* ── 인원 초과 ── 활성 트랙 수만 셉니다(limbo 는 매장 안에 있다는 보장이 없음). */
    if (re->config.max_occupancy > 0) {
        int count = 0;
        double hold = re->config.max_occupancy_hold_seconds > 0.0
                      ? re->config.max_occupancy_hold_seconds : 10.0;
        for (i = 0; i < tl->count; ++i) if (tl->items[i].active) count++;
        if (count > re->config.max_occupancy) {
            if (re->occ_start <= 0.0) re->occ_start = now;
            if (!re->occ_latched && now - re->occ_start >= hold) {
                re->occ_latched = 1;
                snprintf(msg, sizeof(msg), "occupancy_exceeded count=%d limit=%d hold=%.0fs",
                         count, re->config.max_occupancy, now - re->occ_start);
                event_log_write(elog, LOG_WARN, "rules", msg);
            }
        } else {
            re->occ_start = 0.0;
            re->occ_latched = 0;
        }
    } else {
        re->occ_start = 0.0;
        re->occ_latched = 0;
    }

    for (i = 0; i < tl->count; ++i) {
        Track *t = &tl->items[i];
        TrackRuleState *s;

        if (!t->active) {
            release_state(re, t->id);
            continue;
        }

        s = get_state(re, t->id);

        /* ROI 키오스크 체크: 기준점(중심 또는 발밑)이 ROI 안에 있으면 ORDERED 로 전환 */
        /* Proximity is weak evidence, never payment confirmation. */
        if (t->order == TRACK_UNORDERED) {
            const Detection *b=&t->box;
            float bw=b->x2-b->x1, bh=b->y2-b->y1;
            int near=re->frame_width>0 && re->frame_height>0 &&
                bh>=re->frame_height*0.65f &&
                bw*bh>=re->frame_width*(float)re->frame_height*0.20f &&
                b->score>=0.60f && b->keypoint_count>=13 && upper_body_upright(b)==1;
            if(near && b->kp[11].score>=KP_SCORE_THRESH && b->kp[12].score>=KP_SCORE_THRESH)
                near=(b->kp[11].y+b->kp[12].y-b->kp[5].y-b->kp[6].y)*0.5f>bh*0.12f;
            if(t->misses==0 && near) {
                if(s->order_near_start<=0 || now-s->order_near_last>2.0) s->order_near_start=now;
                s->order_near_last=now;
                if(now-s->order_near_start>=30.0) {
                    t->order=TRACK_PROBABLY_ORDERED;
                    snprintf(msg,sizeof(msg),"probably_ordered track=%d hold=%.1fs payment=unconfirmed",t->id,now-s->order_near_start);
                    event_log_write(elog,LOG_INFO,"rules",msg);
                }
            } else if(t->misses==0 || now-s->order_near_last>2.0) {
                s->order_near_start=s->order_near_last=0;
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

        /* ── 미주문 착석 ──
         * PROBABLY_ORDERED(키오스크 앞 30초 유지)도 억제 대상으로 봅니다. 결제 연동이 없어
         * ORDERED 에 도달할 방법이 없으므로, 억제하지 않으면 정상적으로 주문한 손님을 포함한
         * 오래 머문 모든 사람에게 발화합니다. 대신 키오스크 앞에 서 있다가 결제하지 않고
         * 앉은 사람은 놓칠 수 있습니다 — 근접은 결제 증거가 아니라는 위 판단은 그대로이며,
         * 이 조합이 오탐(점주 신뢰 하락)보다 미탐을 택한 결과입니다. */
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
        } else if (t->order != TRACK_UNORDERED) {
            s->unordered_latched = 0; /* 이미 발화한 뒤 주문 추정/확인으로 바뀌면 해제 */
        }

        /* All fall paths share the same fresh-evidence and hold gates. */
        {
            const Detection *b=&t->box;
            double gap=t->last_seen-s->fall_last_observation;
            int pose, horizontal, quality;
            t->fall_sudden=0; /* Head template movement cannot bypass person validation. */
            if(t->misses || t->last_seen<=0 || now-t->last_seen>2.0 || t->last_seen>now+.01) {
                s->fall_gate=2;s->fall_start=0;s->fall_samples=0;
                still_reset(s); /* 관측이 끊긴 구간은 움직임 없음의 증거가 아닙니다 */
                continue;
            }
            if(t->last_seen<=s->fall_last_observation)continue; /* not another observation */
            if(s->fall_last_observation>0 && gap>2.0) {
                s->fall_start=0;s->fall_samples=0;s->upright_start=0;s->upright_samples=0;
            }
            s->fall_last_observation=t->last_seen;
            quality=isfinite(b->score)&&isfinite(t->match_score)&&b->score>=.5f&&t->match_score>=.5f;
            pose=quality?fall_torso_pose(b):0;
            if(!quality || !pose) {
                s->fall_gate=quality?3:1;s->fall_start=0;s->fall_samples=0;
                s->upright_start=0;s->upright_samples=0;
                still_reset(s);
                continue;
            }
            still_update(re,t,s,pose,now,elog);
            if(pose==1) {
                if(s->upright_start<=0)s->upright_start=now;
                s->upright_samples++;
                if(s->upright_samples>=3 && now-s->upright_start>=1.0)s->upright_last=now;
                s->fall_start=0;s->fall_samples=0;s->fall_latched=0;s->fall_gate=0;
                continue;
            }
            s->upright_start=0;s->upright_samples=0;
            horizontal=is_horizontal_pose(b,re->config.fall_aspect_ratio_kp,
                re->config.fall_aspect_ratio_nokp,re->frame_width,re->frame_height);
            if(!horizontal) {s->fall_start=0;s->fall_samples=0;s->fall_gate=3;continue;}
            if(s->fall_latched) {s->fall_gate=6;continue;}
            if(s->upright_last<=0 || now-s->upright_last>30.0) {
                s->fall_gate=4;s->fall_start=0;s->fall_samples=0;continue;
            }
            if(s->fall_start<=0)s->fall_start=now;
            s->fall_samples++;s->fall_gate=5;
            if(s->fall_samples>=3 && now-s->fall_start>=re->config.fall_hold_seconds) {
                s->fall_latched=1;s->fall_gate=6;
                snprintf(msg,sizeof(msg),
                    "person_fallen track=%d hold=%.1fs score=%.2f match=%.2f samples=%u evidence=upright_to_horizontal",
                    t->id,now-s->fall_start,b->score,t->match_score,s->fall_samples);
                event_log_write(elog,LOG_ERROR,"rules",msg);note_fall(re,t);
            }
        }
    }
}
