#ifndef SLOT_MONITOR_H
#define SLOT_MONITOR_H

/*
 * 슬롯 기반 테이블·의자 청결 모니터링.
 *
 * Tier 2 YOLO가 감지한 chair/dining_table bbox를 "슬롯"으로 관리하여
 * 각 슬롯별 기준 이미지(crop)와 현재 프레임을 비교합니다.
 * 전체 프레임 배경 차분(residue.c)이 조명 변화에 취약한 문제를 보완하며,
 * 의자·테이블 위 물체만 판정하여 바닥 잔류물 감지와 역할을 나눕니다.
 *
 * 기존 ResidueMonitor를 대체하지 않고 병행 운영합니다.
 */

#include <stdint.h>
#include "gray.h"
#include "log.h"
#include "config.h"

/* 무인 카페 기준: 테이블 5 + 의자 12 + 여유 3 */
#define SLOT_MAX 20

typedef struct {
    int      id;           /* 안정 식별자: class_id·위치 기반 정수 키 */
    int      class_id;     /* OBJ_CHAIR(14) or OBJ_DININGTABLE(15) */
    GrayRect bbox;         /* Tier 2가 마지막으로 반환한 가구 bbox, 원본 해상도 */

    /* baseline_gray: SlotMonitor 소유, slot_monitor_destroy에서 free.
     * 수명: 슬롯이 활성인 기간. 슬롯 만료·재설정 시 즉시 해제. */
    uint8_t *baseline_gray;
    int      baseline_w;
    int      baseline_h;
    int      has_baseline;        /* 0 = 아직 미캡처 */
    double   clean_since;         /* 마지막으로 clean 상태가 된 시각 (자동 재학습 기준) */

    double   dirty_since;         /* dirty가 된 시각. 0이면 clean. */
    int      dirty;
    int      warn_fired;          /* 중복 발화 억제 latch */
    int      urgent_fired;
    double   last_seen;           /* 마지막으로 Tier 2가 이 가구를 감지한 시각 */
    double   activated_at;        /* 슬롯 최초 활성화 시각 (slot_no_baseline 발화 기준) */
    int      no_baseline_fired;   /* slot_no_baseline 1회 발화 latch */
} Slot;

typedef struct {
    Slot   slots[SLOT_MAX];
    int    count;

    /* 아래 설정은 slot_monitor_apply_config()로 주입합니다. 기본값은 init에서 설정. */
    long   warn_seconds;             /* 기본 60  — 짐 내려놓음(~30s)과 구분 */
    long   urgent_seconds;           /* 기본 300 — 장기 방치 */
    long   ttl_seconds;              /* 기본 120 — Tier 2 미탐지 후 슬롯 만료 */
    int    dirty_threshold;          /* 블록 평균 luma diff 임계값, 기본 20 */
    int    min_dirty_blocks;         /* 슬롯 crop이 작으므로 residue의 3보다 낮게, 기본 2 */
    float  person_iou_skip;          /* 사람이 이 IoU 이상 겹치면 스킵, 기본 0.10f */
    long   no_baseline_warn_seconds; /* 기준 미캡처 경고 지연, 기본 10 */
    long   auto_relearn_seconds;     /* clean 복구 후 자동 재학습 대기, 기본 30 */
} SlotMonitor;

void slot_monitor_init(SlotMonitor *sm);
void slot_monitor_destroy(SlotMonitor *sm);

/*
 * config.json에서 slot_* 키를 읽어 SlotMonitor 설정을 갱신합니다.
 * hot-reload와 초기 시작 두 경로에서 모두 호출합니다.
 */
void slot_monitor_apply_config(SlotMonitor *sm, const Config *cfg);

/*
 * Tier 2 결과를 받아 슬롯 목록 갱신 + 슬롯별 crop diff 체크 + 이벤트 발화.
 *
 * rgb:              원본 프레임 RGB24 데이터 (호출자 소유, 읽기만 함)
 * w, h:             원본 해상도
 * furniture:        Tier 2 chair/dining_table bbox 배열 (호출자 소유, 읽기만 함)
 * furniture_count:  위 배열 원소 수
 * persons:          활성 트랙 bbox 배열 (호출자 소유, 읽기만 함)
 * person_count:     위 배열 원소 수
 * now:              platform_monotonic_seconds() 현재 시각
 * elog:             이벤트 로그 (NULL이면 기록 안 함)
 */
void slot_monitor_update(SlotMonitor *sm,
                         const uint8_t *rgb, int w, int h,
                         const GrayRect *furniture, int furniture_count,
                         const GrayRect *persons,   int person_count,
                         double now, EventLog *elog);

#endif /* SLOT_MONITOR_H */
