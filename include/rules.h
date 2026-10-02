#ifndef RULES_H
#define RULES_H

#include "tracks.h"
#include "log.h"
#include "yolo11.h"

/*
 * 룰 기반 이상 탐지입니다. 각 이벤트는 트랙당 1회만 발화(latch)하고,
 * 조건이 해소되면 latch를 해제하여 재발 시 다시 감지합니다.
 */

/*
 * Tier 2 내부 클래스 ID (0-15)입니다. 7클래스 raw 모델은 decode에서
 * food=4, chair=5, dining_table=6을 내부 4,14,15로 정규화합니다.
 * Ultralytics classes= 파라미터로 학습하면 원본 COCO ID가 이 순서대로 재배치됩니다.
 * 원본 COCO: cat=15, dog=16, bottle=39, cup=41, banana=46…cake=55, chair=56, table=60
 */
#define OBJ_CAT         0
#define OBJ_DOG         1
#define OBJ_BOTTLE      2
#define OBJ_CUP         3
#define OBJ_FOOD_FIRST  4   /* banana */
#define OBJ_FOOD_LAST   13  /* cake   */
#define OBJ_CHAIR       14
#define OBJ_DININGTABLE 15

typedef struct {
    double dwell_limit_seconds;       /* 기본 3600 — 초과 체류 판정 기준 */
    double unordered_grace_seconds;   /* 기본 300  — 미주문 착석 유예 시간 */
    double fall_hold_seconds;         /* 기본 5.0  — 쓰러짐으로 확정하는 최소 지속 시간 */
    /* 쓰러짐 bbox 가로/세로 비 임계값.
     * CCTV 각도(높이·기울기)에 따라 최적값이 달라지므로 설정으로 노출합니다.
     * 낮은 CCTV 각도에서는 1.3~1.5, 하향 각도에서는 기본값(1.8/2.2)이 적합합니다. */
    float  fall_aspect_ratio_kp;      /* 기본 1.8 — keypoint 있을 때 */
    float  fall_aspect_ratio_nokp;    /* 기본 2.2 — keypoint 없을 때 */
    /* 키오스크 ROI: 박스 중심이 이 영역 안에 있으면 ORDERED 로 전환 (결제 프록시) */
    float  roi_kiosk_x, roi_kiosk_y, roi_kiosk_w, roi_kiosk_h;
    int    roi_kiosk_set;
    /* Tier 2 물체 감지 설정 */
    float  animal_iou_threshold;  /* 기본 0.15 — 동물/가구 IoU 판정 기준 */
    int    no_cup_margin;         /* 기본 1    — 미구매 착석 판정 여유분 */
    /* ── 미확인 소실 ─────────────────────────────────────────────────────
     * CCTV 사각(斜角)에서 바닥에 누운 사람을 포즈 모델이 아예 탐지하지 못하는
     * 화각이 있습니다. bbox 가 없으면 fall_aspect_ratio 경로가 통째로 죽으므로,
     * "사라졌다" 자체를 별도 신호로 씁니다.
     *
     * 이름을 쓰러짐과 분리한 이유: 칸막이 뒤로 걸어간 경우도 똑같이 사라집니다.
     * 이를 person_fallen 으로 올리면 오탐이 섞여 진짜 응급 알림의 신뢰가 무너집니다.
     * 그래서 사실만 말하는 person_unaccounted(WARN)를 먼저 내고, 그 자리에 움직이지
     * 않는 것이 남아 있을 때만 person_unaccounted_residue(ERROR)로 올립니다. */
    int    vanish_enabled;            /* 기본 1 */
    double vanish_hold_seconds;       /* 기본 5  — 이 시간 이상 미매칭이면 소실로 봄 */
    double vanish_min_dwell_seconds;  /* 기본 3  — 이보다 짧게 추적된 트랙은 무시(깜빡임) */
    float  vanish_min_score;          /* 기본 0.35 — 사라지기 직전 신뢰도가 이 이상이어야 함 */
    float  vanish_edge_margin;        /* 기본 0.08 — 이 비율 안쪽이면 화면 가장자리로 봅니다 */
    /* 1이면 화면 한가운데에서 사라져도 잔류 흔적이 있어야 경고합니다.
     * 칸막이·기둥이 많아 사람이 정상적으로 자주 가려지는 매장용 안전판입니다. */
    int    vanish_require_residue;    /* 기본 0 */
    /*
     * 구역(roi_kiosk) 판정에 쓰는 사람의 기준점. 0 = 박스 중심(기본), 1 = 발밑(아래 가운데).
     *
     * 발밑이 필요한 이유: 카메라가 비스듬히 내려다보면 박스 중심은 사람의 배 높이라,
     * 바닥에 그린 구역과 어긋나고 옆 구역에 걸쳐 앉은 사람이 엉뚱한 구역으로 잡힙니다.
     * 기본을 중심으로 둔 이유: 이미 키오스크 기기 주변(허리 높이)에 그려 둔 ROI 는 발밑
     * 기준으로 바꾸면 손님 발이 ROI 밖으로 빠져 주문 판정이 조용히 꺼집니다.
     * 발밑을 쓰려면 ROI 를 "손님이 서는 바닥"에 다시 그려야 합니다.
     * 구조체 끝에 둔 이유: 위치 초기화({...})를 쓰는 기존 코드가 자동으로 0(중심)이 됩니다.
     */
    int    roi_anchor_foot;

    /*
     * ── Tier 2 물체 룰 확정 필터 ────────────────────────────────────────
     * Tier 2 는 detect_every_obj(기본 90프레임)마다 한 번 돌아 관측 사이 간격이 깁니다.
     * 한 번의 관측으로 발화하면 선풍기→food, 가방→bottle 같은 단발 오검출이 그대로
     * 경고가 됩니다. 최근 window 번의 관측 중 count 번 이상 보여야 확정하고, window 번
     * 연속으로 안 보여야 해제합니다. 연속 K회가 아니라 K-of-N 으로 둔 이유: 컵·병은 손에
     * 가려 한두 번 빠지는 일이 흔해서, 연속 조건이면 진짜 반입도 계속 리셋됩니다.
     * 0 또는 1 이면 필터 없음(관측 즉시 발화, 안 보이면 즉시 해제) — 기존 동작과 같습니다.
     * 구조체 끝에 둔 이유: 위치 초기화({...})를 쓰는 기존 코드가 자동으로 0(필터 없음)이 됩니다.
     */
    int    object_confirm_count;      /* 기본 3 — window 중 이만큼 보여야 확정 */
    int    object_confirm_window;     /* 기본 5 — 최대 16 (비트마스크 폭) */

    /* ── 인원 초과 ───────────────────────────────────────────────────────
     * 0 이면 비활성: 적정 인원은 매장 평수·좌석 수마다 달라 일반적인 기본값이 없습니다.
     * 트래커가 한 사람을 잠깐 둘로 쪼개는 경우가 있어 hold 초 이상 유지돼야 발화합니다. */
    int    max_occupancy;             /* 기본 0(비활성) */
    double max_occupancy_hold_seconds;/* 기본 10 */

    /* ── 장시간 무동작 ───────────────────────────────────────────────────
     * 서 있거나 누운 채 움직임이 없는 사람. 의자에 앉은 사람은 독서·휴대폰으로 정상적으로
     * 오래 움직이지 않으므로 대상에서 제외합니다(무릎이 엉덩이보다 충분히 아래일 때만 "선 자세").
     * 0 이면 비활성. */
    double still_seconds;             /* 기본 300 */
    /* 기준 자세 대비 관절·박스 중심 이동이 박스 높이의 이 비율을 넘으면 움직인 것으로 봅니다.
     * 포즈 추정 자체의 떨림(수 %)보다 커야 서 있는 사람이 "움직임"으로 오인되지 않습니다. */
    float  still_motion_threshold;    /* 기본 0.10 */
} RulesConfig;

/*
 * Tier 2 물체 룰별 확정 상태입니다. 예전에는 track_id==-2 슬롯의 overstay_latched,
 * fall_latched, fall_start 같은 필드를 룰마다 다른 뜻으로 돌려 썼는데, 필드 이름만 보고
 * 쓰러짐 코드를 고치다 물체 룰이 깨지기 쉬운 구조였습니다. 룰마다 전용 상태를 둡니다.
 */
typedef enum {
    OBJ_RULE_DRINK = 0,
    OBJ_RULE_FOOD,
    OBJ_RULE_ANIMAL_CHAIR,
    OBJ_RULE_ANIMAL_TABLE,
    OBJ_RULE_NO_CUP,
    OBJ_RULE_COUNT
} ObjRuleId;

typedef struct {
    unsigned history;  /* 최근 관측 비트열 (bit0 = 가장 최근, 1 = 보임) */
    int      latched;  /* 발화 후 해제 전까지 1 */
    double   since;    /* 확정 시각 — 로그에 지속 시간을 남기기 위함 */
} ObjRuleState;

/*
 * 트랙당 룰 상태입니다. track_id % capacity 로 슬롯을 인덱싱합니다.
 * track_id == -1 이면 미사용 슬롯입니다.
 */
typedef struct {
    int    track_id;
    int    overstay_latched;
    int    unordered_latched;
    int    fall_latched;
    double order_near_start, order_near_last;
    double fall_start;    /* 수평 자세가 시작된 시각 (0이면 미시작) */
    double fall_last_observation, upright_start, upright_last;
    unsigned int upright_samples, fall_samples;
    int fall_gate; /* 0 idle, 1 confidence, 2 stale, 3 pose, 4 no upright history, 5 holding, 6 confirmed */
    /* 장시간 무동작 — 기준 자세(anchor)와 비교해 움직였는지 봅니다.
     * 직전 프레임과 비교하지 않는 이유: 천천히 걷는 사람은 프레임 간 이동이 떨림 수준이라
     * 매번 "정지"로 보이지만, 시작 지점 기준으로는 누적 이동이 드러납니다. */
    double still_start;                 /* 기준 자세를 잡은 시각 (0 이면 미시작) */
    float  still_ax[6], still_ay[6];    /* [0]=박스 중심, [1..5]=FALL_KP_IDX 관절 */
    int    still_valid_mask;            /* 기준 자세에서 유효했던 점의 비트마스크 */
    int    still_latched;
} TrackRuleState;

/*
 * 한 번의 rules_evaluate 에서 새로 발화한 쓰러짐의 상한입니다.
 * 7~10평 매장에서 같은 추론 주기에 8명이 동시에 쓰러질 일은 없고,
 * 넘치면 캡처만 생략될 뿐 이벤트 로그는 그대로 남습니다.
 */
#define RULES_MAX_FIRED 8

typedef struct {
    TrackRuleState *states;  /* RulesEngine 소유, rules_destroy 에서 free */
    size_t          capacity;
    RulesConfig     config;
    /* 현재 프레임 크기. bbox 가 화면 경계에 닿아 잘렸는지 판단하는 데 씁니다.
     * config 가 아니라 여기에 두는 이유: 설정 hot-reload 로 덮이면 안 되는 런타임 값입니다. */
    int frame_width, frame_height;
    /*
     * 이번 rules_evaluate 호출에서 새로 발화한 person_fallen 목록입니다.
     * 매 호출 시작 시 0 으로 비우므로 다음 호출 전까지만 유효합니다.
     * 룰 엔진이 캡처를 직접 하지 않고 목록만 내놓는 이유: 룰은 프레임 픽셀을
     * 모르는 순수 판정으로 두어야 단위 테스트가 FFmpeg·파일 I/O 없이 돌아갑니다.
     */
    ObjRuleState    obj_rule[OBJ_RULE_COUNT]; /* RulesEngine 값 소유 — 별도 해제 없음 */
    double          occ_start;   /* 인원 초과가 시작된 시각 (0 이면 미초과) */
    int             occ_latched;
    int             fall_fired_count;
    int             fall_fired_track[RULES_MAX_FIRED];
    Detection       fall_fired_box[RULES_MAX_FIRED];
} RulesEngine;

int  rules_init(RulesEngine *re, size_t capacity, const RulesConfig *config,
                char *error, size_t error_size);
void rules_destroy(RulesEngine *re);

/* 실행 중 설정 교체 — 기존 latch 상태는 유지합니다. */
void rules_update_config(RulesEngine *re, const RulesConfig *config);

/*
 * 카메라가 gap 초 동안 끊겼다 돌아온 뒤 호출합니다. 진행 중인 쓰러짐 후보를
 * 초기화하여 화면을 못 본 시간이 "수평 자세 유지"로 세어지지 않게 합니다.
 */
void rules_shift_time(RulesEngine *re, double gap);

/*
 * 프레임 크기를 알려 줍니다. 쓰러짐 판정이 "bbox 가 화면 밖으로 잘렸는가"를 확인하는 데 씁니다.
 * 알려 주지 않으면(0) 잘림 검사를 건너뛰고 기존 bbox 비율 경로를 그대로 씁니다.
 */
void rules_set_frame_size(RulesEngine *re, int width, int height);

/*
 * TrackList 전체를 순회하며 룰을 평가합니다.
 * 이벤트 발생 시 event_log 에 근거 key=value 포함 메시지를 기록합니다.
 */
void rules_evaluate(RulesEngine *re, TrackList *tl, double now, EventLog *elog);

/*
 * Tier 2 물체 감지 결과를 기반으로 룰을 평가합니다.
 * 이 함수는 Tier 2 모델 실행 직후 호출됩니다.
 *
 * 모든 이벤트는 object_confirm_count/window 확정 필터를 통과해야 발화합니다.
 *
 * 발화 이벤트:
 *   external_drink     — bottle 감지
 *   external_food      — 음식류(banana..cake) 감지
 *   animal_on_chair    — cat/dog bbox가 chair와 IoU ≥ animal_iou_threshold
 *   animal_on_table    — cat/dog bbox가 dining table과 IoU ≥ animal_iou_threshold
 *   no_cup_seated      — 활성 사람 수 > cup 감지 수 + no_cup_margin
 */
void rules_evaluate_objects(RulesEngine *re, const DetectionList *objs,
                             const TrackList *tl, double now, EventLog *elog);

/*
 * 소실 판정에 필요한 외부 증거입니다. rules.c 는 프레임 크기·문 상태·픽셀을 모르므로
 * 호출자(main.c)가 재서 넘깁니다. 이렇게 나눠 두면 규칙 자체를 영상 없이 시험할 수 있습니다.
 */
typedef struct {
    /* 사라진 뒤 문이 열린 적이 있는가. 1=있음(정상 퇴장 가능) 0=닫힌 채였음 -1=알 수 없음. */
    int door_can_exit;
    /* 마지막 위치에 청결 기준 대비 변화가 남아 있는가. 1=남음 0=없음 -1=확인 불가.
     * 쓰러진 사람은 그 자리에 남고, 가려진 곳으로 걸어간 사람은 바닥이 기준으로 돌아옵니다. */
    int residue_at_spot;
    /* 마지막 위치가 출입문 ROI 근처인가. */
    int near_door;
    /* 마지막 위치가 화면 가장자리인가 — 화각 밖으로 걸어 나갔을 가능성.
     *
     * near_door 와 반드시 구분해야 합니다. 둘을 뭉치면 둘 중 하나가 반드시 깨집니다:
     * 가장자리를 전부 무시하면 문 앞 쓰러짐(가장 위험한 위치)을 놓치고, 문이 닫혔다는
     * 이유로 가장자리를 전부 의심하면 화각 밖으로 걸어 나간 정상 상황이 모두 경고가 됩니다.
     * 카메라가 매장 전체를 덮지 못하는 배치에서는 후자가 오탐의 주된 원인입니다. */
    int near_edge;
} VanishEvidence;

/*
 * 트랙 하나의 소실 여부를 판정합니다. 활성·비활성 트랙 모두에 대해 매 프레임 호출하세요.
 * 추론 프레임으로 제한하면 안 됩니다 — 소실은 "추론이 사람을 못 찾는 상태"라서
 * 추론 주기와 무관하게 시간이 흐릅니다.
 *
 * 반환: 0=변화 없음, 1=person_unaccounted 발화, 2=person_unaccounted_residue 발화
 */
int rules_check_vanish(RulesEngine *re, Track *t, const VanishEvidence *ev,
                       double now, EventLog *elog);

#endif /* RULES_H */
