#ifndef YOLO11_H
#define YOLO11_H

#include <stddef.h>
#include <stdint.h>

/* COCO pose 규격의 관절 수. 모델 출력 채널 56 = 4(box) + 1(conf) + 17*3 의 근거. */
#define YOLO11_NUM_KEYPOINTS 17

/*
 * 관절 하나의 위치와 신뢰도입니다.
 *
 * score 가 낮으면 화면 밖이거나 가려진 상태이므로 좌표를 신뢰하면 안 됩니다.
 * 박스와 달리 이미지 경계로 clamp 하지 않습니다 — 화면 밖 관절을 경계로
 * 스냅하면 유효한 좌표인 것처럼 오해됩니다.
 */
typedef struct {
    float x;
    float y;
    float score;
} Keypoint;

/*
 * 한 사람을 둘러싸는 사각형과 신체 관절 정보입니다.
 *
 * (x1, y1): 왼쪽 위 좌표
 * (x2, y2): 오른쪽 아래 좌표
 * score:     모델이 사람이라고 판단한 확률에 가까운 값(0.0~1.0)
 *
 * 좌표는 YOLO 모델 입력 좌표가 아니라 원본 이미지 좌표입니다.
 *
 * keypoint_count == 0 이면 detection 전용 모델을 사용한 것이므로
 * kp 배열 내용은 의미가 없습니다.
 *
 * 메모리: Detection 하나가 약 224 바이트입니다. max_candidates=1024 기준
 * 후보 버퍼가 ~229 KB 이고, 시작 시 1회 할당되어 프레임 수와 무관합니다.
 */
typedef struct {
    float x1;
    float y1;
    float x2;
    float y2;
    float score;
    int keypoint_count;
    Keypoint kp[YOLO11_NUM_KEYPOINTS];
} Detection;

/*
 * Detection을 담는 길이 제한 배열입니다.
 *
 * items는 detection_list_init()이 한 번 할당하고,
 * detection_list_destroy()가 해제합니다. 프레임마다 malloc/free하지 않고 같은
 * 배열을 재사용하여 메모리 사용량과 처리 시간의 흔들림을 줄입니다.
 */
typedef struct {
    Detection *items;
    size_t count;
    size_t capacity;
} DetectionList;

/*
 * 원본 이미지를 모델 입력 크기로 바꿀 때 사용한 확대/축소 비율과 여백입니다.
 * YOLO가 알려준 박스 좌표에서 여백을 빼고 scale로 나누면 원본 좌표가 됩니다.
 */
typedef struct {
    int model_width;
    int model_height;
    int image_width;
    int image_height;
    float scale;
    int pad_x;
    int pad_y;
} Letterbox;

/* 이미지 크기 변경 방식입니다. 현재 프로그램은 기본적으로 BILINEAR를 사용합니다. */
typedef enum {
    RESIZE_BILINEAR = 0,
    RESIZE_NEAREST = 1
} ResizeMode;

/* ONNX Runtime이 모델 연산을 실행할 장치입니다. */
typedef enum {
    DETECTOR_PROVIDER_CPU = 0,
    DETECTOR_PROVIDER_DIRECTML = 1
} DetectorProvider;

/*
 * 감지기 내부 설정입니다.
 * 현재 명령줄에서 변경할 수 있는 값은 confidence뿐이며, 나머지는 main.c에서
 * 저사양 장치용 기본값으로 고정합니다.
 */
typedef struct {
    float confidence;
    float iou;
    size_t max_candidates;
    size_t max_detections;
    int threads;
    int low_memory;
    ResizeMode resize_mode;
    int fast_preprocess;
    int graph_optimization_all;
    int allow_spinning;
    /* 시작 시 더미 추론 횟수입니다. 첫 프레임 지연 스파이크를 서비스 전에 소진합니다. */
    int warmup_runs;
    DetectorProvider provider;
    int device_id;
} DetectorOptions;

typedef struct {
    double preprocess_seconds;
    double inference_seconds;
    double postprocess_seconds;
    /* 지연 분포: 최근 N회 추론의 p50/p95/최댓값 (ms). detector_run() 내에서 갱신됩니다. */
    double inference_p50_ms;
    double inference_p95_ms;
    double inference_max_ms;
} DetectorRunStats;

/*
 * Detector의 실제 멤버는 detector_ort.c에만 공개합니다.
 * 이렇게 불완전 타입(opaque type)을 사용하면 다른 파일이 ONNX Runtime 자원을
 * 실수로 직접 수정하거나 해제하는 일을 막을 수 있습니다.
 */
typedef struct Detector Detector;

/* 아래 함수들은 성공하면 0, 실패하면 -1을 반환하는 것이 기본 규칙입니다. */
int detection_list_init(DetectionList *list, size_t capacity);
void detection_list_destroy(DetectionList *list);

/* RGB 이미지를 letterbox로 크기 변경하면서 곧바로 float32 NCHW 텐서에 씁니다. */
int letterbox_to_nchw(const uint8_t *rgb, int width, int height, int stride,
                      float *tensor, int model_width, int model_height,
                      ResizeMode mode, Letterbox *transform);
int letterbox_to_nchw_fast(const uint8_t *rgb, int width, int height, int stride,
                           float *tensor, int model_width, int model_height,
                           ResizeMode mode, Letterbox *transform);

/* YOLO의 숫자 배열을 사람 박스로 변환하고, 겹치는 박스를 NMS로 제거합니다. */
int yolo11_decode(const float *output, const int64_t *shape, size_t rank,
                  const Letterbox *transform, float confidence, float iou,
                  DetectionList *detections, size_t max_detections);

/* 별도 이미지 복사본을 만들지 않고 전달받은 RGB 메모리 위에 직접 그립니다. */
void draw_detections(uint8_t *rgb, int width, int height, int stride,
                     const DetectionList *detections);

/* 왼쪽 상단에 추론 FPS / 카메라 FPS / CPU 사용률 / 온도를 표시합니다.
 * cpu_percent < 0이면 CPU 줄을 건너뜁니다. temperature_celsius < 0이면 온도 줄을 건너뜁니다. */
void draw_hud(uint8_t *rgb, int width, int height, int stride,
              float detect_fps, float camera_fps,
              float cpu_percent, int temperature_celsius);

/*
 * detector_create()로 만든 포인터는 반드시 detector_destroy()로 해제해야 합니다.
 * detector_run()은 생성 시 확보한 입출력 버퍼를 프레임마다 재사용합니다.
 */
Detector *detector_create(const char *model_path, const DetectorOptions *options,
                          char *error, size_t error_size);
int detector_run(Detector *detector, const uint8_t *rgb, int width, int height,
                 int stride, DetectionList *detections,
                 char *error, size_t error_size);
void detector_destroy(Detector *detector);
int detector_input_width(const Detector *detector);
int detector_input_height(const Detector *detector);
void detector_get_last_stats(const Detector *detector, DetectorRunStats *stats);

#endif
