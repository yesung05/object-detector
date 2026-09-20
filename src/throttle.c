#include "throttle.h"

#include <string.h>

static const ThrottleConfig DEFAULT_THROTTLE = {
    1,     /* enabled */
    50,    /* light_percent */
    70,    /* heavy_percent */
    85,    /* minimal_percent */
    10,    /* hysteresis_percent */
    5.0,   /* min_level_seconds */
    2.0,   /* check_seconds */
};

void throttle_init(Throttle *t, const ThrottleConfig *config) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->config = config ? *config : DEFAULT_THROTTLE;
    t->level  = THROTTLE_NONE;
}

void throttle_configure(Throttle *t, const ThrottleConfig *config) {
    if (!t || !config) return;
    /* 단계와 측정 기준점은 유지합니다 — 설정을 저장할 때마다 감속이 풀렸다 걸리면
       키오스크가 바쁜 순간에 오히려 부하가 튑니다. */
    t->config = *config;
}

ThrottleLevel throttle_decide(const ThrottleConfig *config,
                              ThrottleLevel current, double others_percent) {
    int hys;
    ThrottleLevel target;
    if (!config || !config->enabled) return THROTTLE_NONE;
    hys = config->hysteresis_percent;

    /* 진입 임계값으로 목표 단계를 먼저 구합니다. */
    if      (others_percent >= config->minimal_percent) target = THROTTLE_MINIMAL;
    else if (others_percent >= config->heavy_percent)   target = THROTTLE_HEAVY;
    else if (others_percent >= config->light_percent)   target = THROTTLE_LIGHT;
    else                                                target = THROTTLE_NONE;

    /* 상승은 즉시, 한 번에 여러 단계도 건너뜁니다 — 키오스크가 갑자기 바빠지면
       천천히 물러날 여유가 없습니다. */
    if (target >= current) return target;

    /* 하강은 진입 임계값이 아니라 그보다 hys 만큼 낮은 복귀 임계값을 넘겨야 하고,
       한 번에 한 단계씩만 풉니다. 같은 임계값을 양방향에 쓰면 경계에서 단계가 초당
       여러 번 뒤집혀 로그가 폭주하고 추론 주기가 계속 흔들립니다.
       목표 단계로 곧장 내려가지 않는 이유도 같습니다 — 부하가 잠깐 꺼진 것일 수 있습니다. */
    switch (current) {
        case THROTTLE_MINIMAL:
            return (others_percent < config->minimal_percent - hys)
                 ? THROTTLE_HEAVY : THROTTLE_MINIMAL;
        case THROTTLE_HEAVY:
            return (others_percent < config->heavy_percent - hys)
                 ? THROTTLE_LIGHT : THROTTLE_HEAVY;
        case THROTTLE_LIGHT:
            return (others_percent < config->light_percent - hys)
                 ? THROTTLE_NONE : THROTTLE_LIGHT;
        default:
            return THROTTLE_NONE;
    }
}

int throttle_update(Throttle *t, double now, double proc_cpu_seconds, int cpu_count) {
    CpuTimes cur;
    double wall_delta, sys_busy, sys_total, system_pct, ours_pct, others;
    ThrottleLevel next;

    if (!t) return 0;
    if (!t->config.enabled) {
        if (t->level != THROTTLE_NONE) {
            t->level = THROTTLE_NONE;
            t->level_since = now;
            return 1;
        }
        return 0;
    }
    if (now - t->last_check < t->config.check_seconds) return 0;
    t->last_check = now;

    if (platform_cpu_times(&cur) != 0) {
        t->measure_failed = 1;
        return 0;   /* 측정 불가 — 감속 판단을 하지 않습니다(추측으로 줄이지 않음) */
    }
    t->measure_failed = 0;

    if (!t->have_baseline) {
        t->prev_cpu       = cur;
        t->prev_proc_cpu  = proc_cpu_seconds;
        t->prev_wall      = now;
        t->have_baseline  = 1;
        return 0;
    }

    wall_delta = now - t->prev_wall;
    sys_total  = (double)(cur.total - t->prev_cpu.total);
    sys_busy   = sys_total - (double)(cur.idle - t->prev_cpu.idle);

    /* 샘플 간격이 너무 짧거나 카운터가 되감긴 경우는 버립니다. */
    if (wall_delta <= 0.0 || sys_total <= 0.0 || cur.total < t->prev_cpu.total) {
        t->prev_cpu      = cur;
        t->prev_proc_cpu = proc_cpu_seconds;
        t->prev_wall     = now;
        return 0;
    }

    system_pct = sys_busy / sys_total * 100.0;
    if (cpu_count < 1) cpu_count = 1;
    /* 우리 CPU 시간은 모든 스레드 합이므로, 전체 용량(코어 수 × 경과 시간)으로 나눕니다. */
    ours_pct = (proc_cpu_seconds - t->prev_proc_cpu) / (wall_delta * cpu_count) * 100.0;

    t->prev_cpu      = cur;
    t->prev_proc_cpu = proc_cpu_seconds;
    t->prev_wall     = now;

    others = system_pct - ours_pct;
    if (others < 0.0)   others = 0.0;    /* 측정 오차로 음수가 될 수 있습니다 */
    if (others > 100.0) others = 100.0;
    t->others_percent = others;

    next = throttle_decide(&t->config, t->level, others);
    if (next == t->level) return 0;

    /* 단계를 너무 자주 바꾸면 추론 주기가 흔들려 오히려 불안정해집니다.
       단, 부하가 급등해 더 높은 단계로 가는 경우는 즉시 반영합니다 — 키오스크 보호가 우선입니다. */
    if (next < t->level && (now - t->level_since) < t->config.min_level_seconds) return 0;

    t->level       = next;
    t->level_since = now;
    return 1;
}

int throttle_detect_multiplier(ThrottleLevel level) {
    switch (level) {
        case THROTTLE_LIGHT:   return 2;
        case THROTTLE_HEAVY:   return 4;
        case THROTTLE_MINIMAL: return 0;
        default:               return 1;
    }
}

int throttle_tier2_multiplier(ThrottleLevel level) {
    switch (level) {
        case THROTTLE_LIGHT:   return 2;
        case THROTTLE_HEAVY:   return 0;
        case THROTTLE_MINIMAL: return 0;
        default:               return 1;
    }
}

int throttle_stream_fps(ThrottleLevel level, int base_fps) {
    int fps = base_fps;
    if (level == THROTTLE_HEAVY)   fps = base_fps / 2;
    if (level == THROTTLE_MINIMAL) fps = base_fps / 4;
    return fps < 1 ? 1 : fps;
}

const char *throttle_level_name(ThrottleLevel level) {
    switch (level) {
        case THROTTLE_LIGHT:   return "LIGHT";
        case THROTTLE_HEAVY:   return "HEAVY";
        case THROTTLE_MINIMAL: return "MINIMAL";
        default:               return "NONE";
    }
}
