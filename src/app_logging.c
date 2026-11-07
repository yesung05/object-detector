/*
 * 로깅 v2: 실행 정보(run_info), 시간별 요약(summary), 설정 변경 기록(config_changed).
 *
 * 용량 원칙: 여기서 쓰는 기록은 전부 저빈도다 — 파일(시간)당 run_info 한 행(약 4KB), summary 이벤트 한 건(약 600바이트),
 * 설정을 저장할 때 config_changed 한 건. 프레임마다 쓰는 기록은 만들지 않는다.
 * 모든 함수는 메인 스레드에서만 부른다(이벤트 로그가 메인 스레드 전용이다).
 */
#include "app.h"

#include <ctype.h>
#include <stdarg.h>

/* ── JSON 조립 보조 ─────────────────────────────────────────────────────────── */

/* printf 형식으로 끝에 덧붙인다. 공간이 모자라면 아무것도 쓰지 않고 -1. */
static int jadd(char *buf, size_t cap, size_t *pos, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *pos) { buf[*pos] = '\0'; return -1; }
    *pos += (size_t)n;
    return 0;
}

/* 따옴표로 감싼 JSON 문자열. 제어문자는 공백으로 바꾼다(줄바꿈이 섞인 인수·경로 방어). 반환: 쓴 바이트 수, 공간 부족이면 0. */
size_t app_json_quote(char *out, size_t cap, const char *s) {
    size_t pos = 0;
    if (cap < 3) return 0;
    out[pos++] = '"';
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (pos + 4 >= cap) { out[0] = '\0'; return 0; }
        if (c == '"' || c == '\\') out[pos++] = '\\';
        out[pos++] = c < 0x20 ? ' ' : (char)c;
    }
    out[pos++] = '"';
    out[pos] = '\0';
    return pos;
}

static int jquote(char *buf, size_t cap, size_t *pos, const char *s) {
    size_t n = app_json_quote(buf + *pos, cap - *pos, s);
    if (!n) return -1;
    *pos += n;
    return 0;
}

/* PIN·비밀번호·토큰은 설정 사본에 남기지 않는다(로그 파일은 지원 자료로 복사될 수 있다). */
static int key_is_secret(const char *key) {
    static const char *const words[] = { "pin", "password", "passwd", "secret", "token" };
    char low[64];
    size_t i, w;
    for (i = 0; key[i] && i + 1 < sizeof(low); ++i) low[i] = (char)tolower((unsigned char)key[i]);
    low[i] = '\0';
    for (w = 0; w < sizeof(words) / sizeof(words[0]); ++w)
        if (strstr(low, words[w])) return 1;
    return 0;
}

/* 값이 JSON 숫자 모양(앞자리 0 없는 정수·소수)이면 따옴표 없이 쓴다. 대시보드가 저장한 숫자 설정이 문자열로 바뀌지 않게. */
static int looks_numeric(const char *v) {
    size_t i = 0, digits = 0;
    if (v[i] == '-') ++i;
    if (v[i] == '0' && v[i + 1] >= '0' && v[i + 1] <= '9') return 0;
    while (v[i] >= '0' && v[i] <= '9') { ++i; ++digits; }
    if (v[i] == '.') { ++i; while (v[i] >= '0' && v[i] <= '9') { ++i; ++digits; } }
    return digits > 0 && v[i] == '\0' && i < 24;
}

/* ── 실행 정보(run_info) ────────────────────────────────────────────────────── */

/*
 * 로그 파일마다 한 행: {"run","segment","static":{...},"effective":{...},"config":{...}}.
 *   static    시작 때 한 번 만든 조각(버전·빌드·인수·모델 해시) — app.manifest_static
 *   effective 지금 실제로 쓰는 주요 임계값(설정 파일에 없으면 기본값이 쓰이므로 파일만으로는 알 수 없는 값)
 *   config    설정 파일의 명시된 key=value 전부(비밀 키는 "***"). 12KB 를 넘으면 남는 키를 생략하고 truncated 표시.
 * 시간별 파일마다 다시 쓰는 이유: 파일 하나만 열어도 "어떤 설정에서 나온 기록인지" 알 수 있어야 한다(시간당 약 4KB).
 */
void app_log_run_info(AppContext *app, EventLog *events) {
    enum { CAP = 12000 };
    char *buf = (char *)malloc(CAP);     /* 이 함수 안에서만 쓰고 free — 스택에 12KB 를 올리지 않으려는 것 */
    size_t pos = 0;
    Config cfg = {0};
    int have_cfg = 0, truncated = 0;
    if (!buf) return;
    jadd(buf, CAP, &pos, "{\"run\":%lld,\"segment\":%d,\"static\":%s,", events->run_id, app->log_segment,
         app->manifest_static[0] ? app->manifest_static : "{}");
    jadd(buf, CAP, &pos,
         "\"effective\":{\"person_conf\":%.3f,\"object_conf\":%.3f,\"new_person_conf\":%.3f,\"table_conf\":%.3f,"
         "\"detect_every\":%d,\"detect_every_obj\":%d,\"tracking\":%d,\"async_infer\":%d,\"motion_gate\":%d,"
         "\"dwell_limit_s\":%.0f,\"unordered_grace_s\":%.0f,\"fall_hold_s\":%.1f,\"fall_ratio_kp\":%.2f,\"fall_ratio_nokp\":%.2f,"
         "\"fall_hold_max_misses\":%d,\"fight\":%d},",
         (double)app->person_confidence, (double)app->object_confidence, (double)app->new_person_confidence,
         (double)app->table_confidence, app->detect_every, app->detect_every_obj, app->tracking, app->infer_async,
         app->motion_gate_enabled, app->rules.config.dwell_limit_seconds, app->rules.config.unordered_grace_seconds,
         app->rules.config.fall_hold_seconds, (double)app->rules.config.fall_aspect_ratio_kp,
         (double)app->rules.config.fall_aspect_ratio_nokp, app->rules.config.fall_hold_max_misses, app->fight ? 1 : 0);
    jadd(buf, CAP, &pos, "\"config\":");
    if (app->config_reload_path[0] && config_load(&cfg, app->config_reload_path, NULL, 0) == 0) have_cfg = 1;
    if (!have_cfg) {
        jadd(buf, CAP, &pos, "null");
    } else {
        size_t i;
        int first = 1;
        jadd(buf, CAP, &pos, "{");
        for (i = 0; i < cfg.count; ++i) {
            size_t mark = pos;
            const char *k = cfg.keys[i], *v = cfg.values[i];
            if (!k || !v) continue;
            if (!first && jadd(buf, CAP, &pos, ",") != 0) { truncated = 1; break; }
            first = 0;
            if (jquote(buf, CAP, &pos, k) != 0 || jadd(buf, CAP, &pos, ":") != 0 ||
                (key_is_secret(k) ? jadd(buf, CAP, &pos, "\"***\"")
                                  : looks_numeric(v) ? jadd(buf, CAP, &pos, "%s", v) : jquote(buf, CAP, &pos, v)) != 0 ||
                pos + 64 > CAP) {
                pos = mark; buf[pos] = '\0'; truncated = 1; break;
            }
        }
        jadd(buf, CAP, &pos, "%s}", truncated ? ",\"truncated\":true" : "");
        config_destroy(&cfg);
    }
    jadd(buf, CAP, &pos, "}");
    if (event_log_write_run_info(events, buf) != 0)
        event_log_write(events, LOG_WARN, "logging", "run_info 를 저장하지 못했습니다(크기 상한 또는 DB 오류)");
    free(buf);
}

/* ── 시간별 요약(summary) ───────────────────────────────────────────────────── */

/*
 * 파일 하나(보통 1시간)를 몇 줄로 읽게 하는 요약 이벤트. 로그 전환 직전에 옛 파일에, 종료 때 마지막 파일에 남긴다.
 * 값은 이미 쌓인 events/perf 행과 메모리 카운터에서 모으므로 새 계측 비용이 없다.
 *   alerts   알림성 이벤트 총 건수, kinds 는 "종류:건수/서로 다른 트랙 수"를 건수 많은 순으로 최대 10개
 *   cam_bad_pct  카메라가 OK 가 아니었던 시간 비율, longest_track_s  지금 활성 트랙 중 가장 오래된 것
 *   cpu_med/cpu_max, fps_med(처리 프레임/초 중앙값), inf_ms_avg(추론 1회 평균), async_wait/async_drop
 */
void app_log_summary(AppContext *app, EventLog *events, int final) {
    EventKindCount kinds[10];
    PerfSummary ps;
    char kind_text[360], msg[760];
    size_t i, kp = 0;
    int nk, alerts = 0;
    /* 종료 요약은 프레임 문맥을 지운 뒤에 부르므로(shutdown 이벤트를 프레임 밖으로 기록) 마지막 프레임 시각을 쓴다. */
    double end_mono = events->ctx_valid ? events->ctx_mono : app->last_mono;
    double window = end_mono > 0.0 && app->summary_start_mono > 0.0 ? end_mono - app->summary_start_mono : 0.0;
    double longest = 0.0;
    if (!events->db) return;
    nk = event_log_kind_counts(events, app->summary_since_id, kinds, 10);
    for (i = 0; i < (size_t)nk; ++i) {
        alerts += kinds[i].count;
        if (kp + 48 < sizeof(kind_text))
            kp += (size_t)snprintf(kind_text + kp, sizeof(kind_text) - kp, "%s%s:%d/%d", i ? "," : "",
                                   kinds[i].kind, kinds[i].count, kinds[i].tracks);
    }
    if (nk == 0) snprintf(kind_text, sizeof(kind_text), "-");
    for (i = 0; i < app->tracks.count; ++i) {
        const Track *t = &app->tracks.items[i];
        if (t->active && end_mono - t->first_seen > longest) longest = end_mono - t->first_seen;
    }
    perf_log_summary(&app->perf_log, app->summary_perf_since, &ps);
    if (window < 0.0) window = 0.0;
    snprintf(msg, sizeof(msg),
             "summary window=%.0fs final=%d alerts=%d kinds=%s cam_bad_pct=%.1f longest_track_s=%.0f cpu_med=%d cpu_max=%d "
             "fps_med=%.1f inf_runs=%ld inf_ms_avg=%.1f async_wait=%ld async_drop=%ld perf_rows=%d",
             window, final ? 1 : 0, alerts, kind_text, window > 0.0 ? 100.0 * ps.cam_bad_s / window : 0.0, longest,
             ps.cpu_med, ps.cpu_max, ps.fps_med, ps.inf_runs, ps.inf_runs > 0 ? ps.inf_ms / (double)ps.inf_runs : 0.0,
             ps.wait_frames, ps.dropped, ps.rows);
    event_log_write(events, LOG_INFO, "summary", msg);
    /* 같은 파일에서 다시 요약해도(종료 직전 등) 이미 센 구간을 되풀이하지 않게 기준점을 앞으로 옮긴다. */
    app->summary_since_id = event_log_last_id(events);
    if (ps.last_id > app->summary_perf_since) app->summary_perf_since = ps.last_id;
    if (end_mono > 0.0) app->summary_start_mono = end_mono;
}

/* LogRotation 훅(log_rotation.h): 옛 파일에 요약, 새 파일에 실행 정보. 새 파일의 id 는 1부터라 기준점을 0 으로 되돌린다. */
void app_log_hook_before(void *context, EventLog *events) { app_log_summary((AppContext *)context, events, 0); }

void app_log_hook_after(void *context, EventLog *events) {
    AppContext *app = (AppContext *)context;
    app->summary_since_id = 0;
    app->summary_perf_since = 0;
    app->log_segment++;
    app_log_run_info(app, events);
}

/* ── 설정 변경 기록 ─────────────────────────────────────────────────────────── */

/*
 * 설정 파일을 다시 읽었을 때 바뀐 키만 한 건으로 남긴다("무엇이 언제부터 달라졌는가"). 저장 한 번에 한 건이라 빈도가 낮다.
 * 형식: config_changed n=<개수> <키>=<이전>-><이후> ... (최대 12개, 700바이트). 비밀 키는 값을 "***" 로 가린다.
 * 이전 내용은 app->last_cfg 에 보관하고, 비교가 끝나면 새 내용으로 교체한다(파일을 한 번 더 읽는 비용은 수 KB).
 */
void app_log_config_changed(AppContext *app, const Config *now_cfg) {
    char body[640], msg[700];
    size_t pos = 0, i;
    int changed = 0, shown = 0;
    body[0] = '\0';
    if (app->last_cfg_ready) {
        for (i = 0; i < now_cfg->count; ++i) {
            const char *k = now_cfg->keys[i], *nv = now_cfg->values[i], *ov = NULL;
            if (!k || !nv) continue;
            if (config_get(&app->last_cfg, k, &ov) == 0 && ov && strcmp(ov, nv) == 0) continue;
            ++changed;
            if (shown < 12 && pos + 100 < sizeof(body)) {
                int secret = key_is_secret(k);
                pos += (size_t)snprintf(body + pos, sizeof(body) - pos, " %s=%.40s->%.40s", k,
                                        secret ? "***" : (ov ? ov : "(none)"), secret ? "***" : nv);
                ++shown;
            }
        }
        for (i = 0; i < app->last_cfg.count; ++i) {
            const char *k = app->last_cfg.keys[i];
            if (k && config_get(now_cfg, k, NULL) != 0) {
                ++changed;
                if (shown < 12 && pos + 100 < sizeof(body)) {
                    pos += (size_t)snprintf(body + pos, sizeof(body) - pos, " %s=%.40s->(removed)", k,
                                            key_is_secret(k) ? "***" : (app->last_cfg.values[i] ? app->last_cfg.values[i] : ""));
                    ++shown;
                }
            }
        }
        if (changed > 0) {
            snprintf(msg, sizeof(msg), "config_changed n=%d%s", changed, body);
            event_log_write(&app->event_log, LOG_INFO, "config", msg);
        }
    }
    config_destroy(&app->last_cfg);
    app->last_cfg_ready = config_load(&app->last_cfg, app->config_reload_path, NULL, 0) == 0;
}
