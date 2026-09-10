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

/* 이름 있는 뮤텍스로 단일 인스턴스를 보장합니다.
 * CreateMutex는 이미 존재해도 핸들을 반환하므로 ERROR_ALREADY_EXISTS로 구분합니다. */
static HANDLE g_instance_mutex = NULL;

int platform_single_instance_try_lock(void) {
    /* "Global\\" 접두사는 관리자 권한이 필요하므로 세션 로컬 이름을 씁니다.
     * 같은 사용자 세션 내 중복 실행만 막으면 충분합니다. */
    g_instance_mutex = CreateMutexW(NULL, TRUE, L"hunik-detector-lock");
    if (!g_instance_mutex) return -1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_instance_mutex);
        g_instance_mutex = NULL;
        return 0;
    }
    return 1;
}

void platform_single_instance_unlock(void) {
    if (g_instance_mutex) {
        ReleaseMutex(g_instance_mutex);
        CloseHandle(g_instance_mutex);
        g_instance_mutex = NULL;
    }
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

/* lock file + flock(LOCK_EX|LOCK_NB) 로 단일 인스턴스를 보장합니다.
 * 프로세스가 죽으면 커널이 flock을 자동 해제하므로 stale lock 문제가 없습니다. */
#include <fcntl.h>
#include <sys/file.h>

static int g_lock_fd = -1;

int platform_single_instance_try_lock(void) {
    g_lock_fd = open("/tmp/hunik-detector.lock", O_CREAT | O_RDWR, 0666);
    if (g_lock_fd < 0) return -1;
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(g_lock_fd);
        g_lock_fd = -1;
        return 0;
    }
    return 1;
}

void platform_single_instance_unlock(void) {
    if (g_lock_fd >= 0) {
        flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
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
