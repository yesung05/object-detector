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
#include "calib.h"
#include "capture.h"
#include "privacy_style.h"
#include "fight_stage2.h"
#include "reconnect.h"
#include "../third_party/sqlite/sqlite3.h" /* count_events: 룰 발화 횟수 검증 */
#include "infer_cache.h"

#if defined(_WIN32)
#include <direct.h>
#define test_rmdir _rmdir
#else
#include <unistd.h>
#define test_rmdir rmdir
#endif

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
    /* [1, 2, 6]: shape[2]==6 → embedded_nms. class_id 는 보존해서 돌려주고, 어떤 클래스를
     * 쓸지는 호출자가 정합니다(postprocess.c 주석). 같은 자리의 서로 다른 클래스 row 는
     * NMS 가 클래스별이므로 둘 다 남습니다. 예전에는 클래스 비구분 NMS 가 class=1 row 를
     * 우연히 지워서 "필터링된 것처럼" 보였을 뿐입니다. */
    const float output[] = {
        10.0f, 10.0f, 60.0f, 60.0f, 0.85f, 0.0f,  /* person(class=0) */
        10.0f, 10.0f, 60.0f, 60.0f, 0.70f, 1.0f   /* non-person(class=1) */
    };
    int64_t shape[] = {1, 2, 6};
    Letterbox t = {100, 100, 100, 100, 1.0f, 0, 0};
    DetectionList list;
    ASSERT_INT_EQ(detection_list_init(&list, 8), 0);
    ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, 0.25f, 0.45f, &list, 8), 0);
    EXPECT_INT_EQ((int)list.count, 2);
    EXPECT_INT_EQ(list.items[0].class_id, 0);
    EXPECT_FLOAT_NEAR(list.items[0].score, 0.85f, 0.001f);
    EXPECT_INT_EQ(list.items[1].class_id, 1);
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

static void test_draw_label_glyphs_exist(void) {
    /* 화면에 실제로 그려지는 레이블 문구(draw.c, main.c, throttle.c)의 글자가 모두 글리프를 가지는지.
       예전에는 Y 가 없어 "MAYBE ORDER" 가 "MA BE ORDER" 로 보였다. */
    const char *labels[] = {"MAYBE ORDER", "ORDERED", "PERSON", "MISS", "OBJ", "CAT", "DOG", "CUP", "FOOD",
                            "DRINK", "TABLE", "CHAIR", "ANIMAL", "SAVE FAILED", "HEAVY", "INF", "CAM", "CPU"};
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i)
        for (const char *c = labels[i]; *c; ++c)
            if (*c != ' ') EXPECT_TRUE(draw_glyph_defined(*c));
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

/* events 테이블에서 message 가 prefix 로 시작하는 행 수를 셉니다.
 * 기존 룰 테스트는 발화를 확인하지 않고 지나가기만 해서, 재발화 같은 "횟수" 문제는
 * 이 헬퍼 없이는 잡을 수 없습니다. */
static int count_events(const EventLog *elog, const char *prefix) {
    sqlite3_stmt *st = NULL; /* 소유: 이 함수, finalize 로 해제 */
    char like[96];
    int n = -1;
    snprintf(like, sizeof(like), "%s%%", prefix);
    if (sqlite3_prepare_v2(elog->db, "SELECT count(*) FROM events WHERE message LIKE ?",
                           -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, like, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* limbo 중인 트랙이 부활해도 overstay 를 다시 발화하면 안 됩니다.
 * 현장 로그에서 한 트랙이 214회 재발화한 원인이 이 경로였습니다. */
static void test_rules_limbo_revive_does_not_refire(void) {
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
    tl.items[0].dwell_seconds = 120.0;
    tl.items[0].order = TRACK_UNORDERED;
    rules_evaluate(&re, &tl, 100.0, &elog);
    EXPECT_INT_EQ(count_events(&elog, "overstay"), 1);

    /* limbo 진입(만료 전) → 부활: 같은 id·dwell 이므로 다시 발화하면 안 됩니다. */
    tl.items[0].active = 0;
    tl.items[0].limbo_expired_at = 1900.0;
    rules_evaluate(&re, &tl, 101.0, &elog);
    tl.items[0].active = 1;
    rules_evaluate(&re, &tl, 102.0, &elog);
    EXPECT_INT_EQ(count_events(&elog, "overstay"), 1);

    rules_destroy(&re);
    tracks_destroy(&tl);
    event_log_close(&elog);
}

/* limbo 가 만료되면 상태를 반납하므로, 그 슬롯을 물려받은 새 트랙은 새로 판정받습니다. */
static void test_rules_limbo_expiry_releases_state(void) {
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
    tl.items[0].dwell_seconds = 120.0;
    tl.items[0].order = TRACK_UNORDERED;
    rules_evaluate(&re, &tl, 100.0, &elog);
    EXPECT_INT_EQ(count_events(&elog, "overstay"), 1);

    tl.items[0].active = 0;
    tl.items[0].limbo_expired_at = 150.0;
    rules_evaluate(&re, &tl, 200.0, &elog);   /* 만료 후 → 반납 */
    tl.items[0].active = 1;                    /* 슬롯을 재사용한 새 트랙 */
    rules_evaluate(&re, &tl, 201.0, &elog);
    EXPECT_INT_EQ(count_events(&elog, "overstay"), 2);

    rules_destroy(&re);
    tracks_destroy(&tl);
    event_log_close(&elog);
}

/* roi_kiosk 가 없으면 ORDERED 로 갈 수 없으므로 unordered_seated 를 판정하지 않습니다. */
static void test_rules_unordered_requires_roi(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig no_roi = {3600.0, 30.0, 5.0, 1.8f, 2.2f, 0, 0, 0, 0, 0};
    RulesConfig with_roi = {3600.0, 30.0, 5.0, 1.8f, 2.2f, 10, 10, 50, 50, 1};
    char error[128] = {0};
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);

    tl.count = 1;
    tl.items[0].id = 1;
    tl.items[0].active = 1;
    tl.items[0].dwell_seconds = 31.0;
    tl.items[0].order = TRACK_UNORDERED;

    ASSERT_INT_EQ(rules_init(&re, 16, &no_roi, error, sizeof(error)), 0);
    rules_evaluate(&re, &tl, 100.0, &elog);
    EXPECT_INT_EQ(count_events(&elog, "unordered_seated"), 0);
    rules_destroy(&re);

    ASSERT_INT_EQ(rules_init(&re, 16, &with_roi, error, sizeof(error)), 0);
    rules_evaluate(&re, &tl, 100.0, &elog);
    EXPECT_INT_EQ(count_events(&elog, "unordered_seated"), 1);
    rules_destroy(&re);

    tracks_destroy(&tl);
    event_log_close(&elog);
}

/* ── Re-ID 부활 시 체류 초기화 ────────────────────────────────────────────── */

/* 단색 프레임에 박스 하나를 만들고(히스토그램 동일 → Re-ID 가 반드시 성공), 트랙을 limbo 로 보냈다가 gone_seconds 뒤
 * 같은 박스로 부활시킨다. 부활한 트랙의 dwell 을 돌려준다. keep: reid_dwell_keep_seconds. */
static double reid_revive_dwell(double keep, double revive_at, int *same_id, OrderState *order_out) {
    enum { W = 200, H = 200 };
    static uint8_t rgb[W * H * 3];
    TrackList tl; DetectionList det; Detection d; char error[128] = {0};
    double dwell; int i;
    for (i = 0; i < W * H; ++i) { rgb[i * 3] = 200; rgb[i * 3 + 1] = 50; rgb[i * 3 + 2] = 50; }
    memset(&d, 0, sizeof(d)); d.x1 = 50; d.y1 = 50; d.x2 = 120; d.y2 = 180; d.score = 0.9f;
    tracks_init(&tl, 16, 0.3f, 2, 1800.0, 0.45f, error, sizeof(error));
    tl.reid_dwell_keep_seconds = keep;
    det.items = &d; det.capacity = 1; det.count = 1;
    for (i = 0; i <= 10; ++i) tracks_update(&tl, &det, rgb, W, H, W * 3, 100.0 + i);   /* dwell 10초 */
    tl.items[0].order = TRACK_ORDERED;
    det.count = 0;
    for (i = 1; i <= 3; ++i) tracks_update(&tl, &det, rgb, W, H, W * 3, 110.0 + i);    /* 3번 놓쳐 limbo */
    det.count = 1;
    tracks_update(&tl, &det, rgb, W, H, W * 3, revive_at);                              /* 부활 시도 */
    *same_id = (tl.count == 1 && tl.items[0].id == 1 && tl.items[0].active);
    dwell = tl.items[0].dwell_seconds;
    if (order_out) *order_out = tl.items[0].order;
    tracks_destroy(&tl);
    return dwell;
}

static void test_reid_revive_resets_dwell_only_after_long_absence(void) {
    int same; OrderState ord;
    /* 짧은 이탈(90초 < 300): 같은 사람으로 보고 체류·주문 상태를 이어 간다 */
    EXPECT_FLOAT_NEAR((float)reid_revive_dwell(300.0, 200.0, &same, &ord), 10.0f, 1e-3f);
    EXPECT_TRUE(same); EXPECT_INT_EQ((int)ord, (int)TRACK_ORDERED);
    /* 긴 이탈(890초 > 300): 같은 id 로 부활하더라도 체류와 주문 상태를 처음부터 */
    EXPECT_FLOAT_NEAR((float)reid_revive_dwell(300.0, 1000.0, &same, &ord), 0.0f, 1e-3f);
    EXPECT_TRUE(same); EXPECT_INT_EQ((int)ord, (int)TRACK_UNORDERED);
    /* 키 0(끔): 기존 동작 — 오래 떠나도 체류를 이어 간다 */
    EXPECT_FLOAT_NEAR((float)reid_revive_dwell(0.0, 1000.0, &same, &ord), 10.0f, 1e-3f);
    EXPECT_TRUE(same); EXPECT_INT_EQ((int)ord, (int)TRACK_ORDERED);
}

/* 체류가 초기화되면 이미 발화한 미주문 착석도 새 방문으로 다시 판정받아야 한다. */
static void test_rules_unordered_refires_after_dwell_reset(void) {
    TrackList tl; RulesEngine re; EventLog elog;
    RulesConfig rcfg = {3600.0, 30.0, 5.0, 1.8f, 2.2f, 10, 10, 50, 50, 1};
    char error[128] = {0};
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);
    tl.count = 1; tl.items[0].id = 1; tl.items[0].active = 1; tl.items[0].order = TRACK_UNORDERED;
    tl.items[0].dwell_seconds = 31.0;
    rules_evaluate(&re, &tl, 100.0, &elog);
    EXPECT_INT_EQ(count_events(&elog, "unordered_seated"), 1);
    tl.items[0].dwell_seconds = 40.0;
    rules_evaluate(&re, &tl, 101.0, &elog);
    EXPECT_INT_EQ(count_events(&elog, "unordered_seated"), 1);  /* 같은 방문은 한 번만 */
    tl.items[0].dwell_seconds = 0.0;                            /* Re-ID 가 체류를 초기화한 상황 */
    rules_evaluate(&re, &tl, 102.0, &elog);
    tl.items[0].dwell_seconds = 31.0;
    rules_evaluate(&re, &tl, 103.0, &elog);
    EXPECT_INT_EQ(count_events(&elog, "unordered_seated"), 2);  /* 새 방문은 다시 판정 */
    rules_destroy(&re); tracks_destroy(&tl); event_log_close(&elog);
}

/* ── 알림 모드(on/shadow/off) ──────────────────────────────────────────────── */

static int observed_shadow = 0;
static void alert_observer(void *ctx, long long id, LogLevel level, const char *module, const char *message) {
    (void)ctx; (void)id; (void)level; (void)module;
    if (strstr(message, ALERT_SHADOW_SUFFIX)) observed_shadow++;
}

static void test_alert_modes_on_shadow_off(void) {
    EventLog elog;
    char summary[200];
    event_log_clear_alert_modes();
    ASSERT_INT_EQ(event_log_open(&elog, ":memory:", LOG_INFO, 0), 0);
    elog.observer = alert_observer; elog.observer_context = NULL; observed_shadow = 0;

    /* 기본(모드 없음): 그대로 기록 */
    event_log_write(&elog, LOG_ERROR, "rules", "person_fallen track=1 hold=3.0s");
    EXPECT_INT_EQ(count_events(&elog, "person_fallen track=1 hold=3.0s"), 1);
    EXPECT_INT_EQ(count_events(&elog, "person_fallen track=1 hold=3.0s mode=shadow"), 0);

    /* shadow: 기록하되 끝에 접미사. 원문은 그대로 앞에 남아 첫 단어 토큰을 쓰는 분석이 깨지지 않는다 */
    event_log_set_alert_mode("person_fallen", ALERT_SHADOW);
    event_log_write(&elog, LOG_ERROR, "rules", "person_fallen track=2 hold=3.0s");
    EXPECT_INT_EQ(count_events(&elog, "person_fallen track=2 hold=3.0s mode=shadow"), 1);
    EXPECT_INT_EQ(observed_shadow, 1);                /* 관찰자(리플레이 기록)도 그림자 이벤트를 받는다 */
    event_log_write(&elog, LOG_WARN, "rules", "overstay track=3 dwell=9s");   /* 다른 이벤트는 영향 없음 */
    EXPECT_INT_EQ(count_events(&elog, "overstay track=3 dwell=9s"), 1);
    EXPECT_INT_EQ(count_events(&elog, "overstay track=3 dwell=9s mode=shadow"), 0);

    /* off: 기록하지 않는다 */
    event_log_set_alert_mode("overstay", ALERT_OFF);
    event_log_write(&elog, LOG_WARN, "rules", "overstay track=4 dwell=9s");
    EXPECT_INT_EQ(count_events(&elog, "overstay track=4"), 0);

    /* 토큰은 공백뿐 아니라 ':' 에서도 끊긴다("door_open: ...") */
    event_log_set_alert_mode("door_open", ALERT_SHADOW);
    event_log_write(&elog, LOG_WARN, "door", "door_open: 30초 이상 개방 상태 지속");
    EXPECT_INT_EQ(count_events(&elog, "door_open: 30초 이상 개방 상태 지속 mode=shadow"), 1);

    /* 접두 일치가 아니라 토큰 전체 일치다: person_unaccounted 의 모드가 person_unaccounted_cleared 에 번지면 안 된다 */
    event_log_set_alert_mode("person_unaccounted", ALERT_OFF);
    event_log_write(&elog, LOG_INFO, "rules", "person_unaccounted_cleared track=5");
    EXPECT_INT_EQ(count_events(&elog, "person_unaccounted_cleared track=5"), 1);

    /* 요약 */
    EXPECT_INT_EQ(event_log_alert_summary(summary, sizeof(summary)), 4);
    EXPECT_TRUE(strstr(summary, "shadow=") != NULL && strstr(summary, "off=") != NULL);
    EXPECT_TRUE(strstr(summary, "person_fallen") != NULL && strstr(summary, "overstay") != NULL);

    /* 해제(ON)하면 다시 그대로 기록 */
    event_log_set_alert_mode("person_fallen", ALERT_ON);
    event_log_write(&elog, LOG_ERROR, "rules", "person_fallen track=6 hold=3.0s");
    EXPECT_INT_EQ(count_events(&elog, "person_fallen track=6 hold=3.0s"), 1);
    EXPECT_INT_EQ(count_events(&elog, "person_fallen track=6 hold=3.0s mode=shadow"), 0);

    event_log_clear_alert_modes();
    EXPECT_INT_EQ(event_log_alert_summary(summary, sizeof(summary)), 0);
    EXPECT_INT_EQ((int)strlen(summary), 0);
    event_log_close(&elog);
}

/* 매우 긴 메시지에서도 접미사가 잘리지 않고, 원문이 잘려 나가는 쪽을 택한다(필터가 접미사로 거르므로). */
static void test_alert_shadow_suffix_survives_long_message(void) {
    EventLog elog;
    char longmsg[1400];
    event_log_clear_alert_modes();
    ASSERT_INT_EQ(event_log_open(&elog, ":memory:", LOG_INFO, 0), 0);
    event_log_set_alert_mode("external_drink", ALERT_SHADOW);
    memset(longmsg, 'x', sizeof(longmsg)); memcpy(longmsg, "external_drink ", 15); longmsg[sizeof(longmsg) - 1] = 0;
    event_log_write(&elog, LOG_WARN, "rules", longmsg);
    {
        sqlite3_stmt *st = NULL; int ok = 0;
        ASSERT_INT_EQ(sqlite3_prepare_v2(elog.db, "SELECT count(*) FROM events WHERE message LIKE '% mode=shadow'", -1, &st, NULL), SQLITE_OK);
        if (sqlite3_step(st) == SQLITE_ROW) ok = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
        EXPECT_INT_EQ(ok, 1);
    }
    event_log_clear_alert_modes();
    event_log_close(&elog);
}

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

static Detection fall_test_pose(int upright) {
    Detection b={0};b.score=.9f;b.keypoint_count=17;
    b.x1=100;b.x2=upright?180.0f:350.0f;b.y1=upright?100.0f:200.0f;b.y2=300;
    b.kp[0]=(Keypoint){upright?140.0f:120.0f,upright?120.0f:245.0f,.9f};
    b.kp[5]=(Keypoint){upright?120.0f:140.0f,upright?150.0f:240.0f,.9f};
    b.kp[6]=(Keypoint){upright?160.0f:150.0f,upright?150.0f:250.0f,.9f};
    b.kp[11]=(Keypoint){upright?120.0f:290.0f,upright?250.0f:240.0f,.9f};
    b.kp[12]=(Keypoint){upright?160.0f:300.0f,250,.9f};
    return b;
}
static void fall_observe(RulesEngine *re,TrackList *tl,EventLog *log,double now) {
    tl->items[0].last_seen=now;tl->items[0].match_score=tl->items[0].box.score;
    rules_evaluate(re,tl,now,log);
}
static void fall_seed_upright(RulesEngine *re,TrackList *tl,EventLog *log) {
    tl->items[0].box=fall_test_pose(1);
    fall_observe(re,tl,log,98);fall_observe(re,tl,log,98.5);fall_observe(re,tl,log,99.1);
    tl->items[0].box=fall_test_pose(0);
}

static void test_fall_false_positive_regressions(void) {
    /* Reproduce bypass, bed shape, stale boxes, carried score, bad pose and gaps. */
    for(int scenario=0;scenario<9;scenario++) {
        TrackList tl;RulesEngine re={0};EventLog log;char error[128]={0};
        RulesConfig cfg={3600,300,5,1.8f,2.2f};
        ASSERT_INT_EQ(event_log_open(&log,":memory:",LOG_INFO,0),0);
        ASSERT_INT_EQ(tracks_init(&tl,16,.3f,5,1800,.45f,error,sizeof(error)),0);
        ASSERT_INT_EQ(rules_init(&re,16,&cfg,error,sizeof(error)),0);
        tl.count=1;tl.items[0].id=1;tl.items[0].active=1;
        if(scenario!=1)fall_seed_upright(&re,&tl,&log);
        tl.items[0].box=fall_test_pose(0);
        for(int j=0;j<9;j++) {
            Track *t=&tl.items[0];double now=100+j;
            t->fall_sudden=1;t->head_valid=1;t->head_cy_norm=.9f;t->head_y_fall_threshold_norm=.5f;
            t->last_seen=now;t->match_score=.9f;
            if(scenario==0)t->box.score=t->match_score=.29f;
            if(scenario==2)t->box.keypoint_count=0;
            if(scenario==3)t->box.kp[11].score=t->box.kp[12].score=0;
            if(scenario==4)t->match_score=.12f; /* held .9 box must not mask low incoming score */
            if(scenario==5)t->last_seen=100; /* repeated old evidence cannot complete hold */
            if(scenario==6)t->misses=1;
            if(scenario==7)t->box.kp[11].x=10000;
            if(scenario==8 && j==3)t->misses=1;
            else if(scenario==8)t->misses=0;
            rules_evaluate(&re,&tl,now,&log);
            EXPECT_INT_EQ(re.fall_fired_count,0);
            EXPECT_INT_EQ(t->fall_sudden,0);
        }
        rules_destroy(&re);tracks_destroy(&tl);event_log_close(&log);
    }
}

/* fall_hold_max_misses: 누운 사람 9관측(100~108초, hold 5초) 중 일부를 결손(점수 .29)이나 직립으로 바꾼다.
 * miss_a/miss_b: 결손으로 만들 관측 번호(-1 이면 없음), upright_at: 직립으로 만들 관측 번호. */
static int fall_miss_scenario(int max_misses, int miss_a, int miss_b, int upright_at) {
    TrackList tl; RulesEngine re = {0}; EventLog log; char error[128] = {0};
    RulesConfig cfg = {3600, 300, 5, 1.8f, 2.2f};
    int fired, j;
    cfg.fall_hold_max_misses = max_misses;
    event_log_open(&log, ":memory:", LOG_INFO, 0);
    tracks_init(&tl, 16, .3f, 5, 1800, .45f, error, sizeof(error));
    rules_init(&re, 16, &cfg, error, sizeof(error));
    tl.count = 1; tl.items[0].id = 1; tl.items[0].active = 1;
    fall_seed_upright(&re, &tl, &log);
    for (j = 0; j < 9; ++j) {
        Track *t = &tl.items[0]; double now = 100.0 + j;
        t->box = fall_test_pose(j == upright_at); t->last_seen = now; t->match_score = .9f;
        if (j == miss_a || j == miss_b) t->box.score = t->match_score = .29f;
        rules_evaluate(&re, &tl, now, &log);
    }
    fired = count_events(&log, "person_fallen");
    rules_destroy(&re); tracks_destroy(&tl); event_log_close(&log);
    return fired;
}

static void test_fall_hold_tolerates_missed_observations(void) {
    EXPECT_INT_EQ(fall_miss_scenario(0, -1, -1, -1), 1); /* 결손 없음 → 발화 */
    EXPECT_INT_EQ(fall_miss_scenario(0, 3, -1, -1), 0);  /* 이전 동작: 결손 1번에 hold 재시작 → 4초뿐 */
    EXPECT_INT_EQ(fall_miss_scenario(1, 3, -1, -1), 1);  /* 1번 허용 → hold 유지 → 발화 */
    EXPECT_INT_EQ(fall_miss_scenario(1, 3, 4, -1), 0);   /* 연속 2번은 허용치 초과 → 재시작 */
    EXPECT_INT_EQ(fall_miss_scenario(1, 3, 5, -1), 1);   /* 떨어진 결손 2번은 각각 1번씩 → 유지 */
    EXPECT_INT_EQ(fall_miss_scenario(3, -1, -1, 3), 0);  /* 직립은 결손이 아니라 반증 → 즉시 재시작 */
}

/* ── 쓰러짐 보강: 트랙 인계와 넓은 박스 증거(둘 다 config 로 켜는 기능, 기본 꺼짐) ───────────── */

/* 서 있던 트랙 1 이 쓰러지며 매칭이 끊기고, 같은 자리에 누운 사람이 새 트랙 2 로 잡힌 상황을 만든다. */
static int fall_handoff_scenario(double handoff_seconds, double track2_first_seen, float track2_dx,
                                 float reach) {
    TrackList tl; RulesEngine re = {0}; EventLog log; char error[128] = {0};
    RulesConfig cfg = {3600, 300, 5, 1.8f, 2.2f};
    int fired, j;
    cfg.fall_handoff_seconds = handoff_seconds; cfg.fall_handoff_reach = reach;
    event_log_open(&log, ":memory:", LOG_INFO, 0);
    tracks_init(&tl, 16, .3f, 5, 1800, .45f, error, sizeof(error));
    rules_init(&re, 16, &cfg, error, sizeof(error));
    tl.count = 2;
    tl.items[0].id = 1; tl.items[0].active = 1;
    fall_seed_upright(&re, &tl, &log);                 /* 트랙 1: 직립 이력(98~99.1초) */
    tl.items[0].box = fall_test_pose(1);               /* 끊기기 직전 마지막 박스는 선 모양 */
    tl.items[0].misses = 1; tl.items[0].last_seen = 99.1; /* 이후 매칭되지 않음 */
    tl.items[1].id = 2; tl.items[1].active = 1; tl.items[1].first_seen = track2_first_seen;
    tl.items[1].box = fall_test_pose(0);
    tl.items[1].box.x1 += track2_dx; tl.items[1].box.x2 += track2_dx;
    for (j = 0; j < 9; ++j) {
        double now = 100.0 + j;
        tl.items[1].last_seen = now; tl.items[1].match_score = .9f;
        rules_evaluate(&re, &tl, now, &log);
    }
    fired = count_events(&log, "person_fallen");
    rules_destroy(&re); tracks_destroy(&tl); event_log_close(&log);
    return fired;
}

static void test_fall_handoff_links_fragmented_track(void) {
    EXPECT_INT_EQ(fall_handoff_scenario(0.0, 99.5, 0.0f, 0.0f), 0);   /* 꺼짐(기본): 새 트랙은 직립 이력이 없어 막힌다 */
    EXPECT_INT_EQ(fall_handoff_scenario(3.0, 99.5, 0.0f, 0.0f), 1);   /* 켜짐: 이력을 승계해 발화 */
    EXPECT_INT_EQ(fall_handoff_scenario(3.0, 110.0, 0.0f, 0.0f), 0);  /* 새 트랙이 한참 뒤에 태어남 = 다른 사람 */
    EXPECT_INT_EQ(fall_handoff_scenario(3.0, 99.5, 5000.0f, 0.0f), 0);/* 멀리 떨어진 곳 = 다른 사람 */
    /* 도달 거리 배율: 두 박스 중심 거리 약 99px, 기본 도달 250px. 배율 1.0 은 기본과 같고 0.3(75px)은 막힌다. */
    EXPECT_INT_EQ(fall_handoff_scenario(3.0, 99.5, 0.0f, 1.0f), 1);
    EXPECT_INT_EQ(fall_handoff_scenario(3.0, 99.5, 0.0f, 0.3f), 0);
}

/* 이미 매칭 중인 트랙(아직 보이는 사람)의 이력은 새 트랙에 넘기지 않는다. */
static void test_fall_handoff_ignores_matched_track(void) {
    TrackList tl; RulesEngine re = {0}; EventLog log; char error[128] = {0};
    RulesConfig cfg = {3600, 300, 5, 1.8f, 2.2f};
    int j;
    cfg.fall_handoff_seconds = 3.0;
    event_log_open(&log, ":memory:", LOG_INFO, 0);
    tracks_init(&tl, 16, .3f, 5, 1800, .45f, error, sizeof(error));
    rules_init(&re, 16, &cfg, error, sizeof(error));
    tl.count = 2;
    tl.items[0].id = 1; tl.items[0].active = 1;
    fall_seed_upright(&re, &tl, &log);
    tl.items[0].box = fall_test_pose(1);               /* 트랙 1 은 계속 서 있다(misses==0) */
    tl.items[1].id = 2; tl.items[1].active = 1; tl.items[1].first_seen = 99.5;
    tl.items[1].box = fall_test_pose(0);
    for (j = 0; j < 9; ++j) {
        double now = 100.0 + j;
        tl.items[0].last_seen = now; tl.items[0].match_score = .9f;
        tl.items[1].last_seen = now; tl.items[1].match_score = .9f;
        rules_evaluate(&re, &tl, now, &log);
    }
    EXPECT_INT_EQ(count_events(&log, "person_fallen"), 0);
    rules_destroy(&re); tracks_destroy(&tl); event_log_close(&log);
}

/* 직립 이력이 있는 트랙의 낮은 점수 넓은 박스. mode: 0 기본, 1 이력 없음, 2 점수 낮음, 3 가장자리, 4 비율 부족, 5 중간에 직립 복귀 */
static int fall_wide_scenario(float wide_ratio, int mode) {
    TrackList tl; RulesEngine re = {0}; EventLog log; char error[128] = {0};
    RulesConfig cfg = {3600, 300, 5, 1.8f, 2.2f};
    int fired, j;
    cfg.fall_wide_ratio = wide_ratio; cfg.fall_wide_min_score = .25f;
    event_log_open(&log, ":memory:", LOG_INFO, 0);
    tracks_init(&tl, 16, .3f, 5, 1800, .45f, error, sizeof(error));
    rules_init(&re, 16, &cfg, error, sizeof(error));
    rules_set_frame_size(&re, 1920, 1080);
    tl.count = 1; tl.items[0].id = 1; tl.items[0].active = 1;
    if (mode != 1) fall_seed_upright(&re, &tl, &log);
    for (j = 0; j < 9; ++j) {
        Track *t = &tl.items[0]; double now = 100.0 + j;
        t->box = (Detection){0};
        t->box.x1 = mode == 3 ? 0.0f : 100.0f; t->box.x2 = t->box.x1 + (mode == 4 ? 110.0f : 250.0f);
        t->box.y1 = 200; t->box.y2 = 300; t->box.keypoint_count = 0; /* 키포인트 판정 불가 */
        t->box.score = mode == 2 ? .2f : .3f;
        t->match_score = t->box.score; t->last_seen = now;
        if (mode == 5 && j == 3) { t->box = fall_test_pose(1); t->match_score = .9f; } /* 다시 일어섬 */
        rules_evaluate(&re, &tl, now, &log);
    }
    fired = count_events(&log, "person_fallen");
    rules_destroy(&re); tracks_destroy(&tl); event_log_close(&log);
    return fired;
}

static void test_fall_wide_box_evidence(void) {
    EXPECT_INT_EQ(fall_wide_scenario(0.0f, 0), 0);   /* 꺼짐(기본): 낮은 신뢰도 박스는 근거가 아니다 */
    EXPECT_INT_EQ(fall_wide_scenario(1.6f, 0), 1);   /* 켜짐: 이력 + 넓은 박스 유지 → 발화 */
    EXPECT_INT_EQ(fall_wide_scenario(1.6f, 1), 0);   /* 직립 이력 없음 */
    EXPECT_INT_EQ(fall_wide_scenario(1.6f, 2), 0);   /* 관측 점수가 최소 미만 */
    EXPECT_INT_EQ(fall_wide_scenario(1.6f, 3), 0);   /* 화면 가장자리에 닿아 비율을 믿을 수 없음 */
    EXPECT_INT_EQ(fall_wide_scenario(1.6f, 4), 0);   /* 박스가 충분히 넓지 않음(1.1) */
    EXPECT_INT_EQ(fall_wide_scenario(1.6f, 5), 0);   /* 중간에 다시 서면 누적이 초기화됨 */
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

/* PROBABLY_ORDERED 는 미주문 착석을 억제해야 합니다. UNORDERED 는 발화, 이미 발화한 뒤
 * PROBABLY_ORDERED 로 바뀌면 래치가 풀려야 합니다. */
static void test_rules_unordered_suppressed_by_probable_order(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    /* grace=30초. roi_kiosk 가 있어야 unordered_seated 를 판정하므로 ROI 를 설정합니다. */
    RulesConfig rcfg = {3600.0, 30.0, 5.0, 1.8f, 2.2f, 10, 10, 50, 50, 1};
    char error[128] = {0};
    int before, i;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);

    tl.count = 1;
    memset(&tl.items[0], 0, sizeof(tl.items[0]));
    tl.items[0].id = 1;
    tl.items[0].active = 1;
    tl.items[0].dwell_seconds = 31.0;
    tl.items[0].box = (Detection){0, 0, 30, 100, 0.9f};

    /* 주문 추정이면 grace 를 넘어도 발화하지 않음 */
    tl.items[0].order = TRACK_PROBABLY_ORDERED;
    before = event_log_count(&elog, LOG_WARN);
    rules_evaluate(&re, &tl, 100.0, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before);

    /* 미주문이면 발화, 같은 상태가 계속돼도 중복 없음 */
    tl.items[0].order = TRACK_UNORDERED;
    rules_evaluate(&re, &tl, 101.0, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before + 1);
    rules_evaluate(&re, &tl, 102.0, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before + 1);

    /* 발화 뒤 주문 추정으로 바뀌면 래치 해제 */
    tl.items[0].order = TRACK_PROBABLY_ORDERED;
    rules_evaluate(&re, &tl, 103.0, &elog);
    for (i = 0; i < (int)re.capacity && re.states[i].track_id != 1; ++i) {}
    ASSERT_TRUE(i < (int)re.capacity);
    EXPECT_INT_EQ(re.states[i].unordered_latched, 0);

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
    fall_seed_upright(&re,&tl,&elog);
    fall_observe(&re,&tl,&elog,100.0);
    fall_observe(&re,&tl,&elog,100.1);
    fall_observe(&re,&tl,&elog,100.2);

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

static void test_decode_tier2_layouts_and_legacy(void) {
    enum { N = 100, MAX_C = 84 };
    float output[N * MAX_C];
    Letterbox t = {320, 320, 320, 320, 1.0f, 0, 0};
    DetectionList list;
    const int counts[] = {7, 16, 80};
    ASSERT_INT_EQ(detection_list_init(&list, 8), 0);
    for (int version = 0; version < 3; ++version) {
        int nc = counts[version], c = nc + 4;
        for (int layout = 0; layout < 2; ++layout) {
            int64_t shape[3] = {1, layout ? N : c, layout ? c : N};
            for (int id = 0; id < nc; ++id) {
                int expected = nc == 7 && id == 5 ? OBJ_CHAIR :
                               nc == 7 && id == 6 ? OBJ_DININGTABLE : id;
                memset(output, 0, sizeof(output));
                output[0] = 160.0f;
                output[layout ? 1 : N] = 160.0f;
                output[layout ? 2 : 2*N] = 60.0f;
                output[layout ? 3 : 3*N] = 60.0f;
                output[layout ? 4+id : (4+id)*N] = 0.87f;
                ASSERT_INT_EQ(yolo11_decode(output, shape, 3, &t, .2f, .45f, &list, 8), 0);
                ASSERT_INT_EQ((int)list.count, 1);
                EXPECT_INT_EQ(list.items[0].class_id, expected);
                EXPECT_FLOAT_NEAR(list.items[0].score, .87f, .0001f);
            }
        }
    }
    detection_list_destroy(&list);
}

/* 7클래스 Tier 2 출력 [1,11,N]에 박스 2개를 직접 써 넣고 NMS 결과 개수를 봅니다.
 * 채널 0~3=cx,cy,w,h, 4~10=cat,dog,bottle,cup,food,chair,dining table 점수. */
static int decode_two_boxes(int class_a, float score_a, int class_b, float score_b,
                            DetectionList *list, int *out_first, int *out_second) {
    /* N 은 채널 수(C)보다 커야 디코더가 [1,C,N] 채널-우선 배치로 인식합니다. */
    enum { N = 16, C = 11 };
    float output[C * N];
    int64_t shape[3] = {1, C, N};
    Letterbox t = {320, 320, 320, 320, 1.0f, 0, 0};
    int rc;
    memset(output, 0, sizeof(output));
    /* 두 박스는 100x100에 중심이 10px 어긋나 IoU≈0.68 — NMS 기준 0.45를 넘습니다. */
    output[0 * N + 0] = 160.0f; output[1 * N + 0] = 160.0f;
    output[2 * N + 0] = 100.0f; output[3 * N + 0] = 100.0f;
    output[(4 + class_a) * N + 0] = score_a;
    output[0 * N + 1] = 170.0f; output[1 * N + 1] = 165.0f;
    output[2 * N + 1] = 100.0f; output[3 * N + 1] = 100.0f;
    output[(4 + class_b) * N + 1] = score_b;
    rc = yolo11_decode(output, shape, 3, &t, .2f, .45f, list, 8);
    if (out_first && list->count > 0) *out_first = list->items[0].class_id;
    if (out_second && list->count > 1) *out_second = list->items[1].class_id;
    return rc;
}

/* 의자와 식탁은 서로 겹쳐 있는 것이 정상입니다(의자가 식탁 앞에 있음). 클래스가 다른 박스를
 * NMS가 지우면 한쪽이 통째로 사라져 slot_monitor·animal_on_chair 가 근거를 잃습니다. */
static void test_nms_keeps_overlapping_boxes_of_different_classes(void) {
    DetectionList list;
    int first = -1, second = -1;
    ASSERT_INT_EQ(detection_list_init(&list, 8), 0);

    /* 다른 클래스(chair=5, dining table=6): 둘 다 남아야 합니다. */
    ASSERT_INT_EQ(decode_two_boxes(5, 0.9f, 6, 0.8f, &list, &first, &second), 0);
    EXPECT_INT_EQ((int)list.count, 2);
    EXPECT_INT_EQ(first, OBJ_CHAIR);
    EXPECT_INT_EQ(second, OBJ_DININGTABLE);

    /* 같은 클래스(둘 다 chair): 중복이므로 점수 높은 것 하나만 남습니다. */
    ASSERT_INT_EQ(decode_two_boxes(5, 0.9f, 5, 0.8f, &list, &first, &second), 0);
    EXPECT_INT_EQ((int)list.count, 1);
    EXPECT_INT_EQ(first, OBJ_CHAIR);

    /* 고양이와 의자(animal_on_chair 의 전형적 배치)도 둘 다 남아야 합니다. */
    ASSERT_INT_EQ(decode_two_boxes(0, 0.9f, 5, 0.7f, &list, &first, &second), 0);
    EXPECT_INT_EQ((int)list.count, 2);

    detection_list_destroy(&list);
}

/* ── 영상 시간 / 추론 캐시 ─────────────────────────────────────────────── */

/* 영상 시간: 프레임 번호/fps + 오프셋. 모르는 fps 는 -1 로 알려 호출자가 벽시계로 대체하게 한다. */
static void test_platform_video_seconds(void) {
    EXPECT_FLOAT_NEAR((float)platform_video_seconds(0, 3.0), 1000.0f, 1e-4f);
    EXPECT_FLOAT_NEAR((float)platform_video_seconds(30, 3.0), 1010.0f, 1e-4f);
    /* 간격은 오프셋과 무관해야 룰의 유지 시간 계산이 영상 길이를 따른다(3fps 에서 한 프레임 = 1/3 초). */
    EXPECT_FLOAT_NEAR((float)(platform_video_seconds(4, 3.0) - platform_video_seconds(3, 3.0)),
                      0.33333f, 1e-4f);
    EXPECT_TRUE(platform_video_seconds(5, 0.0) < 0.0);
    EXPECT_TRUE(platform_video_seconds(-1, 30.0) < 0.0);
    /* 0초로 시작하지 않아야 "시각 0 = 미설정" 표식과 겹치지 않는다. */
    EXPECT_TRUE(platform_video_seconds(0, 30.0) > 0.0);
}

enum { IC_W = 64, IC_H = 48 };

static void ic_frame(uint8_t *rgb, uint8_t seed) {
    int i;
    for (i = 0; i < IC_W * IC_H * 3; ++i) rgb[i] = (uint8_t)(i * 7 + seed);
}

/* memset 후 채워 패딩까지 결정적으로 만든다(캐시는 구조체를 그대로 저장하므로 memcmp 로 비교 가능). */
static void ic_fill(DetectionList *l, int n, float base) {
    int i, k;
    l->count = (size_t)n;
    for (i = 0; i < n; ++i) {
        Detection *d = &l->items[i];
        memset(d, 0, sizeof(*d));
        d->x1 = base + (float)i; d->y1 = base * 2.0f; d->x2 = base + 50.0f; d->y2 = base + 90.0f;
        d->score = 0.9f - 0.1f * (float)i;
        d->class_id = i % 3;
        d->keypoint_count = YOLO11_NUM_KEYPOINTS;
        for (k = 0; k < YOLO11_NUM_KEYPOINTS; ++k) {
            d->kp[k].x = base + (float)k; d->kp[k].y = base - (float)k; d->kp[k].score = 0.5f;
        }
    }
}

static InferCacheIdentity ic_identity(void) {
    InferCacheIdentity id;
    memset(&id, 0, sizeof(id));
    id.model_bytes = 1234; id.max_candidates = 128; id.max_detections = 100;
    id.confidence = 0.25f; id.iou = 0.45f; id.input_width = 416; id.input_height = 224;
    return id;
}

/* 기록한 호출을 같은 순서·같은 프레임으로 재생하면 같은 검출(키포인트 포함)을 돌려주고, 기록이 끝나면 miss. */
static void test_infer_cache_record_then_replay(void) {
    const char *path = "infer_cache_unit_test.bin";
    InferCacheIdentity id = ic_identity();
    uint8_t f0[IC_W * IC_H * 3], f1[IC_W * IC_H * 3];
    DetectionList in, out;
    Detection saved[2];
    char err[160] = {0};
    InferCache *c;
    ASSERT_INT_EQ(detection_list_init(&in, 128), 0);
    ASSERT_INT_EQ(detection_list_init(&out, 128), 0);
    ic_frame(f0, 1); ic_frame(f1, 2);
    remove(path);

    c = infer_cache_open(path, &id, err, sizeof(err));
    ASSERT_TRUE(c != NULL);
    EXPECT_INT_EQ((int)infer_cache_mode(c), (int)INFER_CACHE_RECORD);
    EXPECT_INT_EQ(infer_cache_lookup(c, f0, IC_W, IC_H, IC_W * 3, 0.25f, &out), -1); /* RECORD 에서 lookup 은 오류 */
    ic_fill(&in, 2, 10.0f);
    memcpy(saved, in.items, sizeof(saved));
    ASSERT_INT_EQ(infer_cache_store(c, f0, IC_W, IC_H, IC_W * 3, 0.25f, &in), 0);
    ic_fill(&in, 0, 0.0f);   /* 검출 0개인 호출도 순번을 차지한다 */
    ASSERT_INT_EQ(infer_cache_store(c, f1, IC_W, IC_H, IC_W * 3, 0.25f, &in), 0);
    infer_cache_close(c);

    c = infer_cache_open(path, &id, err, sizeof(err));
    ASSERT_TRUE(c != NULL);
    EXPECT_INT_EQ((int)infer_cache_mode(c), (int)INFER_CACHE_REPLAY);
    out.count = 99;
    EXPECT_INT_EQ(infer_cache_lookup(c, f0, IC_W, IC_H, IC_W * 3, 0.25f, &out), 1);
    EXPECT_INT_EQ((int)out.count, 2);
    EXPECT_INT_EQ(memcmp(out.items, saved, sizeof(saved)), 0);
    EXPECT_INT_EQ(infer_cache_lookup(c, f1, IC_W, IC_H, IC_W * 3, 0.25f, &out), 1);
    EXPECT_INT_EQ((int)out.count, 0);
    EXPECT_INT_EQ(infer_cache_lookup(c, f1, IC_W, IC_H, IC_W * 3, 0.25f, &out), 0); /* 기록 끝 */
    EXPECT_INT_EQ(infer_cache_lookup(c, f0, IC_W, IC_H, IC_W * 3, 0.25f, &out), 0); /* 이후는 계속 miss */
    infer_cache_close(c);

    remove(path);
    detection_list_destroy(&in);
    detection_list_destroy(&out);
}

/* 틀린 결과를 조용히 재생하면 안 된다: 프레임·신뢰도가 다르면 miss 이고, 한 번 miss 가 나면 이후도 miss. */
static void test_infer_cache_mismatch_is_a_miss(void) {
    const char *path = "infer_cache_unit_test.bin";
    InferCacheIdentity id = ic_identity();
    uint8_t f0[IC_W * IC_H * 3], f1[IC_W * IC_H * 3];
    DetectionList in, out;
    char err[160] = {0};
    InferCache *c;
    ASSERT_INT_EQ(detection_list_init(&in, 128), 0);
    ASSERT_INT_EQ(detection_list_init(&out, 128), 0);
    ic_frame(f0, 1); ic_frame(f1, 2);
    remove(path);
    c = infer_cache_open(path, &id, err, sizeof(err));
    ASSERT_TRUE(c != NULL);
    ic_fill(&in, 1, 5.0f);
    ASSERT_INT_EQ(infer_cache_store(c, f0, IC_W, IC_H, IC_W * 3, 0.25f, &in), 0);
    ASSERT_INT_EQ(infer_cache_store(c, f0, IC_W, IC_H, IC_W * 3, 0.25f, &in), 0);
    infer_cache_close(c);

    /* 다른 프레임 */
    c = infer_cache_open(path, &id, err, sizeof(err));
    ASSERT_TRUE(c != NULL);
    EXPECT_INT_EQ(infer_cache_lookup(c, f1, IC_W, IC_H, IC_W * 3, 0.25f, &out), 0);
    EXPECT_INT_EQ(infer_cache_lookup(c, f0, IC_W, IC_H, IC_W * 3, 0.25f, &out), 0); /* 맞는 프레임이어도 이미 어긋남 */
    infer_cache_close(c);

    /* 같은 프레임이라도 생성 뒤 바뀐 신뢰도(detector_set_confidence)는 다른 결과이므로 miss */
    c = infer_cache_open(path, &id, err, sizeof(err));
    ASSERT_TRUE(c != NULL);
    EXPECT_INT_EQ(infer_cache_lookup(c, f0, IC_W, IC_H, IC_W * 3, 0.10f, &out), 0);
    infer_cache_close(c);

    /* 해상도가 다른 입력 */
    c = infer_cache_open(path, &id, err, sizeof(err));
    ASSERT_TRUE(c != NULL);
    EXPECT_INT_EQ(infer_cache_lookup(c, f0, IC_W / 2, IC_H, IC_W * 3, 0.25f, &out), 0);
    infer_cache_close(c);

    remove(path);
    detection_list_destroy(&in);
    detection_list_destroy(&out);
}

/* 다른 모델·설정의 캐시, 우리 것이 아닌 파일, 중단된 파일, 용량 초과를 각각 안전하게 처리한다. */
static void test_infer_cache_refuses_unsafe_files(void) {
    const char *path = "infer_cache_unit_test.bin";
    InferCacheIdentity id = ic_identity(), other;
    uint8_t f0[IC_W * IC_H * 3];
    DetectionList in, out, small;
    char err[160] = {0};
    InferCache *c;
    FILE *fp;
    char buf[8] = {0};
    uint32_t zero = 0;
    ASSERT_INT_EQ(detection_list_init(&in, 128), 0);
    ASSERT_INT_EQ(detection_list_init(&out, 128), 0);
    ASSERT_INT_EQ(detection_list_init(&small, 2), 0);
    ic_frame(f0, 1);
    remove(path);

    c = infer_cache_open(path, &id, err, sizeof(err));
    ASSERT_TRUE(c != NULL);
    ic_fill(&in, 5, 3.0f);
    ASSERT_INT_EQ(infer_cache_store(c, f0, IC_W, IC_H, IC_W * 3, 0.25f, &in), 0);
    infer_cache_close(c);

    /* 모델 크기가 다르면(=다른 모델) 재생하지 않고 거부한다. 파일은 그대로 둔다. */
    other = id; other.model_bytes = 9999;
    EXPECT_TRUE(infer_cache_open(path, &other, err, sizeof(err)) == NULL);
    EXPECT_TRUE(err[0] != 0);
    other = id; other.confidence = 0.5f;
    EXPECT_TRUE(infer_cache_open(path, &other, err, sizeof(err)) == NULL);

    /* 기록이 out 의 capacity(2)보다 많으면 오류(-1): 잘라서 돌려주면 룰 입력이 달라진다. */
    c = infer_cache_open(path, &id, err, sizeof(err));
    ASSERT_TRUE(c != NULL);
    EXPECT_INT_EQ((int)infer_cache_mode(c), (int)INFER_CACHE_REPLAY);
    EXPECT_INT_EQ(infer_cache_lookup(c, f0, IC_W, IC_H, IC_W * 3, 0.25f, &small), -1);
    infer_cache_close(c);

    /* 정상 종료되지 않은 파일(complete=0): 신뢰하지 않고 처음부터 다시 기록한다. complete 는 오프셋 12. */
    fp = fopen(path, "rb+");
    ASSERT_TRUE(fp != NULL);
    ASSERT_INT_EQ(fseek(fp, 12, SEEK_SET), 0);
    ASSERT_INT_EQ((int)fwrite(&zero, sizeof(zero), 1, fp), 1);
    fclose(fp);
    c = infer_cache_open(path, &id, err, sizeof(err));
    ASSERT_TRUE(c != NULL);
    EXPECT_INT_EQ((int)infer_cache_mode(c), (int)INFER_CACHE_RECORD);
    infer_cache_close(c);   /* 이번에는 정상 종료 */
    c = infer_cache_open(path, &id, err, sizeof(err));
    ASSERT_TRUE(c != NULL);
    EXPECT_INT_EQ((int)infer_cache_mode(c), (int)INFER_CACHE_REPLAY);
    infer_cache_close(c);

    /* 우리 파일이 아니면 덮어쓰지 않는다. */
    remove(path);
    fp = fopen(path, "wb");
    ASSERT_TRUE(fp != NULL);
    fputs("hello!!", fp);
    fclose(fp);
    EXPECT_TRUE(infer_cache_open(path, &id, err, sizeof(err)) == NULL);
    fp = fopen(path, "rb");
    ASSERT_TRUE(fp != NULL);
    EXPECT_TRUE(fread(buf, 1, 7, fp) == 7);
    fclose(fp);
    EXPECT_INT_EQ(strcmp(buf, "hello!!"), 0);

    remove(path);
    detection_list_destroy(&in);
    detection_list_destroy(&out);
    detection_list_destroy(&small);
}

static void test_draw_tier2_confidence_and_masks(void) {
    enum { W = 160, H = 100, BYTES = W*H*3 };
    uint8_t first[BYTES], second[BYTES], blank[BYTES];
    Detection d;
    DetectionList list;
    memset(&d, 0, sizeof(d));
    ASSERT_INT_EQ(detection_list_init(&list, 1), 0);
    d.x1 = 10; d.y1 = 30; d.x2 = 120; d.y2 = 90;
    d.class_id = OBJ_CHAIR; d.score = .47f;
    list.items[0] = d; list.count = 1;
    memset(first, 0, sizeof(first));
    memset(second, 0, sizeof(second));
    memset(blank, 0, sizeof(blank));
    draw_obj_detections(first, W, H, W*3, &list, OBJ_VIS_FURNITURE);
    list.items[0].score = .87f;
    draw_obj_detections(second, W, H, W*3, &list, OBJ_VIS_FURNITURE);
    EXPECT_INT_EQ(memcmp(first, second, sizeof(first)) != 0, 1);
    EXPECT_INT_EQ(memcmp(first, blank, sizeof(first)) != 0, 1);
    memset(first, 0, sizeof(first));
    draw_obj_detections(first, W, H, W*3, &list, OBJ_VIS_FOOD);
    EXPECT_INT_EQ(memcmp(first, blank, sizeof(first)), 0);
    list.items[0].class_id = OBJ_DININGTABLE;
    draw_obj_detections(first, W, H, W*3, &list, OBJ_VIS_FURNITURE);
    EXPECT_INT_EQ(memcmp(first, blank, sizeof(first)) != 0, 1);
    detection_list_destroy(&list);
}

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

    /* 외부 음료는 사람이 있어야 성립합니다(rules.c). 사람 없이 병만 있을 때는 아래 별도 테스트. */
    tl.count = 1; tl.items[0].id = 1; tl.items[0].active = 1;

    make_obj_list(&objs, 100, 100, 150, 200, 0.8f, 2 /* OBJ_BOTTLE */);
    rules_evaluate_objects(&re, &objs, &tl, 100.0, &elog);

    /* confirm 필터가 꺼진 설정(count=0)이므로 관측 즉시 drink 가 확정되어야 합니다. */
    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_DRINK].latched, 1);

    /* 감지 없어지면 latch 해제 */
    objs.count = 0;
    rules_evaluate_objects(&re, &objs, &tl, 101.0, &elog);
    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_DRINK].latched, 0);

    rules_destroy(&re);
    tracks_destroy(&tl);
    detection_list_destroy(&objs);
    event_log_close(&elog);
}

/* 사람이 없으면 병·음식이 보여도 발화하지 않습니다(진열 병이 무인 시간대에 오탐되던 문제). */
static void test_rules_obj_external_requires_person(void) {
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

    make_obj_list(&objs, 100, 100, 150, 200, 0.8f, OBJ_BOTTLE);
    rules_evaluate_objects(&re, &objs, &tl, 100.0, &elog);   /* 사람 0명 */
    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_DRINK].latched, 0);
    EXPECT_INT_EQ(count_events(&elog, "external_drink"), 0);

    make_obj_list(&objs, 50, 50, 100, 100, 0.75f, OBJ_FOOD_FIRST);
    rules_evaluate_objects(&re, &objs, &tl, 101.0, &elog);
    EXPECT_INT_EQ(count_events(&elog, "external_food"), 0);

    tl.count = 1; tl.items[0].id = 1; tl.items[0].active = 1;  /* 사람 등장 */
    make_obj_list(&objs, 100, 100, 150, 200, 0.8f, OBJ_BOTTLE);
    rules_evaluate_objects(&re, &objs, &tl, 102.0, &elog);
    EXPECT_INT_EQ(count_events(&elog, "external_drink"), 1);

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

    tl.count = 1; tl.items[0].id = 1; tl.items[0].active = 1; /* 사람이 있어야 성립 */
    make_obj_list(&objs, 50, 50, 100, 100, 0.75f, 11 /* pizza, OBJ_FOOD_FIRST+7 */);
    rules_evaluate_objects(&re, &objs, &tl, 100.0, &elog);

    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_FOOD].latched, 1);

    rules_destroy(&re);
    tracks_destroy(&tl);
    detection_list_destroy(&objs);
    event_log_close(&elog);
}

/* 큰 의자 위 작은 고양이: IoU 는 0.01 이라 IoU 만으로는 발화하지 않았다. 포함률(1.0)로 발화해야 한다.
 * 의자 옆에 붙어 선 동물(겹침이 동물 면적의 10%)은 여전히 발화하지 않아야 한다. */
static int animal_chair_case(float ax1, float ay1, float ax2, float ay2) {
    TrackList tl; RulesEngine re; EventLog elog; DetectionList objs; char error[128] = {0}; int latched;
    RulesConfig rcfg = {3600.0, 300.0, 5.0, 1.8f, 2.2f, 0, 0, 0, 0, 0, 0.15f, 1};
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error));
    rules_init(&re, 16, &rcfg, error, sizeof(error));
    detection_list_init(&objs, 8);
    objs.count = 2;
    memset(objs.items, 0, 2 * sizeof(objs.items[0]));
    objs.items[0].x2 = 200; objs.items[0].y2 = 200; objs.items[0].score = .9f; objs.items[0].class_id = 14;
    objs.items[1].x1 = ax1; objs.items[1].y1 = ay1; objs.items[1].x2 = ax2; objs.items[1].y2 = ay2;
    objs.items[1].score = .85f; objs.items[1].class_id = 0;
    rules_evaluate_objects(&re, &objs, &tl, 100.0, &elog);
    latched = re.obj_rule[OBJ_RULE_ANIMAL_CHAIR].latched;
    rules_destroy(&re); tracks_destroy(&tl); detection_list_destroy(&objs); event_log_close(&elog);
    return latched;
}

static void test_rules_animal_containment(void) {
    EXPECT_INT_EQ(animal_chair_case(90, 90, 110, 110), 1);   /* 작은 고양이가 의자 박스 안 */
    EXPECT_INT_EQ(animal_chair_case(190, 0, 290, 100), 0);   /* 옆에 선 동물: 겹침 10% */
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

    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_ANIMAL_CHAIR].latched, 1);

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

    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_ANIMAL_TABLE].latched, 1);

    rules_destroy(&re);
    tracks_destroy(&tl);
    detection_list_destroy(&objs);
    event_log_close(&elog);
}

/* 동물만 있고 가구가 없어도 animal_present 가 확정되고, 동물이 없으면 확정되지 않는다 */
static void test_rules_obj_animal_present(void) {
    TrackList tl; RulesEngine re; EventLog elog; DetectionList objs;
    RulesConfig rcfg = {3600.0, 300.0, 5.0, 1.8f, 2.2f, 0, 0, 0, 0, 0, 0.10f, 1};
    char error[128] = {0};
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);
    ASSERT_INT_EQ(detection_list_init(&objs, 8), 0);
    objs.count = 0;
    rules_evaluate_objects(&re, &objs, &tl, 100.0, &elog);
    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_ANIMAL_ANY].latched, 0);
    objs.count = 1;
    memset(&objs.items[0], 0, sizeof(objs.items[0]));
    objs.items[0].x2 = 50; objs.items[0].y2 = 50; objs.items[0].score = 0.5f;
    objs.items[0].class_id = 0; /* OBJ_CAT */
    rules_evaluate_objects(&re, &objs, &tl, 101.0, &elog);
    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_ANIMAL_ANY].latched, 1);
    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_ANIMAL_TABLE].latched, 0);
    rules_destroy(&re); tracks_destroy(&tl); detection_list_destroy(&objs); event_log_close(&elog);
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

/* 단발 오검출은 확정 필터(3-of-5)에서 걸러지고, 반복 관측은 1회만 발화한 뒤
 * 5번 연속 안 보여야 해제되는지 확인합니다. */
static void test_rules_obj_confirm_filter(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    DetectionList objs;
    RulesConfig rcfg;
    char error[128] = {0};
    int before, i;
    double now = 100.0;
    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.animal_iou_threshold = 0.15f; rcfg.no_cup_margin = 1;
    rcfg.object_confirm_count = 3; rcfg.object_confirm_window = 5;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);
    ASSERT_INT_EQ(detection_list_init(&objs, 8), 0);

    tl.count = 1; tl.items[0].id = 1; tl.items[0].active = 1; /* 사람이 있어야 성립 */

    /* 한 번 보이고 사라지는 병: 발화하면 안 됩니다. */
    before = event_log_count(&elog, LOG_WARN);
    make_obj_list(&objs, 100, 100, 150, 200, 0.8f, OBJ_BOTTLE);
    rules_evaluate_objects(&re, &objs, &tl, now++, &elog);
    objs.count = 0;
    for (i = 0; i < 5; ++i) rules_evaluate_objects(&re, &objs, &tl, now++, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before);
    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_DRINK].latched, 0);

    /* 보임·보임·가려짐·보임 = 5번 중 3번 → 확정. 한 번의 누락이 리셋하지 않아야 합니다. */
    make_obj_list(&objs, 100, 100, 150, 200, 0.8f, OBJ_BOTTLE);
    rules_evaluate_objects(&re, &objs, &tl, now++, &elog);
    rules_evaluate_objects(&re, &objs, &tl, now++, &elog);
    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_DRINK].latched, 0);
    objs.count = 0;
    rules_evaluate_objects(&re, &objs, &tl, now++, &elog);
    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_DRINK].latched, 0);
    make_obj_list(&objs, 100, 100, 150, 200, 0.8f, OBJ_BOTTLE);
    before = event_log_count(&elog, LOG_WARN);
    rules_evaluate_objects(&re, &objs, &tl, now++, &elog);
    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_DRINK].latched, 1);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before + 1);

    /* 계속 보여도 중복 발화 없음 */
    rules_evaluate_objects(&re, &objs, &tl, now++, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before + 1);

    /* 4번 안 보임 → 아직 래치 유지, 5번째 → 해제 */
    objs.count = 0;
    for (i = 0; i < 4; ++i) rules_evaluate_objects(&re, &objs, &tl, now++, &elog);
    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_DRINK].latched, 1);
    rules_evaluate_objects(&re, &objs, &tl, now++, &elog);
    EXPECT_INT_EQ(re.obj_rule[OBJ_RULE_DRINK].latched, 0);

    rules_destroy(&re);
    tracks_destroy(&tl);
    detection_list_destroy(&objs);
    event_log_close(&elog);
}

/* 인원 초과: hold 시간 유지돼야 발화, 한 번만 발화, 줄었다가 다시 초과하면 재발화 */
static void test_rules_occupancy_exceeded(void) {
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig rcfg;
    char error[128] = {0};
    int before;
    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.dwell_limit_seconds = 3600.0; rcfg.unordered_grace_seconds = 300.0;
    rcfg.fall_hold_seconds = 5.0;
    rcfg.fall_aspect_ratio_kp = 1.8f; rcfg.fall_aspect_ratio_nokp = 2.2f;
    rcfg.max_occupancy = 2; rcfg.max_occupancy_hold_seconds = 10.0;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);

    tl.count = 3;
    memset(tl.items, 0, sizeof(tl.items[0]) * 3);
    tl.items[0].id = 1; tl.items[0].active = 1;
    tl.items[1].id = 2; tl.items[1].active = 1;
    tl.items[2].id = 3; tl.items[2].active = 1;

    before = event_log_count(&elog, LOG_WARN);
    rules_evaluate(&re, &tl, 100.0, &elog);
    rules_evaluate(&re, &tl, 109.0, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before);       /* hold 미달 */
    rules_evaluate(&re, &tl, 110.0, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before + 1);   /* 확정 */
    rules_evaluate(&re, &tl, 120.0, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before + 1);   /* 중복 없음 */

    tl.items[2].active = 0;                                         /* 2명으로 감소 → 해제 */
    rules_evaluate(&re, &tl, 125.0, &elog);
    EXPECT_INT_EQ(re.occ_latched, 0);
    tl.items[2].active = 1;                                         /* 다시 초과 → hold 부터 */
    rules_evaluate(&re, &tl, 130.0, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before + 1);
    rules_evaluate(&re, &tl, 140.0, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before + 2);

    /* 0 이면 비활성 */
    rcfg.max_occupancy = 0;
    rules_update_config(&re, &rcfg);
    rules_evaluate(&re, &tl, 200.0, &elog);
    rules_evaluate(&re, &tl, 300.0, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before + 2);

    rules_destroy(&re);
    tracks_destroy(&tl);
    event_log_close(&elog);
}

/* 서 있는 사람의 장시간 무동작: 정지 시 발화, 움직이면 해제 후 다시 세기,
 * 앉은 자세(무릎이 엉덩이와 같은 높이)는 대상에서 제외 */
static void set_standing_pose(Track *t, float knee_y, float dx) {
    memset(&t->box, 0, sizeof(t->box));
    t->box.x1 = 300 + dx; t->box.y1 = 0; t->box.x2 = 950 + dx; t->box.y2 = 700;
    t->box.score = 0.9f; t->box.keypoint_count = 17;
    t->box.kp[0]  = (Keypoint){625 + dx, 100, 0.9f};
    t->box.kp[5]  = (Keypoint){450 + dx, 250, 0.9f};
    t->box.kp[6]  = (Keypoint){800 + dx, 250, 0.9f};
    t->box.kp[11] = (Keypoint){480 + dx, 400, 0.9f};
    t->box.kp[12] = (Keypoint){770 + dx, 400, 0.9f};
    t->box.kp[13] = (Keypoint){490 + dx, knee_y, 0.9f};
    t->box.kp[14] = (Keypoint){760 + dx, knee_y, 0.9f};
}

static void test_rules_motionless_standing(void) {
    RulesEngine re; Track t; TrackList tl; EventLog elog;
    RulesConfig rcfg;
    int before; double now;
    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.dwell_limit_seconds = 3600.0; rcfg.unordered_grace_seconds = 300.0;
    rcfg.fall_hold_seconds = 5.0;
    rcfg.fall_aspect_ratio_kp = 1.8f; rcfg.fall_aspect_ratio_nokp = 2.2f;
    rcfg.still_seconds = 60.0; rcfg.still_motion_threshold = 0.10f;
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(rules_init(&re, 8, &rcfg, NULL, 0), 0);
    memset(&t, 0, sizeof(t)); memset(&tl, 0, sizeof(tl));
    tl.items = &t; tl.count = 1; t.id = 1; t.active = 1; t.match_score = 0.9f;

    /* 정지 서 있음: 60초 유지 시 1회 발화 */
    set_standing_pose(&t, 600, 0);
    before = event_log_count(&elog, LOG_WARN);
    for (now = 100; now < 160; now += 10) { t.last_seen = now; rules_evaluate(&re, &tl, now, &elog); }
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before);
    t.last_seen = 160; rules_evaluate(&re, &tl, 160, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before + 1);
    t.last_seen = 170; rules_evaluate(&re, &tl, 170, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before + 1);   /* 중복 없음 */

    /* 박스 폭(650)의 10% 초과 이동 → 해제, 새 기준으로 다시 60초 */
    set_standing_pose(&t, 600, 100);
    t.last_seen = 180; rules_evaluate(&re, &tl, 180, &elog);
    t.last_seen = 230; rules_evaluate(&re, &tl, 230, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before + 1);
    t.last_seen = 240; rules_evaluate(&re, &tl, 240, &elog);
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before + 2);

    /* 앉은 자세(무릎 y == 엉덩이 y): 아무리 오래 가만있어도 대상 아님 */
    set_standing_pose(&t, 410, 0);
    before = event_log_count(&elog, LOG_WARN);
    for (now = 300; now <= 900; now += 10) { t.last_seen = now; rules_evaluate(&re, &tl, now, &elog); }
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before);

    /* 0 이면 비활성 */
    rcfg.still_seconds = 0.0;
    rules_update_config(&re, &rcfg);
    set_standing_pose(&t, 600, 0);
    before = event_log_count(&elog, LOG_WARN);
    for (now = 1000; now <= 1400; now += 10) { t.last_seen = now; rules_evaluate(&re, &tl, now, &elog); }
    EXPECT_INT_EQ(event_log_count(&elog, LOG_WARN), before);

    rules_destroy(&re);
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

/* ── 쓰러짐: 화면 경계에 잘린 박스 ────────────────────────────────────────── */
int is_horizontal_pose_for_test(const Detection *box, float ratio_kp, float ratio_nokp,
                                int frame_w, int frame_h);


/* 앉아 있는 사람: 코가 어깨보다 위, 어깨 너비 60px, 코는 그보다 40px 위. */
static void seated_pose(Detection *d, float x1, float y1, float x2, float y2) {
    int k;
    memset(d, 0, sizeof(*d));
    d->x1 = x1; d->y1 = y1; d->x2 = x2; d->y2 = y2;
    d->score = 0.81f;
    d->keypoint_count = YOLO11_NUM_KEYPOINTS;
    for (k = 0; k < YOLO11_NUM_KEYPOINTS; ++k) d->kp[k].score = 0.0f;
    d->kp[0].x = 600; d->kp[0].y = 460; d->kp[0].score = 0.9f;  /* 코 */
    d->kp[5].x = 570; d->kp[5].y = 500; d->kp[5].score = 0.9f;  /* 왼 어깨 */
    d->kp[6].x = 630; d->kp[6].y = 500; d->kp[6].score = 0.9f;  /* 오른 어깨 */
    /* 엉덩이(11,12)는 화면 밖 — score 0 으로 남겨 둡니다 */
}

static void test_fall_ignores_clipped_bbox_when_upright(void) {
    /* 실측 재현: 카메라 가까이 앉아 팔을 뻗은 사람의 박스가 1045x470(비율 2.22)으로 잡히고
       다리가 화면 밖이라 엉덩이 관절이 없어, nokp 폴백(임계 2.2)을 간발의 차로 넘겨
       person_fallen 이 발화했습니다. 스켈레톤은 명백히 앉은 자세였습니다. */
    Detection d;
    seated_pose(&d, 115.0f, 340.0f, 1160.0f, 810.0f);   /* 1045x470, 아래가 프레임 끝 */

    /* 프레임 크기를 모르면(0) 잘림을 알 수 없어 예전처럼 오탐합니다 */
    EXPECT_INT_EQ(is_horizontal_pose_for_test(&d, 1.8f, 2.2f, 0, 0), 1);
    /* 프레임 크기를 알면 잘린 박스로 보고 상체 자세로 판단 → 쓰러짐 아님 */
    EXPECT_INT_EQ(is_horizontal_pose_for_test(&d, 1.8f, 2.2f, 1280, 810), 0);
}

static void test_fall_still_fires_on_clipped_horizontal_body(void) {
    /* 잘렸더라도 상체가 실제로 수평이면(코와 어깨의 y 가 비슷) 발화해야 합니다.
       잘림 가드가 쓰러짐을 통째로 막아 버리면 안 됩니다. */
    Detection d;
    seated_pose(&d, 115.0f, 340.0f, 1160.0f, 810.0f);
    d.kp[0].y = 498.0f;   /* 코가 어깨와 거의 같은 높이 — 누운 자세 */
    EXPECT_INT_EQ(is_horizontal_pose_for_test(&d, 1.8f, 2.2f, 1280, 810), 1);

    /* 관절이 부족하면 판단하지 않습니다 — 응급 알림은 틀리면 신뢰를 잃습니다 */
    d.kp[0].score = 0.0f;
    EXPECT_INT_EQ(is_horizontal_pose_for_test(&d, 1.8f, 2.2f, 1280, 810), 0);
}

static void test_fall_unclipped_path_unchanged(void) {
    /* 화면 안쪽에 온전히 들어온 박스는 기존 경로를 그대로 타야 합니다. */
    Detection d;
    seated_pose(&d, 300.0f, 200.0f, 500.0f, 600.0f);    /* 세로가 긴 정상 박스 */
    EXPECT_INT_EQ(is_horizontal_pose_for_test(&d, 1.8f, 2.2f, 1280, 720), 0);
}

/* ── 카메라 재연결 · 쓰러짐 캡처 · 흔들림 안정화 ────────────────────────── */

static void test_reconnect_backoff(void) {
    EXPECT_FLOAT_NEAR((float)reconnect_backoff_seconds(0, 1.0, 30.0), 1.0f, 1e-6f);
    EXPECT_FLOAT_NEAR((float)reconnect_backoff_seconds(1, 1.0, 30.0), 2.0f, 1e-6f);
    EXPECT_FLOAT_NEAR((float)reconnect_backoff_seconds(4, 1.0, 30.0), 16.0f, 1e-6f);
    EXPECT_FLOAT_NEAR((float)reconnect_backoff_seconds(5, 1.0, 30.0), 30.0f, 1e-6f);   /* 32 → 상한 */
    EXPECT_FLOAT_NEAR((float)reconnect_backoff_seconds(100000, 1.0, 30.0), 30.0f, 1e-6f); /* 오버플로 없음 */
    EXPECT_FLOAT_NEAR((float)reconnect_backoff_seconds(3, 1.0, 0.5), 1.0f, 1e-6f);   /* 상한<base → base */
}

static void test_reconnect_stall_boundary(void) {
    /* 첫 프레임 이후: 정확히 5.0초에서 stall */
    EXPECT_INT_EQ(reconnect_stalled(104.99, 100.0, 1, 5.0, 10.0), 0);
    EXPECT_INT_EQ(reconnect_stalled(105.0, 100.0, 1, 5.0, 10.0), 1);
    /* 첫 프레임 전에는 유예 10초 — 카메라 그래프 시작 지연을 끊김으로 오판하지 않음 */
    EXPECT_INT_EQ(reconnect_stalled(107.0, 100.0, 0, 5.0, 10.0), 0);
    EXPECT_INT_EQ(reconnect_stalled(110.0, 100.0, 0, 5.0, 10.0), 1);
    EXPECT_INT_EQ(reconnect_stalled(115.0, 100.0, 0, 20.0, 10.0), 0);
}

static void test_tracks_shift_time_after_camera_gap(void) {
    /* 끊긴 30초가 소실·limbo 판정에 섞이지 않게 시간 기준이 함께 밀려야 합니다. */
    TrackList tl;
    char error[128] = {0};
    ASSERT_INT_EQ(tracks_init(&tl, 8, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    tl.count = 2;
    tl.items[0].id = 1;
    tl.items[0].active = 0;                 /* limbo 트랙 */
    tl.items[0].last_seen = 100.0;
    tl.items[0].limbo_expired_at = 200.0;
    tl.items[0].head_y_prev_time = 99.0;
    tl.items[1].id = 0;                     /* 미사용 슬롯 — 건드리지 않음 */
    tl.items[1].last_seen = 50.0;
    tracks_shift_time(&tl, 30.0);
    EXPECT_FLOAT_NEAR((float)tl.items[0].last_seen, 130.0f, 1e-4f);
    EXPECT_FLOAT_NEAR((float)tl.items[0].limbo_expired_at, 230.0f, 1e-4f);
    EXPECT_FLOAT_NEAR((float)tl.items[0].head_y_prev_time, 129.0f, 1e-4f);
    EXPECT_FLOAT_NEAR((float)tl.items[1].last_seen, 50.0f, 1e-4f);
    tracks_destroy(&tl);
}

static void test_rules_fall_once_and_gap_shift(void) {
    /* 발화한 호출에서만 fall_fired 에 담기고(캡처 1회), 카메라 공백은 유지 시간에서 빠져야 합니다. */
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig rcfg = {3600.0, 300.0, 5.0, 1.8f, 2.2f, 0, 0, 0, 0, 0};
    char error[128] = {0};
    event_log_open(&elog, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(tracks_init(&tl, 16, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(rules_init(&re, 16, &rcfg, error, sizeof(error)), 0);
    tl.count = 1;
    tl.items[0].id = 7;
    tl.items[0].active = 1;
    fall_seed_upright(&re,&tl,&elog);
    fall_observe(&re,&tl,&elog,100);
    EXPECT_INT_EQ(re.fall_fired_count,0);
    rules_shift_time(&re,30);
    /* Missing camera time resets the candidate. Five observed seconds required again. */
    for(int j=0;j<5;j++){fall_observe(&re,&tl,&elog,133+j);EXPECT_INT_EQ(re.fall_fired_count,0);}
    fall_observe(&re,&tl,&elog,138);
    ASSERT_INT_EQ(re.fall_fired_count,1);
    EXPECT_INT_EQ(re.fall_fired_track[0],7);
    EXPECT_FLOAT_NEAR(re.fall_fired_box[0].x2,350.0f,.01f);
    fall_observe(&re,&tl,&elog,139);
    EXPECT_INT_EQ(re.fall_fired_count,0);

    rules_destroy(&re);
    tracks_destroy(&tl);
    event_log_close(&elog);
}

/* ── 보호 모드 화면 모양(privacy_style) ─────────────────────────────────────── */

static int priv_all_gray(const uint8_t *rgb, int w, int h, int stride) {
    for (int y = 0; y < h; ++y) {
        const uint8_t *r = rgb + (size_t)y * stride;
        for (int x = 0; x < w; ++x, r += 3)
            if (r[0] != r[1] || r[1] != r[2]) return 0;
    }
    return 1;
}

static void test_privacy_style_solid_is_default_and_fills(void) {
    enum { W = 40, H = 30 };
    uint8_t frame[W * H * 3];
    PrivacyParams p = privacy_params_default();
    EXPECT_INT_EQ(p.style, PRIVACY_STYLE_SOLID);
    memset(frame, 200, sizeof(frame));
    EXPECT_INT_EQ(privacy_style_apply(NULL, frame, W, H, W * 3, &p), 0); /* 단색은 스크래치가 필요 없다 */
    for (size_t i = 0; i < sizeof(frame); ++i) EXPECT_INT_EQ(frame[i], 0x24);
}

static void test_privacy_style_edge_gray_removes_color_keeps_structure(void) {
    enum { W = 160, H = 96 };
    static uint8_t frame[W * H * 3];
    PrivacyScratch *s = privacy_scratch_create();
    PrivacyParams p = privacy_params_default();
    int edge_found = 0;
    ASSERT_TRUE(s != NULL);
    p.style = PRIVACY_STYLE_EDGE_GRAY;
    p.edge_soft = 0; p.edge_opacity = 100;   /* 선명한 흰 선(예전 동작): 정확한 255 를 검사하는 시험 */
    /* 왼쪽은 밝은 노랑 계열, 오른쪽은 어두운 파랑 계열: 휘도 차가 커서 x=80 에 강한 엣지가 생긴다. */
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t *px = frame + ((size_t)y * W + x) * 3;
            if (x < 80) { px[0] = 250; px[1] = 200; px[2] = 50; }
            else        { px[0] = 10;  px[1] = 10;  px[2] = 200; }
        }
    ASSERT_INT_EQ(privacy_style_apply(s, frame, W, H, W * 3, &p), 0);
    EXPECT_TRUE(priv_all_gray(frame, W, H, W * 3));      /* 색 정보가 하나도 남지 않는다 */
    for (int x = 77; x <= 82; ++x) if (frame[((size_t)(H / 2) * W + x) * 3] == 255) edge_found = 1;
    EXPECT_TRUE(edge_found);                              /* 경계선에 흰 엣지가 그려진다 */
    /* 경계에서 먼 곳은 엣지가 아니라 블러된 배경 휘도(왼쪽 ≈198, 오른쪽 ≈31)다. */
    EXPECT_TRUE(abs((int)frame[((size_t)(H / 2) * W + 10) * 3] - 198) <= 25);
    EXPECT_TRUE(abs((int)frame[((size_t)(H / 2) * W + 150) * 3] - 31) <= 25);
    privacy_scratch_destroy(s);
}

static void test_privacy_style_edge_gray_flattens_fine_texture(void) {
    enum { W = 160, H = 96 };
    static uint8_t frame[W * H * 3];
    PrivacyScratch *s = privacy_scratch_create();
    PrivacyParams p = privacy_params_default();
    int white = 0;
    p.style = PRIVACY_STYLE_EDGE_GRAY;
    p.edge_soft = 0; p.edge_opacity = 100;   /* 선명한 흰 선(예전 동작): 정확한 255 를 검사하는 시험 */
    /* 1px 체커보드: 모자이크와 약한 가우시안이 평균(≈127)으로 만들어 엣지도 배경 변화도 남지 않아야 한다.
       "작은 디테일(글자·얼굴 질감)은 사라진다"는 성질의 회귀 시험. */
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            memset(frame + ((size_t)y * W + x) * 3, ((x + y) & 1) ? 255 : 0, 3);
    ASSERT_INT_EQ(privacy_style_apply(s, frame, W, H, W * 3, &p), 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int v = frame[((size_t)y * W + x) * 3];
            if (v == 255) ++white;
            EXPECT_TRUE(v >= 100 && v <= 155 || v == 255);
        }
    EXPECT_TRUE(white < W * H / 50);                      /* 엣지로 흰 픽셀이 2% 넘게 생기면 질감이 새고 있다 */
    privacy_scratch_destroy(s);
}

static void test_privacy_style_failures_never_leave_original_pixels(void) {
    enum { W = 8, H = 8 };
    uint8_t tiny[W * H * 3];
    uint8_t big[64 * 32 * 3];
    PrivacyScratch *s = privacy_scratch_create();
    PrivacyParams p = privacy_params_default();
    p.style = PRIVACY_STYLE_EDGE_GRAY;
    memset(tiny, 77, sizeof(tiny));
    EXPECT_INT_EQ(privacy_style_apply(s, tiny, W, H, W * 3, &p), -1);   /* 너무 작은 프레임 */
    for (size_t i = 0; i < sizeof(tiny); ++i) EXPECT_INT_EQ(tiny[i], 0x24);
    memset(big, 77, sizeof(big));
    EXPECT_INT_EQ(privacy_style_apply(NULL, big, 64, 32, 64 * 3, &p), -1); /* 스크래치 없음 */
    for (size_t i = 0; i < sizeof(big); ++i) EXPECT_INT_EQ(big[i], 0x24);
    EXPECT_INT_EQ(privacy_style_apply(s, big, 64, 32, 10, &p), -1);     /* stride 오류 */
    privacy_scratch_destroy(s);
    privacy_scratch_destroy(NULL);                                       /* NULL 안전 */
}

static void test_privacy_style_respects_stride_padding_and_sanitizes(void) {
    enum { W = 64, H = 48, STRIDE = W * 3 + 16 };
    static uint8_t buf[STRIDE * H];
    PrivacyScratch *s = privacy_scratch_create();
    PrivacyParams p = privacy_params_default();
    PrivacyParams bad = {99, -5, 1000, 0, 0};
    memset(buf, 0xAB, sizeof(buf));
    p.style = PRIVACY_STYLE_EDGE_GRAY;
    ASSERT_INT_EQ(privacy_style_apply(s, buf, W, H, STRIDE, &p), 0);
    for (int y = 0; y < H; ++y)                          /* 행 끝 패딩 16바이트는 건드리면 안 된다 */
        for (int i = W * 3; i < STRIDE; ++i) EXPECT_INT_EQ(buf[(size_t)y * STRIDE + i], 0xAB);
    privacy_params_sanitize(&bad);
    EXPECT_INT_EQ(bad.style, PRIVACY_STYLE_SOLID);
    EXPECT_INT_EQ(bad.solid_gray, 0x24);
    EXPECT_INT_EQ(bad.cell, 64);
    EXPECT_TRUE(bad.edge_low >= 5 && bad.edge_high >= bad.edge_low);
    privacy_scratch_destroy(s);
}

static void test_capture_privacy_edge_style_writes_gray_copy(void) {
    enum { W = 96, H = 64 };
    EventCapture cap;
    static uint8_t frame[W * H * 3];
    PrivacyParams p = privacy_params_default();
    char path[720];
    char error[256] = {0};
    const char *dir = "test_capture_edge_tmp";
    for (int i = 0; i < W * H; ++i) { frame[i * 3] = 250; frame[i * 3 + 1] = 40; frame[i * 3 + 2] = 40; }
    ASSERT_INT_EQ(capture_init(&cap, dir, 85, error, sizeof(error)), 0);
    p.style = PRIVACY_STYLE_EDGE_GRAY;
    capture_set_privacy_style(&cap, &p);
    ASSERT_INT_EQ(capture_save(&cap, "person_fallen", 5, frame, W, H, W * 3,
                               NULL, 1, path, sizeof(path), error, sizeof(error)), 0);
    EXPECT_TRUE(priv_all_gray(cap.buf, W, H, W * 3));    /* 저장 사본에 빨간색이 남지 않는다 */
    EXPECT_INT_EQ(frame[0], 250);                        /* 원본 프레임은 건드리지 않는다 */
    remove(path);
    capture_destroy(&cap);
    EXPECT_INT_EQ(test_rmdir(dir), 0);
}

static void test_privacy_style_edge_blur_reduces_fine_edges(void) {
    enum { W = 160, H = 96 };
    static uint8_t noisy[W * H * 3], a[W * H * 3], b[W * H * 3];
    PrivacyScratch *s = privacy_scratch_create();
    PrivacyParams p = privacy_params_default();
    int white_weak = 0, white_strong = 0;
    p.style = PRIVACY_STYLE_EDGE_GRAY;
    p.edge_soft = 0; p.edge_opacity = 100;   /* 선명한 흰 선(예전 동작): 정확한 255 를 검사하는 시험 */
    /* 거친 노이즈 위에 굵은 사각형: 사각형 윤곽은 남고 잔 노이즈 엣지는 블러가 셀수록 줄어야 한다. */
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int v = (int)(((unsigned)(x * 2654435761u) ^ (unsigned)(y * 40503u)) >> 3 & 63) + 96;
            if (x > 50 && x < 110 && y > 25 && y < 70) v += 90;
            memset(noisy + ((size_t)y * W + x) * 3, v > 255 ? 255 : v, 3);
        }
    memcpy(a, noisy, sizeof(a)); memcpy(b, noisy, sizeof(b));
    p.edge_blur = 1;
    ASSERT_INT_EQ(privacy_style_apply(s, a, W, H, W * 3, &p), 0);
    p.edge_blur = 6;
    ASSERT_INT_EQ(privacy_style_apply(s, b, W, H, W * 3, &p), 0);
    for (int i = 0; i < W * H; ++i) { if (a[i * 3] == 255) ++white_weak; if (b[i * 3] == 255) ++white_strong; }
    EXPECT_TRUE(white_strong < white_weak);             /* 블러가 셀수록 엣지 픽셀이 줄어든다 */
    EXPECT_TRUE(white_strong > 0);                      /* 굵은 윤곽은 그대로 남는다 */
    privacy_scratch_destroy(s);
}

static void test_privacy_style_soft_edges_are_dimmer_and_wider(void) {
    enum { W = 160, H = 96 };
    static uint8_t src[W * H * 3], hard[W * H * 3], soft[W * H * 3];
    PrivacyScratch *s = privacy_scratch_create();
    PrivacyParams ph = privacy_params_default(), ps = privacy_params_default();
    int peak_hard = 0, peak_soft = 0, wide_hard = 0, wide_soft = 0, row = H / 2;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) memset(src + ((size_t)y * W + x) * 3, x < 80 ? 220 : 20, 3);
    ph.style = ps.style = PRIVACY_STYLE_EDGE_GRAY;
    ph.edge_soft = 0; ph.edge_opacity = 100;              /* 예전 동작: 선명한 흰 선 */
    memcpy(hard, src, sizeof(hard)); memcpy(soft, src, sizeof(soft));
    ASSERT_INT_EQ(privacy_style_apply(s, hard, W, H, W * 3, &ph), 0);
    ASSERT_INT_EQ(privacy_style_apply(s, soft, W, H, W * 3, &ps), 0);   /* 기본값: soft 1, opacity 70 */
    /* 경계 근처(x 60~100)에서 배경(왼쪽 ≈220 쪽은 이미 밝아서 오른쪽 어두운 쪽 x 82~100 으로 비교) */
    for (int x = 70; x < 100; ++x) {
        int h = hard[((size_t)row * W + x) * 3], f = soft[((size_t)row * W + x) * 3];
        if (h > peak_hard) peak_hard = h;
        if (f > peak_soft) peak_soft = f;
    }
    for (int x = 82; x < 100; ++x) {
        int bg = 20;                                       /* 오른쪽 배경 휘도 근처 */
        if (hard[((size_t)row * W + x) * 3] > bg + 25) ++wide_hard;
        if (soft[((size_t)row * W + x) * 3] > bg + 25) ++wide_soft;
    }
    EXPECT_INT_EQ(peak_hard, 255);                         /* 선명한 선은 순백 */
    EXPECT_TRUE(peak_soft < peak_hard);                    /* 덜 선명한 선은 최대 밝기가 낮다 */
    EXPECT_TRUE(peak_soft > 100);                          /* 그래도 선은 보인다 */
    EXPECT_TRUE(wide_soft >= wide_hard);                   /* 번져서 같거나 더 넓게 퍼진다 */
    EXPECT_TRUE(priv_all_gray(soft, W, H, W * 3));
    privacy_scratch_destroy(s);
}

static void test_privacy_style_thermal_colors_depend_only_on_luma(void) {
    enum { W = 160, H = 96 };
    static uint8_t a[W * H * 3], b[W * H * 3];
    PrivacyScratch *s = privacy_scratch_create();
    PrivacyParams p = privacy_params_default();
    int colored = 0, differs_from_gray = 0;
    p.style = PRIVACY_STYLE_THERMAL;
    /* 휘도(77R+150G+29B)가 같도록 맞춘 서로 다른 색 두 장: 왼쪽 밝은 칸 (R,G,B) 대 (G만 강한 색), 오른쪽 어두운 칸.
       열화상 색이 휘도에서만 나온다면 두 결과는 비트 단위로 같아야 한다(원본 색상이 새지 않는다는 증명). */
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t *pa = a + ((size_t)y * W + x) * 3, *pb = b + ((size_t)y * W + x) * 3;
            if (x < 80) { pa[0] = 200; pa[1] = 200; pa[2] = 200;   /* 휘도 200 */
                          pb[0] = 120; pb[1] = 256 - 56; pb[2] = 200; /* 휘도 ≈ 200 근처가 되도록 구성 */
                          { int l = (77 * pb[0] + 150 * pb[1] + 29 * pb[2]) >> 8; int d = 200 - l; pb[1] = (uint8_t)(pb[1] + d * 256 / 150); } }
            else        { pa[0] = pa[1] = pa[2] = 30;
                          pb[0] = 60; pb[1] = 20; pb[2] = 10;
                          { int l = (77 * pb[0] + 150 * pb[1] + 29 * pb[2]) >> 8; int d = 30 - l; pb[1] = (uint8_t)(pb[1] + d * 256 / 150); } }
        }
    ASSERT_INT_EQ(privacy_style_apply(s, a, W, H, W * 3, &p), 0);
    ASSERT_INT_EQ(privacy_style_apply(s, b, W, H, W * 3, &p), 0);
    /* 휘도를 ±1 이내로 맞추기 어려워 완전 일치 대신 "거의 같음"(차이 ≤ 6)으로 검사한다. 색상 정보가 새면 수십 이상 벌어진다. */
    int maxdiff = 0;
    for (size_t i = 0; i < sizeof(a); ++i) { int d = abs((int)a[i] - (int)b[i]); if (d > maxdiff) maxdiff = d; }
    EXPECT_TRUE(maxdiff <= 6);
    for (int i = 0; i < W * H; ++i) {
        if (a[i * 3] != a[i * 3 + 1] || a[i * 3 + 1] != a[i * 3 + 2]) ++colored;
        if (a[i * 3] != 200 && a[i * 3] != 30) ++differs_from_gray;
    }
    EXPECT_TRUE(colored > W * H / 2);                    /* 열화상은 대부분의 픽셀이 회색이 아닌 색이다 */
    EXPECT_TRUE(differs_from_gray > 0);
    privacy_scratch_destroy(s);
}

static void test_privacy_style_thermal_bright_is_warmer_than_dark(void) {
    enum { W = 160, H = 96 };
    static uint8_t f[W * H * 3];
    PrivacyScratch *s = privacy_scratch_create();
    PrivacyParams p = privacy_params_default();
    p.style = PRIVACY_STYLE_THERMAL;
    p.edge_opacity = 10;                                   /* 엣지 영향 최소화 */
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) memset(f + ((size_t)y * W + x) * 3, x < 80 ? 40 : 220, 3);
    ASSERT_INT_EQ(privacy_style_apply(s, f, W, H, W * 3, &p), 0);
    const uint8_t *dark = f + ((size_t)(H / 2) * W + 10) * 3, *bright = f + ((size_t)(H / 2) * W + 150) * 3;
    EXPECT_TRUE(bright[0] > dark[0]);                      /* 밝은 쪽이 더 붉고 */
    EXPECT_TRUE(bright[1] > dark[1]);                      /* 더 노랗다(팔레트: 검정 → 보라 → 주황 → 노랑) */
    EXPECT_TRUE(dark[1] < 60);                             /* 어두운 쪽은 초록 성분이 거의 없다 */
    privacy_scratch_destroy(s);
}

static void test_privacy_style_speed_report(void) {
    /* 통과/실패 기준 없음: 이 PC 에서 1280x720 한 장을 가리는 데 걸리는 시간을 출력한다(i5-4200U 실측 아님). */
    enum { W = 1280, H = 720 };
    uint8_t *frame = (uint8_t *)malloc((size_t)W * H * 3);
    PrivacyScratch *s = privacy_scratch_create();
    PrivacyParams p = privacy_params_default();
    double t0, best = 1e9;
    ASSERT_TRUE(frame != NULL && s != NULL);
    p.style = PRIVACY_STYLE_EDGE_GRAY;
    for (int i = 0; i < W * H * 3; ++i) frame[i] = (uint8_t)((i * 31 + (i / 7) * 13) & 255);
    for (int k = 0; k < 6; ++k) {
        t0 = platform_monotonic_seconds();
        ASSERT_INT_EQ(privacy_style_apply(s, frame, W, H, W * 3, &p), 0);
        t0 = platform_monotonic_seconds() - t0;
        if (t0 < best) best = t0;
    }
    printf("  [info] privacy_style EDGE_GRAY 1280x720 (기본 soft=1 opacity=70): best %.1f ms/frame (dev PC)\n", best * 1000.0);
    p.edge_soft = 0; p.edge_opacity = 100;                 /* 비교: 예전 선명한 선 */
    best = 1e9;
    for (int k = 0; k < 6; ++k) {
        t0 = platform_monotonic_seconds();
        ASSERT_INT_EQ(privacy_style_apply(s, frame, W, H, W * 3, &p), 0);
        t0 = platform_monotonic_seconds() - t0;
        if (t0 < best) best = t0;
    }
    printf("  [info] privacy_style EDGE_GRAY 1280x720 (soft=0 opacity=100, 예전): best %.1f ms/frame (dev PC)\n", best * 1000.0);
    privacy_scratch_destroy(s);
    free(frame);
}

/* ── 폭행 2단계(fight_stage2): 특징·게이트·발화 규칙 ──────────────────────────── */

static void fight_person(Detection *d, float cx, float top, float h, float score) {
    memset(d, 0, sizeof(*d));
    d->x1 = cx - h * 0.2f; d->x2 = cx + h * 0.2f; d->y1 = top; d->y2 = top + h;
    d->score = score; d->keypoint_count = 17;
    for (int k = 0; k < 17; ++k) { d->kp[k].x = cx + (k % 3 - 1) * h * 0.05f; d->kp[k].y = top + h * (0.1f + 0.05f * k); d->kp[k].score = 0.8f; }
}

static float fight_const_scorer(void *ctx, const float *w) { (void)w; return *(float *)ctx; }
static int fight_scorer_calls;
static float fight_counting_scorer(void *ctx, const float *w) { (void)w; ++fight_scorer_calls; return *(float *)ctx; }

static void test_fight_features_pair_and_validity(void) {
    Detection people[3];
    DetectionList list;
    float vec[FIGHT_D], close = -1;
    int valid = -1;
    list.items = people; list.capacity = 3;
    fight_person(&people[0], 100, 100, 200, 0.9f);
    fight_person(&people[1], 160, 100, 200, 0.9f);       /* 중심 거리 60, 평균 키 200 → d=0.3 → close=0.8 */
    list.count = 2;
    fight_frame_features(&list, vec, &valid, &close);
    EXPECT_INT_EQ(valid, 1);
    EXPECT_TRUE(fabsf(close - 0.8f) < 0.01f);
    EXPECT_TRUE(fabsf(vec[102] - 0.3f / 4.0f) < 0.01f);  /* 거리/4 */
    EXPECT_TRUE(vec[103] > 0.0f);                        /* 박스가 겹침 */
    EXPECT_TRUE(fabsf(vec[105] - 0.5f) < 0.01f);         /* 사람 수 2/4 */
    list.count = 1;                                      /* 한 명이면 유효하지 않고 사람 수만 기록 */
    fight_frame_features(&list, vec, &valid, &close);
    EXPECT_INT_EQ(valid, 0);
    EXPECT_TRUE(fabsf(vec[105] - 0.25f) < 0.01f);
    fight_person(&people[1], 160, 100, 200, 0.1f);       /* 점수 0.25 미만은 사람으로 세지 않는다 */
    list.count = 2;
    fight_frame_features(&list, vec, &valid, &close);
    EXPECT_INT_EQ(valid, 0);
}

static void test_fight_monitor_gate_fires_and_cooldown(void) {
    Detection near_pair[2], far_pair[2];
    DetectionList near_list, far_list;
    FightMonitor *m = fight_create(0.9, 1.0 / 3.0);
    float p_high = 0.99f, p_low = 0.10f;
    FightEvent ev;
    int fires = 0;
    double t = 0.0;
    ASSERT_TRUE(m != NULL);
    near_list.items = near_pair; near_list.count = 2; near_list.capacity = 2;
    far_list.items = far_pair; far_list.count = 2; far_list.capacity = 2;
    fight_person(&near_pair[0], 100, 100, 200, 0.9f); fight_person(&near_pair[1], 150, 100, 200, 0.9f);
    fight_person(&far_pair[0], 100, 100, 200, 0.9f);  fight_person(&far_pair[1], 900, 100, 200, 0.9f);  /* d=4 → close=0 */

    /* 게이트: 멀리 떨어진 두 사람이면 창이 차도 분류기를 부르지 않는다(평소 AI 호출 0) */
    fight_scorer_calls = 0;
    fight_set_scorer(m, fight_counting_scorer, &p_high);
    for (int i = 0; i < 30; ++i, t += 1.0 / 3.0) fires += fight_update(m, &far_list, t, 30.0, &ev);
    EXPECT_INT_EQ(fight_scorer_calls, 0);
    EXPECT_INT_EQ(fires, 0);

    /* 가까운 두 사람 + 높은 확률: 창 두 개(3표본 간격)가 임계값 이상이면 발화, 쿨다운 안에서는 다시 발화하지 않는다 */
    fires = 0;
    for (int i = 0; i < 40; ++i, t += 1.0 / 3.0) fires += fight_update(m, &near_list, t, 30.0, &ev);
    EXPECT_TRUE(fight_scorer_calls > 0);
    EXPECT_INT_EQ(fires, 1);                            /* 40표본(≈13초) 동안 한 번만 */
    {
        FightStats st;
        fight_get_stats(m, &st);
        EXPECT_TRUE(st.gate_pass >= 2);
        EXPECT_INT_EQ((int)st.fires, 1);
    }

    /* 낮은 확률이면 분류기는 불리지만 발화하지 않는다 */
    fight_destroy(m);
    m = fight_create(0.9, 1.0 / 3.0);
    fight_set_scorer(m, fight_const_scorer, &p_low);
    fires = 0; t = 0.0;
    for (int i = 0; i < 40; ++i, t += 1.0 / 3.0) fires += fight_update(m, &near_list, t, 30.0, &ev);
    EXPECT_INT_EQ(fires, 0);
    fight_destroy(m);
}

static void test_fight_monitor_gap_resets_window(void) {
    Detection near_pair[2];
    DetectionList list;
    FightMonitor *m = fight_create(0.9, 1.0 / 3.0);
    float p_high = 0.99f;
    FightEvent ev;
    int fires = 0;
    double t = 0.0;
    list.items = near_pair; list.count = 2; list.capacity = 2;
    fight_person(&near_pair[0], 100, 100, 200, 0.9f); fight_person(&near_pair[1], 150, 100, 200, 0.9f);
    fight_set_scorer(m, fight_const_scorer, &p_high);
    /* 표본 6개 → 4초 공백(추론이 쉬었거나 카메라 끊김) → 6개: 창이 이어지지 않으므로 9표본이 차지 않아 발화·호출이 없다 */
    for (int i = 0; i < 6; ++i, t += 1.0 / 3.0) fires += fight_update(m, &list, t, 30.0, &ev);
    t += 4.0;
    for (int i = 0; i < 6; ++i, t += 1.0 / 3.0) fires += fight_update(m, &list, t, 30.0, &ev);
    {
        FightStats st;
        fight_get_stats(m, &st);
        EXPECT_INT_EQ((int)st.scorer_calls, 0);
    }
    EXPECT_INT_EQ(fires, 0);
    fight_update(NULL, &list, t, 30.0, &ev);             /* NULL 안전 */
    fight_destroy(m);
    fight_destroy(NULL);
}

static void test_capture_writes_jpeg_and_respects_privacy(void) {
    enum { W = 64, H = 48 };
    EventCapture cap;
    uint8_t frame[W * H * 3];
    uint8_t before[W * H * 3];
    Detection box;
    char path[720];
    char error[256] = {0};
    unsigned char head[2] = {0, 0};
    FILE *f;
    long size;
    const char *dir = "test_capture_tmp";

    memset(frame, 90, sizeof(frame));
    memcpy(before, frame, sizeof(frame));
    memset(&box, 0, sizeof(box));
    box.x1 = 10; box.y1 = 10; box.x2 = 40; box.y2 = 30; box.score = 0.9f;

    ASSERT_INT_EQ(capture_init(&cap, dir, 85, error, sizeof(error)), 0);
    ASSERT_INT_EQ(capture_save(&cap, "person_fallen", 3, frame, W, H, W * 3,
                               &box, 0, path, sizeof(path), error, sizeof(error)), 0);
    EXPECT_TRUE(strstr(path, "person_fallen_") != NULL);
    EXPECT_TRUE(strstr(path, "_track3.jpg") != NULL);
    /* 캡처용 박스는 사본에만 그려야 합니다 — 원본은 스트림·녹화로 이어집니다. */
    EXPECT_TRUE(memcmp(frame, before, sizeof(frame)) == 0);
    EXPECT_TRUE(memcmp(cap.buf + (10 * W + 10) * 3, before, 3) != 0);
    /* 박스 밖은 원본 픽셀 그대로 */
    EXPECT_INT_EQ(cap.buf[(45 * W + 60) * 3], 90);

    f = fopen(path, "rb");
    ASSERT_TRUE(f != NULL);
    EXPECT_INT_EQ((int)fread(head, 1, 2, f), 2);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);
    EXPECT_INT_EQ(head[0], 0xFF);    /* JPEG SOI 마커 */
    EXPECT_INT_EQ(head[1], 0xD8);
    EXPECT_TRUE(size > 100);
    remove(path);

    /* 보호 모드: 박스 밖 픽셀이 원본(90)이 아니라 단색으로 지워져야 합니다. */
    ASSERT_INT_EQ(capture_save(&cap, "person_fallen", 4, frame, W, H, W * 3,
                               &box, 1, path, sizeof(path), error, sizeof(error)), 0);
    EXPECT_INT_EQ(cap.buf[(45 * W + 60) * 3], 0x30);
    remove(path);

    capture_destroy(&cap);
    capture_destroy(&cap);           /* 이중 해제 안전 */
    EXPECT_INT_EQ(test_rmdir(dir), 0);
}

/* 256x128, 다운샘플 1 → 블록 격자 32x16 (블록 하나 = 8px) */
enum { CAL_W = 256, CAL_H = 128 };

static void calib_setup(MotionCalib *c, GrayBuf *g) {
    CalibOptions opt;
    opt.duration_seconds = 600.0;
    opt.sample_seconds   = 0.5;
    opt.noise_ratio      = 0.3f;
    opt.max_area_ratio   = 0.15f;
    memset(c, 0, sizeof(*c));
    ASSERT_INT_EQ(gray_buf_init(g, CAL_W, CAL_H, 1), 0);
    memset(g->data, 100, (size_t)g->width * (size_t)g->height);
    ASSERT_INT_EQ(calib_start(c, g, CAL_W, CAL_H, &opt, 0.0, NULL, 0), 0);
    EXPECT_INT_EQ(c->blocks_x, 32);
    EXPECT_INT_EQ(c->blocks_y, 16);
}

static void map_reset(MotionMap *m) {
    memset(m, 0, sizeof(*m));
    m->blocks_x = 32;
    m->blocks_y = 16;
}

static void map_set(MotionMap *m, int bx, int by) {
    int idx = by * m->blocks_x + bx;
    if (motion_map_get(m, idx)) return;
    m->bits[idx >> 5] |= 1u << (idx & 31);
    m->changed_blocks++;
}

static void test_calib_learns_flicker_region(void) {
    /* 2x2 블록이 절반의 표본에서 변함 → 흔들림. 먼 블록(20,10)은 10% → 미달. */
    MotionCalib c;
    GrayBuf g;
    MotionMap m;
    CalibRegion out[4];
    int i, n, total = -1;
    calib_setup(&c, &g);
    for (i = 0; i < 100; ++i) {
        map_reset(&m);
        if (i % 2 == 0) {
            map_set(&m, 2, 1); map_set(&m, 3, 1);
            map_set(&m, 2, 2); map_set(&m, 3, 2);
        }
        if (i % 10 == 0) map_set(&m, 20, 10);
        calib_accumulate(&c, &m, NULL, 0);
    }
    EXPECT_INT_EQ(c.samples, 100);
    n = calib_extract(&c, NULL, 0, out, 4, &total);
    ASSERT_INT_EQ(n, 1);
    EXPECT_INT_EQ(total, 1);
    EXPECT_INT_EQ(out[0].status, CALIB_REGION_OK);
    EXPECT_INT_EQ(out[0].blocks, 4);
    EXPECT_FLOAT_NEAR(out[0].rect.x1, 16.0f, 0.01f);
    EXPECT_FLOAT_NEAR(out[0].rect.y1, 8.0f, 0.01f);
    EXPECT_FLOAT_NEAR(out[0].rect.x2, 32.0f, 0.01f);
    EXPECT_FLOAT_NEAR(out[0].rect.y2, 24.0f, 0.01f);
    EXPECT_FLOAT_NEAR(out[0].mean_ratio, 0.5f, 0.01f);
    calib_destroy(&c);
    gray_buf_destroy(&g);
}

static void test_calib_grows_into_swing_fringe(void) {
    /* 씨앗 (5,5) 50% 옆의 (6,5) 15% 는 편입, 떨어진 (15,5) 15% 는 제외. */
    MotionCalib c;
    GrayBuf g;
    MotionMap m;
    CalibRegion out[4];
    int i, n, total = -1;
    calib_setup(&c, &g);
    for (i = 0; i < 100; ++i) {
        map_reset(&m);
        if (i % 2 == 0) map_set(&m, 5, 5);
        if (i % 20 < 3) { map_set(&m, 6, 5); map_set(&m, 15, 5); }
        calib_accumulate(&c, &m, NULL, 0);
    }
    n = calib_extract(&c, NULL, 0, out, 4, &total);
    ASSERT_INT_EQ(n, 1);
    EXPECT_INT_EQ(total, 1);
    EXPECT_INT_EQ(out[0].blocks, 2);
    EXPECT_FLOAT_NEAR(out[0].rect.x1, 40.0f, 0.01f);
    EXPECT_FLOAT_NEAR(out[0].rect.x2, 56.0f, 0.01f);
    calib_destroy(&c);
    gray_buf_destroy(&g);
}

static void test_calib_ignores_people_and_global_change(void) {
    MotionCalib c;
    GrayBuf g;
    MotionMap m;
    CalibRegion out[4];
    GrayRect person;
    int i, x, y;
    calib_setup(&c, &g);
    person.x1 = 80.0f; person.y1 = 40.0f; person.x2 = 95.0f; person.y2 = 55.0f;
    for (i = 0; i < 100; ++i) {
        map_reset(&m);
        map_set(&m, 10, 5); map_set(&m, 11, 6);
        calib_accumulate(&c, &m, &person, 1);
    }
    for (i = 0; i < 10; ++i) {
        map_reset(&m);
        for (y = 0; y < 16; ++y)
            for (x = 0; x < 20; ++x) map_set(&m, x, y);
        calib_accumulate(&c, &m, NULL, 0);
    }
    EXPECT_INT_EQ(c.samples, 100);
    EXPECT_INT_EQ(c.skipped_global, 10);
    EXPECT_INT_EQ((int)c.observed[5 * 32 + 10], 0);
    EXPECT_INT_EQ(calib_extract(&c, NULL, 0, out, 4, NULL), 0);
    calib_destroy(&c);
    gray_buf_destroy(&g);
}

static void test_calib_rejects_protected_and_large(void) {
    MotionCalib c;
    GrayBuf g;
    MotionMap m;
    CalibRegion out[4];
    GrayRect protect[2];
    int i, x, y, n;
    calib_setup(&c, &g);
    for (i = 0; i < 100; ++i) {
        map_reset(&m);
        map_set(&m, 25, 2);                                  /* 문 ROI 안 */
        map_set(&m, 4, 11); map_set(&m, 5, 11); map_set(&m, 6, 11);
        map_set(&m, 4, 12); map_set(&m, 6, 12);              /* 보호 블록 (5,12) 를 둘러쌈 */
        for (y = 0; y < 9; ++y)                              /* 10x9 = 90블록 > 76.8 */
            for (x = 10; x < 20; ++x) map_set(&m, x, y);
        calib_accumulate(&c, &m, NULL, 0);
    }
    protect[0].x1 = 200.0f; protect[0].y1 = 16.0f; protect[0].x2 = 207.0f; protect[0].y2 = 23.0f;
    protect[1].x1 = 40.0f;  protect[1].y1 = 96.0f; protect[1].x2 = 47.0f;  protect[1].y2 = 103.0f;
    n = calib_extract(&c, protect, 2, out, 4, NULL);
    ASSERT_INT_EQ(n, 2);
    EXPECT_INT_EQ(out[0].blocks, 90);
    EXPECT_INT_EQ(out[0].status, CALIB_REGION_TOO_LARGE);
    EXPECT_INT_EQ(out[1].blocks, 5);
    EXPECT_INT_EQ(out[1].status, CALIB_REGION_PROTECTED);
    calib_destroy(&c);
    gray_buf_destroy(&g);
}

static void test_calib_tick_samples_on_interval(void) {
    MotionCalib c;
    GrayBuf g;
    calib_setup(&c, &g);
    EXPECT_INT_EQ(calib_tick(&c, &g, NULL, 0, 2, 0.0), 0);
    EXPECT_INT_EQ(calib_tick(&c, &g, NULL, 0, 2, 0.2), 0);
    memset(g.data, 200, 8 * (size_t)g.width);
    EXPECT_INT_EQ(calib_tick(&c, &g, NULL, 0, 2, 0.6), 1);
    EXPECT_INT_EQ(c.samples, 1);
    EXPECT_INT_EQ((int)c.changed[0], 1);
    EXPECT_INT_EQ((int)c.changed[32], 0);
    EXPECT_TRUE(!calib_elapsed_done(&c, 599.0));
    EXPECT_TRUE(calib_elapsed_done(&c, 600.0));
    calib_destroy(&c);
    EXPECT_TRUE(!calib_active(&c));
    calib_destroy(&c);
    gray_buf_destroy(&g);
}

/* ── 대비 저하 · 2단계 매칭 · 이동 예측 · 발밑 기준점 ──────────────────── */

static GrayStats contrast_stats(unsigned long luma_avg, double grad_avg) {
    GrayStats s;
    memset(&s, 0, sizeof(s));
    s.pixels = 1000;
    s.luma_sum = luma_avg * 1000u;
    s.changed_health = 100;            /* 정지(frozen) 판정에 걸리지 않게 변화는 있음 */
    s.grad_count = 1000;
    s.grad_sum = (unsigned long)(grad_avg * 1000.0);
    return s;
}

static void test_camera_health_low_contrast(void) {
    /* hold=3 으로 줄여 3프레임 연속이면 확정되는지, 한 프레임 회복이면 풀리는지 봅니다. */
    CameraHealth h;
    CameraHealthConfig cfg = {240, 12, 150, 5, 8, 0.045f, 3};
    CamState st;
    GrayStats fog = contrast_stats(200, 3.0);    /* 비율 0.015 — 김서림 */
    GrayStats ok  = contrast_stats(110, 10.0);   /* 비율 0.091 — 정상 */
    GrayStats dim = contrast_stats(30, 2.8);     /* 비율 0.093 — 어둡지만 선명 */
    char error[64] = {0};
    int i;
    ASSERT_INT_EQ(camera_health_init(&h, &cfg, error, sizeof(error)), 0);
    EXPECT_INT_EQ(camera_health_update(&h, &fog, &st), 0);
    EXPECT_INT_EQ(camera_health_update(&h, &fog, &st), 0);
    EXPECT_INT_EQ(camera_health_update(&h, &fog, &st), 1);
    EXPECT_INT_EQ(st, CAM_LOW_CONTRAST);
    EXPECT_TRUE(strcmp(cam_state_name(st), "low_contrast") == 0);
    EXPECT_INT_EQ(camera_health_update(&h, &ok, &st), 1);
    EXPECT_INT_EQ(st, CAM_OK);
    /* 조명만 낮춘 매장은 절대 기울기가 김서림과 비슷해도 비율이 정상이라 걸리면 안 됩니다. */
    for (i = 0; i < 10; ++i) camera_health_update(&h, &dim, &st);
    EXPECT_INT_EQ(st, CAM_OK);
    /* 비율 0 이면 판정 자체를 끕니다(기존 위치 초기화 코드의 기본 동작). */
    {
        CameraHealthConfig off = {240, 12, 150, 5, 8};
        ASSERT_INT_EQ(camera_health_init(&h, &off, error, sizeof(error)), 0);
        for (i = 0; i < 10; ++i) camera_health_update(&h, &fog, &st);
        EXPECT_INT_EQ(st, CAM_OK);
    }
}

static void test_gray_analyze_gradient(void) {
    /* 줄무늬(0/100 번갈아)는 기울기 100, 단색은 0 이어야 합니다. prev 유무 두 경로 모두. */
    enum { W = 16, H = 4 };
    GrayBuf g;
    GrayStats st;
    uint8_t prev[W * H];
    int x, y;
    ASSERT_INT_EQ(gray_buf_init(&g, W, H, 1), 0);
    for (y = 0; y < H; ++y)
        for (x = 0; x < W; ++x) g.data[y * W + x] = (uint8_t)((x & 1) ? 100 : 0);
    gray_analyze(&g, NULL, NULL, 8, 8, 2, &st, NULL);
    EXPECT_INT_EQ(st.grad_count, (W - 1) * H);
    EXPECT_INT_EQ((int)st.grad_sum, 100 * (W - 1) * H);
    memcpy(prev, g.data, sizeof(prev));
    gray_analyze(&g, prev, NULL, 8, 8, 2, &st, NULL);
    EXPECT_INT_EQ((int)st.grad_sum, 100 * (W - 1) * H);
    memset(g.data, 77, (size_t)W * H);
    gray_analyze(&g, prev, NULL, 8, 8, 2, &st, NULL);
    EXPECT_INT_EQ((int)st.grad_sum, 0);
    gray_buf_destroy(&g);
}

static DetectionList one_det(DetectionList *dl, float x1, float y1, float x2, float y2,
                             float score) {
    dl->count = 1;
    memset(&dl->items[0], 0, sizeof(Detection));
    dl->items[0].x1 = x1; dl->items[0].y1 = y1;
    dl->items[0].x2 = x2; dl->items[0].y2 = y2;
    dl->items[0].score = score;
    return *dl;
}

static void test_tracks_two_stage_keeps_track_alive(void) {
    /* 가려져 신뢰도가 떨어진 박스(low)로 기존 트랙이 이어지고, score 는 직전 값을 유지하며,
     * low 만으로는 새 트랙이 생기지 않아야 합니다. */
    TrackList tl;
    DetectionList high, low, empty;
    char error[128] = {0};
    ASSERT_INT_EQ(tracks_init(&tl, 8, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(detection_list_init(&high, 4), 0);
    ASSERT_INT_EQ(detection_list_init(&low, 4), 0);
    ASSERT_INT_EQ(detection_list_init(&empty, 1), 0);
    empty.count = 0;

    one_det(&high, 100, 50, 200, 300, 0.8f);
    tracks_update_ex(&tl, &high, NULL, NULL, 0, 0, 0, 0.0);
    ASSERT_INT_EQ((int)tl.count, 1);

    one_det(&low, 105, 55, 205, 305, 0.12f);
    tracks_update_ex(&tl, &empty, &low, NULL, 0, 0, 0, 0.1);
    EXPECT_INT_EQ(tl.items[0].misses, 0);
    EXPECT_FLOAT_NEAR(tl.items[0].box.x1, 105.0f, 1e-3f);        /* 위치는 갱신 */
    EXPECT_FLOAT_NEAR(tl.items[0].box.score, 0.8f, 1e-6f);       /* 점수는 유지 */

    /* low 가 없으면 예전처럼 miss 로 셉니다. */
    tracks_update_ex(&tl, &empty, NULL, NULL, 0, 0, 0, 0.2);
    EXPECT_INT_EQ(tl.items[0].misses, 1);

    /* 멀리 떨어진 low 박스는 새 트랙을 만들지 않습니다. */
    one_det(&low, 600, 50, 700, 300, 0.15f);
    tracks_update_ex(&tl, &empty, &low, NULL, 0, 0, 0, 0.3);
    EXPECT_INT_EQ((int)tl.count, 1);
    EXPECT_INT_EQ(tl.items[0].misses, 2);

    detection_list_destroy(&high);
    detection_list_destroy(&low);
    detection_list_destroy(&empty);
    tracks_destroy(&tl);
}

static void test_tracks_velocity_prediction(void) {
    /* 오른쪽으로 400px/s 로 걷는 사람. 예측 없이는 IoU 0.11(기준 0.3 미달)로 끊기지만,
     * 속도 예측으로 예상 위치에서 재면 0.67 이라 같은 ID 로 이어져야 합니다. */
    TrackList tl;
    DetectionList d;
    char error[128] = {0};
    ASSERT_INT_EQ(tracks_init(&tl, 8, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
    ASSERT_INT_EQ(detection_list_init(&d, 2), 0);

    one_det(&d, 0, 0, 100, 200, 0.9f);   tracks_update(&tl, &d, NULL, 0, 0, 0, 0.0);
    one_det(&d, 40, 0, 140, 200, 0.9f);  tracks_update(&tl, &d, NULL, 0, 0, 0, 0.1);
    one_det(&d, 80, 0, 180, 200, 0.9f);  tracks_update(&tl, &d, NULL, 0, 0, 0, 0.2);
    EXPECT_FLOAT_NEAR(tl.items[0].vx, 300.0f, 1.0f);            /* EMA: 0→200→300 */
    one_det(&d, 160, 0, 260, 200, 0.9f); tracks_update(&tl, &d, NULL, 0, 0, 0, 0.4);
    EXPECT_INT_EQ((int)tl.count, 1);
    EXPECT_INT_EQ(tl.items[0].id, 1);
    EXPECT_INT_EQ(tl.items[0].misses, 0);
    EXPECT_FLOAT_NEAR(tl.items[0].box.x1, 160.0f, 1e-3f);

    detection_list_destroy(&d);
    tracks_destroy(&tl);
}

static void test_rules_kiosk_anchor_foot(void) {
    /* 키오스크 앞 바닥(y 180~220)에 그린 ROI. 박스 중심(y 125)은 밖, 발밑(y 200)은 안. */
    TrackList tl;
    RulesEngine re;
    EventLog elog;
    RulesConfig rcfg;
    char error[128] = {0};
    int foot;
    for (foot = 0; foot <= 1; ++foot) {
        memset(&rcfg, 0, sizeof(rcfg));
        rcfg.dwell_limit_seconds = 3600.0;
        rcfg.unordered_grace_seconds = 300.0;
        rcfg.fall_hold_seconds = 5.0;
        rcfg.fall_aspect_ratio_kp = 1.8f;
        rcfg.fall_aspect_ratio_nokp = 2.2f;
        rcfg.roi_kiosk_x = 0; rcfg.roi_kiosk_y = 180;
        rcfg.roi_kiosk_w = 300; rcfg.roi_kiosk_h = 40;
        rcfg.roi_kiosk_set = 1;
        rcfg.roi_anchor_foot = foot;
        event_log_open(&elog, ":memory:", LOG_INFO, 0);
        ASSERT_INT_EQ(tracks_init(&tl, 4, 0.3f, 5, 1800.0, 0.45f, error, sizeof(error)), 0);
        ASSERT_INT_EQ(rules_init(&re, 4, &rcfg, error, sizeof(error)), 0);
        tl.count = 1;
        tl.items[0].id = 1;
        tl.items[0].active = 1;
        tl.items[0].order = TRACK_UNORDERED;
        tl.items[0].box = (Detection){100, 50, 160, 200, 0.9f};   /* 세로로 선 사람 */
        rules_evaluate(&re, &tl, 10.0, &elog);
        EXPECT_INT_EQ(tl.items[0].order, TRACK_UNORDERED);
        rules_destroy(&re);
        tracks_destroy(&tl);
        event_log_close(&elog);
    }
}


static void test_probable_order_requires_continuous_near_pose(void) {
    RulesEngine re={0}; Track t={0}; TrackList tl={0}; EventLog log;
    event_log_open(&log, ":memory:", LOG_INFO, 0);
    ASSERT_INT_EQ(rules_init(&re,8,NULL,NULL,0),0);
    rules_set_frame_size(&re,1280,720);
    tl.items=&t;tl.count=1;t.id=1;t.active=1;
    t.box=(Detection){300,0,950,700,0.9f};t.box.keypoint_count=17;
    t.box.kp[0]=(Keypoint){625,100,0.9f};
    t.box.kp[5]=(Keypoint){450,250,0.9f};t.box.kp[6]=(Keypoint){800,250,0.9f};
    t.box.kp[11]=(Keypoint){480,550,0.9f};t.box.kp[12]=(Keypoint){770,550,0.9f};
    for(int i=100;i<130;i++) rules_evaluate(&re,&tl,i,&log);
    EXPECT_INT_EQ(t.order,TRACK_UNORDERED);
    rules_evaluate(&re,&tl,130,&log);
    EXPECT_INT_EQ(t.order,TRACK_PROBABLY_ORDERED);
    tracks_mark_ordered(&tl,1);
    rules_evaluate(&re,&tl,131,&log);
    EXPECT_INT_EQ(t.order,TRACK_ORDERED);
    t.order=TRACK_UNORDERED;rules_shift_time(&re,10);
    for(int i=200;i<220;i++) rules_evaluate(&re,&tl,i,&log);
    t.misses=1;rules_evaluate(&re,&tl,223,&log);t.misses=0;
    for(int i=224;i<254;i++) rules_evaluate(&re,&tl,i,&log);
    EXPECT_INT_EQ(t.order,TRACK_UNORDERED);
    rules_evaluate(&re,&tl,254,&log);
    EXPECT_INT_EQ(t.order,TRACK_PROBABLY_ORDERED);
    t.order=TRACK_UNORDERED;rules_shift_time(&re,10);t.box.kp[0].score=0;
    for(int i=300;i<=340;i++) rules_evaluate(&re,&tl,i,&log);
    EXPECT_INT_EQ(t.order,TRACK_UNORDERED);
    t.box.kp[0].score=.9f;t.box.x2=500;t.box.y2=350;
    for(int i=400;i<=440;i++) rules_evaluate(&re,&tl,i,&log);
    EXPECT_INT_EQ(t.order,TRACK_UNORDERED);
    rules_destroy(&re);event_log_close(&log);
}


static int passage_tick(DoorMonitor *d,uint8_t *frame,const GrayRect *person,double *now) {
    *now+=.5;
    return door_auto_update(d,frame,64,64,192,person,person?1:0,1,*now,NULL,NULL);
}
static void passage_paint(uint8_t *frame,int changed,int global) {
    memset(frame,global?150:50,64*64*3);
    if(changed)for(int y=8;y<20;y++)for(int x=16;x<48;x++)
        memset(frame+(y*64+x)*3,150,3);
}
static void test_door_passage_pair(void) {
    /* Scenarios: valid cycle, head occlusion, lighting, stationary person,
     * no panel change, and second crossing lacking its own changed observation. */
    for(int scenario=0;scenario<6;scenario++) {
        DoorMonitor d={0};uint8_t frame[64*64*3];double now=1;int rc=0;
        GrayRect person={18,24,28,54};
        d.enabled=d.auto_enabled=d.passage_enabled=1;
        d.roi_x=16;d.roi_y=8;d.roi_w=32;d.roi_h=48;d.band_ratio=.25f;
        d.auto_quiet_seconds=1;d.auto_open_min_l1=25;d.band_valid=-1;
        passage_paint(frame,0,0);
        for(int i=0;i<5;i++)passage_tick(&d,frame,NULL,&now);
        EXPECT_TRUE(d.passage_base!=NULL);EXPECT_TRUE(d.ref_closed_rgb==NULL);
        for(int cycle=0;cycle<2;cycle++) {
            person=(GrayRect){18,scenario==1?10.0f:24.0f,28,54};
            passage_paint(frame,scenario!=4 && !(scenario==5&&cycle==1),scenario==2);
            passage_tick(&d,frame,&person,&now);
            passage_tick(&d,frame,&person,&now);
            if(scenario!=3){person.x1+=10;person.x2+=10;}
            passage_tick(&d,frame,&person,&now);
            passage_paint(frame,0,0);
            for(int i=0;i<4;i++)rc=passage_tick(&d,frame,NULL,&now);
            if(cycle==0)EXPECT_TRUE(d.ref_open_rgb==NULL);
        }
        if(scenario==0) {
            EXPECT_INT_EQ(rc,3);EXPECT_TRUE(d.ref_closed_rgb!=NULL);EXPECT_TRUE(d.ref_open_rgb!=NULL);
            EXPECT_INT_EQ(d.ref_open_rgb[(30*64+24)*3],50);
            int changed=0;passage_paint(frame,1,0);
            EXPECT_INT_EQ(door_check(&d,frame,64,64,192,&person,1,&changed),1);
            person.y1=10;
            EXPECT_INT_EQ(door_check(&d,frame,64,64,192,&person,1,&changed),-1);
        } else {EXPECT_TRUE(d.ref_open_rgb==NULL);EXPECT_TRUE(d.ref_closed_rgb==NULL);}
        door_destroy(&d);
    }
}

int main(void) {
    TEST_SUITE_BEGIN(core_unit_tests);
    RUN_TEST(test_letterbox);
    RUN_TEST(test_fall_ignores_clipped_bbox_when_upright);
    RUN_TEST(test_probable_order_requires_continuous_near_pose);
    RUN_TEST(test_fall_still_fires_on_clipped_horizontal_body);
    RUN_TEST(test_fall_unclipped_path_unchanged);
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
    RUN_TEST(test_door_passage_pair);
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
    RUN_TEST(test_draw_label_glyphs_exist);
    RUN_TEST(test_privacy_style_solid_is_default_and_fills);
    RUN_TEST(test_privacy_style_edge_gray_removes_color_keeps_structure);
    RUN_TEST(test_privacy_style_edge_gray_flattens_fine_texture);
    RUN_TEST(test_privacy_style_failures_never_leave_original_pixels);
    RUN_TEST(test_privacy_style_respects_stride_padding_and_sanitizes);
    RUN_TEST(test_capture_privacy_edge_style_writes_gray_copy);
    RUN_TEST(test_privacy_style_edge_blur_reduces_fine_edges);
    RUN_TEST(test_privacy_style_soft_edges_are_dimmer_and_wider);
    RUN_TEST(test_privacy_style_thermal_colors_depend_only_on_luma);
    RUN_TEST(test_privacy_style_thermal_bright_is_warmer_than_dark);
    RUN_TEST(test_privacy_style_speed_report);
    RUN_TEST(test_fight_features_pair_and_validity);
    RUN_TEST(test_fight_monitor_gate_fires_and_cooldown);
    RUN_TEST(test_fight_monitor_gap_resets_window);
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
    RUN_TEST(test_alert_modes_on_shadow_off);
    RUN_TEST(test_alert_shadow_suffix_survives_long_message);
    RUN_TEST(test_reid_revive_resets_dwell_only_after_long_absence);
    RUN_TEST(test_rules_unordered_refires_after_dwell_reset);
    RUN_TEST(test_rules_limbo_revive_does_not_refire);
    RUN_TEST(test_rules_limbo_expiry_releases_state);
    RUN_TEST(test_rules_unordered_requires_roi);
    RUN_TEST(test_rules_fall_geometry);
    RUN_TEST(test_fall_handoff_links_fragmented_track);
    RUN_TEST(test_fall_handoff_ignores_matched_track);
    RUN_TEST(test_fall_wide_box_evidence);
    RUN_TEST(test_fall_false_positive_regressions);
    RUN_TEST(test_fall_hold_tolerates_missed_observations);
    RUN_TEST(test_rules_fall_requires_hold);
    RUN_TEST(test_rules_unordered_seated);
    RUN_TEST(test_rules_unordered_suppressed_by_probable_order);
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
    RUN_TEST(test_decode_tier2_layouts_and_legacy);
    RUN_TEST(test_draw_tier2_confidence_and_masks);
    RUN_TEST(test_nms_keeps_overlapping_boxes_of_different_classes);
    RUN_TEST(test_platform_video_seconds);
    RUN_TEST(test_infer_cache_record_then_replay);
    RUN_TEST(test_infer_cache_mismatch_is_a_miss);
    RUN_TEST(test_infer_cache_refuses_unsafe_files);
    RUN_TEST(test_rules_obj_external_drink);
    RUN_TEST(test_rules_obj_external_requires_person);
    RUN_TEST(test_rules_obj_external_food);
    RUN_TEST(test_rules_obj_animal_on_chair);
    RUN_TEST(test_rules_animal_containment);
    RUN_TEST(test_rules_obj_animal_on_table);
    RUN_TEST(test_rules_obj_animal_present);
    RUN_TEST(test_rules_obj_no_cup_seated);
    RUN_TEST(test_rules_obj_confirm_filter);
    RUN_TEST(test_rules_occupancy_exceeded);
    RUN_TEST(test_rules_motionless_standing);
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
    RUN_TEST(test_reconnect_backoff);
    RUN_TEST(test_reconnect_stall_boundary);
    RUN_TEST(test_tracks_shift_time_after_camera_gap);
    RUN_TEST(test_rules_fall_once_and_gap_shift);
    RUN_TEST(test_capture_writes_jpeg_and_respects_privacy);
    RUN_TEST(test_calib_learns_flicker_region);
    RUN_TEST(test_calib_grows_into_swing_fringe);
    RUN_TEST(test_calib_ignores_people_and_global_change);
    RUN_TEST(test_calib_rejects_protected_and_large);
    RUN_TEST(test_calib_tick_samples_on_interval);
    RUN_TEST(test_camera_health_low_contrast);
    RUN_TEST(test_gray_analyze_gradient);
    RUN_TEST(test_tracks_two_stage_keeps_track_alive);
    RUN_TEST(test_tracks_velocity_prediction);
    RUN_TEST(test_rules_kiosk_anchor_foot);
    TEST_SUITE_END();
}
