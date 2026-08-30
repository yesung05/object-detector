#include "test_runner.h"
#include "yolo11.h"
#include "tracker.h"
#include "platform.h"
#include "camera_health.h"
#include "config.h"
#include "gray.h"
#include "log.h"
#include "rules.h"
#include "tracks.h"
#include "door.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 기존 테스트 (assert → ASSERT_TRUE/EXPECT_* 변환) ─────────────────── */

static void test_letterbox(void) {
    const uint8_t image[2 * 1 * 3] = { 255, 0, 0, 0, 255, 0 };
    float tensor[3 * 4 * 4];
    Letterbox t;
    ASSERT_INT_EQ(letterbox_to_nchw(image, 2, 1, 6, tensor, 4, 4,
                                    RESIZE_NEAREST, &t), 0);
    EXPECT_FLOAT_NEAR(t.scale, 2.0f, 0.001f);
    EXPECT_INT_EQ(t.pad_x, 0);
    EXPECT_INT_EQ(t.pad_y, 1);
    EXPECT_FLOAT_NEAR(tensor[0], 114.0f / 255.0f, 0.001f);
    EXPECT_TRUE(tensor[4] > 0.99f);
}

static void test_fast_letterbox_matches_reference(void) {
    enum { WIDTH = 13, HEIGHT = 9, MODEL = 16 };
    uint8_t image[WIDTH * HEIGHT * 3];
    float reference[MODEL * MODEL * 3];
    float optimized[MODEL * MODEL * 3];
    Letterbox a, b;
    for (size_t i = 0; i < sizeof(image); ++i)
        image[i] = (uint8_t)((i * 37u + 11u) & 255u);
    ASSERT_INT_EQ(letterbox_to_nchw(image, WIDTH, HEIGHT, WIDTH * 3, reference,
                                    MODEL, MODEL, RESIZE_BILINEAR, &a), 0);
    ASSERT_INT_EQ(letterbox_to_nchw_fast(image, WIDTH, HEIGHT, WIDTH * 3, optimized,
                                          MODEL, MODEL, RESIZE_BILINEAR, &b), 0);
    ASSERT_TRUE(memcmp(reference, optimized, sizeof(reference)) == 0);
    ASSERT_TRUE(memcmp(&a, &b, sizeof(a)) == 0);
}

static void make_tracking_frame(uint8_t *image, int width, int height, int offset_x) {
    memset(image, 16, (size_t)width * (size_t)height * 3);
    for (int y = 24; y < 72; ++y)
        for (int x = 32 + offset_x; x < 80 + offset_x; ++x) {
            uint8_t *pixel = image + (y * width + x) * 3;
            uint8_t value = (uint8_t)(((x + y * 3) & 7) * 25 + 50);
            pixel[0] = value;
            pixel[1] = (uint8_t)(255 - value);
            pixel[2] = (uint8_t)(value / 2);
        }
}

static void test_light_tracker_translation(void) {
    enum { WIDTH = 128, HEIGHT = 96 };
    uint8_t previous[WIDTH * HEIGHT * 3];
    uint8_t current[WIDTH * HEIGHT * 3];
    DetectionList detections;
    TrackerOptions options = {4, 3, 2, 24};
    LightTracker *tracker = tracker_create(&options);
    char error[128] = {0};
    int request_detection = 0;
    ASSERT_TRUE(tracker != NULL);
    ASSERT_INT_EQ(detection_list_init(&detections, 4), 0);
    detections.count = 1;
    detections.items[0] = (Detection){32, 24, 79, 71, 0.9f};
    make_tracking_frame(previous, WIDTH, HEIGHT, 0);
    make_tracking_frame(current, WIDTH, HEIGHT, 4);
    ASSERT_INT_EQ(tracker_reset(tracker, previous, WIDTH, HEIGHT, WIDTH * 3,
                                error, sizeof(error)), 0);
    ASSERT_INT_EQ(tracker_update(tracker, current, WIDTH, HEIGHT, WIDTH * 3,
                                 &detections, &request_detection,
                                 error, sizeof(error)), 0);
    EXPECT_TRUE(request_detection == 0);
    EXPECT_FLOAT_NEAR(detections.items[0].x1, 36.0f, 1.0f);
    EXPECT_FLOAT_NEAR(detections.items[0].x2, 83.0f, 1.0f);
    detection_list_destroy(&detections);
    tracker_destroy(tracker);
}

static void test_decode_and_nms(void) {
    /*
     * [1, 5, 7]은 cx, cy, width, height, person confidence 순서입니다.
     * 크게 겹치는 두 사람 박스가 NMS 뒤에 하나만 남는지 검사합니다.
     */
    const float output[] = {
        50.0f, 52.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        50.0f, 52.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        40.0f, 40.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        40.0f, 40.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.90f, 0.80f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f
    };
    const int64_t shape[] = {1, 5, 7};
    Letterbox t = {100, 100, 100, 100, 1.0f, 0, 0};
    DetectionList list;
    ASSERT_INT_EQ(detection_list_init(&list, 8), 0);
    ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, 0.25f, 0.45f, &list, 8), 0);
    ASSERT_INT_EQ((int)list.count, 1);
    EXPECT_FLOAT_NEAR(list.items[0].score, 0.90f, 0.001f);
    EXPECT_FLOAT_NEAR(list.items[0].x1, 30.0f, 0.001f);
    detection_list_destroy(&list);
}

static void test_draw_bounds(void) {
    enum { WIDTH = 24, HEIGHT = 24, GUARD = 32 };
    uint8_t *memory = (uint8_t *)malloc(GUARD + WIDTH * HEIGHT * 3 + GUARD);
    DetectionList list;
    ASSERT_TRUE(memory != NULL);
    /*
     * 이미지 앞뒤의 GUARD 영역을 0xA5로 채웁니다. 그리기 후에도 그대로라면
     * draw_detections가 할당된 이미지 범위를 넘겨 쓰지 않은 것입니다.
     */
    memset(memory, 0xa5, GUARD + WIDTH * HEIGHT * 3 + GUARD);
    ASSERT_INT_EQ(detection_list_init(&list, 1), 0);
    list.count = 1;
    list.items[0] = (Detection){0, 0, WIDTH - 1, HEIGHT - 1, 0.88f};
    draw_detections(memory + GUARD, WIDTH, HEIGHT, WIDTH * 3, &list);
    for (int i = 0; i < GUARD; ++i) {
        EXPECT_TRUE(memory[i] == 0xa5);
        EXPECT_TRUE(memory[GUARD + WIDTH * HEIGHT * 3 + i] == 0xa5);
    }
    detection_list_destroy(&list);
    free(memory);
}

/* ── 신규 테스트 ──────────────────────────────────────────────────────── */

static void test_detection_list_lifecycle(void) {
    DetectionList list;
    EXPECT_INT_EQ(detection_list_init(NULL, 4), -1);
    EXPECT_INT_EQ(detection_list_init(&list, 0), -1);
    ASSERT_INT_EQ(detection_list_init(&list, 2), 0);
    EXPECT_TRUE(list.items != NULL);
    EXPECT_INT_EQ((int)list.count, 0);
    EXPECT_INT_EQ((int)list.capacity, 2);
    detection_list_destroy(&list);
    EXPECT_TRUE(list.items == NULL);
    /* 두 번째 호출: items=NULL 상태에서 free(NULL)은 안전해야 합니다 */
    detection_list_destroy(&list);
}

static void test_letterbox_wide_image(void) {
    /* 10×3 → 4×4: scale=0.4, resized=(4,1), pad_x=0, pad_y=1 */
    uint8_t image[10 * 3 * 3];
    float tensor[3 * 4 * 4];
    Letterbox t;
    memset(image, 128, sizeof(image));
    ASSERT_INT_EQ(letterbox_to_nchw(image, 10, 3, 30, tensor, 4, 4,
                                    RESIZE_NEAREST, &t), 0);
    EXPECT_FLOAT_NEAR(t.scale, 0.4f, 0.001f);
    EXPECT_INT_EQ(t.pad_x, 0);
    EXPECT_INT_EQ(t.pad_y, 1);
}

static void test_letterbox_tall_image(void) {
    /* 3×10 → 4×4: scale=0.4, resized=(1,4), pad_x=1, pad_y=0 */
    uint8_t image[3 * 10 * 3];
    float tensor[3 * 4 * 4];
    Letterbox t;
    memset(image, 128, sizeof(image));
    ASSERT_INT_EQ(letterbox_to_nchw(image, 3, 10, 9, tensor, 4, 4,
                                    RESIZE_NEAREST, &t), 0);
    EXPECT_FLOAT_NEAR(t.scale, 0.4f, 0.001f);
    EXPECT_INT_EQ(t.pad_x, 1);
    EXPECT_INT_EQ(t.pad_y, 0);
}

static void test_letterbox_invalid_args(void) {
    uint8_t image[4 * 4 * 3];
    float tensor[3 * 4 * 4];
    Letterbox t;
    EXPECT_INT_EQ(letterbox_to_nchw(NULL,  4, 4, 12, tensor, 4, 4, RESIZE_NEAREST, &t), -1);
    EXPECT_INT_EQ(letterbox_to_nchw(image, 4, 4, 12, NULL,   4, 4, RESIZE_NEAREST, &t), -1);
    EXPECT_INT_EQ(letterbox_to_nchw(image, 0, 4, 12, tensor, 4, 4, RESIZE_NEAREST, &t), -1);
    /* stride(5) < width(4)*3=12 → 무효 */
    EXPECT_INT_EQ(letterbox_to_nchw(image, 4, 4,  5, tensor, 4, 4, RESIZE_NEAREST, &t), -1);
    EXPECT_INT_EQ(letterbox_to_nchw_fast(NULL,  4, 4, 12, tensor, 4, 4, RESIZE_BILINEAR, &t), -1);
    EXPECT_INT_EQ(letterbox_to_nchw_fast(image, 4, 4, 12, NULL,   4, 4, RESIZE_BILINEAR, &t), -1);
}

static void test_decode_channel_first(void) {
    /* [1, 5, 100]: shape[1]=5 < shape[2]=100 → channel_first */
    float output[5 * 100];
    int64_t shape[] = {1, 5, 100};
    /* scale=1, pad=0, model과 image 모두 100×100 */
    Letterbox t = {100, 100, 100, 100, 1.0f, 0, 0};
    DetectionList list;
    memset(output, 0, sizeof(output));
    /* prediction 0만 유효: cx=50, cy=50, w=40, h=40, score=0.9 */
    output[0 * 100 + 0] = 50.0f;
    output[1 * 100 + 0] = 50.0f;
    output[2 * 100 + 0] = 40.0f;
    output[3 * 100 + 0] = 40.0f;
    output[4 * 100 + 0] = 0.9f;
    ASSERT_INT_EQ(detection_list_init(&list, 8), 0);
    ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, 0.25f, 0.45f, &list, 8), 0);
    EXPECT_INT_EQ((int)list.count, 1);
    EXPECT_FLOAT_NEAR(list.items[0].score, 0.9f, 0.001f);
    /* cx=50, w=40 → x1 = 50-20 = 30 (map_box 후 scale=1, pad=0이므로 그대로) */
    EXPECT_FLOAT_NEAR(list.items[0].x1, 30.0f, 0.001f);
    detection_list_destroy(&list);
}

static void test_decode_channel_last(void) {
    /* [1, 100, 5]: shape[1]=100 > shape[2]=5 → channel_last */
    float output[100 * 5];
    int64_t shape[] = {1, 100, 5};
    Letterbox t = {100, 100, 100, 100, 1.0f, 0, 0};
    DetectionList list;
    memset(output, 0, sizeof(output));
    /* prediction 0: row = [cx=50, cy=50, w=40, h=40, score=0.9] */
    output[0 * 5 + 0] = 50.0f;
    output[0 * 5 + 1] = 50.0f;
    output[0 * 5 + 2] = 40.0f;
    output[0 * 5 + 3] = 40.0f;
    output[0 * 5 + 4] = 0.9f;
    ASSERT_INT_EQ(detection_list_init(&list, 8), 0);
    ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, 0.25f, 0.45f, &list, 8), 0);
    EXPECT_INT_EQ((int)list.count, 1);
    EXPECT_FLOAT_NEAR(list.items[0].score, 0.9f, 0.001f);
    detection_list_destroy(&list);
}

static void test_decode_embedded_nms(void) {
    /* [1, 2, 6]: shape[2]==6 → embedded_nms. class≠0인 row는 필터링됩니다. */
    const float output[] = {
        10.0f, 10.0f, 60.0f, 60.0f, 0.85f, 0.0f,  /* person(class=0) */
        10.0f, 10.0f, 60.0f, 60.0f, 0.70f, 1.0f   /* non-person(class=1) */
    };
    int64_t shape[] = {1, 2, 6};
    Letterbox t = {100, 100, 100, 100, 1.0f, 0, 0};
    DetectionList list;
    ASSERT_INT_EQ(detection_list_init(&list, 8), 0);
    ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, 0.25f, 0.45f, &list, 8), 0);
    EXPECT_INT_EQ((int)list.count, 1);
    EXPECT_FLOAT_NEAR(list.items[0].score, 0.85f, 0.001f);
    detection_list_destroy(&list);
}

static void test_decode_invalid_args(void) {
    float output[5 * 7] = {0};
    int64_t shape_ok[]         = {1, 5, 7};
    int64_t shape_bad_batch[]  = {2, 5, 7};
    int64_t shape_bad_format[] = {1, 3, 4};  /* embedded_nms/channel_first/last 모두 아님 */
    Letterbox t = {100, 100, 100, 100, 1.0f, 0, 0};
    DetectionList list;
    ASSERT_INT_EQ(detection_list_init(&list, 8), 0);
    EXPECT_INT_EQ(yolo11_decode(NULL,   shape_ok, 3, &t, 0.25f, 0.45f, &list, 8), -1);
    EXPECT_INT_EQ(yolo11_decode(output, NULL,     3, &t, 0.25f, 0.45f, &list, 8), -1);
    EXPECT_INT_EQ(yolo11_decode(output, shape_ok, 2, &t, 0.25f, 0.45f, &list, 8), -1);
    EXPECT_INT_EQ(yolo11_decode(output, shape_bad_batch,  3, &t, 0.25f, 0.45f, &list, 8), -1);
    EXPECT_INT_EQ(yolo11_decode(output, shape_bad_format, 3, &t, 0.25f, 0.45f, &list, 8), -1);
    detection_list_destroy(&list);
}

static void test_nms_two_overlapping_boxes(void) {
    /* 동일 위치 두 박스 → IoU=1.0 → NMS 후 고점수(0.90)만 생존합니다. */
    float output[5 * 7];
    int64_t shape[] = {1, 5, 7};
    Letterbox t = {100, 100, 100, 100, 1.0f, 0, 0};
    DetectionList list;
    memset(output, 0, sizeof(output));
    output[0 * 7 + 0] = 50.0f; output[1 * 7 + 0] = 50.0f;
    output[2 * 7 + 0] = 40.0f; output[3 * 7 + 0] = 40.0f; output[4 * 7 + 0] = 0.90f;
    output[0 * 7 + 1] = 50.0f; output[1 * 7 + 1] = 50.0f;
    output[2 * 7 + 1] = 40.0f; output[3 * 7 + 1] = 40.0f; output[4 * 7 + 1] = 0.65f;
    ASSERT_INT_EQ(detection_list_init(&list, 8), 0);
    ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, 0.25f, 0.45f, &list, 8), 0);
    EXPECT_INT_EQ((int)list.count, 1);
    EXPECT_FLOAT_NEAR(list.items[0].score, 0.90f, 0.001f);
    detection_list_destroy(&list);
}

static void test_nms_two_nonoverlapping_boxes(void) {
    /* 완전히 떨어진 두 박스 → IoU=0 → NMS 억제 없음 → count=2 */
    float output[5 * 7];
    int64_t shape[] = {1, 5, 7};
    Letterbox t = {100, 100, 100, 100, 1.0f, 0, 0};
    DetectionList list;
    memset(output, 0, sizeof(output));
    /* box 0: 좌상단 근처 */
    output[0 * 7 + 0] = 10.0f; output[1 * 7 + 0] = 10.0f;
    output[2 * 7 + 0] = 10.0f; output[3 * 7 + 0] = 10.0f; output[4 * 7 + 0] = 0.80f;
    /* box 1: 우하단 근처 */
    output[0 * 7 + 1] = 90.0f; output[1 * 7 + 1] = 90.0f;
    output[2 * 7 + 1] = 10.0f; output[3 * 7 + 1] = 10.0f; output[4 * 7 + 1] = 0.70f;
    ASSERT_INT_EQ(detection_list_init(&list, 8), 0);
    ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, 0.25f, 0.45f, &list, 8), 0);
    EXPECT_INT_EQ((int)list.count, 2);
    detection_list_destroy(&list);
}

static void test_draw_partial_box(void) {
    enum { WIDTH = 24, HEIGHT = 24, GUARD = 32 };
    uint8_t *memory = (uint8_t *)malloc(GUARD + WIDTH * HEIGHT * 3 + GUARD);
    DetectionList list;
    ASSERT_TRUE(memory != NULL);
    memset(memory, 0xa5, GUARD + WIDTH * HEIGHT * 3 + GUARD);
    ASSERT_INT_EQ(detection_list_init(&list, 1), 0);
    list.count = 1;
    /* 박스 좌상단이 이미지 밖 → set_pixel이 범위를 확인해야 합니다 */
    list.items[0] = (Detection){-10.0f, -10.0f, 15.0f, 15.0f, 0.75f};
    draw_detections(memory + GUARD, WIDTH, HEIGHT, WIDTH * 3, &list);
    for (int i = 0; i < GUARD; ++i) {
        EXPECT_TRUE(memory[i] == 0xa5);
        EXPECT_TRUE(memory[GUARD + WIDTH * HEIGHT * 3 + i] == 0xa5);
    }
    detection_list_destroy(&list);
    free(memory);
}

static void test_draw_empty_detections(void) {
    enum { WIDTH = 8, HEIGHT = 8 };
    uint8_t buffer[WIDTH * HEIGHT * 3];
    DetectionList list;
    int changed = 0;
    memset(buffer, 0x7f, sizeof(buffer));
    ASSERT_INT_EQ(detection_list_init(&list, 1), 0);
    list.count = 0;
    draw_detections(buffer, WIDTH, HEIGHT, WIDTH * 3, &list);
    for (size_t i = 0; i < sizeof(buffer); ++i)
        if (buffer[i] != 0x7f) changed++;
    EXPECT_INT_EQ(changed, 0);
    detection_list_destroy(&list);
}

static void test_tracker_invalid_create(void) {
    TrackerOptions bad;
    EXPECT_TRUE(tracker_create(NULL) == NULL);
    bad = (TrackerOptions){0, 3, 2, 24}; EXPECT_TRUE(tracker_create(&bad) == NULL);
    bad = (TrackerOptions){4, 0, 2, 24}; EXPECT_TRUE(tracker_create(&bad) == NULL);
    bad = (TrackerOptions){4, 3, 0, 24}; EXPECT_TRUE(tracker_create(&bad) == NULL);
    bad = (TrackerOptions){4, 3, 2,  0}; EXPECT_TRUE(tracker_create(&bad) == NULL);
}

static void test_tracker_null_args(void) {
    TrackerOptions options = {4, 3, 2, 24};
    LightTracker *tracker = tracker_create(&options);
    uint8_t frame[128 * 96 * 3];
    DetectionList list;
    char error[64] = {0};
    int req = 0;
    ASSERT_TRUE(tracker != NULL);
    ASSERT_INT_EQ(detection_list_init(&list, 4), 0);
    memset(frame, 0, sizeof(frame));
    EXPECT_INT_EQ(tracker_reset(NULL,    frame, 128, 96, 384, error, sizeof(error)), -1);
    EXPECT_INT_EQ(tracker_reset(tracker, NULL,  128, 96, 384, error, sizeof(error)), -1);
    EXPECT_INT_EQ(tracker_update(NULL, frame, 128, 96, 384,
                                 &list, &req, error, sizeof(error)), -1);
    detection_list_destroy(&list);
    tracker_destroy(tracker);
}

static void test_tracker_stationary(void) {
    /* 동일 프레임을 두 번 넘기면 픽셀 차이=0이므로 재추론이 불필요합니다. */
    enum { WIDTH = 128, HEIGHT = 96 };
    uint8_t frame[WIDTH * HEIGHT * 3];
    DetectionList detections;
    TrackerOptions options = {4, 3, 2, 24};
    LightTracker *tracker = tracker_create(&options);
    char error[128] = {0};
    int request_detection = 0;
    ASSERT_TRUE(tracker != NULL);
    ASSERT_INT_EQ(detection_list_init(&detections, 4), 0);
    memset(frame, 64, sizeof(frame));
    detections.count = 0;
    ASSERT_INT_EQ(tracker_reset(tracker, frame, WIDTH, HEIGHT, WIDTH * 3,
                                error, sizeof(error)), 0);
    ASSERT_INT_EQ(tracker_update(tracker, frame, WIDTH, HEIGHT, WIDTH * 3,
                                 &detections, &request_detection,
                                 error, sizeof(error)), 0);
    EXPECT_INT_EQ(request_detection, 0);
    detection_list_destroy(&detections);
    tracker_destroy(tracker);
}

static void test_platform_timer_advances(void) {
    double t1 = platform_monotonic_seconds();
    double t2 = platform_monotonic_seconds();
    EXPECT_TRUE(t2 >= t1);
    EXPECT_TRUE(t1 > 0.0);
}

static void test_platform_cpu_count(void) {
    EXPECT_TRUE(platform_cpu_count() >= 1u);
}

/* ── pose 모델 지원 테스트 ─────────────────────────────────────────────── */

/* [1,56,N] 채널 우선 형식(YOLO11n-pose 기본 출력).
 * N > C=56 이어야 shape dispatch 가 channel_first 로 분기됩니다. */
static void test_decode_pose_channel_first(void) {
    enum { N = 100, C = 56 };
    float output[C * N];
    int64_t shape[3] = {1, C, N};
    Letterbox t = {416, 416, 416, 416, 1.0f, 0, 0};
    DetectionList list;
    memset(output, 0, sizeof(output));
    ASSERT_INT_EQ(detection_list_init(&list, 32), 0);

    /* 후보 0번: cx=200, cy=300, w=100, h=150, score=0.9 */
    output[0 * N + 0] = 200.0f;
    output[1 * N + 0] = 300.0f;
    output[2 * N + 0] = 100.0f;
    output[3 * N + 0] = 150.0f;
    output[4 * N + 0] = 0.9f;
    /* keypoint 0 (코): x=200, y=260, score=0.8 */
    output[(5 + 0) * N + 0] = 200.0f;
    output[(5 + 1) * N + 0] = 260.0f;
    output[(5 + 2) * N + 0] = 0.8f;

    ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, 0.5f, 0.45f, &list, 32), 0);
    ASSERT_INT_EQ((int)list.count, 1);
    EXPECT_INT_EQ(list.items[0].keypoint_count, YOLO11_NUM_KEYPOINTS);
    EXPECT_FLOAT_NEAR(list.items[0].kp[0].x, 200.0f, 1.0f);
    EXPECT_FLOAT_NEAR(list.items[0].kp[0].y, 260.0f, 1.0f);
    EXPECT_FLOAT_NEAR(list.items[0].kp[0].score, 0.8f, 0.01f);
    detection_list_destroy(&list);
}

/* [1,N,56] 후보 우선 형식. N > C=56 이어야 channel_last 로 분기됩니다. */
static void test_decode_pose_channel_last(void) {
    enum { N = 100, C = 56 };
    float output[N * C];
    int64_t shape[3] = {1, N, C};
    Letterbox t = {416, 416, 416, 416, 1.0f, 0, 0};
    DetectionList list;
    memset(output, 0, sizeof(output));
    ASSERT_INT_EQ(detection_list_init(&list, 32), 0);

    /* 후보 0번 */
    output[0 * C + 0] = 200.0f; /* cx */
    output[0 * C + 1] = 300.0f; /* cy */
    output[0 * C + 2] = 100.0f; /* w  */
    output[0 * C + 3] = 150.0f; /* h  */
    output[0 * C + 4] = 0.9f;   /* score */
    output[0 * C + 5] = 195.0f; /* kp0.x */
    output[0 * C + 6] = 258.0f; /* kp0.y */
    output[0 * C + 7] = 0.75f;  /* kp0.score */

    ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, 0.5f, 0.45f, &list, 32), 0);
    ASSERT_INT_EQ((int)list.count, 1);
    EXPECT_INT_EQ(list.items[0].keypoint_count, YOLO11_NUM_KEYPOINTS);
    EXPECT_FLOAT_NEAR(list.items[0].kp[0].x, 195.0f, 1.0f);
    detection_list_destroy(&list);
}

/* detection 전용 모델([1,84,N])은 keypoint_count == 0 이어야 합니다.
 * N > C=84 이어야 channel_first 로 분기됩니다. */
static void test_decode_detection_no_keypoints(void) {
    enum { N = 200, C = 84 };
    float output[C * N];
    int64_t shape[3] = {1, C, N};
    Letterbox t = {416, 416, 416, 416, 1.0f, 0, 0};
    DetectionList list;
    memset(output, 0, sizeof(output));
    ASSERT_INT_EQ(detection_list_init(&list, 32), 0);

    output[0 * N + 0] = 200.0f;
    output[1 * N + 0] = 300.0f;
    output[2 * N + 0] = 100.0f;
    output[3 * N + 0] = 150.0f;
    output[4 * N + 0] = 0.9f;

    ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, 0.5f, 0.45f, &list, 32), 0);
    ASSERT_INT_EQ((int)list.count, 1);
    EXPECT_INT_EQ(list.items[0].keypoint_count, 0);
    detection_list_destroy(&list);
}

/* 화면 밖 관절은 경계로 clamp 되지 않아야 합니다.
 * N > C=56 이어야 channel_first 로 분기됩니다. */
static void test_map_point_no_clamp(void) {
    enum { N = 100, C = 56 };
    float output[C * N];
    int64_t shape[3] = {1, C, N};
    /* 원본 이미지가 416x416, 패딩 없음, scale=1 */
    Letterbox t = {416, 416, 416, 416, 1.0f, 0, 0};
    DetectionList list;
    memset(output, 0, sizeof(output));
    ASSERT_INT_EQ(detection_list_init(&list, 32), 0);

    output[0 * N + 0] = 200.0f;
    output[1 * N + 0] = 300.0f;
    output[2 * N + 0] = 100.0f;
    output[3 * N + 0] = 150.0f;
    output[4 * N + 0] = 0.9f;
    /* kp0: x=-20 (화면 왼쪽 밖), score 0.6 */
    output[5 * N + 0] = -20.0f;
    output[6 * N + 0] = 200.0f;
    output[7 * N + 0] = 0.6f;

    ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, 0.5f, 0.45f, &list, 32), 0);
    ASSERT_INT_EQ((int)list.count, 1);
    /* clamp 됐다면 0.0, clamp 안 됐다면 -20.0 */
    EXPECT_FLOAT_NEAR(list.items[0].kp[0].x, -20.0f, 0.5f);
    detection_list_destroy(&list);
}

/* 추적 프레임에서 keypoint 가 박스와 같은 델타로 이동해야 합니다 */
static void test_tracker_translates_keypoints(void) {
    enum { WIDTH = 128, HEIGHT = 96 };
    uint8_t previous[WIDTH * HEIGHT * 3];
    uint8_t current[WIDTH * HEIGHT * 3];
    DetectionList detections;
    TrackerOptions options = {4, 3, 2, 24};
    LightTracker *tracker = tracker_create(&options);
    char error[128] = {0};
    int request_detection = 0;
    float kp_x_before;
    float box_x_before;
    float kp_x_after;
    float box_x_after;

    ASSERT_TRUE(tracker != NULL);
    ASSERT_INT_EQ(detection_list_init(&detections, 4), 0);
    detections.count = 1;
    detections.items[0].x1 = 32.0f;
    detections.items[0].y1 = 24.0f;
    detections.items[0].x2 = 79.0f;
    detections.items[0].y2 = 71.0f;
    detections.items[0].score = 0.9f;
    detections.items[0].keypoint_count = YOLO11_NUM_KEYPOINTS;
    /* 코 관절을 박스 중앙에 놓습니다 */
    detections.items[0].kp[0].x = 55.0f;
    detections.items[0].kp[0].y = 40.0f;
    detections.items[0].kp[0].score = 0.9f;

    make_tracking_frame(previous, WIDTH, HEIGHT, 0);
    make_tracking_frame(current, WIDTH, HEIGHT, 4);  /* 4픽셀 오른쪽으로 이동 */

    ASSERT_INT_EQ(tracker_reset(tracker, previous, WIDTH, HEIGHT, WIDTH * 3,
                                error, sizeof(error)), 0);
    kp_x_before = detections.items[0].kp[0].x;
    box_x_before = detections.items[0].x1;

    ASSERT_INT_EQ(tracker_update(tracker, current, WIDTH, HEIGHT, WIDTH * 3,
                                 &detections, &request_detection,
                                 error, sizeof(error)), 0);

    kp_x_after = detections.items[0].kp[0].x;
    box_x_after = detections.items[0].x1;

    /* 관절이 박스와 같은 방향·크기로 이동했는지 확인 */
    EXPECT_FLOAT_NEAR(kp_x_after - kp_x_before,
                      box_x_after - box_x_before, 0.5f);

    detection_list_destroy(&detections);
    tracker_destroy(tracker);
}

/* ── Config 파싱 테스트 ──────────────────────────────────────────────────── */

static void test_config_parse_basic(void) {
    Config cfg;
    const char *v = NULL;
    char error[128] = {0};
    const char *tmp = "test_cfg_basic.ini";
    FILE *f = fopen(tmp, "w");
    ASSERT_TRUE(f != NULL);
    fputs("dwell_limit_seconds = 7200\n"
          "# comment line is ignored\n"
          "motion_ratio_threshold = 0.01\n", f);
    fclose(f);
    ASSERT_INT_EQ(config_load(&cfg, tmp, error, sizeof(error)), 0);
    EXPECT_INT_EQ((int)config_long(&cfg, "dwell_limit_seconds", 0, 0, 86400), 7200);
    EXPECT_FLOAT_NEAR(config_float(&cfg, "motion_ratio_threshold", 0.0f, 0.0f, 1.0f), 0.01f, 0.0001f);
    /* 없는 키는 기본값 반환 */
    EXPECT_INT_EQ((int)config_long(&cfg, "missing", 999, 0, 86400), 999);
    EXPECT_INT_EQ(config_get(&cfg, "dwell_limit_seconds", &v), 0);
    EXPECT_TRUE(v != NULL);
    EXPECT_INT_EQ(config_get(&cfg, "missing", &v), -1);
    config_destroy(&cfg);
    remove(tmp);
}

static void test_config_defaults(void) {
    Config cfg;
    char error[128] = {0};
    /* path==NULL → 빈 Config, 모든 키가 기본값을 반환해야 합니다. */
    ASSERT_INT_EQ(config_load(&cfg, NULL, error, sizeof(error)), 0);
    EXPECT_INT_EQ((int)config_long(&cfg, "any_key", 42, 0, 9999), 42);
    EXPECT_FLOAT_NEAR(config_float(&cfg, "any_key", 3.14f, 0.0f, 100.0f), 3.14f, 0.001f);
    config_destroy(&cfg);
}

static void test_config_invalid_value(void) {
    Config cfg;
    char error[128] = {0};
    const char *tmp = "test_cfg_invalid.ini";
    FILE *f = fopen(tmp, "w");
    ASSERT_TRUE(f != NULL);
    fputs("bad_int = not_a_number\n"
          "out_of_range = 99999\n", f);
    fclose(f);
    ASSERT_INT_EQ(config_load(&cfg, tmp, error, sizeof(error)), 0);
    /* 숫자가 아닌 값 → 기본값 반환 */
    EXPECT_INT_EQ((int)config_long(&cfg, "bad_int", 5, 0, 100), 5);
    /* 범위 초과 → 기본값 반환 */
    EXPECT_INT_EQ((int)config_long(&cfg, "out_of_range", 7, 0, 100), 7);
    config_destroy(&cfg);
    remove(tmp);
}

/* ── TrackList 테스트 ────────────────────────────────────────────────────── */

static void test_tracks_id_stability(void) {
    TrackList tl;
    DetectionList det;
    char error[128] = {0};
    int first_id;
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, error, sizeof(error)), 0);
    ASSERT_INT_EQ(detection_list_init(&det, 4), 0);

    det.count = 1;
    det.items[0] = (Detection){10, 10, 50, 50, 0.9f};
    tracks_update(&tl, &det, 100.0);
    ASSERT_INT_EQ((int)tl.count, 1);
    EXPECT_TRUE(tl.items[0].active);
    first_id = tl.items[0].id;

    /* 거의 같은 위치 → 동일 트랙 ID */
    det.items[0] = (Detection){11, 11, 51, 51, 0.88f};
    tracks_update(&tl, &det, 101.0);
    EXPECT_INT_EQ((int)tl.count, 1);
    EXPECT_INT_EQ(tl.items[0].id, first_id);
    EXPECT_TRUE(tl.items[0].dwell_seconds > 0.0);

    detection_list_destroy(&det);
    tracks_destroy(&tl);
}

static void test_tracks_eviction(void) {
    /* max_misses=2이면 빈 detection 3회 후 inactive가 되어야 합니다. */
    TrackList tl;
    DetectionList det;
    char error[128] = {0};
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 2, error, sizeof(error)), 0);
    ASSERT_INT_EQ(detection_list_init(&det, 4), 0);

    det.count = 1;
    det.items[0] = (Detection){10, 10, 50, 50, 0.9f};
    tracks_update(&tl, &det, 100.0);
    EXPECT_TRUE(tl.items[0].active);

    det.count = 0;
    tracks_update(&tl, &det, 101.0);  /* miss=1 */
    EXPECT_TRUE(tl.items[0].active);
    tracks_update(&tl, &det, 102.0);  /* miss=2, 아직 살아있음 */
    EXPECT_TRUE(tl.items[0].active);
    tracks_update(&tl, &det, 103.0);  /* miss=3 > max_misses=2 → inactive */
    EXPECT_TRUE(!tl.items[0].active);

    detection_list_destroy(&det);
    tracks_destroy(&tl);
}

/* ── RulesEngine 테스트 ──────────────────────────────────────────────────── */

static void test_rules_overstay_latches_once(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig rcfg = {60.0, 300.0, 5.0, 0, 0, 0, 0, 0};
    char error[128] = {0};
    FILE *f = tmpfile();
    ASSERT_TRUE(f != NULL);
    event_log_init(&elog, f, LOG_INFO);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);

    tl.count = 1;
    tl.items[0].id = 1;
    tl.items[0].active = 1;
    tl.items[0].dwell_seconds = 120.0;  /* limit=60 초과 */
    tl.items[0].order = TRACK_UNORDERED;

    rules_evaluate(&re, &tl, 100.0, &elog);   /* latch 설정 */
    rules_evaluate(&re, &tl, 101.0, &elog);   /* latch 유지, 재발화 없음 */

    /* dwell이 한계 아래로 내려가면 latch 해제 */
    tl.items[0].dwell_seconds = 30.0;
    rules_evaluate(&re, &tl, 102.0, &elog);

    /* 다시 초과하면 재발화 가능 */
    tl.items[0].dwell_seconds = 120.0;
    rules_evaluate(&re, &tl, 103.0, &elog);

    rules_destroy(&re);
    tracks_destroy(&tl);
    fclose(f);
}

static void test_rules_fall_geometry(void) {
    /* 수평 bbox(keypoint 없음) + fall_hold 충족 → person_fallen 이벤트 */
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig rcfg = {3600.0, 300.0, 0.1, 0, 0, 0, 0, 0}; /* fall_hold=0.1초 */
    char error[128] = {0};
    FILE *f = tmpfile();
    ASSERT_TRUE(f != NULL);
    event_log_init(&elog, f, LOG_INFO);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);

    tl.count = 1;
    tl.items[0].id = 1;
    tl.items[0].active = 1;
    tl.items[0].order = TRACK_ORDERED;
    /* 수평 bbox: w=200, h=60, 200 > 60*1.2=72 ✓ */
    tl.items[0].box = (Detection){0, 70, 200, 130, 0.9f};
    tl.items[0].box.keypoint_count = 0;  /* bbox 비율만으로 판정 */

    /* 첫 평가: fall_start 기록 */
    rules_evaluate(&re, &tl, 100.0, &elog);
    /* 0.2초 뒤: fall_hold(0.1s) 충족 → person_fallen */
    rules_evaluate(&re, &tl, 100.2, &elog);

    rules_destroy(&re);
    tracks_destroy(&tl);
    fclose(f);
}

static void test_rules_fall_requires_hold(void) {
    /* fall_hold=5초이므로 3초 후 자세가 풀리면 발화하면 안 됩니다. */
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig rcfg = {3600.0, 300.0, 5.0, 0, 0, 0, 0, 0};
    char error[128] = {0};
    FILE *f = tmpfile();
    ASSERT_TRUE(f != NULL);
    event_log_init(&elog, f, LOG_INFO);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);

    tl.count = 1;
    tl.items[0].id = 1;
    tl.items[0].active = 1;
    tl.items[0].box = (Detection){0, 70, 200, 130, 0.9f};  /* 수평 */
    tl.items[0].box.keypoint_count = 0;

    rules_evaluate(&re, &tl, 100.0, &elog);  /* fall_start = 100 */
    rules_evaluate(&re, &tl, 103.0, &elog);  /* 3s < 5s, 미발화 */

    /* 자세가 수직으로 바뀌면 fall_start 리셋 */
    tl.items[0].box = (Detection){0, 50, 80, 200, 0.9f};  /* 수직 */
    rules_evaluate(&re, &tl, 103.5, &elog);

    /* 다시 수평: 새 타이머 시작 */
    tl.items[0].box = (Detection){0, 70, 200, 130, 0.9f};
    rules_evaluate(&re, &tl, 104.0, &elog);  /* fall_start = 104 */
    rules_evaluate(&re, &tl, 107.0, &elog);  /* 3s < 5s, 여전히 미발화 */

    rules_destroy(&re);
    tracks_destroy(&tl);
    fclose(f);
}

static void test_rules_unordered_seated(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig rcfg = {3600.0, 30.0, 5.0, 0, 0, 0, 0, 0}; /* grace=30초 */
    char error[128] = {0};
    FILE *f = tmpfile();
    ASSERT_TRUE(f != NULL);
    event_log_init(&elog, f, LOG_INFO);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);

    tl.count = 1;
    tl.items[0].id = 1;
    tl.items[0].active = 1;
    tl.items[0].dwell_seconds = 31.0;  /* grace 초과 */
    tl.items[0].order = TRACK_UNORDERED;
    tl.items[0].box = (Detection){0, 0, 30, 100, 0.9f};  /* 수직 */

    rules_evaluate(&re, &tl, 100.0, &elog);  /* unordered_seated 발화 */

    /* ORDERED 전환 후 latch 해제 */
    tl.items[0].order = TRACK_ORDERED;
    rules_evaluate(&re, &tl, 101.0, &elog);  /* latch 해제 */

    rules_destroy(&re);
    tracks_destroy(&tl);
    fclose(f);
}

/* ── CameraHealth 테스트 ─────────────────────────────────────────────────── */

static void test_camera_health_whiteout(void) {
    /* 흰 화면 8프레임 연속 → CAM_WHITEOUT (기본 anomaly_hold=5) */
    enum { SRC_W = 8, SRC_H = 8, DS = 4 };
    CameraHealth health;
    GrayBuf g;
    uint8_t rgb[SRC_W * SRC_H * 3];
    CamState state = CAM_OK;
    char error[128] = {0};
    int i;
    ASSERT_INT_EQ(gray_buf_init(&g, SRC_W, SRC_H, DS), 0);
    ASSERT_INT_EQ(camera_health_init(&health, g.width, g.height, NULL,
                                     error, sizeof(error)), 0);
    memset(rgb, 255, sizeof(rgb));  /* 완전 흰색 */
    for (i = 0; i < 8; ++i) {
        gray_buf_update(&g, rgb, SRC_W, SRC_H, SRC_W * 3);
        camera_health_update(&health, &g, &state);
    }
    EXPECT_INT_EQ((int)state, (int)CAM_WHITEOUT);
    gray_buf_destroy(&g);
    camera_health_destroy(&health);
}

static void test_camera_health_frozen(void) {
    /* 정적 장면(frozen_threshold=3, anomaly_hold=2) → CAM_FROZEN */
    enum { SRC_W = 8, SRC_H = 8, DS = 4 };
    CameraHealth health;
    GrayBuf g;
    uint8_t rgb[SRC_W * SRC_H * 3];
    CamState state = CAM_OK;
    /* 낮은 임계값으로 빠르게 frozen 감지 */
    CameraHealthConfig cfg = {240, 12, 3, 2, 8};
    char error[128] = {0};
    int i;
    ASSERT_INT_EQ(gray_buf_init(&g, SRC_W, SRC_H, DS), 0);
    ASSERT_INT_EQ(camera_health_init(&health, g.width, g.height, &cfg,
                                     error, sizeof(error)), 0);
    memset(rgb, 128, sizeof(rgb));  /* 중간 밝기 정적 프레임 */
    for (i = 0; i < 10; ++i) {
        gray_buf_update(&g, rgb, SRC_W, SRC_H, SRC_W * 3);
        camera_health_update(&health, &g, &state);
    }
    EXPECT_INT_EQ((int)state, (int)CAM_FROZEN);
    gray_buf_destroy(&g);
    camera_health_destroy(&health);
}

/* ── 모션 게이트 / GrayBuf 테스트 ───────────────────────────────────────── */

static void test_motion_gate_static_scene(void) {
    /* 동일 장면 연속 입력 시 픽셀 변화 비율이 0이어야 합니다. */
    enum { SRC_W = 16, SRC_H = 16, DS = 4 };
    GrayBuf g;
    uint8_t prev[4 * 4];  /* (16/4)*(16/4) */
    uint8_t rgb[SRC_W * SRC_H * 3];
    size_t k, changed = 0;
    size_t total;
    double ratio;
    ASSERT_INT_EQ(gray_buf_init(&g, SRC_W, SRC_H, DS), 0);
    memset(rgb, 100, sizeof(rgb));
    gray_buf_update(&g, rgb, SRC_W, SRC_H, SRC_W * 3);
    memcpy(prev, g.data, (size_t)g.width * g.height);
    /* 동일 프레임 재입력 */
    gray_buf_update(&g, rgb, SRC_W, SRC_H, SRC_W * 3);
    total = (size_t)g.width * g.height;
    for (k = 0; k < total; ++k) {
        int d = (int)g.data[k] - (int)prev[k];
        if (d < -8 || d > 8) changed++;
    }
    ratio = (double)changed / (double)total;
    EXPECT_TRUE(ratio < 0.004);  /* 완전 정적 → ratio=0.0 */
    gray_buf_destroy(&g);
}

static void test_door_state_debounce(void) {
    DoorMonitor d;
    uint8_t closed[3] = {0, 0, 0};
    uint8_t open[3] = {255, 255, 255};
    uint8_t frame[3] = {0, 0, 0};
    int changed = 0;
    memset(&d, 0, sizeof(d));
    d.enabled = 1;
    d.ref_closed_rgb = (uint8_t *)malloc(sizeof(closed));
    d.ref_open_rgb = (uint8_t *)malloc(sizeof(open));
    ASSERT_TRUE(d.ref_closed_rgb != NULL && d.ref_open_rgb != NULL);
    memcpy(d.ref_closed_rgb, closed, sizeof(closed));
    memcpy(d.ref_open_rgb, open, sizeof(open));
    d.ref_closed_w = d.ref_open_w = 1;
    d.ref_closed_h = d.ref_open_h = 1;
    d.confirm_frames = 3;
    d.last_state = -1;

    EXPECT_INT_EQ(door_check(&d, frame, 1, 1, 3, &changed), 0);
    memset(frame, 255, sizeof(frame));
    EXPECT_INT_EQ(door_check(&d, frame, 1, 1, 3, &changed), 0);
    EXPECT_INT_EQ(door_check(&d, frame, 1, 1, 3, &changed), 0);
    EXPECT_INT_EQ(door_check(&d, frame, 1, 1, 3, &changed), 1);
    EXPECT_INT_EQ(changed, 1);
    door_destroy(&d);
}

static void test_gray_matches_reference(void) {
    /* BT.601 근사: (77R + 150G + 29B + 128) >> 8 결과를 검증합니다. */
    enum { SRC_W = 4, SRC_H = 4, DS = 1 };
    GrayBuf g;
    uint8_t rgb[SRC_W * SRC_H * 3];
    int i;
    ASSERT_INT_EQ(gray_buf_init(&g, SRC_W, SRC_H, DS), 0);

    /* 순수 빨강(255,0,0): (77*255+128)>>8 = 19763>>8 = 77 */
    for (i = 0; i < SRC_W * SRC_H; ++i) {
        rgb[i * 3 + 0] = 255; rgb[i * 3 + 1] = 0; rgb[i * 3 + 2] = 0;
    }
    gray_buf_update(&g, rgb, SRC_W, SRC_H, SRC_W * 3);
    EXPECT_INT_EQ((int)g.data[0], 77);

    /* 순수 초록(0,255,0): (150*255+128)>>8 = 38378>>8 = 149 */
    for (i = 0; i < SRC_W * SRC_H; ++i) {
        rgb[i * 3 + 0] = 0; rgb[i * 3 + 1] = 255; rgb[i * 3 + 2] = 0;
    }
    gray_buf_update(&g, rgb, SRC_W, SRC_H, SRC_W * 3);
    EXPECT_INT_EQ((int)g.data[0], 149);

    /* 순수 흰색(255,255,255): (256*255+128)>>8 = 65408>>8 = 255 */
    memset(rgb, 255, sizeof(rgb));
    gray_buf_update(&g, rgb, SRC_W, SRC_H, SRC_W * 3);
    EXPECT_INT_EQ((int)g.data[0], 255);

    gray_buf_destroy(&g);
}

int main(void) {
    TEST_SUITE_BEGIN(core_unit_tests);
    RUN_TEST(test_letterbox);
    RUN_TEST(test_fast_letterbox_matches_reference);
    RUN_TEST(test_decode_and_nms);
    RUN_TEST(test_draw_bounds);
    RUN_TEST(test_light_tracker_translation);
    RUN_TEST(test_detection_list_lifecycle);
    RUN_TEST(test_letterbox_wide_image);
    RUN_TEST(test_letterbox_tall_image);
    RUN_TEST(test_letterbox_invalid_args);
    RUN_TEST(test_decode_channel_first);
    RUN_TEST(test_decode_channel_last);
    RUN_TEST(test_decode_embedded_nms);
    RUN_TEST(test_decode_invalid_args);
    RUN_TEST(test_nms_two_overlapping_boxes);
    RUN_TEST(test_nms_two_nonoverlapping_boxes);
    RUN_TEST(test_draw_partial_box);
    RUN_TEST(test_draw_empty_detections);
    RUN_TEST(test_tracker_invalid_create);
    RUN_TEST(test_tracker_null_args);
    RUN_TEST(test_tracker_stationary);
    RUN_TEST(test_platform_timer_advances);
    RUN_TEST(test_platform_cpu_count);
    RUN_TEST(test_decode_pose_channel_first);
    RUN_TEST(test_decode_pose_channel_last);
    RUN_TEST(test_decode_detection_no_keypoints);
    RUN_TEST(test_map_point_no_clamp);
    RUN_TEST(test_tracker_translates_keypoints);
    RUN_TEST(test_config_parse_basic);
    RUN_TEST(test_config_defaults);
    RUN_TEST(test_config_invalid_value);
    RUN_TEST(test_tracks_id_stability);
    RUN_TEST(test_tracks_eviction);
    RUN_TEST(test_rules_overstay_latches_once);
    RUN_TEST(test_rules_fall_geometry);
    RUN_TEST(test_rules_fall_requires_hold);
    RUN_TEST(test_rules_unordered_seated);
    RUN_TEST(test_camera_health_whiteout);
    RUN_TEST(test_camera_health_frozen);
    RUN_TEST(test_motion_gate_static_scene);
    RUN_TEST(test_door_state_debounce);
    RUN_TEST(test_gray_matches_reference);
    TEST_SUITE_END();
}
