/*
 * MJPEG HTTP 스트리밍 서버 (Windows WinSocket2 전용)
 *
 * stb_image_write.h 로 RGB → JPEG 인코딩하고 HTTP multipart/x-mixed-replace
 * 포맷으로 전송합니다. 외부 라이브러리 없이 ws2_32.lib 하나만 필요합니다.
 *
 * stb_image_write를 선택한 이유:
 * - 단일 헤더 파일, 추가 빌드 스텝 없음
 * - MIT 라이선스, 상용 배포 가능
 * - FFmpeg avcodec MJPEG API보다 훨씬 단순 (초기화 10줄 → 1줄)
 * - 품질은 FFmpeg 대비 낮지만 실시간 감시 영상에는 충분
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* STB: 헤더 전용 라이브러리를 이 번역 단위에서만 구현 */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO       /* 파일 I/O 불필요, 메모리 콜백만 사용 */
#include "../third_party/stb/stb_image_write.h"

#include "stream.h"

#pragma comment(lib, "ws2_32.lib")

/* ── 공유 프레임 버퍼 ────────────────────────────────────────────────────── */

#define MAX_CLIENTS 4
#define JPEG_QUALITY 75   /* 0-100. 75이면 1280×720 기준 약 50-80KB */
#define STREAM_FPS   10   /* 최대 전송 FPS — 클라이언트당 100ms sleep */

static CRITICAL_SECTION g_lock;
static uint8_t  *g_rgb    = NULL; /* 최신 프레임 RGB 버퍼 (g_lock 보호) */
static int       g_width  = 0;
static int       g_height = 0;
static uint32_t  g_seq    = 0;    /* 프레임 일련번호: 변경 감지용 */
static int       g_running = 0;
static HANDLE    g_accept_thread = NULL;
static SOCKET    g_srv = INVALID_SOCKET;
static char      g_data_dir[MAX_PATH] = "."; /* door_reference.raw 저장 위치 */
static volatile int g_door_state   = -1; /* -1=알 수 없음, 0=닫힘, 1=열림 */
static volatile int g_door_enabled =  0; /* 감지 활성 여부 */

void stream_set_door_state  (int state)   { g_door_state   = state;   }
void stream_set_door_enabled(int enabled) { g_door_enabled = enabled; }

/* ── JPEG 콜백 버퍼 ─────────────────────────────────────────────────────── */

typedef struct {
    uint8_t *data; /* 동적 할당, 호출자가 free */
    int      size;
    int      cap;
} JpegBuf;

static void jpeg_write_cb(void *ctx, void *data, int size) {
    JpegBuf *b = (JpegBuf *)ctx;
    if (b->size + size > b->cap) {
        int newcap = b->cap * 2 + size;
        uint8_t *nb = (uint8_t *)realloc(b->data, newcap);
        if (!nb) return;
        b->data = nb;
        b->cap  = newcap;
    }
    memcpy(b->data + b->size, data, size);
    b->size += size;
}

/* 현재 g_rgb를 JPEG로 인코딩합니다. 호출자가 반환된 포인터를 free해야 합니다. */
static uint8_t *encode_jpeg(int *out_size) {
    JpegBuf buf = { NULL, 0, 65536 };
    buf.data = (uint8_t *)malloc(buf.cap);
    if (!buf.data) return NULL;

    EnterCriticalSection(&g_lock);
    if (!g_rgb || g_width <= 0 || g_height <= 0) {
        LeaveCriticalSection(&g_lock);
        free(buf.data);
        return NULL;
    }
    int w = g_width, h = g_height;
    uint8_t *local = (uint8_t *)malloc((size_t)w * h * 3);
    if (!local) {
        LeaveCriticalSection(&g_lock);
        free(buf.data);
        return NULL;
    }
    memcpy(local, g_rgb, (size_t)w * h * 3);
    LeaveCriticalSection(&g_lock);

    stbi_write_jpg_to_func(jpeg_write_cb, &buf, w, h, 3, local, JPEG_QUALITY);
    free(local);

    *out_size = buf.size;
    return buf.data;
}

/* ── HTTP 유틸리티 ──────────────────────────────────────────────────────── */

static int send_all(SOCKET s, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* CORS + JSON 응답 전송 */
static void send_json(SOCKET s, int code, const char *body) {
    char hdr[512];
    int blen = (int)strlen(body);
    const char *status = code == 200 ? "OK" : code == 503 ? "Service Unavailable" : "Error";
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, status, blen);
    send_all(s, hdr, hlen);
    send_all(s, body, blen);
}

/* ── 엔드포인트 핸들러 ──────────────────────────────────────────────────── */

/* GET /snapshot → 현재 프레임 JPEG 1장 반환 */
static void handle_snapshot(SOCKET s) {
    int jpg_size = 0;
    uint8_t *jpg = encode_jpeg(&jpg_size);
    if (!jpg || jpg_size == 0) {
        free(jpg);
        send_json(s, 503, "{\"error\":\"no frame\"}");
        return;
    }
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n",
        jpg_size);
    send_all(s, hdr, hlen);
    send_all(s, (char *)jpg, jpg_size);
    free(jpg);
}

/* POST /door/save?state=closed|open → 현재 RGB 프레임을 raw 파일로 저장
 *
 * 파일 형식: [int32 width][int32 height][w*h*3 RGB bytes]
 * JPEG 인코딩 없이 raw RGB를 저장하는 이유:
 * - door.c의 픽셀 비교가 RGB 공간에서 직접 이루어지므로
 *   JPEG 재압축으로 인한 양자화 오차를 피할 수 있습니다.
 * - /door/preview는 이 raw 파일을 JPEG로 변환해서 반환합니다.
 *
 * is_open: 0=닫힌 기준(door_closed_reference.raw), 1=열린 기준(door_open_reference.raw) */
static void handle_door_save(SOCKET s, int is_open) {
    EnterCriticalSection(&g_lock);
    if (!g_rgb || g_width <= 0 || g_height <= 0) {
        LeaveCriticalSection(&g_lock);
        send_json(s, 503, "{\"ok\":false,\"error\":\"no frame\"}");
        return;
    }
    int w = g_width, h = g_height;
    uint8_t *copy = (uint8_t *)malloc((size_t)w * h * 3);
    if (copy) memcpy(copy, g_rgb, (size_t)w * h * 3);
    LeaveCriticalSection(&g_lock);

    if (!copy) { send_json(s, 500, "{\"ok\":false}"); return; }

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", g_data_dir,
             is_open ? "door_open_reference.raw" : "door_closed_reference.raw");
    FILE *f = fopen(path, "wb");
    if (!f) { free(copy); send_json(s, 500, "{\"ok\":false,\"error\":\"write failed\"}"); return; }
    fwrite(&w, sizeof(int), 1, f);
    fwrite(&h, sizeof(int), 1, f);
    fwrite(copy, 1, (size_t)w * h * 3, f);
    fclose(f);
    free(copy);

    fprintf(stderr, "door: %s reference saved %dx%d -> %s\n",
            is_open ? "open" : "closed", w, h, path);
    if (is_open)
        send_json(s, 200, "{\"ok\":true,\"state\":\"open\"}");
    else
        send_json(s, 200, "{\"ok\":true,\"state\":\"closed\"}");
}

/* GET /door/preview?state=closed|open → 해당 raw 파일을 JPEG로 반환 */
static void handle_door_preview(SOCKET s, int is_open) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", g_data_dir,
             is_open ? "door_open_reference.raw" : "door_closed_reference.raw");
    FILE *f = fopen(path, "rb");
    if (!f) {
        send_json(s, 404, "{\"error\":\"no reference\"}");
        return;
    }
    int w = 0, h = 0;
    fread(&w, sizeof(int), 1, f);
    fread(&h, sizeof(int), 1, f);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        fclose(f);
        send_json(s, 500, "{\"error\":\"invalid reference\"}");
        return;
    }
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    if (!rgb) { fclose(f); return; }
    fread(rgb, 1, (size_t)w * h * 3, f);
    fclose(f);

    JpegBuf buf = { NULL, 0, 65536 };
    buf.data = (uint8_t *)malloc(buf.cap);
    if (!buf.data) { free(rgb); return; }
    stbi_write_jpg_to_func(jpeg_write_cb, &buf, w, h, 3, rgb, JPEG_QUALITY);
    free(rgb);

    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n",
        buf.size);
    send_all(s, hdr, hlen);
    send_all(s, (char *)buf.data, buf.size);
    free(buf.data);
}

/* ── 클라이언트 스레드 ──────────────────────────────────────────────────── */

static DWORD WINAPI client_thread(LPVOID arg) {
    SOCKET s = (SOCKET)(uintptr_t)arg;
    char req[2048] = {0};

    int n = recv(s, req, sizeof(req) - 1, 0);
    if (n <= 0) { closesocket(s); return 0; }

    /* CORS 프리플라이트 (브라우저가 POST 전에 OPTIONS로 먼저 물어봄) */
    if (strncmp(req, "OPTIONS", 7) == 0) {
        const char *cors =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Connection: close\r\n"
            "\r\n";
        send(s, cors, (int)strlen(cors), 0);
        closesocket(s);
        return 0;
    }

    /* 경로 + 쿼리스트링 추출 */
    char method[16] = {0}, url[256] = {0};
    sscanf(req, "%15s %255s", method, url);
    char qs[128] = {0};
    char *qmark = strchr(url, '?');
    if (qmark) {
        /* 쿼리스트링을 별도 버퍼에 보관하고 경로에서는 제거 */
        strncpy(qs, qmark + 1, sizeof(qs) - 1);
        *qmark = '\0';
    }

    if (strcmp(url, "/stream") == 0) {
        /* MJPEG 스트림 헤더 */
        const char *boundary = "mjpeg_boundary";
        char hdr[512];
        int hlen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=%s\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            boundary);
        if (send_all(s, hdr, hlen) != 0) { closesocket(s); return 0; }

        uint32_t last_seq = (uint32_t)-1;
        DWORD frame_ms = 1000 / STREAM_FPS;

        while (g_running) {
            uint32_t cur_seq;
            EnterCriticalSection(&g_lock);
            cur_seq = g_seq;
            LeaveCriticalSection(&g_lock);

            if (cur_seq == last_seq) { Sleep(frame_ms / 2); continue; }
            last_seq = cur_seq;

            int jpg_size = 0;
            uint8_t *jpg = encode_jpeg(&jpg_size);
            if (!jpg || jpg_size == 0) { free(jpg); Sleep(frame_ms); continue; }

            char part[256];
            int plen = snprintf(part, sizeof(part),
                "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n",
                boundary, jpg_size);
            int ok = (send_all(s, part, plen) == 0)
                  && (send_all(s, (char *)jpg, jpg_size) == 0)
                  && (send_all(s, "\r\n", 2) == 0);
            free(jpg);
            if (!ok) break;
            Sleep(frame_ms);
        }

    } else if (strcmp(url, "/snapshot") == 0) {
        handle_snapshot(s);

    } else if (strcmp(url, "/door/state") == 0) {
        /* GET /door/state → 현재 문 상태 + 감지 활성 여부 반환 */
        char body[128];
        int s_val = g_door_state;
        int e_val = g_door_enabled;
        snprintf(body, sizeof(body),
                 "{\"state\":%d,\"enabled\":%d,\"label\":\"%s\"}",
                 s_val, e_val,
                 s_val == 1 ? "open" : s_val == 0 ? "closed" : "unknown");
        send_json(s, 200, body);

    } else if (strcmp(url, "/door/save") == 0) {
        int is_open = (strstr(qs, "state=open") != NULL);
        handle_door_save(s, is_open);

    } else if (strcmp(url, "/door/preview") == 0) {
        int is_open = (strstr(qs, "state=open") != NULL);
        handle_door_preview(s, is_open);

    } else {
        const char *r404 =
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(s, r404, (int)strlen(r404), 0);
    }

    closesocket(s);
    return 0;
}

/* ── accept 루프 스레드 ─────────────────────────────────────────────────── */

static DWORD WINAPI accept_thread(LPVOID arg) {
    (void)arg;
    while (g_running) {
        SOCKET client = accept(g_srv, NULL, NULL);
        if (client == INVALID_SOCKET) break;
        HANDLE th = CreateThread(NULL, 0, client_thread,
                                 (LPVOID)(uintptr_t)client, 0, NULL);
        if (th) CloseHandle(th);
        else    closesocket(client);
    }
    return 0;
}

/* ── 공개 API ────────────────────────────────────────────────────────────── */

int stream_start(int port, const char *data_dir) {
    if (data_dir && data_dir[0])
        strncpy(g_data_dir, data_dir, MAX_PATH - 1);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    InitializeCriticalSection(&g_lock);

    g_srv = socket(AF_INET, SOCK_STREAM, 0);
    if (g_srv == INVALID_SOCKET) return -1;

    BOOL reuse = TRUE;
    setsockopt(g_srv, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(g_srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(g_srv, MAX_CLIENTS) != 0) {
        closesocket(g_srv);
        g_srv = INVALID_SOCKET;
        return -1;
    }

    g_running = 1;
    g_accept_thread = CreateThread(NULL, 0, accept_thread, NULL, 0, NULL);
    if (!g_accept_thread) {
        g_running = 0;
        closesocket(g_srv);
        return -1;
    }

    fprintf(stderr, "stream: http://localhost:%d/stream  /snapshot  /door/save\n", port);
    return 0;
}

void stream_push(const uint8_t *rgb, int width, int height, int stride) {
    if (!g_running || !rgb || width <= 0 || height <= 0) return;

    EnterCriticalSection(&g_lock);
    if (!g_rgb || g_width != width || g_height != height) {
        free(g_rgb);
        g_rgb = (uint8_t *)malloc((size_t)width * height * 3);
        g_width  = width;
        g_height = height;
    }
    if (g_rgb) {
        if (stride == width * 3) {
            memcpy(g_rgb, rgb, (size_t)width * height * 3);
        } else {
            for (int y = 0; y < height; y++)
                memcpy(g_rgb + (size_t)y * width * 3,
                       rgb   + (size_t)y * stride, (size_t)width * 3);
        }
        g_seq++;
    }
    LeaveCriticalSection(&g_lock);
}

void stream_stop(void) {
    g_running = 0;
    if (g_srv != INVALID_SOCKET) {
        closesocket(g_srv);
        g_srv = INVALID_SOCKET;
    }
    if (g_accept_thread) {
        WaitForSingleObject(g_accept_thread, 2000);
        CloseHandle(g_accept_thread);
        g_accept_thread = NULL;
    }
    EnterCriticalSection(&g_lock);
    free(g_rgb);
    g_rgb = NULL;
    LeaveCriticalSection(&g_lock);
    DeleteCriticalSection(&g_lock);
}
