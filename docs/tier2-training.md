# Tier 2 모델 학습 가이드

작성일: 2026-09-11

다른 머신에서 Tier 2 객체 검출 모델을 처음부터 재현할 수 있도록 정리한 문서다.
데이터셋 구축 → 학습 → 평가 → 배포까지 전 과정과, 실제로 겪은 함정을 함께 기록한다.

관련 문서:
- [Tier 2 데이터셋 레이블 오매핑 문제](2026-09-10-tier2-dataset-labeling-issue.md) — 이 작업의 출발점
- [i5-4200u 최적화](i5-4200u-optimization.md) — 배포 대상 기기 제약
- [quantization](quantization.md) — INT8 변환

---

## 1. 배경

`docs/2026-09-10-tier2-dataset-labeling-issue.md`에 기록된 레이블 오매핑이 미해결 상태였다.
2026-09-11에 원인을 확정하고 수정했다.

**원인**: `scripts/train_tier2.py`가 COCO **80-클래스 인덱스**를
`instances_*.json`의 **91-카테고리 ID**로 그대로 사용했다. 두 체계는 다르다.

| 의도한 클래스 | 스크립트가 쓴 ID | 그 ID의 실제 이름 | 올바른 ID |
|---|---|---|---|
| cat | 15 | bench | 17 |
| dog | 16 | bird | 18 |
| bottle | 39 | baseball bat | 44 |
| cup | 41 | skateboard | 47 |
| banana | 46 | wine glass | 52 |
| apple | 47 | cup | 53 |
| sandwich | 48 | fork | 54 |
| orange | 49 | knife | 55 |
| broccoli | 50 | spoon | 56 |
| carrot | 51 | bowl | 57 |
| hot dog | 52 | banana | 58 |
| pizza | 53 | apple | 59 |
| donut | 54 | sandwich | 60 |
| cake | 55 | orange | 61 |
| chair | 56 | broccoli | 62 |
| dining table | 60 | donut | 67 |

**수정 방침**: ID 상수표를 손으로 유지하지 않는다. JSON의 `categories` 배열에서
**클래스 이름으로 ID를 조회**한다. 상수표는 언젠가 다시 밀리지만, 이름 조회는
구조적으로 밀릴 수 없다.

폐기 대상:
- `datasets/tier2/labels/**` — 전량 오라벨
- `runs/tier2*`, `runs/detect/runs/tier2`, `runs/detect/runs/tier2_fast` 가중치
- 오라벨 데이터로 만든 `models/yolo11n_tier2_fp32.onnx`, `models/yolo11n_tier2_int8.onnx`

`datasets/tier2/images/**`와 `datasets/tier2/annotations/**`는 재사용 가능하다
(이미지 파일 자체와 COCO 원본 어노테이션은 문제없음).

---

## 2. 실행 환경

### 2.1 GPU와 PyTorch 빌드 — 가장 흔한 함정

**PyTorch cu128 이상 빌드는 Pascal(sm_61) 커널을 포함하지 않는다.**
GTX 10xx 계열에서 `torch.cuda.is_available()`는 `True`를 반환하지만, 실제 커널을
띄우는 순간 죽는다.

```
CUDA error: no kernel image is available for execution on the device
```

`is_available()`만으로 판단하면 학습을 몇 시간 돌린 뒤에야 실패를 알게 된다.
`scripts/run_train_tier2.py`의 `preflight()`가 시작 전에 실제 matmul을 한 번
실행해 이를 걸러낸다.

| GPU 세대 | Compute Capability | 권장 PyTorch |
|---|---|---|
| Pascal (GTX 10xx) | sm_61 | `--index-url .../whl/cu126` (2.7.1 검증됨) |
| Turing (RTX 20xx) 이상 | sm_75+ | 최신 cu128 빌드 사용 가능 |

Pascal 환경 설치 예:

```powershell
py -3.13 -m venv .venv-train
.venv-train\Scripts\python.exe -m pip install --upgrade pip
.venv-train\Scripts\python.exe -m pip install torch==2.7.1 torchvision==0.22.1 `
  --index-url https://download.pytorch.org/whl/cu126
.venv-train\Scripts\python.exe -m pip install ultralytics onnx onnxruntime onnxslim
```

기존 콘다 환경의 torch를 다운그레이드하지 말고 **별도 venv를 만들 것.**
다른 프로젝트가 같은 환경을 공유하고 있으면 연쇄로 깨진다.

### 2.2 AMP는 Pascal에서 끌 것

Pascal 소비자형 GPU(GP104/106/107)는 FP16 연산 처리율이 FP32의 1/64다.
텐서코어가 없어 AMP의 이득이 없고, 메모리만 줄고 연산은 오히려 느려진다.
`run_train_tier2.py`의 기본값은 `amp=False`이며, Ampere 이상에서는 `--amp`로 켠다.

### 2.3 VRAM

GTX 1050(2GB)에서 YOLO11n @320은 `batch=16`이 상한이다. 데스크톱 컴포지터가
이미 500MB 정도를 점유하므로 실사용 가용분은 1.5GB 남짓이다.
`run_train_tier2.py`는 OOM 발생 시 batch를 절반으로 낮춰 자동 재시도한다.

AutoBatch(`batch=-1`)는 측정 시점의 여유 메모리를 기준으로 과대 추정하는 경향이
있어 이 환경에서는 쓰지 않는다.

### 2.4 Windows dataloader 워커 메모리 — 두 번째 함정

Windows는 dataloader 워커를 fork가 아닌 **spawn**으로 띄운다. 워커마다 torch를
새로 import하므로 **프로세스당 약 500MB**가 든다. Ultralytics는 train 로더와
val 로더에 각각 워커를 띄우므로 실제 프로세스 수는 `workers` 값의 2배 이상이다.

실측 (RAM 16GB 머신):

| workers | python 프로세스 | 합계 RAM | 에폭 시간 |
|---|---|---|---|
| 8 | 19개 | 9.0 GB | 348초 |
| 4 | 14개 | 7.3 GB | 346초 |

**속도 차이가 없다.** GPU가 병목이므로 워커를 줄여도 손해가 없다.
RAM 16GB 이하 Windows 머신에서는 `--workers 4` 이하를 권장한다.
Linux는 fork라서 이 문제가 없다.

---

## 3. 데이터셋 구축

### 3.1 스크립트

`scripts/build_tier2_balanced.py`

```powershell
# 분포만 확인 (다운로드 없음)
.venv-train\Scripts\python.exe scripts\build_tier2_balanced.py --preset v2 --report-only

# 실제 구축
.venv-train\Scripts\python.exe scripts\build_tier2_balanced.py --preset v2 --workers 24
```

주요 인자:

| 인자 | 기본값 | 설명 |
|---|---|---|
| `--preset` | `v2` | `v2`(7클래스) 또는 `v1`(16클래스) |
| `--ann` | `datasets/tier2/annotations` | COCO 어노테이션 위치 |
| `--cache` | `datasets/tier2 datasets/tier2_bal` | 이미지 재사용원 (하드링크) |
| `--dst` | `datasets/tier2_v2` | 출력 위치 |
| `--workers` | 24 | 다운로드 병렬 수 |

COCO 어노테이션이 없으면 먼저 받는다 (약 241MB):

```
http://images.cocodataset.org/annotations/annotations_trainval2017.zip
```

### 3.2 전처리 정책과 근거

| 정책 | 근거 |
|---|---|
| 카테고리 ID를 **이름으로 조회** | 상수표 하드코딩이 1절의 오라벨을 만들었다 |
| `iscrowd=1` 어노테이션 제외 | bbox가 군집 전체를 감싸 학습에 해롭다 |
| 폭·높이 2px 미만 박스 제외 | 320px 학습에서 1px 미만이 되는 노이즈 |
| **언더샘플링** (오버샘플링 아님) | 이미지 복제는 320px 저해상도에서 과적합만 키우고 에폭 시간을 늘린다 |
| **데이터셋 레벨 균형** (손실 가중치 아님) | Ultralytics가 클래스 가중치 주입을 공식 지원하지 않는다 |
| **희귀 클래스부터** 그리디 선택 | COCO는 다중 레이블이라 흔한 클래스는 희귀 클래스 이미지에 덤으로 딸려온다. 희귀부터 채워야 흔한 클래스 예산이 낭비되지 않는다 |
| 이미지 **하드링크** 재사용 | 수만 장 중복 복사를 피한다. 볼륨이 다르면 자동으로 복사 폴백 |

### 3.3 v2 클래스 스펙

무인 카페 감지 대상에 맞춘 7클래스다.

| idx | 클래스 | COCO 원본 | train 상한 | 용도 |
|---|---|---|---|---|
| 0 | cat | cat | 무제한 | 반려동물 의자 착석 |
| 1 | dog | dog | 무제한 | 반려동물 의자 착석 |
| 2 | bottle | bottle | 6,000 | 외부 음료 반입 |
| 3 | cup | cup | 6,000 | 주문 음료 / 시럽 과다 |
| 4 | food | banana, apple, sandwich, orange, broccoli, carrot, hot dog, pizza, donut, cake | 8,000 | 외부 음식 반입·취식 |
| 5 | chair | chair | 무제한 | 착석 감지, 장기 체류 |
| 6 | dining table | dining table | 무제한 | 좌석 점유, 잔여물 |

**음식 10종을 `food` 하나로 병합한 이유**
감지 규칙이 "외부 음식 반입"이므로 바나나인지 당근인지 구분할 필요가 없다.
v1 실측에서 apple 0.114, carrot 0.178처럼 개별 AP가 낮았는데, 병합하면 클래스당
학습 데이터가 10배가 되어 훨씬 안정적으로 잡힌다. 룰 기반 판정에는 영향이 없다.

**chair / dining table 상한을 푼 이유**
v1에서 chair를 12,774장 → 2,500장으로 깎았더니 AP50-95가 0.094로 떨어져,
COCO 전체를 학습한 사전학습 모델(0.160)보다 나빠졌다. 착석 감지·장기 체류·
반려동물 의자 착석 판정이 전부 chair에 의존하므로 이 클래스는 깎으면 안 된다.
**균형 조정은 목적이 아니라 수단이다. 제품이 의존하는 클래스를 깎으면 본말전도다.**

### 3.4 v2 예상 규모 (실측)

train 33,147장 / val 1,677장, 불균형비 3.1배 (원본 4.0배).

| 클래스 | train 이미지 | train 인스턴스 | val 이미지 |
|---|---|---|---|
| cat | 4,114 | 4,766 | 184 |
| dog | 4,385 | 5,500 | 177 |
| bottle | 8,434 | 23,906 | 379 |
| cup | 9,122 | 20,425 | 390 |
| food | 9,169 | 29,411 | 640 |
| chair | 12,774 | 38,062 | 580 |
| dining table | 11,837 | 15,687 | 501 |
| **합계** | **33,147장** | **137,757** | **1,677장** |

디스크: 이미지 약 4.5GB + 어노테이션 약 0.8GB.

---

## 4. 학습

### 4.1 실행

```powershell
.venv-train\Scripts\python.exe scripts\run_train_tier2.py `
  --data datasets\tier2_v2\tier2_v2.yaml `
  --epochs 80 --batch 16 --device 0 --workers 4
```

주요 인자:

| 인자 | 기본값 | 설명 |
|---|---|---|
| `--batch` | 16 | OOM 시 자동으로 절반씩 낮춤 |
| `--workers` | 8 | Windows·RAM 16GB면 4 이하 권장 (2.4절) |
| `--amp` | off | Ampere 이상에서만 켤 것 (2.2절) |
| `--resume` | — | 중단된 학습을 `last.pt`에서 이어서 진행 |
| `--no-export` | off | ONNX 변환 생략 |

입력 해상도는 `IMGSZ = 320` 고정이다. 3~5m 거리에서 컵·병을 감지할 수 있는
최소 해상도이며, 배포 대상 i5-4200U의 부하 상한이 이 값을 결정한다.

### 4.2 소요 시간

GTX 1050(2GB) 기준 실측: **21,108장에서 346초/에폭**.
이미지 수에 거의 선형으로 비례한다.

| 데이터셋 | 에폭 시간 | 80에폭 |
|---|---|---|
| v1 (21,108장) | 346초 | 약 7.7시간 |
| v2 (33,147장) | 약 543초 (추정) | 약 12시간 |

GTX 1050은 텐서코어가 없고 640코어뿐이다. RTX 3060급 이상에서는 AMP를 켜고
batch를 64 이상으로 올려 훨씬 빠르게 끝낼 수 있다.

참고로 이 개발 PC의 CPU는 i7-12700(12C/20T)인데, GPU가 CPU보다 3~4배 빠른 정도다.
GTX 1050급이면 GPU 우위가 크지 않으므로, GPU가 없는 머신에서도 CPU 학습이
현실적인 대안이 된다(다만 20시간 이상).

### 4.3 중단 후 재개

```powershell
.venv-train\Scripts\python.exe scripts\run_train_tier2.py `
  --resume runs\detect\runs\tier2_v2\train\weights\last.pt
```

**주의**: `resume=True`일 때 Ultralytics는 체크포인트에 저장된 원래
하이퍼파라미터를 복원하고 **호출 시 넘긴 인자를 무시한다.**
중단 원인이 하이퍼파라미터 자체(예: `workers`가 너무 커서 OOM)라면
체크포인트를 직접 고쳐야 한다.

```powershell
.venv-train\Scripts\python.exe scripts\patch_ckpt_workers.py `
  runs\detect\runs\tier2_v2\train\weights\last.pt --workers 4
```

`.pt.bak` 백업을 자동 생성한다.

### 4.4 출력 경로 주의

Ultralytics 전역 설정의 `runs_dir`과 `--project` 인자가 합쳐져
실제 출력이 `runs/detect/<project>/`처럼 한 단계 더 들어갈 수 있다.

```
%APPDATA%\Ultralytics\settings.json  →  "runs_dir": "runs"
--project runs/tier2_v2
→ 실제 출력: runs/detect/runs/tier2_v2/train/
```

`run_train_tier2.py`는 경로를 재구성하지 않고 `model.trainer.save_dir`에서
받아오므로 이 차이에 영향받지 않는다. 수동으로 파일을 찾을 때만 주의하면 된다.

---

## 5. 평가

### 5.1 스크립트

```powershell
.venv-train\Scripts\python.exe scripts\eval_tier2_vs_baseline.py `
  --model runs\detect\runs\tier2_v2\train\weights\best.pt `
  --data datasets\tier2_v2\tier2_v2.yaml `
  --device cpu
```

두 가지를 출력한다.

1. **클래스별 AP** — 평균을 깎아먹는 클래스가 어디인지
2. **동일 조건 사전학습 yolo11n 대비 비교** — 같은 이미지, 같은 클래스, 같은
   320px에서 COCO 118,000장으로 학습된 공식 모델이 몇 점을 내는지

2번이 핵심이다. **mAP 절대값만으로는 성능을 판단할 수 없다.**
같은 조건의 상한선과 비교해야 학습이 잘 됐는지 알 수 있다.

비교를 위해 같은 val 이미지에 COCO 80클래스 인덱스 레이블을 별도로 생성한다
(`datasets/tier2_base80/`). 사전학습 모델은 80클래스 출력이라 우리 클래스
인덱스로는 평가할 수 없기 때문이다.

> Ultralytics는 이미지 경로의 `/images/` 세그먼트를 `/labels/`로 치환해 레이블을
> 찾는다. 별도 데이터셋 트리를 만들 때 디렉터리 이름을 `images`/`labels`로
> 유지해야 한다. `images80` 같은 이름을 쓰면 레이블을 못 찾고
> `no labels found in detect set` 경고와 함께 mAP가 0으로 나온다.

### 5.2 v1 실측 결과 (16클래스, 에폭 61, imgsz 320)

전체: **mAP50 0.427 / mAP50-95 0.293**

사전학습 yolo11n(COCO 118k 학습)을 같은 val 1,150장·같은 320px로 평가한 값과의 비교:

| 클래스 | 우리 (21k장) | 사전학습 (118k장) | 차이 |
|---|---|---|---|
| cat | 0.621 | 0.634 | −0.013 |
| dog | 0.521 | 0.551 | −0.031 |
| pizza | 0.475 | 0.443 | +0.032 |
| dining table | 0.364 | 0.369 | −0.005 |
| sandwich | 0.335 | 0.286 | +0.049 |
| donut | 0.326 | 0.302 | +0.024 |
| hot dog | 0.301 | 0.240 | +0.060 |
| cake | 0.296 | 0.244 | +0.052 |
| cup | 0.272 | 0.319 | −0.047 |
| orange | 0.263 | 0.234 | +0.029 |
| banana | 0.197 | 0.171 | +0.026 |
| broccoli | 0.183 | 0.177 | +0.006 |
| carrot | 0.178 | 0.122 | +0.056 |
| bottle | 0.149 | 0.197 | −0.048 |
| apple | 0.114 | 0.106 | +0.008 |
| chair | 0.094 | 0.160 | **−0.067** |
| **평균** | **0.293** | 0.285 | **+0.008** |

**해석**

- 데이터 1/6로 사전학습 모델을 근소하게 앞선다. 학습 자체는 정상이다.
- 절대값이 낮은 이유는 **320px + 작은 객체**의 구조적 한계다. apple·bottle·carrot은
  10m 거리 320px에서 몇 픽셀에 불과하다. 데이터를 더 넣어도 해결되지 않는다.
- **chair만 사전학습 대비 크게 뒤진다(−0.067).** 균형 조정이 chair를 12,774장에서
  2,500장으로 깎은 직접적 대가다. v2에서 상한을 푼 근거가 이것이다.

### 5.3 성능을 더 올리는 레버

| 레버 | 기대 효과 | 비용 |
|---|---|---|
| chair/table 상한 해제 | chair AP 회복 | v2에 반영됨 |
| 음식 10종 → `food` 병합 | 음식 AP 대폭 상승 | v2에 반영됨 |
| 입력 320 → 416 | 작은 객체 AP에 가장 큰 레버 | i5-4200U 추론 부하 **1.7배** — 리소스 목표와 충돌 |
| yolo11n → yolo11s | mAP 상당폭 상승 | 파라미터 2.6M → 9.4M, i5-4200U에 부적합 |
| 매장 실촬 영상 파인튜닝 | 도메인 일치로 실효 성능 최대 | 라벨링 공수 |

COCO mAP는 참고 지표일 뿐이다. **최종 판단은 매장 실촬 영상 필드 테스트로 해야 한다.**
COCO의 chair는 심하게 가려지고 각도가 제각각이지만, 고정 카메라 매장 환경의
의자는 훨씬 쉬운 문제다. COCO AP 0.16이 현장에서 쓸 만할 수 있다.

---

## 6. 배포

학습이 끝나면 `run_train_tier2.py`가 ONNX 변환까지 자동으로 수행한다.

```
runs/detect/runs/tier2_v2/train/weights/best.pt
                                     ├─ best_fp32.onnx
                                     └─ best_int8.onnx
models/yolo11n_tier2_fp32.onnx   ← 자동 복사
models/yolo11n_tier2_int8.onnx   ← 자동 복사
```

배포 대상 i5-4200U에서는 INT8이 실질 추론 속도를 좌우한다.
자세한 내용은 [quantization.md](quantization.md)를 참고한다.

### INT8 캘리브레이션 데이터 — 조용히 망가지는 함정

`export(int8=True)`에 **`data=`를 반드시 넘겨야 한다.** 생략하면 Ultralytics가
기본값 `coco8.yaml`(이미지 **4장**짜리 샘플 데이터셋)로 캘리브레이션한다.

```
ONNX: collecting INT8 calibration images from 'data=coco8.yaml'
WARNING ONNX: >300 images recommended for INT8 calibration, found 4 images.
```

**경고만 뜨고 export는 성공한다.** 활성값 분포가 실제 입력과 전혀 달라
양자화 스케일이 어긋나고, 배포 모델의 정확도가 조용히 무너진다.
v1 첫 export가 실제로 이 상태였다.

정상 캘리브레이션은 val 전체를 돌기 때문에 시간이 확연히 다르다
(4장 6.8초 → 1,150장 304초). 소요 시간으로 정상 여부를 판별할 수 있다.

`run_train_tier2.py`는 `--data`를 그대로 캘리브레이션에 넘긴다.
기존 결과에서 ONNX만 다시 만들려면:

```powershell
.venv-train\Scripts\python.exe scripts\run_train_tier2.py `
  --export-only runs\detect\runs\tier2_v2\train `
  --data datasets\tier2_v2\tier2_v2.yaml
```

### 양자화 손실 실측 (v1, val 1,150장, imgsz 320)

| 모델 | mAP50 | mAP50-95 | 크기 |
|---|---|---|---|
| PyTorch FP32 (`best.pt`) | 0.4310 | 0.2956 | 15.9 MB |
| ONNX FP32 | 0.4304 | 0.2943 | 10.0 MB |
| **ONNX INT8** | **0.4182** | **0.2804** | **2.8 MB** |

INT8 손실은 mAP50 −1.2%p, mAP50-95 −1.4%p다. 모델 크기 3.6배 감소 대비
허용 범위이며, i5-4200U 배포에는 INT8을 쓴다.

**클래스 수 변경 시 확인할 것**: v1(16클래스) → v2(7클래스)로 바뀌면서
클래스 인덱스가 전부 달라진다. 파이프라인에서 Tier 2 클래스 인덱스를
하드코딩해 쓰는 곳이 있으면 반드시 함께 수정해야 한다.

v2 인덱스: `0=cat, 1=dog, 2=bottle, 3=cup, 4=food, 5=chair, 6=dining table`

---

## 7. 트러블슈팅

| 증상 | 원인 | 조치 |
|---|---|---|
| `no kernel image is available for execution on the device` | torch 빌드가 해당 GPU 아키텍처를 미포함 | GPU 세대에 맞는 CUDA 빌드로 재설치 (2.1절) |
| `torch.cuda.is_available()`는 True인데 학습이 죽음 | 위와 동일 | `preflight()`가 사전에 검출 |
| 학습 중 시스템 메모리 고갈 | Windows spawn 워커 (2.4절) | `--workers 4` 이하, 이미 학습 중이면 `patch_ckpt_workers.py` |
| `--resume` 시 인자가 무시됨 | Ultralytics가 체크포인트 설정을 복원 | `patch_ckpt_workers.py`로 체크포인트 직접 수정 |
| `no labels found in detect set`, mAP 0 | 데이터셋 디렉터리명이 `images`/`labels`가 아님 | 디렉터리 구조 수정 (5.1절) |
| `best.pt`를 찾을 수 없음 | `runs_dir` + `project` 중첩 | `model.trainer.save_dir` 사용 (4.4절) |
| INT8 모델 정확도가 이상하게 낮음 | 캘리브레이션에 기본값 `coco8.yaml`(4장) 사용 | `export`에 `data=` 전달 (6절) |
| 진행 상황 모니터링 중 RAM 고갈 | `tr -d '\r'`가 tqdm 진행바를 줄바꿈 없는 거대한 한 줄로 만들어 `grep`이 통째로 버퍼링 | 로그를 파일로 직접 쓰고, 읽을 때만 `tr '\r' '\n'`으로 줄을 끊을 것 |

---

## 8. 다른 머신에서 재현하기

```powershell
# 1) 환경 (GPU 세대에 맞는 CUDA 빌드 선택 — 2.1절)
py -3.13 -m venv .venv-train
.venv-train\Scripts\python.exe -m pip install --upgrade pip
.venv-train\Scripts\python.exe -m pip install torch torchvision --index-url https://download.pytorch.org/whl/cu126
.venv-train\Scripts\python.exe -m pip install ultralytics onnx onnxruntime onnxslim

# 2) COCO 어노테이션 (약 241MB) — datasets/tier2/annotations/ 에 압축 해제
#    http://images.cocodataset.org/annotations/annotations_trainval2017.zip

# 3) 데이터셋 구축 (약 33k장 다운로드)
.venv-train\Scripts\python.exe scripts\build_tier2_balanced.py --preset v2 --workers 24

# 4) 학습 (Ampere 이상이면 --amp --batch 64 권장)
.venv-train\Scripts\python.exe scripts\run_train_tier2.py `
  --data datasets\tier2_v2\tier2_v2.yaml --epochs 80 --batch 16 --workers 4

# 5) 평가
.venv-train\Scripts\python.exe scripts\eval_tier2_vs_baseline.py `
  --model runs\detect\runs\tier2_v2\train\weights\best.pt `
  --data datasets\tier2_v2\tier2_v2.yaml --device cpu
```

---

## 9. 스크립트 목록

| 파일 | 역할 |
|---|---|
| `scripts/build_tier2_balanced.py` | 데이터셋 구축 (ID 정정 + 클래스 병합 + 균형 조정) |
| `scripts/run_train_tier2.py` | 학습 + ONNX export (preflight, OOM 재시도, resume) |
| `scripts/eval_tier2_vs_baseline.py` | 클래스별 AP + 사전학습 모델 대비 비교 |
| `scripts/patch_ckpt_workers.py` | 체크포인트의 `train_args` 수정 |
| `scripts/train_tier2.py` | 구 스크립트. ID 버그는 수정했으나 균형 조정이 없다. 신규 작업에는 `build_tier2_balanced.py`를 쓸 것 |
