#ifndef FIGHT_STAGE2_H
#define FIGHT_STAGE2_H

#include "yolo11.h"

/*
 * 폭행(싸움) 2단계 감지 — 1차 게이트(규칙) + 2차 소형 분류기(콜백).
 *
 * 왜 이 구조인가: 폭행은 규칙(쌍 근접 × 팔다리 속도)만으로는 정밀도가 모자라(AI-Hub dev 엄격 TPR 0.84) 모델이 필요하지만,
 * 프로젝트 원칙(규칙 우선·AI 최소)에 따라 모델은 "의심 구간에서만" 부른다. 이 모듈은
 *   1) 추론이 일어난 프레임마다 가장 가까운 두 사람을 골라 쌍 기준으로 정규화한 특징(106차원)을 만들고,
 *   2) 약 3fps 로 표본을 뽑아 9표본(3초) 창을 만든 뒤,
 *   3) 게이트(2인 이상 5표본 이상 + 근접도 0.2 이상 3표본 이상)를 통과한 창에만 분류기를 호출하고,
 *   4) 임계값 이상 창이 9표본 이내에 2개 이상이면 발화한다.
 * 학습 데이터와 같은 규칙이다(scripts/fight2_extract.py, docs/prereg_fight_stage2.md). 규칙을 바꾸면 모델을 다시 학습해야 한다.
 * 분류기는 콜백으로 받아 ONNX Runtime 없이도 단위 시험할 수 있다(src/fight_classifier.c 가 실제 구현).
 *
 * 기본 꺼짐(config fight_enabled). AI-Hub 평가 기준 재현율 약 0.80, 오탐은 "판단 불가(경계)"라 점주 알림으로 켜는 것은 현장 그림자
 * 운영으로 오탐을 잰 뒤에 결정한다.
 */
enum { FIGHT_T = 9, FIGHT_D = 106 };

/* 분류기: window[FIGHT_T*FIGHT_D] 를 받아 폭행 확률(0~1)을 돌려준다. 실패하면 음수. */
typedef float (*FightScoreFn)(void *context, const float *window);

typedef struct FightMonitor FightMonitor;   /* 불완전 타입: 멤버는 fight_stage2.c 에만 있다 */

typedef struct {
    int    fired;            /* 1 이면 이번 호출에서 발화 */
    float  score;            /* 발화를 만든 창의 확률 */
    int    hit_windows;      /* 연속 임계값 초과 창 수(최근 구간) */
} FightEvent;

typedef struct {
    unsigned long samples;        /* 표본 수 */
    unsigned long gate_pass;      /* 게이트를 통과한 창 수(분류기 호출 후보) */
    unsigned long scorer_calls;   /* 분류기 호출 수 */
    unsigned long hits;           /* 임계값 이상이었던 창 수 */
    unsigned long fires;          /* 발화 수 */
    double        scorer_seconds; /* 분류기 누적 시간(호출자가 측정해 넣는다) */
} FightStats;

/* 소유: 호출자가 fight_destroy 로 해제(NULL 안전). threshold: 발화 확률 임계값, sample_interval: 표본 간격(초, 기본 1/3). */
FightMonitor *fight_create(double threshold, double sample_interval);
void          fight_destroy(FightMonitor *m);
void          fight_set_scorer(FightMonitor *m, FightScoreFn fn, void *context);
void          fight_set_threshold(FightMonitor *m, double threshold);
void          fight_get_stats(const FightMonitor *m, FightStats *out);

/*
 * 추론이 일어난 프레임마다 호출한다. persons: 그 프레임의 사람 검출(관절 포함). now: 규칙과 같은 단조 시계(초).
 * 표본 간격마다 하나씩 표본으로 받고, 표본 사이가 너무 벌어지면(추론이 쉬었으면) 창을 비운다 — 창은 연속 시간이어야 학습과 같다.
 * 발화 직후 cooldown 초 동안은 다시 발화하지 않는다. 반환 1 = 발화(out 채움).
 */
int fight_update(FightMonitor *m, const DetectionList *persons, double now, double cooldown, FightEvent *out);

/* 시험용: 프레임 하나의 특징 벡터(106)와 근접도를 만든다. 사람이 2명 미만이면 valid=0. */
void fight_frame_features(const DetectionList *persons, float *vec, int *valid, float *close);

#endif /* FIGHT_STAGE2_H */
