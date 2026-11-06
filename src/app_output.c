/* main.c 에서 분리(app_output.c). 동작 변화 없이 옮긴 코드입니다. */
#include "app.h"

const char *provider_name(DetectorProvider provider) {
    return provider == DETECTOR_PROVIDER_DIRECTML ? "directml" : "cpu";
}

int log_detections(AppContext *app, int64_t frame_index,
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
int log_keypoints(AppContext *app, int64_t frame_index,
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

void preview_frame(AppContext *app, const RgbFrame *frame) {
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
 * 이번 추론에서 새로 발화한 쓰러짐마다 프레임 한 장을 저장합니다.
 *
 * 호출 시점이 그리기·보호 모드 가림 전이라 frame->data 는 원본입니다. 보호 모드가
 * 켜져 있으면 capture_save 가 영상을 지우고 박스·관절만 남깁니다(출력 정책과 동일).
 *
 * JPEG 인코딩(720p 기준 수십 ms)을 처리 스레드에서 동기로 하는 이유:
 * 쓰러짐은 트랙당 한 번만 발화(latch)하므로 드물고, 그때 한두 프레임 늦는
 * 것보다 별도 스레드·프레임 복사 큐를 유지하는 비용이 더 큽니다.
 */
/*
 * 보호 모드 프레임 가림. SOLID 는 단색 채움(기존 동작). EDGE_GRAY 는 프레임 전체에 모자이크·가우시안·엣지를 계산하므로
 * 프레임마다 하면 CPU 를 많이 쓴다. 그래서:
 *   - 보는 사람이 없을 때(스트림 클라이언트·미리보기·출력 파일 없음)는 1초에 한 번만 렌더한다 — /snapshot 용으로
 *     stream_push 가 1fps 로 복사하는 화면이 이 결과와 같아 스냅샷이 단색으로 깜박이지 않는다.
 *   - 보는 사람이 있으면 privacy_style_fps(기본 5)까지 렌더한다. 박스·스켈레톤은 매 프레임 이 화면 위에 새로 그려지므로
 *     움직임은 매끄럽고, 배경 윤곽만 5fps 로 갱신된다.
 *   - 렌더하지 않는 프레임은 캐시 사본을 덮어쓴다(memcpy). 어떤 경로로도 원본 픽셀이 프레임에 남지 않는다.
 * 어느 단계가 실패해도 단색으로 덮는다(실패해도 원본이 나가지 않는 쪽이 안전).
 */
void apply_privacy_mask(AppContext *app, RgbFrame *frame, int has_observer) {
    size_t row = (size_t)frame->width * 3, need = row * (size_t)frame->height;
    double now, interval;
    int y;
    if (app->privacy_params.style == PRIVACY_STYLE_SOLID) {
        int gray = app->privacy_params.solid_gray > 0 ? app->privacy_params.solid_gray : 0x24;
        for (y = 0; y < frame->height; y++)
            memset(frame->data + (size_t)y * frame->stride, gray, row);
        return;
    }
    now = platform_monotonic_seconds();
    interval = has_observer ? 1.0 / (app->privacy_style_fps > 0 ? app->privacy_style_fps : 5) : 1.0;
    if (app->privacy_cache && app->privacy_cache_w == frame->width && app->privacy_cache_h == frame->height &&
        app->privacy_cache_time > 0 && now - app->privacy_cache_time < interval) {
        for (y = 0; y < frame->height; y++)
            memcpy(frame->data + (size_t)y * frame->stride, app->privacy_cache + (size_t)y * row, row);
        app->privacy_reuses++;
        return;
    }
    if (!app->privacy_scratch) app->privacy_scratch = privacy_scratch_create();
    {
        double t0 = platform_monotonic_seconds(), dt;
        int rc = privacy_style_apply(app->privacy_scratch, frame->data, frame->width, frame->height,
                                     frame->stride, &app->privacy_params);
        dt = platform_monotonic_seconds() - t0;
        app->privacy_render_seconds += dt;
        app->privacy_renders++;
        if (rc != 0) {
            /* privacy_style_apply 는 실패 시 이미 단색으로 덮었다. 캐시는 무효화해 다음 프레임에 다시 시도한다. */
            event_log_write(&app->event_log, LOG_ERROR, "access",
                            "privacy_style 렌더 실패 — 단색으로 대체 (메모리 부족 또는 프레임이 너무 작음)");
            app->privacy_cache_time = 0;
            return;
        }
        if (app->privacy_renders == 1 || app->privacy_renders % 600 == 0) {
            char m[200];
            snprintf(m, sizeof(m), "privacy_style 렌더 %lux 평균 %.1fms (최근 %.1fms) 캐시 재사용 %lux",
                     app->privacy_renders, app->privacy_render_seconds / (double)app->privacy_renders * 1000.0,
                     dt * 1000.0, app->privacy_reuses);
            event_log_write(&app->event_log, LOG_INFO, "access", m);
        }
    }
    if (!app->privacy_cache || app->privacy_cache_size < need) {
        uint8_t *nb = (uint8_t *)realloc(app->privacy_cache, need);
        if (!nb) { app->privacy_cache_time = 0; return; }   /* 캐시만 못 만든다: 이번 프레임은 이미 가려져 있다 */
        app->privacy_cache = nb;
        app->privacy_cache_size = need;
    }
    for (y = 0; y < frame->height; y++)
        memcpy(app->privacy_cache + (size_t)y * row, frame->data + (size_t)y * frame->stride, row);
    app->privacy_cache_w = frame->width;
    app->privacy_cache_h = frame->height;
    app->privacy_cache_time = now;
}

void capture_fall_events(AppContext *app, const RgbFrame *frame) {
    int i;
    if (app->rules.fall_fired_count <= 0) return;
    if (!app->capture_fall || app->replay.ring) return; /* Trial evidence is coordinates only. */
    if (!app->capture.dir[0]) {
        event_log_write(&app->event_log, LOG_WARN, "capture",
                        "person_fallen 발생했지만 캡처 비활성 (--config 미지정 또는 폴더 생성 실패)");
        return;
    }
    for (i = 0; i < app->rules.fall_fired_count; ++i) {
        char path[720];
        char err[256] = {0};
        char msg[900];
        int track = app->rules.fall_fired_track[i];
        if (capture_save(&app->capture, "person_fallen", track,
                         frame->data, frame->width, frame->height, frame->stride,
                         &app->rules.fall_fired_box[i], app->privacy_mode,
                         path, sizeof(path), err, sizeof(err)) == 0) {
            snprintf(msg, sizeof(msg), "kind=person_fallen track=%d privacy=%s file=%s",
                     track, app->privacy_mode ? "masked" : "raw", path);
            event_log_write(&app->event_log, LOG_INFO, "capture", msg);
        } else {
            snprintf(msg, sizeof(msg), "kind=person_fallen track=%d failed: %s",
                     track, err);
            event_log_write(&app->event_log, LOG_ERROR, "capture", msg);
        }
    }
}

int write_metrics(const char *path, const Arguments *args,
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
            "  \"gate_l0_hits\": %lld,\n"
            "  \"gate_l1_hits\": %lld,\n"
            "  \"gate_l2_hits\": %lld,\n"
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
            "  \"residue_seconds\": %.6f,\n"
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
            (long long)app->gate_l0_hits,
            (long long)app->gate_l1_hits,
            (long long)app->gate_l2_hits,
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
            app->gray_seconds, app->door_seconds, app->residue_seconds,
            app->stream_seconds,
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
