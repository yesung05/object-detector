#include "door.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RAW 파일 하나를 읽어 버퍼에 채웁니다.
 * 파일 없음 → *out_rgb=NULL, 반환 0 (에러 아님)
 * 형식 오류 또는 malloc 실패 → 반환 -1 */
static int load_raw(const char *path,
                    uint8_t **out_rgb, int *out_w, int *out_h) {
    *out_rgb = NULL;
    *out_w   = 0;
    *out_h   = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0; /* 파일 없음 = 아직 미설정, 정상 */

    int w = 0, h = 0;
    if (fread(&w, sizeof(int), 1, f) != 1 ||
        fread(&h, sizeof(int), 1, f) != 1 ||
        w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        fclose(f);
        return 0; /* 손상된 파일 — 조용히 무시 */
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)w * h * 3);
    if (!buf) { fclose(f); return -1; }

    size_t got = fread(buf, 1, (size_t)w * h * 3, f);
    fclose(f);

    if (got != (size_t)w * h * 3) { free(buf); return 0; }

    *out_rgb = buf; /* 호출자(door_destroy)가 해제 */
    *out_w   = w;
    *out_h   = h;
    return 0;
}

int door_load(DoorMonitor *d,
              const char *closed_path,
              const char *open_path) {
    if (!d) return -1;

    /* 기존 버퍼 해제 */
    free(d->ref_closed_rgb);
    d->ref_closed_rgb = NULL;
    d->ref_closed_w   = 0;
    d->ref_closed_h   = 0;

    free(d->ref_open_rgb);
    d->ref_open_rgb = NULL;
    d->ref_open_w   = 0;
    d->ref_open_h   = 0;

    d->last_state        = -1;
    d->open_since        = -1.0; /* -1 = 현재 닫혀 있음 */
    d->open_event_fired  = 0;

    if (closed_path && load_raw(closed_path,
                                &d->ref_closed_rgb,
                                &d->ref_closed_w,
                                &d->ref_closed_h) != 0)
        return -1;

    if (open_path && load_raw(open_path,
                              &d->ref_open_rgb,
                              &d->ref_open_w,
                              &d->ref_open_h) != 0)
        return -1;

    return 0;
}

void door_destroy(DoorMonitor *d) {
    if (!d) return;
    free(d->ref_closed_rgb);
    d->ref_closed_rgb = NULL;
    free(d->ref_open_rgb);
    d->ref_open_rgb = NULL;
}

/* ROI 안의 두 RGB 버퍼 간 평균 L1 거리(채널 평균)를 반환합니다.
 * 조명 변화에 강인하도록 채널 최대값 대신 채널 평균을 사용합니다.
 * ref는 w*3 stride, cur는 stride 바이트 간격입니다. */
static double avg_l1(const uint8_t *ref, const uint8_t *cur,
                     int w, int stride,
                     int x0, int y0, int x1, int y1) {
    double sum = 0.0;
    long   n   = 0;
    for (int y = y0; y < y1; y++) {
        const uint8_t *cr = cur + (size_t)y * stride;
        const uint8_t *rr = ref + (size_t)y * w * 3;
        for (int x = x0; x < x1; x++) {
            int dr = (int)cr[x*3+0] - (int)rr[x*3+0];
            int dg = (int)cr[x*3+1] - (int)rr[x*3+1];
            int db = (int)cr[x*3+2] - (int)rr[x*3+2];
            if (dr < 0) dr = -dr;
            if (dg < 0) dg = -dg;
            if (db < 0) db = -db;
            sum += (dr + dg + db) / 3.0;
            n++;
        }
    }
    return n > 0 ? sum / n : 255.0;
}

int door_check(DoorMonitor *d,
               const uint8_t *rgb, int w, int h, int stride,
               int *state_changed) {
    if (state_changed) *state_changed = 0;
    if (!d || !d->enabled) return -1;

    int have_closed = (d->ref_closed_rgb != NULL);
    int have_open   = (d->ref_open_rgb   != NULL);
    if (!have_closed && !have_open) return -1;

    /* ROI 범위 계산 */
    int x0 = d->roi_w > 0 ? d->roi_x : 0;
    int y0 = d->roi_h > 0 ? d->roi_y : 0;
    int x1 = d->roi_w > 0 ? (d->roi_x + d->roi_w) : w;
    int y1 = d->roi_h > 0 ? (d->roi_y + d->roi_h) : h;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w; if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return -1;

    int state;

    if (have_closed && have_open) {
        /* 두 기준 모두 있을 때: 현재 프레임이 어느 쪽에 더 가까운지 비교.
         * 기준 이미지 해상도가 현재 프레임과 달라지면 재캡처가 필요합니다. */
        if (d->ref_closed_w != w || d->ref_closed_h != h) return -1;
        if (d->ref_open_w   != w || d->ref_open_h   != h) return -1;

        double dist_c = avg_l1(d->ref_closed_rgb, rgb, w, stride, x0, y0, x1, y1);
        double dist_o = avg_l1(d->ref_open_rgb,   rgb, w, stride, x0, y0, x1, y1);
        state = (dist_o < dist_c) ? 1 : 0;

    } else if (have_closed) {
        /* 닫힌 기준만 있을 때: 기존 방식 — 변화 픽셀 비율 vs diff_threshold */
        if (d->ref_closed_w != w || d->ref_closed_h != h) return -1;

        size_t changed = 0;
        size_t total   = (size_t)(x1 - x0) * (size_t)(y1 - y0);

        for (int y = y0; y < y1; y++) {
            const uint8_t *cr = rgb               + (size_t)y * stride;
            const uint8_t *rr = d->ref_closed_rgb + (size_t)y * w * 3;
            for (int x = x0; x < x1; x++) {
                int dr = (int)cr[x*3+0] - (int)rr[x*3+0];
                int dg = (int)cr[x*3+1] - (int)rr[x*3+1];
                int db = (int)cr[x*3+2] - (int)rr[x*3+2];
                /* 채널 최대 차이가 30 이상이면 변화로 판정
                 * 임계값 30은 조명 미세 변화(노이즈)를 무시하기 위한 값 */
                int diff = dr < 0 ? -dr : dr;
                int dg_abs = dg < 0 ? -dg : dg;
                int db_abs = db < 0 ? -db : db;
                if (dg_abs > diff) diff = dg_abs;
                if (db_abs > diff) diff = db_abs;
                if (diff > 30) changed++;
            }
        }

        float ratio = total > 0 ? (float)changed / (float)total : 0.0f;
        state = (ratio >= d->diff_threshold) ? 1 : 0;

    } else {
        /* 열린 기준만 있을 때: 열린 기준과의 유사도로 판정 */
        if (d->ref_open_w != w || d->ref_open_h != h) return -1;

        size_t changed = 0;
        size_t total   = (size_t)(x1 - x0) * (size_t)(y1 - y0);

        for (int y = y0; y < y1; y++) {
            const uint8_t *cr = rgb             + (size_t)y * stride;
            const uint8_t *rr = d->ref_open_rgb + (size_t)y * w * 3;
            for (int x = x0; x < x1; x++) {
                int dr = (int)cr[x*3+0] - (int)rr[x*3+0];
                int dg = (int)cr[x*3+1] - (int)rr[x*3+1];
                int db = (int)cr[x*3+2] - (int)rr[x*3+2];
                int diff = dr < 0 ? -dr : dr;
                int dg_abs = dg < 0 ? -dg : dg;
                int db_abs = db < 0 ? -db : db;
                if (dg_abs > diff) diff = dg_abs;
                if (db_abs > diff) diff = db_abs;
                if (diff > 30) changed++;
            }
        }

        float ratio = total > 0 ? (float)changed / (float)total : 0.0f;
        state = (ratio < d->diff_threshold) ? 1 : 0; /* 열림 기준과 가까우면 열림 */
    }

    if (state_changed) {
        *state_changed = (d->last_state != state) ? 1 : 0;
    }
    d->last_state = state;
    return state;
}
