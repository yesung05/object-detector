#ifndef RESIDUE_H
#define RESIDUE_H

#include "gray.h"
#include "log.h"

#include <stdint.h>

/*
 * 잔류물 감지 모듈입니다.
 *
 * 통상적인 영상 감시는 움직임을 이상 신호로 봅니다. 이 모듈은 반대 관점에서
 * 동작합니다 — 변화가 사라지지 않는다는 사실(부동성)이 이상 신호입니다.
 * 청결 기준 상태와 현재 프레임을 블록 단위로 비교하고, 변화가 사람에 의한
 * 것이 아니면서 일정 시간 이상 지속될 때 이벤트를 발화합니다.
 *
 * 토사물과 쓰레기를 학습 대상으로 삼지 않는 이유:
 * - 공개 데이터셋(COCO 등)에 해당 클래스가 없습니다.
 * - 실제 발생 장면을 촬영해 수집하는 것은 현실적·윤리적으로 어렵습니다.
 * - 형태·색상·재질이 일정하지 않아 클래스로 정의하는 것 자체가 부적절합니다.
 * 따라서 잔류물의 종류가 아니라 잔류 사실만 판정합니다.
 *
 * 설치 시 대시보드에서 청결 상태를 캡처합니다(residue_clean_reference.raw).
 * 기준이 없으면 조용히 비활성 동작합니다.
 */

#define RESIDUE_MAX_REGIONS 8

typedef enum {
    RESIDUE_UNKNOWN = 0,  /* Tier 2 비활성 — 종류 미분류 */
    RESIDUE_SPILL   = 1,  /* 바닥 — 토사물·액체 흘림 의심 */
    RESIDUE_TRASH   = 2   /* 가구 위 — 방치된 쓰레기 의심 */
} ResidueKind;

/*
 * 추적 중인 잔류 영역 하나입니다.
 * 좌표는 원본 프레임 기준이며, 블록 격자에서 역산합니다.
 */
typedef struct {
    int         in_use;       /* 0이면 빈 슬롯 */
    int         confirmed;    /* 이벤트를 이미 발화했는가 */
    ResidueKind kind;
    double      first_seen;   /* 후보로 처음 잡힌 시각 (monotonic) */
    double      last_seen;    /* 마지막으로 관측된 시각 */
    int         blocks;       /* 구성 블록 수 — 면적의 대용 */
    float       x1, y1, x2, y2;
} ResidueRegion;

typedef struct {
    int    enabled;
    int    diff_threshold;           /* residue_diff_threshold — 블록 평균 휘도 차 */
    int    min_blocks;               /* residue_min_blocks */
    int    person_margin_blocks;     /* residue_person_margin_blocks */
    double confirm_seconds;          /* residue_confirm_seconds */
    double clear_seconds;            /* residue_clear_seconds */
    double baseline_refresh_seconds; /* residue_baseline_refresh_seconds */
    float  global_change_ratio;      /* residue_global_change_ratio */
} ResidueConfig;

typedef struct {
    ResidueConfig config;

    /* 청결 상태 기준. residue_clean_reference.raw 에서 로드하거나
     * 안전 시점에 현재 프레임으로 갱신합니다.
     * ResidueMonitor 소유 — residue_destroy 에서 free 합니다. */
    uint8_t *baseline;
    int      baseline_w, baseline_h;
    int      baseline_ready;
    double   baseline_stamp;   /* 마지막 갱신 시각 */

    /* 후보 블록 표시용 작업 버퍼. 프레임마다 재사용하며 매번 할당하지 않습니다.
     * ResidueMonitor 소유 — residue_destroy 에서 free 합니다. */
    uint8_t *candidate;
    int      blocks_x, blocks_y;

    ResidueRegion regions[RESIDUE_MAX_REGIONS];
} ResidueMonitor;

/*
 * 기준 이미지를 파일에서 로드합니다.
 * 파일이 없으면 baseline_ready=0으로 두고 0을 반환합니다(에러 아님).
 * 이미 로드된 버퍼가 있으면 먼저 해제합니다.
 */
int  residue_load(ResidueMonitor *r, const char *path);

void residue_destroy(ResidueMonitor *r);

/*
 * 잔류 판정을 수행합니다. 매 프레임 그리기 이전에 호출해야 합니다.
 *
 * gray: 현재 프레임의 다운샘플 그레이 버퍼. 호출자(AppContext) 소유이며 읽기만 합니다.
 *       이 함수가 반환한 뒤 보관하지 않습니다.
 * persons: 활성 트랙 bbox 배열. 원본 프레임 좌표계. 호출자 소유이며 읽기만 합니다.
 *          person_count=0이면 사람 배제 없이 판정합니다.
 * furniture: Tier 2 가구 bbox 배열 (chair/dining_table). 호출자 소유.
 *            NULL이면 종류 분류를 건너뜁니다.
 * elog: 이벤트 로그. NULL이면 로그를 남기지 않습니다.
 *
 * 반환값: 이번 프레임에 확정된 영역 수 (0 이상), 비활성이면 0.
 */
int residue_evaluate(ResidueMonitor *r,
                     const GrayBuf *gray,
                     const GrayRect *persons, int person_count,
                     const GrayRect *furniture, int furniture_count,
                     double now, EventLog *elog);

/*
 * 현재 gray 버퍼를 기준으로 새 기준을 설정합니다.
 * 조명 변화 강제 갱신 및 안전 시점 자동 갱신에 쓰입니다.
 */
int residue_refresh_baseline(ResidueMonitor *r, const GrayBuf *gray, double now);

#endif /* RESIDUE_H */
