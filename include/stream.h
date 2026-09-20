#ifndef STREAM_H
#define STREAM_H

#include <stdint.h>
#include <stddef.h>

/*
 * MJPEG HTTP 스트리밍 서버 + 문 여닫이 기준 이미지 관리입니다.
 *
 * 엔드포인트:
 *   GET  /stream          — MJPEG 스트림 (브라우저 <img src="">)
 *   GET  /snapshot        — 현재 프레임 JPEG 1장
 *   POST /door/save       — 현재 프레임을 door_reference.raw로 저장
 *   GET  /door/preview    — 저장된 기준 이미지를 JPEG로 반환
 *   POST /residue/save    — 현재 프레임을 residue_clean_reference.raw로 저장
 *   GET  /status          — 문·잔류물·자동 캡처·개인정보 모드 집계 (대시보드 폴링용)
 *   GET  /privacy/state   — 개인정보 모드 상태 + 요청자의 접근 계층
 *   POST /privacy/unlock?seconds=N — 설정용 임시 해제 (LAN 은 PIN 필요, 서버가 타임아웃 강제)
 *   POST /privacy/lock    — 즉시 재잠금
 *
 * 사용 순서:
 *   stream_start(8081, "C:\\project")  -- 서버 시작 (백그라운드 스레드)
 *   stream_push(rgb, w, h, s)          -- 처리된 프레임 등록
 *   stream_stop()                      -- 서버 종료
 *
 * 보안: INADDR_ANY 에 바인드해 같은 LAN 의 점주 폰·태블릿에서 접근합니다.
 *       접근 판정은 netaccess.h 가 대시보드(8080)와 공유합니다 — 서브넷 밖 거부,
 *       상태를 바꾸는 POST 는 PIN, localhost(키오스크 자체)는 무제한.
 *       대시보드에서 직접 호출할 수 있도록 CORS 헤더를 포함합니다.
 */

/* GET /status 로 대시보드가 한 번에 읽는 집계 상태. main.c 가 프레임마다 채웁니다. */
typedef struct {
    int    door_enabled, door_state;          /* state: -1 미상 0 닫힘 1 열림 */
    int    door_auto_phase;                   /* DoorAutoPhase (door.h) */
    int    door_auto_stalled;                 /* 1=기준이 안 잡힘, ROI 재조정 필요 */
    int    door_roi_set;                      /* 0=ROI 미지정 — 자동 캡처 자체가 불가 */
    int    door_band_valid;                   /* -1 미평가, 0 무효(상인방 의심), 1 유효 */
    int    door_band_active;                  /* 1=이번 프레임을 상단 밴드로 판정 중 */
    double door_band_signal;
    int    door_closed_ready, door_open_ready;
    double door_auto_wait;
    int    residue_enabled, residue_ready, residue_auto_phase;
    double residue_auto_wait;
} StreamStatus;

#if defined(_WIN32)

/* data_dir: door_reference.raw를 저장할 디렉터리 (프로젝트 루트).
 * NULL이면 현재 작업 디렉터리를 사용합니다. */
int  stream_start(int port, const char *data_dir);
/* Snapshot mailbox: copied under stream lock; monitor runs on processing thread. */
void stream_surface_status(const char *json);
int stream_surface_capture_request(char *id, size_t size, int *empty);
int stream_surface_ack_request(char *id, size_t size, int *candidate, int *revision, int *version);

/*
 * 최신 프레임을 스트림 서버에 등록합니다.
 *
 * 매 프레임 호출해도 안전합니다 — 내부에서 전송 주기에 맞춰 스스로
 * 솎아냅니다. 1280x720 한 장 복사는 2.76MB 라 15fps 로 그대로 받으면
 * 83MB/s 가 되고, i5-4200U 의 L3 3MB 를 매번 비워 추론까지 느리게 만듭니다.
 *
 * rgb: 호출자 소유 프레임 버퍼를 읽기만 합니다. 반환 후 보관하지 않습니다.
 */
void stream_push(const uint8_t *rgb, int width, int height, int stride);
void stream_stop(void);

/*
 * 현재 /stream 에 연결된 클라이언트 수입니다.
 *
 * 0 이면 화면을 보는 사람이 없다는 뜻이므로, 호출자는 박스·HUD 그리기처럼
 * 사람이 볼 때만 의미가 있는 작업을 건너뛸 수 있습니다.
 */
int  stream_client_count(void);

/* 문 현재 상태를 갱신합니다. main.c에서 door_check 직후 호출하세요.
 * state: -1=알 수 없음, 0=닫힘, 1=열림 */
void stream_set_door_state  (int state);

/* 문 감지 활성 여부를 갱신합니다. door_enabled 설정 변경 시 호출하세요. */
void stream_set_door_enabled(int enabled);

/* ── 개인정보 보호 · 접근 제어 ─────────────────────────────────────────────
 * 스트림/스냅샷에 실제 영상을 내보내지 않는 모드입니다. 가리는 일은 main.c 가
 * 그리기 직전에 프레임을 단색으로 채워서 하고(그 위에 박스·스켈레톤만 그림),
 * 이 모듈은 "지금 가려야 하는가"를 결정하고 저장된 기준 사진 응답을 막습니다.
 * 설정 작업(ROI 드래그·기준 캡처)은 시간 제한 잠금 해제로 실제 화면을 잠시 봅니다.
 * setter 들은 stream_start 전에 불려도 안전합니다(락 없이 정적 변수만 씀). */
void stream_set_access_pin(const char *pin);      /* NULL/"" = PIN 비활성 */
void stream_set_privacy_mode(int enabled);        /* config stream_privacy_mode */
void stream_set_privacy_unlock_max(int seconds);  /* 잠금 해제 상한 (기본 600) */
int  stream_privacy_active(void);                 /* 1 이면 main.c 가 이번 프레임을 가려야 함 (main 스레드 전용) */

/*
 * 접근 로그 mailbox. 클라이언트 스레드는 SQLite 를 건드릴 수 없으므로(EventLog 의
 * 준비문은 main 스레드 전용) 여기에 쌓고, main.c 가 프레임마다 회수해 기록합니다.
 * level: 0=INFO 1=WARN. 반환 1=꺼냄, 0=비어 있음.
 */
int  stream_pop_access_log(int *level, char *msg, size_t size);
void stream_set_status(const StreamStatus *st);

#else /* !_WIN32 */

/*
 * stream.c 는 WinSocket2 전용이라 macOS/Linux 빌드에 포함되지 않습니다.
 * main.c 를 플랫폼 분기로 어지럽히지 않도록, 아무 일도 하지 않는 인라인
 * 정의를 둡니다. 이 플랫폼에서는 stream_port 가 0 으로 유지되므로 실제
 * 호출 경로도 열리지 않습니다.
 */
static inline int  stream_start(int port, const char *data_dir) {
    (void)port; (void)data_dir; return -1;
}
static inline void stream_push(const uint8_t *rgb, int width, int height,
                               int stride) {
    (void)rgb; (void)width; (void)height; (void)stride;
}
static inline void stream_stop(void) { }
static inline void stream_surface_status(const char *json) { (void)json; }
static inline int stream_surface_capture_request(char *id, size_t size, int *empty) {
    (void)id; (void)size; (void)empty; return 0;
}
static inline int stream_surface_ack_request(char *id, size_t size, int *candidate, int *revision, int *version) {
    (void)id; (void)size; (void)candidate; (void)revision; (void)version; return 0;
}
static inline int  stream_client_count(void) { return 0; }
static inline void stream_set_door_state(int state) { (void)state; }
static inline void stream_set_door_enabled(int enabled) { (void)enabled; }
static inline void stream_set_access_pin(const char *pin) { (void)pin; }
static inline void stream_set_privacy_mode(int enabled) { (void)enabled; }
static inline void stream_set_privacy_unlock_max(int seconds) { (void)seconds; }
static inline int  stream_privacy_active(void) { return 0; }
static inline int  stream_pop_access_log(int *level, char *msg, size_t size) {
    (void)level; (void)msg; (void)size; return 0;
}
static inline void stream_set_status(const StreamStatus *st) { (void)st; }

#endif /* _WIN32 */

#endif /* STREAM_H */
