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

#endif
