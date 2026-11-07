#include "perf_log.h"
#include "../third_party/sqlite/sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
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

/* 로깅 v2 추가 열(perf_log.h 참조). 옛 파일에도 ALTER 로 붙여 INSERT 가 실패하지 않게 한다. */
static const struct { const char *name, *type; } EXTRA_COLUMNS[] = {
    {"pre_ms", "REAL"}, {"inf_ms", "REAL"}, {"post_ms", "REAL"}, {"t2_ms", "REAL"}, {"track_ms", "REAL"},
    {"frames", "INTEGER"}, {"cam_bad_s", "REAL"}, {"wait_frames", "INTEGER"}, {"dropped", "INTEGER"}};

static void ensure_extra_columns(sqlite3 *db) {
    size_t i;
    for (i = 0; i < sizeof(EXTRA_COLUMNS) / sizeof(EXTRA_COLUMNS[0]); ++i) {
        sqlite3_stmt *st = NULL;
        int present = 0;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_table_info('perf') WHERE name=?;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, EXTRA_COLUMNS[i].name, -1, SQLITE_STATIC);
            present = sqlite3_step(st) == SQLITE_ROW;
        }
        sqlite3_finalize(st);
        if (!present) {
            char sql[96];
            snprintf(sql, sizeof(sql), "ALTER TABLE perf ADD COLUMN %s %s;", EXTRA_COLUMNS[i].name, EXTRA_COLUMNS[i].type);
            sqlite3_exec(db, sql, NULL, NULL, NULL);
        }
    }
}

static const char *INSERT_SQL =
    "INSERT INTO perf"
    " (ts_iso,cpu_pct,cpu_temp,cam_fps,inf_fps,mem_kb,"
    "  gray_ms,door_ms,residue_ms,stream_ms,draw_ms,"
    "  gate_l0,gate_l1,gate_l2,gate_l3,inf_runs,obj_runs,"
    "  pre_ms,inf_ms,post_ms,t2_ms,track_ms,frames,cam_bad_s,wait_frames,dropped)"
    " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";

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
    sqlite3_busy_timeout(pl->db, 250);
    sqlite3_exec(pl->db, "PRAGMA synchronous=FULL;", NULL, NULL, NULL);

    if (sqlite3_exec(pl->db, CREATE_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "perf_log: CREATE TABLE failed: %s\n",
                sqlite3_errmsg(pl->db));
        sqlite3_close(pl->db);
        pl->db = NULL;
        return -1;
    }

    ensure_extra_columns(pl->db);

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
    sqlite3_bind_double (s, 18, m->pre_ms);
    sqlite3_bind_double (s, 19, m->inf_ms);
    sqlite3_bind_double (s, 20, m->post_ms);
    sqlite3_bind_double (s, 21, m->t2_ms);
    sqlite3_bind_double (s, 22, m->track_ms);
    sqlite3_bind_int64  (s, 23, (sqlite3_int64)m->frames);
    sqlite3_bind_double (s, 24, m->cam_bad_s);
    sqlite3_bind_int64  (s, 25, (sqlite3_int64)m->wait_frames);
    sqlite3_bind_int64  (s, 26, (sqlite3_int64)m->dropped);

    if (sqlite3_step(s) != SQLITE_DONE)
        fprintf(stderr, "perf_log: INSERT failed: %s\n",
                sqlite3_errmsg(pl->db));
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int cmp_double(const void *a, const void *b) { double d = *(const double *)a - *(const double *)b; return (d > 0) - (d < 0); }

int perf_log_summary(PerfLog *pl, long long since_id, PerfSummary *out) {
    enum { MAX_ROWS = 512 };
    int cpu[MAX_ROWS];
    double fps[MAX_ROWS];
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!pl || !pl->db) return -1;
    if (sqlite3_prepare_v2(pl->db,
            "SELECT id, cpu_pct, cam_fps, inf_runs, inf_ms, wait_frames, dropped, cam_bad_s, frames FROM perf"
            " WHERE id > ? ORDER BY id LIMIT 512;", -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, since_id);
    while (n < MAX_ROWS && sqlite3_step(st) == SQLITE_ROW) {
        out->last_id = (long)sqlite3_column_int64(st, 0);
        cpu[n] = sqlite3_column_int(st, 1);
        fps[n] = sqlite3_column_double(st, 2);
        out->inf_runs    += (long)sqlite3_column_int64(st, 3);
        out->inf_ms      += sqlite3_column_double(st, 4);
        out->wait_frames += (long)sqlite3_column_int64(st, 5);
        out->dropped     += (long)sqlite3_column_int64(st, 6);
        out->cam_bad_s   += sqlite3_column_double(st, 7);
        out->frames      += (long)sqlite3_column_int64(st, 8);
        if (cpu[n] > out->cpu_max) out->cpu_max = cpu[n];
        ++n;
    }
    sqlite3_finalize(st);
    out->rows = n;
    if (n > 0) {
        qsort(cpu, (size_t)n, sizeof(cpu[0]), cmp_int);
        qsort(fps, (size_t)n, sizeof(fps[0]), cmp_double);
        out->cpu_med = cpu[n / 2];
        out->fps_med = fps[n / 2];
    }
    return 0;
}
