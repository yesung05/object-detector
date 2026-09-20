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

/*
 * 시스템 전체 CPU 시간입니다(모든 코어 합, 단위는 플랫폼 내부 틱).
 * 두 샘플의 차이로만 사용률을 계산하므로 절대 단위는 의미가 없습니다.
 * Windows: GetSystemTimes, Linux: /proc/stat. 미지원 시 -1.
 *
 * 이 값이 필요한 이유: 우리가 CPU 를 얼마나 쓰는지가 아니라 "남들이 얼마나 쓰는지"를
 * 알아야 물러날 시점을 판단할 수 있습니다. 키오스크 본 프로그램이 바쁠 때만 감속해야지,
 * 우리 부하만 보고 줄이면 한가한 시간에도 감지가 나빠집니다.
 */
typedef struct { unsigned long long idle, total; } CpuTimes;
int platform_cpu_times(CpuTimes *out);

/*
 * 프로세스 우선순위를 낮춰 키오스크 본 프로그램이 항상 먼저 스케줄되게 합니다.
 * level: 0=보통, 1=보통 아래, 2=유휴. 반환 0=성공, -1=실패/미지원.
 *
 * 우선순위 클래스는 프로세스의 모든 스레드에 적용되므로 ORT 추론 스레드까지 함께
 * 내려갑니다. 적응형 감속 로직에 버그가 있어도 커널이 지켜 주는 최후의 방어선입니다.
 * 유휴(2)는 권장하지 않습니다 — 키오스크가 조금만 바빠도 완전히 굶어 프레임이 몇 초씩 끊깁니다.
 */
int platform_set_priority(int level);

/* 단일 인스턴스 잠금.
 * Windows: 이름 있는 뮤텍스 "Global\hunik-detector"
 * Linux/macOS: /tmp/hunik-detector.lock + flock(LOCK_NB)
 * 반환값: 1=잠금 획득, 0=이미 실행 중, -1=시스템 오류
 * 프로세스 종료 시 OS가 자동 해제하므로 unlock은 선택적입니다. */
int platform_single_instance_try_lock(void);
void platform_single_instance_unlock(void);

#endif
