#ifndef MEDIA_H
#define MEDIA_H

#include <stddef.h>
#include <stdint.h>

/*
 * FFmpeg가 디코딩한 한 프레임을 사용하기 쉬운 RGB 형태로 감싼 구조체입니다.
 *
 * data의 메모리는 media_process()가 소유하고 계속 재사용합니다. 콜백 함수는
 * 픽셀을 수정할 수 있지만, data를 free하거나 콜백 밖에 저장해 두면 안 됩니다.
 * stride는 한 줄의 실제 바이트 수로, 정렬용 여분 때문에 width * 3보다 클 수
 * 있습니다. 다음 줄은 반드시 data + y * stride로 찾아야 합니다.
 */
typedef struct {
    uint8_t *data;
    int width;
    int height;
    int stride;
    int64_t index;
} RgbFrame;

/*
 * 한 프레임이 준비될 때마다 media_process()가 호출하는 함수의 모양입니다.
 * opaque에는 호출자가 원하는 문맥 포인터(AppContext 등)를 넣을 수 있습니다.
 */
typedef int (*FrameCallback)(RgbFrame *frame, void *opaque,
                             char *error, size_t error_size);

/*
 * 실시간 장치 입력이 Ctrl+C 같은 종료 요청을 받았는지 확인하는 콜백입니다.
 * 0이 아닌 값을 반환하면 FFmpeg의 블로킹 입력 함수도 가능한 한 빨리 중단합니다.
 */
typedef int (*MediaInterruptCallback)(void *opaque);

/* 출력 및 선택적인 실시간 입력 장치 설정입니다. */
typedef struct {
    const char *codec;
    int quality;
    int max_frames;
    const char *input_format;
    const char *video_size;
    const char *framerate;
    MediaInterruptCallback interrupt_callback;
    void *interrupt_opaque;
    int realtime;
    struct MediaStats *stats;
} MediaOptions;

typedef struct MediaStats {
    int64_t frames;
    double frame_rate;
    double input_convert_seconds;
    double callback_seconds;
    double output_seconds;
} MediaStats;

/*
 * 입력 열기 → 프레임별 디코딩 → callback → 인코딩 → 출력 저장을 수행합니다.
 * 이미지도 FFmpeg에서는 프레임이 한 장인 영상처럼 같은 흐름으로 처리됩니다.
 */
int media_process(const char *input_path, const char *output_path,
                  const MediaOptions *options, FrameCallback callback,
                  void *opaque, char *error, size_t error_size);

/*
 * format(dshow/v4l2/avfoundation)에서 첫 번째 비디오 장치를 찾아
 * buf에 FFmpeg input 경로 형식으로 씁니다.
 *   Windows dshow  → "video=<장치이름>"
 *   Linux v4l2     → "/dev/videoN"
 *   macOS avfound. → "N" (인덱스 문자열)
 * 성공 시 0, 장치 없음·열거 실패 시 -1.
 */
int media_first_video_device(const char *format, char *buf, size_t bufsize);

#endif
