#ifndef PRIVACY_STYLE_H
#define PRIVACY_STYLE_H

#include <stddef.h>
#include <stdint.h>

/*
 * 보호 모드(stream_privacy_mode)에서 스트림·스냅샷·출력 파일·증거 사진에 내보내는 "가려진 화면"의 모양.
 *
 *   PRIVACY_STYLE_SOLID      단색(기존 동작). 가장 안전하지만 어디서 무슨 일이 있는지 알아보기 어렵다.
 *   PRIVACY_STYLE_EDGE_GRAY  그레이스케일 배경(강한 모자이크 + 강한 가우시안) 위에 약한 가우시안을 건 원본에서
 *                            뽑은 엣지(흰 선)를 얹는다. 방 구조·가구·문의 윤곽은 보이고 색과 질감은 사라진다.
 *
 * 주의(설계 결정): 엣지를 약하게 블러한 원본(edge_blur 로 세기 조절)에서 뽑기 때문에 얼굴 윤곽선 일부가 남을 수 있다. 개발 PC 실험에서
 * pose 모델이 얼굴 키포인트를 일부 검출했다. 이 스타일이 "비식별"로 충분한지는 매장 정책과 법적 판단의 몫이며,
 * 그래서 기본값은 SOLID 다(config: stream_privacy_style). 박스·스켈레톤은 이 함수 뒤에 호출자가 그린다.
 */
typedef enum {
    PRIVACY_STYLE_SOLID = 0,
    PRIVACY_STYLE_EDGE_GRAY = 1,
    /* 열화상: EDGE_GRAY 와 같은 배경(강한 모자이크 + 가우시안)·엣지를 만든 뒤, 배경 밝기를 열화상 팔레트(어두움=검정·보라 →
     * 밝음=주황·노랑)의 색으로 바꿔 보여 준다. 색은 오직 "흐려진 배경 밝기"에서만 나오므로 원본의 색상 정보는 남지 않고,
     * 비식별 수준(얼굴 윤곽선 등)은 EDGE_GRAY 와 같다 — 원본을 그대로 재채색하는 것이 아니다. */
    PRIVACY_STYLE_THERMAL = 2
} PrivacyStyle;

typedef struct {
    int style;       /* PrivacyStyle */
    int solid_gray;  /* SOLID 일 때 채울 회색값(0~255). 0 이하이면 0x24 */
    int cell;        /* 모자이크 한 칸의 픽셀 수(8~64, 기본 24). 클수록 배경 정보가 적다 */
    int edge_low;    /* 엣지 약한 임계(Sobel |gx|+|gy|, 기본 40) */
    int edge_high;   /* 엣지 강한 임계(기본 110). 강한 엣지에 닿은 약한 엣지만 남긴다 */
    int edge_blur;   /* 엣지를 뽑기 전 가우시안 횟수(1~8, 기본 8). 한 번이 시그마≈1, n 번은 ≈√n — 클수록 잔 디테일(얼굴 윤곽·글자)이 엣지에서 빠진다 */
    int edge_soft;   /* 엣지 선을 번지게 하는 가우시안 횟수(0~4, 기본 1). 0 이면 칼같이 선명한 선. 클수록 선이 부드럽고 연해진다 */
    int edge_opacity;/* 엣지 선 농도 %(10~100, 기본 70). 100 이면 배경 위에 완전한 흰색 */
} PrivacyParams;

/* 작업 버퍼. 불완전 타입: 멤버는 privacy_style.c 에만 있다.
 * 소유: 생성한 쪽(main.c 의 AppContext, capture.c 의 EventCapture). 해상도가 바뀌면 내부에서 다시 할당한다.
 * 해제: privacy_scratch_destroy. NULL 을 안전하게 처리한다. */
typedef struct PrivacyScratch PrivacyScratch;

PrivacyScratch *privacy_scratch_create(void);
void            privacy_scratch_destroy(PrivacyScratch *scratch);

/* 범위를 벗어난 값을 허용 범위로 맞춘 기본 매개변수를 돌려준다(SOLID). */
PrivacyParams privacy_params_default(void);
void          privacy_params_sanitize(PrivacyParams *p);

/*
 * rgb 를 제자리에서 가린 화면으로 바꾼다. 호출 뒤 모든 픽셀은 R=G=B(회색)다.
 * rgb: 호출자 소유 프레임, 이 함수가 덮어쓴다. stride 는 바이트 단위 행 간격(>= width*3).
 * 성공 0, 실패(-1: 인자 오류·메모리 부족)이면 rgb 는 SOLID 로 채워져 원본 픽셀이 남지 않는다.
 */
int privacy_style_apply(PrivacyScratch *scratch, uint8_t *rgb, int width, int height, int stride,
                        const PrivacyParams *params);

#endif /* PRIVACY_STYLE_H */
