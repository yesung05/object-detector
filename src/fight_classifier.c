#include "fight_classifier.h"
#include "fight_stage2.h"

#include <onnxruntime_c_api.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

/*
 * detector_ort.c 와 같은 ORT C API 패턴을 쓰되 이미지 전처리가 없는 최소 버전이다.
 * 소유권 요약:
 *   env / session_options / session / memory_info  — ORT 소유 객체, 이 구조체가 소유, ReleaseXxx 로 해제
 *   input_name / output_name                       — ORT allocator 가 만든 문자열, allocator->Free 로 해제(free() 금지)
 *   input_data                                     — 우리가 malloc, free() 로 해제, 호출마다 재사용(프레임마다 할당하지 않는다)
 *   allocator                                      — ORT 소유(빌린 포인터), 해제 금지
 * 입력 텐서(input_value)는 호출마다 CreateTensorWithDataAsOrtValue 로 input_data 를 감싸고 호출이 끝나면 ReleaseValue 한다
 * (데이터는 복사하지 않으므로 input_data 는 호출 동안만 유효하면 된다).
 */
struct FightClassifier {
    const OrtApi *ort;
    OrtEnv *env;
    OrtSessionOptions *options;
    OrtSession *session;
    OrtMemoryInfo *memory_info;
    OrtAllocator *allocator;   /* ORT 소유, 빌림 */
    char *input_name;          /* allocator 소유 문자열 */
    char *output_name;
    float *input_data;         /* FIGHT_T*FIGHT_D, free() */
};

static void set_error(char *error, size_t size, const char *fmt, ...) {
    va_list args;
    if (!error || size == 0) return;
    va_start(args, fmt);
    vsnprintf(error, size, fmt, args);
    va_end(args);
}

static int ok(FightClassifier *c, OrtStatus *status, char *error, size_t error_size, const char *op) {
    if (!status) return 1;
    set_error(error, error_size, "%s: %s", op, c->ort->GetErrorMessage(status));
    c->ort->ReleaseStatus(status);
    return 0;
}

void fight_classifier_destroy(FightClassifier *c) {
    if (!c) return;
    if (c->allocator) {
        if (c->input_name) c->allocator->Free(c->allocator, c->input_name);
        if (c->output_name) c->allocator->Free(c->allocator, c->output_name);
    }
    if (c->ort) {
        if (c->memory_info) c->ort->ReleaseMemoryInfo(c->memory_info);
        if (c->session) c->ort->ReleaseSession(c->session);
        if (c->options) c->ort->ReleaseSessionOptions(c->options);
        if (c->env) c->ort->ReleaseEnv(c->env);
    }
    free(c->input_data);
    free(c);
}

FightClassifier *fight_classifier_create(const char *model_path, int threads, char *error, size_t error_size) {
    FightClassifier *c;
    OrtStatus *status;
#if defined(_WIN32)
    wchar_t *wide = NULL;
    int length;
#endif
    if (!model_path || !model_path[0]) { set_error(error, error_size, "model path is empty"); return NULL; }
    c = (FightClassifier *)calloc(1, sizeof(*c));
    if (!c) { set_error(error, error_size, "out of memory"); return NULL; }
    c->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!c->ort) { set_error(error, error_size, "ONNX Runtime C API is unavailable"); goto fail; }
    c->input_data = (float *)calloc((size_t)FIGHT_T * FIGHT_D, sizeof(float));
    if (!c->input_data) { set_error(error, error_size, "out of memory"); goto fail; }
    if (!ok(c, c->ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "fight-stage2", &c->env), error, error_size, "CreateEnv")) goto fail;
    if (!ok(c, c->ort->CreateSessionOptions(&c->options), error, error_size, "CreateSessionOptions")) goto fail;
    /* 모델이 22k 파라미터라 스레드를 늘려도 이득이 없다. 1스레드로 두어 메인 추론과 CPU 를 다투지 않게 한다. */
    if (!ok(c, c->ort->SetIntraOpNumThreads(c->options, threads > 0 ? threads : 1), error, error_size, "SetIntraOpNumThreads")) goto fail;
    if (!ok(c, c->ort->SetInterOpNumThreads(c->options, 1), error, error_size, "SetInterOpNumThreads")) goto fail;
    if (!ok(c, c->ort->AddSessionConfigEntry(c->options, "session.intra_op.allow_spinning", "0"), error, error_size, "disable spinning")) goto fail;
#if defined(_WIN32)
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, model_path, -1, NULL, 0);
    if (length <= 0) { set_error(error, error_size, "model path is not valid UTF-8"); goto fail; }
    wide = (wchar_t *)malloc((size_t)length * sizeof(*wide));   /* 소유: 이 함수, CreateSession 직후 free */
    if (!wide) { set_error(error, error_size, "out of memory"); goto fail; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, model_path, -1, wide, length);
    status = c->ort->CreateSession(c->env, wide, c->options, &c->session);
    free(wide);
#else
    status = c->ort->CreateSession(c->env, model_path, c->options, &c->session);
#endif
    if (!ok(c, status, error, error_size, "CreateSession")) goto fail;
    if (!ok(c, c->ort->GetAllocatorWithDefaultOptions(&c->allocator), error, error_size, "GetAllocator")) goto fail;
    if (!ok(c, c->ort->SessionGetInputName(c->session, 0, c->allocator, &c->input_name), error, error_size, "SessionGetInputName")) goto fail;
    if (!ok(c, c->ort->SessionGetOutputName(c->session, 0, c->allocator, &c->output_name), error, error_size, "SessionGetOutputName")) goto fail;
    if (!ok(c, c->ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &c->memory_info), error, error_size, "CreateCpuMemoryInfo")) goto fail;
    return c;
fail:
    fight_classifier_destroy(c);
    return NULL;
}

float fight_classifier_score(void *context, const float *window) {
    FightClassifier *c = (FightClassifier *)context;
    static const int64_t shape[3] = {1, FIGHT_T, FIGHT_D};
    OrtValue *input = NULL, *output = NULL;
    const char *in_names[1], *out_names[1];
    float *result = NULL, logit, p = -1.0f;
    if (!c || !window) return -1.0f;
    memcpy(c->input_data, window, sizeof(float) * FIGHT_T * FIGHT_D);
    if (c->ort->CreateTensorWithDataAsOrtValue(c->memory_info, c->input_data, sizeof(float) * FIGHT_T * FIGHT_D, shape, 3,
                                               ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input)) return -1.0f;
    in_names[0] = c->input_name; out_names[0] = c->output_name;
    {
        OrtStatus *st = c->ort->Run(c->session, NULL, in_names, (const OrtValue *const *)&input, 1, out_names, 1, &output);
        if (st) { c->ort->ReleaseStatus(st); c->ort->ReleaseValue(input); return -1.0f; }
    }
    if (!c->ort->GetTensorMutableData(output, (void **)&result) && result) {
        logit = result[0];
        p = 1.0f / (1.0f + expf(-logit));
    }
    c->ort->ReleaseValue(output);
    c->ort->ReleaseValue(input);
    return p;
}
