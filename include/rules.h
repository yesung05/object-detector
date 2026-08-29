#ifndef RULES_H
#define RULES_H

#include "tracks.h"
#include "log.h"

/*
 * 룰 기반 이상 탐지입니다. 각 이벤트는 트랙당 1회만 발화(latch)하고,
 * 조건이 해소되면 latch를 해제하여 재발 시 다시 감지합니다.
 */

typedef struct {
    double dwell_limit_seconds;       /* 기본 3600 — 초과 체류 판정 기준 */
    double unordered_grace_seconds;   /* 기본 300  — 미주문 착석 유예 시간 */
    double fall_hold_seconds;         /* 기본 5.0  — 쓰러짐으로 확정하는 최소 지속 시간 */
    /* 키오스크 ROI: 박스 중심이 이 영역 안에 있으면 ORDERED 로 전환 (결제 프록시) */
    float  roi_kiosk_x, roi_kiosk_y, roi_kiosk_w, roi_kiosk_h;
    int    roi_kiosk_set;
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

#endif /* RULES_H */
