#ifndef GRAY_H
#define GRAY_H

#include <stdint.h>

/*
 * RGB 프레임을 다운샘플+그레이스케일로 변환한 공유 버퍼입니다.
 * camera_health와 모션 게이트가 같은 버퍼를 재사용하여 프레임당 한 번만 계산합니다.
 */
typedef struct {
    uint8_t *data;  /* GrayBuf 소유, gray_buf_destroy 에서 free */
    int width;
    int height;
    int downsample; /* 원본 대비 축소 비율. 4이면 1/4 크기. */
} GrayBuf;

/*
 * source_width/height: 원본 RGB 프레임 크기.
 * downsample: 축소 비율. 1이면 1:1, 4이면 각 축 1/4.
 */
int  gray_buf_init(GrayBuf *g, int source_width, int source_height, int downsample);
void gray_buf_destroy(GrayBuf *g);

/* RGB 프레임 한 장을 g->data 에 다운샘플+그레이스케일로 변환합니다. */
void gray_buf_update(GrayBuf *g, const uint8_t *rgb,
                     int width, int height, int stride);

#endif /* GRAY_H */
