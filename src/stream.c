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

/* 클라이언트가 없을 때도 /snapshot 과 /door/save 가 동작하도록 프레임을
 * 유지하는 최소 주기입니다. 이 값이 0이면 대시보드 첫 접속 시 "no frame"
 * 이 뜨고 문 기준 이미지도 캡처할 수 없습니다. */
#define IDLE_PUSH_FPS 1

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
static volatile LONG g_clients     =  0; /* /stream 연결 수 (Interlocked 로만 변경) */
static ULONGLONG     g_last_push_ms = 0; /* 마지막으로 받아들인 프레임 시각 */

/*
 * JPEG 캐시 — 같은 프레임을 클라이언트마다 다시 인코딩하지 않기 위한 것입니다.
 *
 * g_jpeg 는 g_jpeg_lock 이 보호하며 stream_stop 에서 해제합니다.
 * g_jpeg_seq 가 현재 프레임 일련번호와 같으면 인코딩을 건너뜁니다.
 *
 * g_lock 과 별도 락을 쓰는 이유: 인코딩은 수 ms 가 걸리는데 그동안
 * g_lock 을 잡고 있으면 파이프라인의 stream_push 가 통째로 막힙니다.
 */
static CRITICAL_SECTION g_jpeg_lock;
static uint8_t  *g_jpeg     = NULL;
static int       g_jpeg_size = 0;
static uint32_t  g_jpeg_seq = (uint32_t)-1;
/* 인코더 입력용 재사용 스크래치. g_jpeg_lock 이 보호하고 stream_stop 이 해제합니다. */
static uint8_t  *g_jpeg_scratch = NULL;
static size_t    g_jpeg_scratch_cap = 0;

void stream_set_door_state  (int state)   { g_door_state   = state;   }
void stream_set_door_enabled(int enabled) { g_door_enabled = enabled; }
int  stream_client_count    (void)        { return (int)g_clients;    }

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

/*
 * 최신 프레임의 JPEG을 g_jpeg 캐시에 확보한 뒤 호출자 버퍼로 복사합니다.
 *
 * buf/cap: 호출자 소유의 재사용 버퍼입니다. 부족하면 realloc으로 키우고
 *          갱신된 포인터를 돌려줍니다. 호출자가 free 책임을 집니다.
 * 반환: 0=성공(*size 채움), -1=아직 프레임이 없거나 메모리 부족.
 *
 * 캐시를 두는 이유: 예전에는 클라이언트 스레드마다 같은 프레임을 각각
 * 인코딩했습니다. 브라우저 탭 2개면 인코딩도 2배였습니다. 이제 프레임
 * 일련번호가 같으면 먼저 도착한 스레드의 결과를 나눠 씁니다.
 *
 * 호출자 버퍼로 복사한 뒤 락을 놓는 이유: send()는 느린 클라이언트에서
 * 오래 걸릴 수 있으므로 락을 쥔 채 보내면 안 됩니다. 복사 대상은 프레임
 * 전체(2.76MB)가 아니라 JPEG(수십 KB)이므로 비용이 작습니다.
 */
static int jpeg_copy_latest(uint8_t **buf, int *cap, int *size) {
    int result = -1;

    EnterCriticalSection(&g_jpeg_lock);

    EnterCriticalSection(&g_lock);
    uint32_t cur_seq = g_seq;
    int w = g_width, h = g_height;
    int have = (g_rgb && w > 0 && h > 0);
    if (have && cur_seq != g_jpeg_seq) {
        size_t need = (size_t)w * h * 3;
        if (need > g_jpeg_scratch_cap) {
            uint8_t *nb = (uint8_t *)realloc(g_jpeg_scratch, need);
            if (nb) { g_jpeg_scratch = nb; g_jpeg_scratch_cap = need; }
            else    { have = 0; }
        }
        if (have) memcpy(g_jpeg_scratch, g_rgb, need);
    }
    LeaveCriticalSection(&g_lock);

    if (!have) goto done;

    if (cur_seq != g_jpeg_seq) {
        JpegBuf jb = { NULL, 0, 65536 };
        jb.data = (uint8_t *)malloc((size_t)jb.cap);
        if (!jb.data) goto done;
        stbi_write_jpg_to_func(jpeg_write_cb, &jb, w, h, 3,
                               g_jpeg_scratch, JPEG_QUALITY);
        free(g_jpeg);
        g_jpeg      = jb.data;
        g_jpeg_size = jb.size;
        g_jpeg_seq  = cur_seq;
    }
    if (!g_jpeg || g_jpeg_size <= 0) goto done;

    if (g_jpeg_size > *cap) {
        uint8_t *nb = (uint8_t *)realloc(*buf, (size_t)g_jpeg_size);
        if (!nb) goto done;
        *buf = nb;
        *cap = g_jpeg_size;
    }
    memcpy(*buf, g_jpeg, (size_t)g_jpeg_size);
    *size  = g_jpeg_size;
    result = 0;

done:
    LeaveCriticalSection(&g_jpeg_lock);
    return result;
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
    uint8_t *jpg = NULL;
    int jpg_cap = 0, jpg_size = 0;
    if (jpeg_copy_latest(&jpg, &jpg_cap, &jpg_size) != 0 || jpg_size == 0) {
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

        /* 연결 수를 세어 두면 파이프라인이 "보는 사람이 없다"를 알 수 있습니다.
         * 아래 루프가 어떤 경로로 끝나든 반드시 감소시켜야 하므로
         * break 이후 단일 지점에서 처리합니다. */
        InterlockedIncrement(&g_clients);

        uint32_t last_seq = (uint32_t)-1;
        DWORD frame_ms = 1000 / STREAM_FPS;
        /* 클라이언트별 JPEG 재사용 버퍼 — 프레임마다 malloc/free 하지 않습니다. */
        uint8_t *jpg = NULL;
        int jpg_cap = 0;

        while (g_running) {
            uint32_t cur_seq;
            EnterCriticalSection(&g_lock);
            cur_seq = g_seq;
            LeaveCriticalSection(&g_lock);

            if (cur_seq == last_seq) { Sleep(frame_ms / 2); continue; }
            last_seq = cur_seq;

            int jpg_size = 0;
            if (jpeg_copy_latest(&jpg, &jpg_cap, &jpg_size) != 0 ||
                jpg_size == 0) {
                Sleep(frame_ms);
                continue;
            }

            char part[256];
            int plen = snprintf(part, sizeof(part),
                "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n",
                boundary, jpg_size);
            int ok = (send_all(s, part, plen) == 0)
                  && (send_all(s, (char *)jpg, jpg_size) == 0)
                  && (send_all(s, "\r\n", 2) == 0);
            if (!ok) break;
            Sleep(frame_ms);
        }
        free(jpg);
        InterlockedDecrement(&g_clients);

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

    InitializeCriticalSection(&g_jpeg_lock);
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

    /*
     * 전송 주기를 넘겨 들어온 프레임은 받지 않고 버립니다.
     *
     * 어차피 클라이언트 루프는 STREAM_FPS 로만 내보내므로, 그보다 자주
     * 복사해 봐야 다음 복사에 덮여 없어질 뿐입니다. 15fps 입력에서
     * 프레임당 2.76MB 복사가 그대로 낭비되고 있었습니다.
     *
     * 보는 사람이 없으면 IDLE_PUSH_FPS 로 더 낮춥니다. 완전히 멈추지
     * 않는 이유는 /snapshot 과 /door/save 가 최근 프레임을 필요로 하기
     * 때문입니다 — 0 으로 두면 대시보드 첫 접속과 문 기준 캡처가 실패합니다.
     */
    {
        int fps = (g_clients > 0) ? STREAM_FPS : IDLE_PUSH_FPS;
        ULONGLONG now_ms = GetTickCount64();
        ULONGLONG min_gap = (ULONGLONG)(1000 / fps);
        if (g_last_push_ms != 0 && (now_ms - g_last_push_ms) < min_gap) return;
        g_last_push_ms = now_ms;
    }

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

    EnterCriticalSection(&g_jpeg_lock);
    free(g_jpeg);
    g_jpeg = NULL;
    g_jpeg_size = 0;
    g_jpeg_seq = (uint32_t)-1;
    free(g_jpeg_scratch);
    g_jpeg_scratch = NULL;
    g_jpeg_scratch_cap = 0;
    LeaveCriticalSection(&g_jpeg_lock);
    DeleteCriticalSection(&g_jpeg_lock);
}
