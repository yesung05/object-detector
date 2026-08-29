#include "test_runner.h"
#include "yolo11.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ORT 의존 통합 테스트입니다. 모델 경로를 argv[1]로 받습니다.
 * 파일이 없으면 모델이 필요한 테스트를 SKIP하고 나머지는 실행합니다.
 */

static const char *g_model_path = NULL;

static int model_available(void) {
    FILE *f;
    if (!g_model_path) return 0;
    f = fopen(g_model_path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static DetectorOptions default_options(void) {
    DetectorOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.confidence = 0.25f;
    opts.iou = 0.45f;
    opts.max_candidates = 1024;
    opts.max_detections = 64;
    opts.threads = 1;
    opts.provider = DETECTOR_PROVIDER_CPU;
    return opts;
}

static void test_detector_null_destroy(void) {
    /* detector_destroy(NULL)는 crash 없이 반환해야 합니다. */
    detector_destroy(NULL);
}

static void test_detector_invalid_model(void) {
    char error[256] = {0};
    DetectorOptions opts = default_options();
    Detector *d = detector_create("/nonexistent/path/model.onnx", &opts,
                                  error, sizeof(error));
    EXPECT_TRUE(d == NULL);
    EXPECT_TRUE(error[0] != '\0');
    detector_destroy(d);
}

static void test_detector_lifecycle(void) {
    char error[256] = {0};
    DetectorOptions opts = default_options();
    Detector *d;
    if (!model_available()) {
        printf("    SKIP (model not found: %s)\n",
               g_model_path ? g_model_path : "(none)");
        return;
    }
    d = detector_create(g_model_path, &opts, error, sizeof(error));
    ASSERT_TRUE(d != NULL);
    EXPECT_INT_EQ(detector_input_width(d),  416);
    EXPECT_INT_EQ(detector_input_height(d), 416);
    detector_destroy(d);
}

static void test_detector_blank_image(void) {
    char error[256] = {0};
    DetectorOptions opts = default_options();
    Detector *d;
    DetectionList list;
    uint8_t *image;
    DetectorRunStats stats;
    if (!model_available()) {
        printf("    SKIP (model not found)\n");
        return;
    }
    d = detector_create(g_model_path, &opts, error, sizeof(error));
    ASSERT_TRUE(d != NULL);
    ASSERT_INT_EQ(detection_list_init(&list, opts.max_candidates), 0);
    image = (uint8_t *)malloc(416 * 416 * 3);
    ASSERT_TRUE(image != NULL);
    /* 114: YOLO letterbox 여백 기본색 — 실질적으로 빈 프레임입니다. */
    memset(image, 114, 416 * 416 * 3);
    EXPECT_INT_EQ(detector_run(d, image, 416, 416, 416 * 3, &list, error, sizeof(error)), 0);
    detector_get_last_stats(d, &stats);
    EXPECT_TRUE(stats.inference_seconds > 0.0);
    free(image);
    detection_list_destroy(&list);
    detector_destroy(d);
}

static void test_detector_fast_preprocess(void) {
    /* ref/fast 전처리 모드에서 동일 입력을 넣으면 감지 수가 같아야 합니다. */
    char error[256] = {0};
    DetectorOptions opts_ref  = default_options();
    DetectorOptions opts_fast = default_options();
    Detector *d_ref = NULL, *d_fast = NULL;
    DetectionList list_ref, list_fast;
    uint8_t *image = NULL;
    int ok = 1;
    opts_ref.fast_preprocess  = 0;
    opts_fast.fast_preprocess = 1;
    if (!model_available()) {
        printf("    SKIP (model not found)\n");
        return;
    }
    d_ref  = detector_create(g_model_path, &opts_ref,  error, sizeof(error));
    d_fast = detector_create(g_model_path, &opts_fast, error, sizeof(error));
    if (!d_ref || !d_fast) { ok = 0; goto done; }
    if (detection_list_init(&list_ref,  opts_ref.max_candidates) != 0) { ok = 0; goto done; }
    if (detection_list_init(&list_fast, opts_fast.max_candidates) != 0) {
        detection_list_destroy(&list_ref);
        ok = 0; goto done;
    }
    image = (uint8_t *)malloc(416 * 416 * 3);
    if (!image) { ok = 0; goto done; }
    for (int i = 0; i < 416 * 416 * 3; ++i)
        image[i] = (uint8_t)((i * 37 + 11) & 255);
    EXPECT_INT_EQ(
        detector_run(d_ref,  image, 416, 416, 416*3, &list_ref,  error, sizeof(error)), 0);
    EXPECT_INT_EQ(
        detector_run(d_fast, image, 416, 416, 416*3, &list_fast, error, sizeof(error)), 0);
    EXPECT_INT_EQ((int)list_ref.count, (int)list_fast.count);
    free(image);
    detection_list_destroy(&list_ref);
    detection_list_destroy(&list_fast);
done:
    detector_destroy(d_ref);
    detector_destroy(d_fast);
    EXPECT_TRUE(ok);
}

int main(int argc, char **argv) {
    g_model_path = argc > 1 ? argv[1] : NULL;
    TEST_SUITE_BEGIN(detector_integration_tests);
    RUN_TEST(test_detector_null_destroy);
    RUN_TEST(test_detector_invalid_model);
    RUN_TEST(test_detector_lifecycle);
    RUN_TEST(test_detector_blank_image);
    RUN_TEST(test_detector_fast_preprocess);
    TEST_SUITE_END();
}
