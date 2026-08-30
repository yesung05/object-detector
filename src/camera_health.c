#include "camera_health.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const CameraHealthConfig DEFAULT_CONFIG = {240, 12, 150, 5, 8};

int camera_health_init(CameraHealth *h, const CameraHealthConfig *config,
                       char *error, size_t error_size) {
    if (!h) {
        if (error) snprintf(error, error_size,
                            "camera_health_init: invalid arguments");
        return -1;
    }
    memset(h, 0, sizeof(*h));
    h->config = config ? *config : DEFAULT_CONFIG;
    h->state  = CAM_OK;
    return 0;
}

void camera_health_destroy(CameraHealth *h) {
    /* 소유한 버퍼가 없습니다. 호출부의 정리 흐름을 유지하기 위해 남겨 둡니다. */
    (void)h;
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

int camera_health_update(CameraHealth *h, const GrayStats *stats,
                         CamState *state_out) {
    int n;
    unsigned long luma_avg;
    size_t changed;
    CamState new_state;
    CamState old_state;

    if (!h || !stats || !state_out) {
        if (state_out) *state_out = CAM_OK;
        return 0;
    }
    old_state = h->state;
    n = stats->pixels;
    changed = stats->changed_health;

    luma_avg = n > 0 ? stats->luma_sum / (unsigned long)n : 128u;

    /* 이상 조건 판정 */
    if ((int)luma_avg > h->config.luma_white_threshold) {
        h->anomaly_streak++;
        h->frozen_streak = 0;
    } else if ((int)luma_avg < h->config.luma_black_threshold) {
        h->anomaly_streak++;
        h->frozen_streak = 0;
    } else if (changed == 0 ||
               (n > 0 && changed * 1000 / (size_t)n < 2)) {
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
