/* Included by server.c after replay_api.h (ReplayResponse, replay_api_error, replay_query_id 를 재사용한다).
 *
 * 점주용 알림 품질 API 세 개:
 *   GET  /api/alerts/summary?days=N        알림 종류별 건수·그림자·점주 확인(맞아요/오탐) 집계
 *   POST /api/alerts/feedback?file=&event=&label=correct|false|clear   알림 한 건에 대한 점주 판단 저장
 *   POST /api/alerts/mode?token=&mode=on|shadow|off[&confirm=1]       알림 종류별 모드 저장(config.json 의 alert_<token>)
 *
 * 왜 서버가 이 일을 하는가: alert_<token> 모드는 C 에 이미 있지만 UI 가 없어 점주가 그림자의 존재도 몰랐고, 오탐 판단은
 * 리플레이 화면에서만 가능해 집계되지 않았다. 집계 SQL 은 이벤트·리플레이가 같은 SQLite 파일이라 JOIN 한 번으로 끝난다.
 *
 * 보안: 두 POST 는 client_thread 의 공통 접근 제어(서브넷 + PIN)를 통과한 요청만 도달한다. 이 파일에서는 입력을 다시
 * 화이트리스트로 검증하고(파일명, 토큰, 모드, 라벨), 응답에는 집계 숫자와 알림 이름만 담는다(메시지 본문·좌표·이미지 없음).
 * 변경은 module=access 로그로 남긴다("누가 이 알림을 껐지?"에 로그 한 줄로 답하기 위함). */

/* detector 의 ALERT_TOKENS(src/main.c)와 같은 목록. 새 토큰을 추가할 때 두 곳을 함께 고친다. */
static const char *const ALERT_TOKEN_LIST[] = {
    "overstay", "unordered_seated", "person_fallen", "person_unaccounted", "person_unaccounted_residue",
    "occupancy_exceeded", "person_motionless", "external_drink", "external_food", "animal_on_chair",
    "animal_on_table", "no_cup_seated", "residue_trash", "residue_spill", "residue_unknown", "door_open",
    "slot_item_urgent", "person_fight", "animal_present",
};

static int alert_token_known(const char *t) {
    size_t i;
    for (i = 0; i < sizeof(ALERT_TOKEN_LIST) / sizeof(ALERT_TOKEN_LIST[0]); ++i)
        if (strcmp(t, ALERT_TOKEN_LIST[i]) == 0) return 1;
    return 0;
}

/* 메시지 첫 단어(공백·':'·'=' 앞). analyze_alert_quality.py 의 토큰 분리 규칙과 같다. */
static void alert_token_of(const char *msg, char *out, size_t size) {
    size_t n = 0;
    while (msg[n] && msg[n] != ' ' && msg[n] != ':' && msg[n] != '=' && n + 1 < size) { out[n] = msg[n]; n++; }
    out[n] = '\0';
}

static int alert_ends_with_shadow(const char *msg) {
    static const char suffix[] = " mode=shadow";
    size_t n = strlen(msg), m = sizeof(suffix) - 1;
    return n >= m && strcmp(msg + n - m, suffix) == 0;
}

typedef struct { char name[48]; int total, shadow, correct, wrong; } AlertTally;
enum { ALERT_TALLY_MAX = 40, ALERT_FILES_MAX = 400 };

static AlertTally *alert_tally_slot(AlertTally *t, int *count, const char *name) {
    int i;
    for (i = 0; i < *count; ++i) if (strcmp(t[i].name, name) == 0) return &t[i];
    if (*count >= ALERT_TALLY_MAX) return NULL;
    memset(&t[*count], 0, sizeof(t[*count]));
    strncpy(t[*count].name, name, sizeof(t[*count].name) - 1);
    return &t[(*count)++];
}

static int alert_has_table(sqlite3 *db, const char *name) {
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

static void alert_tally_file(const char *path, double day_start, AlertTally *tally, int *count, int *today_visible) {
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int has_replay, has_feedback;
    char sql[640];
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) { if (db) sqlite3_close(db); return; }
    sqlite3_busy_timeout(db, 250);
    has_replay = alert_has_table(db, "replay_events");
    has_feedback = alert_has_table(db, "alert_feedback");
    /* 점주 판단(alert_feedback)이 있으면 우선하고, 없으면 리플레이 화면의 검토(review)를 같은 뜻으로 쓴다.
       테이블이 없는 DB(옛 로그·리플레이 미기록)도 있어 존재하는 테이블만 JOIN 한다. */
    snprintf(sql, sizeof(sql),
        "SELECT e.message, e.ts_unix, %s FROM events e %s %s "
        "WHERE e.level IN ('WARN','ERROR') AND e.module IN ('rules','door','slot_monitor','residue','surface')",
        (has_feedback && has_replay) ? "COALESCE(f.label, CASE r.review WHEN 'expected' THEN 'correct' WHEN 'suspected_false' THEN 'false' END)"
        : has_feedback ? "f.label"
        : has_replay ? "CASE r.review WHEN 'expected' THEN 'correct' WHEN 'suspected_false' THEN 'false' END"
        : "NULL",
        has_feedback ? "LEFT JOIN alert_feedback f ON f.event_id=e.id" : "",
        has_replay ? "LEFT JOIN replay_events r ON r.event_id=e.id" : "");
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *msg = (const char *)sqlite3_column_text(st, 0);
            const char *label = (const char *)sqlite3_column_text(st, 2);
            char token[48];
            AlertTally *slot;
            int shadow;
            if (!msg) continue;
            alert_token_of(msg, token, sizeof(token));
            if (!token[0]) continue;
            slot = alert_tally_slot(tally, count, token);
            if (!slot) continue;
            shadow = alert_ends_with_shadow(msg);
            slot->total++;
            if (shadow) slot->shadow++;
            else if (sqlite3_column_double(st, 1) >= day_start) (*today_visible)++;
            if (label && strcmp(label, "correct") == 0) slot->correct++;
            else if (label && strcmp(label, "false") == 0) slot->wrong++;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
}

static int alert_tally_cmp(const void *a, const void *b) {
    return ((const AlertTally *)b)->total - ((const AlertTally *)a)->total;
}

/* 최근 days 일(이름의 YYYYMMDD 가 기준일 이상인) 로그 DB 를 모두 합산한다. 시간별 분할(_partNNNNNN) 파일도 포함된다. */
static void serve_alerts_summary(SOCKET s, const char *query) {
    char daytxt[16], pattern[MAX_PATH], cutoff[16];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    AlertTally tally[ALERT_TALLY_MAX];
    int count = 0, files = 0, days = 7, today_visible = 0, i;
    time_t now = time(NULL), cut_t;
    struct tm tmv;
    double day_start;
    ReplayResponse out = {0};
    query_get(query, "days", daytxt, sizeof(daytxt));
    if (daytxt[0]) { days = atoi(daytxt); if (days < 1) days = 1; if (days > 60) days = 60; }
    localtime_s(&tmv, &now);
    {   /* 오늘 0시 */
        struct tm z = tmv; z.tm_hour = z.tm_min = z.tm_sec = 0; day_start = (double)mktime(&z);
    }
    cut_t = now - (time_t)(days - 1) * 86400;
    localtime_s(&tmv, &cut_t);
    strftime(cutoff, sizeof(cutoff), "%Y%m%d", &tmv);
    snprintf(pattern, sizeof(pattern), "%s\\*.db", g_logs);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char path[MAX_PATH];
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || strstr(fd.cFileName, "_perf.db")) continue;
            if (!safe_filename(fd.cFileName) || strlen(fd.cFileName) < 8 || strncmp(fd.cFileName, cutoff, 8) < 0) continue;
            if (files >= ALERT_FILES_MAX) break;
            snprintf(path, sizeof(path), "%s\\%s", g_logs, fd.cFileName);
            alert_tally_file(path, day_start, tally, &count, &today_visible);
            files++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    qsort(tally, (size_t)count, sizeof(tally[0]), alert_tally_cmp);
    out.size = 16 * 1024; out.data = (char *)malloc(out.size);
    if (!out.data) { replay_api_error(s, 500, "out_of_memory"); return; }
    replay_response_add(&out, "{\"days\":%d,\"files\":%d,\"today_visible\":%d,\"tokens\":[", days, files, today_visible);
    for (i = 0; i < count; ++i)
        replay_response_add(&out, "%s{\"t\":\"%s\",\"total\":%d,\"shadow\":%d,\"correct\":%d,\"false\":%d}",
                            i ? "," : "", tally[i].name, tally[i].total, tally[i].shadow, tally[i].correct, tally[i].wrong);
    replay_response_add(&out, "]}");
    if (out.failed) replay_api_error(s, 500, "response_too_large");
    else { send_header(s, 200, "application/json", (int64_t)out.used); send_all(s, out.data, (int)out.used); }
    free(out.data);
}

/* 알림 한 건에 대한 점주 판단. 이벤트 DB 자체에 작은 테이블로 저장해 리플레이 행(10초 링 안의 알림만 있음)에 의존하지 않는다. */
static void serve_alerts_feedback(SOCKET s, const char *query, const NetAccess *acc) {
    char file[256], label[16], db_path[MAX_PATH], msg[300];
    long long event = replay_query_id(query, "event");
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    query_get(query, "file", file, sizeof(file));
    query_get(query, "label", label, sizeof(label));
    if (!safe_filename(file) || !is_db_file(file) || strstr(file, "_perf.db") || event <= 0 ||
        (strcmp(label, "correct") && strcmp(label, "false") && strcmp(label, "clear"))) {
        replay_api_error(s, 400, "invalid_request");
        return;
    }
    snprintf(db_path, sizeof(db_path), "%s\\%s", g_logs, file);
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) { replay_api_error(s, 404, "log_not_found"); goto done; }
    sqlite3_busy_timeout(db, 500);
    if (sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS alert_feedback(event_id INTEGER PRIMARY KEY, label TEXT NOT NULL, ts REAL NOT NULL)",
                     NULL, NULL, NULL) != SQLITE_OK) { replay_api_error(s, 500, "feedback_save_failed"); goto done; }
    /* 존재하지 않는 이벤트 id 는 거부한다(아무 id 나 쌓여 집계가 오염되는 것을 막음). */
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM events WHERE id=?", -1, &st, NULL) != SQLITE_OK) { replay_api_error(s, 500, "read_failed"); goto done; }
    sqlite3_bind_int64(st, 1, event);
    if (sqlite3_step(st) != SQLITE_ROW) { replay_api_error(s, 404, "event_not_found"); goto done; }
    sqlite3_finalize(st); st = NULL;
    if (strcmp(label, "clear") == 0) {
        if (sqlite3_prepare_v2(db, "DELETE FROM alert_feedback WHERE event_id=?", -1, &st, NULL) != SQLITE_OK) { replay_api_error(s, 500, "feedback_save_failed"); goto done; }
        sqlite3_bind_int64(st, 1, event);
    } else {
        if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO alert_feedback(event_id,label,ts) VALUES(?,?,?)", -1, &st, NULL) != SQLITE_OK) { replay_api_error(s, 500, "feedback_save_failed"); goto done; }
        sqlite3_bind_int64(st, 1, event);
        sqlite3_bind_text(st, 2, label, -1, SQLITE_STATIC);
        sqlite3_bind_double(st, 3, (double)time(NULL));
    }
    if (sqlite3_step(st) != SQLITE_DONE) { replay_api_error(s, 500, "feedback_save_failed"); goto done; }
    sqlite3_finalize(st); st = NULL;
    snprintf(msg, sizeof(msg), "alert feedback by %s (%s): event=%lld label=%s file=%s",
             acc ? acc->ip : "?", acc ? netaccess_tier_name(acc->tier) : "?", event, label, file);
    sqlite3_close(db); db = NULL;
    access_log_write(0, msg);
    send_header(s, 200, "application/json", 11);
    send_all(s, "{\"ok\":true}", 11);
done:
    if (st) sqlite3_finalize(st);
    if (db) sqlite3_close(db);
}

/* 알림 종류별 모드. config.json 의 alert_<token> 한 키만 바꾸고 나머지 키는 serve_config_post 의 병합 로직이 보존한다.
 * person_fallen 을 끄는 것은 가장 위험한 변경이라 confirm=1 이 없으면 거부한다(화면에서 확인 창을 거친 뒤에만 도달). */
static void serve_alerts_mode(SOCKET s, const char *query, const NetAccess *acc) {
    char token[48], mode[12], confirm[4], body[160];
    int n;
    query_get(query, "token", token, sizeof(token));
    query_get(query, "mode", mode, sizeof(mode));
    query_get(query, "confirm", confirm, sizeof(confirm));
    if (!alert_token_known(token) || (strcmp(mode, "on") && strcmp(mode, "shadow") && strcmp(mode, "off"))) {
        replay_api_error(s, 400, "invalid_request");
        return;
    }
    if (strcmp(token, "person_fallen") == 0 && strcmp(mode, "off") == 0 && strcmp(confirm, "1") != 0) {
        replay_api_error(s, 409, "confirm_required");
        return;
    }
    n = snprintf(body, sizeof(body), "{\"alert_%s\":\"%s\"}", token, mode);
    serve_config_post(s, body, n, acc);
}

/* POST /api/config/reset[?keep_alerts=1][&keep_pin=1]
 * 일반 설정(감지 민감도·체류 시간·보호 화면 모양·자동 캡처·재시작 등)을 기본값으로 되돌린다: config.json 을 비우고
 * 반드시 지켜야 하는 키만 다시 쓴다. 지우기 전에 config.json 을 config.json.bak-<시각> 으로 복사해 되돌릴 수 있게 한다.
 *   - 항상 유지: stream_port(대시보드가 detector 에 접속하는 주소가 사라지면 화면이 먹통이 된다)
 *   - 선택 유지: alert_<이름>(알림 모드), access_pin
 * 문 위치·기준 사진·테이블 영역 같은 "장소" 데이터는 건드리지 않는다(그건 "장소 초기화"의 몫). 로그·증거 사진도 지우지 않는다.
 * detector 는 config.json 을 2초 안에 다시 읽으므로 재시작은 필요 없다. PIN 규칙은 다른 POST 와 같다(client_thread 에서 통과한 요청만 온다). */
static void serve_config_reset(SOCKET s, const char *query, const NetAccess *acc) {
    char keep_alerts[4], keep_pin[4], backup[MAX_PATH], stamp[24], *out;
    Config existing;
    size_t i, cap = 8192, len = 0;
    time_t now = time(NULL);
    struct tm tmv;
    int ka, kp, wrote;
    FILE *f;
    query_get(query, "keep_alerts", keep_alerts, sizeof(keep_alerts));
    query_get(query, "keep_pin", keep_pin, sizeof(keep_pin));
    ka = strcmp(keep_alerts, "1") == 0;
    kp = strcmp(keep_pin, "1") == 0;
    localtime_s(&tmv, &now);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
    /* 같은 초에 두 번 눌러도 백업이 겹치지 않도록 밀리초를 붙인다 */
    snprintf(backup, sizeof(backup), "%s.bak-%s_%03lu", g_config_path, stamp, (unsigned long)(GetTickCount64() % 1000));
    if (!CopyFileA(g_config_path, backup, TRUE)) {       /* 백업이 안 되면 지우지 않는다 */
        if (GetFileAttributesA(g_config_path) != INVALID_FILE_ATTRIBUTES) { replay_api_error(s, 500, "backup_failed"); return; }
        backup[0] = '\0';                                /* 설정 파일이 아직 없으면 백업할 것도 없다 */
    }
    out = (char *)malloc(cap);
    if (!out) { replay_api_error(s, 500, "out_of_memory"); return; }
    out[len++] = '{';
    out[len] = '\0';
    wrote = 0;
    if (config_load(&existing, g_config_path, NULL, 0) == 0) {
        for (i = 0; i < existing.count; ++i) {
            const char *k = existing.keys[i], *v = existing.values[i];
            int keep, n;
            if (!k || !v) continue;
            keep = strcmp(k, "stream_port") == 0 || (ka && strncmp(k, "alert_", 6) == 0) || (kp && strcmp(k, "access_pin") == 0);
            if (!keep) continue;
            n = json_pair_append(out + len, cap - len - 2, k, v);   /* ,"key":value */
            if (n <= 0) continue;
            if (!wrote) { memmove(out + len, out + len + 1, (size_t)n - 1); n -= 1; }   /* 첫 항목은 앞의 쉼표를 뺀다 */
            len += (size_t)n;
            wrote = 1;
        }
        config_destroy(&existing);
    }
    out[len++] = '}';
    out[len] = '\0';
    f = fopen(g_config_path, "w");
    if (!f) { free(out); replay_api_error(s, 500, "write_failed"); return; }
    fwrite(out, 1, len, f);
    fclose(f);
    free(out);
    {
        char msg[700];
        snprintf(msg, sizeof(msg), "config reset by %s (%s): keep_alerts=%d keep_pin=%d backup=%s",
                 acc ? acc->ip : "?", acc ? netaccess_tier_name(acc->tier) : "?", ka, kp, backup[0] ? backup : "(none)");
        access_log_write(1, msg);
    }
    send_header(s, 200, "application/json", 11);
    send_all(s, "{\"ok\":true}", 11);
}
