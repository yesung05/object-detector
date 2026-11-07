/*
 * main.c 를 나눈 파일들(app_*.c)이 함께 쓰는 내부 헤더. include/ 에 두지 않은 이유: 외부 모듈이 쓰는
 * 공개 API 가 아니라 실행 파일 내부의 상태(AppContext)와 단계 함수이기 때문입니다.
 */
#ifndef APP_H
#define APP_H

#include "calib.h"
#include "camera_health.h"
#include "capture.h"
#include "privacy_style.h"
#include "perf_log.h"
#include "replay.h"
#include "log_rotation.h"
#include "slot_monitor.h"
#include "surface_monitor.h"
#include "throttle.h"
#include "restart.h"
#include "stream.h"
#include "door.h"
#include "door_mapper.h"
#include "table_mapper.h"
#include "residue.h"
#include "config.h"
#include "fight_stage2.h"
#include "fight_classifier.h"
#include "gray.h"
#include "log.h"
#include "media.h"
#include "model_select.h"
#include "platform.h"
#include "rules.h"
#include "tracker.h"
#include "tracks.h"
#include "yolo11.h"
#include "infer_worker.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>  /* _stat / stat: config.json mtime 감시용 */

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define popen _popen
#define pclose _pclose
#endif

/*
 * Tier1(사람·자세) 추론 한 번의 입력과 출력. 동기 모드와 비동기 모드가 같은 infer_job_run 을 써서 결과가 같습니다.
 *
 * 입력 뷰(rgb/luma):
 *   동기   — process_frame 의 frame 을 빌림. 콜백이 반환하면 무효. 해제 금지.
 *   비동기 — buf_rgb/buf_luma(이 구조체 소유, malloc)를 가리킴. 워커 idle 일 때만 main 이 다시 씀.
 * 출력 목록(dets/low):
 *   동기   — app->detections / app->low_detections 를 직접 가리킴(복사 없음 → 이전과 바이트 동일).
 *   비동기 — own_dets/own_low(이 구조체 소유). consume 시점에 app 목록으로 복사(포인터 교환이 아니라 복사:
 *            app 목록의 items 를 붙잡고 있는 코드가 있어도 영향이 없도록).
 * detector 는 AppContext 소유를 빌려 씀 — 비동기에서는 워커만 만지고, 해제는 워커 stop 이후 main.
 */
typedef struct {
    Detector      *detector;
    const uint8_t *rgb, *luma;
    int            width, height, stride, luma_stride;
    int64_t        index;
    double         now;               /* 추론 프레임 시각 — 룰은 이 시각으로 평가 */
    double         last_person_time;  /* 제출 시점 사본(워커가 AppContext 를 읽지 않게) */
    int            requested;         /* 추적기 요청(adaptive)으로 생긴 추론인가 — CSV kind 용 */
    float          conf, keep_score;
    DetectionList *dets, *low;
    DetectionList  own_dets, own_low;
    /* 회전 프레임 재탐지(쓰러짐 보조): 사람을 놓쳤고 10초 안에 사람이 있었으면 90° 돌려 다시 탐지.
     * 회전 버퍼는 처음 필요할 때 1회 할당, 이후 재사용. */
    uint8_t       *rotated_rgb;
    int            rotated_rgb_size;
    DetectionList  rotated_dets;
    uint8_t       *buf_rgb, *buf_luma;     /* 비동기 전용 프레임 사본. infer_job_free 가 free */
    size_t         buf_rgb_size, buf_luma_size;
    DetectorRunStats stats;
} InferJob;

/*
 * 프로그램 전체에서 한 번만 만들고 모든 프레임이 함께 쓰는 상태입니다.
 * detector와 detections를 재사용하므로 프레임마다 큰 메모리를 새로 만들지 않습니다.
 */
typedef struct {
    ReplayLog replay;
    LogRotation log_rotation;
    long log_max_total_mb;
    double log_cleanup_last;
    double replay_cpu_time,replay_cpu_wall,replay_cpu_pct,replay_tier2_at;
    long replay_mem_kb;
    int replay_geometry_revision;
    Detector *detector;
    DetectionList detections;
    /*
     * 2단계 매칭용 낮은 신뢰도 박스. 추론 직후 detections 에서 keep_score 미만을 옮겨 둡니다.
     * detections 에는 기준 이상만 남으므로 그리기·로그·규칙·잔류물 등 다른 소비자는
     * 예전과 똑같은 박스만 봅니다. 트랙을 잇는 데만 쓰입니다.
     * 소유: AppContext, detection_list_destroy 로 해제.
     */
    DetectionList low_detections;
    float person_confidence, object_confidence, new_person_confidence, table_confidence;
    float    keep_score;          /* 0 이면 2단계 매칭 비활성 (검출기 신뢰도를 낮추지 않음) */
    LightTracker *tracker;
    FILE *detection_log;
    FILE *keypoint_log;   /* --keypoints CSV (박스당 관절 좌표), NULL 이면 비활성 */
    FILE *fall_trace;     /* --fall-trace CSV. 소유: main(), 종료 시 fclose. NULL 이면 비활성 */
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
    int64_t gate_l0_hits;        /* 영업시간 밖이라 추론을 건너뛴 프레임 */
    int64_t gate_l1_hits;        /* 변화 자체가 없어 추론을 건너뛴 프레임 */
    int64_t gate_l2_hits;        /* 변화가 감시 제외 영역에만 있어 건너뛴 프레임 */
    int64_t gate_l3_hits;        /* 변화가 기존 트랙 안에만 있어 추적기로 대체한 프레임 */
    int64_t track_refresh_runs;  /* L3 상태에서 안전 밸브로 강제 추론한 횟수 */
    double  gray_seconds;        /* 그레이 변환 + 모션 게이트 + 카메라 헬스 */
    double  door_seconds;        /* door_check 픽셀 비교 */
    double  residue_seconds;     /* residue_evaluate 잔류 판정 */
    double  stream_seconds;      /* stream_push (복사 + 주기 판정) */
    int64_t drawn_frames;        /* 실제로 그린 프레임 수 (관찰자가 있던 프레임) */
    /* 성능 지표 로그 — 이벤트 로그와 별도 DB 파일(*_perf.db).
     * 1분마다 CPU%, 온도, 메모리, FPS, 모듈별 타이밍을 기록합니다. */
    PerfLog perf_log;
    double  perf_log_last;       /* 마지막 성능 로그 시각 (monotonic) */
    double  perf_log_interval;   /* 성능 로그 주기 (초), 기본 60 */
    double  perf_cpu_base;       /* 주기 시작 시점의 프로세스 CPU 시간 */
    /* 주기 내 누적값 스냅샷 — 주기 시작 시점 값과 차이를 로그에 기록 */
    double  perf_gray_base;
    double  perf_door_base;
    double  perf_residue_base;
    double  perf_stream_base;
    double  perf_draw_base;
    int64_t perf_gate_l0_base;
    int64_t perf_gate_l1_base;
    int64_t perf_gate_l2_base;
    int64_t perf_gate_l3_base;
    int64_t perf_inf_runs_base;
    int64_t perf_obj_runs_base;
    DetectorRunStats detector_stats;
    /* Tier 2 — 물체 감지 (고양이/강아지/음식/병/컵/의자/테이블) */
    Detector        *obj_detector;      /* NULL 이면 Tier 2 비활성 */
    DetectionList    obj_detections;    /* Tier 2 전용 결과 목록 — detections 와 별도 */
    DetectorRunStats obj_detector_stats;
    SurfaceMonitor *surface_monitor; /* AppContext owns; destroy at done. */
    DetectionList surface_detections;
    char surface_config_path[640];
    double surface_config_check, surface_people_at, surface_status_at;
    int surface_revision;
    TableMapper table_mapper;
    int table_mapping,table_mapping_revision;
    double table_mapping_at;
    double table_progress_log_at,table_stall_warn_at; /* 탐색 진행·정체 로그의 마지막 시각 */
    /* Re-ID 부활 체류 초기화(reid_dwell_keep_seconds). cfg 가 tracks_init 전에 해제되므로 여기 보관했다가
     * tracks_init 뒤에 TrackList 로 옮긴다. reid_resets_logged 는 마지막으로 로그에 낸 누적 횟수. */
    double reid_dwell_keep;unsigned long reid_resets_logged;
    /* 폭행 2단계(config fight_enabled, 기본 꺼짐). 소유: AppContext. fight_destroy / fight_classifier_destroy 로 종료·재설정 때 해제.
       fight_model 은 현재 로드된 모델 경로(변경 감지용), fight_seconds 는 fight_update 누적 시간(로그용). */
    FightMonitor *fight; FightClassifier *fight_clf; char fight_model[512];
    double fight_cooldown, fight_seconds, fight_stats_log_time;
    char fight_status[400]; int fight_status_level; /* 시작 시 설정을 읽을 때는 이벤트 로그가 아직 없어, 결과를 여기 두었다가 startup 로그로 낸다 */
    char alert_summary[400]; /* 마지막으로 로그에 낸 알림 모드 요약(바뀔 때만 다시 로그) */
    int              detect_every_obj;  /* Tier 2 실행 간격(프레임), 기본 90 */
    int              tier2_always;      /* 1=surface 활성 중에도 Tier 2 강제 실행 (개발용) */
    int              animal_present_on; /* alert_animal_present 가 on|shadow — 전체 프레임 Tier 2 필요 */
    double           obj_box_ttl;       /* Tier 2 박스 표시 수명(초), 기본 10 */
    float            obj_hide[16][5];   /* 수동 삭제 목록: class,x1,y1,x2,y2(정규화) — 값 소유 */
    int              obj_hide_count;
    char             obj_hide_path[600];
    int              animal_crop;       /* 1=등록된 테이블 영역을 잘라 Tier 2 추가 실행(기본 1) */
    int              table_box_count;   /* 값 소유 — surface_reload 가 갱신, 포인터 없음 */
    float            table_box[SURFACE_MAX][4]; /* 정규화 x1,y1,x2,y2 (여유 포함) */
    int64_t          obj_inference_runs;
    /* 이상 탐지 모듈 */
    GrayBuf      gray;           /* 현재 프레임 그레이스케일 (지연 초기화) */
    uint8_t     *gray_prev;      /* AppContext 소유, 직전 프레임 gray (freeze 판정) */
    /*
     * 마지막으로 추론한 시점의 gray 스냅샷입니다. 게이트는 "직전 프레임과
     * 달라졌는가"가 아니라 "지난번에 본 뒤로 달라졌는가"를 물어야 하므로
     * 별도 버퍼가 필요합니다. 다운샘플 8 기준 720p 에서 14KB 이므로
     * 두 벌을 들고 있어도 28KB 입니다.
     */
    uint8_t     *gray_ref;
    int          gray_ref_ready;
    int          gray_ready;     /* gray_prev 가 유효한지 여부 */
    int          gray_from_luma; /* 1이면 디코더 Y 평면 직접 사용, 0이면 RGB 폴백 */
    int          gray_path_logged;
    CameraHealth cam_health;     /* 카메라 장애 감지 */
    CameraHealthConfig cam_health_cfg; /* config.json 값 — 첫 프레임 초기화·재연결 시 사용 */
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
    /*
     * 감시 제외 영역. 창밖 도로, 간판 조명, 모니터 화면처럼 변화는 있지만
     * 감지 대상이 아닌 구역입니다. 변화가 전부 이 안에서만 일어났다면
     * 추론할 이유가 없습니다.
     */
    GrayRect     ignore_roi[16];   /* x1,y1,x2,y2 로 변환해 보관 */
    int          ignore_roi_count;
    /*
     * ignore_roi 는 [0, manual) 수동 설정 + [manual, count) 안정화 자동 결과입니다.
     * 두 출처를 따로 들고 있는 이유: config hot-reload 는 수동 부분만 다시 읽고,
     * 안정화 결과는 calibration.json 에서만 오므로 한쪽 갱신이 다른 쪽을 지우면 안 됩니다.
     */
    int          ignore_manual_count;
    GrayRect     auto_ignore[16];
    int          auto_ignore_count;
    /* 영업시간. 이 밖에서는 추론을 아예 하지 않고 카메라 헬스만 봅니다. */
    int          active_hours_set;
    int          active_start_min;
    int          active_end_min;
    /* 실시간 FPS — 지수 이동 평균(EMA).
     * 누적 평균은 시작 초기값의 영향을 오래 받으므로
     * EMA를 사용해 최근 몇 초간의 순간 FPS를 표시합니다. */
    double       realtime_inf_fps;  /* 추론 간격 EMA, α=0.3 */
    double       realtime_cam_fps;  /* 프레임 간격 EMA, α=0.15 */
    double       last_frame_time;   /* 직전 프레임 시각 (cam FPS EMA용) */
    /* HUD CPU 사용률 — 프로세스 CPU 시간 델타 / 경과 시간으로 계산합니다.
     * 1코어 기준 % 로 표시하므로 4코어 머신에서 4개 코어를 모두 쓰면 400%가 됩니다. */
    double       realtime_cpu_percent; /* 프로세스 CPU 점유율 EMA, α=0.2 */
    double       cpu_prev_cpu;         /* 직전 프레임 process_cpu_seconds */
    double       cpu_prev_wall;        /* 직전 프레임 monotonic_seconds */
    /* CPU 온도 캐시 (2초마다 갱신). -1이면 미지원. */
    int          cached_temperature;
    double       last_temp_check_time;
    /* YOLO 추론 FPS 상한. 0이면 제한 없음. detect_every와 OR 조건으로 동작.
     * 카메라 입력에서 추론이 느려도 디코딩 루프를 막지 않기 위해 사용. */
    double       detect_fps_limit;
    double       hud_start_time;  /* 첫 프레임 시각 (HUD FPS 분모) */
    int          stream_port;     /* MJPEG 스트림 포트, 0이면 비활성 */
    DoorMonitor    door;           /* 문 여닫이 감지 (door_reference.raw 필요) */
    DoorMapper door_mapper;
    int door_roi_auto, door_map_loaded;
    char door_map_path[600];
    ResidueMonitor residue;        /* 잔류물 감지 (residue_clean_reference.raw 필요) */
    SlotMonitor    slot_mon;       /* 슬롯 기반 테이블·의자 청결 감지 */
    uint32_t       obj_vis_mask;   /* Tier 2 표시 카테고리 마스크 (OBJ_VIS_*) */

    /* config.json hot-reload — 2초마다 mtime을 확인하고 변경 시 재로드합니다. */
    char   config_reload_path[512]; /* 재로드할 config 파일 경로 */
    time_t config_mtime;            /* 마지막으로 읽은 파일 수정 시각 */
    double config_check_time;       /* 마지막으로 mtime을 체크한 시각 */
    char   door_closed_path[600];
    char   door_open_path[600];
    time_t door_closed_mtime;
    time_t door_open_mtime;
    double door_refs_check_time;
    char   residue_ref_path[600];
    time_t residue_ref_mtime;
    double residue_ref_check_time;

    /* 보안·개인정보 (config: stream_privacy_mode, privacy_unlock_max_seconds, access_pin) */
    int    privacy_mode;
    int    privacy_unlock_max;
    char   access_pin[64];
    int    privacy_was_active;       /* 전환 로그용 */
    /* 보호 모드 화면 모양 (config: stream_privacy_style, privacy_mosaic_cell, privacy_edge_low/high, privacy_style_fps) */
    PrivacyParams   privacy_params;
    int             privacy_style_fps;      /* 관찰자가 있을 때 엣지 화면을 다시 그리는 최대 횟수/초 */
    /* 스크래치: 소유 AppContext, 첫 렌더 때 할당, 종료 시 privacy_scratch_destroy(NULL 안전).
       캐시: 소유 AppContext, 렌더 결과(RGB, 회색)의 사본. 해상도가 바뀔 때만 realloc, 종료 시 free. */
    PrivacyScratch *privacy_scratch;
    uint8_t        *privacy_cache;
    size_t          privacy_cache_size;
    int             privacy_cache_w, privacy_cache_h;
    double          privacy_cache_time;      /* 마지막 렌더 시각(monotonic) */
    unsigned long   privacy_renders;         /* 렌더 횟수(로그용) */
    unsigned long   privacy_reuses;          /* 캐시 재사용 횟수(로그용) */
    double          privacy_render_seconds;  /* 렌더 누적 시간(로그용) */
    int    door_stall_logged;        /* door.auto_stalled 0→1 전환을 한 번만 기록 */
    /* 문이 마지막으로 열린 시각. 소실 판정에서 "나갈 수 있었나"를 가르는 근거입니다. */
    double door_last_open_time;
    /* 적응형 감속 — 키오스크 본 프로그램이 바쁘면 스스로 추론을 줄입니다. */
    Throttle throttle;
    int      priority_level;   /* config: process_priority (0 보통, 1 보통아래, 2 유휴) */
    /* 예약 재시작 — 점주가 고른 요일·시각 */
    RestartScheduler restart;
    double   restart_check_time;
    char     restart_state_path[600];
    double   process_start_time;   /* uptime 계산용 — hud_start_time 은 첫 프레임에야 설정됩니다 */
    int    door_band_logged;         /* band_valid 측정 결과를 측정당 한 번만 기록 */
    /* 표면 자동 캡처 설정은 surface_monitor 로 넘기고, 문·잔류물은 각 모니터 구조체가 가집니다. */
    int    surface_auto_capture;
    double surface_auto_quiet;

    /* Tier1 추론 작업과 비동기 워커(--async-infer). 회전 재탐지 버퍼도 job 안에 있습니다. */
    InferJob       job;
    InferWorker    worker;
    int            infer_async;       /* 1 = 워커 스레드에서 추론, 0 = 프레임 콜백 안에서(파일 평가 재현성) */
    int            infer_pending;     /* 추론 요청이 왔는데 워커가 바빠 아직 못 넘김 — idle 되면 최신 프레임으로 */
    int64_t        infer_wait_frames; /* 요청이 대기한 프레임 수(워커가 따라가지 못하는 정도) */
    int64_t        infer_dropped_results; /* 해상도가 바뀌어 버린 결과 수(재연결) */
    int64_t        obj_next_index;    /* 비동기 Tier 2 주기 래치: 이 프레임 번호 이상인 추론 결과에서 실행 */
    float          person_det_conf;   /* 다음 추론에 쓸 Tier1 컷. hot-reload 가 여기만 쓰고 job 이 적용(워커와 경합 방지) */
    double         last_person_time;  /* 마지막으로 사람을 탐지한 시각 */
    /* luma 평면 없는 입력(RGB fallback) 대응: 원본 해상도 그레이스케일 버퍼.
     * frame->luma가 있으면 그대로 사용하고, 없으면 RGB→Gray 변환 결과를 씁니다. */
    uint8_t       *full_luma;         /* width×height 바이트, stride=width */
    int            full_luma_size;

    /* 설치 직후 안정화 — 반복 흔들림 구역 자동 학습 */
    MotionCalib  calib;
    CalibOptions calib_opt;
    char         calib_path[600];    /* 비어 있으면(--config 미지정) 기능 비활성 */
    double       calib_progress_log_time;
    /* 표본 부족으로 끝난 안정화의 재시도 예약(안정화 시계 기준). retry_at<=0 이면 예약 없음.
     * 첫 안정화가 매장이 붐비는 시간에 걸려 실패하면, 재시작 전까지 보정 없이 며칠을
     * 운영하게 되므로(현장 로그 6일) 프로세스 안에서 다시 시도합니다. */
    double       calib_retry_at;
    double       calib_retry_seconds;  /* 0 이면 재시도 안 함 */
    int          calib_retry_w, calib_retry_h;
    /*
     * 안정화 시계. 카메라는 벽시계, 영상 파일은 영상 시간(index / fps)을 씁니다.
     * 파일은 실시간보다 수백 배 빨리 디코딩되어 벽시계로는 10분 녹화본이
     * 몇 초 만에 끝나고, 녹화본으로 현장 안정화를 재현·검증할 수 없게 됩니다.
     */
    int          realtime_input;
    int          video_clock;        /* --video-clock: process_frame 의 now 를 영상 시간으로 */
    int          video_clock_warned; /* fps 를 몰라 벽시계로 대체했다고 한 번만 알림 */
    const MediaStats *media_stats;   /* main() 지역 변수를 빌려 씀 — media_process 동안 유효 */

    /* 이벤트 캡처. capture.dir 가 비어 있으면(--config 미지정·폴더 생성 실패) 비활성 */
    EventCapture capture;
    int          capture_fall;       /* config capture_fall, 기본 1 */
    int          capture_quality;    /* config capture_quality, 기본 85 */

    /* 카메라 재연결 */
    int          force_detect;       /* 재연결 직후 다음 프레임에서 한 번 추론 */
    int64_t      camera_attempt_logs;
    double       camera_down_at;     /* 끊김을 알린 시각 — 복귀 시 트랙 시각을 이만큼 미룹니다 */
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
    const char *fall_trace_path; /* --fall-trace: 프레임·트랙별 쓰러짐 게이트 CSV(오프라인 진단 전용) */
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
    /* Tier 2 물체 감지 */
    const char *obj_model;     /* --obj-model PATH, NULL=비활성 */
    int detect_every_obj;      /* --detect-every-obj N, 기본 90 */
    /* 오프라인 평가 전용(기본 꺼짐). 라이브 카메라 동작에는 영향이 없다. */
    int video_clock;           /* --video-clock: 룰 시간을 영상 시간(frame/fps)으로 */
    const char *infer_cache;   /* --infer-cache PATH: 추론 결과 기록·재생, NULL=꺼짐 */
    int async_infer;           /* --async-infer 0|1, -1 = 자동(카메라 1, 파일 0) */
} Arguments;


/* app_args.c */
int parse_arguments(int argc, char **argv, Arguments *args);
void usage(const char *program);
/* app_calib.c */
void calib_boot(AppContext *app, int frame_w, int frame_h, double now);
double calib_clock(const AppContext *app, const RgbFrame *frame, double now);
void calib_step(AppContext *app, double now);
double video_clock_now(AppContext *app, const RgbFrame *frame);
/* app_config.c */
void apply_alert_config(AppContext *app, const Config *cfg);
void apply_auto_capture_config(AppContext *app, const Config *cfg);
void apply_camera_health_config(AppContext *app, const Config *cfg);
void apply_capture_config(AppContext *app, const Config *cfg);
void apply_fall_config(RulesConfig *rc, const Config *cfg);
void apply_fight_config(AppContext *app, const Config *cfg);
void apply_gate_config(AppContext *app, const Config *cfg);
void apply_object_rule_config(RulesConfig *rc, const Config *cfg);
void apply_residue_config(AppContext *app, const Config *cfg);
void apply_restart_config(AppContext *app, const Config *cfg);
void apply_roi_kiosk(RulesConfig *rc, const Config *cfg);
void apply_security_config(AppContext *app, const Config *cfg);
void apply_slot_config(AppContext *app, const Config *cfg);
void apply_throttle_config(AppContext *app, const Config *cfg);
void apply_vanish_config(RulesConfig *rc, const Config *cfg);
time_t door_ref_mtime(const char *path);
void rebuild_ignore_roi(AppContext *app);
void reload_config(AppContext *app);
void reload_door_references(AppContext *app);
void reload_residue_reference(AppContext *app);
void split_low_list(DetectionList *dets, DetectionList *low, float keep_score, int reset);
/* app_output.c */
void apply_privacy_mask(AppContext *app, RgbFrame *frame, int has_observer);
void capture_fall_events(AppContext *app, const RgbFrame *frame);
int log_detections(AppContext *app, int64_t frame_index, const char *kind, char *error, size_t error_size);
int log_keypoints(AppContext *app, int64_t frame_index, char *error, size_t error_size);
void preview_frame(AppContext *app, const RgbFrame *frame);
const char *provider_name(DetectorProvider provider);
int write_metrics(const char *path, const Arguments *args, const AppContext *app, const MediaStats *media, double wall_seconds, double cpu_seconds, char *error, size_t error_size);
/* main.c */
int collect_person_rects(const AppContext *app, GrayRect *out, int *ids, int max);
int should_stop(void *opaque);

#endif
