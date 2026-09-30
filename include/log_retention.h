#ifndef LOG_RETENTION_H
#define LOG_RETENTION_H
typedef struct { unsigned removed, failed; unsigned long long bytes; int over_limit; } LogRetentionResult;
/* Only generated log files directly inside directory; never follows subdirectories. */
LogRetentionResult log_retention_clean(const char *directory, const char *active_event,
    const char *active_perf, unsigned long long limit_bytes, double unix_now);
#endif
