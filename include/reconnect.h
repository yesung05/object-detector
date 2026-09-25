#ifndef RECONNECT_H
#define RECONNECT_H

/*
 * 카메라 재연결 정책의 순수 계산부입니다. FFmpeg 에 의존하지 않아
 * test_core 에서 경계값을 검증할 수 있습니다.
 */

/*
 * attempt 번째(0부터) 재시도 전 대기 시간. base, 2*base, 4*base ... max_s 에서 멈춥니다.
 *
 * 지수 백오프를 쓰는 이유: 케이블이 잠깐 흔들린 경우는 1초 안에 붙어야 하고,
 * 카메라가 아예 빠진 경우에는 장치 열기(dshow 그래프 구성)를 초마다 반복해
 * 저전력 CPU 를 괴롭히지 않아야 합니다. 상한이 있어 복구 지연은 최대 max_s 입니다.
 */
double reconnect_backoff_seconds(int attempt, double base, double max_s);

/*
 * 프레임이 멈췄는지 판정합니다. 반환 1 = stall.
 *
 * got_first == 0 이면 (열기 직후 첫 패킷 전) max(stall_s, first_grace_s) 를 씁니다.
 * dshow 는 그래프 시작 후 첫 패킷까지 1~3초 걸릴 수 있어, 같은 기준을 쓰면
 * 멀쩡한 카메라를 열자마자 끊김으로 오판합니다.
 *
 * last_packet 은 "마지막 패킷을 받은 시각"입니다. 추론이 느려 다음 읽기가
 * 늦어져도 그 사이 쌓인 패킷이 바로 읽히므로 오탐이 나지 않습니다.
 */
int reconnect_stalled(double now, double last_packet, int got_first,
                      double stall_s, double first_grace_s);

#endif /* RECONNECT_H */
