# 잔류물 감지 설계

> 작성일: 2026-09-09
> 기준 코드: `main.c` 1717줄, `gray.c` 210줄, `door.c` 220줄
> 관련 문서: [tier2-object-detection-plan.md](tier2-object-detection-plan.md), [anomaly-rules.md](anomaly-rules.md), [paper-draft.md](paper-draft.md)

---

## 배경

현재 시스템이 감지하는 시설 이상은 출입문 개폐(`door.c`)와 카메라 장애(`camera_health.c`) 둘뿐이다.
바닥의 토사물, 테이블에 방치된 쓰레기는 감지하지 못한다. 무인 매장에서 이 두 가지는 상주 인력이 없어
장시간 방치되고, 다음 이용객에게 그대로 노출된다.

통상적인 접근은 대상을 클래스로 정의해 검출 모델을 학습하는 것이다. 그러나 **토사물과 쓰레기는
학습 대상으로 삼기 곤란하다.**

- COCO를 비롯한 공개 데이터셋에 해당 클래스가 없다.
- 실제 발생 장면을 촬영해 수집하는 것은 현실적으로도 윤리적으로도 어렵다.
- 형태·색상·재질이 일정하지 않아 클래스로 정의하는 것 자체가 부적절하다.

따라서 본 설계는 대상을 분류하지 않는다. 대신 **"있어서는 안 될 자리에 변화가 남아 있다"** 는
사실만으로 판정한다. 사람이 지나간 자리는 곧 원래대로 돌아오지만 잔류물은 움직이지 않으므로,
변화의 발생이 아니라 **변화의 지속**이 신호가 된다.

이 판정에 필요한 원시 도구는 이미 전부 구현되어 있다. 블록 단위 변화 위치(`gray.c`),
사람 bbox(`tracks.c`), 가구 위치(Tier 2 물체 검출), 기준 이미지 캡처 경로(`stream.c`)를
조합하면 새 모델 없이 동작한다.

---

## 전제와 비목표

### 전제

- 카메라는 고정되어 있다. 팬·틸트·줌이 발생하면 기준 이미지가 무효가 된다.
- 매장의 정상 상태는 고정되어 있다. 사람은 계속 움직이므로 기준을 정의할 수 없지만, 시설은 가능하다.
- 사람 검출과 추적은 이미 동작한다(`tracks.c`). 잔류물 판정은 그 결과를 소비한다.
- 설치 시 점주 또는 설치자가 대시보드에서 청결 상태를 한 번 캡처한다.

### 비목표

- **잔류물의 종류를 확정하지 않는다.** 모든 이벤트는 "의심"이며, 최종 판단은 사람이 한다.
- **잔류물을 남긴 사람을 특정하지 않는다.** 사람 검출은 변화 원인을 배제하는 데만 쓴다.
- 개인 식별·얼굴 인식을 하지 않는다.
- 영상을 외부로 전송하지 않는다. 기준 이미지도 기기 내부에만 둔다.
- 잔류물의 양·부피를 추정하지 않는다. 면적(블록 수)만 보고한다.

---

## 아키텍처

```
카메라 프레임
   │
   ├──────────────────────────────┐
   ▼                              ▼
[배경 상태 분석]              [사람 검출·추적]
 gray.c 블록 변화 산출          tracks.c 활성 트랙 bbox
 기준 이미지 대비 비교           "이 변화가 사람인가"만 판별
   │                              │
   └───────────┬──────────────────┘
               ▼
        [residue.c 판정]
         ├─ 후보 블록 산출
         ├─ 사람 겹침 배제
         ├─ 연결 요소 → 영역
         └─ 지속 시간 확인
               │
               ▼ (확정 시에만)
        [Tier 2 온디맨드 실행]
         가구 bbox 확인 → 종류 분류
               │
               ▼
        이벤트 로그 / 대시보드
```

두 경로의 역할 분담이 핵심이다. 배경 분석이 **"무엇이 달라졌는가"** 를 찾고,
사람 검출은 **"그 변화가 사람 때문인가"** 만 판별한다. 사람 검출은 잔류물이 무엇인지
식별하는 데 전혀 관여하지 않는다. 이 분담 덕분에 학습되지 않은 대상도 감지 범위에 들어온다.

---

## 판정 절차

### 1단계 — 후보 블록 산출

기준 이미지와 현재 프레임을 블록 단위로 비교한다. 블록 하나의 **평균 절대 휘도 차**가
`residue_diff_threshold`(기본 18) 이상이면 후보 블록으로 표시한다.

> 픽셀 개수를 세지 않고 블록 평균을 쓰는 이유: 개수 방식은 센서 노이즈 한 점에도 반응한다.
> 블록 평균은 노이즈가 서로 상쇄되어 면적을 가진 변화만 남는다. `gray_analyze()`가 게이트용으로
> 쓰는 개수 방식(`gray.h:104`)과 목적이 다르므로 별도 순회로 계산한다.

블록 크기는 `GRAY_BLOCK_SIZE 8`(`gray.h:81`)을 그대로 쓴다. 다운샘플 8 기준으로
블록 하나가 원본 64×64px에 대응한다.

### 2단계 — 사람에 의한 변화 배제

활성 트랙(`TrackList.items[].active`)의 bbox를 `residue_person_margin_blocks`(기본 1)만큼
확장하고, 겹치는 후보 블록을 제외한다.

> 여유 블록을 두는 이유: bbox 경계는 프레임마다 미세하게 흔들린다. 여유가 없으면 사람의
> 윤곽선 블록이 매번 후보로 잡혀 노이즈가 된다. `gray_blocks_outside()`가 게이트에서
> 같은 목적으로 `block_margin`을 쓴다(`main.c:1026`).

사람이 서 있는 동안 그 자리는 판정 자체가 보류된다. 사람이 떠나야 잔류 후보로 승격한다.

### 3단계 — 연결 요소로 영역화

남은 후보 블록을 4-이웃 연결 요소로 묶는다. 구성 블록이 `residue_min_blocks`(기본 3)
미만인 영역은 폐기한다.

> 최소 3블록인 이유: 블록 하나가 원본 64×64px이므로 3블록은 대략 A4 용지 절반 정도의 면적이다.
> 그 미만은 그림자, 반사, 작은 조명 변화일 가능성이 높다. 소량 흘림을 놓치는 대가로 오탐을 줄이는
> 선택이며, 매장에 따라 조정한다.

영역 하나가 잔류물 후보 하나에 대응한다. 동시 추적 영역 수는 `RESIDUE_MAX_REGIONS`(8)로 제한한다.

### 4단계 — 지속 시간 확인

후보 영역이 `residue_confirm_seconds`(기본 60) 이상 유지되면 이벤트로 확정한다.

> 60초인 이유: 손님이 물건을 잠시 내려놓았다가 가져가는 정상 행동을 걸러내기 위한 값이다.
> 이 값이 짧으면 테이블에 잠깐 올려둔 가방이 쓰레기로 잡히고, 길면 발견이 늦어진다.
> 점주 정책에 따라 달라지므로 미결 사항으로 남긴다.

관측되지 않는 상태가 `residue_clear_seconds`(기본 10) 이상 지속되면 해소로 판정하고 래치를 푼다.

> 해소 판정에 10초를 두는 이유: 청소 중인 사람이 영역을 가리면 한두 프레임 관측이 끊긴다.
> 즉시 해소로 처리하면 청소가 끝나기 전에 래치가 풀려 같은 이벤트가 다시 발화한다.

### 5단계 — 위치 기반 종류 분류

확정 시점에만 Tier 2 물체 검출을 1회 실행하고, 그 결과의 가구 bbox와 비교한다.

| 잔류 영역의 위치 | 판정 | 이벤트 |
|---|---|---|
| `dining table` 또는 `chair` bbox와 겹침 | 방치된 쓰레기 의심 | `residue_trash` |
| 겹치지 않음 (바닥) | 토사물·액체 흘림 의심 | `residue_spill` |

겹침 판정은 IoU가 아니라 **잔류 영역 중심이 가구 bbox 안에 있는지**로 한다.

> IoU를 쓰지 않는 이유: 잔류 영역은 테이블보다 훨씬 작아 IoU가 구조적으로 낮게 나온다.
> 작은 물체가 큰 영역 안에 있는지를 묻는 문제이므로 중심점 포함 판정이 맞다.
> `rules.c`의 동물-가구 판정이 IoU 0.15를 쓰는 것(`tier2-object-detection-plan.md`)과 다른 선택이며,
> 그쪽은 동물과 의자의 크기가 비슷해 IoU가 성립하기 때문이다.

**Tier 2를 온디맨드로 실행하는 이유**: 현재 Tier 2는 `detect_every_obj`(기본 90) 주기로만 돈다
(`main.c:1119-1131`). 가구 위치는 자주 바뀌지 않으므로 잔류가 확정된 순간에만 확인해도 충분하고,
평상시 추가 부하가 없다. Tier 2가 비활성(`obj_detector == NULL`)이면 분류를 생략하고
`residue_unknown`으로 보고한다.

---

## 자료구조

```c
#define RESIDUE_MAX_REGIONS 8

typedef enum {
    RESIDUE_UNKNOWN = 0,  /* Tier 2 비활성 — 종류 미분류 */
    RESIDUE_SPILL   = 1,  /* 바닥 — 토사물·액체 흘림 의심 */
    RESIDUE_TRASH   = 2   /* 가구 위 — 방치된 쓰레기 의심 */
} ResidueKind;

/*
 * 추적 중인 잔류 영역 하나입니다.
 * 좌표는 원본 프레임 기준이며, 블록 격자에서 역산합니다.
 */
typedef struct {
    int         in_use;       /* 0 이면 빈 슬롯 */
    int         confirmed;    /* 이벤트를 이미 발화했는가 */
    ResidueKind kind;
    double      first_seen;   /* 후보로 처음 잡힌 시각 (monotonic) */
    double      last_seen;    /* 마지막으로 관측된 시각 */
    int         blocks;       /* 구성 블록 수 — 면적의 대용 */
    float       x1, y1, x2, y2;
} ResidueRegion;

typedef struct {
    int    enabled;
    int    diff_threshold;          /* residue_diff_threshold */
    int    min_blocks;              /* residue_min_blocks */
    int    person_margin_blocks;    /* residue_person_margin_blocks */
    double confirm_seconds;         /* residue_confirm_seconds */
    double clear_seconds;           /* residue_clear_seconds */
    double baseline_refresh_seconds;/* residue_baseline_refresh_seconds */
    float  global_change_ratio;     /* residue_global_change_ratio */
} ResidueConfig;

typedef struct {
    ResidueConfig config;

    /* 청결 상태 기준. residue_clean_reference.raw 에서 로드하거나
     * 안전 시점에 현재 프레임으로 갱신합니다.
     * ResidueMonitor 소유 — residue_destroy 에서 free 합니다.
     * gray 버퍼와 크기가 다르면 무효로 간주하고 재캡처를 요구합니다. */
    uint8_t *baseline;
    int      baseline_w, baseline_h;
    int      baseline_ready;
    double   baseline_stamp;   /* 마지막 갱신 시각 */

    /* 후보 블록 표시용 작업 버퍼. 프레임마다 재사용하며 매번 할당하지 않습니다.
     * ResidueMonitor 소유 — residue_destroy 에서 free 합니다. */
    uint8_t *candidate;
    int      blocks_x, blocks_y;

    ResidueRegion regions[RESIDUE_MAX_REGIONS];
} ResidueMonitor;
```

```c
/*
 * gray: 현재 프레임의 다운샘플 그레이 버퍼. 호출자(AppContext) 소유이며 읽기만 합니다.
 *       이 함수가 반환한 뒤 보관하지 않습니다.
 * persons: 활성 트랙 bbox 배열. 원본 프레임 좌표계. 호출자 소유이며 읽기만 합니다.
 * elog: 이벤트 로그. NULL 이면 로그를 남기지 않습니다.
 *
 * 반환값: 확정된 영역 수, 실패 시 -1.
 */
int residue_evaluate(ResidueMonitor *r,
                     const GrayBuf *gray,
                     const GrayRect *persons, int person_count,
                     double now, EventLog *elog);
```

---

## 설정 항목

| 설정 키 | 기본값 | 설명 |
|---|---:|---|
| `residue_enabled` | 1 | 기능 활성화. 기준 이미지가 없으면 자동으로 비활성 동작 |
| `residue_diff_threshold` | 18 | 블록 평균 휘도 차 임계. 낮추면 민감해지고 조명에 취약해진다 |
| `residue_min_blocks` | 3 | 영역 최소 블록 수. 블록 하나 = 원본 64×64px |
| `residue_person_margin_blocks` | 1 | 사람 bbox 확장량. bbox 경계 흔들림 흡수 |
| `residue_confirm_seconds` | 60 | 이벤트 확정까지의 지속 시간 |
| `residue_clear_seconds` | 10 | 해소 확정까지의 미관측 시간 |
| `residue_baseline_refresh_seconds` | 300 | 안전 시점 기준 자동 갱신 주기 |
| `residue_global_change_ratio` | 0.5 | 이 비율을 넘는 후보는 조명 변화로 해석 |

키 이름은 `<모듈>_<속성>` snake_case 규칙을 따른다(`door_diff_threshold`, `door_open_seconds`와 동일).

---

## 이벤트 / 로그

| 이벤트 | 조건 | 레벨 |
|---|---|---|
| `residue_trash` | 가구 위 잔류가 `confirm_seconds` 이상 지속 | WARN |
| `residue_spill` | 바닥 잔류가 `confirm_seconds` 이상 지속 | WARN |
| `residue_unknown` | 잔류 확정, Tier 2 비활성으로 종류 미분류 | WARN |
| `residue_cleared` | 확정된 영역이 `clear_seconds` 이상 미관측 | INFO |
| `residue_baseline_reset` | 전역 변화로 기준을 강제 갱신 | INFO |
| `residue_baseline_missing` | 기동 시 기준 이미지 없음 — 기능 비활성 | WARN |

로그 모듈 이름은 `"residue"`를 쓴다. 메시지의 **첫 토큰이 곧 이벤트 이름**이어야 한다 —
`dashboard/server.c:146-152`의 `parse_log_line()`이 첫 공백까지를 `event` 필드로 추출하기 때문이다.

```
2026-09-09T14:22:07 WARN  residue  residue_spill blocks=7 x=412 y=688 held=63s
2026-09-09T14:31:55 INFO  residue  residue_cleared kind=spill x=412 y=688
2026-09-09T18:02:11 INFO  residue  residue_baseline_reset ratio=0.71 reason=global_change
```

---

## 기준 이미지 관리

### 수동 캡처

설치 시 대시보드에서 청결 상태를 캡처해 `residue_clean_reference.raw`로 저장한다.
`stream.c:230-261`의 `handle_door_save()` 경로를 그대로 재사용한다. 파일 포맷도 동일하게
`[int32 width][int32 height][w*h*3 RGB]`를 쓴다.

> raw를 쓰는 이유는 door와 같다 — JPEG 재압축의 양자화 오차가 픽셀 비교에 그대로 섞이기 때문이다
> (`stream.c:221-229` 주석).

현재 `handle_door_save()`는 `is_open` 불리언과 하드코딩된 파일명으로 되어 있다
(`stream.c:245-246`). **파일명을 인자로 받는 형태로 일반화**하고 라우터에서 이름을 결정한다.

### 자동 갱신

조명은 시간대에 따라 서서히 변한다. 수동 기준만 쓰면 저녁이 되면서 화면 전체가 임계를 넘어
오탐이 발생한다. 따라서 **안전한 시점에만** 기준을 현재 프레임으로 갱신한다.

안전 조건은 세 가지를 모두 만족해야 한다.

1. 활성 사람 트랙이 하나도 없다.
2. 후보 블록이 하나도 없다.
3. 마지막 갱신으로부터 `residue_baseline_refresh_seconds`(기본 300) 이상 지났다.

> 2번이 결정적이다. 잔류물이 있는 상태를 기준으로 갱신하면 그 잔류물이 정상 상태가 되어
> 영원히 감지되지 않는다. 후보가 하나라도 있으면 갱신하지 않는다.

---

## 조명 변화 대응

배경 차분의 최대 취약점은 조명 변화다. 소등·점등, 구름에 의한 자연광 변화는 화면 전체를 바꾼다.

후보 블록이 전체 블록의 `residue_global_change_ratio`(기본 0.5)를 넘으면 **잔류물이 아니라
조명 변화로 해석**한다. 이때 추적 중인 영역을 모두 폐기하고 기준을 강제 갱신한 뒤
`residue_baseline_reset` 로그를 남긴다.

> 근거: 잔류물이 화면의 절반을 덮는 상황은 현실적으로 없다. 절반을 넘는 변화는 전역 요인이다.
> 이 가드가 없으면 소등 순간 화면 전체가 하나의 거대한 잔류 영역으로 잡힌다.

문 감지가 쓰는 이중 기준 프레임 비교(`door.h:16-24`)는 여기에 직접 적용할 수 없다.
문은 "열림"이라는 대비 상태를 미리 등록할 수 있지만, 잔류물은 이상 상태를 미리 캡처할 수 없기
때문이다. 자동 갱신과 전역 변화 가드가 그 역할을 대신한다.

---

## 시나리오

### 정상 — 바닥 흘림 감지

```text
14:20:00  손님 입장, 좌석 이동                     → 사람 트랙 활성, 판정 보류
14:21:10  바닥에 음료를 쏟음                        → 변화 발생. 사람과 겹쳐 아직 배제됨
14:21:40  손님이 자리를 뜸                          → 후보 영역 승격, first_seen 기록
14:22:40  60초 경과                                → 확정. Tier 2 온디맨드 실행
                                                    가구 bbox와 겹치지 않음
                                                  → residue_spill 발화 (WARN)
14:31:55  청소 완료, 10초간 미관측                  → residue_cleared 발화 (INFO)
```

### 오탐 위험 — 잠시 내려둔 가방

```text
15:02:00  손님이 테이블에 가방을 올려둠              → 사람과 겹쳐 배제
15:05:30  손님이 화장실로 이동                      → 후보 영역 승격
15:06:10  40초 경과, 아직 60초 미만                 → 확정 안 됨
15:06:20  손님 복귀, 가방 회수                      → 후보 소멸. 이벤트 없음
```

> `confirm_seconds`가 이 시나리오를 걸러낸다. 값을 30초로 낮추면 이 경우가 오탐이 된다.

### 근거 부족 — Tier 2 비활성

```text
20:14:00  바닥에 무언가 잔류, 60초 경과             → 확정
          obj_detector == NULL (--obj-model 미지정)  → 가구 위치를 알 수 없음
                                                  → residue_unknown 발화 (WARN)
```

종류를 모른다는 사실을 숨기지 않고 별도 이벤트로 보고한다. 점주가 화면을 보고 판단한다.

---

## 학습 기반 확장 (나중에)

1단계 구현은 학습을 전혀 쓰지 않는다. 데이터가 쌓인 뒤 2단계를 검토한다.

| 단계 | 방식 | 필요 데이터 | 상태 |
|---|---|---|---|
| 1 | 규칙 기반 잔류 감지 (본 문서) | 없음 | 이번 구현 |
| 2 | 확정 영역 crop → 소형 분류기 | 운영 중 수집분 | 미착수 |

2단계 데이터 수집 경로는 명확하다. 1단계가 확정한 잔류 영역을 crop해 저장하면
**이미 잔류물이라고 판정된 이미지**만 모인다. 무작위 촬영보다 라벨링 비용이 훨씬 낮다.

> **모델 무게에 대한 오해를 정정해 둔다.** 클래스를 하나 늘려도 모델은 거의 무거워지지 않는다.
> YOLO에서 클래스 수는 detection head의 출력 채널에만 영향을 주며, 16→17 클래스는 스케일당
> 출력 채널이 80→81로 늘 뿐이다. 전체 연산량 증가는 1% 미만, 모델 크기 증가는 수 KB다.
> 1단계와 2단계를 나누는 이유는 **연산 비용이 아니라 학습 데이터 확보 가능 여부**다.

2단계 진행 시 영상 저장이 발생하므로, 프라이버시 원칙(영상 외부 전송 금지, 로컬 처리)과의
충돌 여부를 먼저 검토해야 한다. 사람이 없는 시점의 잔류 영역만 crop하므로 인물이 찍히지 않지만,
저장 자체에 대한 점주 동의가 필요하다.

---

## 구현 범위

파일별 작업 목록이다. **소스 목록 4곳이 동기화되어야 한다** — 한 곳만 빠뜨리면 링크 에러나
테스트 미실행으로 조용히 드러난다.

| # | 파일 | 위치 | 작업 |
|---|---|---|---|
| 1 | `include/residue.h` | 신규 | `door.h` 구조를 모방 — 설계 사유 주석 → struct → init/destroy/evaluate |
| 2 | `src/residue.c` | 신규 | `door.c:11-40`의 `load_raw()` 재사용 |
| 3 | `Makefile` | 43-46 | `SOURCES`에 `src/residue.c` |
| 4 | `Makefile` | 86-89 | `TEST_CORE_SRCS`에 `src/residue.c` |
| 5 | `Makefile` | 91-95 | `build/test_core` 헤더 선행조건에 `include/residue.h` |
| 6 | `CMakeLists.txt` | 66-82 | `yolo11-person` 타겟 |
| 7 | `CMakeLists.txt` | 116-128 | `test_core` 타겟 |
| 8 | `src/main.c` | 3, 64, 137 | include / `residue_seconds` 계측 / `AppContext` 필드 |
| 9 | `src/main.c` | 631-657 근처 | `apply_residue_config()` 신설 |
| 10 | `src/main.c` | 689-702, 1509-1516 | 설정 주입 — **시작·reload 두 경로 모두** |
| 11 | `src/main.c` | 737-760, 801-805 | 기준 파일 mtime 2초 폴링 |
| 12 | `src/main.c` | 1643-1660, 1581-1583, 1704 | 기준 로드 / 기동 로그 / `residue_destroy` |
| 13 | `src/main.c` | **1197 직후** | `residue_evaluate()` 호출 + 이벤트 발화 |
| 14 | `src/main.c` | 1292, 1334 | metrics 포맷 문자열과 인자를 **짝으로** |
| 15 | `src/stream.c` | 230-261, 264-305, 409-415 | 기준 캡처 일반화 (파일명을 인자로) |
| 16 | `include/stream.h` | 9-13, 50-55, 65-76 | 주석 + API + **non-Win32 스텁** |
| 17 | `dashboard/server.c` | 404-428 | `DEFAULT_CONFIG`에 키 8개 추가 |
| 18 | `dashboard/index.html` | 414-439, 784-789, 806-825, 861-891 | 폼 / `loadSettings` / `saveSettings` / 캡처 버튼 |
| 19 | `config.json`, `config/store.example.ini` | 전체 | 키 추가 |
| 20 | `tests/test_core.c` | 11, 1153 근처, **1503 근처** | include / 테스트 함수 / `RUN_TEST` 등록 |
| 21 | `src/draw.c` | 488 이후 | 잔류 영역 표시 |

### 반드시 지킬 3가지

구현 중 놓치면 조용히 깨지는 지점이다.

1. **`residue_evaluate()`는 `main.c:1197` 직후, 그리기(1206-1230) 이전에 배치한다.**
   `main.c:1143-1152` 주석이 door에 대해 같은 이유를 경고한다 — 박스와 HUD 픽셀이
   기준 이미지 비교에 섞여 들어간다. 픽셀 비교 기반 모듈은 모두 그리기 이전이어야 한다.

2. **설정 주입은 시작(`main.c:1509-1516`)과 reload(`main.c:689-702`) 두 경로 모두 처리한다.**
   `main.c:624-630` 주석에 "한쪽만 고쳐서 재시작 전까지 반영되지 않았다"는 사고 기록이 있다.
   door는 이 패턴을 따르지 않아 코드가 두 곳에 중복되어 있으므로, residue는
   `apply_gate_config()`(`main.c:631-657`) 스타일의 헬퍼 하나로 만든다.

3. **`dashboard/server.c:404-428`의 `DEFAULT_CONFIG`를 갱신한다.**
   `config.json`이 없는 신규 설치에서만 드러나는 누락이라 개발 중에는 발견되지 않는다.

### 시각화의 링크 제약

`draw_residue()`를 `ResidueMonitor *`를 받는 형태로 `src/draw.c`에 정의하면 안 된다.
`test_detector` 타겟(`CMakeLists.txt:144-150`)이 `src/draw.c`를 포함하지만 `residue.c`는
링크하지 않으므로 심볼을 찾지 못한다. **구조체 의존이 없는 형태**로 설계한다.

```c
void draw_residue_roi(uint8_t *rgb, int w, int h, int stride,
                      int x, int y, int rw, int rh, int alert);
```

색상은 `draw_obj_detections`의 클래스별 색(`draw.c:452-468`)과 `draw_tracks`의
녹색 `(0,224,96)` / 주황 `(255,140,0)`(`draw.c:385, 389`)을 피해 고른다.

---

## 재사용할 기존 코드

새로 만들지 않는다.

| 필요한 기능 | 재사용할 것 |
|---|---|
| 다운샘플 그레이 버퍼 | `GrayBuf`, `gray_buf_update_luma()` — `gray.h:18, 47-48` |
| 블록 격자 좌표 변환 | `GRAY_BLOCK_SIZE`, `MotionMap` — `gray.h:81-95` |
| 사람 bbox와 블록 겹침 | `gray_blocks_outside()` — `gray.h:126-128` |
| raw 기준 이미지 로딩 | `load_raw()` — `door.c:11-40` |
| 기준 이미지 캡처 (대시보드) | `handle_door_save()` — `stream.c:230-261` |
| 사람 활성 트랙 | `TrackList.items[].active`, `.box` — `tracks.h:28-41` |
| 가구 위치 | Tier 2 `obj_detections` — `main.c:1119-1131` |
| 설정 파싱 | `config_long/float` — `config.h:29-33` |
| 이벤트 로그 | `event_log_write()` — `log.h:27-28` |

---

## 검증 지표

### 단위 테스트

`residue_evaluate()`는 파일 I/O 없이 테스트 가능해야 한다. door 테스트가 `door_load()`를
거치지 않고 `ref_*_rgb`를 직접 malloc/memcpy 하는 패턴(`tests/test_core.c:1128-1153`)을 따른다.

| 테스트 | 검증 내용 |
|---|---|
| `test_residue_confirms_after_hold` | `confirm_seconds` 경과 전에는 이벤트 없음, 경과 후 1회 발화 |
| `test_residue_excludes_person_overlap` | 사람 bbox와 겹치는 변화는 후보가 되지 않음 |
| `test_residue_min_blocks` | `min_blocks` 미만 영역은 폐기 |
| `test_residue_global_change_resets` | 전역 변화 비율 초과 시 영역 폐기 + 기준 갱신 |
| `test_residue_clears_after_absence` | `clear_seconds` 후 해소 이벤트, 래치 해제 |
| `test_residue_latches_once` | 조건 지속 중 이벤트가 반복 발화하지 않음 |

이벤트 발화 여부는 `tmpfile()` + `ftell()` 전후 비교로 검증한다
(`tests/test_core.c:1444-1448` 패턴).

### 운영 지표

논문 검증과 연결되는 지표다. 별도 측정 도구가 필요하며 아직 없다.

| 지표 | 정의 |
|---|---|
| 검출률 | 재현한 잔류 상황 중 감지된 비율 |
| 오탐률 | 잔류물이 없는 상태에서 시간당 발생한 이벤트 수 |
| 검출 지연 | 잔류 발생 시각과 이벤트 확정 시각의 차이 |

오탐 측정은 **오탐 유발 영상**으로 한다 — 조명 점등·소등, 창을 통한 자연광 변화,
손님이 물건을 잠시 놓았다 회수하는 동작.

---

## 미결 사항

- [ ] `residue_confirm_seconds` 기본값 60초가 적절한지 — 손님이 물건을 잠시 내려놓는 시간과 구분되어야 한다. 점주 정책 영향
- [ ] `residue_min_blocks` 기본값 3이 적절한지 — 소량 흘림을 놓치는 정도를 실측해야 결정 가능
- [ ] `residue_cleared` 이벤트를 실제로 발생시킬지 — 점주에게 유용한지 확인 필요
- [ ] 매장 배치 변경 시 기준 재캡처를 어떻게 안내할지 — 자동 감지 가능한지 검토
- [ ] Tier 2 온디맨드 실행이 확정 순간의 프레임 지연을 얼마나 유발하는지 실측
- [ ] 2단계 학습 트랙의 데이터 수집 진행 여부 — 저장에 대한 점주 동의 필요
- [ ] 카메라가 물리적으로 흔들렸을 때 기준 무효화를 자동 판정할지
