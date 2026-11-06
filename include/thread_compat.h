#ifndef THREAD_COMPAT_H
#define THREAD_COMPAT_H

/*
 * Windows(CRITICAL_SECTION·CONDITION_VARIABLE·CreateThread) / POSIX(pthread) 공용 최소 래퍼.
 *
 * 원래 media_ffmpeg.c 캡처 스레드 안에만 있던 매크로를 추론 워커(infer_worker.h)도 쓰게 되어 옮겼습니다.
 * C11 <threads.h> 를 쓰지 않은 이유: MSVC 는 VS 2022 17.8 이후에야 들어왔고 배포 빌드 환경을 그 버전에
 * 묶고 싶지 않습니다. 기능은 캡처·워커 두 곳이 실제로 쓰는 것(잠금·신호·시간 제한 대기·시작·join)만 둡니다.
 */

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
   typedef CRITICAL_SECTION   lc_mutex_t;
   typedef CONDITION_VARIABLE lc_cond_t;
#  define lc_mutex_init(m)     InitializeCriticalSection(m)
#  define lc_mutex_lock(m)     EnterCriticalSection(m)
#  define lc_mutex_unlock(m)   LeaveCriticalSection(m)
#  define lc_mutex_destroy(m)  DeleteCriticalSection(m)
#  define lc_cond_init(c)      InitializeConditionVariable(c)
#  define lc_cond_signal(c)    WakeConditionVariable(c)
#  define lc_cond_destroy(c)   ((void)0)
static inline int lc_cond_timedwait(lc_cond_t *c, lc_mutex_t *m, int ms) {
    return SleepConditionVariableCS(c, m, (DWORD)ms) ? 0 : -1;
}
/* 스레드 함수 시그니처가 플랫폼마다 달라 정의 쪽에서 LC_THREAD_FN(name, arg) 로 씁니다. */
typedef HANDLE lc_thread_t;
#  define LC_THREAD_FN(name, arg) DWORD WINAPI name(LPVOID arg)
#  define LC_THREAD_RETURN return 0
static inline int lc_thread_start(lc_thread_t *t, LPTHREAD_START_ROUTINE fn, void *arg) {
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *t ? 0 : -1;
}
static inline void lc_thread_join(lc_thread_t t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
#  include <pthread.h>
#  include <time.h>
   typedef pthread_mutex_t lc_mutex_t;
   typedef pthread_cond_t  lc_cond_t;
#  define lc_mutex_init(m)     pthread_mutex_init(m, NULL)
#  define lc_mutex_lock(m)     pthread_mutex_lock(m)
#  define lc_mutex_unlock(m)   pthread_mutex_unlock(m)
#  define lc_mutex_destroy(m)  pthread_mutex_destroy(m)
#  define lc_cond_init(c)      pthread_cond_init(c, NULL)
#  define lc_cond_signal(c)    pthread_cond_signal(c)
#  define lc_cond_destroy(c)   pthread_cond_destroy(c)
static inline int lc_cond_timedwait(lc_cond_t *c, lc_mutex_t *m, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &ts) == 0 ? 0 : -1;
}
typedef pthread_t lc_thread_t;
#  define LC_THREAD_FN(name, arg) void *name(void *arg)
#  define LC_THREAD_RETURN return NULL
static inline int lc_thread_start(lc_thread_t *t, void *(*fn)(void *), void *arg) {
    return pthread_create(t, NULL, fn, arg) == 0 ? 0 : -1;
}
static inline void lc_thread_join(lc_thread_t t) { pthread_join(t, NULL); }
#endif

#endif
