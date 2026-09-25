#ifndef CAPTURE_H
#define CAPTURE_H

#include "yolo11.h"

#include <stddef.h>
#include <stdint.h>

/*
 * 이벤트 발생 순간의 프레임을 로컬 JPEG 로 남깁니다.
 *
 * 저장 위치를 설정값으로 받지 않는 이유: "\\server\share" 같은 네트워크 경로가
 * 들어가면 영상이 기기 밖으로 나갑니다(CLAUDE.md: 영상 외부 전송 금지).
 * 호출자는 로컬 데이터 디렉터리 아래 고정 하위 폴더만 넘깁니다.
 */
typedef struct {
    char     dir[600];
    int      quality;     /* JPEG 품질 1~100 */
    /*
     * 박스를 그릴 프레임 사본. 원본 프레임에 그리면 스트림·녹화에 캡처용
     * 표시가 섞이므로 따로 복사합니다.
     * 소유: EventCapture. 첫 캡처 때 할당하고 해상도가 커질 때만 다시 할당합니다.
     * 해제: capture_destroy 에서 free.
     */
    uint8_t *buf;
    size_t   buf_size;
} EventCapture;

/* dir 을 만들고(이미 있으면 그대로) 준비합니다. 성공 0, 실패 -1. */
int  capture_init(EventCapture *cap, const char *dir, int quality,
                  char *error, size_t error_size);
void capture_destroy(EventCapture *cap);

/*
 * rgb 사본에 box 를 그려 "<dir>/<kind>_YYYYmmdd_HHMMSS_track<id>.jpg" 로 저장합니다.
 * rgb: 호출자 소유 프레임을 읽기만 합니다. box 가 NULL 이면 표시 없이 저장합니다.
 * privacy_mask: 1 이면 영상을 단색으로 지우고 박스·관절만 남깁니다.
 *   stream_privacy_mode 가 켜진 매장은 "출력에 실제 픽셀을 내보내지 않는다"가 정책이라
 *   증거 사진도 같은 규칙을 따라야 합니다. 위치·자세·시각은 그대로 남습니다.
 * path_out 에 저장한 경로를 씁니다(실패 시에도 시도한 경로).
 * 성공 0, 실패 -1.
 */
int  capture_save(EventCapture *cap, const char *kind, int track_id,
                  const uint8_t *rgb, int width, int height, int stride,
                  const Detection *box, int privacy_mask,
                  char *path_out, size_t path_size,
                  char *error, size_t error_size);

#endif /* CAPTURE_H */
