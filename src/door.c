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

int raw_rgb_save(const char *path, const uint8_t *rgb, int w, int h, int stride) {
    FILE *f;
    int y;
    if (!path || !rgb || w <= 0 || h <= 0) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(&w, sizeof(int), 1, f) != 1 || fwrite(&h, sizeof(int), 1, f) != 1) {
        fclose(f);
        return -1;
    }
    for (y = 0; y < h; y++) {
        if (fwrite(rgb + (size_t)y * stride, 1, (size_t)w * 3, f) != (size_t)w * 3) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
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
    d->candidate_state   = -1;
    d->candidate_frames  = 0;
    d->open_since        = -1.0; /* -1 = 현재 닫혀 있음 */
    d->open_event_fired  = 0;
    d->band_valid        = -1;   /* 기준이 바뀌었으므로 밴드를 다시 측정해야 합니다 */
    d->band_signal       = 0.0f;
    d->band_active       = 0;

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
    free(d->auto_roi_prev);
    d->auto_roi_prev = NULL;
    free(d->auto_roi_cur);
    d->auto_roi_cur = NULL;
    d->auto_roi_valid = 0;
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

/* ROI 상단 밴드의 아래쪽 경계. 최소 한 줄은 남겨 0 높이 비교를 막습니다. */
static int band_bottom(const DoorMonitor *d, int y0, int y1) {
    int b = y0 + (int)((float)(y1 - y0) * d->band_ratio + 0.5f);
    if (b <= y0) b = y0 + 1;
    if (b > y1)  b = y1;
    return b;
}

static int roi_occluded(const GrayRect *persons, int n, int x0, int y0, int x1, int y1) {
    int i;
    for (i = 0; i < n; ++i) {
        if (persons[i].x2 < (float)x0 || persons[i].x1 > (float)x1 ||
            persons[i].y2 < (float)y0 || persons[i].y1 > (float)y1) continue;
        return 1;
    }
    return 0;
}

/*
 * 밴드가 두 상태를 실제로 구분하는지 기준 두 장을 직접 비교해 한 번 측정합니다.
 *
 * 절대 임계가 아니라 전체 ROI 신호 대비 비율로 판정하는 이유: 대비가 낮은 문은 전체 신호
 * 자체가 작아서, 절대값으로 자르면 멀쩡하게 동작할 밴드까지 버립니다. 바닥값 8.0 은 두 기준이
 * 사실상 같은 사진인 경우(같은 상태에서 두 번 캡처한 실수)를 걸러내기 위한 것입니다.
 */
static void evaluate_band(DoorMonitor *d, int x0, int y0, int x1, int y1) {
    int rw = d->ref_closed_w, rstride = d->ref_closed_w * 3;
    int by1 = band_bottom(d, y0, y1);
    double full = avg_l1(d->ref_closed_rgb, d->ref_open_rgb, rw, rstride, x0, y0, x1, y1);
    double band = avg_l1(d->ref_closed_rgb, d->ref_open_rgb, rw, rstride, x0, y0, x1, by1);
    d->band_signal = (float)band;
    d->band_valid  = (band >= full * 0.4 && band >= 8.0) ? 1 : 0;
}

int door_check(DoorMonitor *d,
               const uint8_t *rgb, int w, int h, int stride,
               const GrayRect *persons, int person_count,
               int *state_changed) {
    if (state_changed) *state_changed = 0;
    if (!d) return -1;
    d->band_active = 0;
    if (!d->enabled) return -1;

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

    /* 밴드 유효성은 기준 두 장이 해상도까지 맞은 뒤 한 번만 측정합니다. */
    if (d->band_valid < 0 && d->band_ratio > 0.0f && have_closed && have_open &&
        d->ref_closed_w == w && d->ref_closed_h == h &&
        d->ref_open_w   == w && d->ref_open_h   == h)
        evaluate_band(d, x0, y0, x1, y1);

    /*
     * 사람이 ROI 를 가리면 전체 비교는 문이 아니라 사람을 재게 됩니다. 쓸 만한 밴드가 있으면
     * 머리 위 구간만으로 판정하고, 없으면 판정을 포기합니다 — 이 상황에서 억지로 내놓은 값은
     * 동전 던지기라, 문 앞에 서서 통화하는 손님 하나로 door_open 오탐이 납니다.
     */
    int cmp_y1 = y1;
    if (roi_occluded(persons, person_count, x0, y0, x1, y1)) {
        if (d->band_ratio > 0.0f && d->band_valid == 1) {
            cmp_y1 = band_bottom(d, y0, y1);
            d->band_active = 1;
        } else {
            return -1;
        }
    }

    int state;

    if (have_closed && have_open) {
        /* 두 기준 모두 있을 때: 현재 프레임이 어느 쪽에 더 가까운지 비교.
         * 기준 이미지 해상도가 현재 프레임과 달라지면 재캡처가 필요합니다. */
        if (d->ref_closed_w != w || d->ref_closed_h != h) return -1;
        if (d->ref_open_w   != w || d->ref_open_h   != h) return -1;

        double dist_c = avg_l1(d->ref_closed_rgb, rgb, w, stride, x0, y0, x1, cmp_y1);
        double dist_o = avg_l1(d->ref_open_rgb,   rgb, w, stride, x0, y0, x1, cmp_y1);
        state = (dist_o < dist_c) ? 1 : 0;

    } else if (have_closed) {
        /* 닫힌 기준만 있을 때: 기존 방식 — 변화 픽셀 비율 vs diff_threshold */
        if (d->ref_closed_w != w || d->ref_closed_h != h) return -1;

        size_t changed = 0;
        size_t total   = (size_t)(x1 - x0) * (size_t)(cmp_y1 - y0);

        for (int y = y0; y < cmp_y1; y++) {
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
        size_t total   = (size_t)(x1 - x0) * (size_t)(cmp_y1 - y0);

        for (int y = y0; y < cmp_y1; y++) {
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

    if (d->last_state < 0) {
        d->last_state = state;
        d->candidate_state = state;
        d->candidate_frames = 0;
        return state;
    }
    if (state == d->last_state) {
        d->candidate_state = state;
        d->candidate_frames = 0;
        return state;
    }
    if (d->candidate_state == state) d->candidate_frames++;
    else {
        d->candidate_state = state;
        d->candidate_frames = 1;
    }
    if (d->candidate_frames >= (d->confirm_frames > 0 ? d->confirm_frames : 5)) {
        d->last_state = state;
        d->candidate_frames = 0;
        if (state_changed) *state_changed = 1;
    }
    return d->last_state;
}

/* ── 자동 기준 캡처 ──────────────────────────────────────────────────────── */

/* 프레임을 압축(stride=w*3) 복사해 기준 슬롯에 설치합니다. 실패하면 0, 기존 기준은 유지. */
static int install_reference(uint8_t **slot, int *slot_w, int *slot_h,
                             const uint8_t *rgb, int w, int h, int stride) {
    uint8_t *copy = (uint8_t *)malloc((size_t)w * h * 3);
    int y;
    if (!copy) return 0;
    for (y = 0; y < h; y++)
        memcpy(copy + (size_t)y * w * 3, rgb + (size_t)y * stride, (size_t)w * 3);
    free(*slot);
    *slot   = copy; /* DoorMonitor 소유 — door_destroy 가 해제 */
    *slot_w = w;
    *slot_h = h;
    return 1;
}

/* 기준이 새로 설치되면 밴드 측정 결과가 무효가 되므로 다시 재게 합니다. */
static void invalidate_band(DoorMonitor *d) {
    d->band_valid  = -1;
    d->band_signal = 0.0f;
    d->band_active = 0;
}

/* ROI 를 step 간격으로 샘플한 루마를 out 에 채우고, prev 가 있으면 평균 절대차를 돌려줍니다.
 * 픽셀 전부를 보지 않는 이유: 정지 판정에는 격자 샘플로 충분하고, 1280×720 ROI 전체를
 * 매 프레임 비교하면 door_check 와 합쳐 부담이 두 배가 됩니다. */
static double sample_roi_luma(const uint8_t *rgb, int stride,
                              int x0, int y0, int rw, int rh, int step,
                              uint8_t *out, const uint8_t *prev) {
    int sx, sy, i = 0;
    long diff = 0;
    for (sy = 0; sy < rh; sy++) {
        const uint8_t *row = rgb + (size_t)(y0 + sy * step + step / 2) * stride;
        for (sx = 0; sx < rw; sx++, i++) {
            const uint8_t *p = row + (size_t)(x0 + sx * step + step / 2) * 3;
            uint8_t l = (uint8_t)((77u * p[0] + 150u * p[1] + 29u * p[2] + 128u) >> 8);
            if (prev) { int dd = (int)l - (int)prev[i]; diff += dd < 0 ? -dd : dd; }
            out[i] = l;
        }
    }
    return i > 0 ? (double)diff / i : 255.0;
}

/* 프레임 전체를 step 간격으로 훑은 기준 대비 평균 L1 — 조명 변화 가드용. */
static double sampled_frame_l1(const uint8_t *ref, const uint8_t *cur,
                               int w, int h, int stride, int step) {
    double sum = 0.0;
    long n = 0;
    int x, y;
    for (y = step / 2; y < h; y += step) {
        const uint8_t *cr = cur + (size_t)y * stride;
        const uint8_t *rr = ref + (size_t)y * w * 3;
        for (x = step / 2; x < w; x += step) {
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

static void auto_reset_wait(DoorMonitor *d) {
    d->auto_quiet_since  = 0.0;
    d->auto_open_since   = 0.0;
    d->auto_wait_seconds = 0.0;
    d->auto_roi_valid    = 0;
}

int door_auto_update(DoorMonitor *d,
                     const uint8_t *rgb, int w, int h, int stride,
                     const GrayRect *persons, int person_count,
                     int camera_ok, double now,
                     const char *closed_path, const char *open_path) {
    const int step = 8;
    int x0, y0, x1, y1, rw, rh, i, occupied = 0, stable;
    double roi_diff;

    if (!d || !rgb) return 0;
    if (!d->enabled || !d->auto_enabled) { d->auto_phase = DOOR_AUTO_OFF; return 0; }
    if (d->roi_w <= 0 || d->roi_h <= 0)  { d->auto_phase = DOOR_AUTO_NO_ROI; return 0; }

    if (d->ref_closed_rgb && d->ref_open_rgb) {
        d->auto_phase = DOOR_AUTO_DONE;
        d->auto_wait_seconds = 0.0;
        d->auto_stalled = 0;
        return 0;
    }
    {
        /* 단계가 바뀔 때만 기준 시각을 다시 잡습니다. 첫 호출은 auto_phase 가 OFF(0) 이므로
           이 비교만으로 걸립니다 — now==0.0 을 "미설정"으로 오인하는 별도 조건을 두지 않습니다. */
        int phase = d->ref_closed_rgb ? DOOR_AUTO_WAIT_OPEN : DOOR_AUTO_WAIT_CLOSED;
        if (phase != d->auto_phase) {
            d->auto_phase_since = now;
            d->auto_stalled = 0;
        }
        d->auto_phase = phase;
    }

    /*
     * 정체 판정 — 닫힘 단계에서만 합니다.
     *
     * 열림 단계는 손님이 문을 열 때까지 자연히 오래 걸리므로 오래 기다리는 것 자체가 정상입니다.
     * 반면 닫힘 단계는 "정지 + 무인"만 있으면 되므로, quiet 의 6배가 지나도록 누적 대기가 절반도
     * 못 찼다면 조건이 계속 깨지고 있다는 뜻입니다. 유리문(너머의 바깥 움직임)이나 ROI 안에
     * 상시 사람이 잡히는 배치가 원인이며, 둘 다 사람이 손봐야 풀립니다.
     */
    if (d->auto_phase == DOOR_AUTO_WAIT_CLOSED && !d->auto_stalled &&
        d->auto_quiet_seconds > 0.0 &&
        (now - d->auto_phase_since) > d->auto_quiet_seconds * 6.0 &&
        d->auto_wait_seconds < d->auto_quiet_seconds * 0.5)
        d->auto_stalled = 1;

    x0 = d->roi_x; y0 = d->roi_y;
    x1 = d->roi_x + d->roi_w; y1 = d->roi_y + d->roi_h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 - x0 < step || y1 - y0 < step) { d->auto_phase = DOOR_AUTO_NO_ROI; return 0; }

    /* 사람이 ROI 근처에 있으면 문 상태를 신뢰할 수 없습니다. ROI 를 사방 25% 넓혀
       문 앞에 서 있어 박스가 ROI 에 살짝 못 미치는 사람도 잡습니다. */
    {
        float mx = (float)(x1 - x0) * 0.25f, my = (float)(y1 - y0) * 0.25f;
        float ex0 = (float)x0 - mx, ey0 = (float)y0 - my;
        float ex1 = (float)x1 + mx, ey1 = (float)y1 + my;
        for (i = 0; i < person_count; i++) {
            if (persons[i].x2 < ex0 || persons[i].x1 > ex1 ||
                persons[i].y2 < ey0 || persons[i].y1 > ey1) continue;
            occupied = 1;
            break;
        }
    }
    if (occupied || !camera_ok) { auto_reset_wait(d); return 0; }

    /* 정지 판정용 샘플 버퍼 — ROI 크기가 바뀔 때만 재할당 */
    rw = (x1 - x0) / step;
    rh = (y1 - y0) / step;
    if (!d->auto_roi_prev || !d->auto_roi_cur || d->auto_roi_w != rw || d->auto_roi_h != rh) {
        free(d->auto_roi_prev);
        free(d->auto_roi_cur);
        d->auto_roi_prev = (uint8_t *)malloc((size_t)rw * rh);
        d->auto_roi_cur  = (uint8_t *)malloc((size_t)rw * rh);
        d->auto_roi_w = rw;
        d->auto_roi_h = rh;
        d->auto_roi_valid = 0;
        if (!d->auto_roi_prev || !d->auto_roi_cur) return 0;
    }
    roi_diff = sample_roi_luma(rgb, stride, x0, y0, rw, rh, step,
                               d->auto_roi_cur, d->auto_roi_valid ? d->auto_roi_prev : NULL);
    { uint8_t *t = d->auto_roi_prev; d->auto_roi_prev = d->auto_roi_cur; d->auto_roi_cur = t; }
    stable = d->auto_roi_valid && roi_diff < 3.0; /* 센서 노이즈는 보통 1~2 */
    d->auto_roi_valid = 1;

    if (d->auto_phase == DOOR_AUTO_WAIT_CLOSED) {
        if (!stable) { d->auto_quiet_since = 0.0; d->auto_wait_seconds = 0.0; return 0; }
        if (d->auto_quiet_since <= 0.0) d->auto_quiet_since = now;
        d->auto_wait_seconds = now - d->auto_quiet_since;
        if (d->auto_wait_seconds < d->auto_quiet_seconds) return 0;
        if (closed_path && raw_rgb_save(closed_path, rgb, w, h, stride) != 0) {
            d->auto_quiet_since = now; /* 디스크 오류로 매 프레임 재시도하지 않도록 quiet 시간 뒤 다시 */
            return -1;
        }
        if (!install_reference(&d->ref_closed_rgb, &d->ref_closed_w, &d->ref_closed_h,
                               rgb, w, h, stride))
            return -1;
        invalidate_band(d);
        d->last_state = -1; d->candidate_state = -1; d->candidate_frames = 0;
        auto_reset_wait(d);
        /* 단계 전환을 여기서 끝내 둡니다. 다음 호출까지 미루면 그 사이 /status 가 "정체"를
           보고해 방금 성공한 캡처와 모순됩니다. */
        d->auto_phase = DOOR_AUTO_WAIT_OPEN;
        d->auto_phase_since = now;
        d->auto_stalled = 0;
        return 1;
    }

    /* DOOR_AUTO_WAIT_OPEN */
    if (d->ref_closed_w != w || d->ref_closed_h != h) return 0; /* 해상도 불일치 — door_check 도 -1 인 상태 */
    {
        double roi_l1   = avg_l1(d->ref_closed_rgb, rgb, w, stride, x0, y0, x1, y1);
        double frame_l1 = sampled_frame_l1(d->ref_closed_rgb, rgb, w, h, stride, step);
        /* 문만 열렸다면 ROI 는 크게, 화면 전체는 거의 안 변합니다. 조명이 바뀌면 둘이 함께
           움직여 비율이 1 에 가까워지므로 걸러집니다. ROI 가 화면의 절반 이상이면 이 가드가
           항상 걸리므로 ROI 는 문짝·문틀만 포함해야 합니다(대시보드 안내 문구와 동일). */
        int candidate = stable && roi_l1 >= d->auto_open_min_l1 && frame_l1 < roi_l1 * 0.5;
        if (!candidate) { d->auto_open_since = 0.0; d->auto_wait_seconds = 0.0; return 0; }
        if (d->auto_open_since <= 0.0) d->auto_open_since = now;
        d->auto_wait_seconds = now - d->auto_open_since;
        if (d->auto_wait_seconds < d->auto_open_hold_seconds) return 0;
        if (open_path && raw_rgb_save(open_path, rgb, w, h, stride) != 0) {
            d->auto_open_since = now;
            return -1;
        }
        if (!install_reference(&d->ref_open_rgb, &d->ref_open_w, &d->ref_open_h,
                               rgb, w, h, stride))
            return -1;
        invalidate_band(d);
        d->last_state = -1; d->candidate_state = -1; d->candidate_frames = 0;
        auto_reset_wait(d);
        d->auto_phase = DOOR_AUTO_DONE;
        d->auto_phase_since = now;
        d->auto_stalled = 0;
        return 2;
    }
}
