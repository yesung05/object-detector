#include "platform.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
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

/* WorkingSetSize: 물리 메모리에 실제로 올라와 있는 페이지 크기입니다.
 * PROCESS_MEMORY_COUNTERS는 psapi.h에 있으며 ws2_32 없이 psapi/kernel32로 링크됩니다.
 * Windows 7 이상에서 GetProcessMemoryInfo가 kernel32에 포함되어 별도 라이브러리가 불필요합니다. */
long platform_process_memory_kb(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return -1L;
    return (long)(pmc.WorkingSetSize / 1024UL);
}

static unsigned long long ft_to_u64(FILETIME ft) {
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

int platform_cpu_times(CpuTimes *out) {
    FILETIME idle, kernel, user;
    if (!out) return -1;
    if (!GetSystemTimes(&idle, &kernel, &user)) return -1;
    /* Windows 의 kernel 시간에는 idle 이 포함되어 있습니다. total 을 kernel+user 로 두고
       idle 을 따로 빼야 실제 사용률이 나옵니다. */
    out->idle  = ft_to_u64(idle);
    out->total = ft_to_u64(kernel) + ft_to_u64(user);
    return 0;
}

int platform_set_priority(int level) {
    DWORD cls = (level >= 2) ? IDLE_PRIORITY_CLASS
              : (level == 1) ? BELOW_NORMAL_PRIORITY_CLASS
                             : NORMAL_PRIORITY_CLASS;
    return SetPriorityClass(GetCurrentProcess(), cls) ? 0 : -1;
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

/* ── 시스템 정보 ─────────────────────────────────────────────────────────────── */

/* 제어문자를 공백으로 바꾸고 연속 공백을 하나로 줄여 앞뒤를 자른다(레지스트리 값 끝에 공백이 붙는 CPU 이름 정리). */
static void clean_text(char *s) {
    size_t r, w = 0;
    int space = 1;
    for (r = 0; s[r]; ++r) {
        unsigned char c = (unsigned char)s[r];
        if (c < 0x20 || c == ' ') {
            if (!space) { s[w++] = ' '; space = 1; }
        } else {
            s[w++] = (char)c;
            space = 0;
        }
    }
    if (w > 0 && s[w - 1] == ' ') --w;
    s[w] = '\0';
}

/* 레지스트리 문자열. RegGetValueA 를 쓰는 이유: 값 끝 널 문자 보장과 형식 확인을 대신해 준다(Vista 이상). */
static int reg_string(const char *subkey, const char *name, char *out, size_t out_size) {
    DWORD size = (DWORD)out_size;
    out[0] = '\0';
    if (RegGetValueA(HKEY_LOCAL_MACHINE, subkey, name, RRF_RT_REG_SZ, NULL, out, &size) != ERROR_SUCCESS) {
        out[0] = '\0';
        return -1;
    }
    out[out_size - 1] = '\0';
    clean_text(out);
    return 0;
}

static unsigned int reg_dword(const char *subkey, const char *name) {
    DWORD value = 0, size = (DWORD)sizeof(value);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, subkey, name, RRF_RT_REG_DWORD, NULL, &value, &size) != ERROR_SUCCESS) return 0;
    return (unsigned int)value;
}

int platform_system_info(PlatformSystemInfo *out, const char *disk_path) {
    typedef LONG (WINAPI *RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    static const char *const CPU_KEY = "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    static const char *const BIOS_KEY = "HARDWARE\\DESCRIPTION\\System\\BIOS";
    static const char *const NT_KEY = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    RTL_OSVERSIONINFOW ver;
    SYSTEM_INFO si;
    MEMORYSTATUSEX mem;
    SYSTEM_POWER_STATUS power;
    char display[24], maker[64], product[64];
    HMODULE ntdll;
    RtlGetVersionFn rtl_get_version;
    DWORD length = 0;
    int feature_pos = 0;

    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->power_ac = -1;
    out->battery_percent = -1;
    out->disk_free_mb = out->disk_total_mb = -1;

    /* OS: GetVersionEx 는 매니페스트 없이 10.0 보다 낮은 값을 돌려줄 수 있어 RtlGetVersion 을 쓴다. */
    memset(&ver, 0, sizeof(ver));
    ver.dwOSVersionInfoSize = sizeof(ver);
    ntdll = GetModuleHandleW(L"ntdll.dll");
    rtl_get_version = ntdll ? (RtlGetVersionFn)(void *)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    if (rtl_get_version && rtl_get_version(&ver) == 0) {
        const char *name = ver.dwMajorVersion == 10 && ver.dwBuildNumber >= 22000 ? "Windows 11"
                         : ver.dwMajorVersion == 10 ? "Windows 10" : NULL;
        reg_string(NT_KEY, "DisplayVersion", display, sizeof(display));
        if (name)
            snprintf(out->os, sizeof(out->os), "%s%s%s (build %lu)", name, display[0] ? " " : "", display, (unsigned long)ver.dwBuildNumber);
        else
            snprintf(out->os, sizeof(out->os), "Windows %lu.%lu (build %lu)", (unsigned long)ver.dwMajorVersion,
                     (unsigned long)ver.dwMinorVersion, (unsigned long)ver.dwBuildNumber);
    }

    GetNativeSystemInfo(&si);
    snprintf(out->arch, sizeof(out->arch), "%s",
             si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64" :
             si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" :
             si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? "x86" : "other");

    reg_string(CPU_KEY, "ProcessorNameString", out->cpu, sizeof(out->cpu));
    out->cpu_mhz = reg_dword(CPU_KEY, "~MHz");
    reg_string(BIOS_KEY, "SystemManufacturer", maker, sizeof(maker));
    reg_string(BIOS_KEY, "SystemProductName", product, sizeof(product));
    snprintf(out->model, sizeof(out->model), "%s%s%s", maker, maker[0] && product[0] ? " " : "", product);

    out->logical_cpus = platform_cpu_count();
    /* 물리 코어: RelationProcessorCore 항목 수. 두 번 호출(필요 크기 → 채우기)하는 것이 이 API 의 규약이다. */
    GetLogicalProcessorInformation(NULL, &length);
    if (length >= sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION)) {
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)malloc(length);   /* 이 함수 소유 — 아래에서 free */
        if (info && GetLogicalProcessorInformation(info, &length)) {
            DWORD i, count = length / (DWORD)sizeof(*info);
            for (i = 0; i < count; ++i)
                if (info[i].Relationship == RelationProcessorCore) out->physical_cores++;
        }
        free(info);
    }

    /* 명령어 집합: 숫자는 winnt.h 의 PF_* 값(오래된 SDK 에는 이름이 없어 숫자로 쓴다). */
    if (IsProcessorFeaturePresent(40)) feature_pos += snprintf(out->features + feature_pos, sizeof(out->features) - (size_t)feature_pos, "%savx2", feature_pos ? "," : "");
    if (IsProcessorFeaturePresent(39)) feature_pos += snprintf(out->features + feature_pos, sizeof(out->features) - (size_t)feature_pos, "%savx", feature_pos ? "," : "");
    if (IsProcessorFeaturePresent(38)) feature_pos += snprintf(out->features + feature_pos, sizeof(out->features) - (size_t)feature_pos, "%ssse4.2", feature_pos ? "," : "");
    (void)feature_pos;

    memset(&mem, 0, sizeof(mem));
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        out->ram_total_mb = mem.ullTotalPhys / (1024ULL * 1024ULL);
        out->ram_avail_mb = mem.ullAvailPhys / (1024ULL * 1024ULL);
    }

    if (GetSystemPowerStatus(&power)) {
        out->power_ac = power.ACLineStatus == 1 ? 1 : power.ACLineStatus == 0 ? 0 : -1;
        out->battery_percent = power.BatteryLifePercent <= 100 ? (int)power.BatteryLifePercent : -1;
    }

    /* 로그 폴더가 있는 드라이브의 남은 공간. 경로는 UTF-8(한글 사용자 이름 가능)이라 와이드 문자로 바꿔 부른다. */
    {
        wchar_t wide[MAX_PATH];
        ULARGE_INTEGER avail, total, total_free;
        const char *path = disk_path && disk_path[0] ? disk_path : ".";
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH) > 0 &&
            GetDiskFreeSpaceExW(wide, &avail, &total, &total_free)) {
            out->disk_free_mb = (long long)(avail.QuadPart / (1024ULL * 1024ULL));
            out->disk_total_mb = (long long)(total.QuadPart / (1024ULL * 1024ULL));
        }
    }
    return 0;
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

/* /proc/self/status의 VmRSS 줄을 파싱합니다.
 * VmRSS는 물리 메모리에 올라온 페이지 크기이며 kB 단위로 기록됩니다. */
long platform_process_memory_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1L;
    char line[128];
    long kb = -1L;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
    }
    fclose(f);
    return kb;
}

int platform_cpu_times(CpuTimes *out) {
#if defined(__linux__)
    /* /proc/stat 첫 줄: cpu user nice system idle iowait irq softirq steal ...
     * idle 은 idle+iowait 로 봅니다 — I/O 대기 중에는 CPU 가 남에게 갈 수 있습니다. */
    FILE *f;
    unsigned long long v[8] = {0};
    int n, i;
    if (!out) return -1;
    f = fopen("/proc/stat", "r");
    if (!f) return -1;
    n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4) return -1;
    out->idle  = v[3] + v[4];
    out->total = 0;
    for (i = 0; i < (n < 8 ? n : 8); ++i) out->total += v[i];
    return 0;
#else
    (void)out;
    return -1;   /* macOS 미지원 — 호출자는 감속 판단을 건너뜁니다 */
#endif
}

int platform_set_priority(int level) {
    /* POSIX nice: 0=보통, 5=보통 아래, 15=유휴에 준함. 실패해도 치명적이지 않습니다. */
    int want = (level >= 2) ? 15 : (level == 1) ? 5 : 0;
    return nice(want) == -1 ? -1 : 0;
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

/* ── 시스템 정보(POSIX) ────────────────────────────────────────────────────────
 * Linux: /proc, /etc/os-release, sysconf. 다른 POSIX(macOS 등)는 uname 과 sysconf 로 얻는 값만 채우고 나머지는 비워 둔다
 * (이 프로그램의 배포 대상은 Windows 라 여기는 "컴파일되고 안전하게 비어 있음"이 목표다). */
#include <sys/statvfs.h>
#include <sys/utsname.h>

static void clean_text(char *s) {
    size_t r, w = 0;
    int space = 1;
    for (r = 0; s[r]; ++r) {
        unsigned char c = (unsigned char)s[r];
        if (c < 0x20 || c == ' ') {
            if (!space) { s[w++] = ' '; space = 1; }
        } else {
            s[w++] = (char)c;
            space = 0;
        }
    }
    if (w > 0 && s[w - 1] == ' ') --w;
    s[w] = '\0';
}

int platform_system_info(PlatformSystemInfo *out, const char *disk_path) {
    struct utsname un;
    struct statvfs vfs;
    long pages, avail_pages, page_size;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->power_ac = -1;
    out->battery_percent = -1;
    out->disk_free_mb = out->disk_total_mb = -1;

    if (uname(&un) == 0) {
        snprintf(out->os, sizeof(out->os), "%s %s", un.sysname, un.release);
        snprintf(out->arch, sizeof(out->arch), "%s", un.machine);
    }
#if defined(__linux__)
    {
        char line[256];
        FILE *f = fopen("/etc/os-release", "r");
        if (f) {
            while (fgets(line, sizeof(line), f)) {
                char *eq;
                if (strncmp(line, "PRETTY_NAME=", 12) != 0) continue;
                eq = line + 12;
                if (*eq == '"') ++eq;
                eq[strcspn(eq, "\"\r\n")] = '\0';
                snprintf(out->os, sizeof(out->os), "%s (kernel %s)", eq, un.release);
                break;
            }
            fclose(f);
        }
        f = fopen("/proc/cpuinfo", "r");
        if (f) {
            unsigned int cores_per_socket = 0;
            while (fgets(line, sizeof(line), f)) {
                char *colon = strchr(line, ':');
                if (!colon) continue;
                if (!out->cpu[0] && strncmp(line, "model name", 10) == 0) snprintf(out->cpu, sizeof(out->cpu), "%s", colon + 1);
                else if (!out->cpu_mhz && strncmp(line, "cpu MHz", 7) == 0) out->cpu_mhz = (unsigned int)atof(colon + 1);
                else if (!cores_per_socket && strncmp(line, "cpu cores", 9) == 0) cores_per_socket = (unsigned int)atoi(colon + 1);
                if (strstr(line, "flags") == line) {
                    int pos = 0;
                    if (strstr(colon, " avx2")) pos += snprintf(out->features + pos, sizeof(out->features) - (size_t)pos, "%savx2", pos ? "," : "");
                    if (strstr(colon, " avx ")) pos += snprintf(out->features + pos, sizeof(out->features) - (size_t)pos, "%savx", pos ? "," : "");
                    if (strstr(colon, " sse4_2")) pos += snprintf(out->features + pos, sizeof(out->features) - (size_t)pos, "%ssse4.2", pos ? "," : "");
                    (void)pos;
                }
            }
            fclose(f);
            out->physical_cores = cores_per_socket;   /* 소켓이 여러 개면 모자라는 값이지만, 매장용 PC 는 소켓 1개다 */
        }
    }
#endif
    out->logical_cpus = platform_cpu_count();
    page_size = sysconf(_SC_PAGESIZE);
    pages = sysconf(_SC_PHYS_PAGES);
    avail_pages = sysconf(_SC_AVPHYS_PAGES);
    if (page_size > 0 && pages > 0) out->ram_total_mb = (unsigned long long)pages * (unsigned long long)page_size / (1024ULL * 1024ULL);
    if (page_size > 0 && avail_pages > 0) out->ram_avail_mb = (unsigned long long)avail_pages * (unsigned long long)page_size / (1024ULL * 1024ULL);
    if (statvfs(disk_path && disk_path[0] ? disk_path : ".", &vfs) == 0) {
        out->disk_free_mb = (long long)((unsigned long long)vfs.f_bavail * vfs.f_frsize / (1024ULL * 1024ULL));
        out->disk_total_mb = (long long)((unsigned long long)vfs.f_blocks * vfs.f_frsize / (1024ULL * 1024ULL));
    }
    clean_text(out->cpu);
    return 0;
}

#endif

/* 플랫폼 분기 밖의 순수 계산이라 test_core 에서 그대로 검증한다. */
#define PLATFORM_VIDEO_CLOCK_EPOCH 1000.0

double platform_video_seconds(int64_t frame_index, double fps) {
    if (!(fps > 0.0) || frame_index < 0) return -1.0;
    return PLATFORM_VIDEO_CLOCK_EPOCH + (double)frame_index / fps;
}
