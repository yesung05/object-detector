#include "platform.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdio.h>

static double filetime_seconds(const FILETIME *time) {
    ULARGE_INTEGER value;
    value.LowPart = time->dwLowDateTime;
    value.HighPart = time->dwHighDateTime;
    return (double)value.QuadPart / 10000000.0;
}

double platform_monotonic_seconds(void) {
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}

double platform_process_cpu_seconds(void) {
    FILETIME creation;
    FILETIME exit;
    FILETIME kernel;
    FILETIME user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return 0.0;
    return filetime_seconds(&kernel) + filetime_seconds(&user);
}

unsigned int platform_cpu_count(void) {
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return count > 0 ? (unsigned int)count : 1U;
}

void platform_sleep_milliseconds(unsigned int milliseconds) {
    Sleep((DWORD)milliseconds);
}

/* Windows에서 CPU 온도를 사용자 공간에서 직접 읽는 공개 API가 없으므로 -1을 반환합니다.
 * WMI(MSAcpi_ThermalZoneTemperature)로 구현 가능하지만 COM 초기화가 필요합니다. */
int platform_cpu_temperature_celsius(void) {
    return -1;
}

#else

#include <stdio.h>
#include <time.h>
#include <unistd.h>

double platform_monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

double platform_process_cpu_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

unsigned int platform_cpu_count(void) {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (unsigned int)count : 1U;
}

void platform_sleep_milliseconds(unsigned int milliseconds) {
    struct timespec delay;
    delay.tv_sec = (time_t)(milliseconds / 1000U);
    delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    nanosleep(&delay, NULL);
}

#if defined(__linux__)
/* /sys/class/thermal/thermal_zone*/temp 에서 가장 높은 유효 온도를 반환합니다.
 * 커널이 millidegree 단위로 씁니다. 없거나 비정상이면 -1. */
int platform_cpu_temperature_celsius(void) {
    static const char * const paths[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone1/temp",
        "/sys/class/thermal/thermal_zone2/temp",
        "/sys/class/thermal/thermal_zone3/temp",
    };
    size_t i;
    int best = -1;
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE *f = fopen(paths[i], "r");
        int millideg = 0;
        if (!f) continue;
        if (fscanf(f, "%d", &millideg) == 1) {
            int c = millideg / 1000;
            if (c >= 0 && c < 150 && c > best) best = c;
        }
        fclose(f);
    }
    return best;
}
#else
int platform_cpu_temperature_celsius(void) {
    return -1;
}
#endif

#endif
