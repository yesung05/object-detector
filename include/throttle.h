#ifndef THROTTLE_H
#define THROTTLE_H

#include "platform.h"

/*
 * 적응형 감속 — 이 프로그램은 키오스크의 보조 프로그램이라, 결제를 처리하는 본 프로그램을
 * 절대 밀어내면 안 됩니다.
 *
 * 2층 구조의 2층입니다. 1층은 platform_set_priority() 로 내린 프로세스 우선순위이며,
 * 우리 로직이 어떻게 망가지든 커널이 지켜 줍니다. 이 모듈은 그 위에서 "남들이 바쁘면
 * 스스로 일을 줄인다"를 담당합니다.
 *
 * 판단 기준을 우리 CPU 점유가 아니라 "남들의 CPU 점유"로 잡은 이유: 우리 부하만 보면
 * 한가한 새벽에도 감지를 줄이게 됩니다. 정작 중요한 것은 키오스크가 바쁜 순간입니다.
 *
 * 단계가 바뀔 때마다 반드시 로그를 남깁니다 — 남기지 않으면 "왜 갑자기 감지가 느려졌지"에
 * 답할 방법이 없습니다.
 */
typedef enum {
    THROTTLE_NONE    = 0, /* 정상 */
    THROTTLE_LIGHT   = 1, /* 추론 주기 2배, Tier 2 주기 2배 */
    THROTTLE_HEAVY   = 2, /* 추론 주기 4배, Tier 2 정지, 스트림 FPS 절반 */
    THROTTLE_MINIMAL = 3  /* 추론 정지 — 카메라 상태 감시만 유지 */
} ThrottleLevel;

typedef struct {
    int    enabled;             /* throttle_enabled, 기본 1 */
    int    light_percent;       /* 기본 50 — 남들 부하가 이 이상이면 LIGHT */
    int    heavy_percent;       /* 기본 70 */
    int    minimal_percent;     /* 기본 85 */
    /* 복귀 임계값은 진입 임계값에서 이만큼 낮습니다. 같은 값을 쓰면 경계에서 단계가
     * 초당 여러 번 뒤집혀 로그가 폭주하고 추론 주기가 계속 흔들립니다. */
    int    hysteresis_percent;  /* 기본 10 */
    double min_level_seconds;   /* 기본 5 — 단계 변경 최소 간격 */
    double check_seconds;       /* 기본 2 — 측정 주기 */
} ThrottleConfig;

typedef struct {
    ThrottleConfig config;
    ThrottleLevel  level;
    double         level_since;
    double         last_check;
    CpuTimes       prev_cpu;
    double         prev_proc_cpu;
    double         prev_wall;
    int            have_baseline;
    double         others_percent; /* 마지막 측정값 — 로그·대시보드 표시용 */
    int            measure_failed; /* 1이면 CPU 측정 불가(미지원 플랫폼 등) */
} Throttle;

void throttle_init(Throttle *t, const ThrottleConfig *config);
void throttle_configure(Throttle *t, const ThrottleConfig *config);

/*
 * 한 스텝 진행합니다. 매 프레임 호출해도 안전합니다 — 내부에서 check_seconds 주기로만
 * 실제 측정을 합니다.
 *
 * proc_cpu_seconds: 호출자가 이미 재고 있는 platform_process_cpu_seconds() 값을
 *                   재사용합니다(같은 프레임에서 두 번 부르지 않도록).
 * 반환: 단계가 바뀌었으면 1, 아니면 0. 1일 때 호출자가 로그를 남기고 스트림 FPS 등을 반영합니다.
 */
int throttle_update(Throttle *t, double now, double proc_cpu_seconds, int cpu_count);

/*
 * 순수 판정 함수 — 상태 없이 현재 단계와 부하만으로 다음 단계를 정합니다.
 * 히스테리시스 동작을 영상 없이 시험할 수 있도록 분리했습니다.
 */
ThrottleLevel throttle_decide(const ThrottleConfig *config,
                              ThrottleLevel current, double others_percent);

/* 추론 주기 배수. 0이면 추론을 아예 건너뜁니다. */
int  throttle_detect_multiplier(ThrottleLevel level);
/* Tier 2 주기 배수. 0이면 Tier 2 정지. */
int  throttle_tier2_multiplier(ThrottleLevel level);
/* 기준 FPS 를 단계에 맞게 낮춥니다(최소 1). */
int  throttle_stream_fps(ThrottleLevel level, int base_fps);
const char *throttle_level_name(ThrottleLevel level);

#endif /* THROTTLE_H */
