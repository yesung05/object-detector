#ifndef RESTART_H
#define RESTART_H

/*
 * 예약 재시작 — 점주가 고른 요일·시각에 프로세스를 스스로 끝냅니다.
 *
 * detector 는 자기 자신을 되살릴 수 없으므로, 종료 코드로 의도를 알리고 run-all.ps1 의
 * 감시 루프가 다시 띄웁니다. 정상 종료(0)·예약 재시작(10)·비정상 종료(그 외)를 구분해야
 * 감시자가 "재시작할지 / 백오프할지 / 그만둘지"를 정할 수 있습니다.
 *
 * 왜 자동 판단(메모리 임계값 등)이 아니라 사용자가 고른 시각인가:
 * 재시작은 추적 중인 체류 시간과 기준 상태를 잃는 파괴적 동작입니다. 조건이 우연히
 * 맞아 영업 중에 재시작되는 것보다, 점주가 지정한 한산한 시각에만 일어나는 편이
 * 예측 가능하고 안전합니다.
 */

typedef struct {
    int enabled;        /* restart_enabled, 기본 0 — 파괴적 동작이라 기본은 꺼 둡니다 */
    int days;           /* 비트마스크. bit0=일 bit1=월 … bit6=토. 127=매일 */
    int hour, minute;   /* restart_time "HH:MM" 을 파싱한 값 */
    /* 예정 시각에 사람이 있으면 미루는데, 무한정 미루면 엉뚱한 시간에 재시작됩니다.
     * 이 시간 안에 조건이 안 맞으면 오늘은 포기하고 내일 같은 시각에 다시 시도합니다. */
    int window_minutes; /* 기본 60 */
} RestartConfig;

typedef struct {
    RestartConfig config;
    int handled_yday;   /* 오늘 이미 처리함(tm_yday). -1 = 미처리 */
    int defer_logged;   /* 연기 로그를 이번 창에서 이미 남겼는가 */
} RestartScheduler;

typedef enum {
    RESTART_NO    = 0,  /* 아직 때가 아님 */
    RESTART_DEFER = 1,  /* 때가 됐지만 사람이 있어 연기 (창당 1회만 반환) */
    RESTART_NOW   = 2,  /* 재시작 */
    RESTART_SKIP  = 3   /* 창을 넘겨 오늘은 포기 (1회만 반환) */
} RestartDecision;

void restart_init(RestartScheduler *s, const RestartConfig *config);
void restart_configure(RestartScheduler *s, const RestartConfig *config);

/* "HH:MM" 을 파싱합니다. 실패하면 0 을 반환하고 out 을 건드리지 않습니다. */
int restart_parse_time(const char *text, int *hour, int *minute);

/* 순수 판정 — 지금이 예정 창 안인가. wday 0=일…6=토, minutes_now 는 자정 기준 분. */
int restart_in_window(const RestartConfig *config, int wday, int minutes_now);

/*
 * 한 스텝 진행합니다. 1초에 한 번 정도 호출하세요.
 *
 * busy: 지금 재시작하면 안 되는 상태(사람 추적 중, 미해소 소실 경고 등). 재시작은
 *       체류 시간과 추적을 잃으므로, 사람이 있는 동안에는 미룹니다.
 *
 * DEFER 와 SKIP 은 로그를 남기라는 뜻이며 창당 한 번만 반환합니다 — 매초 같은 줄을
 * 남기면 로그가 덮입니다.
 */
RestartDecision restart_check(RestartScheduler *s, int wday, int yday,
                              int minutes_now, int busy);

/*
 * "오늘 이미 재시작했다"를 파일로 남기고 다시 읽습니다.
 *
 * 없으면 재시작이 무한 반복됩니다: 재시작은 프로세스 상태를 통째로 초기화하는데, 새로 뜬
 * 프로세스는 오늘 처리 여부를 모른 채 여전히 예정 창(기본 60분) 안에 있다고 판단해 곧바로
 * 또 재시작합니다. 파일 하나로 창 전체를 덮어 하루 한 번만 일어나게 만듭니다.
 *
 * 읽기 실패(파일 없음·손상)는 오류가 아니라 "아직 재시작한 적 없음"으로 다룹니다 —
 * 상태 파일을 지웠다고 재시작 기능이 죽으면 안 됩니다.
 */
int restart_state_load(RestartScheduler *s, const char *path, int today_yday, int today_year);
int restart_state_save(const char *path, int yday, int year);

#endif /* RESTART_H */
