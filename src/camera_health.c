#include "camera_health.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 앞 7개는 이전 값 그대로, 뒤 2개(recover_hold_frames, recover_margin)가 복구 히스테리시스(camera_health.h) */
static const CameraHealthConfig DEFAULT_CONFIG = {240, 12, 150, 5, 8, 0.045f, 300, 30, 4};

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

void camera_health_default_config(CameraHealthConfig *out) {
    if (out) *out = DEFAULT_CONFIG;
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
        case CAM_LOW_CONTRAST: return "low_contrast";
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

    /* 선명도 — 밝기가 정상 범위일 때만 봅니다. 백화·암흑은 위에서 따로 잡습니다. */
    if (h->config.low_contrast_ratio > 0.0f && stats->grad_count > 0 &&
        h->anomaly_streak == 0 && luma_avg > 0) {
        double grad = (double)stats->grad_sum / (double)stats->grad_count;
        if (grad / (double)luma_avg < (double)h->config.low_contrast_ratio)
            h->low_contrast_streak++;
        else
            h->low_contrast_streak = 0;
    } else {
        h->low_contrast_streak = 0;
    }

    /* hold_frames 이상 지속 시 상태 전환 (히스테리시스) */
    if (h->frozen_streak >= h->config.frozen_frames_threshold) {
        new_state = CAM_FROZEN;
    } else if (h->anomaly_streak >= h->config.anomaly_hold_frames) {
        if ((int)luma_avg > h->config.luma_white_threshold)
            new_state = CAM_WHITEOUT;
        else
            new_state = CAM_BLACKOUT;
    } else if (h->config.low_contrast_hold_frames > 0 &&
               h->low_contrast_streak >= h->config.low_contrast_hold_frames) {
        new_state = CAM_LOW_CONTRAST;
    } else if (h->anomaly_streak == 0 && h->frozen_streak == 0 &&
               h->low_contrast_streak == 0) {
        /* 연속 이상 없음 → 복구 */
        new_state = CAM_OK;
    } else {
        new_state = h->state; /* 아직 hold 미달, 상태 유지 */
    }

    /* 복구 히스테리시스: 정상 프레임이 연속으로 충분히 이어지고(밝기 여유 포함) 나서야 OK 로 돌아간다. */
    if (old_state != CAM_OK && new_state == CAM_OK) {
        int clear_of_band = 1;
        if (h->config.recover_margin > 0) {
            if (old_state == CAM_BLACKOUT && (int)luma_avg < h->config.luma_black_threshold + h->config.recover_margin)
                clear_of_band = 0;
            if (old_state == CAM_WHITEOUT && (int)luma_avg > h->config.luma_white_threshold - h->config.recover_margin)
                clear_of_band = 0;
        }
        if (clear_of_band) h->recover_streak++;
        else h->recover_streak = 0;
        if (!clear_of_band || h->recover_streak < h->config.recover_hold_frames)
            new_state = old_state;
    } else {
        h->recover_streak = 0;
    }

    h->state  = new_state;
    *state_out = new_state;
    return new_state != old_state ? 1 : 0;
}
