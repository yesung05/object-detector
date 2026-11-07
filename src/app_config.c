/* main.c 에서 분리(app_config.c). 동작 변화 없이 옮긴 코드입니다. */
#include "app.h"

/*
 * config.json이 변경되면 AppContext 설정 필드를 새 값으로 교체합니다.
 * detector, tracker, media 설정 등 재시작이 필요한 항목은 제외하고,
 * 런타임에 안전하게 바꿀 수 있는 임계값들만 업데이트합니다.
 */
/*
 * 시작과 hot-reload 양쪽에서 같은 값을 읽도록 한 곳에 모읍니다.
 *
 * 예전에는 두 경로가 각각 config 를 파싱했고, 그 결과 reload_config() 가
 * 읽지 않는 키는 파일에 적어도 재시작 전까지 반영되지 않았습니다.
 * roi_kiosk 처럼 한쪽에만 추가되면 조용히 동작하지 않는 원인이 됩니다.
 */
/*
 * 카메라 장애 임계값. 시작과 hot-reload 양쪽에서 호출합니다.
 *
 * 예전에는 reload 에서만 읽고, 첫 프레임의 camera_health_init(NULL) 이 기본값으로
 * 덮어써서 config.json 의 값이 파일이 한 번 바뀌기 전까지 적용되지 않았습니다.
 * 키가 없을 때의 기본값도 두 경로가 달랐으므로(40/45 vs 12/150) camera_health 의
 * 기본값 하나로 통일합니다.
 */
void apply_camera_health_config(AppContext *app, const Config *cfg) {
    CameraHealthConfig d, *c = &app->cam_health_cfg;
    camera_health_default_config(&d);
    *c = d;
    c->luma_black_threshold =
        (int)config_long(cfg, "luma_black_threshold", d.luma_black_threshold, 0, 255);
    c->luma_white_threshold =
        (int)config_long(cfg, "luma_white_threshold", d.luma_white_threshold, 0, 255);
    c->frozen_frames_threshold =
        (int)config_long(cfg, "frozen_frames_threshold", d.frozen_frames_threshold, 1, 10000);
    c->low_contrast_ratio =
        config_float(cfg, "camera_low_contrast_ratio", d.low_contrast_ratio, 0.0f, 1.0f);
    c->low_contrast_hold_frames =
        (int)config_long(cfg, "camera_low_contrast_hold_frames",
                         d.low_contrast_hold_frames, 1, 100000);
    /* 이미 동작 중이면 연속 카운터는 두고 임계값만 교체합니다. */
    app->cam_health.config = *c;
}

/*
 * detections 에서 keep_score 미만 박스를 low_detections 로 옮깁니다.
 * reset=1 이면 low 목록을 비우고 시작합니다(추론 직후 첫 호출). 회전 재탐지가 박스를
 * 덧붙인 뒤에는 reset=0 으로 다시 불러 그 박스들도 나눕니다.
 */
void split_low_list(DetectionList *dets, DetectionList *low, float keep_score, int reset) {
    size_t i, kept = 0;
    if (keep_score <= 0.0f) return;
    if (reset) low->count = 0;
    for (i = 0; i < dets->count; ++i) {
        const Detection *d = &dets->items[i];
        if (d->score >= keep_score) {
            dets->items[kept++] = *d;
        } else if (low->count < low->capacity) {
            low->items[low->count++] = *d;
        }
    }
    dets->count = kept;
}

void rebuild_ignore_roi(AppContext *app) {
    int n = app->ignore_manual_count;
    int max = (int)(sizeof(app->ignore_roi) / sizeof(app->ignore_roi[0]));
    int i;
    for (i = 0; i < app->auto_ignore_count && n < max; ++i)
        app->ignore_roi[n++] = app->auto_ignore[i];
    app->ignore_roi_count = n;
}

void apply_gate_config(AppContext *app, const Config *cfg) {
    float low=config_float(cfg,"track_low_score",.10f,0,.9f);
    /* 기본 0.28: 실제 카페(필드 6일)의 식탁 검출 점수는 중앙값 0.28·최대 0.40 이라 COCO 검증 기준의 0.55 로는
     * 자동 등록이 한 번도 되지 않았다. 0.25~0.30 에서는 위치가 안정된 군집이 확인됐다. 오등록(선반·의자)은
     * 등록 조건(5회 이상·10초 이상·겹침 없음)과 사용자 수정/잠금으로 막는다. */
    float table=config_float(cfg,"table_confidence",.28f,.01f,.99f);
    app->person_confidence=config_float(cfg,"person_confidence",app->person_confidence,.01f,.99f);
    app->object_confidence=config_float(cfg,"object_confidence",app->object_confidence,.01f,.99f);
    app->new_person_confidence=config_float(cfg,"new_person_confidence",.30f,.01f,.99f);
    app->keep_score=low>0 && low<app->person_confidence ? app->person_confidence : 0;
    /* 워커가 추론 중일 수 있어 감지기에 직접 쓰지 않고, 다음 추론 작업이 실행 직전에 적용한다. */
    app->person_det_conf=app->keep_score>0?low:app->person_confidence;
    detector_set_confidence(app->obj_detector,app->object_confidence);
    app->tracks.new_track_min_score=fmaxf(app->person_confidence,app->new_person_confidence);
    app->reid_dwell_keep=(double)config_float(cfg,"reid_dwell_keep_seconds",300.0f,0.0f,86400.0f);
    app->tracks.reid_dwell_keep_seconds=app->reid_dwell_keep; /* hot-reload 즉시 적용. 시작 시에는 tracks_init 뒤에 다시 적용 */
    if(table!=app->table_confidence){memset(&app->table_mapper,0,sizeof(app->table_mapper));app->table_confidence=table;}

    app->block_gate_enabled = (int)config_long(cfg, "block_gate", 1, 0, 1);
    app->block_min_changed  = (int)config_long(cfg, "block_min_changed", 2, 1, 64);
    app->block_margin       = (int)config_long(cfg, "block_margin", 1, 0, 16);
    app->track_refresh_seconds =
        (double)config_float(cfg, "track_refresh_seconds", 5.0f, 0.5f, 120.0f);

    /* 설정은 x,y,w,h 로 적지만 블록 판정은 x1,y1,x2,y2 를 쓰므로
     * 프레임마다 변환하지 않도록 여기서 한 번만 바꿔 둡니다. */
    {
        ConfigRect raw[16];
        int n = config_rect_list(cfg, "ignore_roi", raw,
                                 (int)(sizeof(raw) / sizeof(raw[0])));
        int i;
        for (i = 0; i < n; ++i) {
            app->ignore_roi[i].x1 = raw[i].x;
            app->ignore_roi[i].y1 = raw[i].y;
            app->ignore_roi[i].x2 = raw[i].x + raw[i].w;
            app->ignore_roi[i].y2 = raw[i].y + raw[i].h;
        }
        app->ignore_manual_count = n;
        rebuild_ignore_roi(app);
    }

    app->active_hours_set =
        config_time_range(cfg, "active_hours",
                          &app->active_start_min, &app->active_end_min);

    /* 안정화 설정. 진행 중인 안정화는 시작 시점 값(MotionCalib.opt)을 유지합니다. */
    app->calib_opt.duration_seconds =
        60.0 * (double)config_float(cfg, "calibration_minutes", 10.0f, 0.0f, 240.0f);
    app->calib_opt.sample_seconds =
        (double)config_float(cfg, "calib_sample_seconds", 0.5f, 0.1f, 10.0f);
    app->calib_opt.noise_ratio =
        config_float(cfg, "calib_noise_ratio", 0.3f, 0.05f, 1.0f);
    app->calib_opt.max_area_ratio =
        config_float(cfg, "calib_max_area_ratio", 0.15f, 0.01f, 0.5f);
    app->calib_retry_seconds =
        60.0 * (double)config_float(cfg, "calib_retry_minutes", 30.0f, 0.0f, 1440.0f);
}

/*
 * 캡처 설정. 품질 기본값 85: 쓰러짐 확인용 증거 사진이라 스트림(75)보다
 * 조금 높게 둡니다. 720p 기준 한 장 60~150KB 수준입니다.
 */
void apply_capture_config(AppContext *app, const Config *cfg) {
    app->capture_fall    = (int)config_long(cfg, "capture_fall", 1, 0, 1);
    app->capture_quality = (int)config_long(cfg, "capture_quality", 85, 30, 100);
    if (app->capture.dir[0]) app->capture.quality = app->capture_quality;
}

void apply_residue_config(AppContext *app, const Config *cfg) {
    app->residue.config.enabled =
        (int)config_long(cfg, "residue_enabled", 1, 0, 1);
    app->residue.config.diff_threshold =
        (int)config_long(cfg, "residue_diff_threshold", 18, 1, 255);
    app->residue.config.min_blocks =
        (int)config_long(cfg, "residue_min_blocks", 3, 1, 256);
    app->residue.config.person_margin_blocks =
        (int)config_long(cfg, "residue_person_margin_blocks", 1, 0, 16);
    app->residue.config.confirm_seconds =
        (double)config_float(cfg, "residue_confirm_seconds", 60.0f, 1.0f, 3600.0f);
    app->residue.config.clear_seconds =
        (double)config_float(cfg, "residue_clear_seconds", 10.0f, 1.0f, 300.0f);
    app->residue.config.baseline_refresh_seconds =
        (double)config_float(cfg, "residue_baseline_refresh_seconds", 300.0f, 30.0f, 86400.0f);
    app->residue.config.global_change_ratio =
        config_float(cfg, "residue_global_change_ratio", 0.5f, 0.1f, 1.0f);
    app->residue.config.learn_background =
        (int)config_long(cfg, "residue_learn_background", 0, 0, 1);
    app->residue.config.owner_reach =
        config_float(cfg, "residue_owner_reach", 1.0f, 0.0f, 5.0f);
}

void apply_slot_config(AppContext *app, const Config *cfg) {
    slot_monitor_apply_config(&app->slot_mon, cfg);
    app->log_max_total_mb = config_long(cfg,"log_max_total_mb",10240,0,1048576);
    app->perf_log_interval =
        (double)config_long(cfg, "perf_log_interval_seconds",
                            (long)app->perf_log_interval, 10, 3600);
}

/* 보안·개인정보 설정. 시작 시와 hot-reload 양쪽에서 호출합니다.
 * PIN 은 config.json 에 평문입니다 — 파일을 읽을 수 있는 사람은 이미 기기를 장악한 셈이라
 * 해시로 얻는 것이 없고, 대시보드 서버(8080)도 같은 파일에서 같은 값을 읽어야 합니다. */
void apply_security_config(AppContext *app, const Config *cfg) {
    const char *pin = NULL;
    app->privacy_mode       = (int)config_long(cfg, "stream_privacy_mode", 1, 0, 1);
    app->privacy_unlock_max = (int)config_long(cfg, "privacy_unlock_max_seconds", 600, 30, 3600);
    if (config_get(cfg, "access_pin", &pin) == 0 && pin)
        snprintf(app->access_pin, sizeof(app->access_pin), "%s", pin);
    else
        app->access_pin[0] = '\0';
    {
        /* 화면 모양: 기본은 단색(SOLID). 엣지 스타일은 얼굴 윤곽선이 일부 남으므로(privacy_style.h) 명시적으로 켠다. */
        PrivacyParams pp = privacy_params_default();
        pp.style      = (int)config_long(cfg, "stream_privacy_style", PRIVACY_STYLE_SOLID, 0, 2);
        pp.cell       = (int)config_long(cfg, "privacy_mosaic_cell", 24, 8, 64);
        pp.edge_low   = (int)config_long(cfg, "privacy_edge_low", 40, 5, 250);
        pp.edge_high  = (int)config_long(cfg, "privacy_edge_high", 110, 5, 255);
        pp.edge_blur  = (int)config_long(cfg, "privacy_edge_blur", 8, 1, 8);
        pp.edge_soft  = (int)config_long(cfg, "privacy_edge_soft", 1, 0, 4);
        pp.edge_opacity = (int)config_long(cfg, "privacy_edge_opacity", 70, 10, 100);
        privacy_params_sanitize(&pp);
        app->privacy_style_fps = (int)config_long(cfg, "privacy_style_fps", 5, 1, 30);
        if (pp.style != app->privacy_params.style || pp.cell != app->privacy_params.cell ||
            pp.edge_low != app->privacy_params.edge_low || pp.edge_high != app->privacy_params.edge_high ||
            pp.edge_blur != app->privacy_params.edge_blur || pp.edge_soft != app->privacy_params.edge_soft ||
            pp.edge_opacity != app->privacy_params.edge_opacity) {
            char smsg[200];
            snprintf(smsg, sizeof(smsg), "privacy_style=%s cell=%d edge=%d/%d blur=%d soft=%d opacity=%d fps<=%d",
                     pp.style == PRIVACY_STYLE_THERMAL ? "thermal" : pp.style == PRIVACY_STYLE_EDGE_GRAY ? "edge_gray" : "solid", pp.cell, pp.edge_low,
                     pp.edge_high, pp.edge_blur, pp.edge_soft, pp.edge_opacity, app->privacy_style_fps);
            event_log_write(&app->event_log, LOG_INFO, "access", smsg);
            app->privacy_cache_time = 0; /* 새 모양이 바로 보이도록 캐시를 무효화 */
        }
        app->privacy_params = pp;
        capture_set_privacy_style(&app->capture, &pp);
    }
    stream_set_privacy_mode(app->privacy_mode);
    stream_set_privacy_unlock_max(app->privacy_unlock_max);
    stream_set_access_pin(app->access_pin);
}

/* 자동 기준 캡처 설정 — 문·잔류물·표면 공통. */
void apply_auto_capture_config(AppContext *app, const Config *cfg) {
    app->door.passage_enabled = (int)config_long(cfg,"door_passage_learning",1,0,1);
    app->door.auto_enabled           = (int)config_long(cfg, "door_auto_capture", 1, 0, 1);
    app->door.auto_quiet_seconds     = (double)config_float(cfg, "door_auto_quiet_seconds", 20.0f, 5.0f, 600.0f);
    app->door.auto_open_hold_seconds = (double)config_float(cfg, "door_auto_open_hold_seconds", 1.0f, 0.2f, 10.0f);
    app->door.auto_open_min_l1       = config_float(cfg, "door_auto_open_l1", 25.0f, 5.0f, 120.0f);
    {
        /* 비율이 바뀌면 밴드 영역 자체가 달라지므로 유효성을 다시 재야 합니다. */
        float ratio = config_float(cfg, "door_band_ratio", 0.2f, 0.0f, 0.8f);
        if (ratio != app->door.band_ratio) {
            app->door.band_ratio = ratio;
            app->door.band_valid = -1;
        }
    }
    app->residue.config.auto_enabled       = (int)config_long(cfg, "residue_auto_capture", 1, 0, 1);
    app->residue.config.auto_quiet_seconds = (double)config_float(cfg, "residue_auto_quiet_seconds", 15.0f, 3.0f, 600.0f);
    app->surface_auto_capture = (int)config_long(cfg, "surface_auto_capture", 1, 0, 1);
    app->surface_auto_quiet   = (double)config_float(cfg, "surface_auto_quiet_seconds", 10.0f, 3.0f, 600.0f);
    if (app->surface_monitor)
        surface_monitor_set_auto_capture(app->surface_monitor, app->surface_auto_capture, app->surface_auto_quiet);
}

/* 예약 재시작 설정. 시각 문자열이 잘못되면 기본값(04:00)을 쓰고 경고는 호출자가 남깁니다. */
void apply_restart_config(AppContext *app, const Config *cfg) {
    RestartConfig rc;
    const char *t = NULL;
    rc.enabled        = (int)config_long(cfg, "restart_enabled", 0, 0, 1);
    rc.days           = (int)config_long(cfg, "restart_days", 127, 0, 127);
    rc.window_minutes = (int)config_long(cfg, "restart_window_minutes", 60, 5, 720);
    rc.hour = 4; rc.minute = 0;
    if (config_get(cfg, "restart_time", &t) == 0 && t)
        restart_parse_time(t, &rc.hour, &rc.minute);
    restart_configure(&app->restart, &rc);
}

/* 적응형 감속 설정. 우선순위는 시작 시 한 번만 적용하고 여기서는 임계값만 다룹니다. */
void apply_throttle_config(AppContext *app, const Config *cfg) {
    ThrottleConfig tc;
    tc.enabled            = (int)config_long(cfg, "throttle_enabled", 1, 0, 1);
    tc.light_percent      = (int)config_long(cfg, "throttle_light_percent",   50, 10, 100);
    tc.heavy_percent      = (int)config_long(cfg, "throttle_heavy_percent",   70, 10, 100);
    tc.minimal_percent    = (int)config_long(cfg, "throttle_minimal_percent", 85, 10, 100);
    /* 히스테리시스와 측정 주기는 설정으로 내보내지 않습니다 — 잘못 만지면 단계가 흔들려
       오히려 불안정해지고, 현장에서 조절할 이유가 거의 없습니다.
       복귀 최소 유지시간을 5→30초로 올린 이유: 현장 로그에서 단계 전환 255회 중 177회가
       10초 이내 왕복이었습니다(others_cpu 69%→11%가 6초 만에 바뀜). 키오스크 부하가 짧게
       튀는 매장에서 5초 유지로는 detect_every 가 계속 x1↔x2 로 흔들립니다. 상승은 이
       값과 무관하게 즉시 반영되므로 키오스크 보호에는 영향이 없습니다. */
    tc.hysteresis_percent = 10;
    tc.min_level_seconds  = 30.0;
    tc.check_seconds      = 2.0;
    throttle_configure(&app->throttle, &tc);
}

/*
 * 알림 모드 설정: alert_<이벤트> = on | shadow | off. 기본은 모두 on(현재 동작). 시작 시와 hot-reload 양쪽에서
 * 호출하며, 바뀐 경우에만 startup 로그로 현재 상태를 남깁니다(그림자·꺼짐 알림이 왜 안 보이는지 로그로 알 수 있게).
 * 이벤트 종류를 목록으로 두는 이유: Config 에 키 열거 기능이 없고, 오타 키가 조용히 무시되는 것보다 알려진 목록만
 * 받는 편이 안전하다. 새 알림을 추가하면 이 목록에도 넣는다.
 */
static const char *const ALERT_TOKENS[] = {
    "overstay", "unordered_seated", "person_fallen", "person_unaccounted", "person_unaccounted_residue",
    "occupancy_exceeded", "person_motionless", "external_drink", "external_food", "animal_on_chair",
    "animal_on_table", "no_cup_seated", "residue_trash", "residue_spill", "residue_unknown", "door_open",
    "slot_item_urgent", "person_fight", "animal_present", "item_left",
};
/* 폭행 2단계 설정. 시작 시와 hot-reload 양쪽에서 호출한다.
 *   fight_enabled (0|1, 기본 0)        끄면 모델도 해제한다
 *   fight_model   (기본 models/fight_stage2.onnx)
 *   fight_threshold (기본 0.98386)     교차검증에서 정한 값(docs/fight-stage2-cv-2026-11-06.md). 바꾸면 모델 성능 근거가 달라진다
 *   fight_cooldown_seconds (기본 30)   발화 뒤 재발화 금지
 * 모델이 없거나 로드에 실패하면 기능을 끄고 이유를 로그로 남긴다(조용히 실패하지 않기). */
void apply_fight_config(AppContext *app, const Config *cfg) {
    int enabled = (int)config_long(cfg, "fight_enabled", 0, 0, 1);
    const char *model = NULL;
    char path[512], err[300] = {0}, msg[700];
    double thr = (double)config_float(cfg, "fight_threshold", 0.98386f, 0.5f, 0.9999f);
    app->fight_cooldown = (double)config_float(cfg, "fight_cooldown_seconds", 30.0f, 0.0f, 3600.0f);
    if (config_get(cfg, "fight_model", &model) != 0 || !model || !model[0]) model = "models/fight_stage2.onnx";
    snprintf(path, sizeof(path), "%s", model);
    if (!enabled) {
        if (app->fight) {
            fight_destroy(app->fight); app->fight = NULL;
            fight_classifier_destroy(app->fight_clf); app->fight_clf = NULL;
            app->fight_model[0] = '\0';
            event_log_write(&app->event_log, LOG_INFO, "rules", "fight_stage2 비활성 (fight_enabled=0)");
        }
        app->fight_status[0] = '\0';   /* 꺼져 있는 것이 기본이라 시작 로그는 남기지 않는다 */
        return;
    }
    if (app->fight && strcmp(path, app->fight_model) == 0) { fight_set_threshold(app->fight, thr); return; }
    fight_destroy(app->fight); app->fight = NULL;
    fight_classifier_destroy(app->fight_clf); app->fight_clf = NULL;
    app->fight_clf = fight_classifier_create(path, 1, err, sizeof(err));
    if (!app->fight_clf) {
        snprintf(msg, sizeof(msg), "fight_stage2 비활성 — 모델 로드 실패: %s (%s)", err, path);
        event_log_write(&app->event_log, LOG_WARN, "rules", msg);
        snprintf(app->fight_status, sizeof(app->fight_status), "%s", msg); app->fight_status_level = LOG_WARN;
        return;
    }
    app->fight = fight_create(thr, 1.0 / 3.0);
    if (!app->fight) {
        fight_classifier_destroy(app->fight_clf); app->fight_clf = NULL;
        event_log_write(&app->event_log, LOG_WARN, "rules", "fight_stage2 비활성 — 메모리 부족");
        return;
    }
    fight_set_scorer(app->fight, fight_classifier_score, app->fight_clf);
    snprintf(app->fight_model, sizeof(app->fight_model), "%s", path);
    snprintf(msg, sizeof(msg), "fight_stage2 활성 model=%s threshold=%.5f cooldown=%.0fs (AI-Hub 평가용 프로토타입, 알림 모드 alert_person_fight 로 on/shadow/off)",
             path, thr, app->fight_cooldown);
    event_log_write(&app->event_log, LOG_INFO, "rules", msg);
    snprintf(app->fight_status, sizeof(app->fight_status), "%s", msg); app->fight_status_level = LOG_INFO;
}

void apply_alert_config(AppContext *app, const Config *cfg) {
    char key[96], summary[400];
    size_t i;
    event_log_clear_alert_modes();
    for (i = 0; i < sizeof(ALERT_TOKENS) / sizeof(ALERT_TOKENS[0]); ++i) {
        const char *v = NULL;
        snprintf(key, sizeof(key), "alert_%s", ALERT_TOKENS[i]);
        /* animal_present 는 "동물 출입 금지" 매장만 쓰는 정책이라 키가 없으면 끈다. 기본 on 이면 반려동물
         * 허용 매장에서 첫날부터 알림이 쏟아진다. 켜져 있을 때만 전체 프레임 Tier 2 도 돌린다(아래 플래그). */
        int is_any = strcmp(ALERT_TOKENS[i], "animal_present") == 0;
        if (is_any) app->animal_present_on = 0;
        if (config_get(cfg, key, &v) != 0 || !v) {
            if (is_any) event_log_set_alert_mode(ALERT_TOKENS[i], ALERT_OFF);
            continue;
        }
        if (is_any && strcmp(v, "off") != 0) app->animal_present_on = 1;
        if (strcmp(v, "shadow") == 0) event_log_set_alert_mode(ALERT_TOKENS[i], ALERT_SHADOW);
        else if (strcmp(v, "off") == 0) event_log_set_alert_mode(ALERT_TOKENS[i], ALERT_OFF);
        else if (strcmp(v, "on") != 0) {
            char bad[200];
            snprintf(bad, sizeof(bad), "%s=%s 는 알 수 없는 값입니다(on|shadow|off) — on 으로 처리합니다", key, v);
            fprintf(stderr, "%s\n", bad);  /* 시작 시에는 이벤트 로그가 아직 없어 stderr 로도 남긴다 */
            event_log_write(&app->event_log, LOG_WARN, "startup", bad);
        }
    }
    event_log_alert_summary(summary, sizeof(summary));
    /* 이벤트 로그가 열려 있을 때만 요약을 남기고 기억한다(시작 시 호출은 로그가 열리기 전이라 건너뛰고, 로그가 열린
     * 뒤 startup 블록이 한 번 남긴다). */
    if (app->event_log.db && strcmp(summary, app->alert_summary) != 0) {
        char m[480];
        snprintf(m, sizeof(m), "alert_modes %s", summary[0] ? summary : "모두 on");
        event_log_write(&app->event_log, LOG_INFO, "startup", m);
        snprintf(app->alert_summary, sizeof(app->alert_summary), "%s", summary);
    }
}

/* 쓰러짐 설정. 시작 시와 hot-reload 양쪽에서 호출합니다. 기본 임계값은 rules_default_config() 한 곳에서 가져옵니다
 * (예전엔 여기 fallback 이 1.8/2.2 라 키가 없는 설정 파일 — config/store-launch.ini 등 — 에서 rules.c 기본 1.3/1.6 보다
 * 엄격하게 돌았고, AI-Hub test 쓰러짐 재현율이 11.1% vs 14.8% 로 갈렸습니다). 보강 기능은 기본 꺼짐(rules.h 이유 참조). */
void apply_fall_config(RulesConfig *rc, const Config *cfg) {
    const RulesConfig *d = rules_default_config();
    rc->fall_hold_seconds =
        (double)config_float(cfg, "fall_hold_seconds", (float)d->fall_hold_seconds, 1.0f, 60.0f);
    rc->fall_aspect_ratio_kp =
        config_float(cfg, "fall_aspect_ratio_kp", d->fall_aspect_ratio_kp, 0.8f, 5.0f);
    rc->fall_aspect_ratio_nokp =
        config_float(cfg, "fall_aspect_ratio_nokp", d->fall_aspect_ratio_nokp, 0.8f, 5.0f);
    rc->fall_hold_max_misses =
        (int)config_long(cfg, "fall_hold_max_misses", d->fall_hold_max_misses, 0, 10);
    rc->fall_handoff_seconds =
        (double)config_float(cfg, "fall_handoff_seconds", 0.0f, 0.0f, 30.0f);
    rc->fall_handoff_reach =
        config_float(cfg, "fall_handoff_reach", 1.0f, 0.1f, 3.0f);
    rc->fall_wide_ratio =
        config_float(cfg, "fall_wide_ratio", 0.0f, 0.0f, 5.0f);
    rc->fall_wide_min_score =
        config_float(cfg, "fall_wide_min_score", 0.25f, 0.05f, 0.99f);
}

/* 미확인 소실 임계값. 시작 시와 hot-reload 양쪽에서 같은 값을 쓰도록 한 곳에 모읍니다. */
void apply_vanish_config(RulesConfig *rc, const Config *cfg) {
    rc->vanish_enabled =
        (int)config_long(cfg, "vanish_enabled", 1, 0, 1);
    rc->vanish_hold_seconds =
        (double)config_float(cfg, "vanish_hold_seconds", 5.0f, 1.0f, 300.0f);
    rc->vanish_min_dwell_seconds =
        (double)config_float(cfg, "vanish_min_dwell_seconds", 3.0f, 0.0f, 300.0f);
    rc->vanish_min_score =
        config_float(cfg, "vanish_min_score", 0.35f, 0.0f, 1.0f);
    rc->vanish_edge_margin =
        config_float(cfg, "vanish_edge_margin", 0.08f, 0.0f, 0.4f);
    rc->vanish_require_residue =
        (int)config_long(cfg, "vanish_require_residue", 0, 0, 1);
}

/*
 * Tier 2 물체 룰·인원 초과·무동작 임계값. 시작과 hot-reload 양쪽에서 호출합니다.
 *
 * 한 곳에 모은 이유: 시작 경로는 RulesConfig 를 memset 0 으로 비운 뒤 일부 키만 읽어서,
 * animal_iou_threshold·no_cup_margin 이 reload 가 한 번 돌기 전까지 config.json 값이
 * 아니라 사용 시점의 폴백 값으로 동작했습니다. 새 키가 같은 함정에 빠지지 않게 합니다.
 */
void apply_object_rule_config(RulesConfig *rc, const Config *cfg) {
    rc->animal_iou_threshold =
        config_float(cfg, "animal_iou_threshold", 0.15f, 0.01f, 1.0f);
    rc->no_cup_margin =
        (int)config_long(cfg, "no_cup_margin", 1, 0, 100);
    /* 1 이하면 확정 필터가 꺼져 단발 관측으로 발화합니다. 기본 3-of-5. */
    rc->object_confirm_count =
        (int)config_long(cfg, "object_confirm_count", 3, 1, 16);
    rc->object_confirm_window =
        (int)config_long(cfg, "object_confirm_window", 5, 1, 16);
    /* 0 = 비활성. 적정 인원은 매장마다 달라 기본값을 두지 않습니다. */
    rc->max_occupancy =
        (int)config_long(cfg, "max_occupancy", 0, 0, 200);
    rc->max_occupancy_hold_seconds =
        (double)config_float(cfg, "max_occupancy_hold_seconds", 10.0f, 1.0f, 600.0f);
    /* 0 = 비활성 */
    rc->still_seconds =
        (double)config_float(cfg, "still_seconds", 300.0f, 0.0f, 86400.0f);
    rc->still_motion_threshold =
        config_float(cfg, "still_motion_threshold", 0.10f, 0.01f, 0.5f);
}

/* 키오스크 ROI 를 rules 설정에 채웁니다. 미설정이면 roi_kiosk_set = 0 이 되어
 * 주문 상태 전환 자체가 비활성화됩니다(호출자가 로그로 알립니다). */
void apply_roi_kiosk(RulesConfig *rc, const Config *cfg) {
    ConfigRect r;
    const char *anchor = NULL;
    /* "foot" 만 발밑, 그 외(미설정·오타)는 기존 동작인 중심으로 둡니다. */
    rc->roi_anchor_foot =
        (config_get(cfg, "roi_anchor", &anchor) == 0 && anchor &&
         strcmp(anchor, "foot") == 0) ? 1 : 0;
    if (config_rect(cfg, "roi_kiosk", &r)) {
        rc->roi_kiosk_x = r.x;
        rc->roi_kiosk_y = r.y;
        rc->roi_kiosk_w = r.w;
        rc->roi_kiosk_h = r.h;
        rc->roi_kiosk_set = 1;
    } else {
        rc->roi_kiosk_set = 0;
    }
}

void reload_config(AppContext *app) {
    Config cfg;
    char err[128] = {0};
    int previous_x=app->door.roi_x,previous_y=app->door.roi_y;
    int previous_w=app->door.roi_w,previous_h=app->door.roi_h;

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
    apply_gate_config(app, &cfg);

    /* 문 여닫이 설정 */
    app->door.enabled                = (int)config_long(&cfg, "door_enabled", 1, 0, 1);
    app->door.diff_threshold         = config_float(&cfg, "door_diff_threshold", 0.05f, 0.001f, 1.0f);
    app->door.confirm_frames         = (int)config_long(&cfg, "door_confirm_frames", 5, 1, 300);
    app->door.open_threshold_seconds = (double)config_float(&cfg, "door_open_seconds", 30.0f, 1.0f, 3600.0f);
    app->door.roi_x = (int)config_long(&cfg, "door_roi_x", 0, 0, 9999);
    app->door.roi_y = (int)config_long(&cfg, "door_roi_y", 0, 0, 9999);
    app->door.roi_w = (int)config_long(&cfg, "door_roi_w", 0, 0, 9999);
    app->door.roi_h = (int)config_long(&cfg, "door_roi_h", 0, 0, 9999);
    if (app->door.roi_w > 0 && app->door.roi_h > 0) app->door_roi_auto = 0;
    else if (app->door_mapper.ready) {
        app->door.roi_x=(int)(app->door_mapper.x1*app->door_mapper.width);
        app->door.roi_y=(int)(app->door_mapper.y1*app->door_mapper.height);
        app->door.roi_w=(int)((app->door_mapper.x2-app->door_mapper.x1)*app->door_mapper.width);
        app->door.roi_h=(int)((app->door_mapper.y2-app->door_mapper.y1)*app->door_mapper.height);
        app->door_roi_auto=1;
    }
    if(previous_x!=app->door.roi_x||previous_y!=app->door.roi_y||
       previous_w!=app->door.roi_w||previous_h!=app->door.roi_h){
        door_destroy(&app->door);app->door.last_state=-1;
        app->door.auto_quiet_since=app->door.auto_open_since=0;
        app->door.band_valid=-1;app->door.candidate_frames=0;
        stream_set_door_state(-1);
    }
    /* 설정 변경 즉시 대시보드에 반영 — process_frame을 기다리지 않고 바로 갱신 */
    if (app->stream_port > 0)
        stream_set_door_enabled(app->door.enabled);

    apply_camera_health_config(app, &cfg);

    /* 체류/쓰러짐 룰 임계값 */
    RulesConfig rules_cfg = app->rules.config; /* 기존값으로 초기화 (roi_kiosk 등 유지) */
    rules_cfg.dwell_limit_seconds =
        (double)config_float(&cfg, "dwell_limit_seconds", 3600.0f, 60.0f, 86400.0f);
    rules_cfg.unordered_grace_seconds =
        (double)config_float(&cfg, "unordered_grace_seconds", 300.0f, 0.0f, 3600.0f);
    apply_object_rule_config(&rules_cfg, &cfg);
    apply_vanish_config(&rules_cfg, &cfg);
    apply_fall_config(&rules_cfg, &cfg);
    apply_alert_config(app, &cfg);
    apply_fight_config(app, &cfg);
    apply_roi_kiosk(&rules_cfg, &cfg);
    rules_update_config(&app->rules, &rules_cfg);

    /* detect_every_obj 는 런타임 교체 가능 — 다음 프레임부터 바로 적용됩니다. */
    app->detect_every_obj =
        (int)config_long(&cfg, "detect_every_obj",
                         (long)app->detect_every_obj, 1, 10000);
    /* tier2_always: surface 활성 중에도 Tier 2를 강제 실행합니다.
     * 운영 환경에서는 0(기본값)으로 두어 CPU 부하를 아낍니다. */
    app->tier2_always = (int)config_long(&cfg, "tier2_always", 0, 0, 1);
    /* animal_crop: 테이블 영역 크롭 Tier 2. 기본 1 — 전체 프레임 320 입력에서 놓치는 작은 동물을 잡는
     * 가장 싼 방법이다(주기당 크롭 수만큼 추론 추가). CPU가 빠듯하면 0으로 끈다. */
    app->animal_crop = (int)config_long(&cfg, "animal_crop", 1, 0, 1);
    app->obj_box_ttl = (double)config_long(&cfg, "obj_box_ttl_seconds", 10, 1, 3600);

    apply_residue_config(app, &cfg);
    apply_slot_config(app, &cfg);
    apply_throttle_config(app, &cfg);
    apply_restart_config(app, &cfg);
    {
        int prev_privacy = app->privacy_mode;
        apply_security_config(app, &cfg);
        apply_auto_capture_config(app, &cfg);
        apply_capture_config(app, &cfg);
        if (prev_privacy != app->privacy_mode) {
            char msg[96];
            snprintf(msg, sizeof(msg), "stream_privacy_mode=%d (config reload)", app->privacy_mode);
            event_log_write(&app->event_log, LOG_INFO, "access", msg);
        }
    }

    /* Tier 2 바운딩 박스 표시 카테고리 — 체크박스로 토글 가능 */
    {
        uint32_t mask = 0;
        if (config_long(&cfg, "show_animal",    1, 0, 1)) mask |= OBJ_VIS_ANIMAL;
        if (config_long(&cfg, "show_food",      1, 0, 1)) mask |= OBJ_VIS_FOOD;
        if (config_long(&cfg, "show_drink",     1, 0, 1)) mask |= OBJ_VIS_DRINK;
        if (config_long(&cfg, "show_furniture", 1, 0, 1)) mask |= OBJ_VIS_FURNITURE;
        app->obj_vis_mask = mask;
    }

    config_destroy(&cfg);
    fprintf(stderr, "config: reloaded from %s\n", app->config_reload_path);
}

time_t door_ref_mtime(const char *path) {
    if (!path || !path[0]) return 0;
#if defined(_WIN32)
    struct _stat st;
    return _stat(path, &st) == 0 ? st.st_mtime : 0;
#else
    struct stat st;
    return stat(path, &st) == 0 ? (time_t)st.st_mtime : 0;
#endif
}

void reload_residue_reference(AppContext *app) {
    time_t mtime;
#if defined(_WIN32)
    struct _stat st;
    mtime = (_stat(app->residue_ref_path, &st) == 0) ? st.st_mtime : 0;
#else
    struct stat st;
    mtime = (stat(app->residue_ref_path, &st) == 0) ? (time_t)st.st_mtime : 0;
#endif
    if (mtime == app->residue_ref_mtime) return;
    if (residue_load(&app->residue, app->residue_ref_path) != 0) {
        fprintf(stderr, "residue: failed to reload reference\n");
        return;
    }
    app->residue_ref_mtime = mtime;
    if (app->residue.baseline_ready)
        fprintf(stderr, "residue: reference reloaded %dx%d\n",
                app->residue.baseline_w, app->residue.baseline_h);
}

void reload_door_references(AppContext *app) {
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
