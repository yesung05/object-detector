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
    /* 주인 추정(residue_track_owners): 영역이 처음 잡힌 순간 가장 가까이 있던 사람 트랙.
     * owner_checked=0 이면 아직 판정 전, owner_id<0 이면 주변에 사람이 없었음. */
    int         owner_checked;
    int         owner_id;
    int         left_fired;   /* item_left 를 이미 발화했는가 */
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
    /* 자동 기준 캡처: 기준이 없을 때 사람 없음·화면 정지가 auto_quiet_seconds 지속되면
       현재 gray 를 기준으로 삼습니다. 설치 직후 "대시보드에서 캡처하세요" 단계를 없앱니다. */
    int    auto_enabled;             /* residue_auto_capture (기본 1) */
    double auto_quiet_seconds;       /* residue_auto_quiet_seconds (기본 15) */
    /* 기준 학습(residue_learn_background, 기본 0=끔). 1 이면 "빈 매장 사진" 없이 사람이 없는 블록에서 배경을
     * 배워 기준으로 쓴다. 손님이 끊이지 않아 빈 매장 순간이 없는 매장(AI-Hub 클립 42개 중 0개가 자동 기준을 잡음)용. */
    int    learn_background;         /* residue_learn_background */
    /* 주인 거리 배율: 영역이 처음 잡힐 때 사람 박스 중심에서 (박스 높이 × 이 값) 안에 있으면 그 사람을 주인으로 본다.
     * 주인이 사라진 뒤 영역이 확정되면 item_left 를 남긴다. 0 이면 주인 추정 끔. */
    float  owner_reach;              /* residue_owner_reach (기본 1.0) */
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

    /* 자동 캡처 런타임 (residue_auto_init) */
    int    auto_phase;           /* 0=비활성 1=대기 2=기준 있음 */
    double auto_quiet_since;     /* 조건 시작 시각, 0=미충족 */
    double auto_wait_seconds;    /* 대시보드 표시용 */
    int    size_mismatch_logged; /* 기준 크기 불일치 경고를 한 번만 남기기 위한 래치 */
    /* 학습 모드에서 블록별로 배경을 본 적이 있는지(1) 없는지(0). 본 적 없는 블록은 후보가 되지 않는다.
     * ResidueMonitor 소유 — residue_destroy 에서 free. 고정 기준 모드에서는 NULL. */
    uint8_t *known;
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

/*
 * 기준이 없을 때 자동으로 기준을 잡습니다. residue_evaluate 앞에서 매 프레임 호출합니다.
 *
 * motion_ratio: 이번 프레임의 변화 픽셀 비율(gray_analyze 결과). 0.002 미만이면 정지로 봅니다.
 * 기준을 잡으면 1 을 반환하고 baseline_ready=1 이 됩니다. 이 모듈은 gray 만 가지므로
 * 파일은 쓰지 않습니다 — 호출자가 원본 RGB 를 raw_rgb_save 로 저장하면 재시작 후에도 남습니다.
 */
/*
 * 기준 학습 한 단계(config.learn_background=1 일 때만 동작). residue_evaluate 앞에서 매 프레임 호출합니다.
 * 사람 박스(+person_margin_blocks)에 덮이지 않은 블록만 배운다:
 *   - 처음 보는 블록: 현재 값을 그대로 기준으로(known=1)
 *   - 이미 아는 블록: 기준과 차이가 diff_threshold 미만이면 1/8 씩 따라감(조명·그림자 서서히 변하는 것)
 *   - 차이가 큰 블록(후보)은 배우지 않는다 — 놓고 간 물건이 배경에 흡수되면 영원히 못 잡는다.
 * 고정 기준(파일·자동 캡처)이 이미 있으면 아무것도 하지 않는다. 반환 0, 메모리 부족이면 -1.
 */
int residue_learn_background(ResidueMonitor *r, const GrayBuf *gray,
                             const GrayRect *persons, int person_count, double now);

/*
 * 영역의 주인을 추정하고, 주인이 떠난 뒤 확정된 영역에 item_left 를 남깁니다. residue_evaluate 뒤에 호출합니다.
 * persons/person_ids: 활성 사람 트랙 박스와 트랙 id(같은 순서, 호출자 소유·읽기만). config.owner_reach<=0 이면 끔.
 * 반환: 이번 호출에서 발화한 item_left 수.
 */
int residue_track_owners(ResidueMonitor *r, const GrayRect *persons, const int *person_ids,
                         int person_count, double now, EventLog *elog);

int residue_auto_init(ResidueMonitor *r, const GrayBuf *gray,
                      int person_count, int camera_ok, double motion_ratio, double now);

#endif /* RESIDUE_H */
