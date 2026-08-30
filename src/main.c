#include "camera_health.h"
#include "stream.h"
#include "door.h"
#include "config.h"
#include "gray.h"
#include "log.h"
#include "media.h"
#include "platform.h"
#include "rules.h"
#include "tracker.h"
#include "tracks.h"
#include "yolo11.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>  /* _stat / stat: config.json mtime 감시용 */

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

/*
 * 프로그램 전체에서 한 번만 만들고 모든 프레임이 함께 쓰는 상태입니다.
 * detector와 detections를 재사용하므로 프레임마다 큰 메모리를 새로 만들지 않습니다.
 */
typedef struct {
    Detector *detector;
    DetectionList detections;
    LightTracker *tracker;
    FILE *detection_log;
    FILE *keypoint_log;   /* --keypoints CSV (박스당 관절 좌표), NULL 이면 비활성 */
    FILE *preview_pipe;
    int detect_every;
    int tracking;
    int preview;
    int preview_fps;
    int has_output;      /* --output 지정 여부. 그리기 필요성 판단에 사용 */
    int64_t frames;
    int64_t inference_runs;
    int64_t tracked_frames;
    int64_t reused_frames;
    int64_t adaptive_runs;
    int64_t gated_frames;
    int64_t event_count;
    double tracking_seconds;
    double drawing_seconds;
    /*
     * 최적화 대상 구간의 계측입니다. 이 값들이 없으면 개선 여부를 주장할 수
     * 없어 추가했습니다 — 측정되지 않는 구간은 최적화 대상이 될 수 없습니다.
     */
    int64_t gate_l1_hits;        /* 변화 자체가 없어 추론을 건너뛴 프레임 */
    int64_t gate_l3_hits;        /* 변화가 기존 트랙 안에만 있어 추적기로 대체한 프레임 */
    int64_t track_refresh_runs;  /* L3 상태에서 안전 밸브로 강제 추론한 횟수 */
    double  gray_seconds;        /* 그레이 변환 + 모션 게이트 + 카메라 헬스 */
    double  door_seconds;        /* door_check 픽셀 비교 */
    double  stream_seconds;      /* stream_push (복사 + 주기 판정) */
    int64_t drawn_frames;        /* 실제로 그린 프레임 수 (관찰자가 있던 프레임) */
    DetectorRunStats detector_stats;
    /* 이상 탐지 모듈 */
    GrayBuf      gray;           /* 현재 프레임 그레이스케일 (지연 초기화) */
    uint8_t     *gray_prev;      /* AppContext 소유, 이전 프레임 gray (모션 게이트 + 카메라 헬스 공용) */
    int          gray_ready;     /* gray_prev 가 유효한지 여부 */
    int          gray_from_luma; /* 1이면 디코더 Y 평면 직접 사용, 0이면 RGB 폴백 */
    int          gray_path_logged;
    CameraHealth cam_health;     /* 카메라 장애 감지 */
    int          cam_health_init_done;
    TrackList    tracks;
    RulesEngine  rules;
    EventLog     event_log;
    int          motion_gate_enabled;
    double       motion_ratio_threshold;  /* 기본 0.004 */
    double       idle_refresh_seconds;    /* 기본 10.0 */
    double       last_detection_time;
    /*
     * 변화 위치 기반 게이트 설정.
     * track_refresh_seconds: 변화가 트랙 안에만 있어 추론을 건너뛰더라도
     *   이 시간이 지나면 한 번은 강제로 YOLO를 돌립니다. 추적기 드리프트가
     *   누적되어 영영 갱신되지 않는 상태를 막는 안전 밸브입니다.
     */
    int          block_gate_enabled;
    int          block_min_changed;
    int          block_margin;
    double       track_refresh_seconds;
    /* 실시간 FPS — 지수 이동 평균(EMA).
     * 누적 평균은 시작 초기값의 영향을 오래 받으므로
     * EMA를 사용해 최근 몇 초간의 순간 FPS를 표시합니다. */
    double       realtime_inf_fps;  /* 추론 간격 EMA, α=0.3 */
    double       realtime_cam_fps;  /* 프레임 간격 EMA, α=0.15 */
    double       last_frame_time;   /* 직전 프레임 시각 (cam FPS EMA용) */
    /* YOLO 추론 FPS 상한. 0이면 제한 없음. detect_every와 OR 조건으로 동작.
     * 카메라 입력에서 추론이 느려도 디코딩 루프를 막지 않기 위해 사용. */
    double       detect_fps_limit;
    double       hud_start_time;  /* 첫 프레임 시각 (HUD FPS 분모) */
    int          stream_port;     /* MJPEG 스트림 포트, 0이면 비활성 */
    DoorMonitor  door;            /* 문 여닫이 감지 (door_reference.raw 필요) */

    /* config.json hot-reload — 2초마다 mtime을 확인하고 변경 시 재로드합니다. */
    char   config_reload_path[512]; /* 재로드할 config 파일 경로 */
    time_t config_mtime;            /* 마지막으로 읽은 파일 수정 시각 */
    double config_check_time;       /* 마지막으로 mtime을 체크한 시각 */
    char   door_closed_path[600];
    char   door_open_path[600];
    time_t door_closed_mtime;
    time_t door_open_mtime;
    double door_refs_check_time;
} AppContext;

/* 명령줄에서 읽은 경로와 프로그램 내부 기본 설정을 한곳에 모읍니다. */
typedef struct {
    const char *model;
    const char *input;
    const char *output;
    const char *camera_device;
    const char *camera_format;
    const char *detections_path;
    const char *keypoints_path;
    const char *metrics_path;
    const char *config_path;
    const char *event_log_path;
    DetectorOptions detector;
    MediaOptions media;
    int detect_every;
    double detect_fps_limit;  /* --detect-fps: YOLO 최대 FPS, 0=무제한 */
    int stream_port;          /* --stream-port: MJPEG 포트, 0=비활성 */
    int tracking;
    int stream_port_set;
    int preview;
    int preview_fps;
    int camera;
    int motion_gate;   /* 1 = 활성 (기본), 0 = 비활성 */
} Arguments;

/* SIGINT/SIGTERM 처리기는 비동기 신호에 안전한 sig_atomic_t 값만 변경합니다. */
static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static int should_stop(void *opaque) {
    (void)opaque;
    return stop_requested != 0;
}

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
static void usage(const char *program) {
    fprintf(stderr,
        "YOLO11n person detector (streaming, bounded memory)\n"
        "\n"
        "Usage:\n"
        "  %s --model yolo11n.onnx --input in.mp4 --output out.mp4 [options]\n"
        "  %s --model yolo11n.onnx --camera --output capture.mp4 [options]\n"
        "\n"
        "Required:\n"
        "  -m, --model PATH        fixed-shape YOLO11/YOLO11n ONNX model\n"
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

static int parse_arguments(int argc, char **argv, Arguments *args) {
    /*
     * 구조체 전체를 0으로 초기화한 뒤 기본값을 덮어씁니다. 이렇게 하면 새 멤버가
     * 추가되어도 초기화되지 않은 쓰레기 값이 들어갈 가능성이 줄어듭니다.
     */
    memset(args, 0, sizeof(*args));
    args->detector.confidence = 0.25f;
    args->detector.iou = 0.45f;
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

static const char *provider_name(DetectorProvider provider) {
    return provider == DETECTOR_PROVIDER_DIRECTML ? "directml" : "cpu";
}

static int log_detections(AppContext *app, int64_t frame_index,
                          const char *kind, char *error, size_t error_size) {
    if (!app->detection_log) return 0;
    for (size_t i = 0; i < app->detections.count; ++i) {
        const Detection *d = &app->detections.items[i];
        if (fprintf(app->detection_log,
                    "%lld,%s,%.4f,%.4f,%.4f,%.4f,%.6f\n",
                    (long long)frame_index, kind, d->x1, d->y1,
                    d->x2, d->y2, d->score) < 0) {
            snprintf(error, error_size, "failed to write detection log");
            return -1;
        }
    }
    return 0;
}

/* keypoint 별도 CSV: 박스 인덱스를 키로 detections CSV 와 조인할 수 있습니다. */
static int log_keypoints(AppContext *app, int64_t frame_index,
                         char *error, size_t error_size) {
    if (!app->keypoint_log) return 0;
    for (size_t i = 0; i < app->detections.count; ++i) {
        const Detection *d = &app->detections.items[i];
        for (int k = 0; k < d->keypoint_count; ++k) {
            if (fprintf(app->keypoint_log,
                        "%lld,%zu,%d,%.4f,%.4f,%.6f\n",
                        (long long)frame_index, i, k,
                        d->kp[k].x, d->kp[k].y, d->kp[k].score) < 0) {
                snprintf(error, error_size, "failed to write keypoint log");
                return -1;
            }
        }
    }
    return 0;
}

static void preview_frame(AppContext *app, const RgbFrame *frame) {
    if (!app->preview) return;
    if (!app->preview_pipe) {
        char command[512];
        snprintf(command, sizeof(command),
                 "ffplay -loglevel error -fflags nobuffer -flags low_delay "
                 "-probesize 32 -analyzeduration 0 -framedrop -sync ext "
                 "-f rawvideo -pixel_format rgb24 -video_size %dx%d "
                 "-framerate %d -window_title \"YOLO11 Person Detector\" -i -",
                 frame->width, frame->height, app->preview_fps * 4);
#if defined(_WIN32)
        app->preview_pipe = popen(command, "wb");
#else
        app->preview_pipe = popen(command, "w");
#endif
        if (!app->preview_pipe) {
            fprintf(stderr, "warning: failed to start ffplay preview\n");
            app->preview = 0;
            return;
        }
        setvbuf(app->preview_pipe, NULL, _IONBF, 0);
    }
    for (int y = 0; y < frame->height; ++y) {
        const uint8_t *row = frame->data + y * frame->stride;
        size_t bytes = (size_t)frame->width * 3;
        if (fwrite(row, 1, bytes, app->preview_pipe) != bytes) {
            fprintf(stderr, "warning: preview window closed; recording continues\n");
            pclose(app->preview_pipe);
            app->preview_pipe = NULL;
            app->preview = 0;
            break;
        }
    }
}

/*
 * config.json이 변경되면 AppContext 설정 필드를 새 값으로 교체합니다.
 * detector, tracker, media 설정 등 재시작이 필요한 항목은 제외하고,
 * 런타임에 안전하게 바꿀 수 있는 임계값들만 업데이트합니다.
 */
static void reload_config(AppContext *app) {
    Config cfg;
    char err[128] = {0};

    if (config_load(&cfg, app->config_reload_path, err, sizeof(err)) != 0) {
        fprintf(stderr, "config reload failed: %s\n", err);
        return;
    }

    /* 모션 게이트 설정 */
    app->motion_gate_enabled    = (int)config_long(&cfg, "motion_gate", 1, 0, 1);
    app->motion_ratio_threshold =
        (double)config_float(&cfg, "motion_ratio_threshold", 0.004f, 0.0001f, 1.0f);
    app->idle_refresh_seconds   =
        (double)config_float(&cfg, "idle_refresh_seconds", 10.0f, 1.0f, 3600.0f);
    app->block_gate_enabled = (int)config_long(&cfg, "block_gate", 1, 0, 1);
    app->block_min_changed  = (int)config_long(&cfg, "block_min_changed", 2, 1, 64);
    app->block_margin       = (int)config_long(&cfg, "block_margin", 1, 0, 16);
    app->track_refresh_seconds =
        (double)config_float(&cfg, "track_refresh_seconds", 5.0f, 0.5f, 120.0f);

    /* 문 여닫이 설정 */
    app->door.enabled                = (int)config_long(&cfg, "door_enabled", 0, 0, 1);
    app->door.diff_threshold         = config_float(&cfg, "door_diff_threshold", 0.05f, 0.001f, 1.0f);
    app->door.confirm_frames         = (int)config_long(&cfg, "door_confirm_frames", 5, 1, 300);
    app->door.open_threshold_seconds = (double)config_float(&cfg, "door_open_seconds", 30.0f, 1.0f, 3600.0f);
    app->door.roi_x = (int)config_long(&cfg, "door_roi_x", 0, 0, 9999);
    app->door.roi_y = (int)config_long(&cfg, "door_roi_y", 0, 0, 9999);
    app->door.roi_w = (int)config_long(&cfg, "door_roi_w", 0, 0, 9999);
    app->door.roi_h = (int)config_long(&cfg, "door_roi_h", 0, 0, 9999);
    /* 설정 변경 즉시 대시보드에 반영 — process_frame을 기다리지 않고 바로 갱신 */
    if (app->stream_port > 0)
        stream_set_door_enabled(app->door.enabled);

    /* 카메라 장애 임계값 — setter가 없으므로 config 구조체에 직접 대입합니다. */
    app->cam_health.config.luma_black_threshold    =
        (int)config_long(&cfg, "luma_black_threshold",    40,  0, 255);
    app->cam_health.config.luma_white_threshold    =
        (int)config_long(&cfg, "luma_white_threshold",   240,  0, 255);
    app->cam_health.config.frozen_frames_threshold =
        (int)config_long(&cfg, "frozen_frames_threshold",  45,  1, 10000);

    /* 체류/쓰러짐 룰 임계값 */
    RulesConfig rules_cfg = app->rules.config; /* 기존값으로 초기화 (roi_kiosk 등 유지) */
    rules_cfg.dwell_limit_seconds =
        (double)config_float(&cfg, "dwell_limit_seconds", 3600.0f, 60.0f, 86400.0f);
    rules_cfg.unordered_grace_seconds =
        (double)config_float(&cfg, "unordered_grace_seconds", 300.0f, 0.0f, 3600.0f);
    rules_cfg.fall_hold_seconds =
        (double)config_float(&cfg, "fall_hold_seconds", 5.0f, 1.0f, 60.0f);
    rules_update_config(&app->rules, &rules_cfg);

    config_destroy(&cfg);
    fprintf(stderr, "config: reloaded from %s\n", app->config_reload_path);
}

static time_t door_ref_mtime(const char *path) {
    if (!path || !path[0]) return 0;
#if defined(_WIN32)
    struct _stat st;
    return _stat(path, &st) == 0 ? st.st_mtime : 0;
#else
    struct stat st;
    return stat(path, &st) == 0 ? (time_t)st.st_mtime : 0;
#endif
}

static void reload_door_references(AppContext *app) {
    time_t closed_mtime = door_ref_mtime(app->door_closed_path);
    time_t open_mtime = door_ref_mtime(app->door_open_path);
    if (closed_mtime == app->door_closed_mtime &&
        open_mtime == app->door_open_mtime) return;
    if (door_load(&app->door, app->door_closed_path, app->door_open_path) != 0) {
        fprintf(stderr, "door: failed to reload references\n");
        return;
    }
    app->door_closed_mtime = closed_mtime;
    app->door_open_mtime = open_mtime;
    fprintf(stderr, "door: references reloaded\n");
}

/*
 * FFmpeg가 RGB 프레임 하나를 준비할 때마다 호출하는 콜백입니다.
 *
 * detect-every가 3이면 0, 3, 6...번 프레임에서만 YOLO를 실행하고, 그 사이
 * 프레임에는 detections 배열에 남아 있는 직전 박스를 다시 그립니다.
 */
static int process_frame(RgbFrame *frame, void *opaque,
                         char *error, size_t error_size) {
    AppContext *app = (AppContext *)opaque;
    int run_detector = frame->index % app->detect_every == 0;
    int tracked = 0;
    int requested = 0;
    const char *kind = "reused";
    double started;
    double now = platform_monotonic_seconds();

    /* config.json hot-reload: 2초마다 파일 수정 시각을 체크합니다.
     * 대시보드에서 설정 저장 후 2초 이내에 자동으로 반영됩니다. */
    if (app->config_reload_path[0] &&
        (now - app->config_check_time) >= 2.0) {
        app->config_check_time = now;
#if defined(_WIN32)
        struct _stat st;
        if (_stat(app->config_reload_path, &st) == 0 &&
            st.st_mtime != app->config_mtime) {
            app->config_mtime = st.st_mtime;
            reload_config(app);
        }
#else
        struct stat st;
        if (stat(app->config_reload_path, &st) == 0 &&
            st.st_mtime != app->config_mtime) {
            app->config_mtime = (time_t)st.st_mtime;
            reload_config(app);
        }
#endif
    }
    if (app->door_closed_path[0] &&
        (now - app->door_refs_check_time) >= 2.0) {
        app->door_refs_check_time = now;
        reload_door_references(app);
    }

    /* 첫 프레임에서 HUD FPS 계산 기준 시각을 기록합니다. */
    if (app->hud_start_time == 0.0)
        app->hud_start_time = now;

    /* 카메라 FPS EMA 업데이트 — 프레임 간격의 역수로 순간 FPS 계산.
     * α=0.15로 약 7~8 프레임 반응폭을 가집니다. */
    if (app->last_frame_time > 0.0) {
        double interval = now - app->last_frame_time;
        if (interval > 0.001) { /* 1ms 미만 이상치 무시 */
            double inst = 1.0 / interval;
            app->realtime_cam_fps = app->realtime_cam_fps * 0.85 + inst * 0.15;
        }
    }
    app->last_frame_time = now;

    /* detect_fps_limit: 시간 기반 추론 상한.
     * 카메라 모드에서 추론이 느려도 디코딩 루프를 블로킹하지 않기 위해,
     * 마지막 추론 이후 경과 시간이 1/fps 미만이면 이번 프레임은 건너뜁니다.
     * detect_every 와 AND 조건: 둘 다 허용할 때만 추론합니다. */
    if (run_detector && app->detect_fps_limit > 0.0 &&
        (now - app->last_detection_time) < (1.0 / app->detect_fps_limit)) {
        run_detector = 0;
    }

    /* 첫 프레임에서 GrayBuf와 CameraHealth를 지연 초기화합니다.
     * frame->width/height는 콜백이 와야 알 수 있습니다. */
    if (app->gray.data == NULL) {
        /*
         * 다운샘플 8: 모션 게이트·카메라 헬스·문 트리거는 위치 해상도가
         * 거칠어도 되는 판정입니다. 720p 기준 160x90 = 14KB 라 i5-4200U 의
         * L1d(코어당 32KB) 안에 통째로 들어가고, 이후 모든 차분 연산이
         * 캐시 미스 없이 끝납니다. 4로 두면 57KB 라 L1 을 넘칩니다.
         */
        if (gray_buf_init(&app->gray, frame->width, frame->height, 8) != 0) {
            snprintf(error, error_size, "gray_buf_init: out of memory");
            return -1;
        }
        app->gray_prev = (uint8_t *)calloc(
            (size_t)app->gray.width * app->gray.height, 1);
        if (!app->gray_prev) {
            snprintf(error, error_size, "gray_prev: out of memory");
            return -1;
        }
        if (camera_health_init(&app->cam_health, NULL,
                               error, error_size) != 0)
            return -1;
        app->cam_health_init_done = 1;
    }
    started = platform_monotonic_seconds();
    /*
     * 그레이스케일 원본 선택.
     *
     * 디코더가 YUV 계열이면 Y 평면이 곧 우리가 원하는 그레이스케일이므로
     * RGB에서 다시 계산하지 않습니다. 어느 경로를 탔는지는 한 번만 로그로
     * 남겨, 현장에서 "왜 이 기기만 느리지"를 로그로 구분할 수 있게 합니다.
     */
    app->gray_from_luma = (frame->luma != NULL);
    if (app->gray_from_luma)
        gray_buf_update_luma(&app->gray, frame->luma, frame->width,
                             frame->height, frame->luma_stride);
    else
        gray_buf_update(&app->gray, frame->data, frame->width, frame->height,
                        frame->stride);
    if (!app->gray_path_logged) {
        app->gray_path_logged = 1;
        event_log_write(&app->event_log, LOG_INFO, "gray",
                        app->gray_from_luma
                            ? "source=luma_plane 그레이스케일 RGB 재계산 생략"
                            : "source=rgb_fallback 입력에 luma 평면 없음");
    }

    /*
     * 프레임 통계를 단일 순회로 계산합니다.
     *
     * 예전에는 같은 버퍼를 세 번 훑었습니다 — 평균 luma, 카메라 헬스용
     * 변화 픽셀 수, 모션 게이트용 변화 픽셀 수. 임계값만 다르고 읽는
     * 데이터는 같았으며, 이전 프레임 사본도 두 벌(gray_prev + CameraHealth
     * 내부 prev_gray) 유지했습니다. 이제 사본은 gray_prev 하나뿐입니다.
     */
    {
        GrayStats stats;
        MotionMap map;
        CamState cam_state;
        int want_map = app->block_gate_enabled && app->gray_ready;
        gray_analyze(&app->gray, app->gray_ready ? app->gray_prev : NULL,
                     8, app->cam_health.config.motion_threshold,
                     app->block_min_changed, &stats, want_map ? &map : NULL);

        /* 카메라 장애 상태가 바뀔 때마다 이벤트를 남깁니다. */
        if (camera_health_update(&app->cam_health, &stats, &cam_state) > 0) {
            char cam_msg[64];
            snprintf(cam_msg, sizeof(cam_msg), "state=%s",
                     cam_state_name(cam_state));
            event_log_write(&app->event_log,
                            cam_state != CAM_OK ? LOG_WARN : LOG_INFO,
                            "camera", cam_msg);
        }

        /*
         * 추론 회피 게이트.
         *
         * 블록 지도가 있으면 위치 기반 판정이 비율 판정을 대체합니다. 비율
         * 하나로는 "창밖 나뭇가지"와 "좌석의 사람"을 구분할 수 없고, 이미
         * 추적 중인 사람이 제자리에서 움직이는 것과 새 사람이 들어온 것도
         * 구분되지 않기 때문입니다. 두 게이트를 겹쳐 걸면 먼저 걸린 쪽이
         * 뒤쪽을 가려 계측이 무의미해지므로 하나만 적용합니다.
         */
        if (app->motion_gate_enabled && run_detector && app->gray_ready) {
            double since_detect = now - app->last_detection_time;

            if (want_map && map.blocks_x > 0) {
                if (map.changed_blocks == 0) {
                    /* L1 — 변화 없음. idle_refresh_seconds 까지만 건너뜁니다. */
                    if (since_detect < app->idle_refresh_seconds) {
                        run_detector = 0;
                        app->gated_frames++;
                        app->gate_l1_hits++;
                    } else {
                        app->track_refresh_runs++;
                    }
                } else if (app->tracking &&
                           since_detect < app->track_refresh_seconds) {
                    /*
                     * L3 — 변화가 전부 이미 추적 중인 박스 안에서 일어났다면
                     * 새 객체가 등장한 것이 아니므로 SAD 추적기로 충분합니다.
                     * 앉아 있는 손님 한 명 때문에 추론이 계속 도는 상황이
                     * 여기서 걸립니다.
                     *
                     * track_refresh_seconds 안에서만 유효합니다. 그 시간이
                     * 지나면 추적기 드리프트를 끊기 위해 한 번은 실행합니다.
                     */
                    GrayRect boxes[64];
                    int n = 0;
                    size_t ti;
                    for (ti = 0; ti < app->tracks.count && n < 64; ++ti) {
                        const Track *t = &app->tracks.items[ti];
                        if (!t->active) continue;
                        boxes[n].x1 = t->box.x1; boxes[n].y1 = t->box.y1;
                        boxes[n].x2 = t->box.x2; boxes[n].y2 = t->box.y2;
                        n++;
                    }
                    if (n > 0 &&
                        gray_blocks_outside(&map, boxes, n,
                                            app->gray.downsample,
                                            app->block_margin) == 0) {
                        run_detector = 0;
                        app->gated_frames++;
                        app->gate_l3_hits++;
                    }
                }
            } else {
                /* 폴백 — 블록 지도를 쓸 수 없을 때의 기존 비율 게이트. */
                double limit = app->motion_ratio_threshold * (double)stats.pixels;
                if ((double)stats.changed_motion < limit &&
                    since_detect < app->idle_refresh_seconds) {
                    run_detector = 0;
                    app->gated_frames++;
                }
            }
        }
    }
    app->gray_seconds += platform_monotonic_seconds() - started;

    /* 추적기도 같은 luma 평면을 쓰게 합니다. 프레임마다 갱신해야 합니다 —
     * 포인터는 이 콜백이 반환하면 무효가 됩니다. */
    if (app->tracker)
        tracker_set_luma(app->tracker, frame->luma, frame->luma_stride);

    if (!run_detector && app->tracking) {
        started = platform_monotonic_seconds();
        if (tracker_update(app->tracker, frame->data, frame->width,
                           frame->height, frame->stride, &app->detections,
                           &requested, error, error_size) != 0)
            return -1;
        app->tracking_seconds += platform_monotonic_seconds() - started;
        if (requested) {
            run_detector = 1;
            app->adaptive_runs++;
        } else {
            tracked = 1;
            app->tracked_frames++;
            kind = "tracked";
        }
    }
    if (run_detector) {
        DetectorRunStats run_stats;
        if (detector_run(app->detector, frame->data, frame->width,
                         frame->height, frame->stride, &app->detections,
                         error, error_size) != 0)
            return -1;
        detector_get_last_stats(app->detector, &run_stats);
        app->detector_stats.preprocess_seconds += run_stats.preprocess_seconds;
        app->detector_stats.inference_seconds += run_stats.inference_seconds;
        app->detector_stats.postprocess_seconds += run_stats.postprocess_seconds;
        /* 링 버퍼 백분위는 누적이 아니라 최신값을 그대로 씁니다. */
        app->detector_stats.inference_p50_ms = run_stats.inference_p50_ms;
        app->detector_stats.inference_p95_ms = run_stats.inference_p95_ms;
        app->detector_stats.inference_max_ms = run_stats.inference_max_ms;
        app->inference_runs++;
        kind = requested ? "adaptive" : "inference";
        if (app->tracking) {
            started = platform_monotonic_seconds();
            if (tracker_reset(app->tracker, frame->data, frame->width,
                              frame->height, frame->stride,
                              error, error_size) != 0)
                return -1;
            app->tracking_seconds += platform_monotonic_seconds() - started;
        }
        /* INF FPS EMA 업데이트 — 이전 추론 이후 경과 시간의 역수.
         * α=0.3으로 약 3회 추론 반응폭. 첫 추론은 건너뜁니다. */
        if (app->last_detection_time > 0.0) {
            double det_interval = now - app->last_detection_time;
            if (det_interval > 0.001) {
                double inst = 1.0 / det_interval;
                app->realtime_inf_fps = app->realtime_inf_fps * 0.7 + inst * 0.3;
            }
        }
        app->last_detection_time = now;
        tracks_update(&app->tracks, &app->detections, now);
        rules_evaluate(&app->rules, &app->tracks, now, &app->event_log);
    } else if (!tracked) {
        app->reused_frames++;
    }

    if (log_detections(app, frame->index, kind, error, error_size) != 0)
        return -1;
    if (log_keypoints(app, frame->index, error, error_size) != 0)
        return -1;

    /* 문 여닫이 감지: 닫힘/열림 기준 이미지와 현재 프레임 ROI 비교
     *
     * 그리기보다 먼저 실행하는 이유: door_check()는 프레임 픽셀을 기준
     * 이미지와 직접 비교합니다. 박스와 HUD를 먼저 그리면 그 픽셀이 그대로
     * 비교 대상에 섞여 들어갑니다. 사람이 문 근처에 서면 그 사람의 박스
     * 선이 ROI 안에 그려져 없던 변화를 만들어 냅니다.
     *
     * 이벤트 정책: 열리는 즉시 로그를 남기지 않고, open_threshold_seconds 이상
     * 열린 상태가 지속될 때만 door_open 이벤트를 발생시킵니다.
     * 잠깐 열렸다 닫히는 정상 입퇴장을 무시하기 위한 설계입니다. */
    if (app->door.enabled) {
        /* 감지 활성 여부를 먼저 알림 — 기준 이미지 없어도 "활성"임을 대시보드에 표시 */
        if (app->stream_port > 0)
            stream_set_door_enabled(1);
    }
    if (app->door.enabled && (app->door.ref_closed_rgb || app->door.ref_open_rgb)) {
        int changed = 0;
        int state;
        started = platform_monotonic_seconds();
        state = door_check(&app->door,
                           frame->data, frame->width, frame->height,
                           frame->stride, &changed);
        app->door_seconds += platform_monotonic_seconds() - started;
        /* 현재 문 상태를 stream 서버에 전달 — /door/state API로 실시간 조회 가능 */
        if (app->stream_port > 0)
            stream_set_door_state(state);

        if (state == 1) {
            /* 열린 상태 — 지속 시간 누적 */
            if (app->door.open_since < 0.0) {
                app->door.open_since = now; /* 이번 개방 시작 시각 */
            } else if (!app->door.open_event_fired &&
                       (now - app->door.open_since) >= app->door.open_threshold_seconds) {
                char door_msg[80];
                snprintf(door_msg, sizeof(door_msg),
                         "door_open: %.0f초 이상 개방 상태 지속",
                         app->door.open_threshold_seconds);
                event_log_write(&app->event_log, LOG_WARN, "door", door_msg);
                app->event_count++;
                app->door.open_event_fired = 1;
            }
        } else if (state == 0) {
            /* 닫힌 상태 — 이전에 개방 중이었다면 닫힘 이벤트 발생 */
            if (app->door.open_since >= 0.0) {
                if (app->door.open_event_fired) {
                    /* door_open 이벤트가 발생한 경우에만 닫힘도 기록 */
                    event_log_write(&app->event_log, LOG_INFO, "door",
                                    "door_closed: 문 닫힘 확인");
                    app->event_count++;
                }
                app->door.open_since       = -1.0;
                app->door.open_event_fired = 0;
            }
        }
    }

    /*
     * 그리기는 볼 사람이 있을 때만 합니다.
     *
     * 배포 상태(출력 파일 없음 + 미리보기 없음 + 대시보드 미접속)에서는
     * 아무도 보지 않는 프레임에 박스와 HUD를 칠하고 있었습니다. 관찰자가
     * 생기면 다음 프레임부터 즉시 다시 그려지므로 지연은 한 프레임입니다.
     */
    {
        int has_observer = app->preview || app->has_output ||
                           (app->stream_port > 0 && stream_client_count() > 0);
        started = platform_monotonic_seconds();
        if (has_observer) {
            draw_detections(frame->data, frame->width, frame->height,
                            frame->stride, &app->detections);
            /* 실시간 EMA FPS를 HUD에 표시합니다.
             * 시작 직후(EMA=0)에는 0이 표시되다가 몇 프레임 후 안정됩니다. */
            draw_hud(frame->data, frame->width, frame->height, frame->stride,
                     (float)app->realtime_inf_fps,
                     (float)app->realtime_cam_fps);
            app->drawn_frames++;
        }
        app->drawing_seconds += platform_monotonic_seconds() - started;
    }

    if (app->stream_port > 0) {
        started = platform_monotonic_seconds();
        stream_push(frame->data, frame->width, frame->height, frame->stride);
        app->stream_seconds += platform_monotonic_seconds() - started;
    }
    preview_frame(app, frame);
    /* 다음 프레임의 모션 게이트 비교를 위해 현재 gray를 복사합니다. */
    if (app->gray.data && app->gray_prev) {
        memcpy(app->gray_prev, app->gray.data,
               (size_t)app->gray.width * app->gray.height);
        app->gray_ready = 1;
    }
    app->frames++;
    return 0;
}

static int write_metrics(const char *path, const Arguments *args,
                         const AppContext *app, const MediaStats *media,
                         double wall_seconds, double cpu_seconds,
                         char *error, size_t error_size) {
    FILE *file;
    unsigned int online_cpus = platform_cpu_count();
    double video_seconds = media->frame_rate > 0.0
                               ? (double)app->frames / media->frame_rate
                               : 0.0;
    if (!path) return 0;
    file = fopen(path, "w");
    if (!file) {
        snprintf(error, error_size, "failed to open metrics file: %s", path);
        return -1;
    }
    fprintf(file,
            "{\n"
            "  \"frames\": %lld,\n"
            "  \"inference_runs\": %lld,\n"
            "  \"tracked_frames\": %lld,\n"
            "  \"reused_frames\": %lld,\n"
            "  \"adaptive_runs\": %lld,\n"
            "  \"gated_frames\": %lld,\n"
            "  \"gate_l1_hits\": %lld,\n"
            "  \"gate_l3_hits\": %lld,\n"
            "  \"track_refresh_runs\": %lld,\n"
            "  \"motion_gate\": %s,\n"
            "  \"frame_rate\": %.6f,\n"
            "  \"video_seconds\": %.6f,\n"
            "  \"wall_seconds\": %.6f,\n"
            "  \"cpu_seconds\": %.6f,\n"
            "  \"cpu_ms_per_frame\": %.6f,\n"
            "  \"one_core_cpu_percent\": %.3f,\n"
            "  \"machine_capacity_percent\": %.3f,\n"
            "  \"estimated_realtime_one_core_percent\": %.3f,\n"
            "  \"input_convert_seconds\": %.6f,\n"
            "  \"preprocess_seconds\": %.6f,\n"
            "  \"inference_seconds\": %.6f,\n"
            "  \"postprocess_seconds\": %.6f,\n"
            "  \"tracking_seconds\": %.6f,\n"
            "  \"drawing_seconds\": %.6f,\n"
            "  \"gray_seconds\": %.6f,\n"
            "  \"door_seconds\": %.6f,\n"
            "  \"stream_seconds\": %.6f,\n"
            "  \"drawn_frames\": %lld,\n"
            "  \"output_seconds\": %.6f,\n"
            "  \"inference_p50_ms\": %.3f,\n"
            "  \"inference_p95_ms\": %.3f,\n"
            "  \"inference_max_ms\": %.3f,\n"
            "  \"settings\": {\n"
            "    \"detect_every\": %d,\n"
            "    \"tracking\": %s,\n"
            "    \"fast_preprocess\": %s,\n"
            "    \"graph_optimization_all\": %s,\n"
            "    \"allow_spinning\": %s,\n"
            "    \"threads\": %d,\n"
            "    \"provider\": \"%s\",\n"
            "    \"device_id\": %d,\n"
            "    \"codec\": \"%s\"\n"
            "  }\n"
            "}\n",
            (long long)app->frames, (long long)app->inference_runs,
            (long long)app->tracked_frames, (long long)app->reused_frames,
            (long long)app->adaptive_runs,
            (long long)app->gated_frames,
            (long long)app->gate_l1_hits,
            (long long)app->gate_l3_hits,
            (long long)app->track_refresh_runs,
            app->motion_gate_enabled ? "true" : "false",
            media->frame_rate, video_seconds,
            wall_seconds, cpu_seconds,
            app->frames > 0 ? cpu_seconds * 1000.0 / (double)app->frames : 0.0,
            wall_seconds > 0.0 ? cpu_seconds * 100.0 / wall_seconds : 0.0,
            wall_seconds > 0.0
                ? cpu_seconds * 100.0 / wall_seconds / (double)online_cpus
                : 0.0,
            video_seconds > 0.0 ? cpu_seconds * 100.0 / video_seconds : 0.0,
            media->input_convert_seconds,
            app->detector_stats.preprocess_seconds,
            app->detector_stats.inference_seconds,
            app->detector_stats.postprocess_seconds,
            app->tracking_seconds, app->drawing_seconds,
            app->gray_seconds, app->door_seconds, app->stream_seconds,
            (long long)app->drawn_frames,
            media->output_seconds,
            app->detector_stats.inference_p50_ms,
            app->detector_stats.inference_p95_ms,
            app->detector_stats.inference_max_ms,
            args->detect_every, args->tracking ? "true" : "false",
            args->detector.fast_preprocess ? "true" : "false",
            args->detector.graph_optimization_all ? "true" : "false",
            args->detector.allow_spinning ? "true" : "false",
            args->detector.threads, provider_name(args->detector.provider),
            args->detector.device_id, args->media.codec);
    if (fclose(file) != 0) {
        snprintf(error, error_size, "failed to finish metrics file: %s", path);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    Arguments args;
    AppContext app;
    char error[512] = {0};
    double start;
    double end;
    double cpu_start;
    double cpu_end;
    MediaStats media_stats;
    int parse_result;
    int result = EXIT_FAILURE;
    FILE *event_log_file = NULL;  /* NULL=미열림, stderr=기본값, 파일=직접 열었음 */

    /* 도움말은 정상 종료, 잘못된 옵션은 실패 종료로 구분합니다. */
    parse_result = parse_arguments(argc, argv, &args);
    if (parse_result > 0) return EXIT_SUCCESS;
    if (parse_result < 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    memset(&app, 0, sizeof(app));
    app.detect_every = args.detect_every;
    app.detect_fps_limit = args.detect_fps_limit;
    app.stream_port = args.stream_port;
    app.tracking = args.tracking;
    app.preview = args.preview;
    app.preview_fps = args.preview_fps;
    app.has_output = args.output != NULL;
    memset(&media_stats, 0, sizeof(media_stats));
    args.media.stats = &media_stats;

    /*
     * 후보 배열은 여기서 딱 한 번 만들고 모든 프레임에서 재사용합니다.
     * 이후 실패하면 goto done으로 이동하여 이미 만든 자원을 한곳에서 정리합니다.
     */
    if (detection_list_init(&app.detections,
                            args.detector.max_candidates) != 0) {
        fprintf(stderr, "failed to allocate bounded detection buffer\n");
        goto done;
    }
    app.detector = detector_create(args.model, &args.detector,
                                   error, sizeof(error));
    if (!app.detector) {
        fprintf(stderr, "detector initialization failed: %s\n", error);
        goto done;
    }
    if (app.tracking) {
        const TrackerOptions tracker_options = {4, 3, 2, 24};
        app.tracker = tracker_create(&tracker_options);
        if (!app.tracker) {
            fprintf(stderr, "failed to allocate lightweight tracker\n");
            goto done;
        }
    }
    if (args.detections_path) {
        app.detection_log = fopen(args.detections_path, "w");
        if (!app.detection_log) {
            fprintf(stderr, "failed to open detection log: %s\n",
                    args.detections_path);
            goto done;
        }
        fputs("frame,kind,x1,y1,x2,y2,score\n", app.detection_log);
    }
    if (args.keypoints_path) {
        app.keypoint_log = fopen(args.keypoints_path, "w");
        if (!app.keypoint_log) {
            fprintf(stderr, "failed to open keypoint log: %s\n",
                    args.keypoints_path);
            goto done;
        }
        fputs("frame,det_index,kp_index,x,y,score\n", app.keypoint_log);
    }
    /* 매장별 설정 파일 읽기 (path==NULL이면 빈 Config, 오류는 무시하고 기본값 사용) */
    {
        Config cfg;
        RulesConfig rules_cfg;
        memset(&rules_cfg, 0, sizeof(rules_cfg));
        if (config_load(&cfg, args.config_path, error, sizeof(error)) != 0 &&
            args.config_path) {
            fprintf(stderr, "warning: config load failed (%s), using defaults\n",
                    error);
        }
        rules_cfg.dwell_limit_seconds =
            (double)config_float(&cfg, "dwell_limit_seconds",  3600.0f, 60.0f, 86400.0f);
        rules_cfg.unordered_grace_seconds =
            (double)config_float(&cfg, "unordered_grace_seconds", 300.0f, 0.0f, 3600.0f);
        rules_cfg.fall_hold_seconds =
            (double)config_float(&cfg, "fall_hold_seconds", 5.0f, 1.0f, 60.0f);
        app.motion_gate_enabled   =
            (int)config_long(&cfg, "motion_gate", args.motion_gate, 0, 1);
        app.motion_ratio_threshold =
            (double)config_float(&cfg, "motion_ratio_threshold", 0.004f, 0.0001f, 1.0f);
        app.idle_refresh_seconds  =
            (double)config_float(&cfg, "idle_refresh_seconds", 10.0f, 1.0f, 3600.0f);
        app.block_gate_enabled = (int)config_long(&cfg, "block_gate", 1, 0, 1);
        app.block_min_changed  = (int)config_long(&cfg, "block_min_changed", 2, 1, 64);
        app.block_margin       = (int)config_long(&cfg, "block_margin", 1, 0, 16);
        app.track_refresh_seconds =
            (double)config_float(&cfg, "track_refresh_seconds", 5.0f, 0.5f, 120.0f);
        /* 문 여닫이 설정 — door_load는 cfg 블록 밖에서 (data_dir 필요) */
        app.door.enabled                 = (int)config_long (&cfg, "door_enabled",        0,    0,    1);
        app.door.diff_threshold          = config_float      (&cfg, "door_diff_threshold",  0.05f, 0.001f, 1.0f);
        app.door.confirm_frames          = (int)config_long  (&cfg, "door_confirm_frames",  5,    1,  300);
        app.door.open_threshold_seconds  = (double)config_float(&cfg, "door_open_seconds", 30.0f, 1.0f, 3600.0f);
        app.door.roi_x                   = (int)config_long (&cfg, "door_roi_x",          0,    0, 9999);
        app.door.roi_y          = (int)config_long (&cfg, "door_roi_y",         0,    0, 9999);
        app.door.roi_w          = (int)config_long (&cfg, "door_roi_w",         0,    0, 9999);
        app.door.roi_h          = (int)config_long (&cfg, "door_roi_h",         0,    0, 9999);
        if (!args.stream_port_set)
            app.stream_port = (int)config_long(&cfg, "stream_port", 0, 1024, 65535);
        config_destroy(&cfg);
        if (tracks_init(&app.tracks, 64, 0.4f, 5,
                        error, sizeof(error)) != 0) {
            fprintf(stderr, "failed to init track list: %s\n", error);
            goto done;
        }
        if (rules_init(&app.rules, 64, &rules_cfg, error, sizeof(error)) != 0) {
            fprintf(stderr, "failed to init rules engine: %s\n", error);
            goto done;
        }
    }
    /* 이벤트 로그 파일 오픈 (미지정이면 stderr로 출력) */
    event_log_file = args.event_log_path
                         ? fopen(args.event_log_path, "a")
                         : stderr;
    if (args.event_log_path && !event_log_file) {
        fprintf(stderr, "failed to open event log: %s\n",
                args.event_log_path);
        goto done;
    }
    event_log_init(&app.event_log, event_log_file, LOG_INFO);

    fprintf(stderr,
            "model input: %dx%d, provider: %s, threads: %d, detect every: %d, tracker: %s, "
            "candidate cap: %zu%s\n",
            detector_input_width(app.detector),
            detector_input_height(app.detector),
            provider_name(args.detector.provider), args.detector.threads,
            args.detect_every, args.tracking ? "on" : "off",
            args.detector.max_candidates,
            args.detector.low_memory ? ", low-memory mode" : "");
    if (args.camera) {
        fprintf(stderr,
                "camera: %s via %s, %s at %s fps (press Ctrl+C to stop)\n",
                args.input, args.media.input_format, args.media.video_size,
                args.media.framerate);
    }

    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
#if defined(SIGPIPE)
    signal(SIGPIPE, SIG_IGN);
#endif
    /* config hot-reload 경로 및 초기 mtime 설정 */
    if (args.config_path) {
        strncpy(app.config_reload_path, args.config_path,
                sizeof(app.config_reload_path) - 1);
#if defined(_WIN32)
        struct _stat ini_st;
        if (_stat(args.config_path, &ini_st) == 0)
            app.config_mtime = ini_st.st_mtime;
#else
        struct stat ini_st;
        if (stat(args.config_path, &ini_st) == 0)
            app.config_mtime = (time_t)ini_st.st_mtime;
#endif
    }

    /* data_dir: config 파일 위치를 프로젝트 루트로 사용합니다.
     * door_reference.raw와 config.json이 같은 디렉터리에 놓입니다. */
    char data_dir[512] = ".";
    if (args.config_path) {
        strncpy(data_dir, args.config_path, sizeof(data_dir) - 1);
        char *bs = strrchr(data_dir, '\\');
        char *fs = strrchr(data_dir, '/');
        char *last = (bs > fs) ? bs : fs;
        if (last) *last = '\0';
        else { data_dir[0] = '.'; data_dir[1] = '\0'; }
    }

    if (app.stream_port > 0) {
        if (stream_start(app.stream_port, data_dir) != 0)
            fprintf(stderr, "stream: failed to start on port %d\n", app.stream_port);
        /* 시작 즉시 door 활성 상태를 대시보드에 노출 — process_frame 첫 호출을 기다리지 않음 */
        stream_set_door_enabled(app.door.enabled);
    }

    /* 문 여닫이 기준 이미지 로드 — 닫힘/열림 기준 각각 로드합니다.
     * 파일 없으면 0 반환(에러 아님)이므로 그냥 계속 진행합니다. */
    {
        snprintf(app.door_closed_path, sizeof(app.door_closed_path),
                 "%s\\door_closed_reference.raw", data_dir);
        snprintf(app.door_open_path, sizeof(app.door_open_path),
                 "%s\\door_open_reference.raw", data_dir);
        if (door_load(&app.door, app.door_closed_path, app.door_open_path) != 0)
            fprintf(stderr, "door: failed to load reference\n");
        else {
            if (app.door.ref_closed_rgb)
                fprintf(stderr, "door: closed reference loaded %dx%d\n",
                        app.door.ref_closed_w, app.door.ref_closed_h);
            if (app.door.ref_open_rgb)
                fprintf(stderr, "door: open reference loaded %dx%d\n",
                        app.door.ref_open_w, app.door.ref_open_h);
        }
        app.door_closed_mtime = door_ref_mtime(app.door_closed_path);
        app.door_open_mtime = door_ref_mtime(app.door_open_path);
    }
    start = platform_monotonic_seconds();
    cpu_start = platform_process_cpu_seconds();
    /*
     * 실제 반복문은 media_process() 안에 있습니다. 프레임이 준비될 때마다 위의
     * process_frame 함수가 호출되고, 수정된 RGB 프레임이 곧바로 저장됩니다.
     */
    if (media_process(args.input, args.output, &args.media, process_frame, &app,
                      error, sizeof(error)) != 0) {
        fprintf(stderr, "processing failed: %s\n", error);
        goto done;
    }
    end = platform_monotonic_seconds();
    cpu_end = platform_process_cpu_seconds();
    {
        double seconds = end - start;
        double cpu_seconds = cpu_end - cpu_start;
        fprintf(stderr,
                "%s: %lld frames, %lld inference runs, %.2f s (%.2f fps), "
                "CPU %.2f s (%.2f ms/frame)\n",
                stop_requested ? "stopped" : "done",
                (long long)app.frames, (long long)app.inference_runs, seconds,
                seconds > 0.0 ? (double)app.frames / seconds : 0.0,
                cpu_seconds,
                app.frames > 0 ? cpu_seconds * 1000.0 / (double)app.frames
                               : 0.0);
        if (write_metrics(args.metrics_path, &args, &app, &media_stats,
                          seconds, cpu_seconds, error, sizeof(error)) != 0) {
            fprintf(stderr, "metrics failed: %s\n", error);
            goto done;
        }
    }
    result = EXIT_SUCCESS;

done:
    if (app.stream_port > 0) stream_stop();
    /*
     * C에는 자동 자원 정리가 없으므로 성공/실패 모두 이 지점을 거치게 합니다.
     * destroy 함수들은 NULL도 안전하게 받으므로 생성 도중 실패해도 호출 가능합니다.
     */
    if (app.detection_log) fclose(app.detection_log);
    if (app.keypoint_log)  fclose(app.keypoint_log);
    if (app.preview_pipe) pclose(app.preview_pipe);
    if (event_log_file && event_log_file != stderr) fclose(event_log_file);
    door_destroy(&app.door);
    rules_destroy(&app.rules);
    tracks_destroy(&app.tracks);
    camera_health_destroy(&app.cam_health);
    free(app.gray_prev);
    gray_buf_destroy(&app.gray);
    tracker_destroy(app.tracker);
    detector_destroy(app.detector);
    detection_list_destroy(&app.detections);
    return result;
}
