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
    /* 인코딩 중 g_rgb가 변경되지 않도록 로컬 복사 */
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

/* ── 클라이언트 스레드 ──────────────────────────────────────────────────── */

static DWORD WINAPI client_thread(LPVOID arg) {
    SOCKET s = (SOCKET)(uintptr_t)arg;
    char req[1024] = {0};

    /* 요청 수신 */
    int n = recv(s, req, sizeof(req) - 1, 0);
    if (n <= 0) { closesocket(s); return 0; }

    /* /stream 이외의 경로는 404 */
    if (!strstr(req, "GET /stream")) {
        const char *r404 = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(s, r404, (int)strlen(r404), 0);
        closesocket(s);
        return 0;
    }

    /* MJPEG 스트림 헤더 */
    const char *boundary = "mjpeg_boundary";
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=%s\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        boundary);
    if (send_all(s, hdr, hlen) != 0) { closesocket(s); return 0; }

    uint32_t last_seq = (uint32_t)-1;
    DWORD frame_ms = 1000 / STREAM_FPS;

    while (g_running) {
        /* 새 프레임이 없으면 대기 */
        uint32_t cur_seq;
        EnterCriticalSection(&g_lock);
        cur_seq = g_seq;
        LeaveCriticalSection(&g_lock);

        if (cur_seq == last_seq) {
            Sleep(frame_ms / 2);
            continue;
        }
        last_seq = cur_seq;

        int jpg_size = 0;
        uint8_t *jpg = encode_jpeg(&jpg_size);
        if (!jpg || jpg_size == 0) { free(jpg); Sleep(frame_ms); continue; }

        /* MJPEG 파트 헤더 */
        char part[256];
        int plen = snprintf(part, sizeof(part),
            "--%s\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %d\r\n"
            "\r\n",
            boundary, jpg_size);
        int ok = (send_all(s, part, plen) == 0)
              && (send_all(s, (char *)jpg, jpg_size) == 0)
              && (send_all(s, "\r\n", 2) == 0);
        free(jpg);
        if (!ok) break;

        Sleep(frame_ms);
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

int stream_start(int port) {
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

    fprintf(stderr, "stream: http://localhost:%d/stream\n", port);
    return 0;
}

void stream_push(const uint8_t *rgb, int width, int height, int stride) {
    if (!g_running || !rgb || width <= 0 || height <= 0) return;

    EnterCriticalSection(&g_lock);
    /* 해상도 변경 또는 최초 할당 */
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
            /* stride 정렬이 다를 때 행 단위로 복사 */
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
