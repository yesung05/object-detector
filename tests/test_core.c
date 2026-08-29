#include "test_runner.h"
#include "yolo11.h"
#include "tracker.h"
#include "platform.h"

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
    TEST_SUITE_END();
}
