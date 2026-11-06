#ifndef FIGHT_CLASSIFIER_H
#define FIGHT_CLASSIFIER_H

#include <stddef.h>

/*
 * 폭행 2차 분류기(ONNX, FP32)의 ORT 래퍼. 입력 [1,9,106] float, 출력 로짓 1개.
 * fight_stage2.h 의 FightScoreFn 콜백으로 쓴다(fight_classifier_score).
 *
 * 왜 별도 모듈인가: 단위 시험(test_core)은 ONNX Runtime 없이 빌드되므로 모니터 로직(fight_stage2.c)과 분리했다.
 * 모델이 없거나 로드에 실패하면 create 가 NULL 을 돌려주고 호출자가 "비활성 + 이유"를 로그로 남긴다(조용히 실패하지 않기).
 */
typedef struct FightClassifier FightClassifier;

/* 성공하면 소유권은 호출자 — fight_classifier_destroy 로 해제(NULL 안전). 실패하면 NULL 과 error. */
FightClassifier *fight_classifier_create(const char *model_path, int threads, char *error, size_t error_size);
void             fight_classifier_destroy(FightClassifier *c);
/* FightScoreFn 형식. context 는 FightClassifier*. window 는 읽기만 한다. 실패하면 -1. */
float            fight_classifier_score(void *context, const float *window);

#endif /* FIGHT_CLASSIFIER_H */
