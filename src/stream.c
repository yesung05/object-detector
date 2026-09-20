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
#include <stdarg.h>

/* STB: 헤더 전용 라이브러리를 이 번역 단위에서만 구현 */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO       /* 파일 I/O 불필요, 메모리 콜백만 사용 */
#include "../third_party/stb/stb_image_write.h"

#include "stream.h"
#include "netaccess.h"

#pragma comment(lib, "ws2_32.lib")

/* ── 공유 프레임 버퍼 ────────────────────────────────────────────────────── */

#define MAX_CLIENTS 4
#define JPEG_QUALITY 75   /* 0-100. 75이면 1280×720 기준 약 50-80KB */
#define STREAM_FPS   10   /* 기본 최대 전송 FPS — 클라이언트당 100ms sleep */

/* 클라이언트가 없을 때도 /snapshot 과 /door/save 가 동작하도록 프레임을
 * 유지하는 최소 주기입니다. 이 값이 0이면 대시보드 첫 접속 시 "no frame"
 * 이 뜨고 문 기준 이미지도 캡처할 수 없습니다. */
#define IDLE_PUSH_FPS 1

static CRITICAL_SECTION g_lock;
static int g_running;
static char g_surface_status[32768] = "{\"enabled\":false,\"surfaces\":[]}";
static char g_surface_capture[40];
static int g_surface_empty;
static char g_surface_ack[40];
static int g_surface_ack_candidate,g_surface_ack_revision,g_surface_ack_version;
int stream_surface_ack_request(char *id,size_t size,int *candidate,int *revision,int *version) {
    int found;if(!g_running)return 0;
    EnterCriticalSection(&g_lock);found=g_surface_ack[0]!=0;
    if(found){snprintf(id,size,"%s",g_surface_ack);*candidate=g_surface_ack_candidate;*revision=g_surface_ack_revision;*version=g_surface_ack_version;g_surface_ack[0]=0;}
    LeaveCriticalSection(&g_lock);return found;
}
void stream_surface_status(const char *json) {
    if(!g_running) return;
    EnterCriticalSection(&g_lock);
    snprintf(g_surface_status,sizeof(g_surface_status),"%s",json);
    LeaveCriticalSection(&g_lock);
}
int stream_surface_capture_request(char *id,size_t size,int *empty) {
    int found;
    if(!g_running)return 0;
    EnterCriticalSection(&g_lock);found=g_surface_capture[0]!=0;
    if(found){snprintf(id,size,"%s",g_surface_capture);*empty=g_surface_empty;g_surface_capture[0]=0;}
    LeaveCriticalSection(&g_lock);return found;
}
static uint8_t  *g_rgb    = NULL; /* 최신 프레임 RGB 버퍼 (g_lock 보호) */
static int       g_width  = 0;
static int       g_height = 0;
static uint32_t  g_seq    = 0;    /* 프레임 일련번호: 변경 감지용 */
static HANDLE    g_accept_thread = NULL;
static SOCKET    g_srv = INVALID_SOCKET;
static char      g_data_dir[MAX_PATH] = "."; /* door_reference.raw 저장 위치 */
static volatile int g_door_state   = -1; /* -1=알 수 없음, 0=닫힘, 1=열림 */
static volatile int g_door_enabled =  0; /* 감지 활성 여부 */
static volatile LONG g_clients     =  0; /* /stream 연결 수 (Interlocked 로만 변경) */
static ULONGLONG     g_last_push_ms = 0; /* 마지막으로 받아들인 프레임 시각 */
/* 적응형 감속이 낮출 수 있는 전송 FPS. 키오스크가 바쁠 때 JPEG 인코딩 부하를 줄입니다. */
static volatile LONG g_stream_fps = STREAM_FPS;

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

void stream_set_max_fps(int fps) {
    if (fps < 1)  fps = 1;
    if (fps > 30) fps = 30;
    g_stream_fps = fps;
}
void stream_set_door_state  (int state)   { g_door_state   = state;   }
void stream_set_door_enabled(int enabled) { g_door_enabled = enabled; }
int  stream_client_count    (void)        { return (int)g_clients;    }

/* ── 개인정보 보호 · 접근 제어 상태 ─────────────────────────────────────────
 * g_running 이 0 인 동안(stream_start 전)은 다른 스레드가 없으므로 락 없이 씁니다 —
 * g_lock 은 stream_start 에서 초기화되는데 main.c 의 설정 적용은 그보다 먼저 옵니다. */

static char          g_access_pin[64];              /* "" = PIN 비활성 */
static volatile int  g_privacy_mode       = 1;      /* 배포 기본값 on — localhost 대시보드에서 즉시 끌 수 있음 */
static volatile int  g_privacy_unlock_max = 600;
static ULONGLONG     g_privacy_unlock_until_ms = 0; /* 0 = 잠김 (g_lock 보호) */
static StreamStatus  g_status;                      /* g_lock 보호 */

/* 접근 로그 링 — 클라이언트 스레드가 쌓고 main 스레드가 stream_pop_access_log 로 회수 */
#define ACCESS_RING 32
typedef struct { int level; char msg[240]; } AccessEntry;
static AccessEntry g_access[ACCESS_RING];
static int g_access_head = 0, g_access_count = 0, g_access_dropped = 0;

static void access_log(int level, const char *fmt, ...) {
    AccessEntry *e;
    va_list ap;
    if (!g_running) return;
    EnterCriticalSection(&g_lock);
    if (g_access_count >= ACCESS_RING) {
        /* 넘치면 가장 오래된 항목을 버리고 개수만 셉니다 — 다음 회수 때 한 줄로 보고 */
        g_access_head = (g_access_head + 1) % ACCESS_RING;
        g_access_count--;
        g_access_dropped++;
    }
    e = &g_access[(g_access_head + g_access_count) % ACCESS_RING];
    e->level = level;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
    g_access_count++;
    LeaveCriticalSection(&g_lock);
}

int stream_pop_access_log(int *level, char *msg, size_t size) {
    int found = 0;
    if (!g_running) return 0;
    EnterCriticalSection(&g_lock);
    if (g_access_dropped) {
        snprintf(msg, size, "access log ring overflow — %d entries dropped", g_access_dropped);
        *level = 1;
        g_access_dropped = 0;
        found = 1;
    } else if (g_access_count > 0) {
        AccessEntry *e = &g_access[g_access_head];
        *level = e->level;
        snprintf(msg, size, "%s", e->msg);
        g_access_head = (g_access_head + 1) % ACCESS_RING;
        g_access_count--;
        found = 1;
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

void stream_set_access_pin(const char *pin) {
    if (g_running) EnterCriticalSection(&g_lock);
    snprintf(g_access_pin, sizeof(g_access_pin), "%s", pin ? pin : "");
    if (g_running) LeaveCriticalSection(&g_lock);
}
void stream_set_privacy_mode(int enabled)       { g_privacy_mode = enabled ? 1 : 0; }
void stream_set_privacy_unlock_max(int seconds) { g_privacy_unlock_max = seconds > 0 ? seconds : 600; }

/* main 스레드가 프레임마다 호출합니다. 해제 시간이 끝나면 여기서 재잠금하고 로그를 남깁니다 —
   브라우저가 닫혀도 서버가 스스로 복귀하도록 타임아웃은 서버가 강제합니다. */
int stream_privacy_active(void) {
    int active;
    if (!g_privacy_mode) return 0;
    if (!g_running) return 1;
    EnterCriticalSection(&g_lock);
    if (g_privacy_unlock_until_ms && GetTickCount64() >= g_privacy_unlock_until_ms) {
        g_privacy_unlock_until_ms = 0;
        LeaveCriticalSection(&g_lock);
        access_log(0, "privacy relocked (timeout)");
        return 1;
    }
    active = g_privacy_unlock_until_ms == 0;
    LeaveCriticalSection(&g_lock);
    return active;
}

/* 클라이언트 스레드용 — 로그 없이 현재 상태만 읽습니다. */
static int privacy_active_now(void) {
    int active;
    if (!g_privacy_mode) return 0;
    EnterCriticalSection(&g_lock);
    active = g_privacy_unlock_until_ms == 0 || GetTickCount64() >= g_privacy_unlock_until_ms;
    LeaveCriticalSection(&g_lock);
    return active;
}

void stream_set_status(const StreamStatus *st) {
    if (!st) return;
    if (g_running) EnterCriticalSection(&g_lock);
    g_status = *st;
    if (g_running) LeaveCriticalSection(&g_lock);
}

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
    const char *status = code == 200 ? "OK" : code == 202 ? "Accepted" : code == 403 ? "Forbidden"
                       : code == 404 ? "Not Found" : code == 409 ? "Conflict"
                       : code == 503 ? "Service Unavailable" : "Error";
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

/* 현재 RGB 프레임을 raw 파일로 저장합니다.
 *
 * 파일 형식: [int32 width][int32 height][w*h*3 RGB bytes]
 * JPEG 인코딩 없이 raw RGB를 저장하는 이유:
 * - 픽셀 비교가 RGB/luma 공간에서 직접 이루어지므로
 *   JPEG 재압축으로 인한 양자화 오차를 피할 수 있습니다.
 *
 * filename: 저장할 파일명 (g_data_dir 아래에 저장됩니다)
 * ok_json:  성공 시 반환할 JSON 문자열 */
static void handle_save_raw(SOCKET s,
                             const char *filename,
                             const char *ok_json) {
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
    snprintf(path, sizeof(path), "%s\\%s", g_data_dir, filename);
    FILE *f = fopen(path, "wb");
    if (!f) { free(copy); send_json(s, 500, "{\"ok\":false,\"error\":\"write failed\"}"); return; }
    fwrite(&w, sizeof(int), 1, f);
    fwrite(&h, sizeof(int), 1, f);
    fwrite(copy, 1, (size_t)w * h * 3, f);
    fclose(f);
    free(copy);

    fprintf(stderr, "stream: saved %dx%d -> %s\n", w, h, path);
    send_json(s, 200, ok_json);
}

/* POST /door/save?state=closed|open → 문 기준 이미지 저장 */
static void handle_door_save(SOCKET s, int is_open) {
    handle_save_raw(s,
        is_open ? "door_open_reference.raw" : "door_closed_reference.raw",
        is_open ? "{\"ok\":true,\"state\":\"open\"}"
                : "{\"ok\":true,\"state\":\"closed\"}");
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

static void handle_surface_reference(SOCKET s,const char *qs) {
    char name[160]={0},path[1024],header[384];const char *p=strstr(qs,"file=");size_t n=0;
    uint32_t meta[4];uint8_t rgb[96*96*3];FILE *f;JpegBuf jpg={NULL,0,65536};int h;
    if(p){p+=5;while(*p&&*p!='&'&&n<sizeof(name)-1){char c=*p++;
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'))break;
        name[n++]=c;}}
    if(strncmp(name,"surface-",8)||strstr(name,"..")||n<12||strcmp(name+n-4,".bin")){send_json(s,400,"{\"error\":\"invalid reference\"}");return;}
    snprintf(path,sizeof(path),"%s/config/%s",g_data_dir,name);f=fopen(path,"rb");
    if(!f){send_json(s,404,"{\"error\":\"reference unavailable\"}");return;}
    if(fread(meta,sizeof(meta),1,f)!=1||meta[0]!=0x53524631||meta[1]!=96||fread(rgb,1,sizeof(rgb),f)!=sizeof(rgb)){
        fclose(f);send_json(s,400,"{\"error\":\"invalid image\"}");return;}
    fclose(f);jpg.data=(uint8_t*)malloc(jpg.cap);if(!jpg.data)return;
    stbi_write_jpg_to_func(jpeg_write_cb,&jpg,96,96,3,rgb,JPEG_QUALITY);
    h=snprintf(header,sizeof(header),"HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",jpg.size);
    send_all(s,header,h);send_all(s,(const char*)jpg.data,jpg.size);free(jpg.data);
}
static DWORD WINAPI client_thread(LPVOID arg) {
    SOCKET s = (SOCKET)(uintptr_t)arg;
    /* PIN 헤더까지 붙은 브라우저 요청 헤더는 1KB 를 넘기도 합니다. 헤더 끝(\r\n\r\n)이
       올 때까지 이어 받되 상한과 시간을 둬 반쪽 요청이 스레드를 붙잡지 못하게 합니다. */
    char req[4096] = {0};
    int n = 0;
    {
        DWORD timeout = 5000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    }
    for (;;) {
        int got = recv(s, req + n, (int)sizeof(req) - 1 - n, 0);
        if (got <= 0) { closesocket(s); return 0; }
        n += got;
        req[n] = '\0';
        if (strstr(req, "\r\n\r\n") || n >= (int)sizeof(req) - 1) break;
    }

    /* CORS 프리플라이트 (브라우저가 POST 전에 OPTIONS로 먼저 물어봄).
       X-Hunik-Pin 을 허용 목록에 넣지 않으면 브라우저가 PIN 헤더를 보내지 못합니다. */
    if (strncmp(req, "OPTIONS", 7) == 0) {
        const char *cors =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST\r\n"
            "Access-Control-Allow-Headers: Content-Type, X-Hunik-Pin\r\n"
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

    /* ── 접근 제어 ─────────────────────────────────────────────────────────
       모든 경로에 공통: 서브넷 밖이면 거부. POST(상태를 바꾸는 요청)는 PIN 까지.
       거부는 WARN, 허용된 변경은 INFO 로 남겨 "누가 언제 무엇을"이 로그 한 줄에 있게 합니다. */
    NetAccess acc;
    char pin[64];
    int is_post = strcmp(method, "POST") == 0;
    EnterCriticalSection(&g_lock);
    snprintf(pin, sizeof(pin), "%s", g_access_pin);
    LeaveCriticalSection(&g_lock);
    netaccess_evaluate(s, req, pin, &acc);
    if (acc.tier == NET_TIER_DENIED) {
        access_log(1, "DENIED %s %s %s — %s", acc.ip, method, url,
                   netaccess_deny_reason_name(acc.deny_reason));
        send_json(s, 403, "{\"ok\":false,\"error\":\"forbidden\"}");
        closesocket(s);
        return 0;
    }
    if (is_post && !acc.pin_ok) {
        char body[96];
        snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
                 netaccess_pin_reason_name(acc.pin_reason));
        access_log(1, "DENIED %s %s %s — %s", acc.ip, method, url,
                   netaccess_pin_reason_name(acc.pin_reason));
        send_json(s, 403, body);
        closesocket(s);
        return 0;
    }
    /* 저장된 매장 사진은 보호 모드에서 내보내지 않습니다. 라이브 /snapshot 은 main.c 가
       이미 가린 프레임이라 그대로 응답해도 새는 것이 없습니다. */
    if (privacy_active_now() &&
        (strcmp(url, "/door/preview") == 0 || strcmp(url, "/surface/reference") == 0)) {
        send_json(s, 403, "{\"ok\":false,\"error\":\"privacy_locked\"}");
        closesocket(s);
        return 0;
    }

    if (strcmp(url, "/status") == 0) {
        StreamStatus st;
        char body[640];
        ULONGLONG until;
        int remaining = 0;
        EnterCriticalSection(&g_lock);
        st = g_status;
        until = g_privacy_unlock_until_ms;
        LeaveCriticalSection(&g_lock);
        if (until) { ULONGLONG now = GetTickCount64(); remaining = until > now ? (int)((until - now) / 1000) : 0; }
        snprintf(body, sizeof(body),
            "{\"door\":{\"enabled\":%d,\"state\":%d,\"label\":\"%s\",\"auto_phase\":%d,"
            "\"auto_stalled\":%d,\"roi_set\":%d,"
            "\"band_valid\":%d,\"band_active\":%d,\"band_signal\":%.1f,"
            "\"closed_ready\":%d,\"open_ready\":%d,\"auto_wait\":%.1f},"
            "\"residue\":{\"enabled\":%d,\"ready\":%d,\"auto_phase\":%d,\"auto_wait\":%.1f},"
            "\"privacy\":{\"mode\":%d,\"active\":%d,\"unlock_remaining\":%d,\"unlock_max\":%d},"
            "\"access\":{\"tier\":\"%s\",\"pin_required\":%d}}",
            st.door_enabled, st.door_state,
            st.door_state == 1 ? "open" : st.door_state == 0 ? "closed" : "unknown",
            st.door_auto_phase, st.door_auto_stalled, st.door_roi_set,
            st.door_band_valid, st.door_band_active, st.door_band_signal,
            st.door_closed_ready, st.door_open_ready, st.door_auto_wait,
            st.residue_enabled, st.residue_ready, st.residue_auto_phase, st.residue_auto_wait,
            g_privacy_mode, privacy_active_now(), remaining, g_privacy_unlock_max,
            netaccess_tier_name(acc.tier), (acc.tier == NET_TIER_LAN && pin[0]) ? 1 : 0);
        send_json(s, 200, body);

    } else if (strcmp(url, "/privacy/state") == 0) {
        char body[200];
        ULONGLONG until;
        int remaining = 0;
        EnterCriticalSection(&g_lock);
        until = g_privacy_unlock_until_ms;
        LeaveCriticalSection(&g_lock);
        if (until) { ULONGLONG now = GetTickCount64(); remaining = until > now ? (int)((until - now) / 1000) : 0; }
        snprintf(body, sizeof(body),
            "{\"mode\":%d,\"active\":%d,\"unlock_remaining\":%d,\"unlock_max\":%d,\"tier\":\"%s\",\"pin_required\":%d}",
            g_privacy_mode, privacy_active_now(), remaining, g_privacy_unlock_max,
            netaccess_tier_name(acc.tier), (acc.tier == NET_TIER_LAN && pin[0]) ? 1 : 0);
        send_json(s, 200, body);

    } else if (strcmp(url, "/privacy/unlock") == 0 && is_post) {
        const char *p = strstr(qs, "seconds=");
        int seconds = p ? atoi(p + 8) : 300;
        char body[96];
        if (seconds <= 0) seconds = 300;
        if (seconds > g_privacy_unlock_max) seconds = g_privacy_unlock_max;
        EnterCriticalSection(&g_lock);
        g_privacy_unlock_until_ms = GetTickCount64() + (ULONGLONG)seconds * 1000ULL;
        LeaveCriticalSection(&g_lock);
        access_log(0, "privacy unlocked by %s (%s) for %ds", acc.ip, netaccess_tier_name(acc.tier), seconds);
        snprintf(body, sizeof(body), "{\"ok\":true,\"unlock_remaining\":%d}", seconds);
        send_json(s, 200, body);

    } else if (strcmp(url, "/privacy/lock") == 0 && is_post) {
        EnterCriticalSection(&g_lock);
        g_privacy_unlock_until_ms = 0;
        LeaveCriticalSection(&g_lock);
        access_log(0, "privacy relocked by %s (%s)", acc.ip, netaccess_tier_name(acc.tier));
        send_json(s, 200, "{\"ok\":true}");

    } else if (strcmp(url,"/surface/reference")==0) {
        handle_surface_reference(s,qs);
    } else if (strcmp(url, "/surface/status") == 0) {
        char copy[32768];
        EnterCriticalSection(&g_lock);memcpy(copy,g_surface_status,sizeof(copy));LeaveCriticalSection(&g_lock);
        send_json(s,200,copy);
    } else if ((!strcmp(url,"/surface/capture")||!strcmp(url,"/surface/ack")) && strcmp(method,"POST")==0) {
        char id[40]={0};int k=0;const char *p=strstr(qs,"id=");
        if(p) {p+=3;while(*p&&*p!='&'&&k<39) {
            char c=*p++;if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')){k=0;break;}id[k++]=c;}}
        if(!k)send_json(s,400,"{\"error\":\"invalid id\"}");
        else {EnterCriticalSection(&g_lock);
            if(!strcmp(url,"/surface/ack")){const char *c=strstr(qs,"candidate="),*r=strstr(qs,"revision="),*v=strstr(qs,"version=");
                /* revision contains 'version='; require a query-key boundary. */
                v=strstr(qs,"&version=");
                snprintf(g_surface_ack,sizeof(g_surface_ack),"%s",id);g_surface_ack_candidate=c?atoi(c+10):-1;g_surface_ack_revision=r?atoi(r+9):-1;g_surface_ack_version=v?atoi(v+9):-1;}
            else{snprintf(g_surface_capture,sizeof(g_surface_capture),"%s",id);g_surface_empty=strstr(qs,"empty=1")!=NULL;}
            LeaveCriticalSection(&g_lock);
            access_log(0,"%s by %s (%s) id=%s",url+1,acc.ip,netaccess_tier_name(acc.tier),id);
            send_json(s,202,"{\"queued\":true}");}
    } else if (strcmp(url, "/stream") == 0) {
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
        access_log(0, "stream opened by %s (%s) privacy=%d",
                   acc.ip, netaccess_tier_name(acc.tier), privacy_active_now());

        uint32_t last_seq = (uint32_t)-1;
        /* 루프 안에서 매번 읽어 감속이 진행 중인 연결에도 곧바로 반영되게 합니다. */
        DWORD frame_ms = 1000 / (DWORD)(g_stream_fps > 0 ? g_stream_fps : 1);
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
            frame_ms = 1000 / (DWORD)(g_stream_fps > 0 ? g_stream_fps : 1);
            Sleep(frame_ms);
        }
        free(jpg);
        InterlockedDecrement(&g_clients);
        access_log(0, "stream closed by %s", acc.ip);

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

    /* 저장 계열은 POST 만 받습니다 — GET 을 열어 두면 위의 PIN 검사(POST 전용)를 우회합니다. */
    } else if ((strcmp(url, "/door/save") == 0 || strcmp(url, "/residue/save") == 0) && is_post &&
               privacy_active_now()) {
        /* 보호 모드에서는 g_rgb 가 main.c 가 가린 프레임입니다. 그걸 기준으로 저장하면 검은 사진이
           되어 문·잔류물 감지가 조용히 망가지므로 서버에서 막습니다. 대시보드는 캡처 전에
           /privacy/unlock 을 먼저 부릅니다. */
        access_log(1, "DENIED %s POST %s — privacy_locked (unlock first)", acc.ip, url);
        send_json(s, 409, "{\"ok\":false,\"error\":\"privacy_locked\"}");

    } else if (strcmp(url, "/door/save") == 0 && is_post) {
        int is_open = (strstr(qs, "state=open") != NULL);
        access_log(0, "door reference %s captured by %s (%s)",
                   is_open ? "open" : "closed", acc.ip, netaccess_tier_name(acc.tier));
        handle_door_save(s, is_open);

    } else if (strcmp(url, "/residue/save") == 0 && is_post) {
        access_log(0, "residue clean reference captured by %s (%s)",
                   acc.ip, netaccess_tier_name(acc.tier));
        handle_save_raw(s,
            "residue_clean_reference.raw",
            "{\"ok\":true,\"kind\":\"residue_clean\"}");

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
    addr.sin_addr.s_addr = INADDR_ANY;  /* 모든 인터페이스 수신 (LAN 접근 허용) */

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

    fprintf(stderr, "stream: http://0.0.0.0:%d  /stream /snapshot /status  privacy=%s pin=%s\n",
            port, g_privacy_mode ? "on" : "off", g_access_pin[0] ? "set" : "off");
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
        int fps = (g_clients > 0) ? (int)g_stream_fps : IDLE_PUSH_FPS;
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
