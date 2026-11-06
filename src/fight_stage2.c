#include "fight_stage2.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * 특징·게이트·결정 규칙은 scripts/fight2_extract.py 의 frame_vec / windows_of 와 같아야 한다(학습 분포와 일치).
 * 바꾸면 모델을 다시 학습해야 하므로 상수를 이 파일 위쪽에 모아 둔다.
 */
#define PERSON_MIN_SCORE 0.25f   /* 사람으로 인정하는 최소 점수(학습 때와 동일) */
#define GATE_MIN_VALID   5       /* 9표본 중 2인 이상이 보인 표본 수 */
#define GATE_MIN_CLOSE   3       /* 그중 근접도 >= GATE_CLOSE 인 표본 수 */
#define GATE_CLOSE       0.2f
#define STRIDE           3       /* 창 평가 간격(표본) */
#define FIRE_SPAN        (3 * STRIDE) /* 임계값 이상 창 두 개가 이 표본 수 안에 있으면 발화 */

typedef struct {
    float vec[FIGHT_D];
    int   valid;
    float close;
} Sample;

struct FightMonitor {
    double threshold, interval;
    FightScoreFn scorer;
    void *scorer_ctx;
    Sample ring[FIGHT_T];       /* 최근 9표본(오래된 것부터 ring_start 이후) */
    int    ring_count;          /* 채워진 표본 수(최대 FIGHT_T) */
    int    ring_start;
    unsigned long sample_index; /* 지금까지 받은 표본 번호(창 시작 비교용) */
    double last_sample_time;
    long long last_hit_start;   /* 마지막 임계값 초과 창의 시작 표본 번호, 없으면 -1 */
    double fired_until;         /* 이 시각까지 재발화 금지 */
    FightStats stats;
};

FightMonitor *fight_create(double threshold, double sample_interval) {
    FightMonitor *m = (FightMonitor *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->threshold = threshold;
    m->interval = sample_interval > 0.05 ? sample_interval : 1.0 / 3.0;
    m->last_hit_start = -1;
    m->last_sample_time = -1e18;
    return m;
}

void fight_destroy(FightMonitor *m) { free(m); }
void fight_set_scorer(FightMonitor *m, FightScoreFn fn, void *context) { if (m) { m->scorer = fn; m->scorer_ctx = context; } }
void fight_set_threshold(FightMonitor *m, double threshold) { if (m) m->threshold = threshold; }
void fight_get_stats(const FightMonitor *m, FightStats *out) { if (m && out) *out = m->stats; }

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void fight_frame_features(const DetectionList *persons, float *vec, int *valid, float *close) {
    int idx[64], n = 0, i, j, k, s;
    int best_i = -1, best_j = -1;
    float best_d = 1e30f, best_ha = 1, best_hb = 1;
    memset(vec, 0, sizeof(float) * FIGHT_D);
    *valid = 0;
    *close = 0.0f;
    if (persons && persons->items) {
        for (i = 0; i < (int)persons->count && n < 64; ++i)
            if (persons->items[i].score >= PERSON_MIN_SCORE) idx[n++] = i;
    }
    vec[105] = (float)(n < 4 ? n : 4) / 4.0f;
    if (n < 2) return;
    /* 가장 가까운 두 사람: 중심 거리를 두 사람 평균 키로 나눈 값이 최소인 쌍 */
    for (i = 0; i < n; ++i) {
        for (j = i + 1; j < n; ++j) {
            const Detection *a = &persons->items[idx[i]], *b = &persons->items[idx[j]];
            float ha = fmaxf(a->y2 - a->y1, 1.0f), hb = fmaxf(b->y2 - b->y1, 1.0f);
            float dx = (a->x1 + a->x2) / 2 - (b->x1 + b->x2) / 2, dy = (a->y1 + a->y2) / 2 - (b->y1 + b->y2) / 2;
            float d = sqrtf(dx * dx + dy * dy) / ((ha + hb) / 2);
            if (d < best_d) { best_d = d; best_i = idx[i]; best_j = idx[j]; best_ha = ha; best_hb = hb; }
        }
    }
    {
        const Detection *p[2] = {&persons->items[best_i], &persons->items[best_j]};
        float ux1, uy1, ux2, uy2, uw, uh, ix, iy, inter, uni;
        if ((p[0]->x1 + p[0]->x2) > (p[1]->x1 + p[1]->x2)) { const Detection *t = p[0]; p[0] = p[1]; p[1] = t; } /* 왼쪽 사람이 먼저 */
        ux1 = fminf(p[0]->x1, p[1]->x1); uy1 = fminf(p[0]->y1, p[1]->y1);
        ux2 = fmaxf(p[0]->x2, p[1]->x2); uy2 = fmaxf(p[0]->y2, p[1]->y2);
        uw = fmaxf(ux2 - ux1, 1.0f); uh = fmaxf(uy2 - uy1, 1.0f);
        for (s = 0; s < 2; ++s) {
            for (k = 0; k < 17; ++k) {
                float x = 0, y = 0, sc = 0;
                if (k < p[s]->keypoint_count) { x = p[s]->kp[k].x; y = p[s]->kp[k].y; sc = p[s]->kp[k].score; }
                vec[s * 51 + k * 3]     = clampf((x - ux1) / uw, -0.5f, 1.5f);
                vec[s * 51 + k * 3 + 1] = clampf((y - uy1) / uh, -0.5f, 1.5f);
                vec[s * 51 + k * 3 + 2] = sc;
            }
        }
        ix = fmaxf(0.0f, fminf(p[0]->x2, p[1]->x2) - fmaxf(p[0]->x1, p[1]->x1));
        iy = fmaxf(0.0f, fminf(p[0]->y2, p[1]->y2) - fmaxf(p[0]->y1, p[1]->y1));
        inter = ix * iy;
        uni = (p[0]->x2 - p[0]->x1) * (p[0]->y2 - p[0]->y1) + (p[1]->x2 - p[1]->x1) * (p[1]->y2 - p[1]->y1) - inter;
        vec[102] = fminf(best_d, 4.0f) / 4.0f;
        vec[103] = uni > 0 ? inter / uni : 0.0f;
        /* 키 비율: 학습 스크립트(fight2_extract.py frame_vec)는 쌍을 고른 순서(검출 순번이 낮은 사람/높은 사람)의 높이로
           log(ha/hb)를 계산했다. 좌우 정렬 뒤의 높이가 아니다. 학습 분포와 맞추려고 같은 순서를 쓴다. */
        vec[104] = clampf(logf(best_ha / best_hb), -1.5f, 1.5f) / 1.5f;
    }
    *valid = 1;
    *close = fmaxf(0.0f, 1.0f - best_d / 1.5f);
}

static Sample *ring_at(FightMonitor *m, int i) { return &m->ring[(m->ring_start + i) % FIGHT_T]; }

int fight_update(FightMonitor *m, const DetectionList *persons, double now, double cooldown, FightEvent *out) {
    float window[FIGHT_T * FIGHT_D];
    int i, valid_count = 0, close_count = 0;
    Sample s;
    if (out) memset(out, 0, sizeof(*out));
    if (!m) return 0;
    /* 표본 간격: 이전 표본에서 interval 이 지나지 않았으면 이 프레임은 건너뛴다. 너무 오래 비었으면(추론이 쉬었거나 카메라 끊김)
       창을 비운다 — 시간적으로 이어지지 않은 표본으로 만든 창은 학습 분포와 다르다. */
    if (now - m->last_sample_time < m->interval * 0.9) return 0;
    if (now - m->last_sample_time > m->interval * 2.5) { m->ring_count = 0; m->ring_start = 0; m->last_hit_start = -1; }
    m->last_sample_time = now;
    fight_frame_features(persons, s.vec, &s.valid, &s.close);
    if (m->ring_count < FIGHT_T) {
        *ring_at(m, m->ring_count++) = s;
    } else {
        m->ring_start = (m->ring_start + 1) % FIGHT_T;
        *ring_at(m, FIGHT_T - 1) = s;
    }
    m->sample_index++;
    m->stats.samples++;
    if (m->ring_count < FIGHT_T || (m->sample_index % STRIDE) != 0) return 0;

    for (i = 0; i < FIGHT_T; ++i) {
        const Sample *q = ring_at(m, i);
        valid_count += q->valid;
        close_count += q->close >= GATE_CLOSE;
        memcpy(window + i * FIGHT_D, q->vec, sizeof(float) * FIGHT_D);
    }
    if (valid_count < GATE_MIN_VALID || close_count < GATE_MIN_CLOSE) return 0;   /* 게이트 미통과: 분류기를 부르지 않는다 */
    m->stats.gate_pass++;
    if (!m->scorer) return 0;
    {
        float p = m->scorer(m->scorer_ctx, window);
        long long start = (long long)m->sample_index - FIGHT_T;
        m->stats.scorer_calls++;
        if (p < 0) return 0;                                      /* 분류기 오류는 발화 없이 넘긴다(통계로만 드러남) */
        if (p < (float)m->threshold) return 0;
        m->stats.hits++;
        if (m->last_hit_start >= 0 && start - m->last_hit_start <= FIRE_SPAN && now >= m->fired_until) {
            m->stats.fires++;
            m->fired_until = now + cooldown;
            if (out) { out->fired = 1; out->score = p; out->hit_windows = 2; }
            m->last_hit_start = -1;
            return 1;
        }
        m->last_hit_start = start;
    }
    return 0;
}
