#include "residue.h"
#include "gray.h"
#include "log.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RAW 파일 하나를 읽어 그레이스케일 버퍼에 채웁니다.
 * 파일 형식: [int32 width][int32 height][w*h*3 RGB bytes]
 * door.c:load_raw() 와 동일한 형식이지만 RGB → luma 변환을 여기서 수행합니다.
 * 파일 없음 → *out=NULL, 반환 0 (에러 아님)
 * 형식 오류 또는 malloc 실패 → 반환 -1 */
static int load_raw_luma(const char *path,
                         uint8_t **out, int *out_w, int *out_h) {
    *out   = NULL;
    *out_w = 0;
    *out_h = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0; /* 파일 없음 = 아직 미설정, 정상 */

    int w = 0, h = 0;
    if (fread(&w, sizeof(int), 1, f) != 1 ||
        fread(&h, sizeof(int), 1, f) != 1 ||
        w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        fclose(f);
        return 0; /* 손상된 파일 — 조용히 무시 */
    }

    size_t npix = (size_t)w * h;
    uint8_t *rgb = (uint8_t *)malloc(npix * 3);
    uint8_t *luma = (uint8_t *)malloc(npix);
    if (!rgb || !luma) {
        free(rgb); free(luma); fclose(f); return -1;
    }

    size_t got = fread(rgb, 1, npix * 3, f);
    fclose(f);

    if (got != npix * 3) { free(rgb); free(luma); return 0; }

    /* BT.601 근사: (77R + 150G + 29B + 128) >> 8 */
    for (size_t i = 0; i < npix; ++i) {
        unsigned int r = rgb[i * 3 + 0];
        unsigned int g = rgb[i * 3 + 1];
        unsigned int b = rgb[i * 3 + 2];
        luma[i] = (uint8_t)((77u * r + 150u * g + 29u * b + 128u) >> 8);
    }
    free(rgb);

    *out   = luma; /* 호출자(residue_destroy)가 해제 */
    *out_w = w;
    *out_h = h;
    return 0;
}

int residue_load(ResidueMonitor *r, const char *path) {
    if (!r) return -1;

    free(r->baseline);
    r->baseline       = NULL;
    r->baseline_w     = 0;
    r->baseline_h     = 0;
    r->baseline_ready = 0;

    if (!path) return 0;

    if (load_raw_luma(path, &r->baseline, &r->baseline_w, &r->baseline_h) != 0)
        return -1;

    if (r->baseline)
        r->baseline_ready = 1;
    return 0;
}

void residue_destroy(ResidueMonitor *r) {
    if (!r) return;
    free(r->baseline);
    free(r->candidate);
    r->baseline  = NULL;
    r->candidate = NULL;
}

int residue_refresh_baseline(ResidueMonitor *r, const GrayBuf *gray, double now) {
    if (!r || !gray || !gray->data) return -1;

    size_t npix = (size_t)gray->width * gray->height;
    if (!r->baseline || r->baseline_w != gray->width ||
        r->baseline_h != gray->height) {
        free(r->baseline);
        r->baseline = (uint8_t *)malloc(npix);
        if (!r->baseline) return -1;
        r->baseline_w = gray->width;
        r->baseline_h = gray->height;
    }
    memcpy(r->baseline, gray->data, npix);
    r->baseline_ready = 1;
    r->baseline_stamp = now;
    return 0;
}

/* 두 uint8_t 절댓값 차 */
static inline int absdiff(int a, int b) {
    int d = a - b;
    return d < 0 ? -d : d;
}

/* 블록(bx, by)의 평균 luma 계산. gray는 다운샘플 버퍼이므로 블록=1픽셀 단위.
 * 기준 이미지와 현재 gray는 동일한 다운샘플 계수로 저장되어 있어야 합니다.
 *
 * 설계 문서 1단계: 블록 평균 휘도 차를 쓰는 이유 — 픽셀 개수 방식은 센서
 * 노이즈 한 점에도 반응하지만 블록 평균은 노이즈를 상쇄합니다.
 * 여기서는 GrayBuf가 이미 다운샘플된 1픽셀=1블록 구조이므로,
 * 단일 픽셀 차이를 직접 비교합니다. */
static inline int block_diff(const uint8_t *baseline, const uint8_t *cur,
                              int bx, int by, int bw) {
    int idx = by * bw + bx;
    return absdiff((int)baseline[idx], (int)cur[idx]);
}

/* 후보 블록 배열에서 4-이웃 연결 요소 하나를 영역으로 수집합니다.
 * 수집된 블록은 visited로 표시합니다.
 * stk_x, stk_y: 호출자 소유 int 배열 (크기 = total_blocks). */
static int flood_fill(const uint8_t *candidate, uint8_t *visited,
                      int bw, int bh,
                      int start_bx, int start_by,
                      int *stk_x, int *stk_y,
                      int stk_cap,
                      int *out_min_bx, int *out_min_by,
                      int *out_max_bx, int *out_max_by) {
    int top = 0, count = 0;
    int min_bx = start_bx, min_by = start_by;
    int max_bx = start_bx, max_by = start_by;

    stk_x[top] = start_bx;
    stk_y[top] = start_by;
    top++;
    visited[start_by * bw + start_bx] = 1;

    while (top > 0) {
        top--;
        int cx = stk_x[top], cy = stk_y[top];
        count++;
        if (cx < min_bx) min_bx = cx;
        if (cy < min_by) min_by = cy;
        if (cx > max_bx) max_bx = cx;
        if (cy > max_by) max_by = cy;

        static const int dx[4] = {1, -1, 0, 0};
        static const int dy[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; ++d) {
            int nx = cx + dx[d], ny = cy + dy[d];
            if (nx < 0 || nx >= bw || ny < 0 || ny >= bh) continue;
            int ni = ny * bw + nx;
            if (!visited[ni] && candidate[ni]) {
                if (top < stk_cap) {
                    visited[ni] = 1;
                    stk_x[top] = nx;
                    stk_y[top] = ny;
                    top++;
                }
            }
        }
    }

    *out_min_bx = min_bx; *out_min_by = min_by;
    *out_max_bx = max_bx; *out_max_by = max_by;
    return count;
}

/* 잔류 영역 중심이 가구 bbox 안에 있는지 확인합니다.
 * IoU 대신 중심점 포함 판정을 쓰는 이유: 잔류 영역이 테이블보다 훨씬 작아
 * IoU가 구조적으로 낮게 나오기 때문입니다. */
static int center_in_furniture(float cx, float cy,
                                const GrayRect *furniture, int count) {
    for (int i = 0; i < count; ++i) {
        if (cx >= furniture[i].x1 && cx <= furniture[i].x2 &&
            cy >= furniture[i].y1 && cy <= furniture[i].y2)
            return 1;
    }
    return 0;
}

int residue_evaluate(ResidueMonitor *r,
                     const GrayBuf *gray,
                     const GrayRect *persons, int person_count,
                     const GrayRect *furniture, int furniture_count,
                     double now, EventLog *elog) {
    if (!r || !gray || !gray->data) return 0;
    if (!r->config.enabled) return 0;
    if (!r->baseline_ready || !r->baseline) return 0;

    int bw = gray->width;
    int bh = gray->height;
    int total_blocks = bw * bh;

    /* 후보 버퍼 지연 초기화 */
    if (!r->candidate || r->blocks_x != bw || r->blocks_y != bh) {
        free(r->candidate);
        r->candidate = (uint8_t *)calloc((size_t)total_blocks, 1);
        if (!r->candidate) return 0;
        r->blocks_x = bw;
        r->blocks_y = bh;
    }

    /* 기준 크기와 현재 버퍼 크기가 다르면 판정 불가 */
    if (r->baseline_w != bw || r->baseline_h != bh) return 0;

    int ds = gray->downsample; /* 블록 → 원본 픽셀 변환 계수 */

    /* ─── 1단계: 후보 블록 산출 ─────────────────────────────────────────── */
    int candidate_count = 0;
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            int diff = block_diff(r->baseline, gray->data, bx, by, bw);
            r->candidate[by * bw + bx] =
                (diff >= r->config.diff_threshold) ? 1 : 0;
            if (r->candidate[by * bw + bx]) candidate_count++;
        }
    }

    /* ─── 조명 변화 가드 ────────────────────────────────────────────────── */
    /* 후보가 전체의 global_change_ratio를 넘으면 조명 변화로 판정합니다.
     * 소등 순간 화면 전체가 하나의 거대한 잔류 영역으로 잡히는 것을 막습니다. */
    if (total_blocks > 0 &&
        (float)candidate_count / (float)total_blocks >=
            r->config.global_change_ratio) {
        /* 모든 추적 영역 폐기 + 기준 강제 갱신 */
        memset(r->regions, 0, sizeof(r->regions));
        residue_refresh_baseline(r, gray, now);
        if (elog) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "residue_baseline_reset ratio=%.2f reason=global_change",
                     (float)candidate_count / (float)total_blocks);
            event_log_write(elog, LOG_INFO, "residue", msg);
        }
        return 0;
    }

    /* ─── 2단계: 사람에 의한 변화 배제 ─────────────────────────────────── */
    /* 활성 트랙 bbox 확장 후 겹치는 후보 블록을 제외합니다.
     * gray_blocks_outside()는 MotionMap 기반이므로, 여기서는 직접 처리합니다. */
    int margin = r->config.person_margin_blocks;
    for (int pi = 0; pi < person_count; ++pi) {
        /* 원본 좌표 → 블록 좌표 (margin 확장) */
        int px1 = (int)(persons[pi].x1 / (float)ds) - margin;
        int py1 = (int)(persons[pi].y1 / (float)ds) - margin;
        int px2 = (int)(persons[pi].x2 / (float)ds) + margin;
        int py2 = (int)(persons[pi].y2 / (float)ds) + margin;
        if (px1 < 0) px1 = 0;
        if (py1 < 0) py1 = 0;
        if (px2 >= bw) px2 = bw - 1;
        if (py2 >= bh) py2 = bh - 1;
        for (int by = py1; by <= py2; ++by)
            for (int bx = px1; bx <= px2; ++bx)
                r->candidate[by * bw + bx] = 0;
    }

    /* ─── 3단계: 4-이웃 연결 요소 → 영역화 ─────────────────────────────── */
    /* visited 및 flood_fill 스택을 동적 할당합니다.
     * 720p 다운샘플 8이면 160×90=14,400 블록으로 정적 배열로는 감당할 수 없습니다. */
    uint8_t *visited = (uint8_t *)calloc((size_t)total_blocks, 1);
    int     *stk_x   = (int *)malloc((size_t)total_blocks * sizeof(int));
    int     *stk_y   = (int *)malloc((size_t)total_blocks * sizeof(int));
    if (!visited || !stk_x || !stk_y) {
        free(visited); free(stk_x); free(stk_y);
        return 0;
    }

    /* 이번 프레임 영역 목록을 임시로 수집합니다. */
    typedef struct {
        int blocks;
        float x1, y1, x2, y2;
        float cx, cy;
    } FoundRegion;
    FoundRegion found[RESIDUE_MAX_REGIONS];
    int found_count = 0;

    for (int by = 0; by < bh && found_count < RESIDUE_MAX_REGIONS; ++by) {
        for (int bx = 0; bx < bw && found_count < RESIDUE_MAX_REGIONS; ++bx) {
            int idx = by * bw + bx;
            if (!r->candidate[idx] || visited[idx]) goto next_block;

            int min_bx, min_by, max_bx, max_by;
            int cnt = flood_fill(r->candidate, visited,
                                 bw, bh, bx, by,
                                 stk_x, stk_y, total_blocks,
                                 &min_bx, &min_by, &max_bx, &max_by);

            if (cnt < r->config.min_blocks) goto next_block;

            FoundRegion *fr = &found[found_count++];
            /* 블록 좌표 → 원본 픽셀 좌표 */
            fr->x1 = (float)(min_bx * ds);
            fr->y1 = (float)(min_by * ds);
            fr->x2 = (float)((max_bx + 1) * ds);
            fr->y2 = (float)((max_by + 1) * ds);
            fr->cx = (fr->x1 + fr->x2) * 0.5f;
            fr->cy = (fr->y1 + fr->y2) * 0.5f;
            fr->blocks = cnt;
next_block:;
        }
    }

    /* ─── 4단계: 기존 영역과 현재 발견 영역 매칭 ────────────────────────── */
    /* 이미 추적 중인 영역과 이번 발견 영역을 중심점 거리로 매칭합니다. */
    int matched_found[RESIDUE_MAX_REGIONS];
    memset(matched_found, 0, sizeof(matched_found));

    /* 기존 영역 업데이트 */
    for (int ri = 0; ri < RESIDUE_MAX_REGIONS; ++ri) {
        ResidueRegion *reg = &r->regions[ri];
        if (!reg->in_use) continue;

        /* 가장 가까운 발견 영역 찾기 */
        float best_dist = 999999.0f;
        int best_fi = -1;
        float reg_cx = (reg->x1 + reg->x2) * 0.5f;
        float reg_cy = (reg->y1 + reg->y2) * 0.5f;

        for (int fi = 0; fi < found_count; ++fi) {
            float dx = found[fi].cx - reg_cx;
            float dy = found[fi].cy - reg_cy;
            float dist = dx * dx + dy * dy;
            /* 허용 반경: 영역 대각선의 절반 */
            float max_dist = (reg->x2 - reg->x1) * (reg->x2 - reg->x1) +
                             (reg->y2 - reg->y1) * (reg->y2 - reg->y1);
            if (dist < best_dist && dist < max_dist) {
                best_dist = dist;
                best_fi = fi;
            }
        }

        if (best_fi >= 0) {
            /* 매칭 성공 — 영역 갱신 */
            matched_found[best_fi] = 1;
            reg->last_seen = now;
            reg->blocks    = found[best_fi].blocks;
            reg->x1        = found[best_fi].x1;
            reg->y1        = found[best_fi].y1;
            reg->x2        = found[best_fi].x2;
            reg->y2        = found[best_fi].y2;
        } else {
            /* 매칭 실패 — clear_seconds 후 해소 처리 */
            if ((now - reg->last_seen) >= r->config.clear_seconds) {
                if (reg->confirmed && elog) {
                    char msg[128];
                    const char *kind_str =
                        reg->kind == RESIDUE_TRASH ? "trash" :
                        reg->kind == RESIDUE_SPILL ? "spill" : "unknown";
                    snprintf(msg, sizeof(msg),
                             "residue_cleared kind=%s x=%.0f y=%.0f",
                             kind_str, (reg->x1 + reg->x2) * 0.5f,
                             (reg->y1 + reg->y2) * 0.5f);
                    event_log_write(elog, LOG_INFO, "residue", msg);
                }
                memset(reg, 0, sizeof(*reg));
            }
        }
    }

    /* 신규 발견 영역 — 빈 슬롯에 등록 */
    for (int fi = 0; fi < found_count; ++fi) {
        if (matched_found[fi]) continue;
        for (int ri = 0; ri < RESIDUE_MAX_REGIONS; ++ri) {
            if (!r->regions[ri].in_use) {
                ResidueRegion *reg = &r->regions[ri];
                memset(reg, 0, sizeof(*reg));
                reg->in_use    = 1;
                reg->first_seen = now;
                reg->last_seen = now;
                reg->blocks    = found[fi].blocks;
                reg->x1        = found[fi].x1;
                reg->y1        = found[fi].y1;
                reg->x2        = found[fi].x2;
                reg->y2        = found[fi].y2;
                break;
            }
        }
    }

    /* ─── 4단계 계속: 지속 시간 확인 + 이벤트 발화 ─────────────────────── */
    int confirmed_this_frame = 0;
    for (int ri = 0; ri < RESIDUE_MAX_REGIONS; ++ri) {
        ResidueRegion *reg = &r->regions[ri];
        if (!reg->in_use || reg->confirmed) continue;

        double held = now - reg->first_seen;
        if (held < r->config.confirm_seconds) continue;

        /* ─── 5단계: 위치 기반 종류 분류 ──────────────────────────────── */
        float cx = (reg->x1 + reg->x2) * 0.5f;
        float cy = (reg->y1 + reg->y2) * 0.5f;

        if (furniture && furniture_count > 0) {
            reg->kind = center_in_furniture(cx, cy, furniture, furniture_count)
                            ? RESIDUE_TRASH : RESIDUE_SPILL;
        } else {
            reg->kind = RESIDUE_UNKNOWN;
        }

        reg->confirmed = 1;
        confirmed_this_frame++;

        if (elog) {
            char msg[128];
            const char *evt =
                reg->kind == RESIDUE_TRASH  ? "residue_trash"  :
                reg->kind == RESIDUE_SPILL  ? "residue_spill"  :
                                              "residue_unknown";
            snprintf(msg, sizeof(msg),
                     "%s blocks=%d x=%.0f y=%.0f held=%.0fs",
                     evt, reg->blocks, cx, cy, held);
            event_log_write(elog, LOG_WARN, "residue", msg);
        }
    }

    /* ─── 안전 시점 기준 자동 갱신 ─────────────────────────────────────── */
    /* 조건: 사람 없음 + 후보 없음 + refresh 주기 경과.
     * 잔류물이 있는 상태를 기준으로 갱신하면 영원히 감지되지 않으므로
     * 후보가 하나라도 있으면 갱신하지 않습니다. */
    int any_candidate = 0;
    for (int i = 0; i < total_blocks; ++i) {
        if (r->candidate[i]) { any_candidate = 1; break; }
    }
    if (person_count == 0 && !any_candidate &&
        (now - r->baseline_stamp) >= r->config.baseline_refresh_seconds) {
        residue_refresh_baseline(r, gray, now);
    }

    free(visited);
    free(stk_x);
    free(stk_y);
    return confirmed_this_frame;
}
