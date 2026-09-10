#include "slot_monitor.h"
#include "rules.h"       /* OBJ_CHAIR, OBJ_DININGTABLE */
#include "platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * 슬롯 ID 결정 전략.
 *
 * 가구는 거의 이동하지 않으므로 bbox 중심점을 32px 그리드로 양자화하고,
 * (class_id, grid_x, grid_y) 세 값을 정수 키로 조합합니다.
 * 32px는 720p 기준 의자 폭(~80px)의 절반 수준으로, 미세 흔들림에 안정적이면서
 * 인접한 두 의자가 같은 슬롯으로 합쳐지지 않는 균형점입니다.
 */
static int make_slot_id(int class_id, int cx, int cy) {
    int gx = cx / 32;
    int gy = cy / 32;
    /* class_id는 14 또는 15이므로 10000 단위로 분리하면 충돌 없음 */
    return (class_id * 10000) + (gx * 100) + gy;
}

/* bbox 중심점 (정수) */
static int bbox_cx(const GrayRect *r) { return (int)((r->x1 + r->x2) * 0.5f); }
static int bbox_cy(const GrayRect *r) { return (int)((r->y1 + r->y2) * 0.5f); }

/*
 * IoU 계산 — 사람 bbox와 슬롯 bbox가 얼마나 겹치는지 판단합니다.
 * 사람이 슬롯에 앉아 있는 동안은 오염 판정을 건너뜁니다.
 */
static float rect_iou(const GrayRect *a, const GrayRect *b) {
    float ix1 = a->x1 > b->x1 ? a->x1 : b->x1;
    float iy1 = a->y1 > b->y1 ? a->y1 : b->y1;
    float ix2 = a->x2 < b->x2 ? a->x2 : b->x2;
    float iy2 = a->y2 < b->y2 ? a->y2 : b->y2;
    if (ix2 <= ix1 || iy2 <= iy1) return 0.0f;
    float inter = (ix2 - ix1) * (iy2 - iy1);
    float ua = (a->x2 - a->x1) * (a->y2 - a->y1);
    float ub = (b->x2 - b->x1) * (b->y2 - b->y1);
    float denom = ua + ub - inter;
    if (denom <= 0.0f) return 0.0f;
    return inter / denom;
}

/* 슬롯 bbox에서 사람 겹침 여부 확인. 겹치면 1 반환. */
static int slot_person_overlap(const Slot *s,
                                const GrayRect *persons, int person_count,
                                float iou_threshold) {
    int i;
    for (i = 0; i < person_count; ++i) {
        if (rect_iou(&s->bbox, &persons[i]) >= iou_threshold) return 1;
    }
    return 0;
}

/*
 * RGB24 프레임에서 bbox 영역을 그레이스케일로 잘라냅니다.
 * 그레이 변환: (R + G + B) / 3 — 채널 가중치보다 속도가 중요한 경로이므로 정수 평균을 씁니다.
 * 경계 클램핑: bbox가 프레임 밖으로 나가면 0으로 채웁니다.
 */
static void crop_to_gray(const uint8_t *rgb, int w, int h,
                          const GrayRect *bbox,
                          uint8_t *out, int ow, int oh) {
    int x0 = (int)bbox->x1;
    int y0 = (int)bbox->y1;
    int r, c;
    for (r = 0; r < oh; ++r) {
        for (c = 0; c < ow; ++c) {
            int sx = x0 + c;
            int sy = y0 + r;
            if (sx < 0 || sx >= w || sy < 0 || sy >= h) {
                out[r * ow + c] = 0;
                continue;
            }
            const uint8_t *p = rgb + (sy * w + sx) * 3;
            out[r * ow + c] = (uint8_t)(((int)p[0] + (int)p[1] + (int)p[2]) / 3);
        }
    }
}

/*
 * crop 내에서 8×8 블록 단위 평균 luma diff를 세어 임계값 이상인 블록 수를 반환합니다.
 * 블록 단위 집계는 개별 픽셀 노이즈를 평활화하고 캐시 효율도 좋습니다.
 */
static int count_dirty_blocks(const uint8_t *cur, const uint8_t *base,
                               int w, int h, int thr) {
    int bw = w / 8;
    int bh = h / 8;
    int count = 0;
    int by, bx, py, px;
    for (by = 0; by < bh; ++by) {
        for (bx = 0; bx < bw; ++bx) {
            long sc = 0, sb = 0;
            int n = 0;
            for (py = by * 8; py < (by + 1) * 8 && py < h; ++py) {
                for (px = bx * 8; px < (bx + 1) * 8 && px < w; ++px) {
                    sc += cur[py * w + px];
                    sb += base[py * w + px];
                    ++n;
                }
            }
            if (n > 0 && abs((int)(sc / n) - (int)(sb / n)) >= thr) ++count;
        }
    }
    return count;
}

/* 슬롯 하나 초기화 */
static void slot_clear(Slot *s) {
    free(s->baseline_gray);
    memset(s, 0, sizeof(*s));
}

/* ── 공개 API ─────────────────────────────────────────────────────────── */

void slot_monitor_init(SlotMonitor *sm) {
    memset(sm, 0, sizeof(*sm));
    sm->warn_seconds             = 60;
    sm->urgent_seconds           = 300;
    sm->ttl_seconds              = 120;
    sm->dirty_threshold          = 20;
    sm->min_dirty_blocks         = 2;
    sm->person_iou_skip          = 0.10f;
    sm->no_baseline_warn_seconds = 10;
    sm->auto_relearn_seconds     = 30;
}

void slot_monitor_destroy(SlotMonitor *sm) {
    int i;
    for (i = 0; i < sm->count; ++i)
        free(sm->slots[i].baseline_gray);
    memset(sm, 0, sizeof(*sm));
}

void slot_monitor_apply_config(SlotMonitor *sm, const Config *cfg) {
    sm->warn_seconds =
        config_long(cfg, "slot_warn_seconds", sm->warn_seconds, 5, 3600);
    sm->urgent_seconds =
        config_long(cfg, "slot_urgent_seconds", sm->urgent_seconds, 30, 86400);
    sm->ttl_seconds =
        config_long(cfg, "slot_ttl_seconds", sm->ttl_seconds, 10, 3600);
    sm->dirty_threshold =
        (int)config_long(cfg, "slot_dirty_threshold", sm->dirty_threshold, 1, 255);
    sm->min_dirty_blocks =
        (int)config_long(cfg, "slot_min_dirty_blocks", sm->min_dirty_blocks, 1, 256);
    sm->person_iou_skip =
        config_float(cfg, "slot_person_iou_skip", sm->person_iou_skip, 0.01f, 1.0f);
    sm->no_baseline_warn_seconds =
        config_long(cfg, "slot_no_baseline_warn_seconds",
                    sm->no_baseline_warn_seconds, 1, 3600);
    sm->auto_relearn_seconds =
        config_long(cfg, "slot_auto_relearn_seconds",
                    sm->auto_relearn_seconds, 5, 3600);
}

void slot_monitor_update(SlotMonitor *sm,
                         const uint8_t *rgb, int w, int h,
                         const GrayRect *furniture, int furniture_count,
                         const GrayRect *persons,   int person_count,
                         double now, EventLog *elog) {
    int i, fi;

    /* ── 1단계: 슬롯 목록 갱신 ─────────────────────────────────────────── */

    /* 각 furniture bbox를 슬롯에 매칭 */
    for (fi = 0; fi < furniture_count; ++fi) {
        const GrayRect *fb = &furniture[fi];
        int class_id = OBJ_CHAIR; /* 아래에서 실제 class_id로 교체됨 */

        /*
         * furniture 배열에는 class_id가 없으므로 위치로만 매칭합니다.
         * 실제로는 호출자(main.c)가 OBJ_CHAIR/OBJ_DININGTABLE만 필터링해서 넘깁니다.
         * 두 클래스를 구분하려면 호출자가 struct를 넘겨야 하지만,
         * 현재 API(GrayRect 배열)를 유지하면서 class_id를 추론하는 방법은 없습니다.
         * 이 버전에서는 class_id를 OBJ_CHAIR로 고정하고, 슬롯 ID는 위치만으로 구분합니다.
         * TODO: furniture 배열에 class_id 포함하는 구조체로 시그니처 변경 고려.
         */
        int cx = bbox_cx(fb);
        int cy = bbox_cy(fb);
        int sid = make_slot_id(class_id, cx, cy);

        /* 기존 슬롯 탐색 */
        int found = -1;
        for (i = 0; i < sm->count; ++i) {
            if (sm->slots[i].id == sid) { found = i; break; }
        }

        if (found >= 0) {
            /* 기존 슬롯 갱신 */
            sm->slots[found].bbox     = *fb;
            sm->slots[found].last_seen = now;
        } else if (sm->count < SLOT_MAX) {
            /* 신규 슬롯 추가 */
            Slot *s        = &sm->slots[sm->count++];
            memset(s, 0, sizeof(*s));
            s->id          = sid;
            s->class_id    = class_id;
            s->bbox        = *fb;
            s->last_seen   = now;
            s->activated_at = now;
            s->clean_since  = now;
        }
    }

    /* TTL 만료 슬롯 제거 (뒤에서 앞으로 순회해 배열 압축) */
    for (i = sm->count - 1; i >= 0; --i) {
        if (now - sm->slots[i].last_seen >= (double)sm->ttl_seconds) {
            free(sm->slots[i].baseline_gray);
            /* 마지막 슬롯을 빈 자리로 복사해 압축 */
            sm->slots[i] = sm->slots[--sm->count];
        }
    }

    /* ── 2단계: 슬롯별 diff 체크 + 이벤트 발화 ─────────────────────────── */

    for (i = 0; i < sm->count; ++i) {
        Slot *s = &sm->slots[i];
        char msg[160];

        int bbox_w = (int)(s->bbox.x2 - s->bbox.x1);
        int bbox_h = (int)(s->bbox.y2 - s->bbox.y1);

        /* bbox 너무 작으면 의미 없는 감지이므로 스킵 */
        if (bbox_w < 8 || bbox_h < 8) continue;

        /* 기준 이미지 미캡처 처리 */
        if (!s->has_baseline) {
            /* 사람 미겹침이면 현재 프레임을 기준으로 캡처 */
            int person_overlapping =
                slot_person_overlap(s, persons, person_count, sm->person_iou_skip);

            if (!person_overlapping) {
                int bsz = bbox_w * bbox_h;
                uint8_t *buf = (uint8_t *)malloc((size_t)bsz);
                if (buf) {
                    crop_to_gray(rgb, w, h, &s->bbox, buf, bbox_w, bbox_h);
                    free(s->baseline_gray);
                    s->baseline_gray = buf;  /* SlotMonitor 소유, destroy에서 free */
                    s->baseline_w    = bbox_w;
                    s->baseline_h    = bbox_h;
                    s->has_baseline  = 1;
                    s->clean_since   = now;
                }
            } else {
                /* 기준 없는 상태에서 사람 겹침 → 경고 지연 후 발화 */
                if (!s->no_baseline_fired &&
                    now - s->activated_at >= (double)sm->no_baseline_warn_seconds) {
                    snprintf(msg, sizeof(msg),
                             "slot_no_baseline slot=%d dirty_check_skipped",
                             s->id);
                    event_log_write(elog, LOG_WARN, "slot_monitor", msg);
                    s->no_baseline_fired = 1;
                }
            }
            continue;  /* 기준 캡처 프레임에서는 diff 체크 건너뜀 */
        }

        /* 사람 겹침 시 판정 스킵 — 착석 중인 손님 물건을 잔류물로 오탐 방지 */
        if (slot_person_overlap(s, persons, person_count, sm->person_iou_skip))
            continue;

        /*
         * 현재 bbox 크기가 baseline과 다를 수 있음 (Tier 2 결과 흔들림).
         * 기준과 현재 crop 크기를 맞추기 위해 baseline 크기를 사용합니다.
         */
        int cw = s->baseline_w;
        int ch = s->baseline_h;

        uint8_t *cur_gray = (uint8_t *)malloc((size_t)(cw * ch));
        if (!cur_gray) continue;

        /* bbox 위치는 최신 값을 쓰되, crop 크기는 baseline 크기로 고정 */
        GrayRect crop_rect = s->bbox;
        crop_rect.x2 = crop_rect.x1 + (float)cw;
        crop_rect.y2 = crop_rect.y1 + (float)ch;

        crop_to_gray(rgb, w, h, &crop_rect, cur_gray, cw, ch);

        int dirty_blocks = count_dirty_blocks(cur_gray, s->baseline_gray,
                                              cw, ch, sm->dirty_threshold);
        free(cur_gray);

        if (dirty_blocks >= sm->min_dirty_blocks) {
            /* 오염 감지 */
            if (!s->dirty) {
                s->dirty       = 1;
                s->dirty_since = now;
            }

            /* 에스컬레이션: urgent 우선 확인 (warn보다 먼저, 나중에 발화되면 두 이벤트 모두 발화) */
            double elapsed = now - s->dirty_since;

            if (!s->urgent_fired && elapsed >= (double)sm->urgent_seconds) {
                snprintf(msg, sizeof(msg),
                         "slot_item_urgent slot=%d dirty=%.0fs",
                         s->id, elapsed);
                event_log_write(elog, LOG_ERROR, "slot_monitor", msg);
                s->urgent_fired = 1;
            }
            if (!s->warn_fired && elapsed >= (double)sm->warn_seconds) {
                snprintf(msg, sizeof(msg),
                         "slot_item_warning slot=%d dirty=%.0fs bbox=[%.0f,%.0f,%.0f,%.0f]",
                         s->id, elapsed,
                         s->bbox.x1, s->bbox.y1, s->bbox.x2, s->bbox.y2);
                event_log_write(elog, LOG_WARN, "slot_monitor", msg);
                s->warn_fired = 1;
            }
        } else {
            /* 오염 해소 */
            if (s->dirty) {
                snprintf(msg, sizeof(msg),
                         "slot_cleared slot=%d was_dirty=%.0fs",
                         s->id, now - s->dirty_since);
                event_log_write(elog, LOG_INFO, "slot_monitor", msg);
                s->dirty        = 0;
                s->dirty_since  = 0.0;
                s->warn_fired   = 0;
                s->urgent_fired = 0;
                s->clean_since  = now;
            }

            /*
             * 자동 재학습: clean 상태가 auto_relearn_seconds 이상 지속되면
             * 기준 이미지를 갱신합니다. 조명 드리프트를 흡수하되,
             * dirty인 상태에서 잘못 학습하지 않도록 clean 진입 시에만 허용합니다.
             */
            if (s->has_baseline &&
                s->clean_since > 0.0 &&
                now - s->clean_since >= (double)sm->auto_relearn_seconds) {
                int bsz = cw * ch;
                uint8_t *buf = (uint8_t *)malloc((size_t)bsz);
                if (buf) {
                    GrayRect relearn_rect = s->bbox;
                    relearn_rect.x2 = relearn_rect.x1 + (float)cw;
                    relearn_rect.y2 = relearn_rect.y1 + (float)ch;
                    crop_to_gray(rgb, w, h, &relearn_rect, buf, cw, ch);
                    free(s->baseline_gray);
                    s->baseline_gray = buf;  /* SlotMonitor 소유 */
                    s->clean_since   = now;  /* 재학습 후 타이머 리셋 */
                }
            }
        }
    }
}
