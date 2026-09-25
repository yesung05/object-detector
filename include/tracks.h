#ifndef TRACKS_H
#define TRACKS_H

#include "yolo11.h"

#include <stddef.h>
#include <stdint.h>

/*
 * LightTracker는 박스를 이동시키지만 개체별 ID가 없어 체류 시간 누적이 불가능합니다.
 * TrackList는 IoU 그리디 매칭으로 프레임 간 안정적 ID를 부여하고 체류 시간을 누적합니다.
 *
 * 외관 기반 재식별(Re-ID):
 *   IoU 매칭 실패 시, bbox 상체 영역에서 추출한 HSV 히스토그램으로
 *   limbo 상태(active=0, 아직 limbo_expired_at 이전)의 트랙과 비교합니다.
 *   히스토그램 교차값이 appear_threshold 이상이면 동일 인물로 판단해 트랙을 부활시킵니다.
 *   이를 통해 카메라 프레임 밖으로 나갔다가 다른 위치에서 재진입한 경우에도 ID가 유지됩니다.
 */

/* H(색상) 16빈 + S(채도) 16빈. V는 조명 변화에 취약해 제외합니다. */
#define APPEAR_HIST_BINS    32
/* 머리 패치 크기. SAD 템플릿 매칭으로 YOLO가 놓쳤을 때 머리를 찾습니다. */
#define HEAD_PATCH_SIZE     24

typedef enum { TRACK_UNORDERED = 0, TRACK_ORDERED = 1 } OrderState;

typedef struct {
    int        id;
    int        active;
    int        misses;              /* 연속으로 매칭 실패한 프레임 수 */
    double     first_seen;          /* platform_monotonic_seconds() 기준 */
    double     last_seen;
    double     dwell_seconds;
    /* active=0이 된 시점에 (now + limbo_seconds)로 설정됩니다.
     * 이 시각 이후에는 슬롯을 재사용할 수 있습니다. */
    double     limbo_expired_at;
    OrderState order;
    Detection  box;                 /* 가장 최근 매칭된 박스 (keypoint 포함) */
    /* 상체 HSV 히스토그램. EMA(α=0.15)로 누적 갱신됩니다. */
    float      appear_hist[APPEAR_HIST_BINS];
    int        appear_valid;        /* 히스토그램이 한 번이라도 계산됐으면 1 */

    /* ── 머리 위치 추적 (쓰러짐 감지 보조) ──────────────────────────
     * YOLO 키포인트에서 코(kp[0]) 좌표를 기준으로 24x24 그레이 패치를 저장합니다.
     * YOLO가 누운 자세를 놓쳤을 때 SAD 템플릿 매칭으로 머리 위치를 추적합니다.
     * head_y_baseline_norm: 서있을 때 head_cy_norm 값. -1이면 미설정. */
    uint8_t    head_patch[HEAD_PATCH_SIZE * HEAD_PATCH_SIZE]; /* 소유: 트랙 수명과 동일 */
    int        head_cx, head_cy;        /* 현재 머리 중심 (픽셀, 원본 해상도) */
    float      head_cy_norm;            /* head_cy / img_h, 0..1 */
    float      head_y_baseline_norm;       /* 서있을 때 head_cy_norm (-1=미설정) */
    float      head_y_fall_threshold_norm; /* baseline + bbox_h*0.7/img_h. 이 이상이면 낙하 */
    float      head_y_prev_norm;           /* 직전 갱신 시 head_cy_norm (속도 계산용) */
    double     head_y_prev_time;           /* 직전 갱신 시각 */
    int        head_valid;                 /* 패치가 한 번이라도 캡처됐으면 1 */
    int        fall_sudden;               /* 머리 낙하 속도 임계 초과 시 1 (즉시 감지) */

    /* ── 미확인 소실 래치 ────────────────────────────────────────────────
     * rules.c 가 아니라 Track 에 두는 이유: rules.c 는 active=0 이 되는 즉시
     * 트랙별 상태 슬롯을 반납하는데, 소실 판정이 필요한 시점이 바로 그때부터입니다.
     * Track 슬롯은 limbo 동안 유지되므로 래치가 살아남습니다.
     * 슬롯 재사용 시 tracks.c 가 memset 으로 0 초기화하므로 이전 사람의 상태가
     * 새 사람에게 새지 않습니다. */
    int        vanish_warned;              /* person_unaccounted 발화함 */
    int        vanish_escalated;           /* person_unaccounted_residue 발화함 */

    /* 박스 중심 속도(px/s, EMA). 매칭 시 예상 위치에서 IoU 를 재기 위해 씁니다.
     * 새 트랙·Re-ID 부활 시 0 에서 시작하므로 첫 매칭은 기존과 똑같이 동작합니다. */
    float      vx, vy;
} Track;

typedef struct {
    Track  *items;          /* TrackList 소유, tracks_destroy 에서 free */
    size_t  count;          /* 슬롯 수 (active + limbo + expired 모두 포함) */
    size_t  capacity;
    int     next_id;
    float   iou_threshold;
    int     max_misses;
    /* active=0 이후 Re-ID 매칭을 시도할 최대 유지 시간 (초). 기본 1800 = 30분 */
    double  limbo_seconds;
    /* 히스토그램 교차값 임계치. 이 값 이상이면 동일 인물로 판단. */
    float   appear_threshold;
    /* 신규 트랙 생성 최소 신뢰도. 이미 추적 중인 트랙은 detector 임계값(더 낮음)으로 유지.
     * 0이면 비활성화 (detector 임계값과 동일하게 동작). */
    float   new_track_min_score;
} TrackList;

int  tracks_init(TrackList *tl, size_t capacity, float iou_threshold,
                 int max_misses, double limbo_seconds, float appear_threshold,
                 char *error, size_t error_size);
void tracks_destroy(TrackList *tl);

/*
 * detections 와 기존 트랙을 매칭합니다.
 * Phase 1: 활성 트랙 IoU 매칭
 * Phase 2: 미매칭 detection → limbo 트랙과 HSV 히스토그램 비교로 Re-ID
 * Phase 3: 여전히 미매칭이면 신규 트랙 생성
 *
 * rgb/img_w/img_h/img_stride: 현재 RGB 프레임. NULL이면 Re-ID를 건너뜁니다.
 */
void tracks_update(TrackList *tl, const DetectionList *detections,
                   const uint8_t *rgb, int img_w, int img_h, int img_stride,
                   double now);

/*
 * 2단계 매칭(ByteTrack 방식) 판입니다. tracks_update 는 low=NULL 로 이것을 부릅니다.
 *
 * high: 기준 신뢰도 이상 박스 — 기존과 같이 매칭·Re-ID·신규 생성에 씁니다.
 * low:  기준 미만이지만 검출기 하한 이상인 박스 — 1차에서 못 이어진 활성 트랙만
 *       잇는 데 씁니다. 새 트랙·Re-ID 에는 절대 쓰지 않습니다.
 *
 * 필요한 이유: 사람이 기둥·다른 손님에 반쯤 가리거나 쓰러져 자세가 바뀌면 신뢰도가
 * 0.1~0.2 로 떨어져 1차 기준에 못 미칩니다. 그대로 두면 misses 가 쌓여 트랙이 끊기고,
 * 다시 보일 때 새 ID 가 붙어 체류 시간·주문 상태가 처음부터 시작됩니다.
 * 낮은 박스로 트랙을 이어 갈 때는 위치만 갱신하고 score 는 직전 값을 유지합니다 —
 * 쓰러짐·소실 판정은 box.score 로 "흐릿한 탐지"를 거르므로, 낮은 박스가 그 판정을
 * 조용히 꺼 버리면 안 되기 때문입니다.
 */
void tracks_update_ex(TrackList *tl, const DetectionList *high,
                      const DetectionList *low,
                      const uint8_t *rgb, int img_w, int img_h, int img_stride,
                      double now);

/*
 * track_id 에 해당하는 트랙을 ORDERED 로 전환합니다.
 * 향후 결제 DB 연동 시 이 함수를 호출합니다 (현재는 ROI 프록시로 대체).
 */
void tracks_mark_ordered(TrackList *tl, int track_id);

/*
 * 카메라가 gap 초 동안 끊겼다 돌아온 뒤 호출합니다. 시간 기준 필드(last_seen,
 * limbo_expired_at, head_y_prev_time)를 gap 만큼 뒤로 미룹니다.
 *
 * 필요한 이유: 끊긴 동안 process_frame 이 불리지 않으므로, 돌아온 첫 판정에서
 * now - last_seen 이 끊긴 시간만큼 커집니다. 그대로 두면 자리에 있던 손님 전원이
 * 소실(person_unaccounted)로 판정되고 체류 시간에도 끊긴 시간이 더해집니다.
 * 트랙을 지우지 않는 이유: 손님의 ID·주문 상태·누적 체류를 카메라 끊김 때문에
 * 잃으면 초과 체류·미주문 판정이 처음부터 다시 시작됩니다.
 */
void tracks_shift_time(TrackList *tl, double gap);

/*
 * limbo_expired_at이 지난 만료 트랙 수를 반환합니다 (디버깅용 로그에 활용).
 */
int tracks_limbo_count(const TrackList *tl, double now);

/*
 * 머리 패치를 갱신하고 YOLO가 놓친 트랙의 머리 위치를 SAD 검색으로 추정합니다.
 * luma: 그레이스케일 프레임 (stride = img_w, 패딩 없음)
 * misses==0 트랙: kp[0](코) 유효 시 패치 캡처 + baseline 갱신
 * misses>0  트랙: SAD 검색으로 head_cx/head_cy/head_cy_norm 갱신
 */
/* luma_stride: 행 바이트 수 (보통 img_w, 패딩 없음)
 * now: platform_monotonic_seconds() — 낙하 속도 계산용 */
void tracks_update_heads(TrackList *tl, const uint8_t *luma,
                         int img_w, int img_h, int luma_stride, double now);

/*
 * RGB 버퍼에 모든 활성 트랙을 그립니다. draw_detections 의 TrackList 버전입니다.
 * misses==0인 트랙(현재 감지됨)은 녹색 박스 + "#ID PERSON N%",
 * misses>0인 트랙(프레임 밖, 만료 전)은 주황색 박스 + "#ID MISS" 레이블로 표시합니다.
 */
void draw_tracks(uint8_t *rgb, int width, int height, int stride,
                 const TrackList *tracks);

#endif /* TRACKS_H */
