#include "restart.h"

#include <stdio.h>
#include <string.h>

void restart_init(RestartScheduler *s, const RestartConfig *config) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->handled_yday = -1;
    if (config) s->config = *config;
    else {
        s->config.enabled        = 0;
        s->config.days           = 127;  /* 매일 */
        s->config.hour           = 4;
        s->config.minute         = 0;
        s->config.window_minutes = 60;
    }
}

void restart_configure(RestartScheduler *s, const RestartConfig *config) {
    if (!s || !config) return;
    /* 예정 시각이 바뀌면 오늘 처리 기록을 지웁니다 — 점주가 시각을 옮겼는데
     * "오늘은 이미 했음"으로 남아 새 시각이 무시되면 설정이 먹히지 않는 것처럼 보입니다. */
    if (config->hour != s->config.hour || config->minute != s->config.minute ||
        config->days != s->config.days) {
        s->handled_yday = -1;
        s->defer_logged = 0;
    }
    s->config = *config;
}

int restart_parse_time(const char *text, int *hour, int *minute) {
    int h = 0, m = 0;
    char extra = 0;
    if (!text || !hour || !minute) return 0;
    if (sscanf(text, "%d:%d%c", &h, &m, &extra) != 2) return 0;
    if (h < 0 || h > 23 || m < 0 || m > 59) return 0;
    *hour = h;
    *minute = m;
    return 1;
}

int restart_in_window(const RestartConfig *c, int wday, int minutes_now) {
    int start, end;
    if (!c || !c->enabled) return 0;
    if (wday < 0 || wday > 6) return 0;
    if (!((c->days >> wday) & 1)) return 0;
    start = c->hour * 60 + c->minute;
    end   = start + (c->window_minutes > 0 ? c->window_minutes : 60);
    /* 자정을 넘기지 않도록 자릅니다. 날짜 경계를 넘나들면 "오늘 처리했는가" 판정이
     * 복잡해지는데, 한산한 새벽 시간대를 쓰는 용도라 잘라도 실용상 문제가 없습니다. */
    if (end > 24 * 60) end = 24 * 60;
    return minutes_now >= start && minutes_now < end;
}

int restart_state_load(RestartScheduler *s, const char *path,
                       int today_yday, int today_year) {
    FILE *f;
    int yday = -1, year = -1;
    if (!s || !path) return 0;
    f = fopen(path, "r");
    if (!f) return 0;                       /* 아직 재시작한 적 없음 — 정상 */
    if (fscanf(f, "%d %d", &year, &yday) != 2) { fclose(f); return 0; }
    fclose(f);
    if (year != today_year || yday != today_yday) return 0;
    /* 오늘 이미 재시작했습니다 — 같은 창 안에서 다시 끝내지 않도록 처리 완료로 표시합니다. */
    s->handled_yday = today_yday;
    return 1;
}

int restart_state_save(const char *path, int yday, int year) {
    FILE *f;
    if (!path) return -1;
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%d %d\n", year, yday);
    fclose(f);
    return 0;
}

RestartDecision restart_check(RestartScheduler *s, int wday, int yday,
                              int minutes_now, int busy) {
    if (!s || !s->config.enabled) return RESTART_NO;
    if (s->handled_yday == yday)  return RESTART_NO;   /* 오늘은 끝 */

    if (!restart_in_window(&s->config, wday, minutes_now)) {
        /* 창을 지나쳤는데 아직 처리하지 못했다면 오늘은 포기합니다.
         * 미뤄 두었다가 영업이 한창인 낮에 갑자기 재시작되는 것이, 예정 시각을
         * 한 번 건너뛰는 것보다 훨씬 나쁩니다. */
        if (wday >= 0 && wday <= 6 && ((s->config.days >> wday) & 1) &&
            minutes_now >= s->config.hour * 60 + s->config.minute) {
            s->handled_yday = yday;
            s->defer_logged = 0;
            return RESTART_SKIP;
        }
        s->defer_logged = 0;
        return RESTART_NO;
    }

    if (busy) {
        if (s->defer_logged) return RESTART_NO;
        s->defer_logged = 1;
        return RESTART_DEFER;
    }

    s->handled_yday = yday;
    s->defer_logged = 0;
    return RESTART_NOW;
}
