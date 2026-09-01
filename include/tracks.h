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
 * track_id 에 해당하는 트랙을 ORDERED 로 전환합니다.
 * 향후 결제 DB 연동 시 이 함수를 호출합니다 (현재는 ROI 프록시로 대체).
 */
void tracks_mark_ordered(TrackList *tl, int track_id);

/*
 * limbo_expired_at이 지난 만료 트랙 수를 반환합니다 (디버깅용 로그에 활용).
 */
int tracks_limbo_count(const TrackList *tl, double now);

/*
 * RGB 버퍼에 모든 활성 트랙을 그립니다. draw_detections 의 TrackList 버전입니다.
 * misses==0인 트랙(현재 감지됨)은 녹색 박스 + "#ID PERSON N%",
 * misses>0인 트랙(프레임 밖, 만료 전)은 주황색 박스 + "#ID MISS" 레이블로 표시합니다.
 */
void draw_tracks(uint8_t *rgb, int width, int height, int stride,
                 const TrackList *tracks);

#endif /* TRACKS_H */
