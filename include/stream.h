#ifndef STREAM_H
#define STREAM_H

#include <stdint.h>

/*
 * MJPEG HTTP 스트리밍 서버 + 문 여닫이 기준 이미지 관리입니다.
 *
 * 엔드포인트:
 *   GET  /stream        — MJPEG 스트림 (브라우저 <img src="">)
 *   GET  /snapshot      — 현재 프레임 JPEG 1장
 *   POST /door/save     — 현재 프레임을 door_reference.raw로 저장
 *   GET  /door/preview  — 저장된 기준 이미지를 JPEG로 반환
 *
 * 사용 순서:
 *   stream_start(8081, "C:\\project")  -- 서버 시작 (백그라운드 스레드)
 *   stream_push(rgb, w, h, s)          -- 처리된 프레임 등록
 *   stream_stop()                      -- 서버 종료
 *
 * 보안: 127.0.0.1 전용 — 외부 네트워크에 노출되지 않습니다.
 *       대시보드(8080)에서 직접 호출할 수 있도록 CORS 헤더를 포함합니다.
 */

#if defined(_WIN32)

/* data_dir: door_reference.raw를 저장할 디렉터리 (프로젝트 루트).
 * NULL이면 현재 작업 디렉터리를 사용합니다. */
int  stream_start(int port, const char *data_dir);

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
static inline int  stream_client_count(void) { return 0; }
static inline void stream_set_door_state(int state) { (void)state; }
static inline void stream_set_door_enabled(int enabled) { (void)enabled; }

#endif /* _WIN32 */

#endif /* STREAM_H */
