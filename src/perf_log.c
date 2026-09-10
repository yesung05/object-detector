#include "perf_log.h"
#include "../third_party/sqlite/sqlite3.h"

#include <stdio.h>
#include <time.h>
#include <string.h>

/*
 * WAL 모드를 쓰는 이유: 대시보드가 SELECT하는 동안 detector가 INSERT할 수 있어야 합니다.
 * 기본 저널 모드에서는 쓰기 중 읽기가 블록됩니다 (SQLite 공유 잠금 규칙).
 */
static int enable_wal(sqlite3 *db) {
    return sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
}

static const char *CREATE_SQL =
    "CREATE TABLE IF NOT EXISTS perf ("
    "  id          INTEGER PRIMARY KEY,"
    "  ts_iso      TEXT    NOT NULL,"
    "  cpu_pct     INTEGER,"   /* 단일 코어 기준 % */
    "  cpu_temp    INTEGER,"   /* 섭씨, -1이면 미지원 */
    "  cam_fps     REAL,"      /* 카메라 입력 EMA FPS */
    "  inf_fps     REAL,"      /* 추론 EMA FPS */
    "  mem_kb      INTEGER,"   /* 프로세스 RSS KB, -1이면 미지원 */
    "  gray_ms     REAL,"      /* 주기 내 gray 분석 누적 ms */
    "  door_ms     REAL,"
    "  residue_ms  REAL,"
    "  stream_ms   REAL,"
    "  draw_ms     REAL,"
    "  gate_l0     INTEGER,"   /* 영업시간 밖 건너뜀 */
    "  gate_l1     INTEGER,"   /* 변화 없음 건너뜀 */
    "  gate_l2     INTEGER,"   /* ignore_roi 안 건너뜀 */
    "  gate_l3     INTEGER,"   /* 트랙 안 변화, 추적기로 대체 */
    "  inf_runs    INTEGER,"   /* Tier 1 추론 횟수 */
    "  obj_runs    INTEGER"    /* Tier 2 추론 횟수 */
    ");";

static const char *INSERT_SQL =
    "INSERT INTO perf"
    " (ts_iso,cpu_pct,cpu_temp,cam_fps,inf_fps,mem_kb,"
    "  gray_ms,door_ms,residue_ms,stream_ms,draw_ms,"
    "  gate_l0,gate_l1,gate_l2,gate_l3,inf_runs,obj_runs)"
    " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

int perf_log_open(PerfLog *pl, const char *path) {
    memset(pl, 0, sizeof(*pl));

    if (sqlite3_open(path, &pl->db) != SQLITE_OK) {
        fprintf(stderr, "perf_log: sqlite3_open(%s) failed: %s\n",
                path, sqlite3_errmsg(pl->db));
        sqlite3_close(pl->db);
        pl->db = NULL;
        return -1;
    }
    enable_wal(pl->db);

    if (sqlite3_exec(pl->db, CREATE_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "perf_log: CREATE TABLE failed: %s\n",
                sqlite3_errmsg(pl->db));
        sqlite3_close(pl->db);
        pl->db = NULL;
        return -1;
    }

    if (sqlite3_prepare_v2(pl->db, INSERT_SQL, -1, &pl->stmt_ins, NULL)
        != SQLITE_OK) {
        fprintf(stderr, "perf_log: prepare INSERT failed: %s\n",
                sqlite3_errmsg(pl->db));
        sqlite3_close(pl->db);
        pl->db = NULL;
        return -1;
    }

    return 0;
}

void perf_log_close(PerfLog *pl) {
    if (!pl) return;
    if (pl->stmt_ins) sqlite3_finalize(pl->stmt_ins);
    if (pl->db)       sqlite3_close(pl->db);
    memset(pl, 0, sizeof(*pl));
}

void perf_log_write(PerfLog *pl, const PerfMetrics *m) {
    if (!pl || !pl->db || !pl->stmt_ins) return;

    /* ISO 8601 타임스탬프 */
    char ts[32];
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    if (tm_info)
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm_info);
    else
        ts[0] = '\0';

    sqlite3_stmt *s = pl->stmt_ins;
    sqlite3_reset(s);
    sqlite3_bind_text   (s,  1, ts,          -1, SQLITE_TRANSIENT);
    sqlite3_bind_int    (s,  2, m->cpu_pct);
    sqlite3_bind_int    (s,  3, m->cpu_temp);
    sqlite3_bind_double (s,  4, m->cam_fps);
    sqlite3_bind_double (s,  5, m->inf_fps);
    sqlite3_bind_int64  (s,  6, (sqlite3_int64)m->mem_kb);
    sqlite3_bind_double (s,  7, m->gray_ms);
    sqlite3_bind_double (s,  8, m->door_ms);
    sqlite3_bind_double (s,  9, m->residue_ms);
    sqlite3_bind_double (s, 10, m->stream_ms);
    sqlite3_bind_double (s, 11, m->draw_ms);
    sqlite3_bind_int64  (s, 12, (sqlite3_int64)m->gate_l0);
    sqlite3_bind_int64  (s, 13, (sqlite3_int64)m->gate_l1);
    sqlite3_bind_int64  (s, 14, (sqlite3_int64)m->gate_l2);
    sqlite3_bind_int64  (s, 15, (sqlite3_int64)m->gate_l3);
    sqlite3_bind_int64  (s, 16, (sqlite3_int64)m->inf_runs);
    sqlite3_bind_int64  (s, 17, (sqlite3_int64)m->obj_runs);

    if (sqlite3_step(s) != SQLITE_DONE)
        fprintf(stderr, "perf_log: INSERT failed: %s\n",
                sqlite3_errmsg(pl->db));
}
