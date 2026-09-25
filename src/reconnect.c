#include "reconnect.h"

double reconnect_backoff_seconds(int attempt, double base, double max_s) {
    double wait = base;
    int i;
    if (base <= 0.0) return 0.0;
    if (max_s < base) max_s = base;
    /* pow() 대신 반복 곱셈: attempt 가 커져도 상한에서 멈춰 오버플로가 없습니다. */
    for (i = 0; i < attempt && wait < max_s; ++i) wait *= 2.0;
    return wait > max_s ? max_s : wait;
}

int reconnect_stalled(double now, double last_packet, int got_first,
                      double stall_s, double first_grace_s) {
    double limit = stall_s;
    if (!got_first && first_grace_s > limit) limit = first_grace_s;
    return (now - last_packet) >= limit;
}
