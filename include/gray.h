#ifndef GRAY_H
#define GRAY_H

#include <stddef.h>
#include <stdint.h>

/*
 * 프레임을 다운샘플한 그레이스케일 공유 버퍼입니다.
 * camera_health와 모션 게이트가 같은 버퍼를 재사용하여 프레임당 한 번만 계산합니다.
 */
typedef struct {
    uint8_t *data;  /* GrayBuf 소유, gray_buf_destroy 에서 free */
    int width;
    int height;
    int downsample; /* 원본 대비 축소 비율. 8이면 1/8 크기. */
} GrayBuf;

/*
 * source_width/height: 원본 프레임 크기.
 * downsample: 축소 비율. 1이면 1:1, 8이면 각 축 1/8.
 */
int  gray_buf_init(GrayBuf *g, int source_width, int source_height, int downsample);
void gray_buf_destroy(GrayBuf *g);

/*
 * RGB 프레임 한 장을 g->data 에 다운샘플+그레이스케일로 변환합니다.
 *
 * 입력에 luma 평면이 없을 때만 쓰는 폴백 경로입니다. 픽셀마다 곱셈 3회와
 * 시프트가 필요하고, RGB는 픽셀당 3바이트라 다운샘플 접근이 캐시를 낭비합니다.
 * 가능하면 gray_buf_update_luma()를 쓰세요.
 */
void gray_buf_update(GrayBuf *g, const uint8_t *rgb,
                     int width, int height, int stride);

/*
 * 디코더가 준 luma(Y) 평면에서 곧바로 다운샘플합니다.
 *
 * 이 경로가 존재하는 이유: 디코더 출력이 YUV420P 계열이면 Y 평면이 이미
 * 우리가 원하는 그레이스케일입니다. 예전에는 sws_scale로 RGB를 만든 뒤
 * 그 RGB에서 luma를 다시 계산했습니다. 같은 값을 두 번 만든 셈이고,
 * 그것도 gray/tracker/camera_health가 각각 반복했습니다.
 *
 * luma: 호출자(디코더)가 소유한 평면을 읽기만 합니다. 보관하지 않습니다.
 * stride: 평면의 한 줄 바이트 수(FFmpeg linesize[0]). width보다 클 수 있습니다.
 */
void gray_buf_update_luma(GrayBuf *g, const uint8_t *luma,
                          int width, int height, int stride);

/*
 * 한 프레임의 통계를 단일 순회로 모두 계산합니다.
 *
 * 예전에는 같은 그레이 버퍼를 세 번 훑었습니다 — 평균 luma(카메라 헬스),
 * 변화 픽셀 수(카메라 헬스), 변화 픽셀 수(모션 게이트). 임계값만 다를 뿐
 * 읽는 데이터는 완전히 같았고, 이전 프레임 사본도 두 벌 유지했습니다.
 *
 * 임계값 비교 방향이 둘로 나뉘는 것은 기존 동작을 그대로 보존하기 위함입니다.
 *   모션 게이트: |diff| >  motion_gt
 *   카메라 헬스: |diff| >= health_ge
 *
 * prev: 이전 프레임의 그레이 버퍼. 호출자 소유이며 읽기만 합니다.
 *       NULL이면 변화량 항목은 0으로 두고 luma 합계만 계산합니다.
 */
typedef struct {
    unsigned long luma_sum;       /* cur 전체 luma 합 */
    int           pixels;         /* 순회한 픽셀 수 */
    size_t        changed_motion; /* |diff| >  motion_gt */
    size_t        changed_health; /* |diff| >= health_ge */
} GrayStats;

void gray_analyze(const GrayBuf *cur, const uint8_t *prev,
                  int motion_gt, int health_ge, GrayStats *out);

#endif /* GRAY_H */
