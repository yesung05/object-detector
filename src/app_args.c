/* main.c 에서 분리(app_args.c). 동작 변화 없이 옮긴 코드입니다. */
#include "app.h"

static const char *default_camera_format(void) {
#if defined(__APPLE__)
    return "avfoundation";
#elif defined(__linux__)
    return "v4l2";
#elif defined(_WIN32)
    return "dshow";
#else
    return NULL;
#endif
}

static const char *default_camera_device(void) {
#if defined(__APPLE__)
    return "0:none";
#elif defined(__linux__)
    return "/dev/video0";
#elif defined(_WIN32)
    /* 고정 장치명은 기기마다 달라 오작동을 일으킵니다. avdevice_list_input_sources로
     * 첫 번째 dshow 비디오 장치를 동적으로 찾습니다. */
    static char buf[256];
    if (media_first_video_device("dshow", buf, sizeof(buf)) == 0)
        return buf;
    return NULL;
#else
    return NULL;
#endif
}

/* 사용법만 출력합니다. stderr는 일반 결과가 아닌 안내/오류용 출력 통로입니다. */
void usage(const char *program) {
    fprintf(stderr,
        "YOLO11n person detector (streaming, bounded memory)\n"
        "\n"
        "Usage:\n"
        "  %s --model yolo11n.onnx --input in.mp4 --output out.mp4 [options]\n"
        "  %s --model yolo11n.onnx --camera --output capture.mp4 [options]\n"
        "\n"
        "Required:\n"
        "  -m, --model PATH        ONNX 모델 파일, 또는 *-WxH.onnx 파일이 담긴 디렉터리\n"
        "                          디렉터리 지정 시 카메라 비율에 맞는 모델을 자동 선택합니다\n"
        "  -i, --input PATH        input image/video (mutually exclusive with --camera)\n"
        "  --camera                capture from the default camera until Ctrl+C\n"
        "  -o, --output PATH       annotated image/video (optional with camera preview)\n"
        "\n"
        "Options:\n"
        "  --camera-device DEVICE  FFmpeg device name/index (default: platform camera 0)\n"
        "  --camera-format FORMAT  FFmpeg input device (avfoundation/v4l2/dshow)\n"
        "  --camera-size WxH       capture resolution (device default if unset)\n"
        "  --camera-fps N          capture frame rate (device default if unset)\n"
        "  --max-frames N          stop after N frames (default: unlimited)\n"
        "  --detect-every N        full inference interval in frames (default: 1)\n"
        "  --detect-fps F          max YOLO inference FPS; 0=unlimited (default: 10)\n"
        "  --stream-port N         MJPEG stream port on 127.0.0.1 (default: off)\n"
        "                          browse http://localhost:N/stream in dashboard\n"
        "  --track / --no-track    track boxes between skipped frames (default: off)\n"
        "  --confidence F          confidence threshold (default: 0.25)\n"
        "  --threads N             ONNX intra-op threads, 1-3 (default: 1)\n"
        "  --warmup N              dummy inference runs at startup (default: 0)\n"
        "  --provider NAME         cpu or directml (Windows default: directml)\n"
        "  --device-id N           DirectML adapter index (default: 0)\n"
        "  --preprocess MODE       fast or reference (default: fast)\n"
        "  --graph-opt LEVEL       all or extended (default: all)\n"
        "  --allow-spinning 0|1    ORT worker busy-waiting (default: 0)\n"
        "  --codec NAME            auto, h264, h264_qsv, mpeg4, mjpeg\n"
        "  --preview               show annotated frames in an ffplay window\n"
        "  --detections PATH       write per-frame detection CSV\n"
        "  --metrics PATH          write performance metrics JSON\n"
        "  --config PATH           store-specific config file (key=value)\n"
        "  --event-log PATH        structured anomaly event log (default: stderr)\n"
        "  --motion-gate 0|1       skip inference on static scenes (default: 1)\n"
        "  --obj-model PATH        Tier 2 object detection model (ONNX); omit to disable\n"
        "  --detect-every-obj N    run Tier 2 every N frames (default: 90, ~3s at 30fps)\n"
        "  --fall-trace PATH       offline diagnosis: per-frame fall-rule gate CSV\n"
        "  --video-clock           offline evaluation: rule time = video time (frame/fps), not wall clock\n"
        "  --infer-cache PATH      offline evaluation: record/replay model outputs (PATH.obj for Tier 2)\n"
        "  --async-infer 0|1       run person inference on a worker thread (default: 1 for camera, 0 for files)\n"
        "\n",
        program, program);
}

/*
 * strtol()은 실패해도 숫자 0을 반환할 수 있으므로 errno와 end를 함께 확인합니다.
 * end가 문자열 끝('\0')을 가리켜야 "12abc" 같은 잘못된 입력을 거를 수 있습니다.
 */
static int parse_long(const char *text, long low, long high, long *value) {
    char *end = NULL;
    long parsed;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno || !end || *end != '\0' || parsed < low || parsed > high)
        return -1;
    *value = parsed;
    return 0;
}

/* 실수 옵션도 문자열 전체가 올바른 숫자이고 허용 범위 안인지 검사합니다. */
static int parse_float(const char *text, float low, float high, float *value) {
    char *end = NULL;
    float parsed;
    errno = 0;
    parsed = strtof(text, &end);
    if (errno || !end || *end != '\0' || parsed < low || parsed > high)
        return -1;
    *value = parsed;
    return 0;
}

static int valid_video_size(const char *text) {
    int width;
    int height;
    char trailing;
    return text && sscanf(text, "%dx%d%c", &width, &height, &trailing) == 2 &&
           width >= 16 && width <= 8192 && height >= 16 && height <= 8192;
}

/*
 * "--model" 다음처럼 옵션 뒤에 값이 있는지 확인합니다.
 * index 자체를 포인터로 받아 다음 인수의 위치로 한 칸 이동시킵니다.
 */
static int require_value(int argc, char **argv, int *index,
                         const char **value) {
    if (*index + 1 >= argc) {
        fprintf(stderr, "missing value after %s\n", argv[*index]);
        return -1;
    }
    *value = argv[++(*index)];
    return 0;
}

int parse_arguments(int argc, char **argv, Arguments *args) {
    /*
     * 구조체 전체를 0으로 초기화한 뒤 기본값을 덮어씁니다. 이렇게 하면 새 멤버가
     * 추가되어도 초기화되지 않은 쓰레기 값이 들어갈 가능성이 줄어듭니다.
     */
    memset(args, 0, sizeof(*args));
    args->detector.confidence = 0.25f;
    args->detector.iou = 0.45f;
    args->async_infer = -1;
    /*
     * NMS 이전 후보 상한입니다. 1024는 7~10평 매장에서 도달할 수 없는 값이고,
     * DetectionList(후보당 224바이트)를 229KB로 만들어 i5-4200U의 L2 256KB를
     * 혼자 차지했습니다. 128이면 29KB라 L2에 여유롭게 상주합니다.
     *
     * 상한에 걸려도 append_candidate()가 점수가 낮은 후보부터 밀어내므로
     * 상위 128개는 항상 보존됩니다. max_detections(64)의 두 배라 NMS가
     * 고를 여지도 충분합니다.
     */
    args->detector.max_candidates = 128;
    args->detector.max_detections = 64;
    args->detector.threads = 1;
    args->detector.resize_mode = RESIZE_BILINEAR;
    args->detector.fast_preprocess = 1;
    args->detector.graph_optimization_all = 1;
    args->detector.allow_spinning = 0;
#if defined(_WIN32)
    args->detector.provider = DETECTOR_PROVIDER_DIRECTML;
#else
    args->detector.provider = DETECTOR_PROVIDER_CPU;
#endif
    args->detector.device_id = 0;
    args->media.codec = "auto";
    args->media.quality = 23;
    /* video_size·framerate는 기본값을 주지 않습니다. NULL이면 dshow/v4l2/avfoundation이
     * 장치 자체 포맷으로 협상하므로, 해상도를 거부하는 가상 카메라에서도 열립니다.
     * 원하는 해상도가 있으면 --camera-size/--camera-fps 로 명시합니다. */
    args->media.interrupt_callback = should_stop;
    args->detect_every = 1;
    args->detect_every_obj = 90;   /* Tier 2 기본 실행 주기: 30fps 기준 약 3초 */
    args->detect_fps_limit = 10.0; /* 카메라 추론 기본 상한: 10 FPS */
    args->tracking = 0;
    args->preview_fps = 30;
    args->motion_gate = 1;

    /* argv[0]은 실행 파일 이름이므로 실제 옵션은 argv[1]부터 읽습니다. */
    for (int i = 1; i < argc; ++i) {
        const char *value = NULL;
        long parsed;
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 1;
        } else if (strcmp(argv[i], "-m") == 0 ||
                   strcmp(argv[i], "--model") == 0) {
            if (require_value(argc, argv, &i, &args->model) != 0) return -1;
        } else if (strcmp(argv[i], "-i") == 0 ||
                   strcmp(argv[i], "--input") == 0) {
            if (require_value(argc, argv, &i, &args->input) != 0) return -1;
        } else if (strcmp(argv[i], "-o") == 0 ||
                   strcmp(argv[i], "--output") == 0) {
            if (require_value(argc, argv, &i, &args->output) != 0) return -1;
        } else if (strcmp(argv[i], "--camera") == 0) {
            args->camera = 1;
        } else if (strcmp(argv[i], "--camera-device") == 0) {
            if (require_value(argc, argv, &i, &args->camera_device) != 0)
                return -1;
            args->camera = 1;
        } else if (strcmp(argv[i], "--camera-format") == 0) {
            if (require_value(argc, argv, &i, &args->camera_format) != 0)
                return -1;
            args->camera = 1;
        } else if (strcmp(argv[i], "--camera-size") == 0) {
            if (require_value(argc, argv, &i, &value) != 0 ||
                !valid_video_size(value))
                return -1;
            args->media.video_size = value;
            args->camera = 1;
        } else if (strcmp(argv[i], "--camera-fps") == 0) {
            if (require_value(argc, argv, &i, &value) != 0 ||
                parse_long(value, 1, 240, &parsed) != 0)
                return -1;
            args->media.framerate = value;
            args->preview_fps = (int)parsed;
            args->camera = 1;
        } else if (strcmp(argv[i], "--max-frames") == 0) {
            if (require_value(argc, argv, &i, &value) != 0 ||
                parse_long(value, 1, INT_MAX, &parsed) != 0)
                return -1;
            args->media.max_frames = (int)parsed;
        } else if (strcmp(argv[i], "--detect-every") == 0) {
            if (require_value(argc, argv, &i, &value) != 0 ||
                parse_long(value, 1, 1000, &parsed) != 0) return -1;
            args->detect_every = (int)parsed;
        } else if (strcmp(argv[i], "--detect-fps") == 0) {
            float fv;
            if (require_value(argc, argv, &i, &value) != 0 ||
                parse_float(value, 0.0f, 120.0f, &fv) != 0) return -1;
            args->detect_fps_limit = (double)fv;
        } else if (strcmp(argv[i], "--stream-port") == 0) {
            if (require_value(argc, argv, &i, &value) != 0 ||
                parse_long(value, 1024, 65535, &parsed) != 0) return -1;
            args->stream_port = (int)parsed;
            args->stream_port_set = 1;
        } else if (strcmp(argv[i], "--confidence") == 0) {
            if (require_value(argc, argv, &i, &value) != 0 ||
                parse_float(value, 0.0f, 1.0f,
                            &args->detector.confidence) != 0) return -1;
        } else if (strcmp(argv[i], "--threads") == 0) {
            /* 상한 3: CLAUDE.md 방침 "2코어 4스레드 중 3스레드를 파이프라인에 활용"
             * (나머지 1스레드는 OS/시스템 예비). 4 전부를 YOLO에 주면 UI가 멈춥니다. */
            if (require_value(argc, argv, &i, &value) != 0 ||
                parse_long(value, 1, 3, &parsed) != 0) return -1;
            args->detector.threads = (int)parsed;
        } else if (strcmp(argv[i], "--warmup") == 0) {
            if (require_value(argc, argv, &i, &value) != 0 ||
                parse_long(value, 0, 100, &parsed) != 0) return -1;
            args->detector.warmup_runs = (int)parsed;
        } else if (strcmp(argv[i], "--provider") == 0) {
            if (require_value(argc, argv, &i, &value) != 0) return -1;
            if (strcmp(value, "cpu") == 0)
                args->detector.provider = DETECTOR_PROVIDER_CPU;
            else if (strcmp(value, "directml") == 0)
                args->detector.provider = DETECTOR_PROVIDER_DIRECTML;
            else
                return -1;
        } else if (strcmp(argv[i], "--device-id") == 0) {
            if (require_value(argc, argv, &i, &value) != 0 ||
                parse_long(value, 0, 31, &parsed) != 0) return -1;
            args->detector.device_id = (int)parsed;
        } else if (strcmp(argv[i], "--track") == 0) {
            args->tracking = 1;
        } else if (strcmp(argv[i], "--no-track") == 0) {
            args->tracking = 0;
        } else if (strcmp(argv[i], "--preprocess") == 0) {
            if (require_value(argc, argv, &i, &value) != 0) return -1;
            if (strcmp(value, "fast") == 0)
                args->detector.fast_preprocess = 1;
            else if (strcmp(value, "reference") == 0)
                args->detector.fast_preprocess = 0;
            else
                return -1;
        } else if (strcmp(argv[i], "--graph-opt") == 0) {
            if (require_value(argc, argv, &i, &value) != 0) return -1;
            if (strcmp(value, "all") == 0)
                args->detector.graph_optimization_all = 1;
            else if (strcmp(value, "extended") == 0)
                args->detector.graph_optimization_all = 0;
            else
                return -1;
        } else if (strcmp(argv[i], "--allow-spinning") == 0) {
            if (require_value(argc, argv, &i, &value) != 0 ||
                parse_long(value, 0, 1, &parsed) != 0) return -1;
            args->detector.allow_spinning = (int)parsed;
        } else if (strcmp(argv[i], "--codec") == 0) {
            if (require_value(argc, argv, &i, &value) != 0) return -1;
            if (strcmp(value, "auto") != 0 && strcmp(value, "h264") != 0 &&
                strcmp(value, "h264_qsv") != 0 &&
                strcmp(value, "mpeg4") != 0 && strcmp(value, "mjpeg") != 0)
                return -1;
            args->media.codec = value;
        } else if (strcmp(argv[i], "--preview") == 0) {
            args->preview = 1;
        } else if (strcmp(argv[i], "--detections") == 0) {
            if (require_value(argc, argv, &i, &args->detections_path) != 0)
                return -1;
        } else if (strcmp(argv[i], "--keypoints") == 0) {
            if (require_value(argc, argv, &i, &args->keypoints_path) != 0)
                return -1;
        } else if (strcmp(argv[i], "--metrics") == 0) {
            if (require_value(argc, argv, &i, &args->metrics_path) != 0)
                return -1;
        } else if (strcmp(argv[i], "--config") == 0) {
            if (require_value(argc, argv, &i, &args->config_path) != 0)
                return -1;
        } else if (strcmp(argv[i], "--event-log") == 0) {
            if (require_value(argc, argv, &i, &args->event_log_path) != 0)
                return -1;
        } else if (strcmp(argv[i], "--motion-gate") == 0) {
            if (require_value(argc, argv, &i, &value) != 0 ||
                parse_long(value, 0, 1, &parsed) != 0) return -1;
            args->motion_gate = (int)parsed;
        } else if (strcmp(argv[i], "--obj-model") == 0) {
            if (require_value(argc, argv, &i, &args->obj_model) != 0)
                return -1;
        } else if (strcmp(argv[i], "--detect-every-obj") == 0) {
            if (require_value(argc, argv, &i, &value) != 0 ||
                parse_long(value, 1, 10000, &parsed) != 0) return -1;
            args->detect_every_obj = (int)parsed;
        } else if (strcmp(argv[i], "--fall-trace") == 0) {
            if (require_value(argc, argv, &i, &args->fall_trace_path) != 0)
                return -1;
        } else if (strcmp(argv[i], "--video-clock") == 0) {
            args->video_clock = 1;
        } else if (strcmp(argv[i], "--infer-cache") == 0) {
            if (require_value(argc, argv, &i, &args->infer_cache) != 0)
                return -1;
        } else if (strcmp(argv[i], "--async-infer") == 0) {
            if (require_value(argc, argv, &i, &value) != 0 ||
                parse_long(value, 0, 1, &parsed) != 0)
                return -1;
            args->async_infer = (int)parsed;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    /*
     * 출력을 필수로 두지 않습니다.
     *
     * 예전에는 --output 이 없으면 "카메라 + 미리보기" 조합만 허용했습니다.
     * 그러나 실제 배포 형태는 화면도 파일도 없는 헤드리스 상시 감시이고,
     * 결과는 이벤트 로그·감지 CSV·대시보드 스트림으로 나갑니다.
     * 미리보기(ffplay)는 배포 기기에서 오히려 꺼야 하는 기능이므로
     * 그것을 실행 조건으로 삼으면 안 됩니다.
     */
    if (!args->model ||
        (args->camera && args->input) ||
        (!args->camera && !args->input))
        return -1;
    /* 라이브 카메라에는 영상 시간축이 없다. 조용히 무시하지 않고 거부한다. */
    if (args->video_clock && args->camera) {
        fprintf(stderr, "--video-clock cannot be used with --camera (live input has no video timeline)\n");
        return -1;
    }
    /* 추론 캐시는 모델 출력을 실행 순서대로 재생하는데, 비동기는 어느 프레임을 추론할지가 실행마다 달라
     * 순서가 어긋난다. --video-clock 과는 같이 쓸 수 있다(결과마다 추론 프레임의 영상 시각을 들고 다님) —
     * 비동기 지연이 판정에 주는 영향을 오프라인으로 볼 때 쓴다. 결과는 실행마다 조금씩 다르다. */
    if (args->async_infer == 1 && args->infer_cache) {
        fprintf(stderr, "--async-infer 1 cannot be used with --infer-cache (cache replays outputs in run order)\n");
        return -1;
    }
    if (args->async_infer < 0) args->async_infer = args->camera && !args->infer_cache;
    if (args->camera) {
        args->media.input_format = args->camera_format
                                       ? args->camera_format
                                       : default_camera_format();
        args->input = args->camera_device
                          ? args->camera_device
                          : default_camera_device();
        if (!args->camera_device && args->input)
            fprintf(stderr, "camera: auto-detected %s\n", args->input);
        args->media.realtime = 1;
        /* [수정 2026-09-21] 15fps → 30fps: HP TrueVision 등 내장 카메라는 YUY2 15fps를
         * 지원하지 않아 avformat_open_input I/O error가 발생했음. 30fps는 거의 모든
         * dshow 카메라가 지원하는 최소 공약수. 발열 억제는 --detect-every 5 가 담당. */
        if (!args->media.framerate)
            args->media.framerate = "30";
        if (!args->media.input_format || !args->input) {
            fprintf(stderr,
                    "no default camera backend on this platform; use "
                    "--camera-format and --camera-device\n");
            return -1;
        }
    } else {
        /* 카메라 전용 옵션을 일반 파일 demuxer에 전달하지 않습니다. */
        args->media.video_size = NULL;
        args->media.framerate = NULL;
    }
    return 0;
}
