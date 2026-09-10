#ifndef PLATFORM_H
#define PLATFORM_H

/*
 * OS별 시간/프로세서 API 차이를 감추는 작은 이식 계층입니다.
 * 모든 시간 함수는 초 단위 double을 반환합니다.
 */
double platform_monotonic_seconds(void);
double platform_process_cpu_seconds(void);
unsigned int platform_cpu_count(void);
void platform_sleep_milliseconds(unsigned int milliseconds);

/* CPU 온도를 섭씨로 반환합니다. 측정 불가 시 -1을 반환합니다.
 * Linux: /sys/class/thermal/thermal_zoneN/temp (N=0..3) 순서로 시도합니다.
 * Windows/macOS: 미지원, -1 반환. */
int platform_cpu_temperature_celsius(void);

/* 현재 프로세스 RSS(Resident Set Size)를 KB 단위로 반환합니다.
 * Windows: GetProcessMemoryInfo WorkingSetSize
 * Linux:   /proc/self/status VmRSS
 * 측정 불가 시 -1 반환. */
long platform_process_memory_kb(void);

/* 단일 인스턴스 잠금.
 * Windows: 이름 있는 뮤텍스 "Global\hunik-detector"
 * Linux/macOS: /tmp/hunik-detector.lock + flock(LOCK_NB)
 * 반환값: 1=잠금 획득, 0=이미 실행 중, -1=시스템 오류
 * 프로세스 종료 시 OS가 자동 해제하므로 unlock은 선택적입니다. */
int platform_single_instance_try_lock(void);
void platform_single_instance_unlock(void);

#endif
