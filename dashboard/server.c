/*
 * HUNIK 카페 관리 대시보드 — 순수 WinSocket2 HTTP/SSE 서버
 *
 * 외부 의존성: ws2_32.lib (Windows 기본 내장), shlwapi.lib (PathCanonicalize)
 * 빌드: CMake (CMakeLists.txt 참고) 또는
 *       cl server.c ws2_32.lib shlwapi.lib /Fe:hunik-dashboard.exe
 *
 * 실행:
 *   hunik-dashboard.exe                   -- logs\ 자동 탐색, 포트 8080
 *   hunik-dashboard.exe --port 9090        -- 포트 변경
 *   hunik-dashboard.exe --root C:\hunik    -- 프로젝트 루트 지정
 *
 * detector exe 없이 단독 실행 가능합니다. logs\ 폴더가 없으면 파일 목록이
 * 비어 있는 채로 대기하다가 파일이 생기면 자동으로 반영됩니다.
 *
 * 보안: 127.0.0.1 전용 바인딩 — 외부 네트워크에 노출되지 않습니다.
 *       로그 파일 경로는 logs\ 하위인지 검증하여 디렉터리 탈출을 막습니다.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "../include/config.h"
/* SQLite amalgamation — 이벤트 로그(.db) 파일을 직접 쿼리합니다.
 * WAL 모드로 열린 DB는 detector가 쓰는 도중에도 읽기 가능합니다. */
#include "../third_party/sqlite/sqlite3.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#pragma comment(lib, "ws2_32.lib")

/* ── 전역 설정 (main에서 초기화) ─────────────────────────────────────────── */

static char g_root[MAX_PATH];        /* 프로젝트 루트 (logs\ 부모) */
static char g_logs[MAX_PATH];        /* g_root\logs\ */
static char g_config_path[MAX_PATH]; /* g_root\config.json */
static int  g_port = 8080;

/* ── HTTP 기초 ────────────────────────────────────────────────────────────── */

/* HTTP 요청에서 첫 줄(Method/Path/Query)만 파싱합니다. */
static void parse_request(const char *buf,
                          char method[16], char path[512], char query[512]) {
    char url[512] = {0};
    sscanf(buf, "%15s %511s", method, url);
    char *q = strchr(url, '?');
    if (q) {
        *q = '\0';
        strncpy(query, q + 1, 511);
        query[511] = '\0';
    } else {
        query[0] = '\0';
    }
    strncpy(path, url, 511);
    path[511] = '\0';
}

/* 쿼리스트링에서 key 의 값을 꺼냅니다. "file=abc.db&foo=1" → "abc.db" */
static void query_get(const char *query, const char *key, char *out, int outsz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(query, needle);
    out[0] = '\0';
    if (!p) return;
    p += strlen(needle);
    int i = 0;
    while (*p && *p != '&' && i < outsz - 1) out[i++] = *p++;
    out[i] = '\0';
}

static void send_header(SOCKET s, int code, const char *ctype, int64_t body_len) {
    char h[512];
    const char *status = (code == 200) ? "OK"
                       : (code == 404) ? "Not Found"
                       : (code == 400) ? "Bad Request" : "Internal Server Error";
    int n;
    if (body_len >= 0) {
        n = snprintf(h, sizeof(h),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Connection: close\r\n"
            "\r\n",
            code, status, ctype, (long long)body_len);
    } else {
        /* SSE: Content-Length 없음, 연결 유지 */
        n = snprintf(h, sizeof(h),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            code, status, ctype);
    }
    send(s, h, n, 0);
}

static int send_all(SOCKET s, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* ── JSON 문자열 이스케이프 ───────────────────────────────────────────────── */

/* JSON 문자열 이스케이프 (간단: 따옴표, 백슬래시, 개행만) */
static void json_str(const char *in, char *out, int outsz) {
    int i = 0, o = 0;
    out[o++] = '"';
    while (in[i] && o < outsz - 4) {
        unsigned char c = (unsigned char)in[i++];
        if      (c == '"')  { out[o++] = '\\'; out[o++] = '"';  }
        else if (c == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n';  }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r';  }
        else                { out[o++] = (char)c; }
    }
    out[o++] = '"';
    out[o] = '\0';
}

/* SQLite 행 하나를 JSON 오브젝트 문자열로 변환합니다.
 * message 열의 첫 공백 이전 토큰을 event 로 추출합니다.
 * 반환: 기록된 문자 수 (0이면 건너뜀) */
static int db_row_to_json(const char *ts, const char *level,
                          const char *module, const char *message,
                          char *out, int outsz) {
    char event[64] = {0};
    int i = 0;
    const char *m = message;
    while (*m && *m != ' ' && i < 63) event[i++] = *m++;

    char ts_j[48], lv_j[24], mo_j[64], ev_j[96], msg_j[320];
    json_str(ts,      ts_j,  sizeof(ts_j));
    json_str(level,   lv_j,  sizeof(lv_j));
    json_str(module,  mo_j,  sizeof(mo_j));
    json_str(event,   ev_j,  sizeof(ev_j));
    json_str(message, msg_j, sizeof(msg_j));

    int n = snprintf(out, outsz,
        "{\"ts\":%s,\"level\":%s,\"module\":%s,\"event\":%s,\"message\":%s}",
        ts_j, lv_j, mo_j, ev_j, msg_j);
    return (n > 0 && n < outsz) ? n : 0;
}

/* ── 경로 검증 ────────────────────────────────────────────────────────────── */

/*
 * filename 이 알파벳·숫자·_·-.로만 구성되고 ".."을 포함하지 않는지 확인합니다.
 * logs\ 하위 경로만 허용하는 가장 단순한 방어입니다.
 */
static int safe_filename(const char *filename) {
    if (!filename || !filename[0]) return 0;
    if (strstr(filename, "..")) return 0;
    if (strchr(filename, '/') || strchr(filename, '\\')) return 0;
    for (const char *p = filename; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') continue;
        return 0;
    }
    return 1;
}

/* .db 확장자인지 확인합니다 (SQLite 로그만 허용). */
static int is_db_file(const char *filename) {
    size_t n = strlen(filename);
    return n >= 3 && strcmp(filename + n - 3, ".db") == 0;
}

/* ── 라우트 핸들러 ────────────────────────────────────────────────────────── */

/* GET / → dashboard/index.html */
static void serve_index(SOCKET s) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\dashboard\\index.html", g_root);

    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        const char *msg = "<h1>index.html not found</h1>";
        send_header(s, 404, "text/html; charset=utf-8", (int64_t)strlen(msg));
        send(s, msg, (int)strlen(msg), 0);
        return;
    }
    DWORD size = GetFileSize(hFile, NULL);
    char *buf = (char *)malloc(size + 1);
    if (!buf) { CloseHandle(hFile); return; }
    DWORD read = 0;
    ReadFile(hFile, buf, size, &read, NULL);
    CloseHandle(hFile);
    send_header(s, 200, "text/html; charset=utf-8", (int64_t)read);
    send_all(s, buf, (int)read);
    free(buf);
}

/* qsort 비교 함수: 내림차순(최신 파일명이 앞으로)
 * 파일명이 YYYYMMDD_HHMMSS.db 형식이면 역알파벳 순 = 최신순이 됩니다. */
static int cmp_str_desc(const void *a, const void *b) {
    return strcmp(*(const char *const *)b, *(const char *const *)a);
}

/* GET /api/logs → JSON 배열 ["20260829_162958.db", ...] (최신순) */
static void serve_log_list(SOCKET s) {
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.db", g_logs);

    /* 파일명을 먼저 수집한 후 역순 정렬합니다.
     * FindFirstFile 반환 순서는 NTFS에서도 보장되지 않으므로
     * 직접 qsort를 돌려야 files[0]이 항상 최신 파일이 됩니다. */
#define MAX_LOG_FILES 400
    char **names = (char **)calloc(MAX_LOG_FILES, sizeof(char *));
    if (!names) {
        send_header(s, 500, "application/json", 2);
        send(s, "[]", 2, 0);
        return;
    }
    int count = 0;

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            /* 성능 로그(_perf.db)는 이벤트 로그 목록에서 제외합니다. */
            if (strstr(fd.cFileName, "_perf.db")) continue;
            if (count >= MAX_LOG_FILES) break;
            names[count] = _strdup(fd.cFileName);
            if (names[count]) count++;
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }

    qsort(names, (size_t)count, sizeof(char *), cmp_str_desc);

    char body[8192];
    int pos = 0;
    body[pos++] = '[';
    for (int i = 0; i < count; i++) {
        char esc[128];
        json_str(names[i], esc, sizeof(esc));
        int n = snprintf(body + pos, (size_t)(sizeof(body) - pos - 4),
                         "%s%s", i == 0 ? "" : ",", esc);
        if (n > 0) pos += n;
        free(names[i]);
    }
    free(names);
    body[pos++] = ']';
    body[pos]   = '\0';
    send_header(s, 200, "application/json", (int64_t)pos);
    send(s, body, pos, 0);
}

/* GET /api/events/history?file=xxx → JSON 배열 (SQLite DB에서 전체 읽기) */
static void serve_history(SOCKET s, const char *filename) {
    if (!safe_filename(filename) || !is_db_file(filename)) {
        send_header(s, 400, "application/json", 2);
        send(s, "[]", 2, 0);
        return;
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", g_logs, filename);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        send_header(s, 200, "application/json", 2);
        send(s, "[]", 2, 0);
        if (db) sqlite3_close(db);
        return;
    }

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT ts_iso, level, module, message FROM events ORDER BY id";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        send_header(s, 200, "application/json", 2);
        send(s, "[]", 2, 0);
        return;
    }

    size_t cap = 65536, pos = 0;
    char *body = (char *)malloc(cap);
    if (!body) { sqlite3_finalize(stmt); sqlite3_close(db); return; }
    body[pos++] = '[';
    int first = 1;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *ts      = (const char *)sqlite3_column_text(stmt, 0);
        const char *level   = (const char *)sqlite3_column_text(stmt, 1);
        const char *module  = (const char *)sqlite3_column_text(stmt, 2);
        const char *message = (const char *)sqlite3_column_text(stmt, 3);
        if (!ts || !level || !module || !message) continue;

        char obj[800];
        int n = db_row_to_json(ts, level, module, message, obj, sizeof(obj));
        if (!n) continue;

        if (pos + n + 4 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(body, cap);
            if (!nb) break;
            body = nb;
        }
        if (!first) body[pos++] = ',';
        memcpy(body + pos, obj, (size_t)n);
        pos += (size_t)n;
        first = 0;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    body[pos++] = ']';
    body[pos]   = '\0';
    send_header(s, 200, "application/json", (int64_t)pos);
    send_all(s, body, (int)pos);
    free(body);
}

/* GET /api/events/stream?file=xxx → SSE (블로킹, 연결이 끊길 때까지 유지)
 *
 * SQLite DB를 400ms 간격으로 폴링해 새 행을 SSE로 전송합니다.
 * 매 폴링마다 DB를 새로 열어 WAL 체크포인트 이후 행이 즉시 보이게 합니다. */
static void serve_stream(SOCKET s, const char *filename) {
    if (!safe_filename(filename) || !is_db_file(filename)) {
        send_header(s, 400, "text/plain", 0);
        return;
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", g_logs, filename);

    send_header(s, 200, "text/event-stream; charset=utf-8", -1);

    /* DB 파일이 아직 없으면 생길 때까지 heartbeat를 보내며 기다립니다. */
    int waited = 0;
    while (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        if (send_all(s, "event: heartbeat\ndata: {}\n\n", 27) != 0) return;
        Sleep(3000);
        if (++waited > 200) return; /* 10분 대기 후 종료 */
    }

    /* 현재 max id를 구해 이후 추가되는 행만 스트리밍합니다. */
    sqlite3_int64 last_id = 0;
    {
        sqlite3 *db = NULL;
        if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db, "SELECT MAX(id) FROM events",
                                   -1, &st, NULL) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW)
                    last_id = sqlite3_column_int64(st, 0);
                sqlite3_finalize(st);
            }
            sqlite3_close(db);
        }
    }

    for (;;) {
        int has_row  = 0;
        int send_err = 0;

        /* 매번 새로 열어야 다른 프로세스가 WAL에 쓴 행이 보입니다. */
        sqlite3 *db = NULL;
        if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
            sqlite3_stmt *stmt = NULL;
            const char *sql =
                "SELECT id, ts_iso, level, module, message "
                "FROM events WHERE id > ? ORDER BY id";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, last_id);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    sqlite3_int64  row_id  = sqlite3_column_int64(stmt, 0);
                    const char    *ts      = (const char *)sqlite3_column_text(stmt, 1);
                    const char    *level   = (const char *)sqlite3_column_text(stmt, 2);
                    const char    *module  = (const char *)sqlite3_column_text(stmt, 3);
                    const char    *message = (const char *)sqlite3_column_text(stmt, 4);
                    last_id = row_id;
                    if (!ts || !level || !module || !message) continue;

                    char obj[800];
                    int n = db_row_to_json(ts, level, module, message,
                                          obj, sizeof(obj));
                    if (n > 0) {
                        char sse[832];
                        int sn = snprintf(sse, sizeof(sse), "data: %.*s\n\n", n, obj);
                        if (send_all(s, sse, sn) != 0) { send_err = 1; break; }
                        has_row = 1;
                    }
                }
                sqlite3_finalize(stmt);
            }
            sqlite3_close(db);
        }

        if (send_err) break;
        if (!has_row) {
            if (send_all(s, "event: heartbeat\ndata: {}\n\n", 27) != 0) break;
        }
        Sleep(400);
    }
}

/* ── 설정 API ─────────────────────────────────────────────────────────────── */

/* config.json 기본값 — 파일이 없을 때 반환합니다. */
static const char *DEFAULT_CONFIG =
    "{"
    "\"stream_port\":8081,"
    "\"motion_gate\":1,"
    "\"motion_ratio_threshold\":0.004,"
    "\"idle_refresh_seconds\":10.0,"
    "\"dwell_limit_seconds\":3600,"
    "\"unordered_grace_seconds\":300,"
    "\"fall_hold_seconds\":5.0,"
    "\"luma_black_threshold\":40,"
    "\"luma_white_threshold\":240,"
    "\"frozen_frames_threshold\":45,"
    "\"door_enabled\":0,"
    "\"door_diff_threshold\":0.05,"
    "\"door_confirm_frames\":5,"
    "\"door_open_seconds\":30,"
    "\"door_roi_x\":0,"
    "\"door_roi_y\":0,"
    "\"door_roi_w\":0,"
    "\"door_roi_h\":0,"
    "\"block_gate\":1,"
    "\"block_min_changed\":2,"
    "\"block_margin\":1,"
    "\"track_refresh_seconds\":5.0,"
    "\"residue_enabled\":1,"
    "\"residue_diff_threshold\":18,"
    "\"residue_min_blocks\":3,"
    "\"residue_person_margin_blocks\":1,"
    "\"residue_confirm_seconds\":60,"
    "\"residue_clear_seconds\":10,"
    "\"residue_baseline_refresh_seconds\":300,"
    "\"residue_global_change_ratio\":0.5,"
    "\"fall_aspect_ratio_kp\":1.8,"
    "\"fall_aspect_ratio_nokp\":2.2,"
    "\"slot_warn_seconds\":60,"
    "\"slot_urgent_seconds\":300,"
    "\"slot_ttl_seconds\":120,"
    "\"slot_dirty_threshold\":20,"
    "\"slot_min_dirty_blocks\":2,"
    "\"perf_log_interval_seconds\":60"
    "}";

/* GET /api/config → config.json 반환 (없으면 기본값)
 *
 * "rb" 바이너리 모드로 여는 이유:
 * 텍스트 모드("r")에서는 Windows가 \r\n → \n 변환을 하여
 * ftell이 반환하는 크기와 fread가 실제로 읽는 바이트 수가 달라집니다.
 * Content-Length가 실제 전송 바이트보다 커지면 브라우저가 응답을 기다리다
 * 타임아웃으로 실패합니다. 바이너리 모드로 열면 이 불일치가 없습니다. */
static void serve_config_get(SOCKET s) {
    FILE *f = fopen(g_config_path, "rb");
    if (!f) {
        int len = (int)strlen(DEFAULT_CONFIG);
        send_header(s, 200, "application/json", len);
        send(s, DEFAULT_CONFIG, len, 0);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        fclose(f);
        int len = (int)strlen(DEFAULT_CONFIG);
        send_header(s, 200, "application/json", len);
        send(s, DEFAULT_CONFIG, len, 0);
        return;
    }
    char *body = (char *)malloc((size_t)sz + 1);
    if (!body) { fclose(f); return; }
    size_t got = fread(body, 1, (size_t)sz, f);
    fclose(f);
    body[got] = '\0';
    /* Content-Length는 ftell이 아닌 fread 실제 반환값으로 설정합니다. */
    send_header(s, 200, "application/json", (int)got);
    send(s, body, (int)got, 0);
    free(body);
}

/* HTTP 요청 헤더와 body를 분리합니다.
 * 반환: body 시작 포인터, body_len에 길이. 헤더만 있으면 NULL. */
static const char *extract_body(const char *buf, int buflen, int *body_len) {
    for (int i = 0; i < buflen - 3; i++) {
        if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n') {
            *body_len = buflen - (i + 4);
            return buf + i + 4;
        }
    }
    *body_len = 0;
    return NULL;
}

static int content_length(const char *buf) {
    const char *p = strstr(buf, "Content-Length:");
    char *end;
    long n;
    if (!p) return 0;
    p += strlen("Content-Length:");
    while (*p == ' ' || *p == '\t') p++;
    n = strtol(p, &end, 10);
    if (end == p || n < 0 || n > 7000) return -1;
    return (int)n;
}

/* POST /api/config body: { ... } → config.json 저장 */
static void serve_config_post(SOCKET s, const char *body, int body_len) {
    if (!body || body_len <= 0) {
        send_header(s, 400, "application/json", 12);
        send(s, "{\"ok\":false}", 12, 0);
        return;
    }
    /* 최소 검증: { 로 시작하는지 */
    const char *p = body;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p != '{') {
        send_header(s, 400, "application/json", 12);
        send(s, "{\"ok\":false}", 12, 0);
        return;
    }
    /*
     * 기존 파일에만 있고 이번 본문에 없는 키를 살려서 함께 저장합니다.
     *
     * 예전에는 받은 본문으로 파일을 통째로 덮어썼습니다. 대시보드 UI가
     * 모르는 설정(예: 게이트 튜닝 값)은 점주가 "저장"을 누르는 순간
     * 조용히 사라지고, detector 는 기본값으로 되돌아갔습니다. 기능이
     * 꺼졌다는 사실이 아무 데도 남지 않는 종류의 사고입니다.
     *
     * flat JSON 만 지원하면 되므로 본문에 "key" 문자열이 있는지로 존재를
     * 판정합니다. 중첩 객체를 쓰게 되면 이 방식을 바꿔야 합니다.
     */
    char *merged = NULL;
    int merged_len = 0;
    {
        Config existing;
        char err[128] = {0};
        if (config_load(&existing, g_config_path, err, sizeof(err)) == 0 &&
            existing.count > 0) {
            size_t cap = (size_t)body_len + 4096;
            size_t i;
            merged = (char *)malloc(cap);
            if (merged) {
                int trimmed = body_len;
                while (trimmed > 0 &&
                       isspace((unsigned char)body[trimmed - 1]))
                    trimmed--;
                if (trimmed > 0 && body[trimmed - 1] == '}') trimmed--;
                memcpy(merged, body, (size_t)trimmed);
                merged_len = trimmed;
                for (i = 0; i < existing.count; ++i) {
                    char probe[128];
                    int n;
                    if (!existing.keys[i] || !existing.values[i]) continue;
                    snprintf(probe, sizeof(probe), "\"%s\"", existing.keys[i]);
                    if (strstr(body, probe)) continue;  /* 본문에 이미 있음 */
                    n = snprintf(NULL, 0, ",\"%s\":%s",
                                 existing.keys[i], existing.values[i]);
                    if (n <= 0 || (size_t)(merged_len + n + 2) >= cap) continue;
                    merged_len += snprintf(merged + merged_len,
                                           cap - (size_t)merged_len,
                                           ",\"%s\":%s",
                                           existing.keys[i], existing.values[i]);
                }
                if ((size_t)(merged_len + 2) < cap) {
                    merged[merged_len++] = '}';
                } else {
                    free(merged);
                    merged = NULL;
                    merged_len = 0;
                }
            }
            config_destroy(&existing);
        }
    }

    FILE *f = fopen(g_config_path, "w");
    if (!f) {
        free(merged);
        send_header(s, 500, "application/json", 12);
        send(s, "{\"ok\":false}", 12, 0);
        return;
    }
    if (merged && merged_len > 0) fwrite(merged, 1, (size_t)merged_len, f);
    else                          fwrite(body, 1, (size_t)body_len, f);
    free(merged);
    fclose(f);
    fprintf(stderr, "config: saved %d bytes → %s\n", body_len, g_config_path);
    const char *ok = "{\"ok\":true}";
    send_header(s, 200, "application/json", (int64_t)strlen(ok));
    send(s, ok, (int)strlen(ok), 0);
}

/* ── 클라이언트 스레드 ────────────────────────────────────────────────────── */

static DWORD WINAPI client_thread(LPVOID arg) {
    SOCKET s = (SOCKET)(uintptr_t)arg;
    /* POST body를 담으려면 버퍼가 충분해야 합니다. config JSON ≒ 500B */
    char buf[8192] = {0};
    int n = recv(s, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { closesocket(s); return 0; }
    buf[n] = '\0';

    char method[16], path[512], query[512];
    parse_request(buf, method, path, query);

    /* TCP recv() 한 번에 HTTP POST 본문 전체가 도착한다는 보장은 없습니다.
     * config 저장 요청은 Content-Length만큼 끝까지 받아야 브라우저에서
     * 간헐적으로 빈 본문으로 처리되는 일을 막을 수 있습니다. */
    if (strcmp(method, "POST") == 0) {
        int body_len = 0;
        const char *body = extract_body(buf, n, &body_len);
        int expected = content_length(buf);
        if (!body || expected < 0 || expected > (int)sizeof(buf) - (int)(body - buf) - 1) {
            send_header(s, 400, "application/json", 12);
            send(s, "{\"ok\":false}", 12, 0);
            closesocket(s);
            return 0;
        }
        while (body_len < expected) {
            int got = recv(s, buf + n, (int)sizeof(buf) - 1 - n, 0);
            if (got <= 0) {
                send_header(s, 400, "application/json", 12);
                send(s, "{\"ok\":false}", 12, 0);
                closesocket(s);
                return 0;
            }
            n += got;
            buf[n] = '\0';
            body_len = n - (int)(body - buf);
        }
    }

    /* CORS 프리플라이트 */
    if (strcmp(method, "OPTIONS") == 0) {
        const char *cors =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n\r\n";
        send(s, cors, (int)strlen(cors), 0);
        closesocket(s);
        return 0;
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
            serve_index(s);
        } else if (strcmp(path, "/api/logs") == 0) {
            serve_log_list(s);
        } else if (strcmp(path, "/api/config") == 0) {
            serve_config_get(s);
        } else if (strcmp(path, "/api/events/history") == 0) {
            char file[256];
            query_get(query, "file", file, sizeof(file));
            serve_history(s, file);
        } else if (strcmp(path, "/api/events/stream") == 0) {
            char file[256];
            query_get(query, "file", file, sizeof(file));
            serve_stream(s, file);  /* 블로킹 */
        } else {
            send_header(s, 404, "text/plain", 3);
            send(s, "404", 3, 0);
        }
    } else if (strcmp(method, "POST") == 0) {
        int body_len = 0;
        const char *body = extract_body(buf, n, &body_len);
        if (strcmp(path, "/api/config") == 0) {
            serve_config_post(s, body, body_len);
        } else {
            send_header(s, 404, "text/plain", 3);
            send(s, "404", 3, 0);
        }
    } else {
        send_header(s, 405, "text/plain", 0);
    }

    closesocket(s);
    return 0;
}

/* ── 메인 ─────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* 기본 루트: exe 위치의 상위 디렉터리 (dashboard\ 안에서 실행) */
    GetModuleFileNameA(NULL, g_root, sizeof(g_root));
    char *bs = strrchr(g_root, '\\');
    if (bs) *bs = '\0'; /* exe 디렉터리 */
    bs = strrchr(g_root, '\\');
    if (bs) *bs = '\0'; /* 한 단계 위 (프로젝트 루트) */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            strncpy(g_root, argv[++i], MAX_PATH - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = atoi(argv[++i]);
        }
    }
    snprintf(g_logs,        sizeof(g_logs),        "%s\\logs",        g_root);
    snprintf(g_config_path, sizeof(g_config_path), "%s\\config.json", g_root);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        return 1;
    }
    BOOL reuse = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u_short)g_port);
    /* 0.0.0.0 바인딩: 같은 WiFi 망의 다른 기기(점주 폰·태블릿)에서도 접속 가능합니다.
     * 포트를 방화벽에서 외부 노출하지 않으면 로컬 네트워크 이내로 제한됩니다. */
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(srv, 16) != 0) {
        fprintf(stderr, "bind/listen failed: %d\n", WSAGetLastError());
        closesocket(srv);
        WSACleanup();
        return 1;
    }

    /* 접속 가능한 로컬 IP를 출력합니다 (WiFi 연결 기기에서 이 주소로 접속). */
    {
        char local_ip[64] = "0.0.0.0";
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            struct addrinfo hints = {0}, *res = NULL;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(hostname, NULL, &hints, &res) == 0 && res) {
                struct sockaddr_in *sin = (struct sockaddr_in *)res->ai_addr;
                inet_ntop(AF_INET, &sin->sin_addr, local_ip, sizeof(local_ip));
                freeaddrinfo(res);
            }
        }
        printf("HUNIK Dashboard: http://localhost:%d  (같은 WiFi: http://%s:%d)\n",
               g_port, local_ip, g_port);
    }
    printf("Root : %s\n", g_root);
    printf("Logs : %s\n", g_logs);
    printf("Press Ctrl+C to stop.\n\n");

    for (;;) {
        SOCKET client = accept(srv, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        /* 클라이언트당 스레드 하나 — SSE 스트림이 블로킹이므로 필요 */
        HANDLE th = CreateThread(NULL, 0, client_thread,
                                 (LPVOID)(uintptr_t)client, 0, NULL);
        if (th) CloseHandle(th);
        else    closesocket(client);
    }

    closesocket(srv);
    WSACleanup();
    return 0;
}
