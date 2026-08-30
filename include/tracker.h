#ifndef TRACKER_H
#define TRACKER_H

#include "yolo11.h"

#include <stddef.h>
#include <stdint.h>

typedef struct LightTracker LightTracker;

typedef struct {
    int downsample;
    int search_radius;
    int patch_radius;
    int motion_threshold;
} TrackerOptions;

LightTracker *tracker_create(const TrackerOptions *options);
void tracker_destroy(LightTracker *tracker);

/*
 * 이번 프레임의 luma(Y) 평면을 알려 줍니다.
 *
 * 설정되어 있으면 추적기가 내부 그레이스케일을 RGB에서 계산하는 대신 이
 * 평면에서 곧바로 샘플링합니다. 추적기는 SAD 비교만 하므로 색 정보가
 * 필요 없고, 디코더가 이미 luma를 갖고 있는데 RGB로 바꿨다가 다시
 * 되돌리는 것은 같은 값을 두 번 만드는 일이었습니다.
 *
 * luma: 디코더 소유 평면을 차용만 합니다. 추적기는 보관하지 않지만,
 *       포인터는 다음 tracker_reset/tracker_update 호출이 끝날 때까지
 *       유효해야 합니다. 프레임마다 다시 설정하세요.
 * NULL을 넘기면 RGB에서 계산하는 기존 경로로 돌아갑니다.
 */
void tracker_set_luma(LightTracker *tracker, const uint8_t *luma, int stride);

/* A full detector result becomes the reference for the following frames. */
int tracker_reset(LightTracker *tracker, const uint8_t *rgb, int width,
                  int height, int stride, char *error, size_t error_size);

/* Updates boxes in place and asks for an early detector refresh when unsure. */
int tracker_update(LightTracker *tracker, const uint8_t *rgb, int width,
                   int height, int stride, DetectionList *detections,
                   int *request_detection, char *error, size_t error_size);

#endif
