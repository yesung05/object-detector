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
#include "residue.h"
#include "netaccess.h"
#include "throttle.h"
#include "restart.h"
#include "slot_monitor.h"

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
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(detection_list_init(&det, 4), 0);

    det.count = 1;
    det.items[0] = (Detection){10, 10, 50, 50, 0.9f};
    tracks_update(&tl, &det, NULL, 0, 0, 0, 100.0);
    ASSERT_INT_EQ((int)tl.count, 1);
    EXPECT_TRUE(tl.items[0].active);
    first_id = tl.items[0].id;

    /* 거의 같은 위치 → 동일 트랙 ID */
    det.items[0] = (Detection){11, 11, 51, 51, 0.88f};
    tracks_update(&tl, &det, NULL, 0, 0, 0, 101.0);
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
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 2, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(detection_list_init(&det, 4), 0);

    det.count = 1;
    det.items[0] = (Detection){10, 10, 50, 50, 0.9f};
    tracks_update(&tl, &det, NULL, 0, 0, 0, 100.0);
    EXPECT_TRUE(tl.items[0].active);

    det.count = 0;
    tracks_update(&tl, &det, NULL, 0, 0, 0, 101.0);  /* miss=1 */
    EXPECT_TRUE(tl.items[0].active);
    tracks_update(&tl, &det, NULL, 0, 0, 0, 102.0);  /* miss=2, 아직 살아있음 */
    EXPECT_TRUE(tl.items[0].active);
    tracks_update(&tl, &det, NULL, 0, 0, 0, 103.0);  /* miss=3 > max_misses=2 → inactive */
    EXPECT_TRUE(!tl.items[0].active);

    detection_list_destroy(&det);
    tracks_destroy(&tl);
}

/* ── RulesEngine 테스트 ──────────────────────────────────────────────────── */

static void test_rules_overstay_latches_once(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig rcfg = {60.0, 300.0, 5.0, 1.8f, 2.2f, 0, 0, 0, 0, 0};
    char error[128] = {0};
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
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
    event_log_close(&elog);
}

static void test_rules_fall_geometry(void) {
    /* 수평 bbox(keypoint 없음) + fall_hold 충족 → person_fallen 이벤트 */
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig rcfg = {3600.0, 300.0, 0.1, 1.8f, 2.2f, 0, 0, 0, 0, 0}; /* fall_hold=0.1초 */
    char error[128] = {0};
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
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
    event_log_close(&elog);
}

static void test_rules_fall_requires_hold(void) {
    /* fall_hold=5초이므로 3초 후 자세가 풀리면 발화하면 안 됩니다. */
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig rcfg = {3600.0, 300.0, 5.0, 1.8f, 2.2f, 0, 0, 0, 0, 0};
    char error[128] = {0};
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
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
    event_log_close(&elog);
}

static void test_rules_unordered_seated(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig rcfg = {3600.0, 30.0, 5.0, 1.8f, 2.2f, 0, 0, 0, 0, 0}; /* grace=30초 */
    char error[128] = {0};
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
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
    event_log_close(&elog);
}

/* ── CameraHealth 테스트 ─────────────────────────────────────────────────── */

/* 그레이 버퍼와 이전 프레임 사본으로 한 프레임을 진행시키는 헬퍼입니다.
 * main.c 의 흐름(analyze → health → prev 갱신)과 같은 순서를 씁니다. */
static void feed_health(CameraHealth *health, GrayBuf *g, uint8_t *prev,
                        int *ready, CamState *state) {
    GrayStats stats;
    gray_analyze(g, *ready ? prev : NULL, NULL, 8,
                 health->config.motion_threshold, 2, &stats, NULL);
    camera_health_update(health, &stats, state);
    memcpy(prev, g->data, (size_t)g->width * g->height);
    *ready = 1;
}

static void test_config_rect_and_time(void) {
    Config c;
    ConfigRect r;
    ConfigRect list[8];
    char err[128] = {0};
    int start = 0, end = 0, n;
    const char *path = "test_rect.json";
    FILE *f = fopen(path, "w");
    ASSERT_TRUE(f != NULL);
    fputs("{\n"
          "  \"roi_kiosk\": \"820,120,300,420\",\n"
          "  \"bad_rect\": \"1,2,3\",\n"
          "  \"zero_rect\": \"1,2,0,4\",\n"
          "  \"trailing\": \"1,2,3,4x\",\n"
          "  \"ignore_roi_1\": \"0,0,320,180\",\n"
          "  \"ignore_roi_2\": \"900,0,380,200\",\n"
          "  \"ignore_roi_4\": \"5,5,5,5\",\n"
          "  \"active_hours\": \"07:00-23:30\",\n"
          "  \"night\": \"22:00-02:00\",\n"
          "  \"bad_time\": \"7-23\"\n"
          "}\n", f);
    fclose(f);
    ASSERT_INT_EQ(config_load(&c, path, err, sizeof(err)), 0);

    ASSERT_INT_EQ(config_rect(&c, "roi_kiosk", &r), 1);
    EXPECT_TRUE(r.x == 820.0f && r.y == 120.0f);
    EXPECT_TRUE(r.w == 300.0f && r.h == 420.0f);

    /* 형식이 어긋나면 "미설정"으로 다뤄야 합니다 — 잘못된 값을 쓰면 안 됩니다. */
    EXPECT_INT_EQ(config_rect(&c, "bad_rect", &r), 0);
    EXPECT_INT_EQ(config_rect(&c, "zero_rect", &r), 0);
    EXPECT_INT_EQ(config_rect(&c, "trailing", &r), 0);
    EXPECT_INT_EQ(config_rect(&c, "missing", &r), 0);

    /* 번호가 끊기면(3번 없음) 거기서 멈춥니다. */
    n = config_rect_list(&c, "ignore_roi", list, 8);
    EXPECT_INT_EQ(n, 2);
    EXPECT_TRUE(list[1].x == 900.0f && list[1].w == 380.0f);

    ASSERT_INT_EQ(config_time_range(&c, "active_hours", &start, &end), 1);
    EXPECT_INT_EQ(start, 7 * 60);
    EXPECT_INT_EQ(end, 23 * 60 + 30);
    EXPECT_INT_EQ(config_time_in_range(8 * 60, start, end), 1);
    EXPECT_INT_EQ(config_time_in_range(6 * 60, start, end), 0);

    /* 자정을 넘는 구간 */
    ASSERT_INT_EQ(config_time_range(&c, "night", &start, &end), 1);
    EXPECT_INT_EQ(config_time_in_range(23 * 60, start, end), 1);
    EXPECT_INT_EQ(config_time_in_range(1 * 60, start, end), 1);
    EXPECT_INT_EQ(config_time_in_range(12 * 60, start, end), 0);

    EXPECT_INT_EQ(config_time_range(&c, "bad_time", &start, &end), 0);

    config_destroy(&c);
    remove(path);
}

static void test_camera_health_whiteout(void) {
    /* 흰 화면 8프레임 연속 → CAM_WHITEOUT (기본 anomaly_hold=5) */
    enum { SRC_W = 8, SRC_H = 8, DS = 4 };
    CameraHealth health;
    GrayBuf g;
    uint8_t rgb[SRC_W * SRC_H * 3];
    uint8_t prev[2 * 2];
    int ready = 0;
    CamState state = CAM_OK;
    char error[128] = {0};
    int i;
    ASSERT_INT_EQ(gray_buf_init(&g, SRC_W, SRC_H, DS), 0);
    ASSERT_INT_EQ(camera_health_init(&health, NULL, error, sizeof(error)), 0);
    memset(rgb, 255, sizeof(rgb));  /* 완전 흰색 */
    for (i = 0; i < 8; ++i) {
        gray_buf_update(&g, rgb, SRC_W, SRC_H, SRC_W * 3);
        feed_health(&health, &g, prev, &ready, &state);
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
    uint8_t prev[2 * 2];
    int ready = 0;
    CamState state = CAM_OK;
    /* 낮은 임계값으로 빠르게 frozen 감지 */
    CameraHealthConfig cfg = {240, 12, 3, 2, 8};
    char error[128] = {0};
    int i;
    ASSERT_INT_EQ(gray_buf_init(&g, SRC_W, SRC_H, DS), 0);
    ASSERT_INT_EQ(camera_health_init(&health, &cfg, error, sizeof(error)), 0);
    memset(rgb, 128, sizeof(rgb));  /* 중간 밝기 정적 프레임 */
    for (i = 0; i < 10; ++i) {
        gray_buf_update(&g, rgb, SRC_W, SRC_H, SRC_W * 3);
        feed_health(&health, &g, prev, &ready, &state);
    }
    EXPECT_INT_EQ((int)state, (int)CAM_FROZEN);
    gray_buf_destroy(&g);
    camera_health_destroy(&health);
}

static void test_gray_luma_matches_plane(void) {
    /* luma 경로는 Y 평면 값을 산술 없이 그대로 옮겨야 합니다. */
    enum { SRC_W = 8, SRC_H = 8, DS = 4, PAD = 5 };
    GrayBuf g;
    uint8_t luma[SRC_H * (SRC_W + PAD)];  /* stride > width 인 경우 포함 */
    int x, y;
    ASSERT_INT_EQ(gray_buf_init(&g, SRC_W, SRC_H, DS), 0);
    /* 각 픽셀에 고유 값을 넣어 좌표 매핑까지 검증합니다. */
    for (y = 0; y < SRC_H; ++y)
        for (x = 0; x < SRC_W + PAD; ++x)
            luma[y * (SRC_W + PAD) + x] = (uint8_t)(y * 16 + x);
    gray_buf_update_luma(&g, luma, SRC_W, SRC_H, SRC_W + PAD);
    /* DS=4 이므로 셀 중앙은 (2,2), (6,2), (2,6), (6,6) */
    EXPECT_INT_EQ((int)g.data[0],            2 * 16 + 2);
    EXPECT_INT_EQ((int)g.data[1],            2 * 16 + 6);
    EXPECT_INT_EQ((int)g.data[g.width + 0],  6 * 16 + 2);
    EXPECT_INT_EQ((int)g.data[g.width + 1],  6 * 16 + 6);
    gray_buf_destroy(&g);
}

static void test_gray_analyze_single_pass(void) {
    /* 두 임계값의 비교 방향이 기존 동작과 같아야 합니다.
     * 모션 게이트는 |diff| > motion_gt, 카메라 헬스는 |diff| >= health_ge. */
    enum { SRC_W = 4, SRC_H = 1, DS = 1 };
    GrayBuf g;
    uint8_t prev[SRC_W] = {100, 100, 100, 100};
    uint8_t rgb[SRC_W * SRC_H * 3];
    GrayStats st;
    int i;
    ASSERT_INT_EQ(gray_buf_init(&g, SRC_W, SRC_H, DS), 0);
    /* 회색(v,v,v) 는 luma 로 거의 v 가 됩니다. diff 를 0/8/9/50 으로 만듭니다. */
    {
        const int vals[SRC_W] = {100, 108, 109, 150};
        for (i = 0; i < SRC_W; ++i) {
            rgb[i * 3 + 0] = (uint8_t)vals[i];
            rgb[i * 3 + 1] = (uint8_t)vals[i];
            rgb[i * 3 + 2] = (uint8_t)vals[i];
        }
    }
    gray_buf_update(&g, rgb, SRC_W, SRC_H, SRC_W * 3);
    gray_analyze(&g, prev, NULL, 8, 8, 2, &st, NULL);
    EXPECT_INT_EQ(st.pixels, SRC_W);
    /* |diff| > 8  → 109, 150 두 개 */
    EXPECT_INT_EQ((int)st.changed_motion, 2);
    /* |diff| >= 8 → 108, 109, 150 세 개 */
    EXPECT_INT_EQ((int)st.changed_health, 3);
    /* prev == NULL 이면 변화량은 0, luma 합만 계산 */
    gray_analyze(&g, NULL, NULL, 8, 8, 2, &st, NULL);
    EXPECT_INT_EQ((int)st.changed_motion, 0);
    EXPECT_INT_EQ((int)st.changed_health, 0);
    EXPECT_TRUE(st.luma_sum > 0);
    gray_buf_destroy(&g);
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

static void test_motion_map_locates_change(void) {
    /* 변화 블록의 위치가 실제 변화 지점과 맞아야 하고,
     * 그 블록을 덮는 박스를 주면 "박스 밖 변화"가 0이어야 합니다. */
    enum { W = 32, H = 16, DS = 1 };
    GrayBuf g;
    uint8_t prev[W * H];
    uint8_t rgb[W * H * 3];
    GrayStats st;
    MotionMap map;
    GrayRect box;
    int i;
    ASSERT_INT_EQ(gray_buf_init(&g, W, H, DS), 0);
    memset(prev, 100, sizeof(prev));
    memset(rgb, 100, sizeof(rgb));
    /* (x=17..20, y=9..10) 영역만 크게 바꿉니다 → 블록 (2,1) */
    for (i = 0; i < 4; ++i) {
        int x = 17 + i, y = 9;
        rgb[(y * W + x) * 3 + 0] = 200;
        rgb[(y * W + x) * 3 + 1] = 200;
        rgb[(y * W + x) * 3 + 2] = 200;
    }
    gray_buf_update(&g, rgb, W, H, W * 3);
    gray_analyze(&g, prev, NULL, 8, 8, 2, &st, &map);

    EXPECT_INT_EQ(map.blocks_x, W / GRAY_BLOCK_SIZE);
    EXPECT_INT_EQ(map.blocks_y, H / GRAY_BLOCK_SIZE);
    EXPECT_INT_EQ(map.changed_blocks, 1);
    EXPECT_TRUE(motion_map_get(&map, 1 * map.blocks_x + 2) == 1);

    /* 변화 지점을 덮는 박스 → 박스 밖 변화 없음 */
    box.x1 = 16.0f; box.y1 = 8.0f; box.x2 = 24.0f; box.y2 = 16.0f;
    EXPECT_INT_EQ(gray_blocks_outside(&map, &box, 1, DS, 0), 0);
    /* 엉뚱한 곳의 박스 → 변화 블록 1개가 박스 밖 */
    box.x1 = 0.0f; box.y1 = 0.0f; box.x2 = 8.0f; box.y2 = 8.0f;
    EXPECT_INT_EQ(gray_blocks_outside(&map, &box, 1, DS, 0), 1);

    /* 변화가 전혀 없으면 블록도 0 */
    gray_buf_update(&g, rgb, W, H, W * 3);
    memcpy(prev, g.data, (size_t)g.width * g.height);
    gray_analyze(&g, prev, NULL, 8, 8, 2, &st, &map);
    EXPECT_INT_EQ(map.changed_blocks, 0);
    EXPECT_INT_EQ(gray_blocks_outside(&map, &box, 1, DS, 0), 0);

    gray_buf_destroy(&g);
}

/* 오탐 재현: 코+어깨만 유효(엉덩이 없음), w가 1.8x~2.2x 사이 → 미발화해야 함 */
static void test_rules_fall_no_hip_no_fire(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig rcfg = {3600.0, 300.0, 0.1, 1.8f, 2.2f, 0, 0, 0, 0, 0}; /* fall_hold=0.1s */
    char error[128] = {0};
    int i;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);

    tl.count = 1;
    tl.items[0].id = 1;
    tl.items[0].active = 1;
    tl.items[0].order = TRACK_ORDERED;
    /* w=190, h=100 → 190 > 100×1.8=180 (KP 비율 통과), 190 < 100×2.2=220 (NOKP 폴백 미충족) */
    tl.items[0].box = (Detection){0, 0, 190, 100, 0.9f};
    tl.items[0].box.keypoint_count = YOLO11_NUM_KEYPOINTS;
    /* 코: y=20, 양어깨: y=40 — valid=3, y-std≈9.4px, std_ratio≈0.094 ≤ 0.20 */
    tl.items[0].box.kp[0].x  = 95.0f; tl.items[0].box.kp[0].y  = 20.0f; tl.items[0].box.kp[0].score  = 0.9f;
    tl.items[0].box.kp[5].x  = 50.0f; tl.items[0].box.kp[5].y  = 40.0f; tl.items[0].box.kp[5].score  = 0.9f;
    tl.items[0].box.kp[6].x  = 140.0f; tl.items[0].box.kp[6].y = 40.0f; tl.items[0].box.kp[6].score  = 0.9f;
    /* 엉덩이: score 미달 (hip_valid=0) */
    tl.items[0].box.kp[11].score = 0.1f;
    tl.items[0].box.kp[12].score = 0.1f;

    rules_evaluate(&re, &tl, 100.0, &elog);
    rules_evaluate(&re, &tl, 100.2, &elog); /* fall_hold(0.1s) 경과 */

    /* track_id=1, capacity=16 → 슬롯 인덱스 1 */
    i = 0;
    while (i < (int)re.capacity && re.states[i].track_id != 1) i++;
    ASSERT_TRUE(i < (int)re.capacity);
    EXPECT_INT_EQ(re.states[i].fall_latched, 0); /* 오탐 발화 없어야 함 */

    rules_destroy(&re);
    tracks_destroy(&tl);
    event_log_close(&elog);
}

/* 정상 감지: 코+어깨+엉덩이 모두 수평 → 발화해야 함 */
static void test_rules_fall_with_hip_fires(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig rcfg = {3600.0, 300.0, 0.1, 1.8f, 2.2f, 0, 0, 0, 0, 0}; /* fall_hold=0.1s */
    char error[128] = {0};
    int i;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);

    tl.count = 1;
    tl.items[0].id = 1;
    tl.items[0].active = 1;
    tl.items[0].order = TRACK_ORDERED;
    /* w=250, h=100 → 250 > 100×1.8=180 ✓ */
    tl.items[0].box = (Detection){0, 0, 250, 100, 0.9f};
    tl.items[0].box.keypoint_count = YOLO11_NUM_KEYPOINTS;
    /* 코·양어깨·양엉덩이 모두 y=50 (완전 수평), valid=5, hip_valid=2, std=0 */
    tl.items[0].box.kp[0].x  = 125.0f; tl.items[0].box.kp[0].y  = 50.0f; tl.items[0].box.kp[0].score  = 0.9f;
    tl.items[0].box.kp[5].x  = 60.0f;  tl.items[0].box.kp[5].y  = 50.0f; tl.items[0].box.kp[5].score  = 0.9f;
    tl.items[0].box.kp[6].x  = 190.0f; tl.items[0].box.kp[6].y  = 50.0f; tl.items[0].box.kp[6].score  = 0.9f;
    tl.items[0].box.kp[11].x = 80.0f;  tl.items[0].box.kp[11].y = 50.0f; tl.items[0].box.kp[11].score = 0.9f;
    tl.items[0].box.kp[12].x = 170.0f; tl.items[0].box.kp[12].y = 50.0f; tl.items[0].box.kp[12].score = 0.9f;

    rules_evaluate(&re, &tl, 100.0, &elog);
    rules_evaluate(&re, &tl, 100.2, &elog); /* fall_hold(0.1s) 경과 */

    i = 0;
    while (i < (int)re.capacity && re.states[i].track_id != 1) i++;
    ASSERT_TRUE(i < (int)re.capacity);
    EXPECT_INT_EQ(re.states[i].fall_latched, 1); /* 정상 발화 */

    rules_destroy(&re);
    tracks_destroy(&tl);
    event_log_close(&elog);
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

    EXPECT_INT_EQ(door_check(&d, frame, 1, 1, 3, NULL, 0, &changed), 0);
    memset(frame, 255, sizeof(frame));
    EXPECT_INT_EQ(door_check(&d, frame, 1, 1, 3, NULL, 0, &changed), 0);
    EXPECT_INT_EQ(door_check(&d, frame, 1, 1, 3, NULL, 0, &changed), 0);
    EXPECT_INT_EQ(door_check(&d, frame, 1, 1, 3, NULL, 0, &changed), 1);
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

/* ── Tier 2 class_id 전파 테스트 ─────────────────────────────────────────── */

/* pose 모델([1,56,N])은 has_keypoints=1이므로 argmax 경로를 타지 않고
 * class_id = 0(person)이 되어야 합니다. */
static void test_decode_class_id_pose_is_zero(void) {
    enum { N = 100, C = 56 };
    float output[C * N];
    int64_t shape[3] = {1, C, N};
    Letterbox t = {416, 416, 416, 416, 1.0f, 0, 0};
    DetectionList list;
    memset(output, 0, sizeof(output));
    ASSERT_INT_EQ(detection_list_init(&list, 8), 0);
    output[0 * N + 0] = 200.0f;
    output[1 * N + 0] = 200.0f;
    output[2 * N + 0] = 80.0f;
    output[3 * N + 0] = 120.0f;
    output[4 * N + 0] = 0.9f;
    ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, 0.5f, 0.45f, &list, 8), 0);
    ASSERT_INT_EQ((int)list.count, 1);
    EXPECT_INT_EQ(list.items[0].class_id, 0);   /* pose 모델은 항상 person(0) */
    EXPECT_INT_EQ(list.items[0].keypoint_count, YOLO11_NUM_KEYPOINTS);
    detection_list_destroy(&list);
}

/* 16클래스 Tier 2 모델([1,20,N])은 has_keypoints=0, channels=20 이므로
 * 채널 4-19 argmax로 class_id가 결정됩니다.
 * 채널 7(=4+3)에 최고 점수 → class_id=3(CUP) */
static void test_decode_class_id_multiclass_argmax(void) {
    enum { N = 200, C = 20 };
    float output[C * N];
    int64_t shape[3] = {1, C, N};
    Letterbox t = {320, 320, 320, 320, 1.0f, 0, 0};
    DetectionList list;
    memset(output, 0, sizeof(output));
    ASSERT_INT_EQ(detection_list_init(&list, 8), 0);
    output[0 * N + 0] = 160.0f;
    output[1 * N + 0] = 160.0f;
    output[2 * N + 0] = 60.0f;
    output[3 * N + 0] = 60.0f;
    /* 채널 4(=OBJ_CAT): 0.3,  채널 7(=OBJ_CUP): 0.85 → 최대 */
    output[4 * N + 0] = 0.3f;   /* cat */
    output[7 * N + 0] = 0.85f;  /* cup — argmax 승자 */
    ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, 0.5f, 0.45f, &list, 8), 0);
    ASSERT_INT_EQ((int)list.count, 1);
    EXPECT_INT_EQ(list.items[0].class_id, 3);   /* OBJ_CUP = 3 */
    EXPECT_INT_EQ(list.items[0].keypoint_count, 0);
    EXPECT_FLOAT_NEAR(list.items[0].score, 0.85f, 0.001f);
    detection_list_destroy(&list);
}

/* ── rules_evaluate_objects 테스트 ──────────────────────────────────────── */

static void make_obj_list(DetectionList *list, float x1, float y1,
                           float x2, float y2, float score, int class_id) {
    list->count = 1;
    memset(&list->items[0], 0, sizeof(list->items[0]));
    list->items[0].x1 = x1;
    list->items[0].y1 = y1;
    list->items[0].x2 = x2;
    list->items[0].y2 = y2;
    list->items[0].score = score;
    list->items[0].class_id = class_id;
}

/* bottle(OBJ_BOTTLE=2) 감지 → external_drink 이벤트가 발화해야 합니다. */
static void test_rules_obj_external_drink(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    DetectionList objs;
    RulesConfig rcfg = {3600.0, 300.0, 5.0, 1.8f, 2.2f, 0, 0, 0, 0, 0, 0.15f, 1};
    char error[128] = {0};
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);
    ASSERT_INT_EQ(detection_list_init(&objs, 8), 0);

    make_obj_list(&objs, 100, 100, 150, 200, 0.8f, 2 /* OBJ_BOTTLE */);
    rules_evaluate_objects(&re, &objs, &tl, 100.0, &elog);

    /* track_id=-2 슬롯의 overstay_latched(drink latch)가 설정되어야 합니다. */
    {
        int i = 0;
        while (i < (int)re.capacity && re.states[i].track_id != -2) i++;
        ASSERT_TRUE(i < (int)re.capacity);
        EXPECT_INT_EQ(re.states[i].overstay_latched, 1);
    }

    /* 감지 없어지면 latch 해제 */
    objs.count = 0;
    rules_evaluate_objects(&re, &objs, &tl, 101.0, &elog);
    {
        int i = 0;
        while (i < (int)re.capacity && re.states[i].track_id != -2) i++;
        if (i < (int)re.capacity)
            EXPECT_INT_EQ(re.states[i].overstay_latched, 0);
    }

    rules_destroy(&re);
    tracks_destroy(&tl);
    detection_list_destroy(&objs);
    event_log_close(&elog);
}

/* 음식 클래스(OBJ_FOOD_FIRST=4 ~ OBJ_FOOD_LAST=13) 감지 → external_food 이벤트 */
static void test_rules_obj_external_food(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    DetectionList objs;
    RulesConfig rcfg = {3600.0, 300.0, 5.0, 1.8f, 2.2f, 0, 0, 0, 0, 0, 0.15f, 1};
    char error[128] = {0};
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);
    ASSERT_INT_EQ(detection_list_init(&objs, 8), 0);

    make_obj_list(&objs, 50, 50, 100, 100, 0.75f, 11 /* pizza, OBJ_FOOD_FIRST+7 */);
    rules_evaluate_objects(&re, &objs, &tl, 100.0, &elog);

    {
        int i = 0;
        while (i < (int)re.capacity && re.states[i].track_id != -2) i++;
        ASSERT_TRUE(i < (int)re.capacity);
        EXPECT_INT_EQ(re.states[i].unordered_latched, 1);  /* food latch */
    }

    rules_destroy(&re);
    tracks_destroy(&tl);
    detection_list_destroy(&objs);
    event_log_close(&elog);
}

/* 동물(cat=0)과 의자(OBJ_CHAIR=14) bbox가 충분히 겹치면 animal_on_chair 발화 */
static void test_rules_obj_animal_on_chair(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    DetectionList objs;
    /* animal_iou_threshold=0.1: 동물/의자 IoU가 0.1 이상이면 발화 */
    RulesConfig rcfg = {3600.0, 300.0, 5.0, 1.8f, 2.2f, 0, 0, 0, 0, 0, 0.10f, 1};
    char error[128] = {0};
    int i;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);
    ASSERT_INT_EQ(detection_list_init(&objs, 8), 0);

    /* 의자(0,0)-(200,200)  동물(100,100)-(200,200) → 겹침 100x100=10000
     * 합집합 = 200×200 - 10000 = 30000, IoU = 10000/30000 = 0.333 > 0.10 */
    objs.count = 2;
    memset(&objs.items[0], 0, sizeof(objs.items[0]));
    objs.items[0].x1 = 0; objs.items[0].y1 = 0;
    objs.items[0].x2 = 200; objs.items[0].y2 = 200;
    objs.items[0].score = 0.9f;
    objs.items[0].class_id = 14; /* OBJ_CHAIR */

    memset(&objs.items[1], 0, sizeof(objs.items[1]));
    objs.items[1].x1 = 100; objs.items[1].y1 = 100;
    objs.items[1].x2 = 200; objs.items[1].y2 = 200;
    objs.items[1].score = 0.85f;
    objs.items[1].class_id = 0; /* OBJ_CAT */

    rules_evaluate_objects(&re, &objs, &tl, 100.0, &elog);

    i = 0;
    while (i < (int)re.capacity && re.states[i].track_id != -2) i++;
    ASSERT_TRUE(i < (int)re.capacity);
    EXPECT_INT_EQ(re.states[i].fall_latched, 1);  /* animal_on_chair latch */

    rules_destroy(&re);
    tracks_destroy(&tl);
    detection_list_destroy(&objs);
    event_log_close(&elog);
}

/* 동물(dog=1)과 dining table(OBJ_DININGTABLE=15)이 겹치면 animal_on_table 발화 */
static void test_rules_obj_animal_on_table(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    DetectionList objs;
    RulesConfig rcfg = {3600.0, 300.0, 5.0, 1.8f, 2.2f, 0, 0, 0, 0, 0, 0.10f, 1};
    char error[128] = {0};
    int i;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);
    ASSERT_INT_EQ(detection_list_init(&objs, 8), 0);

    objs.count = 2;
    memset(&objs.items[0], 0, sizeof(objs.items[0]));
    objs.items[0].x1 = 0; objs.items[0].y1 = 0;
    objs.items[0].x2 = 200; objs.items[0].y2 = 200;
    objs.items[0].score = 0.9f;
    objs.items[0].class_id = 15; /* OBJ_DININGTABLE */

    memset(&objs.items[1], 0, sizeof(objs.items[1]));
    objs.items[1].x1 = 50; objs.items[1].y1 = 50;
    objs.items[1].x2 = 150; objs.items[1].y2 = 150;
    objs.items[1].score = 0.8f;
    objs.items[1].class_id = 1; /* OBJ_DOG */

    rules_evaluate_objects(&re, &objs, &tl, 100.0, &elog);

    i = 0;
    while (i < (int)re.capacity && re.states[i].track_id != -2) i++;
    ASSERT_TRUE(i < (int)re.capacity);
    /* fall_start가 0이 아니면 animal_on_table latch가 설정된 것입니다. */
    EXPECT_TRUE(re.states[i].fall_start != 0.0);

    rules_destroy(&re);
    tracks_destroy(&tl);
    detection_list_destroy(&objs);
    event_log_close(&elog);
}

/* 2명 착석 + 컵 0개, margin=1 → 2 > 0+1 → no_cup_seated 발화
 * 2명 착석 + 컵 1개, margin=1 → 2 > 1+1 은 거짓 → 발화 없음 */
static void test_rules_obj_no_cup_seated(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    DetectionList objs;
    RulesConfig rcfg = {3600.0, 300.0, 5.0, 1.8f, 2.2f, 0, 0, 0, 0, 0, 0.15f, 1 /* margin=1 */};
    char error[128] = {0};
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);
    ASSERT_INT_EQ(detection_list_init(&objs, 8), 0);

    /* 활성 사람 2명 추가 */
    tl.count = 2;
    memset(tl.items, 0, sizeof(tl.items[0]) * 2);
    tl.items[0].id = 1; tl.items[0].active = 1;
    tl.items[1].id = 2; tl.items[1].active = 1;

    /* 컵 없음 → 2 > 0+1 → no_cup_seated 발화해야 함 */
    objs.count = 0;
    {
        int cnt_before = event_log_count(&elog, LOG_WARN);
        rules_evaluate_objects(&re, &objs, &tl, 100.0, &elog);
        EXPECT_TRUE(event_log_count(&elog, LOG_WARN) > cnt_before);  /* 로그가 기록됐어야 함 */
    }

    /* 컵 1개 → 2 > 1+1 은 거짓 → 발화 없음 */
    make_obj_list(&objs, 50, 50, 80, 100, 0.7f, 3 /* OBJ_CUP */);
    {
        int cnt_before = event_log_count(&elog, LOG_WARN);
        rules_evaluate_objects(&re, &objs, &tl, 101.0, &elog);
        EXPECT_TRUE(event_log_count(&elog, LOG_WARN) == cnt_before);  /* 아무것도 기록되지 않아야 함 */
    }

    rules_destroy(&re);
    tracks_destroy(&tl);
    detection_list_destroy(&objs);
    event_log_close(&elog);
}

/* ── 헬퍼: ResidueMonitor 기본 초기화 ──────────────────────────────────── */
static ResidueMonitor make_residue_monitor(void) {
    ResidueMonitor r;
    memset(&r, 0, sizeof(r));
    r.config.enabled                  = 1;
    r.config.diff_threshold           = 18;
    r.config.min_blocks               = 3;
    r.config.person_margin_blocks     = 1;
    r.config.confirm_seconds          = 60.0;
    r.config.clear_seconds            = 10.0;
    r.config.baseline_refresh_seconds = 300.0;
    r.config.global_change_ratio      = 1.1f; /* 기본 비활성 — 전용 테스트에서 명시적으로 설정 */
    return r;
}

/* 헬퍼: 1×1 GrayBuf (블록 1개, 원본 8×8 = downsample 8) */
static GrayBuf make_gray1(int value) {
    GrayBuf g;
    memset(&g, 0, sizeof(g));
    g.data      = (uint8_t *)malloc(1);
    g.width     = 1;
    g.height    = 1;
    g.downsample = 8;
    if (g.data) g.data[0] = (uint8_t)value;
    return g;
}

static void test_residue_no_event_before_confirm(void) {
    /* confirm_seconds 경과 전에는 이벤트가 발화하지 않아야 합니다. */
    ResidueMonitor r = make_residue_monitor();
    r.config.confirm_seconds = 60.0;
    r.config.min_blocks = 1; /* 1×1 테스트용 */

    /* 기준: 픽셀 값 0 */
    GrayBuf gray = make_gray1(0);
    ASSERT_TRUE(gray.data != NULL);
    residue_refresh_baseline(&r, &gray, 0.0);

    /* 현재 프레임: 픽셀 값 100 (차이 100 > threshold 18) */
    gray.data[0] = 100;

    EventLog elog;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);

    /* 59초 경과 — 확정 전 */
    int cnt_before = event_log_count(&elog, LOG_WARN);
    residue_evaluate(&r, &gray, NULL, 0, NULL, 0, 59.0, &elog);
    EXPECT_TRUE(event_log_count(&elog, LOG_WARN) == cnt_before); /* 이벤트 없음 */

    free(gray.data);
    residue_destroy(&r);
    event_log_close(&elog);
}

static void test_residue_confirms_after_hold(void) {
    /* confirm_seconds 경과 후 정확히 1회 이벤트가 발화해야 합니다. */
    ResidueMonitor r = make_residue_monitor();
    r.config.confirm_seconds = 60.0;
    r.config.min_blocks = 1;

    GrayBuf gray = make_gray1(0);
    ASSERT_TRUE(gray.data != NULL);
    residue_refresh_baseline(&r, &gray, 0.0);
    gray.data[0] = 100;

    EventLog elog;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);

    /* 등록 (t=0) 후 61초 경과 — 확정 */
    residue_evaluate(&r, &gray, NULL, 0, NULL, 0, 0.0, &elog);
    residue_evaluate(&r, &gray, NULL, 0, NULL, 0, 61.0, &elog);
    EXPECT_TRUE(event_log_count(&elog, LOG_WARN) > 0); /* 이벤트 발화 */

    /* 같은 조건 재호출 — 래치로 인해 중복 발화 없음 */
    int cnt_after = event_log_count(&elog, LOG_WARN);
    residue_evaluate(&r, &gray, NULL, 0, NULL, 0, 62.0, &elog);
    EXPECT_TRUE(event_log_count(&elog, LOG_WARN) == cnt_after);

    free(gray.data);
    residue_destroy(&r);
    event_log_close(&elog);
}

static void test_residue_excludes_person_overlap(void) {
    /* 사람 bbox와 겹치는 변화는 후보가 되지 않아야 합니다. */
    ResidueMonitor r = make_residue_monitor();
    r.config.confirm_seconds = 0.1; /* 즉시 확정 허용 */
    r.config.min_blocks = 1;

    GrayBuf gray = make_gray1(0);
    ASSERT_TRUE(gray.data != NULL);
    residue_refresh_baseline(&r, &gray, 0.0);
    gray.data[0] = 100; /* 큰 차이 */

    /* 사람 bbox: 블록 [0,0]을 완전히 포함 (원본 0~7px 범위) */
    GrayRect person;
    person.x1 = 0.0f; person.y1 = 0.0f;
    person.x2 = 7.0f; person.y2 = 7.0f;

    EventLog elog;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);

    int cnt_before = event_log_count(&elog, LOG_WARN);
    residue_evaluate(&r, &gray, &person, 1, NULL, 0, 1.0, &elog);
    EXPECT_TRUE(event_log_count(&elog, LOG_WARN) == cnt_before); /* 사람에 의해 배제 — 이벤트 없음 */

    free(gray.data);
    residue_destroy(&r);
    event_log_close(&elog);
}

static void test_residue_global_change_resets(void) {
    /* 전역 변화 비율 초과 시 영역 폐기 + 기준 갱신 이벤트가 발생해야 합니다. */
    ResidueMonitor r = make_residue_monitor();
    r.config.global_change_ratio = 0.5f;
    r.config.min_blocks = 1;

    /* 2×1 그레이 버퍼 (블록 2개) */
    GrayBuf gray;
    memset(&gray, 0, sizeof(gray));
    gray.data      = (uint8_t *)malloc(2);
    gray.width     = 2;
    gray.height    = 1;
    gray.downsample = 8;
    ASSERT_TRUE(gray.data != NULL);

    /* 기준: 두 픽셀 모두 0 */
    gray.data[0] = 0; gray.data[1] = 0;
    residue_refresh_baseline(&r, &gray, 0.0);

    /* 현재: 두 픽셀 모두 100 — 전체 블록이 후보 (비율 1.0 > 0.5) */
    gray.data[0] = 100; gray.data[1] = 100;

    EventLog elog;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);

    int cnt_before = event_log_count(&elog, LOG_INFO);
    residue_evaluate(&r, &gray, NULL, 0, NULL, 0, 1.0, &elog);
    EXPECT_TRUE(event_log_count(&elog, LOG_INFO) > cnt_before); /* residue_baseline_reset 이벤트 */

    free(gray.data);
    residue_destroy(&r);
    event_log_close(&elog);
}

static void test_residue_clears_after_absence(void) {
    /* clear_seconds 경과 후 cleared 이벤트가 발생하고 래치가 해제되어야 합니다. */
    ResidueMonitor r = make_residue_monitor();
    r.config.confirm_seconds = 0.1;
    r.config.clear_seconds   = 10.0;
    r.config.min_blocks = 1;

    GrayBuf gray = make_gray1(0);
    ASSERT_TRUE(gray.data != NULL);
    residue_refresh_baseline(&r, &gray, 0.0);
    gray.data[0] = 100;

    EventLog elog;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);

    /* 등록 후 확정 (confirm_seconds=0.1이므로 1.0s 후 확정) */
    residue_evaluate(&r, &gray, NULL, 0, NULL, 0, 0.0, &elog);
    residue_evaluate(&r, &gray, NULL, 0, NULL, 0, 1.0, &elog);

    /* 잔류물 사라짐 — 기준과 동일한 값으로 */
    gray.data[0] = 0;

    /* clear_seconds 미경과 — 아직 cleared 없음 */
    int cnt_before = event_log_count(&elog, LOG_WARN);
    residue_evaluate(&r, &gray, NULL, 0, NULL, 0, 5.0, &elog);
    EXPECT_TRUE(event_log_count(&elog, LOG_WARN) == cnt_before);

    /* clear_seconds 경과 — cleared 발화 */
    residue_evaluate(&r, &gray, NULL, 0, NULL, 0, 20.0, &elog);
    EXPECT_TRUE(event_log_count(&elog, LOG_WARN) > cnt_before);

    free(gray.data);
    residue_destroy(&r);
    event_log_close(&elog);
}

static void test_residue_min_blocks_filter(void) {
    /* min_blocks 미만 영역은 폐기되어야 합니다. */
    ResidueMonitor r = make_residue_monitor();
    r.config.confirm_seconds = 0.1;
    r.config.min_blocks = 3; /* 3블록 이상만 유효 */

    /* 1×1 그레이 버퍼 (1블록) */
    GrayBuf gray = make_gray1(0);
    ASSERT_TRUE(gray.data != NULL);
    residue_refresh_baseline(&r, &gray, 0.0);
    gray.data[0] = 100;

    EventLog elog;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);

    int cnt_before = event_log_count(&elog, LOG_WARN);
    residue_evaluate(&r, &gray, NULL, 0, NULL, 0, 1.0, &elog);
    EXPECT_TRUE(event_log_count(&elog, LOG_WARN) == cnt_before); /* 1블록이므로 min_blocks=3에 의해 폐기 */

    free(gray.data);
    residue_destroy(&r);
    event_log_close(&elog);
}

/* ── 슬롯 모니터 단위 테스트 ─────────────────────────────────────────────── */

/* 테스트용 80×80 RGB 프레임 (전부 같은 luma 값으로 채움) */
#define SM_W 80
#define SM_H 80

static uint8_t *make_rgb_frame(uint8_t luma) {
    uint8_t *rgb = (uint8_t *)malloc(SM_W * SM_H * 3);
    if (!rgb) return NULL;
    memset(rgb, luma, (size_t)(SM_W * SM_H * 3));
    return rgb;
}

/* 의자 bbox: (0,0)~(79,79) — 프레임 전체를 덮는 슬롯 */
static GrayRect make_chair_bbox(void) {
    GrayRect r;
    r.x1 = 0.0f; r.y1 = 0.0f;
    r.x2 = (float)SM_W; r.y2 = (float)SM_H;
    return r;
}

static void test_slot_monitor_init(void) {
    /* init 후 count=0, destroy는 free를 호출하지 않아야 합니다. */
    SlotMonitor sm;
    slot_monitor_init(&sm);
    EXPECT_TRUE(sm.count == 0);
    EXPECT_TRUE(sm.warn_seconds == 60);
    EXPECT_TRUE(sm.urgent_seconds == 300);
    slot_monitor_destroy(&sm);
    EXPECT_TRUE(sm.count == 0);
}

static void test_slot_monitor_add_slot(void) {
    /* furniture bbox 1개 → 슬롯 1개 생성 */
    SlotMonitor sm;
    slot_monitor_init(&sm);

    uint8_t *rgb = make_rgb_frame(50);
    ASSERT_TRUE(rgb != NULL);

    GrayRect chair = make_chair_bbox();
    EventLog elog;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);

    slot_monitor_update(&sm, rgb, SM_W, SM_H, &chair, 1, NULL, 0, 1.0, &elog);
    EXPECT_TRUE(sm.count == 1);

    free(rgb);
    event_log_close(&elog);
    slot_monitor_destroy(&sm);
}

static void test_slot_monitor_ttl_expire(void) {
    /* TTL 이후에 슬롯이 제거되어야 합니다. */
    SlotMonitor sm;
    slot_monitor_init(&sm);
    sm.ttl_seconds = 10;

    uint8_t *rgb = make_rgb_frame(50);
    ASSERT_TRUE(rgb != NULL);

    GrayRect chair = make_chair_bbox();
    EventLog elog;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);

    /* t=0에 슬롯 추가 */
    slot_monitor_update(&sm, rgb, SM_W, SM_H, &chair, 1, NULL, 0, 0.0, &elog);
    EXPECT_TRUE(sm.count == 1);

    /* t=20, furniture 없음 — TTL(10) 초과 → 슬롯 만료 */
    slot_monitor_update(&sm, rgb, SM_W, SM_H, NULL, 0, NULL, 0, 20.0, &elog);
    EXPECT_TRUE(sm.count == 0);

    free(rgb);
    event_log_close(&elog);
    slot_monitor_destroy(&sm);
}

static void test_slot_monitor_dirty_warn(void) {
    /* dirty 누적 → warn_seconds 경과 후 warn_fired = 1 + WARN 이벤트 발화 */
    SlotMonitor sm;
    slot_monitor_init(&sm);
    sm.warn_seconds     = 60;
    sm.dirty_threshold  = 20;
    sm.min_dirty_blocks = 1;

    /* 기준 프레임: luma=50 */
    uint8_t *rgb_clean = make_rgb_frame(50);
    ASSERT_TRUE(rgb_clean != NULL);

    GrayRect chair = make_chair_bbox();
    EventLog elog;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);

    /* t=0: 슬롯 추가 + 기준 캡처 (사람 없음) */
    slot_monitor_update(&sm, rgb_clean, SM_W, SM_H, &chair, 1, NULL, 0, 0.0, &elog);
    ASSERT_TRUE(sm.count == 1);
    EXPECT_TRUE(sm.slots[0].has_baseline == 1);

    /* luma=100인 "오염" 프레임: diff=50 > threshold=20 */
    uint8_t *rgb_dirty = make_rgb_frame(100);
    ASSERT_TRUE(rgb_dirty != NULL);

    /* t=1: dirty 감지, 아직 warn_seconds 미경과 */
    slot_monitor_update(&sm, rgb_dirty, SM_W, SM_H, &chair, 1, NULL, 0, 1.0, &elog);
    EXPECT_TRUE(sm.slots[0].dirty == 1);
    EXPECT_TRUE(sm.slots[0].warn_fired == 0);

    /* t=70: warn_seconds=60 경과 → warn_fired */
    int cnt_before = event_log_count(&elog, LOG_WARN);
    slot_monitor_update(&sm, rgb_dirty, SM_W, SM_H, &chair, 1, NULL, 0, 70.0, &elog);
    EXPECT_TRUE(sm.slots[0].warn_fired == 1);
    EXPECT_TRUE(event_log_count(&elog, LOG_WARN) > cnt_before);

    free(rgb_clean);
    free(rgb_dirty);
    event_log_close(&elog);
    slot_monitor_destroy(&sm);
}

static void test_slot_monitor_person_skip(void) {
    /* 사람 bbox가 슬롯과 충분히 겹치면 dirty 판정을 건너뜁니다. */
    SlotMonitor sm;
    slot_monitor_init(&sm);
    sm.dirty_threshold  = 20;
    sm.min_dirty_blocks = 1;
    sm.person_iou_skip  = 0.10f;

    uint8_t *rgb_clean = make_rgb_frame(50);
    ASSERT_TRUE(rgb_clean != NULL);

    GrayRect chair = make_chair_bbox();
    EventLog elog;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);

    /* t=0: 기준 캡처 */
    slot_monitor_update(&sm, rgb_clean, SM_W, SM_H, &chair, 1, NULL, 0, 0.0, &elog);
    ASSERT_TRUE(sm.slots[0].has_baseline == 1);

    /* 사람 bbox = 슬롯과 동일 (IoU=1.0 > 0.10) */
    GrayRect person = make_chair_bbox();
    uint8_t *rgb_dirty = make_rgb_frame(100);
    ASSERT_TRUE(rgb_dirty != NULL);

    /* t=1: 오염 프레임이지만 사람이 겹쳐서 스킵 → dirty 안 됨 */
    slot_monitor_update(&sm, rgb_dirty, SM_W, SM_H, &chair, 1, &person, 1, 1.0, &elog);
    EXPECT_TRUE(sm.slots[0].dirty == 0);

    free(rgb_clean);
    free(rgb_dirty);
    event_log_close(&elog);
    slot_monitor_destroy(&sm);
}

static void test_slot_monitor_cleared(void) {
    /* dirty → clean 복구 시 slot_cleared 이벤트 발화 */
    SlotMonitor sm;
    slot_monitor_init(&sm);
    sm.warn_seconds     = 1;
    sm.dirty_threshold  = 20;
    sm.min_dirty_blocks = 1;

    uint8_t *rgb_clean = make_rgb_frame(50);
    ASSERT_TRUE(rgb_clean != NULL);

    GrayRect chair = make_chair_bbox();
    EventLog elog;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);

    /* t=0: 기준 캡처 */
    slot_monitor_update(&sm, rgb_clean, SM_W, SM_H, &chair, 1, NULL, 0, 0.0, &elog);
    ASSERT_TRUE(sm.slots[0].has_baseline == 1);

    /* t=1~2: dirty 누적 + warn 발화 */
    uint8_t *rgb_dirty = make_rgb_frame(100);
    ASSERT_TRUE(rgb_dirty != NULL);
    slot_monitor_update(&sm, rgb_dirty, SM_W, SM_H, &chair, 1, NULL, 0, 1.0, &elog);
    slot_monitor_update(&sm, rgb_dirty, SM_W, SM_H, &chair, 1, NULL, 0, 2.0, &elog);
    EXPECT_TRUE(sm.slots[0].dirty == 1);

    /* t=3: clean 프레임으로 복구 → slot_cleared */
    int cnt_before = event_log_count(&elog, LOG_INFO);
    slot_monitor_update(&sm, rgb_clean, SM_W, SM_H, &chair, 1, NULL, 0, 3.0, &elog);
    EXPECT_TRUE(sm.slots[0].dirty == 0);
    EXPECT_TRUE(sm.slots[0].warn_fired == 0); /* latch 해제 */
    EXPECT_TRUE(event_log_count(&elog, LOG_INFO) > cnt_before);

    free(rgb_clean);
    free(rgb_dirty);
    event_log_close(&elog);
    slot_monitor_destroy(&sm);
}

/* ── 자동 기준 캡처 · 접근 제어 ─────────────────────────────────────────── */

static uint32_t be_ip(int a, int b, int c, int d) {
    uint32_t v;
    unsigned char *p = (unsigned char *)&v;
    p[0] = (unsigned char)a; p[1] = (unsigned char)b;
    p[2] = (unsigned char)c; p[3] = (unsigned char)d;
    return v;
}

static void test_netaccess_subnet_and_pin_header(void) {
    uint32_t mask24 = be_ip(255, 255, 255, 0);
    char pin[64];
    EXPECT_TRUE(netaccess_same_subnet(be_ip(192,168,0,10), be_ip(192,168,0,200), mask24));
    EXPECT_TRUE(!netaccess_same_subnet(be_ip(192,168,0,10), be_ip(192,168,50,3), mask24)); /* 게스트 Wi-Fi */
    EXPECT_TRUE(netaccess_is_loopback(be_ip(127,0,0,1)));
    EXPECT_TRUE(netaccess_is_loopback(be_ip(127,5,6,7)));
    EXPECT_TRUE(!netaccess_is_loopback(be_ip(128,0,0,1)));
    EXPECT_TRUE(netaccess_extract_pin("POST /x HTTP/1.1\r\nHost: a\r\nx-hunik-pin:  4321 \r\n\r\n", pin, sizeof(pin)));
    EXPECT_TRUE(strcmp(pin, "4321") == 0);
    EXPECT_TRUE(!netaccess_extract_pin("POST /x HTTP/1.1\r\nHost: a\r\n\r\n{\"X-Hunik-Pin\":\"1\"}", pin, sizeof(pin))); /* 본문은 무시 */
    EXPECT_TRUE(!netaccess_extract_pin("GET / HTTP/1.1\r\nX-Hunik-Pin: \r\n\r\n", pin, sizeof(pin)));               /* 빈 값 */
    EXPECT_TRUE(!netaccess_extract_pin("GET / HTTP/1.1\r\nX-Hunik-Pin: 12 34\r\n\r\n", pin, sizeof(pin)));          /* 내부 공백 거부 */
}

static void test_residue_downsamples_fullres_baseline(void) {
    /* 파일 기준(원본 해상도)이 gray(다운샘플 8)와 만나면 블록 중앙 점 샘플로 축소되어야 합니다.
       예전에는 크기 불일치로 매 프레임 0 을 반환해 파일 기준이 있는 설치에서 감지가 죽어 있었습니다. */
    ResidueMonitor r = make_residue_monitor();
    GrayBuf gray = make_gray1(50);
    int i;
    ASSERT_TRUE(gray.data != NULL);
    r.baseline = (uint8_t *)malloc(64);
    ASSERT_TRUE(r.baseline != NULL);
    for (i = 0; i < 64; i++) r.baseline[i] = 0;
    r.baseline[4 * 8 + 4] = 50; /* 블록 중앙 (4,4) — gray_buf_update_luma 와 같은 샘플 위치 */
    r.baseline_w = 8; r.baseline_h = 8; r.baseline_ready = 1;
    residue_evaluate(&r, &gray, NULL, 0, NULL, 0, 1.0, NULL);
    EXPECT_INT_EQ(r.baseline_w, 1);
    EXPECT_INT_EQ(r.baseline_h, 1);
    EXPECT_INT_EQ(r.baseline[0], 50);
    EXPECT_INT_EQ(r.baseline_ready, 1);
    free(gray.data);
    residue_destroy(&r);
}

static void test_residue_auto_init_waits_for_quiet(void) {
    ResidueMonitor r = make_residue_monitor();
    GrayBuf gray = make_gray1(30);
    ASSERT_TRUE(gray.data != NULL);
    r.config.auto_enabled = 1;
    r.config.auto_quiet_seconds = 10.0;
    /* 사람 없음·정지·카메라 정상이 10초 유지돼야 기준을 잡습니다 */
    EXPECT_INT_EQ(residue_auto_init(&r, &gray, 0, 1, 0.0, 1.0), 0);
    EXPECT_INT_EQ(residue_auto_init(&r, &gray, 0, 1, 0.0, 6.0), 0);
    EXPECT_INT_EQ(residue_auto_init(&r, &gray, 1, 1, 0.0, 8.0), 0);  /* 사람 등장 → 처음부터 */
    EXPECT_INT_EQ(residue_auto_init(&r, &gray, 0, 1, 0.0, 9.0), 0);
    EXPECT_INT_EQ(residue_auto_init(&r, &gray, 0, 1, 0.0, 15.0), 0); /* 9 부터 6초 — 아직 */
    EXPECT_INT_EQ(residue_auto_init(&r, &gray, 0, 1, 0.5, 18.0), 0); /* 움직임 → 처음부터 */
    EXPECT_INT_EQ(residue_auto_init(&r, &gray, 0, 1, 0.0, 19.0), 0);
    EXPECT_INT_EQ(residue_auto_init(&r, &gray, 0, 1, 0.0, 29.5), 1); /* 19 부터 10.5초 */
    EXPECT_INT_EQ(r.baseline_ready, 1);
    EXPECT_INT_EQ(r.baseline[0], 30);
    EXPECT_INT_EQ(r.auto_phase, 2);
    EXPECT_INT_EQ(residue_auto_init(&r, &gray, 0, 1, 0.0, 40.0), 0); /* 이미 있음 */
    free(gray.data);
    residue_destroy(&r);
}

static void test_door_auto_captures_closed_then_open(void) {
    /* 16×16 프레임, 문 ROI (4,4)-(12,12). 사람 없음, 카메라 정상.
       1) 정지·무인 1초 → 닫힘 기준(전부 0) 저장  2) ROI 만 200 으로 변하고 0.5초 유지 → 열림 저장.
       경로 NULL 이라 파일은 쓰지 않고 메모리에만 설치합니다. */
    DoorMonitor d;
    uint8_t frame[16 * 16 * 3];
    int x, y, rc;
    memset(&d, 0, sizeof(d));
    d.enabled = 1; d.auto_enabled = 1;
    d.roi_x = 4; d.roi_y = 4; d.roi_w = 8; d.roi_h = 8;
    d.auto_quiet_seconds = 1.0; d.auto_open_hold_seconds = 0.5; d.auto_open_min_l1 = 25.0f;
    memset(frame, 0, sizeof(frame));

    EXPECT_INT_EQ(door_auto_update(&d, frame, 16, 16, 48, NULL, 0, 1, 1.0, NULL, NULL), 0); /* 첫 프레임: 정지 판정 불가 */
    EXPECT_INT_EQ(d.auto_phase, DOOR_AUTO_WAIT_CLOSED);
    EXPECT_INT_EQ(door_auto_update(&d, frame, 16, 16, 48, NULL, 0, 1, 1.5, NULL, NULL), 0);
    rc = door_auto_update(&d, frame, 16, 16, 48, NULL, 0, 1, 2.6, NULL, NULL);
    EXPECT_INT_EQ(rc, 1);
    ASSERT_TRUE(d.ref_closed_rgb != NULL);
    EXPECT_INT_EQ(d.auto_phase, DOOR_AUTO_WAIT_OPEN);

    /* 사람이 ROI 근처(확장 영역 안)에 있으면 아무것도 하지 않아야 합니다 */
    {
        GrayRect person = { 0.0f, 0.0f, 5.0f, 5.0f };
        EXPECT_INT_EQ(door_auto_update(&d, frame, 16, 16, 48, &person, 1, 1, 3.0, NULL, NULL), 0);
    }

    /* 문 열림: ROI 안만 200, 밖은 0 → ROI L1(200) ≫ 전체 샘플 L1(50) */
    for (y = 4; y < 12; y++) for (x = 4; x < 12; x++) {
        frame[(y * 16 + x) * 3 + 0] = 200;
        frame[(y * 16 + x) * 3 + 1] = 200;
        frame[(y * 16 + x) * 3 + 2] = 200;
    }
    EXPECT_INT_EQ(door_auto_update(&d, frame, 16, 16, 48, NULL, 0, 1, 4.0, NULL, NULL), 0); /* 정지 판정용 첫 프레임 */
    EXPECT_INT_EQ(door_auto_update(&d, frame, 16, 16, 48, NULL, 0, 1, 4.2, NULL, NULL), 0); /* 후보 시작 */
    rc = door_auto_update(&d, frame, 16, 16, 48, NULL, 0, 1, 4.8, NULL, NULL);
    EXPECT_INT_EQ(rc, 2);
    ASSERT_TRUE(d.ref_open_rgb != NULL);
    EXPECT_INT_EQ(d.auto_phase, DOOR_AUTO_DONE);
    EXPECT_INT_EQ(door_auto_update(&d, frame, 16, 16, 48, NULL, 0, 1, 5.0, NULL, NULL), 0);
    door_destroy(&d);
}

static void test_door_auto_stall_detection(void) {
    /* 유리문 시나리오: ROI 안에서 계속 변화가 일어나면(유리 너머 사람·차) 정지 조건이 영원히
       충족되지 않습니다. quiet 의 6배가 지나도록 누적 대기가 절반도 못 차면 auto_stalled=1 로
       설정 점검을 알려야 합니다 — 조용히 대기만 하면 "켜져 있는데 아무 일도 없는" 상태가 됩니다. */
    DoorMonitor d;
    uint8_t frame[16 * 16 * 3];
    int i, x, y;
    memset(&d, 0, sizeof(d));
    d.enabled = 1; d.auto_enabled = 1;
    d.roi_x = 4; d.roi_y = 4; d.roi_w = 8; d.roi_h = 8;
    d.auto_quiet_seconds = 10.0; d.auto_open_hold_seconds = 1.0; d.auto_open_min_l1 = 25.0f;
    memset(frame, 0, sizeof(frame));

    for (i = 0; i < 12; i++) {
        int v = (i % 2) ? 20 : 200;  /* 매 프레임 ROI 를 뒤집어 정지 조건을 계속 깨뜨림 */
        for (y = 4; y < 12; y++) for (x = 4; x < 12; x++) {
            frame[(y * 16 + x) * 3 + 0] = (uint8_t)v;
            frame[(y * 16 + x) * 3 + 1] = (uint8_t)v;
            frame[(y * 16 + x) * 3 + 2] = (uint8_t)v;
        }
        door_auto_update(&d, frame, 16, 16, 48, NULL, 0, 1, (double)i * 10.0, NULL, NULL);
        if (i * 10 <= 60) EXPECT_INT_EQ(d.auto_stalled, 0); /* 임계 전에는 조용히 대기 */
    }
    EXPECT_INT_EQ(d.auto_phase, DOOR_AUTO_WAIT_CLOSED);
    EXPECT_INT_EQ(d.auto_stalled, 1);
    EXPECT_TRUE(d.ref_closed_rgb == NULL); /* 정체 중에 엉뚱한 기준을 잡아 버리면 안 됨 */

    /* ROI 가 안정되면 정상 경로로 복귀해 기준을 잡고 정체 표시가 풀려야 합니다 */
    for (i = 0; i < 3; i++)
        door_auto_update(&d, frame, 16, 16, 48, NULL, 0, 1, 200.0 + i * 6.0, NULL, NULL);
    EXPECT_INT_EQ(d.auto_stalled, 0);
    ASSERT_TRUE(d.ref_closed_rgb != NULL);
    EXPECT_INT_EQ(d.auto_phase, DOOR_AUTO_WAIT_OPEN);
    door_destroy(&d);
}

/* 16x16 프레임, ROI (4,4)-(12,12) 인 DoorMonitor 를 만듭니다. band_ratio=0.3 → 밴드는 y 4..5. */
static void door_band_fixture(DoorMonitor *d) {
    memset(d, 0, sizeof(*d));
    d->enabled = 1;
    d->roi_x = 4; d->roi_y = 4; d->roi_w = 8; d->roi_h = 8;
    d->confirm_frames = 1;
    d->last_state = -1;
    d->band_ratio = 0.3f;
    d->band_valid = -1;
    d->ref_closed_w = d->ref_open_w = 16;
    d->ref_closed_h = d->ref_open_h = 16;
    d->ref_closed_rgb = (uint8_t *)calloc(16 * 16 * 3, 1);
    d->ref_open_rgb   = (uint8_t *)calloc(16 * 16 * 3, 1);
}

/* [y_from, y_to) × ROI 가로 범위를 값 v 로 채웁니다. */
static void fill_rows(uint8_t *buf, int y_from, int y_to, int v) {
    int x, y;
    for (y = y_from; y < y_to; y++)
        for (x = 4; x < 12; x++) {
            buf[(y * 16 + x) * 3 + 0] = (uint8_t)v;
            buf[(y * 16 + x) * 3 + 1] = (uint8_t)v;
            buf[(y * 16 + x) * 3 + 2] = (uint8_t)v;
        }
}

static void test_door_band_judges_while_occluded(void) {
    /* 문 전체가 열림에서 달라지는 정상 케이스. 사람이 문 아래쪽을 가려도 머리 위 밴드로
       열림을 맞혀야 합니다 — 전체 ROI 로 비교하면 사람 픽셀에 눌려 판정이 뒤집힙니다. */
    DoorMonitor d;
    uint8_t frame[16 * 16 * 3];
    GrayRect person = { 4.0f, 7.0f, 12.0f, 14.0f }; /* ROI 아래쪽을 덮음 */
    int changed = 0;
    door_band_fixture(&d);
    ASSERT_TRUE(d.ref_closed_rgb != NULL && d.ref_open_rgb != NULL);
    fill_rows(d.ref_open_rgb, 4, 12, 200);          /* 열림: ROI 전체가 밝아짐 */

    /* 현재 프레임: 문은 열려 있지만(밴드=200) 아래쪽은 어두운 옷을 입은 사람이 가려 0.
       전체 ROI 평균: 닫힘까지 (2*200+6*0)/8=50, 열림까지 (2*0+6*200)/8=150 → 닫힘으로 오판.
       밴드(y4..5)만 보면 닫힘까지 200, 열림까지 0 → 열림. 가림이 결과를 뒤집는 상황입니다. */
    memset(frame, 0, sizeof(frame));
    fill_rows(frame, 4, 6, 200);

    EXPECT_INT_EQ(door_check(&d, frame, 16, 16, 48, &person, 1, &changed), 1);
    EXPECT_INT_EQ(d.band_valid, 1);
    EXPECT_INT_EQ(d.band_active, 1);

    /* 같은 프레임을 가림 정보 없이 넣으면 전체 ROI 비교가 닫힘으로 오판합니다 —
       밴드가 실제로 결과를 바로잡았다는 대조 확인입니다. */
    d.last_state = -1; d.candidate_state = -1; d.candidate_frames = 0;
    EXPECT_INT_EQ(door_check(&d, frame, 16, 16, 48, NULL, 0, &changed), 0);
    EXPECT_INT_EQ(d.band_active, 0);
    door_destroy(&d);
}

static void test_door_band_rejects_transom_roi(void) {
    /* ROI 를 문틀 위(상인방)까지 잡아 밴드 구간이 열림·닫힘 간에 동일한 경우.
       밴드를 믿고 판정하면 계속 틀리므로, 무효로 표시하고 가림 중에는 보류해야 합니다. */
    DoorMonitor d;
    uint8_t frame[16 * 16 * 3];
    GrayRect person = { 4.0f, 7.0f, 12.0f, 14.0f };
    int changed = 0;
    door_band_fixture(&d);
    ASSERT_TRUE(d.ref_closed_rgb != NULL && d.ref_open_rgb != NULL);
    fill_rows(d.ref_open_rgb, 6, 12, 200);          /* 밴드(y4..5)는 그대로, 아래만 변함 */

    memset(frame, 0, sizeof(frame));
    fill_rows(frame, 6, 12, 200);

    EXPECT_INT_EQ(door_check(&d, frame, 16, 16, 48, &person, 1, &changed), -1); /* 판정 보류 */
    EXPECT_INT_EQ(d.band_valid, 0);
    EXPECT_INT_EQ(d.band_active, 0);
    /* 가림이 풀리면 전체 ROI 로 정상 판정 */
    EXPECT_INT_EQ(door_check(&d, frame, 16, 16, 48, NULL, 0, &changed), 1);
    door_destroy(&d);
}

static void test_door_band_disabled_holds_when_occluded(void) {
    /* band_ratio=0 이면 밴드를 쓰지 않습니다. 이때 가려지면 억지 판정 대신 보류해야 합니다 —
       문 앞에서 통화하는 손님 하나로 door_open 오탐이 나던 경로를 막습니다. */
    DoorMonitor d;
    uint8_t frame[16 * 16 * 3];
    GrayRect person = { 4.0f, 4.0f, 12.0f, 12.0f }; /* ROI 전체를 덮음 */
    int changed = 0;
    door_band_fixture(&d);
    ASSERT_TRUE(d.ref_closed_rgb != NULL && d.ref_open_rgb != NULL);
    d.band_ratio = 0.0f;
    fill_rows(d.ref_open_rgb, 4, 12, 200);
    memset(frame, 0, sizeof(frame));
    fill_rows(frame, 4, 12, 130);                   /* 사람 픽셀 — 열림 쪽에 더 가까움 */

    EXPECT_INT_EQ(door_check(&d, frame, 16, 16, 48, &person, 1, &changed), -1);
    EXPECT_INT_EQ(changed, 0);
    door_destroy(&d);
}

/* ── 미확인 소실 ──────────────────────────────────────────────────────────── */

static void vanish_track(Track *t) {
    memset(t, 0, sizeof(*t));
    t->id = 7;
    t->active = 1;
    t->misses = 1;              /* 매칭 실패 상태 */
    t->dwell_seconds = 30.0;    /* 충분히 오래 추적됨 */
    t->last_seen = 100.0;
    t->box.score = 0.62f;       /* 사라지기 직전 신뢰도 충분 */
    t->box.x1 = 400; t->box.y1 = 300; t->box.x2 = 460; t->box.y2 = 460;
}

static void test_vanish_at_door_uses_door_state(void) {
    /* 문 앞에서 사라짐: 문이 열렸으면 정상 퇴장, 닫힌 채였으면 나갈 수 없었다는 강한 신호. */
    RulesEngine re; Track t; VanishEvidence ev; EventLog el;
    ASSERT_TRUE(rules_init(&re, 8, NULL, NULL, 0) == 0);
    event_log_open(&el, ":memory:", LOG_INFO, 0);
    ev.near_door = 1; ev.near_edge = 1;   /* 문이 화면 끝에 있는 흔한 배치 */

    vanish_track(&t);
    ev.door_can_exit = 1; ev.residue_at_spot = 1;
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 120.0, &el), 0);  /* 나갔음 */
    EXPECT_INT_EQ(t.vanish_warned, 0);

    /* 문이 닫힌 채였으면 가장자리여도 경고해야 합니다 — 여기가 쓰러지면 가장 위험한 곳입니다. */
    vanish_track(&t);
    ev.door_can_exit = 0; ev.residue_at_spot = 0;
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 120.0, &el), 1);
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 130.0, &el), 0);  /* 중복 발화 없음 */

    /* 문 상태를 모르면(감지 꺼짐) 나간 것인지 알 수 없으므로 잔류가 있어야 합니다. */
    vanish_track(&t);
    ev.door_can_exit = -1; ev.residue_at_spot = 0;
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 120.0, &el), 0);
    vanish_track(&t);
    ev.residue_at_spot = 1;
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 120.0, &el), 1);
    rules_destroy(&re); event_log_close(&el);
}

static void test_vanish_ignores_walk_off_frame_edge(void) {
    /* 문이 아닌 화면 가장자리에서 사라진 경우 — 화각 밖으로 걸어 나갔을 수 있습니다.
       문이 닫혀 있다는 사실만으로는 구분되지 않습니다(카메라가 매장 전체를 못 덮음).
       잔류 흔적이 있을 때만 말해야 오탐이 쏟아지지 않습니다. */
    RulesEngine re; Track t; VanishEvidence ev; EventLog el;
    ASSERT_TRUE(rules_init(&re, 8, NULL, NULL, 0) == 0);
    event_log_open(&el, ":memory:", LOG_INFO, 0);
    ev.near_door = 0; ev.near_edge = 1; ev.door_can_exit = 0;  /* 문은 닫혀 있었음 */

    vanish_track(&t);
    ev.residue_at_spot = 0;
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 120.0, &el), 0);  /* 그냥 나간 것 */
    EXPECT_INT_EQ(t.vanish_warned, 0);

    vanish_track(&t);
    ev.residue_at_spot = 1;                                          /* 뭔가 남았다 */
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 120.0, &el), 1);
    rules_destroy(&re); event_log_close(&el);
}

static void test_vanish_interior_and_escalation(void) {
    /* 화면 한가운데 — 나갈 곳이 없는데 사라졌으므로 가장 강한 신호입니다.
       ERROR 승격은 잔류가 확인될 때만 합니다. */
    RulesEngine re; Track t; VanishEvidence ev; EventLog el;
    ASSERT_TRUE(rules_init(&re, 8, NULL, NULL, 0) == 0);
    event_log_open(&el, ":memory:", LOG_INFO, 0);
    vanish_track(&t);
    ev.near_door = 0; ev.near_edge = 0; ev.door_can_exit = 0; ev.residue_at_spot = 0;

    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 120.0, &el), 1);
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 130.0, &el), 0);  /* 잔류 없음 → 승격 안 함 */
    EXPECT_INT_EQ(t.vanish_escalated, 0);

    ev.residue_at_spot = 1;
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 140.0, &el), 2);
    EXPECT_INT_EQ(t.vanish_escalated, 1);
    EXPECT_TRUE(event_log_count(&el, LOG_ERROR) > 0);

    /* 칸막이가 많은 매장용 옵션: 화면 안쪽에서도 잔류를 요구 */
    {
        RulesConfig rc = re.config;
        rc.vanish_require_residue = 1;
        rules_update_config(&re, &rc);
        vanish_track(&t);
        ev.residue_at_spot = 0;
        EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 200.0, &el), 0);
    }
    rules_destroy(&re); event_log_close(&el);
}

static void test_vanish_ignores_weak_and_brief_tracks(void) {
    /* 깜빡이던 트랙·저신뢰 탐지가 사라진 것은 정보가 아닙니다. 여기를 막지 않으면
       잡동사니를 사람으로 오인했다 놓칠 때마다 경고가 납니다. */
    RulesEngine re; Track t; VanishEvidence ev; EventLog el;
    ASSERT_TRUE(rules_init(&re, 8, NULL, NULL, 0) == 0);
    event_log_open(&el, ":memory:", LOG_INFO, 0);
    ev.door_can_exit = 0; ev.residue_at_spot = 1; ev.near_edge = 0; ev.near_door = 0;

    vanish_track(&t); t.dwell_seconds = 1.0;               /* 너무 짧게 추적됨 */
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 120.0, &el), 0);

    vanish_track(&t); t.box.score = 0.22f;                 /* 원래 흐릿하던 탐지 */
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 120.0, &el), 0);

    vanish_track(&t);                                      /* 아직 유예 시간 전 */
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 103.0, &el), 0);
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 106.0, &el), 1); /* 5초 경과 → 발화 */
    rules_destroy(&re); event_log_close(&el);
}

static void test_vanish_clears_on_reappear(void) {
    /* 다시 보이면 래치가 풀려 재발 시 다시 감지되어야 합니다. */
    RulesEngine re; Track t; VanishEvidence ev; EventLog el;
    ASSERT_TRUE(rules_init(&re, 8, NULL, NULL, 0) == 0);
    event_log_open(&el, ":memory:", LOG_INFO, 0);
    vanish_track(&t);
    ev.door_can_exit = 0; ev.residue_at_spot = 1; ev.near_edge = 0; ev.near_door = 0;
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 120.0, &el), 1);
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 125.0, &el), 2);

    t.misses = 0; t.active = 1;                            /* 재등장 */
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 130.0, &el), 0);
    EXPECT_INT_EQ(t.vanish_warned, 0);
    EXPECT_INT_EQ(t.vanish_escalated, 0);

    t.misses = 1; t.last_seen = 130.0;                     /* 다시 사라짐 */
    EXPECT_INT_EQ(rules_check_vanish(&re, &t, &ev, 140.0, &el), 1);
    rules_destroy(&re); event_log_close(&el);
}

/* ── 적응형 감속 ──────────────────────────────────────────────────────────── */

static ThrottleConfig throttle_cfg(void) {
    ThrottleConfig c;
    c.enabled = 1; c.light_percent = 50; c.heavy_percent = 70; c.minimal_percent = 85;
    c.hysteresis_percent = 10; c.min_level_seconds = 5.0; c.check_seconds = 2.0;
    return c;
}

static void test_throttle_levels_and_hysteresis(void) {
    /* 올라갈 때는 진입 임계값 그대로 — 키오스크가 갑자기 바빠지면 즉시 물러나야 합니다. */
    ThrottleConfig c = throttle_cfg();
    EXPECT_INT_EQ(throttle_decide(&c, THROTTLE_NONE, 49.0), THROTTLE_NONE);
    EXPECT_INT_EQ(throttle_decide(&c, THROTTLE_NONE, 50.0), THROTTLE_LIGHT);
    EXPECT_INT_EQ(throttle_decide(&c, THROTTLE_NONE, 72.0), THROTTLE_HEAVY);   /* 한 번에 두 단계 */
    EXPECT_INT_EQ(throttle_decide(&c, THROTTLE_NONE, 90.0), THROTTLE_MINIMAL);

    /* 내려올 때는 히스테리시스만큼 더 떨어져야 풀립니다. 같은 값을 쓰면 경계에서
       단계가 초당 여러 번 뒤집혀 로그가 폭주하고 추론 주기가 흔들립니다. */
    EXPECT_INT_EQ(throttle_decide(&c, THROTTLE_LIGHT, 45.0), THROTTLE_LIGHT);  /* 40 밑까지 유지 */
    EXPECT_INT_EQ(throttle_decide(&c, THROTTLE_LIGHT, 39.0), THROTTLE_NONE);
    EXPECT_INT_EQ(throttle_decide(&c, THROTTLE_HEAVY, 65.0), THROTTLE_HEAVY);  /* 60 밑까지 유지 */
    EXPECT_INT_EQ(throttle_decide(&c, THROTTLE_HEAVY, 55.0), THROTTLE_LIGHT);  /* 한 단계씩 복귀 */
    EXPECT_INT_EQ(throttle_decide(&c, THROTTLE_MINIMAL, 80.0), THROTTLE_MINIMAL);
    EXPECT_INT_EQ(throttle_decide(&c, THROTTLE_MINIMAL, 74.0), THROTTLE_HEAVY);

    /* 꺼져 있으면 부하와 무관하게 항상 NONE */
    c.enabled = 0;
    EXPECT_INT_EQ(throttle_decide(&c, THROTTLE_HEAVY, 99.0), THROTTLE_NONE);
}

static void test_throttle_actions_per_level(void) {
    /* 단계별 동작이 실제로 부하를 줄이는 방향인지 — 추론 주기는 늘고 Tier 2 는 꺼져야 합니다. */
    EXPECT_INT_EQ(throttle_detect_multiplier(THROTTLE_NONE),    1);
    EXPECT_INT_EQ(throttle_detect_multiplier(THROTTLE_LIGHT),   2);
    EXPECT_INT_EQ(throttle_detect_multiplier(THROTTLE_HEAVY),   4);
    EXPECT_INT_EQ(throttle_detect_multiplier(THROTTLE_MINIMAL), 0); /* 추론 정지 */
    EXPECT_INT_EQ(throttle_tier2_multiplier(THROTTLE_HEAVY),    0);
    EXPECT_INT_EQ(throttle_stream_fps(THROTTLE_NONE,  10), 10);
    EXPECT_INT_EQ(throttle_stream_fps(THROTTLE_HEAVY, 10), 5);
    EXPECT_INT_EQ(throttle_stream_fps(THROTTLE_MINIMAL, 2), 1);     /* 0 으로 떨어지지 않음 */
}

static void test_throttle_needs_baseline_before_deciding(void) {
    /* 첫 호출은 기준점만 잡아야 합니다. 바로 판단하면 프로세스 시작 직후의
       로딩 부하를 "키오스크가 바쁘다"로 오인해 곧장 감속합니다. */
    Throttle t;
    ThrottleConfig c = throttle_cfg();
    throttle_init(&t, &c);
    EXPECT_INT_EQ(throttle_update(&t, 10.0, 1.0, 4), 0);
    EXPECT_INT_EQ(t.level, THROTTLE_NONE);
    /* check_seconds 전에는 측정 자체를 건너뜁니다 */
    EXPECT_INT_EQ(throttle_update(&t, 10.5, 1.1, 4), 0);
    EXPECT_INT_EQ(t.level, THROTTLE_NONE);
}

/* ── 예약 재시작 ──────────────────────────────────────────────────────────── */

static RestartConfig restart_cfg(void) {
    RestartConfig c;
    c.enabled = 1;
    c.days = 127;          /* 매일 */
    c.hour = 4; c.minute = 0;
    c.window_minutes = 60;
    return c;
}

static void test_restart_time_parse_and_window(void) {
    int h = 0, m = 0;
    RestartConfig c = restart_cfg();
    EXPECT_TRUE(restart_parse_time("04:00", &h, &m) && h == 4 && m == 0);
    EXPECT_TRUE(restart_parse_time("23:59", &h, &m) && h == 23 && m == 59);
    EXPECT_TRUE(!restart_parse_time("24:00", &h, &m));
    EXPECT_TRUE(!restart_parse_time("4", &h, &m));
    EXPECT_TRUE(!restart_parse_time("04:00:00", &h, &m));  /* 뒤에 잡다한 문자 거부 */

    EXPECT_INT_EQ(restart_in_window(&c, 3, 3 * 60 + 59), 0); /* 03:59 — 아직 */
    EXPECT_INT_EQ(restart_in_window(&c, 3, 4 * 60), 1);      /* 04:00 */
    EXPECT_INT_EQ(restart_in_window(&c, 3, 4 * 60 + 59), 1); /* 04:59 — 창 안 */
    EXPECT_INT_EQ(restart_in_window(&c, 3, 5 * 60), 0);      /* 05:00 — 창 밖 */

    c.days = 1 << 1;                                         /* 월요일만 */
    EXPECT_INT_EQ(restart_in_window(&c, 1, 4 * 60), 1);
    EXPECT_INT_EQ(restart_in_window(&c, 2, 4 * 60), 0);
    c.enabled = 0;
    EXPECT_INT_EQ(restart_in_window(&c, 1, 4 * 60), 0);
}

static void test_restart_defers_while_people_present(void) {
    /* 재시작은 추적 중인 체류 시간과 래치를 모두 잃습니다. 사람이 있으면 미뤄야 합니다. */
    RestartScheduler s;
    RestartConfig c = restart_cfg();
    restart_init(&s, &c);

    EXPECT_INT_EQ(restart_check(&s, 3, 100, 3 * 60, 0), RESTART_NO);        /* 03:00 */
    EXPECT_INT_EQ(restart_check(&s, 3, 100, 4 * 60, 1), RESTART_DEFER);     /* 사람 있음 */
    EXPECT_INT_EQ(restart_check(&s, 3, 100, 4 * 60 + 1, 1), RESTART_NO);    /* 연기 로그는 1회만 */
    EXPECT_INT_EQ(restart_check(&s, 3, 100, 4 * 60 + 5, 0), RESTART_NOW);   /* 사람 떠남 */
    EXPECT_INT_EQ(restart_check(&s, 3, 100, 4 * 60 + 6, 0), RESTART_NO);    /* 오늘은 끝 */
}

static void test_restart_skips_when_window_passes(void) {
    /* 사람이 계속 있어 창을 넘기면 오늘은 포기해야 합니다 — 미뤄 두었다가 영업이
       한창인 낮에 갑자기 재시작되는 것이 한 번 건너뛰는 것보다 훨씬 나쁩니다. */
    RestartScheduler s;
    RestartConfig c = restart_cfg();
    restart_init(&s, &c);
    EXPECT_INT_EQ(restart_check(&s, 3, 100, 4 * 60 + 10, 1), RESTART_DEFER);
    EXPECT_INT_EQ(restart_check(&s, 3, 100, 5 * 60 + 1, 1), RESTART_SKIP);  /* 창 지남 */
    EXPECT_INT_EQ(restart_check(&s, 3, 100, 14 * 60, 0), RESTART_NO);       /* 낮에 안 함 */
    /* 다음 날 같은 시각에는 다시 시도 */
    EXPECT_INT_EQ(restart_check(&s, 4, 101, 4 * 60, 0), RESTART_NOW);
}

static void test_restart_state_file_prevents_loop(void) {
    /* 재시작은 프로세스 상태를 초기화합니다. 상태 파일이 없으면 새로 뜬 프로세스가
       여전히 예정 창 안이라고 판단해 곧바로 또 재시작합니다 — 창 내내 무한 루프입니다. */
    RestartScheduler s;
    RestartConfig c = restart_cfg();
    const char *path = "test_restart_state.tmp";
    remove(path);

    restart_init(&s, &c);
    EXPECT_INT_EQ(restart_state_load(&s, path, 100, 125), 0);   /* 파일 없음 = 미처리 */
    EXPECT_INT_EQ(restart_check(&s, 3, 100, 4 * 60, 0), RESTART_NOW);
    EXPECT_INT_EQ(restart_state_save(path, 100, 125), 0);

    /* 재시작 직후 새 프로세스 — 같은 날, 아직 창 안 */
    restart_init(&s, &c);
    EXPECT_INT_EQ(restart_state_load(&s, path, 100, 125), 1);
    EXPECT_INT_EQ(restart_check(&s, 3, 100, 4 * 60 + 10, 0), RESTART_NO); /* 루프 차단 */

    /* 다음 날에는 다시 동작해야 합니다 */
    restart_init(&s, &c);
    EXPECT_INT_EQ(restart_state_load(&s, path, 101, 125), 0);
    EXPECT_INT_EQ(restart_check(&s, 4, 101, 4 * 60, 0), RESTART_NOW);
    remove(path);
}

static void test_restart_config_change_rearms_today(void) {
    /* 점주가 시각을 옮겼는데 "오늘은 이미 했음"으로 남아 새 시각이 무시되면
       설정이 안 먹히는 것처럼 보입니다. */
    RestartScheduler s;
    RestartConfig c = restart_cfg();
    restart_init(&s, &c);
    EXPECT_INT_EQ(restart_check(&s, 3, 100, 4 * 60, 0), RESTART_NOW);
    EXPECT_INT_EQ(restart_check(&s, 3, 100, 4 * 60 + 1, 0), RESTART_NO);
    c.hour = 6;
    restart_configure(&s, &c);
    EXPECT_INT_EQ(restart_check(&s, 3, 100, 6 * 60, 0), RESTART_NOW);
}

int main(void) {
    TEST_SUITE_BEGIN(core_unit_tests);
    RUN_TEST(test_letterbox);
    RUN_TEST(test_restart_time_parse_and_window);
    RUN_TEST(test_restart_defers_while_people_present);
    RUN_TEST(test_restart_skips_when_window_passes);
    RUN_TEST(test_restart_state_file_prevents_loop);
    RUN_TEST(test_restart_config_change_rearms_today);
    RUN_TEST(test_throttle_levels_and_hysteresis);
    RUN_TEST(test_throttle_actions_per_level);
    RUN_TEST(test_throttle_needs_baseline_before_deciding);
    RUN_TEST(test_vanish_at_door_uses_door_state);
    RUN_TEST(test_vanish_ignores_walk_off_frame_edge);
    RUN_TEST(test_vanish_interior_and_escalation);
    RUN_TEST(test_vanish_ignores_weak_and_brief_tracks);
    RUN_TEST(test_vanish_clears_on_reappear);
    RUN_TEST(test_netaccess_subnet_and_pin_header);
    RUN_TEST(test_door_auto_stall_detection);
    RUN_TEST(test_door_band_judges_while_occluded);
    RUN_TEST(test_door_band_rejects_transom_roi);
    RUN_TEST(test_door_band_disabled_holds_when_occluded);
    RUN_TEST(test_residue_downsamples_fullres_baseline);
    RUN_TEST(test_residue_auto_init_waits_for_quiet);
    RUN_TEST(test_door_auto_captures_closed_then_open);
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
    RUN_TEST(test_rules_fall_no_hip_no_fire);
    RUN_TEST(test_rules_fall_with_hip_fires);
    RUN_TEST(test_config_rect_and_time);
    RUN_TEST(test_camera_health_whiteout);
    RUN_TEST(test_camera_health_frozen);
    RUN_TEST(test_gray_luma_matches_plane);
    RUN_TEST(test_gray_analyze_single_pass);
    RUN_TEST(test_motion_map_locates_change);
    RUN_TEST(test_motion_gate_static_scene);
    RUN_TEST(test_door_state_debounce);
    RUN_TEST(test_gray_matches_reference);
    /* Tier 2 class_id 전파 및 rules_evaluate_objects 테스트 */
    RUN_TEST(test_decode_class_id_pose_is_zero);
    RUN_TEST(test_decode_class_id_multiclass_argmax);
    RUN_TEST(test_rules_obj_external_drink);
    RUN_TEST(test_rules_obj_external_food);
    RUN_TEST(test_rules_obj_animal_on_chair);
    RUN_TEST(test_rules_obj_animal_on_table);
    RUN_TEST(test_rules_obj_no_cup_seated);
    /* 잔류물 감지 단위 테스트 */
    RUN_TEST(test_residue_no_event_before_confirm);
    RUN_TEST(test_residue_confirms_after_hold);
    RUN_TEST(test_residue_excludes_person_overlap);
    RUN_TEST(test_residue_global_change_resets);
    RUN_TEST(test_residue_clears_after_absence);
    RUN_TEST(test_residue_min_blocks_filter);
    /* 슬롯 기반 모니터 단위 테스트 */
    RUN_TEST(test_slot_monitor_init);
    RUN_TEST(test_slot_monitor_add_slot);
    RUN_TEST(test_slot_monitor_ttl_expire);
    RUN_TEST(test_slot_monitor_dirty_warn);
    RUN_TEST(test_slot_monitor_person_skip);
    RUN_TEST(test_slot_monitor_cleared);
    TEST_SUITE_END();
}
