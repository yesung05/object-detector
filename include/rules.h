#ifndef RULES_H
#define RULES_H

#include "tracks.h"
#include "log.h"
#include "yolo11.h"

/*
 * 룰 기반 이상 탐지입니다. 각 이벤트는 트랙당 1회만 발화(latch)하고,
 * 조건이 해소되면 latch를 해제하여 재발 시 다시 감지합니다.
 */

/*
 * Tier 2 파인튜닝 모델의 리매핑 클래스 ID (0-15)입니다.
 * Ultralytics classes= 파라미터로 학습하면 원본 COCO ID가 이 순서대로 재배치됩니다.
 * 원본 COCO: cat=15, dog=16, bottle=39, cup=41, banana=46…cake=55, chair=56, table=60
 */
#define OBJ_CAT         0
#define OBJ_DOG         1
#define OBJ_BOTTLE      2
#define OBJ_CUP         3
#define OBJ_FOOD_FIRST  4   /* banana */
#define OBJ_FOOD_LAST   13  /* cake   */
#define OBJ_CHAIR       14
#define OBJ_DININGTABLE 15

typedef struct {
    double dwell_limit_seconds;       /* 기본 3600 — 초과 체류 판정 기준 */
    double unordered_grace_seconds;   /* 기본 300  — 미주문 착석 유예 시간 */
    double fall_hold_seconds;         /* 기본 5.0  — 쓰러짐으로 확정하는 최소 지속 시간 */
    /* 키오스크 ROI: 박스 중심이 이 영역 안에 있으면 ORDERED 로 전환 (결제 프록시) */
    float  roi_kiosk_x, roi_kiosk_y, roi_kiosk_w, roi_kiosk_h;
    int    roi_kiosk_set;
    /* Tier 2 물체 감지 설정 */
    float  animal_iou_threshold;  /* 기본 0.15 — 동물/가구 IoU 판정 기준 */
    int    no_cup_margin;         /* 기본 1    — 미구매 착석 판정 여유분 */
} RulesConfig;

/*
 * 트랙당 룰 상태입니다. track_id % capacity 로 슬롯을 인덱싱합니다.
 * track_id == -1 이면 미사용 슬롯입니다.
 */
typedef struct {
    int    track_id;
    int    overstay_latched;
    int    unordered_latched;
    int    fall_latched;
    double fall_start;  /* 수평 자세가 시작된 시각 (0이면 미시작) */
} TrackRuleState;

typedef struct {
    TrackRuleState *states;  /* RulesEngine 소유, rules_destroy 에서 free */
    size_t          capacity;
    RulesConfig     config;
} RulesEngine;

int  rules_init(RulesEngine *re, size_t capacity, const RulesConfig *config,
                char *error, size_t error_size);
void rules_destroy(RulesEngine *re);

/* 실행 중 설정 교체 — 기존 latch 상태는 유지합니다. */
void rules_update_config(RulesEngine *re, const RulesConfig *config);

/*
 * TrackList 전체를 순회하며 룰을 평가합니다.
 * 이벤트 발생 시 event_log 에 근거 key=value 포함 메시지를 기록합니다.
 */
void rules_evaluate(RulesEngine *re, TrackList *tl, double now, EventLog *elog);

/*
 * Tier 2 물체 감지 결과를 기반으로 룰을 평가합니다.
 * 이 함수는 Tier 2 모델 실행 직후 호출됩니다.
 *
 * 발화 이벤트:
 *   external_drink     — bottle 감지
 *   external_food      — 음식류(banana..cake) 감지
 *   animal_on_chair    — cat/dog bbox가 chair와 IoU ≥ animal_iou_threshold
 *   animal_on_table    — cat/dog bbox가 dining table과 IoU ≥ animal_iou_threshold
 *   no_cup_seated      — 활성 사람 수 > cup 감지 수 + no_cup_margin
 */
void rules_evaluate_objects(RulesEngine *re, const DetectionList *objs,
                             const TrackList *tl, double now, EventLog *elog);

#endif /* RULES_H */
