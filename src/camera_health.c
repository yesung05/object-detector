#include "camera_health.h"

#include <stdlib.h>
#include <string.h>

static const CameraHealthConfig DEFAULT_CONFIG = {240, 12, 150, 5, 8};

int camera_health_init(CameraHealth *h, int gray_width, int gray_height,
                       const CameraHealthConfig *config,
                       char *error, size_t error_size) {
    int size;
    uint8_t *buf;
    if (!h || gray_width <= 0 || gray_height <= 0) {
        if (error) snprintf(error, error_size,
                            "camera_health_init: invalid arguments");
        return -1;
    }
    size = gray_width * gray_height;
    buf = (uint8_t *)calloc((size_t)size, 1);
    if (!buf) {
        if (error) snprintf(error, error_size,
                            "camera_health_init: out of memory");
        return -1;
    }
    memset(h, 0, sizeof(*h));
    h->config    = config ? *config : DEFAULT_CONFIG;
    h->prev_gray = buf;
    h->gray_size = size;
    h->state     = CAM_OK;
    return 0;
}

void camera_health_destroy(CameraHealth *h) {
    if (!h) return;
    free(h->prev_gray);
    h->prev_gray = NULL;
}

const char *cam_state_name(CamState s) {
    switch (s) {
        case CAM_OK:       return "ok";
        case CAM_WHITEOUT: return "whiteout";
        case CAM_BLACKOUT: return "blackout";
        case CAM_FROZEN:   return "frozen";
        default:           return "unknown";
    }
}

int camera_health_update(CameraHealth *h, const GrayBuf *g, CamState *state_out) {
    int n, i, changed, luma_sum;
    unsigned long luma_avg;
    CamState new_state;
    CamState old_state;

    if (!h || !g || !g->data || !state_out) {
        if (state_out) *state_out = CAM_OK;
        return 0;
    }
    old_state = h->state;
    n = g->width * g->height;
    if (n > h->gray_size) n = h->gray_size;

    /* 평균 luma 계산 */
    luma_sum = 0;
    for (i = 0; i < n; ++i) luma_sum += g->data[i];
    luma_avg = n > 0 ? (unsigned long)luma_sum / (unsigned long)n : 128u;

    /* 변화 픽셀 수 (freeze 감지) */
    changed = 0;
    for (i = 0; i < n; ++i) {
        int diff = (int)g->data[i] - (int)h->prev_gray[i];
        if (diff < 0) diff = -diff;
        if (diff >= h->config.motion_threshold) changed++;
    }

    /* prev_gray 갱신 */
    memcpy(h->prev_gray, g->data, (size_t)n);

    /* 이상 조건 판정 */
    if ((int)luma_avg > h->config.luma_white_threshold) {
        h->anomaly_streak++;
        h->frozen_streak = 0;
    } else if ((int)luma_avg < h->config.luma_black_threshold) {
        h->anomaly_streak++;
        h->frozen_streak = 0;
    } else if (changed == 0 || (n > 0 && changed * 1000 / n < 2)) {
        /* 변화 픽셀 비율 0.2% 미만 → freeze 후보 */
        h->frozen_streak++;
        h->anomaly_streak = 0;
    } else {
        h->anomaly_streak = 0;
        h->frozen_streak  = 0;
    }

    /* hold_frames 이상 지속 시 상태 전환 (히스테리시스) */
    if (h->frozen_streak >= h->config.frozen_frames_threshold) {
        new_state = CAM_FROZEN;
    } else if (h->anomaly_streak >= h->config.anomaly_hold_frames) {
        if ((int)luma_avg > h->config.luma_white_threshold)
            new_state = CAM_WHITEOUT;
        else
            new_state = CAM_BLACKOUT;
    } else if (h->anomaly_streak == 0 && h->frozen_streak == 0) {
        /* 연속 이상 없음 → 복구 */
        new_state = CAM_OK;
    } else {
        new_state = h->state; /* 아직 hold 미달, 상태 유지 */
    }

    h->state  = new_state;
    *state_out = new_state;
    return new_state != old_state ? 1 : 0;
}
