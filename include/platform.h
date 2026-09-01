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

#endif
