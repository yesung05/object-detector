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

/* data_dir: door_reference.raw를 저장할 디렉터리 (프로젝트 루트).
 * NULL이면 현재 작업 디렉터리를 사용합니다. */
int  stream_start(int port, const char *data_dir);
void stream_push(const uint8_t *rgb, int width, int height, int stride);
void stream_stop(void);

/* 문 현재 상태를 갱신합니다. main.c에서 door_check 직후 호출하세요.
 * state: -1=알 수 없음, 0=닫힘, 1=열림 */
void stream_set_door_state  (int state);

/* 문 감지 활성 여부를 갱신합니다. door_enabled 설정 변경 시 호출하세요. */
void stream_set_door_enabled(int enabled);

#endif /* STREAM_H */
