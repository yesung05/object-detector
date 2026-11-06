#ifndef INFER_WORKER_H
#define INFER_WORKER_H

/*
 * 작업 하나짜리 백그라운드 워커 — Tier1 추론(detector_run 100~500ms)을 프레임 루프에서 떼어 내는 데 씁니다.
 *
 * 큐가 아니라 슬롯 하나인 이유: 2코어 기기에서 추론이 밀리면 오래된 프레임을 뒤늦게 처리할 가치가 없습니다.
 * 바쁠 때 들어온 요청은 호출자가 "다음에 다시"로 처리하고(main.c infer_pending), 항상 가장 최근 프레임만 봅니다.
 *
 * 소유권은 state 값으로 넘깁니다(별도 락 없이 작업 데이터를 공유하는 근거):
 *   0 idle    — 호출자가 작업 데이터(ctx 가 가리키는 것)를 읽고 쓸 수 있음. submit 은 이 상태에서만.
 *   1 running — 워커만 작업 데이터를 만짐. 호출자는 손대지 않음.
 *   2 done    — 워커가 끝냄. poll 이 결과를 넘기며 0 으로 되돌림.
 * state 전환은 lock 안에서만 일어나므로 그 전후의 메모리 쓰기가 서로에게 보입니다.
 *
 * 헤더 전용인 이유: 사용처가 main.c 하나와 test_core 뿐이라 빌드 목록(CMake·Makefile)을 건드리지 않습니다
 * (door_mapper.h 와 같은 방식).
 */

#include "thread_compat.h"

#include <stdio.h>
#include <string.h>

/* 작업 함수. 반환 0 = 성공, -1 = 실패(error 에 이유). 워커 스레드에서 호출됩니다. */
typedef int (*InferJobFn)(void *ctx, char *error, size_t error_size);

typedef struct {
    lc_mutex_t  lock;
    lc_cond_t   cond;
    lc_thread_t thread;
    InferJobFn  fn;
    void       *ctx;        /* 호출자 소유 — 워커는 running 동안만 빌려 씀. 해제는 호출자가 stop 이후에 */
    int         state;      /* 0 idle, 1 running, 2 done (위 설명) */
    int         rc;
    char        error[256];
    int         stop;
    int         started;
} InferWorker;

static LC_THREAD_FN(infer_worker_main, arg) {
    InferWorker *w = (InferWorker *)arg;
    lc_mutex_lock(&w->lock);
    for (;;) {
        /* 시간 제한 대기: 신호를 놓쳐도 100ms 안에 상태를 다시 봅니다. */
        while (!w->stop && w->state != 1) lc_cond_timedwait(&w->cond, &w->lock, 100);
        if (w->stop) break;
        lc_mutex_unlock(&w->lock);
        {
            char err[256] = {0};
            int rc = w->fn(w->ctx, err, sizeof(err));
            lc_mutex_lock(&w->lock);
            w->rc = rc;
            memcpy(w->error, err, sizeof(w->error));
            w->state = 2;
        }
    }
    lc_mutex_unlock(&w->lock);
    LC_THREAD_RETURN;
}

/* w 는 0 으로 초기화된 상태여야 합니다. 반환 0 = 시작, -1 = 실패(error). */
static inline int infer_worker_start(InferWorker *w, InferJobFn fn, void *ctx,
                                     char *error, size_t error_size) {
    w->fn = fn; w->ctx = ctx; w->state = 0; w->stop = 0;
    lc_mutex_init(&w->lock);
    lc_cond_init(&w->cond);
    if (lc_thread_start(&w->thread, infer_worker_main, w) != 0) {
        lc_cond_destroy(&w->cond);
        lc_mutex_destroy(&w->lock);
        if (error) snprintf(error, error_size, "infer_worker: thread start failed");
        return -1;
    }
    w->started = 1;
    return 0;
}

/* 1 = 작업 데이터가 워커 소유(실행 중이거나 결과 대기). 0 = idle. */
static inline int infer_worker_busy(InferWorker *w) {
    int busy;
    lc_mutex_lock(&w->lock);
    busy = w->state != 0;
    lc_mutex_unlock(&w->lock);
    return busy;
}

/* idle 일 때만 호출합니다. 호출 뒤에는 결과를 poll 하기 전까지 작업 데이터를 만지지 않습니다. */
static inline void infer_worker_submit(InferWorker *w) {
    lc_mutex_lock(&w->lock);
    if (w->state == 0) {
        w->state = 1;
        lc_cond_signal(&w->cond);
    }
    lc_mutex_unlock(&w->lock);
}

/* 1 = 완료(작업 데이터가 호출자에게 돌아옴), 0 = 아직, -1 = 작업 실패(error 복사, 데이터도 돌아옴). */
static inline int infer_worker_poll(InferWorker *w, char *error, size_t error_size) {
    int result = 0;
    lc_mutex_lock(&w->lock);
    if (w->state == 2) {
        w->state = 0;
        result = w->rc == 0 ? 1 : -1;
        if (result < 0 && error) snprintf(error, error_size, "%s", w->error);
    }
    lc_mutex_unlock(&w->lock);
    return result;
}

/* 실행 중인 작업이 있으면 끝날 때까지(추론 한 번) 기다린 뒤 join 합니다. 제출됐지만 워커가 아직 집어 가지 않은
 * 작업은 실행하지 않고 버립니다(종료 중에는 결과를 쓸 곳이 없으므로 기다릴 이유가 없음). 시작 안 한 워커·NULL 도 안전. */
static inline void infer_worker_stop(InferWorker *w) {
    if (!w || !w->started) return;
    lc_mutex_lock(&w->lock);
    w->stop = 1;
    lc_cond_signal(&w->cond);
    lc_mutex_unlock(&w->lock);
    lc_thread_join(w->thread);
    lc_cond_destroy(&w->cond);
    lc_mutex_destroy(&w->lock);
    w->started = 0;
}

#endif
