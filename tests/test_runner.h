#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

/*
 * 외부 의존 없는 매크로 기반 테스트 프레임워크입니다.
 * 표준 헤더 <setjmp.h>와 <stdio.h>만 사용합니다.
 *
 * ASSERT_* : 실패 시 longjmp로 현재 테스트를 즉시 중단합니다.
 * EXPECT_* : 실패를 기록하되 테스트는 계속 실행합니다.
 *
 * static 전역 변수를 헤더에 두는 이유: 이 헤더를 포함하는 translation unit이
 * 테스트 바이너리 하나뿐이므로 복사본이 하나만 생깁니다.
 */

#include <setjmp.h>
#include <stdio.h>

static int _tr_pass = 0;
static int _tr_fail = 0;
static int _tr_current_fail = 0;
static jmp_buf _tr_jmp;

#define TEST_SUITE_BEGIN(name) \
    do { printf("=== " #name " ===\n"); _tr_pass = 0; _tr_fail = 0; } while(0)

#define TEST_SUITE_END() \
    do { \
        printf("--- %d passed, %d failed ---\n", _tr_pass, _tr_fail); \
        return _tr_fail > 0 ? 1 : 0; \
    } while(0)

/* ASSERT_*가 longjmp를 발사하면 setjmp가 캐치하여 다음 테스트로 계속합니다. */
#define RUN_TEST(fn) \
    do { \
        _tr_current_fail = 0; \
        if (setjmp(_tr_jmp) == 0) { fn(); } \
        if (_tr_current_fail == 0) { \
            printf("  PASS  " #fn "\n"); ++_tr_pass; \
        } else { \
            printf("  FAIL  " #fn "\n"); ++_tr_fail; \
        } \
    } while(0)

#define EXPECT_TRUE(cond) \
    do { if (!(cond)) { \
        fprintf(stderr, "    expect: %s at %s:%d\n", #cond, __FILE__, __LINE__); \
        ++_tr_current_fail; \
    } } while(0)

#define EXPECT_INT_EQ(a, b) \
    do { int _a = (a), _b = (b); if (_a != _b) { \
        fprintf(stderr, "    expect %s==%s: %d!=%d at %s:%d\n", \
                #a, #b, _a, _b, __FILE__, __LINE__); \
        ++_tr_current_fail; \
    } } while(0)

#define EXPECT_FLOAT_NEAR(a, b, eps) \
    do { float _a = (a), _b = (b), _e = (eps); \
        float _d = _a > _b ? _a - _b : _b - _a; \
        if (_d > _e) { \
            fprintf(stderr, "    expect |%s-%s|<=%g: |%g-%g|=%g at %s:%d\n", \
                    #a, #b, (double)_e, (double)_a, (double)_b, (double)_d, \
                    __FILE__, __LINE__); \
            ++_tr_current_fail; \
    } } while(0)

#define ASSERT_TRUE(cond) \
    do { if (!(cond)) { \
        fprintf(stderr, "    assert: %s at %s:%d\n", #cond, __FILE__, __LINE__); \
        ++_tr_current_fail; longjmp(_tr_jmp, 1); \
    } } while(0)

#define ASSERT_INT_EQ(a, b) \
    do { int _a = (a), _b = (b); if (_a != _b) { \
        fprintf(stderr, "    assert %s==%s: %d!=%d at %s:%d\n", \
                #a, #b, _a, _b, __FILE__, __LINE__); \
        ++_tr_current_fail; longjmp(_tr_jmp, 1); \
    } } while(0)

#endif /* TEST_RUNNER_H */
