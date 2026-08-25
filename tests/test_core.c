#include "yolo11.h"
#include "tracker.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 외부 테스트 프레임워크 없이 assert만 사용하는 핵심 단위 테스트입니다.
 * 조건이 거짓이면 즉시 중단되어 좌표 계산이나 메모리 경계 문제를 알려줍니다.
 */

static void test_letterbox(void) {
    const uint8_t image[2 * 1 * 3] = {
        255, 0, 0, 0, 255, 0
    };
    float tensor[3 * 4 * 4];
    Letterbox t;
    assert(letterbox_to_nchw(image, 2, 1, 6, tensor, 4, 4,
                             RESIZE_NEAREST, &t) == 0);
    assert(fabsf(t.scale - 2.0f) < 0.001f);
    assert(t.pad_x == 0);
    assert(t.pad_y == 1);
    assert(fabsf(tensor[0] - 114.0f / 255.0f) < 0.001f);
    assert(tensor[4] > 0.99f);
}

static void test_fast_letterbox_matches_reference(void) {
    enum { WIDTH = 13, HEIGHT = 9, MODEL = 16 };
    uint8_t image[WIDTH * HEIGHT * 3];
    float reference[MODEL * MODEL * 3];
    float optimized[MODEL * MODEL * 3];
    Letterbox a;
    Letterbox b;
    for (size_t i = 0; i < sizeof(image); ++i)
        image[i] = (uint8_t)((i * 37u + 11u) & 255u);
    assert(letterbox_to_nchw(image, WIDTH, HEIGHT, WIDTH * 3, reference,
                             MODEL, MODEL, RESIZE_BILINEAR, &a) == 0);
    assert(letterbox_to_nchw_fast(image, WIDTH, HEIGHT, WIDTH * 3, optimized,
                                  MODEL, MODEL, RESIZE_BILINEAR, &b) == 0);
    assert(memcmp(reference, optimized, sizeof(reference)) == 0);
    assert(memcmp(&a, &b, sizeof(a)) == 0);
}

static void make_tracking_frame(uint8_t *image, int width, int height,
                                int offset_x) {
    memset(image, 16, (size_t)width * (size_t)height * 3);
    for (int y = 24; y < 72; ++y) {
        for (int x = 32 + offset_x; x < 80 + offset_x; ++x) {
            uint8_t *pixel = image + (y * width + x) * 3;
            uint8_t value = (uint8_t)(((x + y * 3) & 7) * 25 + 50);
            pixel[0] = value;
            pixel[1] = (uint8_t)(255 - value);
            pixel[2] = (uint8_t)(value / 2);
        }
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
    assert(tracker != NULL);
    assert(detection_list_init(&detections, 4) == 0);
    detections.count = 1;
    detections.items[0] = (Detection){32, 24, 79, 71, 0.9f};
    make_tracking_frame(previous, WIDTH, HEIGHT, 0);
    make_tracking_frame(current, WIDTH, HEIGHT, 4);
    assert(tracker_reset(tracker, previous, WIDTH, HEIGHT, WIDTH * 3,
                         error, sizeof(error)) == 0);
    assert(tracker_update(tracker, current, WIDTH, HEIGHT, WIDTH * 3,
                          &detections, &request_detection,
                          error, sizeof(error)) == 0);
    assert(request_detection == 0);
    assert(fabsf(detections.items[0].x1 - 36.0f) <= 1.0f);
    assert(fabsf(detections.items[0].x2 - 83.0f) <= 1.0f);
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
    assert(detection_list_init(&list, 8) == 0);
    assert(yolo11_decode(output, shape, 3, &t, 0.25f, 0.45f,
                         &list, 8) == 0);
    assert(list.count == 1);
    assert(fabsf(list.items[0].score - 0.90f) < 0.001f);
    assert(fabsf(list.items[0].x1 - 30.0f) < 0.001f);
    detection_list_destroy(&list);
}

static void test_draw_bounds(void) {
    enum { WIDTH = 24, HEIGHT = 24, GUARD = 32 };
    uint8_t *memory = (uint8_t *)malloc(GUARD + WIDTH * HEIGHT * 3 + GUARD);
    DetectionList list;
    assert(memory != NULL);
    /*
     * 이미지 앞뒤의 GUARD 영역을 0xA5로 채웁니다. 그리기 후에도 그대로라면
     * draw_detections가 할당된 이미지 범위를 넘겨 쓰지 않은 것입니다.
     */
    memset(memory, 0xa5, GUARD + WIDTH * HEIGHT * 3 + GUARD);
    assert(detection_list_init(&list, 1) == 0);
    list.count = 1;
    list.items[0] = (Detection){0, 0, WIDTH - 1, HEIGHT - 1, 0.88f};
    draw_detections(memory + GUARD, WIDTH, HEIGHT, WIDTH * 3, &list);
    for (int i = 0; i < GUARD; ++i) {
        assert(memory[i] == 0xa5);
        assert(memory[GUARD + WIDTH * HEIGHT * 3 + i] == 0xa5);
    }
    detection_list_destroy(&list);
    free(memory);
}

int main(void) {
    test_letterbox();
    test_fast_letterbox_matches_reference();
    test_decode_and_nms();
    test_draw_bounds();
    test_light_tracker_translation();
    puts("core tests passed");
    return 0;
}
