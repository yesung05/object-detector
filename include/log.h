#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <stdio.h>

typedef enum { LOG_INFO = 0, LOG_WARN = 1, LOG_ERROR = 2 } LogLevel;

/*
 * 구조화 이벤트 로그입니다.
 * file == NULL 이면 stderr 에 출력합니다.
 * 새 기능을 추가할 때 정상/비정상 상태를 모두 기록하여 코드 없이 원인을 파악할 수
 * 있게 합니다 (CLAUDE.md 로깅 요구사항).
 */
typedef struct {
    FILE    *file;      /* 외부 소유 — event_log 는 열거나 닫지 않습니다 */
    LogLevel min_level;
} EventLog;

void event_log_init(EventLog *log, FILE *file, LogLevel min_level);

/*
 * 출력 형식:
 *   2026-08-29T14:03:11 WARN  rules  overstay track=7 dwell=4821s limit=3600s
 */
void event_log_write(EventLog *log, LogLevel level,
                     const char *module, const char *message);

#endif /* EVENT_LOG_H */
