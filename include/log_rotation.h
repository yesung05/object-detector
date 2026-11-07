#ifndef LOG_ROTATION_H
#define LOG_ROTATION_H
#include "replay.h"
#include "perf_log.h"
/*
 * 전환 훅. before: 새 파일을 무사히 열고 난 뒤 옛 파일을 닫기 직전 — 옛 파일에 마지막 기록(시간별 요약)을 남기는 자리.
 * after: 새 파일로 바뀐 직후 — 새 파일의 첫 기록(실행 정보)을 남기는 자리. 둘 다 events 는 그 순간 "열려 있는" 로그를 가리킨다.
 * 시간 맞춤을 로테이션 안에서 하는 이유: 별도 타이머를 두면 전환 시각과 어긋나 요약이 엉뚱한 파일에 들어가거나, 전환이 실패해
 * 60초마다 재시도될 때 요약이 반복된다.
 */
typedef void (*LogRotationHook)(void *context, EventLog *events);
typedef struct {
    char base[700], metadata[1024];
    double next;
    unsigned part;
    LogRotationHook before_switch, after_switch;
    void *hook_context;   /* 호출자 소유 — 로테이션은 보관만 한다 */
} LogRotation;
void log_rotation_set_hooks(LogRotation *rotation, LogRotationHook before_switch, LogRotationHook after_switch, void *context);
void log_rotation_init(LogRotation *rotation, const char *path, const char *metadata, double now);
/* 1: switched, 0: not due/disabled, -1: failed; previous logs remain usable. */
int log_rotation_tick(LogRotation *rotation, EventLog *events, ReplayLog *replay, PerfLog *perf, double now);
void log_rotation_prune(EventLog *events, PerfLog *perf, unsigned long long limit_bytes);
#endif
