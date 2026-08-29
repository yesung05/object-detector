#ifndef STREAM_H
#define STREAM_H

#include <stdint.h>

/*
 * MJPEG HTTP 스트리밍 서버입니다.
 *
 * 사용 순서:
 *   stream_start(8081)         -- 127.0.0.1:port 서버 시작 (백그라운드 스레드)
 *   stream_push(rgb, w, h, s)  -- 처리된 프레임을 전달 (process_frame에서 호출)
 *   stream_stop()              -- 서버 종료 및 스레드 정리
 *
 * 브라우저에서 http://localhost:<port>/stream 으로 접속합니다.
 * --stream-port 가 지정되지 않으면 stream_start를 호출하지 않으면 됩니다.
 *
 * 설계 원칙:
 * - 최신 프레임 하나만 버퍼에 유지 (링 버퍼 불필요, 메모리 고정)
 * - stream_push는 RGB를 복사만 하고 즉시 반환 (process_frame을 블로킹하지 않음)
 * - 실제 JPEG 인코딩과 전송은 각 클라이언트 스레드에서 수행
 * - 동시 접속자 제한: 4명 (매장 관리 용도)
 *
 * 보안: 127.0.0.1 전용 — 외부 네트워크에 노출되지 않습니다.
 */

/* stream_start: 서버를 시작합니다. 성공 0, 실패 -1. */
int stream_start(int port);

/* stream_push: 최신 프레임을 등록합니다. detector가 없으면 호출하지 않아도 됩니다. */
void stream_push(const uint8_t *rgb, int width, int height, int stride);

/* stream_stop: 서버를 정지하고 리소스를 해제합니다. */
void stream_stop(void);

#endif /* STREAM_H */
