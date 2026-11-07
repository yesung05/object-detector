/* main.c 에서 분리(app_calib.c). 동작 변화 없이 옮긴 코드입니다. */
#include "app.h"

/*
 * calibration.json 에서 이전 안정화 결과를 읽습니다.
 * 반환 1 = 적용함, 0 = 파일 없음·해상도 불일치 등으로 새로 안정화해야 함.
 *
 * 해상도를 확인하는 이유: ROI 는 원본 픽셀 좌표라 카메라 해상도가 바뀌면
 * 엉뚱한 곳을 무시하게 됩니다. 그 상태가 조용히 유지되는 것이 가장 위험합니다.
 */
static int calib_load_file(AppContext *app, int frame_w, int frame_h) {
    Config cfg;
    ConfigRect raw[16];
    const char *frame_text = NULL;
    const char *created = NULL;
    char msg[256];
    char err[128] = {0};
    int saved_w = 0, saved_h = 0, n, i;

    if (config_load(&cfg, app->calib_path, err, sizeof(err)) != 0) {
        snprintf(msg, sizeof(msg), "file=%s 없음 — 새로 안정화합니다", app->calib_path);
        event_log_write(&app->event_log, LOG_INFO, "calib", msg);
        return 0;
    }
    if (config_get(&cfg, "calib_frame", &frame_text) != 0 ||
        sscanf(frame_text, "%dx%d", &saved_w, &saved_h) != 2 ||
        saved_w != frame_w || saved_h != frame_h) {
        snprintf(msg, sizeof(msg),
                 "file=%s saved_frame=%s current_frame=%dx%d 불일치 — 다시 안정화합니다",
                 app->calib_path, frame_text ? frame_text : "(없음)", frame_w, frame_h);
        event_log_write(&app->event_log, LOG_WARN, "calib", msg);
        config_destroy(&cfg);
        return 0;
    }
    config_get(&cfg, "calib_created", &created);

    n = config_rect_list(&cfg, "auto_ignore_roi", raw, 16);
    for (i = 0; i < n; ++i) {
        app->auto_ignore[i].x1 = raw[i].x;
        app->auto_ignore[i].y1 = raw[i].y;
        app->auto_ignore[i].x2 = raw[i].x + raw[i].w;
        app->auto_ignore[i].y2 = raw[i].y + raw[i].h;
        snprintf(msg, sizeof(msg), "auto_ignore_roi_%d=%.0f,%.0f,%.0f,%.0f",
                 i + 1, raw[i].x, raw[i].y, raw[i].w, raw[i].h);
        event_log_write(&app->event_log, LOG_INFO, "calib", msg);
    }
    app->auto_ignore_count = n;
    rebuild_ignore_roi(app);

    snprintf(msg, sizeof(msg),
             "loaded file=%s created=%s auto_ignore_roi=%d ignore_roi_total=%d "
             "(재보정하려면 파일을 지우고 재시작)",
             app->calib_path, created ? created : "?", n, app->ignore_roi_count);
    event_log_write(&app->event_log, LOG_INFO, "calib", msg);
    config_destroy(&cfg);
    return 1;
}

/*
 * 임시 파일에 쓴 뒤 교체합니다. 쓰는 도중 전원이 나가도 반쯤 쓴 파일이
 * 남지 않고, 최악의 경우 파일이 없어 다음 시작 때 다시 안정화할 뿐입니다.
 * config.json 이 아니라 별도 파일인 이유: config.json 은 대시보드도 쓰는
 * 파일이라 두 프로세스가 동시에 쓰면 한쪽 저장이 사라질 수 있습니다.
 */
static int calib_save_file(const char *path, int frame_w, int frame_h,
                           const GrayRect *rects, int count, int samples) {
    char tmp[640];
    char created[32];
    time_t raw = time(NULL);
    struct tm tmv;
    FILE *f;
    int i;

#if defined(_WIN32)
    localtime_s(&tmv, &raw);
#else
    localtime_r(&raw, &tmv);
#endif
    strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%S", &tmv);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f,
            "{\n"
            "  \"calib_version\": 1,\n"
            "  \"calib_created\": \"%s\",\n"
            "  \"calib_frame\": \"%dx%d\",\n"
            "  \"calib_samples\": %d",
            created, frame_w, frame_h, samples);
    for (i = 0; i < count; ++i) {
        fprintf(f, ",\n  \"auto_ignore_roi_%d\": \"%.0f,%.0f,%.0f,%.0f\"",
                i + 1, rects[i].x1, rects[i].y1,
                rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1);
    }
    fprintf(f, "\n}\n");
    if (fclose(f) != 0) {
        remove(tmp);
        return -1;
    }
    /* Windows rename() 은 대상이 있으면 실패하므로 먼저 지웁니다. */
    remove(path);
    return rename(tmp, path) == 0 ? 0 : -1;
}

/*
 * 표본이 이보다 적으면 결과를 믿을 수 없어 저장하지 않습니다.
 * 0.5초 간격 기준 30초 분량입니다. 대부분 조명 변화로 표본이 버려졌거나
 * 영상이 너무 짧은 경우이고, 다음 시작 때 다시 시도하는 편이 안전합니다.
 */
#define CALIB_MIN_SAMPLES 60

static void calib_finish(AppContext *app, double now) {
    /* 문 + 키오스크 + 활성 가구 슬롯. 흔들려 보여도 무시하면 안 되는 구역입니다 —
     * 슬롯을 무시하면 그 테이블·의자의 청결 감지가 통째로 꺼집니다. */
    GrayRect protect[2 + SLOT_MAX];
    CalibRegion regions[16];
    char msg[256];
    int protect_count = 0, total = 0, n, i, slots;
    int frame_w = app->calib.frame_width, frame_h = app->calib.frame_height;
    int samples = app->calib.samples;

    if (samples < CALIB_MIN_SAMPLES) {
        /* 재시도에 쓸 해상도는 calib_destroy 가 지우기 전에 보관합니다. */
        app->calib_retry_w = frame_w;
        app->calib_retry_h = frame_h;
        app->calib_retry_at = app->calib_retry_seconds > 0.0 ? now + app->calib_retry_seconds : 0.0;
        if (app->calib_retry_at > 0.0)
            snprintf(msg, sizeof(msg),
                     "done 표본 부족 samples=%d skipped_global=%d (최소 %d) — "
                     "저장하지 않음, %.0f분 뒤 매장이 비면 재시도",
                     samples, app->calib.skipped_global, CALIB_MIN_SAMPLES,
                     app->calib_retry_seconds / 60.0);
        else
            snprintf(msg, sizeof(msg),
                     "done 표본 부족 samples=%d skipped_global=%d (최소 %d) — "
                     "저장하지 않음, calib_retry_minutes=0 이라 다음 시작 시 재시도",
                     samples, app->calib.skipped_global, CALIB_MIN_SAMPLES);
        event_log_write(&app->event_log, LOG_WARN, "calib", msg);
        calib_destroy(&app->calib);
        return;
    }
    app->calib_retry_at = 0.0; /* 성공 — 예약된 재시도가 있었다면 해제 */

    if (app->door.enabled && app->door.roi_w > 0 && app->door.roi_h > 0) {
        protect[protect_count].x1 = (float)app->door.roi_x;
        protect[protect_count].y1 = (float)app->door.roi_y;
        protect[protect_count].x2 = (float)(app->door.roi_x + app->door.roi_w);
        protect[protect_count].y2 = (float)(app->door.roi_y + app->door.roi_h);
        protect_count++;
    }
    if (app->rules.config.roi_kiosk_set) {
        protect[protect_count].x1 = app->rules.config.roi_kiosk_x;
        protect[protect_count].y1 = app->rules.config.roi_kiosk_y;
        protect[protect_count].x2 = app->rules.config.roi_kiosk_x + app->rules.config.roi_kiosk_w;
        protect[protect_count].y2 = app->rules.config.roi_kiosk_y + app->rules.config.roi_kiosk_h;
        protect_count++;
    }
    for (i = 0; i < app->slot_mon.count && i < SLOT_MAX; ++i)
        protect[protect_count++] = app->slot_mon.slots[i].bbox;

    n = calib_extract(&app->calib, protect, protect_count, regions, 16, &total);
    slots = 16 - app->ignore_manual_count;
    app->auto_ignore_count = 0;
    for (i = 0; i < n; ++i) {
        const CalibRegion *r = &regions[i];
        const GrayRect *b = &r->rect;
        int accepted = r->status == CALIB_REGION_OK && app->auto_ignore_count < slots;
        const char *reason = r->status != CALIB_REGION_OK
                                 ? calib_region_status_name(r->status)
                                 : (accepted ? "ok" : "no_slot");
        snprintf(msg, sizeof(msg),
                 "region %s rect=%.0f,%.0f,%.0f,%.0f blocks=%d ratio=%.2f",
                 accepted ? "accepted" : "rejected",
                 b->x1, b->y1, b->x2 - b->x1, b->y2 - b->y1,
                 r->blocks, r->mean_ratio);
        if (!accepted) {
            size_t len = strlen(msg);
            snprintf(msg + len, sizeof(msg) - len, " reason=%s", reason);
        }
        event_log_write(&app->event_log, accepted ? LOG_INFO : LOG_WARN, "calib", msg);
        if (accepted) app->auto_ignore[app->auto_ignore_count++] = *b;
    }
    if (total > n) {
        snprintf(msg, sizeof(msg), "흔들림 구역 %d개 중 작은 %d개는 상한(16) 초과로 버림",
                 total, total - n);
        event_log_write(&app->event_log, LOG_WARN, "calib", msg);
    }
    rebuild_ignore_roi(app);

    if (calib_save_file(app->calib_path, frame_w, frame_h, app->auto_ignore,
                        app->auto_ignore_count, samples) != 0) {
        snprintf(msg, sizeof(msg),
                 "save failed file=%s — 이번 실행에만 적용, 다음 시작 시 재안정화",
                 app->calib_path);
        event_log_write(&app->event_log, LOG_ERROR, "calib", msg);
    }

    snprintf(msg, sizeof(msg),
             "done samples=%d skipped_global=%d found=%d auto_ignore_roi=%d "
             "ignore_roi_total=%d protected=%d file=%s",
             samples, app->calib.skipped_global, total, app->auto_ignore_count,
             app->ignore_roi_count, protect_count, app->calib_path);
    event_log_write(&app->event_log, LOG_INFO, "calib", msg);
    calib_destroy(&app->calib);
}

double calib_clock(const AppContext *app, const RgbFrame *frame, double now) {
    double fps = app->media_stats ? app->media_stats->frame_rate : 0.0;
    if (app->realtime_input || fps <= 0.0) return now;
    return (double)frame->index / fps;
}

/*
 * 영상 시간 모드(--video-clock)의 now. 파일을 실시간보다 빨리 돌리면 벽시계 기준 룰(쓰러짐 유지 시간,
 * 체류 시간, 소실 판정 등)이 영상과 어긋나므로, 오프라인 평가에서는 프레임 번호/fps 로 시간을 만든다.
 * 같은 계산이 calib_clock 에도 있지만 그쪽은 보정 전용이라 오프셋이 없어 따로 둔다.
 * fps 를 알 수 없으면 벽시계로 대체하고 한 번만 알린다(조용히 틀린 시간으로 평가하지 않기 위해).
 */
double video_clock_now(AppContext *app, const RgbFrame *frame) {
    double fps = app->media_stats ? app->media_stats->frame_rate : 0.0;
    double t = platform_video_seconds(frame->index, fps);
    if (t < 0.0) {
        if (!app->video_clock_warned) {
            app->video_clock_warned = 1;
            fprintf(stderr, "video-clock: input frame rate unknown, falling back to wall clock\n");
            event_log_write(&app->event_log, LOG_WARN, "startup",
                            "video_clock: 입력 fps를 알 수 없어 벽시계로 대체합니다 — 룰 시간이 영상과 맞지 않습니다");
        }
        return platform_monotonic_seconds();
    }
    return t;
}

/* 첫 프레임에서 한 번 호출합니다. 해상도는 프레임이 와야 알 수 있기 때문입니다. */
void calib_boot(AppContext *app, int frame_w, int frame_h, double now) {
    char msg[256];
    char err[128] = {0};

    if (app->calib_opt.duration_seconds <= 0.0) {
        event_log_write(&app->event_log, LOG_INFO, "calib",
                        "calibration_minutes=0 — 자동 무시 구역 비활성");
        return;
    }
    /* --config 없이 돌리는 영상 파일 테스트에서 작업 디렉터리에
     * calibration.json 이 생기는 부작용을 막습니다. */
    if (!app->calib_path[0]) {
        event_log_write(&app->event_log, LOG_INFO, "calib",
                        "--config 미지정 — 결과 저장 위치가 없어 안정화 비활성");
        return;
    }
    if (calib_load_file(app, frame_w, frame_h)) return;

    if (calib_start(&app->calib, &app->gray, frame_w, frame_h, &app->calib_opt,
                    now, err, sizeof(err)) != 0) {
        snprintf(msg, sizeof(msg), "start failed: %s", err);
        event_log_write(&app->event_log, LOG_ERROR, "calib", msg);
        return;
    }
    app->calib_progress_log_time = now;
    snprintf(msg, sizeof(msg),
             "start duration=%.1fmin sample=%.1fs noise_ratio=%.2f max_area=%.2f "
             "grid=%dx%d — 빈 매장 상태에서 진행해야 정확합니다",
             app->calib_opt.duration_seconds / 60.0, app->calib_opt.sample_seconds,
             app->calib_opt.noise_ratio, app->calib_opt.max_area_ratio,
             app->calib.blocks_x, app->calib.blocks_y);
    event_log_write(&app->event_log, LOG_INFO, "calib", msg);
}

/* 매 프레임 호출. 비활성이면 즉시 반환합니다. */
void calib_step(AppContext *app, double now) {
    GrayRect people[64];
    int n;
    if (!calib_active(&app->calib)) {
        /* 예약된 재시도: 시각이 지났어도 활성 트랙(limbo 제외)이 있으면 60초 미룹니다.
         * 사람이 있는 채로 시작하면 같은 이유로 또 표본 부족이 되기 때문입니다.
         * 시작 자체는 calib_boot 가 합니다 — 파일이 이미 생겼으면 그쪽에서 불러오고 끝납니다. */
        if (app->calib_retry_at > 0.0 && now >= app->calib_retry_at) {
            size_t i;
            int busy = 0;
            for (i = 0; i < app->tracks.count; ++i)
                if (app->tracks.items[i].active) { busy = 1; break; }
            if (busy) {
                app->calib_retry_at = now + 60.0;
            } else {
                app->calib_retry_at = 0.0;
                event_log_write(&app->event_log, LOG_INFO, "calib", "재시도 시작 — 활성 트랙 없음");
                calib_boot(app, app->calib_retry_w, app->calib_retry_h, now);
            }
        }
        return;
    }

    n = collect_person_rects(app, people, NULL, 64);
    calib_tick(&app->calib, &app->gray, people, n, app->block_min_changed, now);

    if (now - app->calib_progress_log_time >= 60.0) {
        char msg[160];
        app->calib_progress_log_time = now;
        snprintf(msg, sizeof(msg),
                 "progress=%.1f/%.1fmin samples=%d skipped_global=%d people=%d",
                 (now - app->calib.started) / 60.0,
                 app->calib.opt.duration_seconds / 60.0,
                 app->calib.samples, app->calib.skipped_global, n);
        event_log_write(&app->event_log, LOG_INFO, "calib", msg);
    }
    if (calib_elapsed_done(&app->calib, now)) calib_finish(app, now);
}
