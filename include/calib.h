#ifndef CALIB_H
#define CALIB_H

#include "gray.h"

#include <stddef.h>
#include <stdint.h>

/*
 * 설치 직후 안정화(캘리브레이션) — 반복 흔들림 구역을 학습해 무시 구역 후보를 만듭니다.
 *
 * 선풍기, 바람에 흔들리는 달력·현수막은 사람이 없어도 계속 변화를 만들어
 * 게이트(L1)를 통과시키고 YOLO를 깨웁니다. 점주가 ignore_roi 를 손으로
 * 적어 넣기를 기대하기 어려우므로, 처음 N분 동안 블록별 변화 빈도를
 * 세어 "자주 변하는데 사람과 무관한 블록"을 찾아냅니다.
 *
 * 매 프레임이 아니라 sample_seconds 간격으로 비교하는 이유:
 *   - 천천히 흔들리는 달력은 프레임 간 변위가 다운샘플 격자에서 1픽셀
 *     미만이라 매 프레임 비교로는 잡히지 않습니다. 0.5초 간격이면 누적됩니다.
 *   - 비용이 초당 2회로 고정되어, 안정화 중에도 i5-4200U 부하가 거의 늘지 않습니다.
 *
 * 누적 배열을 구조체 안의 고정 배열로 두는 이유: 블록 수 상한(GRAY_MAX_BLOCKS)이
 * 이미 정해져 있어 8KB 로 충분하고, 별도 malloc/free 쌍을 늘리지 않기 위함입니다.
 */

typedef struct {
    double duration_seconds;  /* 안정화 기간. 0 이하면 기능 비활성 */
    double sample_seconds;    /* 비교 간격 */
    float  noise_ratio;       /* 관측 표본 중 이 비율 이상 변한 블록 = 흔들림 */
    float  max_area_ratio;    /* 후보 사각형이 전체 블록의 이 비율을 넘으면 거부 */
} CalibOptions;

typedef enum {
    CALIB_REGION_OK = 0,
    CALIB_REGION_TOO_LARGE,   /* 조명 변화·TV 등 넓은 영역 — 자동 무시하기엔 위험 */
    CALIB_REGION_PROTECTED    /* 문/키오스크 ROI 와 겹침 — 절대 무시하면 안 되는 곳 */
} CalibRegionStatus;

typedef struct {
    GrayRect          rect;        /* 원본 프레임 좌표 (x1,y1,x2,y2) */
    int               blocks;      /* 구성 블록 수 */
    float             mean_ratio;  /* 구성 블록의 평균 변화 비율 */
    CalibRegionStatus status;
} CalibRegion;

typedef struct {
    CalibOptions opt;          /* calib_start 시점 사본. 진행 중 설정 변경은 반영하지 않음 */
    /* 직전 표본 시점의 gray 사본.
     * 소유: MotionCalib. calib_start 에서 malloc, calib_destroy 에서 free.
     * NULL 이면 안정화가 진행 중이 아님(calib_active 판정 기준). */
    uint8_t     *snapshot;
    int          snapshot_ready;
    int          gray_width;
    int          gray_height;
    int          downsample;
    int          frame_width;
    int          frame_height;
    int          blocks_x;
    int          blocks_y;
    uint32_t     changed[GRAY_MAX_BLOCKS];   /* 변화로 관측된 표본 수 */
    uint32_t     observed[GRAY_MAX_BLOCKS];  /* 사람에 가려지지 않은 표본 수 */
    int          samples;         /* 집계에 반영된 표본 수 */
    int          skipped_global;  /* 화면 전체 변화(조명·노출)로 버린 표본 수 */
    double       started;
    double       last_sample;
} MotionCalib;

/* g 의 크기를 기준으로 누적을 시작합니다. 성공 0, 실패 -1. */
int  calib_start(MotionCalib *c, const GrayBuf *g, int frame_width,
                 int frame_height, const CalibOptions *opt, double now,
                 char *error, size_t error_size);
/* snapshot 을 해제하고 비활성 상태로 되돌립니다. NULL·비활성 상태도 안전합니다. */
void calib_destroy(MotionCalib *c);
int  calib_active(const MotionCalib *c);
int  calib_elapsed_done(const MotionCalib *c, double now);

/*
 * sample_seconds 가 지났으면 현재 gray 를 직전 표본과 비교해 누적합니다.
 * people: 현재 추적 중인 사람 박스(원본 좌표). 그 주변 블록은 집계에서 뺍니다 —
 *         사람의 움직임을 "흔들림"으로 배우면 그 자리 감지가 꺼지기 때문입니다.
 * 반환 1 = 이번 호출에서 표본을 집계함, 0 = 건너뜀.
 */
int  calib_tick(MotionCalib *c, const GrayBuf *cur, const GrayRect *people,
                int people_count, int block_min_changed, double now);

/* 이미 계산된 블록 지도 하나를 누적합니다. calib_tick 내부와 테스트에서 씁니다. */
void calib_accumulate(MotionCalib *c, const MotionMap *map,
                      const GrayRect *people, int people_count);

/*
 * 누적 결과에서 흔들림 구역을 인접 블록끼리 묶어 사각형으로 뽑습니다.
 * protect 와 겹치는 블록은 후보에서 먼저 빼고, 묶은 뒤에도 겹치면 PROTECTED 로 표시합니다.
 * 블록 수가 큰 순서로 최대 max_out 개를 out 에 채우고 그 개수를 반환합니다.
 * total_found 가 NULL 이 아니면 상한과 무관하게 찾은 전체 구역 수를 씁니다.
 */
int  calib_extract(const MotionCalib *c, const GrayRect *protect,
                   int protect_count, CalibRegion *out, int max_out,
                   int *total_found);

const char *calib_region_status_name(CalibRegionStatus status);

#endif /* CALIB_H */
