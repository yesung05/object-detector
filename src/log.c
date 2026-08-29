#include "log.h"

#include <time.h>

void event_log_init(EventLog *log, FILE *file, LogLevel min_level) {
    if (!log) return;
    log->file      = file;
    log->min_level = min_level;
}

static const char *level_name(LogLevel level) {
    switch (level) {
        case LOG_INFO:  return "INFO ";
        case LOG_WARN:  return "WARN ";
        case LOG_ERROR: return "ERROR";
        default:        return "?????";
    }
}

void event_log_write(EventLog *log, LogLevel level,
                     const char *module, const char *message) {
    FILE *out;
    time_t now;
    struct tm tm_buf;
    char ts[24];

    if (!log || level < log->min_level) return;
    out = log->file ? log->file : stderr;

    time(&now);
#if defined(_WIN32)
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    /* ISO 8601 초 단위: "2026-08-29T14:03:11" */
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    fprintf(out, "%s %s %-8s %s\n", ts, level_name(level),
            module ? module : "", message ? message : "");
    fflush(out);
}
