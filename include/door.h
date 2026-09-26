#ifndef DOOR_H
#define DOOR_H

#include "gray.h"   /* GrayRect — 자동 캡처가 받는 사람 bbox 형식 */

#include <stdint.h>

/*
 * 문 여닫이 감지 모듈입니다.
 *
 * 대시보드에서 "닫힌 상태"와 "열린 상태" 기준 이미지를 각각 캡처하면
 * stream.c가 door_closed_reference.raw / door_open_reference.raw로 저장합니다.
 * 이 모듈은 현재 프레임을 두 기준과 비교해 더 가까운 상태로 판정합니다.
 *
 * 두 기준 이미지를 모두 사용하는 이유:
 * - 단일 기준(닫힘만)은 "현재 vs 닫힘 차이"가 임계값을 넘으면 열림으로 판정하므로
 *   조명 변화나 카메라 노이즈로 인한 위양성이 많음
 * - 두 기준 모두 있으면 "닫힘과의 거리 vs 열림과의 거리"를 비교해
 *   더 가까운 쪽으로 판정 → 조명 변화에 강인함
 *
 * 닫힌 기준만 있으면 기존 방식(변화 비율 vs diff_threshold)으로 동작합니다.
 *
 * 추론 없이 픽셀 차이만 사용하는 이유:
 * - 문 개폐는 고정 배경 대비 뚜렷한 픽셀 변화로 감지 가능
 * - YOLO 추론 없이 1ms 미만으로 처리 가능 (i5-4200U 부하 최소화)
 * - 기준 이미지 없으면 조용히 비활성
 */

typedef struct {
    /* 닫힌 상태 기준 이미지 (door_closed_reference.raw) — door_destroy가 해제 */
    uint8_t *ref_closed_rgb;
    int      ref_closed_w;
    int      ref_closed_h;

    /* 열린 상태 기준 이미지 (door_open_reference.raw) — door_destroy가 해제 */
    uint8_t *ref_open_rgb;
    int      ref_open_w;
    int      ref_open_h;

    /* config.json에서 읽어오는 설정 */
    int   enabled;
    int   roi_x, roi_y, roi_w, roi_h; /* 0이면 전체 프레임 사용 */

    /* 단일 기준(닫힘만 있을 때) 판정 임계값.
     * 두 기준 모두 있으면 거리 비교로 판정하므로 이 값은 무시됩니다. */
    float diff_threshold; /* 0.0-1.0, 기본 0.05 */

    /* ── 상단 밴드 판정 (사람이 문을 가릴 때) ──────────────────────────────
     * 사람이 ROI 를 채우고 서 있으면 전체 비교는 문의 차이가 아니라 사람 픽셀을 재게 되어
     * 두 기준까지의 거리가 비슷해지고 판정이 사실상 무작위가 됩니다. 문 위쪽은 머리 위라
     * 통행에 가려지지 않으므로, 가려진 동안에는 ROI 상단 일부만으로 판정합니다.
     *
     * 평소에는 픽셀이 많은 전체 ROI 가 더 정확하므로 밴드는 가려졌을 때만 씁니다.
     *
     * 위험: ROI 를 문틀 위(상인방)까지 넉넉히 잡으면 밴드가 "열려도 닫혀도 똑같은" 구역에
     * 걸려 감지가 조용히 죽습니다. 그래서 기준 두 장이 모이면 밴드가 실제로 두 상태를
     * 구분하는지 한 번 측정해 band_valid 로 남기고, 무효면 가림 중 판정을 보류합니다. */
    float band_ratio;   /* config: door_band_ratio. ROI 상단 비율(0.3=위 30%). 0=밴드 비활성 */
    int   band_valid;   /* -1=미평가, 0=무효(신호 없음), 1=유효 */
    float band_signal;  /* 닫힘·열림 기준의 밴드 영역 평균 L1 — 진단용 */
    int   band_active;  /* 이번 프레임을 밴드로 판정했는가 (대시보드 표시용) */

    /* 상태 래치 — 매 프레임 이벤트 폭주 방지 */
    int last_state; /* -1=초기화 전, 0=닫힘, 1=열림 */

    /* 지속 시간 기반 이벤트 — 열린 상태가 이 시간 이상 유지될 때만 로그 발생.
     * 문이 잠깐 열렸다 닫히는 정상 상황(고객 입퇴장)을 필터링하기 위한 값. */
    int    confirm_frames;
    int    candidate_state;
    int    candidate_frames;
    double open_threshold_seconds; /* config: door_open_seconds, 기본 30.0 */
    double open_since;             /* 열림 시작 시각 (monotonic). -1=닫힌 상태 */
    int    open_event_fired;       /* 1=이번 개방 주기에 이미 이벤트를 발생시킴 */

    /* ── 자동 기준 캡처 (door_auto_update) ──────────────────────────────────
     * 설치 기사가 닫힘/열림 두 장을 손으로 캡처하던 절차를 대신합니다.
     * 닫힘: ROI 가 정지해 있고 사람이 없는 상태가 auto_quiet_seconds 지속되면 저장.
     *       무인 카페 문은 자동 닫힘이라 "아무도 없을 때의 문 = 닫힘"을 전제합니다.
     *       이 전제가 깨지면(문을 열어 둔 매장) door_auto_capture=0 으로 끄고 수동 캡처합니다.
     * 열림: 닫힘 기준이 있고, 사람은 ROI 를 벗어났는데 ROI 만 크게 달라진 상태가
     *       auto_open_hold_seconds 유지되면 저장. 화면 전체가 같이 달라졌으면
     *       조명 변화로 보고 건너뜁니다. */
    /* Passage learning: provisional frames never become references until two
     * local upper-panel changes return to the same resting scene. */
    int passage_enabled, passage_cycles, passage_inside, passage_moved, passage_observed;
    uint8_t *passage_base, *passage_candidate, *passage_previous;
    int passage_w, passage_h, passage_x, passage_y, passage_rw, passage_rh;
    double passage_last, passage_started, passage_quiet, passage_hold, passage_return;
    float passage_start_x, passage_start_y;
    int      auto_enabled;            /* config: door_auto_capture */
    double   auto_quiet_seconds;      /* config: door_auto_quiet_seconds (기본 20) */
    double   auto_open_hold_seconds;  /* config: door_auto_open_hold_seconds (기본 1.0) */
    float    auto_open_min_l1;        /* config: door_auto_open_l1 (기본 25) — 닫힘 기준 대비 ROI 평균 L1 */
    int      auto_phase;              /* DoorAutoPhase */
    double   auto_phase_since;        /* 현재 단계 진입 시각 — 정체 판정 기준 */
    /* 1 = 닫힘 기준 대기가 지나치게 길어 설정 점검이 필요합니다. 유리문이 대표적인 원인으로,
     * ROI 가 유리를 포함하면 너머의 바깥 움직임 때문에 정지 조건이 영원히 충족되지 않습니다.
     * 단계가 바뀌면 0 으로 돌아갑니다. main.c 가 0→1 전환을 한 번만 로그로 남깁니다. */
    int      auto_stalled;
    double   auto_quiet_since;        /* 정지·무인 조건 시작 시각, 0=미충족 */
    double   auto_open_since;         /* 열림 후보 조건 시작 시각, 0=미충족 */
    double   auto_wait_seconds;       /* 대시보드 표시용: 현재 조건 유지 시간 */
    /* ROI 를 8픽셀 간격으로 샘플한 루마 두 장 — 프레임 간 정지 판정용.
     * DoorMonitor 소유, door_destroy 가 free. ROI 크기가 바뀌면 재할당합니다. */
    uint8_t *auto_roi_prev;
    uint8_t *auto_roi_cur;
    int      auto_roi_w, auto_roi_h;
    int      auto_roi_valid;          /* prev 가 직전 프레임 값인지 */
} DoorMonitor;

/* 닫힘/열림 기준 파일을 각각 로드합니다.
 * 파일이 없으면 해당 기준만 NULL로 두고 0을 반환합니다(에러 아님).
 * 이미 로드된 버퍼가 있으면 먼저 해제합니다. */
int  door_load(DoorMonitor *d,
               const char *closed_path,
               const char *open_path);

void door_destroy(DoorMonitor *d);

/*
 * 현재 프레임과 기준 이미지 비교.
 *
 * persons: 활성 트랙 bbox(원본 좌표). 호출자 소유이며 읽기만 하고 보관하지 않습니다.
 *          NULL/0 이면 가림 판정 없이 항상 전체 ROI 로 비교합니다(기존 동작).
 *
 * 반환값: 0=닫힘, 1=열림, -1=기준 없음(enabled=0 또는 파일 미로드) 또는 가림 중 판정 보류.
 * state_changed: 이전 상태와 달라졌으면 1, 같으면 0.
 */
int  door_check(DoorMonitor *d,
                const uint8_t *rgb, int w, int h, int stride,
                const GrayRect *persons, int person_count,
                int *state_changed);

typedef enum {
    DOOR_AUTO_OFF         = 0, /* 비활성(door_enabled=0 또는 door_auto_capture=0) */
    DOOR_AUTO_NO_ROI      = 1, /* ROI 미지정 — 전체 프레임을 기준으로 삼는 것은 위험해 자동 캡처하지 않음 */
    DOOR_AUTO_WAIT_CLOSED = 2, /* 닫힘 기준 대기: 정지 + 무인 */
    DOOR_AUTO_WAIT_OPEN   = 3, /* 열림 기준 대기: 문만 달라진 순간 */
    DOOR_AUTO_DONE        = 4, /* 두 기준 모두 있음 */
    DOOR_AUTO_PASSAGE_BASE = 5,
    DOOR_AUTO_PASSAGE_WAIT = 6,
    DOOR_AUTO_PASSAGE_BLOCKED = 7,
    DOOR_AUTO_PASSAGE_RETURN = 8,
    DOOR_AUTO_PASSAGE_REPEAT = 9
} DoorAutoPhase;

/*
 * RGB 프레임을 [int32 w][int32 h][w*h*3] raw 로 저장합니다. stream.c 의 수동 캡처와
 * 같은 형식이며 residue 청결 기준도 이 형식을 씁니다. stride 가 w*3 이 아니어도
 * 행 단위로 복사해 압축 저장합니다. 반환 0=성공, -1=실패.
 */
int raw_rgb_save(const char *path, const uint8_t *rgb, int w, int h, int stride);

/*
 * 자동 기준 캡처 한 스텝. door_check 와 같은 위치(그리기 전, 실제 픽셀)에서 매 프레임 호출합니다.
 *
 * persons: 활성 트랙 bbox(원본 좌표). 호출자 소유, 읽기만 하며 보관하지 않습니다.
 * closed_path/open_path: 저장 경로. NULL 이면 파일은 쓰지 않고 메모리에만 설치합니다(테스트용).
 * 저장에 성공하면 해당 기준을 d->ref_*_rgb 에 곧바로 설치하므로 호출자는 파일을
 * 다시 읽을 필요가 없습니다(mtime 캐시만 맞추면 됩니다).
 *
 * 반환: 0=변화 없음, 1=닫힘 기준 저장, 2=열림 기준 저장, 3=통과 학습 기준 쌍 저장, -1=저장 실패.
 */
int door_auto_update(DoorMonitor *d,
                     const uint8_t *rgb, int w, int h, int stride,
                     const GrayRect *persons, int person_count,
                     int camera_ok, double now,
                     const char *closed_path, const char *open_path);

#endif /* DOOR_H */
