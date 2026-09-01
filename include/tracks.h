#ifndef TRACKS_H
#define TRACKS_H

#include "yolo11.h"

#include <stddef.h>

/*
 * LightTracker는 박스를 이동시키지만 개체별 ID가 없어 체류 시간 누적이 불가능합니다.
 * TrackList는 IoU 그리디 매칭으로 프레임 간 안정적 ID를 부여하고 체류 시간을 누적합니다.
 */

typedef enum { TRACK_UNORDERED = 0, TRACK_ORDERED = 1 } OrderState;

typedef struct {
    int        id;
    int        active;
    int        misses;         /* 연속으로 매칭 실패한 프레임 수 */
    double     first_seen;     /* platform_monotonic_seconds() 기준 */
    double     last_seen;
    double     dwell_seconds;
    OrderState order;
    Detection  box;            /* 가장 최근 매칭된 박스 (keypoint 포함) */
} Track;

typedef struct {
    Track  *items;     /* TrackList 소유, tracks_destroy 에서 free */
    size_t  count;     /* 슬롯 수 (active + inactive 모두 포함) */
    size_t  capacity;
    int     next_id;
    float   iou_threshold;
    int     max_misses;
} TrackList;

int  tracks_init(TrackList *tl, size_t capacity, float iou_threshold,
                 int max_misses, char *error, size_t error_size);
void tracks_destroy(TrackList *tl);

/* detections 와 기존 트랙을 IoU 그리디 매칭으로 연결·갱신합니다. */
void tracks_update(TrackList *tl, const DetectionList *detections, double now);

/*
 * track_id 에 해당하는 트랙을 ORDERED 로 전환합니다.
 * 향후 결제 DB 연동 시 이 함수를 호출합니다 (현재는 ROI 프록시로 대체).
 */
void tracks_mark_ordered(TrackList *tl, int track_id);

/*
 * RGB 버퍼에 모든 활성 트랙을 그립니다. draw_detections 의 TrackList 버전입니다.
 * misses==0인 트랙(현재 감지됨)은 녹색 박스 + "#ID PERSON N%",
 * misses>0인 트랙(프레임 밖, 만료 전)은 주황색 박스 + "#ID MISS" 레이블로 표시합니다.
 */
void draw_tracks(uint8_t *rgb, int width, int height, int stride,
                 const TrackList *tracks);

#endif /* TRACKS_H */
