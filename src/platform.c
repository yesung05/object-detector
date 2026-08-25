#include "platform.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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

#else

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

#endif
