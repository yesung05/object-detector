#include "yolo11.h"
#include "platform.h"

#include <onnxruntime_c_api.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/*
 * GetExecutionProviderApi가 돌려주는 OrtDmlApi의 첫 ABI 항목입니다. 이 방식은
 * D3D12 C++ 형식까지 포함하는 dml_provider_factory.h 없이 순수 C로 빌드됩니다.
 */
typedef struct {
    OrtStatus *(ORT_API_CALL *SessionOptionsAppendExecutionProvider_DML)(
        OrtSessionOptions *options, int device_id);
} OrtDmlApiPrefix;
#endif

/*
 * ONNX Runtime(이하 ORT) 감지기 한 개가 소유하는 모든 자원입니다.
 *
 * OrtXXX 포인터는 ORT가 정의한 객체이고 각각 알맞은 ReleaseXXX 함수로 해제해야
 * 합니다. input_data와 output_data만 일반 C malloc/free로 관리합니다.
 *
 * 입력/출력 버퍼를 구조체 안에 보관하는 이유는 매 프레임마다 큰 배열을 새로
 * 할당하지 않고 같은 메모리를 계속 재사용하기 위해서입니다.
 */
/* 최근 추론 시간을 저장하는 링 버퍼 크기입니다. p95 계산에 최소 20회가 필요합니다. */
enum { LATENCY_BUF_SIZE = 256 };

struct Detector {
    const OrtApi *ort;
    OrtEnv *env;
    OrtSessionOptions *session_options;
    OrtSession *session;
    OrtMemoryInfo *memory_info;
    OrtValue *input_value;
    OrtValue *output_value;
    char *input_name;
    char *output_name;
    float *input_data;
    float *output_data;
    int64_t input_shape[4];
    int64_t output_shape[3];
    int input_width;
    int input_height;
    DetectorOptions options;
    DetectorRunStats last_stats;
    OrtAllocator *allocator;
    /* 최근 추론 시간 링 버퍼 (ms). 시작 시 1회 할당, 프레임 수와 무관한 고정 크기. */
    double *latency_buf;   /* Pipeline 소유, detector_destroy 에서 해제 */
    int latency_head;
    int latency_count;
};

/* printf와 같은 형식으로 호출자의 오류 문자열 버퍼를 채우는 보조 함수입니다. */
static void set_error(char *error, size_t error_size, const char *format, ...) {
    va_list args;
    if (!error || error_size == 0) return;
    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

/*
 * ORT 함수는 실패 시 OrtStatus 포인터, 성공 시 NULL을 반환합니다.
 * 오류 메시지를 복사한 뒤 OrtStatus까지 여기서 해제하여 누수를 막습니다.
 */
static int ort_ok(Detector *d, OrtStatus *status, char *error,
                  size_t error_size, const char *operation) {
    if (!status) return 1;
    set_error(error, error_size, "%s: %s", operation,
              d->ort->GetErrorMessage(status));
    d->ort->ReleaseStatus(status);
    return 0;
}

#if defined(_WIN32)
static wchar_t *utf8_to_wide(const char *text, char *error,
                             size_t error_size) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
                                     NULL, 0);
    wchar_t *wide;
    if (length <= 0) {
        set_error(error, error_size, "model path is not valid UTF-8");
        return NULL;
    }
    wide = (wchar_t *)malloc((size_t)length * sizeof(*wide));
    if (!wide) {
        set_error(error, error_size, "out of memory converting model path");
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide,
                            length) <= 0) {
        free(wide);
        set_error(error, error_size, "failed to convert model path");
        return NULL;
    }
    return wide;
}
#endif

/*
 * detector_create() 도중 어느 단계에서 실패하더라도 사용할 수 있는 공통 정리
 * 함수입니다. 각 포인터가 NULL인지 확인하므로 일부만 생성된 상태도 안전합니다.
 *
 * 이름 문자열은 ORT allocator가 만들었으므로 allocator->Free로, float 배열은
 * 우리가 malloc했으므로 free로 해제한다는 차이가 중요합니다.
 */
static void detector_release_members(Detector *d) {
    if (!d) return;
    if (d->allocator) {
        if (d->input_name) d->allocator->Free(d->allocator, d->input_name);
        if (d->output_name) d->allocator->Free(d->allocator, d->output_name);
    }
    if (d->ort) {
        if (d->output_value) d->ort->ReleaseValue(d->output_value);
        if (d->input_value) d->ort->ReleaseValue(d->input_value);
        if (d->memory_info) d->ort->ReleaseMemoryInfo(d->memory_info);
        if (d->session) d->ort->ReleaseSession(d->session);
        if (d->session_options)
            d->ort->ReleaseSessionOptions(d->session_options);
        if (d->env) d->ort->ReleaseEnv(d->env);
    }
    free(d->output_data);
    free(d->input_data);
    free(d->latency_buf);
}

Detector *detector_create(const char *model_path, const DetectorOptions *options,
                          char *error, size_t error_size) {
    Detector *d = NULL;
    OrtTypeInfo *type_info = NULL;
    const OrtTensorTypeAndShapeInfo *tensor_info = NULL;
    ONNXTensorElementDataType element_type;
    size_t rank = 0;
    size_t input_count = 0;
    size_t output_count = 0;
    size_t elements;
    size_t output_elements = 1;
    OrtStatus *status;
#if defined(_WIN32)
    wchar_t *wide_model_path = NULL;
#endif

    /* 라이브러리 호출 전에 값 범위를 검사하면 더 이해하기 쉬운 오류를 줄 수 있습니다. */
    if (!model_path || !options || options->max_candidates == 0 ||
        options->max_detections == 0 || options->threads <= 0 ||
        (options->provider != DETECTOR_PROVIDER_CPU &&
         options->provider != DETECTOR_PROVIDER_DIRECTML) ||
        options->device_id < 0) {
        set_error(error, error_size, "invalid detector options");
        return NULL;
    }
#if !defined(_WIN32)
    if (options->provider == DETECTOR_PROVIDER_DIRECTML) {
        set_error(error, error_size,
                  "DirectML is available only in a DML-enabled Windows build");
        return NULL;
    }
#endif

    /* calloc은 메모리를 0으로 채우므로 아직 생성하지 않은 포인터가 모두 NULL입니다. */
    d = (Detector *)calloc(1, sizeof(*d));
    if (!d) {
        set_error(error, error_size, "out of memory creating detector");
        return NULL;
    }
    d->options = *options;
    d->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!d->ort) {
        set_error(error, error_size, "ONNX Runtime C API is unavailable");
        goto fail;
    }

    /*
     * Env는 ORT 전체 환경, SessionOptions는 실행 방식, Session은 실제로 읽은
     * ONNX 모델을 뜻합니다. 추론 스레드를 제한하고 순차 실행을 사용해 저사양
     * 장치에서 CPU와 메모리가 갑자기 늘어나는 것을 억제합니다.
     */
    if (!ort_ok(d, d->ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "yolo11-c",
                                     &d->env),
                error, error_size, "CreateEnv"))
        goto fail;
    if (!ort_ok(d, d->ort->CreateSessionOptions(&d->session_options),
                error, error_size, "CreateSessionOptions"))
        goto fail;
    if (!ort_ok(d, d->ort->SetIntraOpNumThreads(d->session_options,
                                                options->threads),
                error, error_size, "SetIntraOpNumThreads"))
        goto fail;
    if (!ort_ok(d, d->ort->SetInterOpNumThreads(d->session_options, 1),
                error, error_size, "SetInterOpNumThreads"))
        goto fail;
    if (!ort_ok(d, d->ort->SetSessionExecutionMode(d->session_options,
                                                   ORT_SEQUENTIAL),
                error, error_size, "SetSessionExecutionMode"))
        goto fail;
    if (!ort_ok(d, d->ort->SetSessionGraphOptimizationLevel(
                       d->session_options,
                       options->graph_optimization_all ? ORT_ENABLE_ALL
                                                       : ORT_ENABLE_EXTENDED),
                error, error_size, "SetSessionGraphOptimizationLevel"))
        goto fail;
    if (!options->allow_spinning) {
        if (!ort_ok(d, d->ort->AddSessionConfigEntry(
                           d->session_options,
                           "session.intra_op.allow_spinning", "0"),
                    error, error_size, "disable intra-op spinning"))
            goto fail;
        if (!ort_ok(d, d->ort->AddSessionConfigEntry(
                           d->session_options,
                           "session.inter_op.allow_spinning", "0"),
                    error, error_size, "disable inter-op spinning"))
            goto fail;
    }
    if (options->low_memory) {
        if (!ort_ok(d, d->ort->DisableCpuMemArena(d->session_options),
                    error, error_size, "DisableCpuMemArena"))
            goto fail;
    }
    if (options->low_memory ||
        options->provider == DETECTOR_PROVIDER_DIRECTML) {
        if (!ort_ok(d, d->ort->DisableMemPattern(d->session_options),
                    error, error_size, "DisableMemPattern"))
            goto fail;
    }

    /* Haswell 등 FP32 경로에서 denormal 수는 수백 사이클 페널티가 있습니다.
     * DAZ/FTZ 모드를 켜서 0으로 처리하게 하면 실질적인 속도 이득이 있습니다. */
    if (!ort_ok(d, d->ort->AddSessionConfigEntry(
                       d->session_options,
                       "session.set_denormals_as_zero", "1"),
                error, error_size, "set_denormals_as_zero"))
        goto fail;

    if (options->provider == DETECTOR_PROVIDER_DIRECTML) {
#if defined(_WIN32)
        const OrtDmlApiPrefix *dml = NULL;
        status = d->ort->GetExecutionProviderApi(
            "DML", ORT_API_VERSION, (const void **)&dml);
        if (!ort_ok(d, status, error, error_size,
                    "GetExecutionProviderApi(DML)"))
            goto fail;
        if (!dml || !dml->SessionOptionsAppendExecutionProvider_DML) {
            set_error(error, error_size,
                      "this ONNX Runtime build does not contain DirectML");
            goto fail;
        }
        if (!ort_ok(d, dml->SessionOptionsAppendExecutionProvider_DML(
                           d->session_options, options->device_id),
                    error, error_size, "enable DirectML"))
            goto fail;
#endif
    }

#if defined(_WIN32)
    wide_model_path = utf8_to_wide(model_path, error, error_size);
    if (!wide_model_path) goto fail;
    status = d->ort->CreateSession(d->env, wide_model_path,
                                   d->session_options, &d->session);
    free(wide_model_path);
    wide_model_path = NULL;
#else
    status = d->ort->CreateSession(d->env, model_path,
                                   d->session_options, &d->session);
#endif
    if (!ort_ok(d, status,
                error, error_size, "CreateSession"))
        goto fail;
    if (!ort_ok(d, d->ort->GetAllocatorWithDefaultOptions(&d->allocator),
                error, error_size, "GetAllocatorWithDefaultOptions"))
        goto fail;
    if (!ort_ok(d, d->ort->SessionGetInputCount(d->session, &input_count),
                error, error_size, "SessionGetInputCount"))
        goto fail;
    if (!ort_ok(d, d->ort->SessionGetOutputCount(d->session, &output_count),
                error, error_size, "SessionGetOutputCount"))
        goto fail;
    if (input_count != 1 || output_count < 1) {
        set_error(error, error_size,
                  "expected 1 input and at least 1 output, got %zu/%zu",
                  input_count, output_count);
        goto fail;
    }

    /*
     * 이 프로그램은 입출력 이름을 하드코딩하지 않습니다. ONNX 파일에서 첫 번째
     * 입력과 출력의 이름 및 자료형/크기를 읽어 모델과 실제 Run 호출을 연결합니다.
     */
    if (!ort_ok(d, d->ort->SessionGetInputName(d->session, 0, d->allocator,
                                               &d->input_name),
                error, error_size, "SessionGetInputName"))
        goto fail;
    if (!ort_ok(d, d->ort->SessionGetOutputName(d->session, 0, d->allocator,
                                                &d->output_name),
                error, error_size, "SessionGetOutputName"))
        goto fail;
    if (!ort_ok(d, d->ort->SessionGetInputTypeInfo(d->session, 0, &type_info),
                error, error_size, "SessionGetInputTypeInfo"))
        goto fail;
    if (!ort_ok(d, d->ort->CastTypeInfoToTensorInfo(type_info, &tensor_info),
                error, error_size, "CastTypeInfoToTensorInfo"))
        goto fail;
    if (!tensor_info) {
        set_error(error, error_size, "model input is not a tensor");
        goto fail;
    }
    status = d->ort->GetTensorElementType(tensor_info, &element_type);
    if (!ort_ok(d, status, error, error_size, "GetTensorElementType"))
        goto fail;
    if (element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        set_error(error, error_size,
                  "model input must be float32 (got ONNX type %d)",
                  (int)element_type);
        goto fail;
    }
    if (!ort_ok(d, d->ort->GetDimensionsCount(tensor_info, &rank),
                error, error_size, "GetDimensionsCount"))
        goto fail;
    if (rank != 4) {
        set_error(error, error_size, "expected NCHW rank 4 input, got rank %zu",
                  rank);
        goto fail;
    }
    if (!ort_ok(d, d->ort->GetDimensions(tensor_info, d->input_shape, rank),
                error, error_size, "GetDimensions"))
        goto fail;
    d->ort->ReleaseTypeInfo(type_info);
    type_info = NULL;

    /*
     * NCHW [1,3,H,W]에서 1은 한 번에 한 장, 3은 RGB 채널입니다.
     * H와 W가 양수여야 시작할 때 버퍼 크기를 확정하여 한 번만 할당할 수 있습니다.
     */
    if (d->input_shape[0] != 1 || d->input_shape[1] != 3 ||
        d->input_shape[2] <= 0 || d->input_shape[3] <= 0) {
        set_error(error, error_size,
                  "use a fixed-shape [1,3,H,W] YOLO11 model");
        goto fail;
    }
    d->input_height = (int)d->input_shape[2];
    d->input_width = (int)d->input_shape[3];
    elements = (size_t)d->input_width * (size_t)d->input_height * 3;

    /* 곱셈이 size_t 범위를 넘는지 먼저 확인해야 너무 작은 메모리를 잘못 할당하지 않습니다. */
    if (elements > SIZE_MAX / sizeof(float)) {
        set_error(error, error_size, "model input tensor is too large");
        goto fail;
    }
    d->input_data = (float *)malloc(elements * sizeof(float));
    if (!d->input_data) {
        set_error(error, error_size, "out of memory allocating input tensor");
        goto fail;
    }

    /*
     * CreateTensorWithDataAsOrtValue는 input_data를 복사하지 않고 ORT 텐서로
     * 감쌉니다. 따라서 input_data는 input_value보다 오래 살아 있어야 합니다.
     */
    if (!ort_ok(d, d->ort->CreateCpuMemoryInfo(OrtArenaAllocator,
                                               OrtMemTypeDefault,
                                               &d->memory_info),
                error, error_size, "CreateCpuMemoryInfo"))
        goto fail;
    if (!ort_ok(d, d->ort->CreateTensorWithDataAsOrtValue(
                       d->memory_info, d->input_data,
                       elements * sizeof(float), d->input_shape, 4,
                       ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &d->input_value),
                error, error_size, "CreateTensorWithDataAsOrtValue"))
        goto fail;

    /*
     * 대부분의 고정 크기 YOLO 모델은 출력도 [1,채널,후보]처럼 크기가 고정됩니다.
     * 이 경우 output_data를 한 번만 만들고 OrtValue로 감싸서 매 프레임 재사용합니다.
     *
     * 반대로 NMS가 모델 안에 포함되어 결과 개수가 매번 달라지는 모델은 출력
     * 크기를 미리 알 수 없습니다. 그 경우 output_value는 NULL로 남겨 두고,
     * detector_run()에서 ORT가 그 프레임의 출력 메모리를 만들게 합니다.
     */
    if (!ort_ok(d, d->ort->SessionGetOutputTypeInfo(d->session, 0, &type_info),
                error, error_size, "SessionGetOutputTypeInfo"))
        goto fail;
    tensor_info = NULL;
    if (!ort_ok(d, d->ort->CastTypeInfoToTensorInfo(type_info, &tensor_info),
                error, error_size, "CastTypeInfoToTensorInfo(output)"))
        goto fail;
    if (tensor_info) {
        if (!ort_ok(d, d->ort->GetTensorElementType(tensor_info, &element_type),
                    error, error_size, "GetTensorElementType(output)"))
            goto fail;
        if (element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            set_error(error, error_size,
                      "model output must be float32 (got ONNX type %d)",
                      (int)element_type);
            goto fail;
        }
        if (!ort_ok(d, d->ort->GetDimensionsCount(tensor_info, &rank),
                    error, error_size, "GetDimensionsCount(output)"))
            goto fail;
        if (rank == 3) {
            if (!ort_ok(d, d->ort->GetDimensions(
                               tensor_info, d->output_shape, rank),
                        error, error_size,
                        "GetDimensions(output metadata)"))
                goto fail;
        }
        if (rank == 3 && d->output_shape[0] > 0 &&
            d->output_shape[1] > 0 && d->output_shape[2] > 0) {
            for (size_t i = 0; i < 3; ++i) {
                if ((size_t)d->output_shape[i] >
                    SIZE_MAX / output_elements) {
                    set_error(error, error_size, "model output is too large");
                    goto fail;
                }
                output_elements *= (size_t)d->output_shape[i];
            }
            if (output_elements > SIZE_MAX / sizeof(float)) {
                set_error(error, error_size, "model output is too large");
                goto fail;
            }
            d->output_data =
                (float *)malloc(output_elements * sizeof(float));
            if (!d->output_data) {
                set_error(error, error_size,
                          "out of memory allocating output tensor");
                goto fail;
            }
            if (!ort_ok(d, d->ort->CreateTensorWithDataAsOrtValue(
                               d->memory_info, d->output_data,
                               output_elements * sizeof(float),
                               d->output_shape, 3,
                               ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                               &d->output_value),
                        error, error_size,
                        "CreateTensorWithDataAsOrtValue(output)"))
                goto fail;
        }
    }
    d->ort->ReleaseTypeInfo(type_info);
    type_info = NULL;

    /* 웜업 추론: 첫 프레임 지연 스파이크를 서비스 시작 전으로 옮깁니다.
     * 7/6 미팅의 "순간 피크가 매우 위험" 요구에 대응합니다.
     * 입력을 회색(114/255.0)으로 채워 더미 추론을 실행합니다. */
    if (options->warmup_runs > 0) {
        size_t input_elements = (size_t)d->input_width * (size_t)d->input_height * 3;
        const char *warmup_input_names[1];
        const char *warmup_output_names[1];
        const OrtValue *warmup_inputs[1];
        float fill_value = 114.0f / 255.0f;
        int w;

        warmup_input_names[0] = d->input_name;
        warmup_output_names[0] = d->output_name;
        warmup_inputs[0] = d->input_value;

        for (size_t j = 0; j < input_elements; ++j)
            d->input_data[j] = fill_value;

        for (w = 0; w < options->warmup_runs; ++w) {
            OrtValue *out = d->output_value;
            d->ort->Run(d->session, NULL, warmup_input_names, warmup_inputs, 1,
                        warmup_output_names, 1, &out);
            if (out != d->output_value) {
                d->ort->ReleaseValue(out);
                out = NULL;
            }
        }
        fprintf(stderr, "detector: %d warmup runs done\n", options->warmup_runs);
    }

    /* INT8 모델 + DirectML 조합 경고: Intel HD 4400의 DirectML은 QDQ INT8을 올바르게
     * 가속하지 못해 CPU보다 느려질 수 있습니다. 파일명에 "int8"이 포함된 경우 안내합니다. */
    if (options->provider == DETECTOR_PROVIDER_DIRECTML) {
        if (strstr(model_path, "int8") || strstr(model_path, "INT8")) {
            fprintf(stderr,
                    "detector: WARNING: DirectML + INT8 model — Intel HD 4400 DirectML "
                    "does not accelerate QDQ INT8 correctly. "
                    "Try --provider cpu for better throughput.\n");
        }
    }

    /* 링 버퍼는 시작 시 1회 할당합니다. 프레임 수에 무관한 고정 상한. */
    d->latency_buf = (double *)calloc(LATENCY_BUF_SIZE, sizeof(double));
    if (!d->latency_buf) {
        set_error(error, error_size, "out of memory allocating latency buffer");
        goto fail;
    }

    return d;

fail:
    /* 생성 중 실패하면 그 시점까지 성공한 자원만 역순으로 정리합니다. */
#if defined(_WIN32)
    free(wide_model_path);
#endif
    if (type_info && d && d->ort) d->ort->ReleaseTypeInfo(type_info);
    detector_release_members(d);
    free(d);
    return NULL;
}

int detector_run(Detector *d, const uint8_t *rgb, int width, int height,
                 int stride, DetectionList *detections,
                 char *error, size_t error_size) {
    Letterbox transform;
    OrtValue *output = NULL;
    OrtTensorTypeAndShapeInfo *shape_info = NULL;
    ONNXTensorElementDataType output_type;
    size_t rank = 0;
    int64_t shape[4] = {0, 0, 0, 0};
    float *output_data = NULL;
    const char *input_names[1];
    const char *output_names[1];
    const OrtValue *inputs[1];
    int result = -1;
    int output_owned_by_runtime;
    double phase_start;

    /*
     * detections의 capacity가 생성 시 설정과 같은지도 확인합니다. ORT 출력이
     * 예상보다 많더라도 후처리가 배열 범위를 넘지 않게 하는 방어 장치입니다.
     */
    if (!d || !rgb || !detections ||
        detections->capacity != d->options.max_candidates) {
        set_error(error, error_size, "invalid detector_run arguments");
        return -1;
    }

    memset(&d->last_stats, 0, sizeof(d->last_stats));
    phase_start = platform_monotonic_seconds();
    /* RGB 한 장을 재사용 중인 input_data에 letterbox + NCHW 형태로 직접 씁니다. */
    if ((d->options.fast_preprocess
             ? letterbox_to_nchw_fast(rgb, width, height, stride, d->input_data,
                                      d->input_width, d->input_height,
                                      d->options.resize_mode, &transform)
             : letterbox_to_nchw(rgb, width, height, stride, d->input_data,
                                 d->input_width, d->input_height,
                                 d->options.resize_mode, &transform)) != 0) {
        set_error(error, error_size, "failed to preprocess RGB frame");
        return -1;
    }
    d->last_stats.preprocess_seconds =
        platform_monotonic_seconds() - phase_start;
    input_names[0] = d->input_name;
    output_names[0] = d->output_name;
    inputs[0] = d->input_value;
    output = d->output_value;

    /*
     * 고정 출력이면 output은 우리가 만든 d->output_value이고,
     * 동적 출력이면 NULL을 전달하여 ORT가 새 OrtValue를 반환하게 합니다.
     */
    output_owned_by_runtime = output == NULL;
    phase_start = platform_monotonic_seconds();
    if (!ort_ok(d, d->ort->Run(d->session, NULL, input_names, inputs, 1,
                               output_names, 1, &output),
                error, error_size, "Run"))
        goto done;
    d->last_stats.inference_seconds =
        platform_monotonic_seconds() - phase_start;

    /* 링 버퍼에 최근 추론 시간(ms)을 기록하고 백분위 통계를 갱신합니다.
     * insertion sort는 n<=256에서 충분히 빠릅니다. */
    {
        double inference_ms = d->last_stats.inference_seconds * 1000.0;
        double sorted[LATENCY_BUF_SIZE];
        int n, i, j;
        double tmp;
        d->latency_buf[d->latency_head] = inference_ms;
        d->latency_head = (d->latency_head + 1) % LATENCY_BUF_SIZE;
        if (d->latency_count < LATENCY_BUF_SIZE) d->latency_count++;
        n = d->latency_count;
        memcpy(sorted, d->latency_buf, (size_t)n * sizeof(double));
        for (i = 1; i < n; ++i) {
            tmp = sorted[i];
            j = i;
            while (j > 0 && sorted[j - 1] > tmp) {
                sorted[j] = sorted[j - 1];
                --j;
            }
            sorted[j] = tmp;
        }
        d->last_stats.inference_p50_ms = sorted[n / 2];
        d->last_stats.inference_p95_ms = sorted[(int)((size_t)n * 95 / 100)];
        d->last_stats.inference_max_ms = sorted[n - 1];
    }

    phase_start = platform_monotonic_seconds();
    if (d->output_value) {
        /* 고정 출력 경로: 이미 알고 있는 shape와 재사용 버퍼를 곧바로 해석합니다. */
        if (yolo11_decode(d->output_data, d->output_shape, 3, &transform,
                          d->options.confidence, d->options.iou, detections,
                          d->options.max_detections) != 0) {
            set_error(error, error_size,
                      "unsupported YOLO output shape [%lld,%lld,%lld]",
                      (long long)d->output_shape[0],
                      (long long)d->output_shape[1],
                      (long long)d->output_shape[2]);
            goto done;
        }
        result = 0;
        d->last_stats.postprocess_seconds =
            platform_monotonic_seconds() - phase_start;
        goto done;
    }

    /*
     * 동적 출력 경로: 이번 프레임에서 실제로 만들어진 텐서의 크기와 float 포인터를
     * ORT에서 빌린 뒤 해석합니다. output_data 자체를 직접 free하면 안 됩니다.
     */
    if (!ort_ok(d, d->ort->GetTensorTypeAndShape(output, &shape_info),
                error, error_size, "GetTensorTypeAndShape"))
        goto done;
    if (!ort_ok(d, d->ort->GetTensorElementType(shape_info, &output_type),
                error, error_size, "GetTensorElementType(output)"))
        goto done;
    if (output_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        set_error(error, error_size,
                  "model output must be float32 (got ONNX type %d)",
                  (int)output_type);
        goto done;
    }
    if (!ort_ok(d, d->ort->GetDimensionsCount(shape_info, &rank),
                error, error_size, "GetDimensionsCount(output)"))
        goto done;
    if (rank != 3) {
        set_error(error, error_size, "expected rank 3 detection output");
        goto done;
    }
    if (!ort_ok(d, d->ort->GetDimensions(shape_info, shape, rank),
                error, error_size, "GetDimensions(output)"))
        goto done;
    if (!ort_ok(d, d->ort->GetTensorMutableData(output,
                                                (void **)&output_data),
                error, error_size, "GetTensorMutableData"))
        goto done;
    if (yolo11_decode(output_data, shape, rank, &transform,
                      d->options.confidence, d->options.iou, detections,
                      d->options.max_detections) != 0) {
        set_error(error, error_size,
                  "unsupported YOLO output shape [%lld,%lld,%lld]",
                  (long long)shape[0], (long long)shape[1],
                  (long long)shape[2]);
        goto done;
    }
    result = 0;
    d->last_stats.postprocess_seconds =
        platform_monotonic_seconds() - phase_start;

done:
    /*
     * shape_info는 실행할 때마다 받은 임시 메타데이터입니다.
     * 동적 output만 ORT 소유이므로 여기서 ReleaseValue하고, 고정 output은 다음
     * 프레임에 재사용해야 하므로 detector_destroy() 때까지 유지합니다.
     */
    if (shape_info) d->ort->ReleaseTensorTypeAndShapeInfo(shape_info);
    if (output_owned_by_runtime && output) d->ort->ReleaseValue(output);
    return result;
}

void detector_destroy(Detector *d) {
    /* free(NULL)은 안전하지만 d의 멤버에 접근하기 전에 d 자체는 확인해야 합니다. */
    if (!d) return;
    detector_release_members(d);
    free(d);
}

int detector_input_width(const Detector *d) {
    return d ? d->input_width : 0;
}

int detector_input_height(const Detector *d) {
    return d ? d->input_height : 0;
}

void detector_get_last_stats(const Detector *d, DetectorRunStats *stats) {
    if (!stats) return;
    if (!d) {
        memset(stats, 0, sizeof(*stats));
        return;
    }
    *stats = d->last_stats;
}
