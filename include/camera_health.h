#ifndef CAMERA_HEALTH_H
#define CAMERA_HEALTH_H

#include "gray.h"

#include <stddef.h>
#include <stdint.h>

typedef enum {
    CAM_OK       = 0,
    CAM_WHITEOUT = 1,   /* 평균 luma > white_threshold 가 N프레임 지속 */
    CAM_BLACKOUT = 2,   /* 평균 luma < black_threshold 가 N프레임 지속 */
    CAM_FROZEN   = 3    /* 변화 픽셀 비율 ≈ 0 이 frozen_threshold 프레임 지속 */
} CamState;

typedef struct {
    int luma_white_threshold;    /* 기본 240 */
    int luma_black_threshold;    /* 기본 12  */
    int frozen_frames_threshold; /* 기본 150 */
    int anomaly_hold_frames;     /* 이 프레임 수 이상 연속이어야 상태 전환 (기본 5) */
    int motion_threshold;        /* 픽셀 변화 임계값 (기본 8) */
} CameraHealthConfig;

/*
 * 픽셀을 직접 훑지 않습니다. 호출자가 gray_analyze()로 한 번에 계산한
 * GrayStats를 넘겨주면 임계값 판정과 히스테리시스만 담당합니다.
 *
 * 예전에는 이 모듈이 그레이 버퍼를 두 번 순회하고 이전 프레임 사본까지
 * 따로 들고 있었습니다. 모션 게이트가 같은 데이터를 또 훑고 또 한 벌
 * 들고 있었으므로, 같은 일을 두 곳에서 중복하고 있었습니다.
 */
typedef struct {
    CameraHealthConfig config;
    int      anomaly_streak; /* 현재 이상 조건 연속 프레임 수 */
    int      frozen_streak;  /* 변화 없는 프레임 연속 수 */
    CamState state;
} CameraHealth;

/* config == NULL 이면 기본값을 사용합니다. 버퍼를 소유하지 않으므로
 * 실패할 일이 없지만, 호출부 형태를 유지하기 위해 오류 인자를 남겨 둡니다. */
int  camera_health_init(CameraHealth *h, const CameraHealthConfig *config,
                        char *error, size_t error_size);
void camera_health_destroy(CameraHealth *h);

/*
 * 상태가 바뀌면 1, 유지면 0을 반환합니다.
 * state_out 에 새 상태를 씁니다.
 * 진입·복구 양쪽 모두 호출자가 로그를 남길 수 있도록 항상 state_out 을 갱신합니다.
 *
 * stats: 이번 프레임의 gray_analyze() 결과. 호출자 소유이며 읽기만 합니다.
 */
int  camera_health_update(CameraHealth *h, const GrayStats *stats,
                          CamState *state_out);
const char *cam_state_name(CamState s);

#endif /* CAMERA_HEALTH_H */
