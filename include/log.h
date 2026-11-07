#ifndef EVENT_LOG_H
#define EVENT_LOG_H

/*
 * 구조화 이벤트 로그 — SQLite 백엔드.
 * 이벤트는 events 테이블에 저장되고, 실시간 확인용으로 stderr에도 동시 출력합니다.
 * path=":memory:" 로 열면 인메모리 DB (단위 테스트용).
 *
 * 새 기능을 추가할 때 정상/비정상 상태를 모두 기록하여 코드 없이 원인을 파악할 수
 * 있게 합니다 (CLAUDE.md 로깅 요구사항).
 */

typedef enum { LOG_INFO = 0, LOG_WARN = 1, LOG_ERROR = 2 } LogLevel;

/* SQLite 헤더는 구현 파일에서만 include합니다.
 * 외부에는 불투명 포인터로 노출합니다. */
typedef struct sqlite3       sqlite3;
typedef struct sqlite3_stmt  sqlite3_stmt;

typedef struct {
    sqlite3      *db;         /* 소유 — event_log_close 에서 해제 */
    sqlite3_stmt *stmt_ins;   /* 미리 컴파일한 INSERT 구문, db 수명과 동일 */
    LogLevel      min_level;
    void (*observer)(void *, long long, LogLevel, const char *, const char *);
    void *observer_context;
    int           echo_stderr; /* 1이면 SQLite 저장과 동시에 stderr 출력 */
    /* 분석용 부가 열(events.run/mono/frame). 시간별 파일 전환이 EventLog 를 통째로 바꾸므로 log_rotation.c 가 복사한다. */
    long long     run_id;      /* 프로세스 시작 시각(epoch 초). 0 이면 run 열을 NULL 로 */
    long long     ctx_frame;   /* 지금 처리 중인 프레임 번호 */
    double        ctx_mono;    /* 그 프레임의 룰 시각(실시간=단조 시계, --video-clock=영상 시간) — replay_events.mono 와 같은 기준 */
    int           ctx_valid;   /* 0 이면 frame/mono 열을 NULL 로(시작·종료 같은 프레임 밖 이벤트) */
} EventLog;

/* path: DB 파일 경로, 또는 ":memory:" (인메모리, 테스트용)
 * echo_stderr: 1이면 실시간 콘솔 출력 병행
 * 반환: 0=성공, -1=실패 */
int  event_log_open(EventLog *log, const char *path,
                    LogLevel min_level, int echo_stderr);
void event_log_close(EventLog *log);

/*
 * 이벤트 구조화 열 — 메시지 문자열은 그대로 두고, 같은 정보를 질의하기 쉬운 열로 함께 저장한다(정규식 파싱 제거).
 *   run   프로세스 시작 시각. 시간별 파일 여러 개를 합쳐도 어느 실행의 이벤트인지 알 수 있다.
 *   mono/frame  이벤트가 나온 프레임의 시각·번호(event_log_set_context). 프레임 밖 이벤트는 NULL.
 *   kind  메시지 첫 단어(알림 모드가 쓰는 토큰과 같음). "state=ok" 처럼 key=value 로 시작하면 module 이름.
 *   track 메시지의 track=N.
 *   data  메시지의 key=value 쌍을 JSON 객체로(숫자 뒤 단위 s/ms/px/% 는 떼고 숫자로). 400바이트 상한 — 넘으면 남는 쌍은 생략.
 * 용량: 이벤트당 수십 바이트(분석 대상 이벤트는 시간당 수십~수백 건이라 합계가 리플레이 좌표의 1% 미만).
 */
void event_log_set_run(EventLog *log, long long run_id);
void event_log_set_context(EventLog *log, long long frame, double mono);
void event_log_clear_context(EventLog *log);

/* 파일 하나당 실행 정보(버전·빌드·인수·설정)를 run_info 테이블에 한 행 저장한다. 12KB 를 넘으면 -1(저장 안 함). */
int  event_log_write_run_info(EventLog *log, const char *manifest_json);

/* 가장 최근 이벤트 id(없으면 0). 시간별 요약이 "그 뒤의 이벤트"만 세는 기준점. */
long long event_log_last_id(EventLog *log);

/* since_id 뒤의 알림성 이벤트(WARN 이상, 룰·문·슬롯·잔류물·표면 모듈)를 종류별로 센다. 건수 많은 순, 최대 max 개. 반환: 채운 개수. */
typedef struct { char kind[48]; int count; int tracks; } EventKindCount;
int  event_log_kind_counts(EventLog *log, long long since_id, EventKindCount *out, int max);

/*
 * 출력 형식 (stderr echo):
 *   2026-08-29T14:03:11 WARN  rules    overstay track=7 dwell=4821s
 */
void event_log_write(EventLog *log, LogLevel level,
                     const char *module, const char *message);

/*
 * 알림 모드: 이벤트 종류(메시지 첫 단어, 예 person_fallen)별로 기록 방식을 정한다.
 *   ALERT_ON     그대로 기록(기본).
 *   ALERT_SHADOW 기록하되 메시지 끝에 " mode=shadow"를 붙인다. 대시보드 알림 피드가 이 접미사를 걸러서 점주에게는
 *                보이지 않지만, 리플레이 기록과 분석은 그대로 쓸 수 있다.
 *   ALERT_OFF    기록하지 않는다(룰 계산은 그대로).
 * 그림자 모드가 필요한 이유: 현장 정답 데이터가 없어 정밀도를 모르는 알림을 곧바로 점주에게 보내면 오탐이 신뢰를
 * 깎는다. 기록만 하면서 현장 데이터를 쌓아 통과한 알림만 켠다(매장별 설정 분리 원칙).
 *
 * 표를 EventLog 가 아니라 프로세스 전역에 두는 이유: 시간별 로그 전환이 EventLog 구조체를 통째로 교체해서
 * (log_rotation.c) 구조체 안의 설정은 한 시간 뒤 사라진다. 메인 스레드에서만 설정·기록한다.
 */
typedef enum { ALERT_ON = 0, ALERT_SHADOW = 1, ALERT_OFF = 2 } AlertMode;
#define ALERT_SHADOW_SUFFIX " mode=shadow"
void event_log_set_alert_mode(const char *token, AlertMode mode);   /* ALERT_ON 이면 해제 */
void event_log_clear_alert_modes(void);
/* 기본이 아닌 모드를 "shadow=a,b off=c" 형태로 out 에 쓰고 해당 개수를 반환한다(없으면 "" 과 0). */
int  event_log_alert_summary(char *out, unsigned long out_size);

/* level 이상으로 기록된 행 수 반환. 테스트 검증용. */
int event_log_count(EventLog *log, LogLevel min_level);

#endif /* EVENT_LOG_H */
