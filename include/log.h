#ifndef EVENT_LOG_H
#define EVENT_LOG_H

/*
 * 구조화 이벤트 로그 — SQLite 백엔드.
 * 이벤트는 events 테이블에 저장되고, 실시간 확인용으로 stderr에도 동시 출력합니다.
 * path=":memory:" 로 열면 인메모리 DB (단위 테스트용).
 *
 * 새 기능을 추가할 때 정상/비정상 상태를 모두 기록하여 코드 없이 원인을 파악할 수
 * 있게 합니다 (CLAUDE.md 로깅 요구사항).
 */

typedef enum { LOG_INFO = 0, LOG_WARN = 1, LOG_ERROR = 2 } LogLevel;

/* SQLite 헤더는 구현 파일에서만 include합니다.
 * 외부에는 불투명 포인터로 노출합니다. */
typedef struct sqlite3       sqlite3;
typedef struct sqlite3_stmt  sqlite3_stmt;

typedef struct {
    sqlite3      *db;         /* 소유 — event_log_close 에서 해제 */
    sqlite3_stmt *stmt_ins;   /* 미리 컴파일한 INSERT 구문, db 수명과 동일 */
    LogLevel      min_level;
    int           echo_stderr; /* 1이면 SQLite 저장과 동시에 stderr 출력 */
} EventLog;

/* path: DB 파일 경로, 또는 ":memory:" (인메모리, 테스트용)
 * echo_stderr: 1이면 실시간 콘솔 출력 병행
 * 반환: 0=성공, -1=실패 */
int  event_log_open(EventLog *log, const char *path,
                    LogLevel min_level, int echo_stderr);
void event_log_close(EventLog *log);

/*
 * 출력 형식 (stderr echo):
 *   2026-08-29T14:03:11 WARN  rules    overstay track=7 dwell=4821s
 */
void event_log_write(EventLog *log, LogLevel level,
                     const char *module, const char *message);

/* level 이상으로 기록된 행 수 반환. 테스트 검증용. */
int event_log_count(EventLog *log, LogLevel min_level);

#endif /* EVENT_LOG_H */
