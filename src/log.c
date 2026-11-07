#include "log.h"

/* SQLite 스레드 안전 모드: SQLITE_THREADSAFE=1 (기본값) — 직렬화 모드.
 * 이 프로세스의 모든 이벤트는 메인 스레드에서만 기록하므로 충분합니다. */
#define SQLITE_THREADSAFE 1
#include "../third_party/sqlite/sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
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

/* 분석용 부가 열. 기존 파일(옛 빌드가 만든 DB)에도 ALTER 로 붙인다 — 그래야 같은 경로를 다시 열어도 INSERT 가 실패하지 않는다. */
static const struct { const char *name, *type; } EXTRA_COLUMNS[] = {
    {"run", "INTEGER"}, {"mono", "REAL"}, {"frame", "INTEGER"}, {"kind", "TEXT"}, {"track", "INTEGER"}, {"data", "TEXT"}};

static const char *INSERT_SQL =
    "INSERT INTO events(ts_unix,ts_iso,level,module,message,run,mono,frame,kind,track,data)"
    " VALUES(?,?,?,?,?,?,?,?,?,?,?);";

static void ensure_extra_columns(sqlite3 *db) {
    size_t i;
    for (i = 0; i < sizeof(EXTRA_COLUMNS) / sizeof(EXTRA_COLUMNS[0]); ++i) {
        sqlite3_stmt *st = NULL;
        int present = 0;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_table_info('events') WHERE name=?;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, EXTRA_COLUMNS[i].name, -1, SQLITE_STATIC);
            present = sqlite3_step(st) == SQLITE_ROW;
        }
        sqlite3_finalize(st);
        if (!present) {
            char sql[96];
            snprintf(sql, sizeof(sql), "ALTER TABLE events ADD COLUMN %s %s;", EXTRA_COLUMNS[i].name, EXTRA_COLUMNS[i].type);
            sqlite3_exec(db, sql, NULL, NULL, NULL);
        }
    }
    /* kind 인덱스: 종류별 집계(시간별 요약, 분석)를 풀스캔 없이. 행당 ~25바이트. */
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_events_kind ON events(kind);", NULL, NULL, NULL);
}

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
    sqlite3_exec(log->db, "PRAGMA synchronous=FULL;", NULL, NULL, NULL);
    /* 대시보드 프로세스가 접근 로그를 같은 DB 에 씁니다. WAL 은 쓰기를 직렬화하므로
       두 쓰기가 겹치는 순간 한쪽이 SQLITE_BUSY 를 받습니다. 대기 없이 그냥 실패하면
       detector 이벤트가 조용히 사라지므로 짧게 기다립니다. 250ms 는 프레임 처리를
       눈에 띄게 막지 않으면서 상대 쓰기(수 ms)를 넉넉히 넘기는 값입니다. */
    sqlite3_busy_timeout(log->db, 250);

    if (sqlite3_exec(log->db, SCHEMA, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "event_log: 스키마 생성 실패: %s\n",
                sqlite3_errmsg(log->db));
        sqlite3_close(log->db);
        log->db = NULL;
        return -1;
    }

    ensure_extra_columns(log->db);

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

/* ── 알림 모드 표(프로세스 전역, log.h 참조) ───────────────────────────────────── */
#define ALERT_TABLE_MAX 32
#define ALERT_TOKEN_MAX 48
static struct { char token[ALERT_TOKEN_MAX]; AlertMode mode; } g_alert[ALERT_TABLE_MAX];
static int g_alert_count = 0;

/* 메시지 첫 단어를 token 에 복사한다. 단어는 공백, ':' , '=' 에서 끝난다(예: "door_open: ..."). */
static void first_token(const char *msg, char *token, size_t token_size) {
    size_t n = 0;
    if (msg)
        while (msg[n] && msg[n] != ' ' && msg[n] != ':' && msg[n] != '=' && n + 1 < token_size) {
            token[n] = msg[n];
            ++n;
        }
    token[n] = '\0';
}

void event_log_set_alert_mode(const char *token, AlertMode mode) {
    int i;
    if (!token || !token[0]) return;
    for (i = 0; i < g_alert_count; ++i)
        if (strcmp(g_alert[i].token, token) == 0) {
            if (mode == ALERT_ON) g_alert[i] = g_alert[--g_alert_count];  /* 해제: 마지막 항목으로 메운다 */
            else g_alert[i].mode = mode;
            return;
        }
    if (mode == ALERT_ON || g_alert_count >= ALERT_TABLE_MAX) return;
    strncpy(g_alert[g_alert_count].token, token, ALERT_TOKEN_MAX - 1);
    g_alert[g_alert_count].token[ALERT_TOKEN_MAX - 1] = '\0';
    g_alert[g_alert_count].mode = mode;
    ++g_alert_count;
}

void event_log_clear_alert_modes(void) { g_alert_count = 0; }

int event_log_alert_summary(char *out, unsigned long out_size) {
    static const struct { AlertMode mode; const char *label; } kinds[] = {
        { ALERT_SHADOW, "shadow" }, { ALERT_OFF, "off" } };
    unsigned long pos = 0;
    int i, k, listed = 0;
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    for (k = 0; k < 2; ++k) {
        int first = 1;
        for (i = 0; i < g_alert_count; ++i) {
            if (g_alert[i].mode != kinds[k].mode) continue;
            pos += (unsigned long)snprintf(out + pos, pos < out_size ? out_size - pos : 0, "%s%s%s%s",
                                           first && pos ? " " : "", first ? kinds[k].label : "",
                                           first ? "=" : ",", g_alert[i].token);
            first = 0;
            ++listed;
            if (pos >= out_size) { out[out_size - 1] = '\0'; return listed; }
        }
    }
    return listed;
}

/* ── 구조화 열 계산 ───────────────────────────────────────────────────────────── */
#define EVENT_DATA_MAX 400

/* "12", "-3.5" 같은 JSON 숫자 + 선택적 단위(s, ms, px, %). 숫자 부분 길이를 num_len 에 돌려주고 1, 아니면 0.
 * "007" 처럼 앞자리 0 은 JSON 숫자가 아니므로 문자열로 둔다. */
static int number_with_unit(const char *v, size_t n, size_t *num_len) {
    size_t i = 0, digits = 0;
    if (i < n && v[i] == '-') ++i;
    if (i < n && v[i] == '0' && i + 1 < n && v[i + 1] >= '0' && v[i + 1] <= '9') return 0;
    while (i < n && v[i] >= '0' && v[i] <= '9') { ++i; ++digits; }
    if (i < n && v[i] == '.') {
        ++i;
        while (i < n && v[i] >= '0' && v[i] <= '9') { ++i; ++digits; }
    }
    if (!digits) return 0;
    *num_len = i;
    if (i == n) return 1;
    if (n - i == 1 && (v[i] == 's' || v[i] == '%')) return 1;
    if (n - i == 2 && ((v[i] == 'm' && v[i + 1] == 's') || (v[i] == 'p' && v[i + 1] == 'x'))) return 1;
    return 0;
}

/*
 * 메시지에서 kind/track/data 를 뽑는다. 호출 지점 130여 곳을 바꾸지 않고 구조화 열을 얻으려고 메시지를 파싱한다 —
 * 메시지가 이미 "토큰 key=value ..." 규칙으로 쓰여 있다(src/rules.c 등). 파싱이 어긋나도 message 열은 그대로다.
 */
static void derive_fields(const char *module, const char *msg, char *kind, size_t kind_size,
                          char *data, size_t data_size, long long *track, int *has_track) {
    const char *p;
    size_t klen, used = 1;
    *has_track = 0;
    first_token(msg, kind, kind_size);
    klen = strlen(kind);
    if (msg[klen] == '=') { snprintf(kind, kind_size, "%s", module ? module : ""); p = msg; }
    else p = msg + klen;
    data[0] = '{'; data[1] = '\0';
    while (*p) {
        const char *s, *eq, *v;
        size_t n, key_len, val_len, num_len, i, need;
        char piece[160];
        while (*p == ' ') ++p;
        s = p;
        while (*p && *p != ' ') ++p;
        n = (size_t)(p - s);
        if (!n) break;
        eq = (const char *)memchr(s, '=', n);
        if (!eq || eq == s) continue;
        key_len = (size_t)(eq - s);
        v = eq + 1;
        val_len = (size_t)((s + n) - v);
        if (key_len > 32 || val_len == 0 || val_len > 64) continue;
        for (i = 0; i < key_len; ++i) {
            char c = s[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) break;
        }
        if (i < key_len) continue;
        {
            char key[40];
            memcpy(key, s, key_len); key[key_len] = '\0';
            snprintf(piece, sizeof(piece), "\"%s\":", key);
            if (strstr(data, piece)) continue;                       /* 같은 키는 처음 값만 */
            need = strlen(piece);
            if (number_with_unit(v, val_len, &num_len)) {
                memcpy(piece + need, v, num_len); need += num_len;
                if (strcmp(key, "track") == 0 && !*has_track) {
                    int integral = 1;
                    for (i = 0; i < num_len; ++i) if (v[i] == '.') integral = 0;
                    if (integral) { *track = strtoll(v, NULL, 10); *has_track = 1; }
                }
            } else {
                piece[need++] = '"';
                for (i = 0; i < val_len && need + 3 < sizeof(piece); ++i) {
                    unsigned char c = (unsigned char)v[i];
                    if (c == '"' || c == '\\') piece[need++] = '\\';
                    if (c >= 0x20) piece[need++] = (char)c;
                }
                piece[need++] = '"';
            }
        }
        if (used + need + 2 > data_size || used + need + 2 > EVENT_DATA_MAX) break;   /* 상한: 남는 쌍은 생략 */
        if (used > 1) data[used++] = ',';
        memcpy(data + used, piece, need); used += need;
        data[used] = '\0';
    }
    if (used > 1) { data[used++] = '}'; data[used] = '\0'; }
    else data[0] = '\0';
}

static AlertMode alert_mode_of(const char *message) {
    char token[ALERT_TOKEN_MAX];
    int i;
    if (g_alert_count == 0) return ALERT_ON;
    first_token(message, token, sizeof(token));
    for (i = 0; i < g_alert_count; ++i)
        if (strcmp(g_alert[i].token, token) == 0) return g_alert[i].mode;
    return ALERT_ON;
}

void event_log_write(EventLog *log, LogLevel level,
                     const char *module, const char *message) {
    char ts[24];
    char shadowed[1100];
    time_t now_t;
    double now_d;

    if (!log || !log->db || level < log->min_level) return;

    switch (alert_mode_of(message)) {
        case ALERT_OFF:
            return;
        case ALERT_SHADOW:
            /* 원문을 자르지 않도록 접미사 길이만큼 여유를 두고 복사한다. 이 버퍼는 이 호출 안에서만 쓰인다. */
            snprintf(shadowed, sizeof(shadowed), "%.*s%s",
                     (int)(sizeof(shadowed) - sizeof(ALERT_SHADOW_SUFFIX)), message ? message : "",
                     ALERT_SHADOW_SUFFIX);
            message = shadowed;
            break;
        default:
            break;
    }

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
    {
        char kind[48], data[EVENT_DATA_MAX + 8];
        long long track = 0;
        int has_track = 0;
        derive_fields(module, message ? message : "", kind, sizeof(kind), data, sizeof(data), &track, &has_track);
        /* data 는 분석 대상 모듈만. 시작·종료·스로틀 같은 일회성 안내문은 message 와 같은 내용을 한 번 더 저장할 뿐이다(용량 낭비). */
        if (!module || !(!strcmp(module, "rules") || !strcmp(module, "door") || !strcmp(module, "slot_monitor") ||
                         !strcmp(module, "residue") || !strcmp(module, "surface") || !strcmp(module, "camera") ||
                         !strcmp(module, "summary") || !strcmp(module, "config")))
            data[0] = '\0';
        sqlite3_reset(log->stmt_ins);
        sqlite3_bind_double(log->stmt_ins, 1, now_d);
        sqlite3_bind_text  (log->stmt_ins, 2, ts,                   -1, SQLITE_STATIC);
        sqlite3_bind_text  (log->stmt_ins, 3, level_name(level),    -1, SQLITE_STATIC);
        sqlite3_bind_text  (log->stmt_ins, 4, module  ? module  : "", -1, SQLITE_STATIC);
        sqlite3_bind_text  (log->stmt_ins, 5, message ? message : "", -1, SQLITE_STATIC);
        if (log->run_id > 0) sqlite3_bind_int64(log->stmt_ins, 6, log->run_id); else sqlite3_bind_null(log->stmt_ins, 6);
        if (log->ctx_valid) {
            sqlite3_bind_double(log->stmt_ins, 7, log->ctx_mono);
            sqlite3_bind_int64 (log->stmt_ins, 8, log->ctx_frame);
        } else {
            sqlite3_bind_null(log->stmt_ins, 7);
            sqlite3_bind_null(log->stmt_ins, 8);
        }
        sqlite3_bind_text(log->stmt_ins, 9, kind, -1, SQLITE_STATIC);
        if (has_track) sqlite3_bind_int64(log->stmt_ins, 10, track); else sqlite3_bind_null(log->stmt_ins, 10);
        if (data[0]) sqlite3_bind_text(log->stmt_ins, 11, data, -1, SQLITE_STATIC); else sqlite3_bind_null(log->stmt_ins, 11);
    }
    if (sqlite3_step(log->stmt_ins) != SQLITE_DONE) {
        /* 로그 기록 실패는 로그로 남길 수 없으므로 stderr 가 마지막 수단입니다. */
        fprintf(stderr, "event_log: insert failed (%s): %s %s\n",
                sqlite3_errmsg(log->db), module ? module : "", message ? message : "");
    } else if(log->observer) {
        log->observer(log->observer_context,sqlite3_last_insert_rowid(log->db),level,module?module:"",message?message:"");
    }
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

void event_log_set_run(EventLog *log, long long run_id) { if (log) log->run_id = run_id; }

void event_log_set_context(EventLog *log, long long frame, double mono) {
    if (!log) return;
    log->ctx_frame = frame; log->ctx_mono = mono; log->ctx_valid = 1;
}

void event_log_clear_context(EventLog *log) { if (log) log->ctx_valid = 0; }

int event_log_write_run_info(EventLog *log, const char *manifest_json) {
    sqlite3_stmt *st = NULL;
    char ts[24];
    int rc;
    if (!log || !log->db || !manifest_json || strlen(manifest_json) > 12288) return -1;
    if (sqlite3_exec(log->db,
            "CREATE TABLE IF NOT EXISTS run_info(id INTEGER PRIMARY KEY, run INTEGER, ts_iso TEXT, manifest TEXT NOT NULL);",
            NULL, NULL, NULL) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v2(log->db, "INSERT INTO run_info(run,ts_iso,manifest) VALUES(?,?,?);", -1, &st, NULL) != SQLITE_OK) return -1;
    iso_now(ts, sizeof(ts));
    if (log->run_id > 0) sqlite3_bind_int64(st, 1, log->run_id); else sqlite3_bind_null(st, 1);
    sqlite3_bind_text(st, 2, ts, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, manifest_json, -1, SQLITE_STATIC);
    rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return rc;
}

long long event_log_last_id(EventLog *log) {
    sqlite3_stmt *st = NULL;
    long long id = 0;
    if (!log || !log->db) return 0;
    if (sqlite3_prepare_v2(log->db, "SELECT COALESCE(MAX(id),0) FROM events;", -1, &st, NULL) != SQLITE_OK) return 0;
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return id;
}

int event_log_kind_counts(EventLog *log, long long since_id, EventKindCount *out, int max) {
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!log || !log->db || !out || max <= 0) return 0;
    if (sqlite3_prepare_v2(log->db,
            "SELECT kind, COUNT(*), COUNT(DISTINCT track) FROM events"
            " WHERE id > ? AND kind IS NOT NULL AND level IN ('WARN','ERROR')"
            "   AND module IN ('rules','door','slot_monitor','residue','surface')"
            " GROUP BY kind ORDER BY COUNT(*) DESC, kind LIMIT ?;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, since_id);
    sqlite3_bind_int(st, 2, max);
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *k = sqlite3_column_text(st, 0);
        snprintf(out[n].kind, sizeof(out[n].kind), "%s", k ? (const char *)k : "");
        out[n].count = sqlite3_column_int(st, 1);
        out[n].tracks = sqlite3_column_int(st, 2);
        ++n;
    }
    sqlite3_finalize(st);
    return n;
}
