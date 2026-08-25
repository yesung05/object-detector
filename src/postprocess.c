#include "yolo11.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* value가 low~high 범위를 벗어나지 않게 잘라내는 작은 보조 함수입니다. */
static float clampf(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

int detection_list_init(DetectionList *list, size_t capacity) {
    /*
     * capacity * 원소 크기의 곱셈이 size_t 범위를 넘지 않는지 먼저 검사합니다.
     * calloc은 배열을 할당하는 동시에 0으로 초기화합니다.
     */
    if (!list || capacity == 0 ||
        capacity > SIZE_MAX / sizeof(*list->items)) {
        return -1;
    }
    list->items = (Detection *)calloc(capacity, sizeof(*list->items));
    if (!list->items) return -1;
    list->count = 0;
    list->capacity = capacity;
    return 0;
}

void detection_list_destroy(DetectionList *list) {
    /* 해제 후 포인터와 크기를 0으로 바꾸면 실수로 재사용하기가 더 어려워집니다. */
    if (!list) return;
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/*
 * 원본 이미지의 실수 좌표 (x, y)에 해당하는 RGB 채널값을 계산합니다.
 * 주변 4픽셀을 거리에 따라 섞는 bilinear 방식이라 nearest보다 부드럽습니다.
 */
static uint8_t sample_bilinear(const uint8_t *rgb, int stride, int width,
                               int height, float x, float y, int channel) {
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    int x1;
    int y1;
    float fx;
    float fy;
    float top;
    float bottom;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    x1 = x0 + 1;
    y1 = y0 + 1;
    if (x1 >= width) x1 = width - 1;
    if (y1 >= height) y1 = height - 1;

    /* fx/fy는 왼쪽 위 픽셀에서 오른쪽/아래로 얼마나 이동했는지 나타냅니다. */
    fx = x - floorf(x);
    fy = y - floorf(y);
    top = rgb[y0 * stride + x0 * 3 + channel] * (1.0f - fx) +
          rgb[y0 * stride + x1 * 3 + channel] * fx;
    bottom = rgb[y1 * stride + x0 * 3 + channel] * (1.0f - fx) +
             rgb[y1 * stride + x1 * 3 + channel] * fx;
    return (uint8_t)(top * (1.0f - fy) + bottom * fy + 0.5f);
}

int letterbox_to_nchw(const uint8_t *rgb, int width, int height, int stride,
                      float *tensor, int model_width, int model_height,
                      ResizeMode mode, Letterbox *transform) {
    size_t plane;
    size_t total;
    float scale;
    int resized_width;
    int resized_height;
    int pad_x;
    int pad_y;
    int y;
    int x;

    if (!rgb || !tensor || !transform || width <= 0 || height <= 0 ||
        stride < width * 3 || model_width <= 0 || model_height <= 0) {
        return -1;
    }

    /*
     * NCHW는 R 평면 전체, G 평면 전체, B 평면 전체가 차례로 놓이는 형식입니다.
     * plane은 채널 하나의 원소 수입니다. 먼저 전체를 YOLO 기본 여백색 114로
     * 채워 두고, 실제 이미지가 들어갈 중앙 영역만 덮어씁니다.
     */
    plane = (size_t)model_width * (size_t)model_height;
    if (plane > SIZE_MAX / (3 * sizeof(float))) return -1;
    total = plane * 3;
    for (size_t i = 0; i < total; ++i) tensor[i] = 114.0f / 255.0f;

    /*
     * 가로/세로 비율 중 작은 것을 선택하면 원본 비율을 유지하면서 모델 영역
     * 밖으로 나가지 않습니다. 남는 공간은 양쪽에 똑같이 나누어 넣습니다.
     */
    scale = fminf((float)model_width / (float)width,
                  (float)model_height / (float)height);
    resized_width = (int)roundf((float)width * scale);
    resized_height = (int)roundf((float)height * scale);
    if (resized_width > model_width) resized_width = model_width;
    if (resized_height > model_height) resized_height = model_height;
    pad_x = (model_width - resized_width) / 2;
    pad_y = (model_height - resized_height) / 2;

    /*
     * 모델 이미지의 각 픽셀이 원본 이미지의 어디에서 왔는지 역으로 계산합니다.
     * 별도의 "크기 변경된 RGB 이미지"를 만들지 않고 바로 float 텐서에 쓰므로
     * 전체 프레임 버퍼 하나를 절약합니다.
     */
    for (y = 0; y < resized_height; ++y) {
        float sy = ((float)y + 0.5f) / scale - 0.5f;
        for (x = 0; x < resized_width; ++x) {
            float sx = ((float)x + 0.5f) / scale - 0.5f;
            size_t dst = (size_t)(y + pad_y) * (size_t)model_width +
                         (size_t)(x + pad_x);
            for (int c = 0; c < 3; ++c) {
                uint8_t value;
                if (mode == RESIZE_NEAREST) {
                    int nx = (int)floorf(sx + 0.5f);
                    int ny = (int)floorf(sy + 0.5f);
                    if (nx < 0) nx = 0;
                    if (ny < 0) ny = 0;
                    if (nx >= width) nx = width - 1;
                    if (ny >= height) ny = height - 1;
                    value = rgb[ny * stride + nx * 3 + c];
                } else {
                    value = sample_bilinear(rgb, stride, width, height,
                                            sx, sy, c);
                }
                /* uint8(0~255)을 모델이 기대하는 float(0.0~1.0)로 정규화합니다. */
                tensor[(size_t)c * plane + dst] = (float)value / 255.0f;
            }
        }
    }

    /* 나중에 모델 박스를 원본 좌표로 되돌릴 수 있도록 변환 정보를 저장합니다. */
    transform->model_width = model_width;
    transform->model_height = model_height;
    transform->image_width = width;
    transform->image_height = height;
    transform->scale = scale;
    transform->pad_x = pad_x;
    transform->pad_y = pad_y;
    return 0;
}

/*
 * Same transform as letterbox_to_nchw(), but coordinate and interpolation
 * work is shared by the three color channels. Keeping the reference function
 * makes numerical agreement and performance independently measurable.
 */
int letterbox_to_nchw_fast(const uint8_t *rgb, int width, int height, int stride,
                           float *tensor, int model_width, int model_height,
                           ResizeMode mode, Letterbox *transform) {
    const float padding = 114.0f / 255.0f;
    size_t plane;
    size_t total;
    float scale;
    int resized_width;
    int resized_height;
    int pad_x;
    int pad_y;

    if (!rgb || !tensor || !transform || width <= 0 || height <= 0 ||
        stride < width * 3 || model_width <= 0 || model_height <= 0) {
        return -1;
    }
    plane = (size_t)model_width * (size_t)model_height;
    if (plane > SIZE_MAX / (3 * sizeof(float))) return -1;
    total = plane * 3;
    for (size_t i = 0; i < total; ++i) tensor[i] = padding;

    scale = fminf((float)model_width / (float)width,
                  (float)model_height / (float)height);
    resized_width = (int)roundf((float)width * scale);
    resized_height = (int)roundf((float)height * scale);
    if (resized_width > model_width) resized_width = model_width;
    if (resized_height > model_height) resized_height = model_height;
    pad_x = (model_width - resized_width) / 2;
    pad_y = (model_height - resized_height) / 2;

    for (int y = 0; y < resized_height; ++y) {
        float sy = ((float)y + 0.5f) / scale - 0.5f;
        int raw_y0 = (int)floorf(sy);
        float fy = sy - floorf(sy);
        int y0 = raw_y0 < 0 ? 0 : raw_y0;
        int y1 = y0 + 1;
        if (y0 >= height) y0 = height - 1;
        if (y1 >= height) y1 = height - 1;

        for (int x = 0; x < resized_width; ++x) {
            float sx = ((float)x + 0.5f) / scale - 0.5f;
            size_t dst = (size_t)(y + pad_y) * (size_t)model_width +
                         (size_t)(x + pad_x);

            if (mode == RESIZE_NEAREST) {
                int nx = (int)floorf(sx + 0.5f);
                int ny = (int)floorf(sy + 0.5f);
                const uint8_t *pixel;
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx >= width) nx = width - 1;
                if (ny >= height) ny = height - 1;
                pixel = rgb + ny * stride + nx * 3;
                tensor[dst] = (float)pixel[0] / 255.0f;
                tensor[plane + dst] = (float)pixel[1] / 255.0f;
                tensor[2 * plane + dst] = (float)pixel[2] / 255.0f;
            } else {
                int raw_x0 = (int)floorf(sx);
                float fx = sx - floorf(sx);
                int x0 = raw_x0 < 0 ? 0 : raw_x0;
                int x1 = x0 + 1;
                const uint8_t *p00;
                const uint8_t *p01;
                const uint8_t *p10;
                const uint8_t *p11;
                if (x0 >= width) x0 = width - 1;
                if (x1 >= width) x1 = width - 1;
                p00 = rgb + y0 * stride + x0 * 3;
                p01 = rgb + y0 * stride + x1 * 3;
                p10 = rgb + y1 * stride + x0 * 3;
                p11 = rgb + y1 * stride + x1 * 3;
                for (int c = 0; c < 3; ++c) {
                    float top = (float)p00[c] * (1.0f - fx) +
                                (float)p01[c] * fx;
                    float bottom = (float)p10[c] * (1.0f - fx) +
                                   (float)p11[c] * fx;
                    uint8_t value = (uint8_t)(top * (1.0f - fy) +
                                              bottom * fy + 0.5f);
                    tensor[(size_t)c * plane + dst] = (float)value / 255.0f;
                }
            }
        }
    }

    transform->model_width = model_width;
    transform->model_height = model_height;
    transform->image_width = width;
    transform->image_height = height;
    transform->scale = scale;
    transform->pad_x = pad_x;
    transform->pad_y = pad_y;
    return 0;
}

/* qsort가 요구하는 비교 함수입니다. 점수가 높은 Detection이 앞에 오게 합니다. */
static int score_descending(const void *a, const void *b) {
    const Detection *da = (const Detection *)a;
    const Detection *db = (const Detection *)b;
    return (da->score < db->score) - (da->score > db->score);
}

/*
 * IoU(Intersection over Union):
 * 두 박스가 겹친 면적 / 두 박스를 합친 면적입니다.
 * 0이면 전혀 겹치지 않고, 1이면 같은 박스입니다.
 */
static float intersection_over_union(const Detection *a, const Detection *b) {
    float left = fmaxf(a->x1, b->x1);
    float top = fmaxf(a->y1, b->y1);
    float right = fminf(a->x2, b->x2);
    float bottom = fminf(a->y2, b->y2);
    float width = fmaxf(0.0f, right - left);
    float height = fmaxf(0.0f, bottom - top);
    float intersection = width * height;
    float area_a = fmaxf(0.0f, a->x2 - a->x1) *
                   fmaxf(0.0f, a->y2 - a->y1);
    float area_b = fmaxf(0.0f, b->x2 - b->x1) *
                   fmaxf(0.0f, b->y2 - b->y1);
    float denominator = area_a + area_b - intersection;
    return denominator > 0.0f ? intersection / denominator : 0.0f;
}

/*
 * 후보 배열에 새 박스를 넣되 capacity를 절대 넘지 않습니다.
 * 배열이 가득 찼다면 현재 최저 점수보다 좋은 후보만 그 자리를 교체합니다.
 * 따라서 입력 장면이 복잡해도 메모리 사용량에는 고정 상한이 있습니다.
 */
static void append_candidate(DetectionList *list, const Detection *candidate) {
    if (list->count < list->capacity) {
        list->items[list->count++] = *candidate;
        return;
    }

    size_t weakest = 0;
    for (size_t i = 1; i < list->count; ++i) {
        if (list->items[i].score < list->items[weakest].score) weakest = i;
    }
    if (candidate->score > list->items[weakest].score) {
        list->items[weakest] = *candidate;
    }
}

/* letterbox 여백과 확대 비율을 반대로 적용하여 원본 이미지 좌표로 돌아갑니다. */
static Detection map_box(float x1, float y1, float x2, float y2, float score,
                         const Letterbox *t) {
    Detection d;
    d.x1 = clampf((x1 - (float)t->pad_x) / t->scale, 0.0f,
                  (float)(t->image_width - 1));
    d.y1 = clampf((y1 - (float)t->pad_y) / t->scale, 0.0f,
                  (float)(t->image_height - 1));
    d.x2 = clampf((x2 - (float)t->pad_x) / t->scale, 0.0f,
                  (float)(t->image_width - 1));
    d.y2 = clampf((y2 - (float)t->pad_y) / t->scale, 0.0f,
                  (float)(t->image_height - 1));
    d.score = score;
    return d;
}

int yolo11_decode(const float *output, const int64_t *shape, size_t rank,
                  const Letterbox *transform, float confidence, float iou,
                  DetectionList *detections, size_t max_detections) {
    size_t predictions;
    size_t channels;
    int channel_first;
    int embedded_nms;

    if (!output || !shape || !transform || !detections || !detections->items ||
        rank != 3 || shape[0] != 1 || shape[1] <= 0 || shape[2] <= 0 ||
        confidence < 0.0f || confidence > 1.0f ||
        iou < 0.0f || iou > 1.0f) {
        return -1;
    }
    detections->count = 0;

    /*
     * YOLO 내보내기 방식에 따라 출력 배열의 모양이 다를 수 있습니다.
     *
     * [1,N,6]  : 박스 좌표 4개 + 점수 + 클래스, 모델 내부에서 NMS까지 완료
     * [1,84,N] : 채널이 먼저 오는 일반 COCO detection 출력
     * [1,N,84] : 후보가 먼저 오는 같은 정보의 다른 배치
     */
    embedded_nms = (shape[2] == 6 && shape[1] <= 4096);
    if (embedded_nms) {
        predictions = (size_t)shape[1];
        channels = 6;
        channel_first = 0;
    } else if (shape[1] >= 5 && shape[1] <= 512 && shape[2] > shape[1]) {
        channels = (size_t)shape[1];
        predictions = (size_t)shape[2];
        channel_first = 1;
    } else if (shape[2] >= 5 && shape[2] <= 512 && shape[1] > shape[2]) {
        predictions = (size_t)shape[1];
        channels = (size_t)shape[2];
        channel_first = 0;
    } else {
        return -1;
    }

    /* 모든 후보 중 class 0(person)의 점수가 confidence 이상인 박스만 모읍니다. */
    for (size_t i = 0; i < predictions; ++i) {
        Detection d;
        if (embedded_nms) {
            const float *row = output + i * channels;
            if (row[4] < confidence || (int)row[5] != 0) continue;
            d = map_box(row[0], row[1], row[2], row[3], row[4], transform);
        } else {
            float cx;
            float cy;
            float width;
            float height;
            float score;
            if (channel_first) {
                /*
                 * 배열이 채널별 평면으로 저장되어 있으므로 같은 후보 i의 값도
                 * predictions만큼 떨어져 있습니다.
                 */
                cx = output[0 * predictions + i];
                cy = output[1 * predictions + i];
                width = output[2 * predictions + i];
                height = output[3 * predictions + i];
                score = output[4 * predictions + i]; /* COCO의 class 0은 person입니다. */
            } else {
                const float *row = output + i * channels;
                cx = row[0];
                cy = row[1];
                width = row[2];
                height = row[3];
                score = row[4];
            }
            if (score < confidence) continue;
            d = map_box(cx - width * 0.5f, cy - height * 0.5f,
                        cx + width * 0.5f, cy + height * 0.5f,
                        score, transform);
        }
        if (d.x2 > d.x1 && d.y2 > d.y1) append_candidate(detections, &d);
    }

    /*
     * NMS(Non-Maximum Suppression):
     * 1) 점수가 높은 순서로 정렬합니다.
     * 2) 이미 선택한 박스와 IoU가 기준보다 큰 후보는 같은 사람의 중복으로 봅니다.
     * 3) 겹치지 않은 박스만 앞으로 복사하고 최대 개수에서 멈춥니다.
     *
     * 별도의 결과 배열을 만들지 않고 items 안에서 앞쪽으로 덮어써 메모리를 아낍니다.
     */
    qsort(detections->items, detections->count, sizeof(*detections->items),
          score_descending);
    size_t kept = 0;
    for (size_t i = 0; i < detections->count; ++i) {
        int suppressed = 0;
        for (size_t j = 0; j < kept; ++j) {
            if (intersection_over_union(&detections->items[i],
                                        &detections->items[j]) > iou) {
                suppressed = 1;
                break;
            }
        }
        if (!suppressed) {
            detections->items[kept++] = detections->items[i];
            if (kept >= max_detections) break;
        }
    }
    detections->count = kept;
    return 0;
}
