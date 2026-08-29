#ifndef DOOR_H
#define DOOR_H

#include <stdint.h>

/*
 * 문 여닫이 감지 모듈입니다.
 *
 * 대시보드에서 "닫힌 상태"와 "열린 상태" 기준 이미지를 각각 캡처하면
 * stream.c가 door_closed_reference.raw / door_open_reference.raw로 저장합니다.
 * 이 모듈은 현재 프레임을 두 기준과 비교해 더 가까운 상태로 판정합니다.
 *
 * 두 기준 이미지를 모두 사용하는 이유:
 * - 단일 기준(닫힘만)은 "현재 vs 닫힘 차이"가 임계값을 넘으면 열림으로 판정하므로
 *   조명 변화나 카메라 노이즈로 인한 위양성이 많음
 * - 두 기준 모두 있으면 "닫힘과의 거리 vs 열림과의 거리"를 비교해
 *   더 가까운 쪽으로 판정 → 조명 변화에 강인함
 *
 * 닫힌 기준만 있으면 기존 방식(변화 비율 vs diff_threshold)으로 동작합니다.
 *
 * 추론 없이 픽셀 차이만 사용하는 이유:
 * - 문 개폐는 고정 배경 대비 뚜렷한 픽셀 변화로 감지 가능
 * - YOLO 추론 없이 1ms 미만으로 처리 가능 (i5-4200U 부하 최소화)
 * - 기준 이미지 없으면 조용히 비활성
 */

typedef struct {
    /* 닫힌 상태 기준 이미지 (door_closed_reference.raw) — door_destroy가 해제 */
    uint8_t *ref_closed_rgb;
    int      ref_closed_w;
    int      ref_closed_h;

    /* 열린 상태 기준 이미지 (door_open_reference.raw) — door_destroy가 해제 */
    uint8_t *ref_open_rgb;
    int      ref_open_w;
    int      ref_open_h;

    /* config.json에서 읽어오는 설정 */
    int   enabled;
    int   roi_x, roi_y, roi_w, roi_h; /* 0이면 전체 프레임 사용 */

    /* 단일 기준(닫힘만 있을 때) 판정 임계값.
     * 두 기준 모두 있으면 거리 비교로 판정하므로 이 값은 무시됩니다. */
    float diff_threshold; /* 0.0-1.0, 기본 0.05 */

    /* 상태 래치 — 매 프레임 이벤트 폭주 방지 */
    int last_state; /* -1=초기화 전, 0=닫힘, 1=열림 */

    /* 지속 시간 기반 이벤트 — 열린 상태가 이 시간 이상 유지될 때만 로그 발생.
     * 문이 잠깐 열렸다 닫히는 정상 상황(고객 입퇴장)을 필터링하기 위한 값. */
    double open_threshold_seconds; /* config: door_open_seconds, 기본 30.0 */
    double open_since;             /* 열림 시작 시각 (monotonic). -1=닫힌 상태 */
    int    open_event_fired;       /* 1=이번 개방 주기에 이미 이벤트를 발생시킴 */
} DoorMonitor;

/* 닫힘/열림 기준 파일을 각각 로드합니다.
 * 파일이 없으면 해당 기준만 NULL로 두고 0을 반환합니다(에러 아님).
 * 이미 로드된 버퍼가 있으면 먼저 해제합니다. */
int  door_load(DoorMonitor *d,
               const char *closed_path,
               const char *open_path);

void door_destroy(DoorMonitor *d);

/* 현재 프레임과 기준 이미지 비교.
 * 반환값: 0=닫힘, 1=열림, -1=기준 없음(enabled=0 또는 파일 미로드).
 * state_changed: 이전 상태와 달라졌으면 1, 같으면 0. */
int  door_check(DoorMonitor *d,
                const uint8_t *rgb, int w, int h, int stride,
                int *state_changed);

#endif /* DOOR_H */
