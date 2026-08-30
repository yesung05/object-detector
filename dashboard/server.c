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
 * 비어 있는 채로 대기하다가 파일이 생기면 자동으로 반영합니다.
 *
 * 보안: 127.0.0.1 전용 바인딩 — 외부 네트워크에 노출되지 않습니다.
 *       로그 파일 경로는 logs\ 하위인지 검증하여 디렉터리 탈출을 막습니다.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "../include/config.h"

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

/* 쿼리스트링에서 key 의 값을 꺼냅니다. "file=abc.log&foo=1" → "abc.log" */
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

/* ── 로그 파싱 ────────────────────────────────────────────────────────────── */

/*
 * 포맷: "2026-08-29T16:21:02 WARN  camera   state=whiteout\n"
 * ts(19) + 공백 + level(4~5) + 공백+ + module + 공백+ + message
 */
static int parse_log_line(const char *line, char ts[20], char level[8],
                           char module[32], char event[64], char message[256]) {
    char buf[512];
    int i = 0;
    /* ts: "YYYY-MM-DDTHH:MM:SS" */
    while (i < 19 && line[i] && line[i] != ' ')
        buf[i] = line[i++];
    if (i != 19 || line[i] != ' ') return -1;
    buf[i] = '\0';
    strncpy(ts, buf, 19); ts[19] = '\0';
    /* 공백 건너뜀 */
    const char *p = line + 20;
    while (*p == ' ') p++;
    /* level */
    i = 0;
    while (*p && *p != ' ' && i < 7) level[i++] = *p++;
    level[i] = '\0';
    if (!*p) return -1;
    /* 공백 건너뜀 */
    while (*p == ' ') p++;
    /* module */
    i = 0;
    while (*p && *p != ' ' && i < 31) module[i++] = *p++;
    module[i] = '\0';
    if (!*p) return -1;
    /* 공백 건너뜀 */
    while (*p == ' ') p++;
    /* message */
    strncpy(message, p, 255); message[255] = '\0';
    /* 개행 제거 */
    char *nl = strpbrk(message, "\r\n");
    if (nl) *nl = '\0';
    /* event: 메시지 첫 토큰 */
    i = 0;
    const char *m = message;
    while (*m && *m != ' ' && i < 63) event[i++] = *m++;
    event[i] = '\0';
    return 0;
}

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
 * 파일명이 YYYYMMDD_HHMMSS.log 형식이면 역알파벳 순 = 최신순이 됩니다. */
static int cmp_str_desc(const void *a, const void *b) {
    return strcmp(*(const char *const *)b, *(const char *const *)a);
}

/* GET /api/logs → JSON 배열 ["20260829_162958.log", ...] (최신순) */
static void serve_log_list(SOCKET s) {
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*.log", g_logs);

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

/* 로그 파일 한 줄을 JSON 오브젝트로 변환해서 버퍼에 씁니다. */
static int line_to_json(const char *line, char *out, int outsz) {
    char ts[20], level[8], module[32], event[64], message[256];
    char ts_j[48], level_j[24], module_j[64], event_j[96], msg_j[320];
    if (parse_log_line(line, ts, level, module, event, message) != 0) return 0;
    json_str(ts,      ts_j,     sizeof(ts_j));
    json_str(level,   level_j,  sizeof(level_j));
    json_str(module,  module_j, sizeof(module_j));
    json_str(event,   event_j,  sizeof(event_j));
    json_str(message, msg_j,    sizeof(msg_j));
    int n = snprintf(out, outsz,
        "{\"ts\":%s,\"level\":%s,\"module\":%s,\"event\":%s,\"message\":%s}",
        ts_j, level_j, module_j, event_j, msg_j);
    return (n > 0 && n < outsz) ? n : 0;
}

/* GET /api/events/history?file=xxx → JSON 배열 */
static void serve_history(SOCKET s, const char *filename) {
    if (!safe_filename(filename)) {
        send_header(s, 400, "application/json", 2);
        send(s, "[]", 2, 0);
        return;
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", g_logs, filename);

    FILE *f = fopen(path, "r");
    if (!f) {
        send_header(s, 200, "application/json", 2);
        send(s, "[]", 2, 0);
        return;
    }

    /* 먼저 전체 크기를 계산하기 어려우므로 동적 버퍼로 구성합니다. */
    size_t cap = 65536, pos = 0;
    char *body = (char *)malloc(cap);
    if (!body) { fclose(f); return; }
    body[pos++] = '[';
    int first = 1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char obj[768];
        int n = line_to_json(line, obj, sizeof(obj));
        if (!n) continue;
        /* 버퍼 확장 */
        if (pos + n + 4 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(body, cap);
            if (!nb) break;
            body = nb;
        }
        if (!first) body[pos++] = ',';
        memcpy(body + pos, obj, n);
        pos += n;
        first = 0;
    }
    fclose(f);
    body[pos++] = ']';
    body[pos]   = '\0';
    send_header(s, 200, "application/json", (int64_t)pos);
    send_all(s, body, (int)pos);
    free(body);
}

/* GET /api/events/stream?file=xxx → SSE (블로킹, 연결이 끊길 때까지 유지) */
static void serve_stream(SOCKET s, const char *filename) {
    if (!safe_filename(filename)) {
        send_header(s, 400, "text/plain", 0);
        return;
    }
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", g_logs, filename);

    send_header(s, 200, "text/event-stream; charset=utf-8", -1);

    HANDLE hFile = INVALID_HANDLE_VALUE;
    /* 파일이 아직 없으면 생길 때까지 heartbeat를 보내며 기다립니다. */
    int waited = 0;
    while (hFile == INVALID_HANDLE_VALUE) {
        hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            if (send_all(s, "event: heartbeat\ndata: {}\n\n", 27) != 0) return;
            Sleep(3000);
            waited++;
            if (waited > 200) return; /* 10분 대기 후 종료 */
        }
    }

    /* 파일 끝으로 이동 (새로 추가될 줄만 스트리밍) */
    SetFilePointer(hFile, 0, NULL, FILE_END);

    char rbuf[1024];
    char linebuf[1024];
    int linelen = 0;

    for (;;) {
        DWORD read = 0;
        ReadFile(hFile, rbuf, sizeof(rbuf) - 1, &read, NULL);
        if (read == 0) {
            /* 새 데이터 없음 — heartbeat */
            if (send_all(s, "event: heartbeat\ndata: {}\n\n", 27) != 0) break;
            Sleep(400);
            continue;
        }
        /* 읽은 데이터를 줄 단위로 조립해서 파싱합니다. */
        for (DWORD i = 0; i < read; i++) {
            char c = rbuf[i];
            if (c == '\n' || c == '\r') {
                if (linelen > 0) {
                    linebuf[linelen] = '\0';
                    char obj[768];
                    int n = line_to_json(linebuf, obj, sizeof(obj));
                    if (n > 0) {
                        char sse[800];
                        int sn = snprintf(sse, sizeof(sse), "data: %.*s\n\n", n, obj);
                        if (send_all(s, sse, sn) != 0) goto done;
                    }
                    linelen = 0;
                }
            } else if (linelen < (int)sizeof(linebuf) - 1) {
                linebuf[linelen++] = c;
            }
        }
    }
done:
    CloseHandle(hFile);
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
    "\"track_refresh_seconds\":5.0"
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
                    /* fwrite 는 merged_len 을 쓰므로 끝 문자가 필요 없습니다. */
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
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr); /* 로컬호스트 전용 */

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(srv, 16) != 0) {
        fprintf(stderr, "bind/listen failed: %d\n", WSAGetLastError());
        closesocket(srv);
        WSACleanup();
        return 1;
    }

    printf("HUNIK Dashboard: http://localhost:%d\n", g_port);
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
