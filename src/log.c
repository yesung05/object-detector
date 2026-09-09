#include "log.h"

/* SQLite 스레드 안전 모드: SQLITE_THREADSAFE=1 (기본값) — 직렬화 모드.
 * 이 프로세스의 모든 이벤트는 메인 스레드에서만 기록하므로 충분합니다. */
#define SQLITE_THREADSAFE 1
#include "../third_party/sqlite/sqlite3.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS events ("
    "  id      INTEGER PRIMARY KEY,"
    "  ts_unix REAL    NOT NULL,"   /* platform_monotonic 대신 wall-clock double */
    "  ts_iso  TEXT    NOT NULL,"   /* 'YYYY-MM-DDTHH:MM:SS' */
    "  level   TEXT    NOT NULL,"   /* 'INFO'|'WARN'|'ERROR' */
    "  module  TEXT    NOT NULL,"
    "  message TEXT    NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_events_ts ON events(ts_unix);";

static const char *INSERT_SQL =
    "INSERT INTO events(ts_unix,ts_iso,level,module,message)"
    " VALUES(?,?,?,?,?);";

static const char *level_name(LogLevel level) {
    switch (level) {
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        default:        return "?";
    }
}

static void iso_now(char *buf, size_t n) {
    time_t t;
    struct tm tm_buf;
    time(&t);
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    strftime(buf, n, "%Y-%m-%dT%H:%M:%S", &tm_buf);
}

int event_log_open(EventLog *log, const char *path,
                   LogLevel min_level, int echo_stderr) {
    if (!log || !path) return -1;
    memset(log, 0, sizeof(*log));
    log->min_level    = min_level;
    log->echo_stderr  = echo_stderr;

    if (sqlite3_open(path, &log->db) != SQLITE_OK) {
        fprintf(stderr, "event_log: sqlite3_open('%s') 실패: %s\n",
                path, sqlite3_errmsg(log->db));
        sqlite3_close(log->db);
        log->db = NULL;
        return -1;
    }

    /* WAL 모드: 읽기와 쓰기가 서로 막지 않아 대시보드 조회와 충돌 없음 */
    sqlite3_exec(log->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(log->db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    if (sqlite3_exec(log->db, SCHEMA, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "event_log: 스키마 생성 실패: %s\n",
                sqlite3_errmsg(log->db));
        sqlite3_close(log->db);
        log->db = NULL;
        return -1;
    }

    if (sqlite3_prepare_v2(log->db, INSERT_SQL, -1,
                           &log->stmt_ins, NULL) != SQLITE_OK) {
        fprintf(stderr, "event_log: INSERT 준비 실패: %s\n",
                sqlite3_errmsg(log->db));
        sqlite3_close(log->db);
        log->db = NULL;
        return -1;
    }

    return 0;
}

void event_log_close(EventLog *log) {
    if (!log) return;
    if (log->stmt_ins) { sqlite3_finalize(log->stmt_ins); log->stmt_ins = NULL; }
    if (log->db)       { sqlite3_close(log->db);           log->db       = NULL; }
}

void event_log_write(EventLog *log, LogLevel level,
                     const char *module, const char *message) {
    char ts[24];
    time_t now_t;
    double now_d;

    if (!log || !log->db || level < log->min_level) return;

    time(&now_t);
    now_d = (double)now_t;
    iso_now(ts, sizeof(ts));

    /* stderr echo — 실시간 터미널 확인용 */
    if (log->echo_stderr) {
        fprintf(stderr, "%s %-5s %-8s %s\n",
                ts, level_name(level),
                module  ? module  : "",
                message ? message : "");
        fflush(stderr);
    }

    /* SQLite INSERT */
    sqlite3_reset(log->stmt_ins);
    sqlite3_bind_double(log->stmt_ins, 1, now_d);
    sqlite3_bind_text  (log->stmt_ins, 2, ts,                   -1, SQLITE_STATIC);
    sqlite3_bind_text  (log->stmt_ins, 3, level_name(level),    -1, SQLITE_STATIC);
    sqlite3_bind_text  (log->stmt_ins, 4, module  ? module  : "", -1, SQLITE_STATIC);
    sqlite3_bind_text  (log->stmt_ins, 5, message ? message : "", -1, SQLITE_STATIC);
    sqlite3_step(log->stmt_ins);
}

int event_log_count(EventLog *log, LogLevel min_level) {
    sqlite3_stmt *st = NULL;
    int count = 0;
    if (!log || !log->db) return 0;
    if (sqlite3_prepare_v2(log->db,
            "SELECT COUNT(*) FROM events WHERE level >= ?;",
            -1, &st, NULL) != SQLITE_OK) return 0;
    /* level 비교는 문자열 대신 정수 매핑으로 처리합니다 */
    const char *lv = level_name(min_level);
    /* INFO < WARN < ERROR 알파벳 순서와 맞지 않으므로 CASE WHEN으로 변환 */
    sqlite3_finalize(st);

    /* 정수 레벨로 직접 집계 */
    const char *sql =
        "SELECT COUNT(*) FROM events WHERE"
        " CASE level WHEN 'INFO' THEN 0 WHEN 'WARN' THEN 1 ELSE 2 END >= ?;";
    if (sqlite3_prepare_v2(log->db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, (int)min_level);
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return count;
}
