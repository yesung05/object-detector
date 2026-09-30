#ifndef LOG_ROTATION_H
#define LOG_ROTATION_H
#include "replay.h"
#include "perf_log.h"
typedef struct {
    char base[700], metadata[1024];
    double next;
    unsigned part;
} LogRotation;
void log_rotation_init(LogRotation *rotation, const char *path, const char *metadata, double now);
/* 1: switched, 0: not due/disabled, -1: failed; previous logs remain usable. */
int log_rotation_tick(LogRotation *rotation, EventLog *events, ReplayLog *replay, PerfLog *perf, double now);
void log_rotation_prune(EventLog *events, PerfLog *perf, unsigned long long limit_bytes);
#endif
