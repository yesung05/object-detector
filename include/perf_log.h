#ifndef PERF_LOG_H
#define PERF_LOG_H

/*
 * 성능 지표 전용 로그 — 이벤트 로그와 별도 SQLite DB.
 *
 * 이벤트 로그(log.h)는 "무슨 일이 있었는가"를 기록하고,
 * 이 모듈은 "얼마나 빠르게/얼마나 많이 쓰고 있는가"를 기록합니다.
 * 두 데이터를 분리하는 이유: 대시보드 쿼리 분리, 보존 기간 차이,
 * 이벤트 뷰에 성능 행이 섞여 가독성을 해치는 문제 방지.
 */

/* SQLite 헤더는 구현 파일에서만 include합니다. */
typedef struct sqlite3      sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

typedef struct {
    sqlite3      *db;       /* 소유 — perf_log_close에서 해제 */
    sqlite3_stmt *stmt_ins; /* 미리 컴파일한 INSERT, db 수명과 동일 */
} PerfLog;

/*
 * path: DB 파일 경로. ":memory:"이면 인메모리(테스트·미지정 시).
 * 반환: 0=성공, -1=실패.
 */
int  perf_log_open(PerfLog *pl, const char *path);
void perf_log_close(PerfLog *pl);

/*
 * 성능 스냅샷 한 행을 기록합니다.
 *
 * cpu_pct:    프로세스 CPU 사용률 (단일 코어 기준 %). -1이면 측정 불가.
 * cpu_temp:   CPU 온도 (섭씨). -1이면 미지원.
 * cam_fps:    카메라 입력 FPS (EMA).
 * inf_fps:    YOLO 추론 FPS (EMA).
 * mem_kb:     프로세스 RSS (KB). -1이면 미지원.
 * gray_ms:    주기 내 gray 분석 누적 (ms).
 * door_ms:    주기 내 door_check 누적 (ms).
 * residue_ms: 주기 내 residue_evaluate 누적 (ms).
 * stream_ms:  주기 내 stream_push 누적 (ms).
 * draw_ms:    주기 내 draw 누적 (ms).
 * gate_l0~3:  주기 내 각 게이트 히트 프레임 수.
 * inf_runs:   주기 내 Tier 1 추론 실행 횟수.
 * obj_runs:   주기 내 Tier 2 추론 실행 횟수.
 *
 * 아래는 로깅 v2 에서 추가했다(옛 DB 는 열 때 ALTER 로 붙는다).
 * pre_ms/inf_ms/post_ms: 주기 내 Tier 1 전처리·추론·후처리 합계(ms). 비동기에서는 결과를 반영한 추론만 센다 — inf_runs 와 같은 기준이라
 *                        inf_ms / inf_runs 가 추론 한 번의 평균 시간.
 * t2_ms:      주기 내 Tier 2(전처리+추론+후처리) 합계.
 * track_ms:   주기 내 추적기 합계.
 * frames:     주기 내 처리한 프레임 수. cam_fps 는 이제 frames / 경과 시간이다(예전 값은 1/Δt 의 EMA 라 간격이 흔들리면 실제보다 크게 나왔다).
 * cam_bad_s:  주기 내 카메라 상태가 OK 가 아니었던 시간(초, 암전·정지 등).
 * wait_frames: 비동기 추론 요청이 워커가 바빠 대기한 프레임 수. dropped: 해상도가 바뀌어 버린 결과 수.
 */
typedef struct {
    int    cpu_pct;
    int    cpu_temp;
    double cam_fps;
    double inf_fps;
    long   mem_kb;
    double gray_ms;
    double door_ms;
    double residue_ms;
    double stream_ms;
    double draw_ms;
    long   gate_l0;
    long   gate_l1;
    long   gate_l2;
    long   gate_l3;
    long   inf_runs;
    long   obj_runs;
    double pre_ms;
    double inf_ms;
    double post_ms;
    double t2_ms;
    double track_ms;
    long   frames;
    double cam_bad_s;
    long   wait_frames;
    long   dropped;
} PerfMetrics;

void perf_log_write(PerfLog *pl, const PerfMetrics *m);

/*
 * since_id 뒤의 perf 행을 모아 시간별 요약용 수치를 낸다(최대 512행 — 1분 주기면 8시간치라 시간별 요약에는 충분).
 * 중앙값은 정렬해서 구한다(SQLite 에 percentile 이 없고, 행이 수십 개라 비용이 없다).
 */
typedef struct {
    int    rows;
    long   last_id;
    int    cpu_med, cpu_max;      /* 단일 코어 기준 % */
    double fps_med;               /* frames/초 */
    long   inf_runs, wait_frames, dropped, frames;
    double inf_ms, cam_bad_s;
} PerfSummary;
int perf_log_summary(PerfLog *pl, long long since_id, PerfSummary *out);

#endif /* PERF_LOG_H */
