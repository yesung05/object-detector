# Tier 2 보강 데이터 학습 실행 기록

## 정리한 파일

- `datasets/pose_original_12000_interrupted_20260920`: 중단된 Pose 데이터 준비 산출물, 약 911MiB. 원본에서 재생성 가능.
- `datasets/tier2/annotations_trainval2017.zip`: 약 241MiB. 압축 해제된 주석은 보존, 필요 시 재다운로드 가능.

위 두 경로만 삭제했다. 삭제는 휴지통 이동이 아니다. 원본 영상, 기존 Tier 2/Pose 모델, 학습 결과 및 평가 자료는 보존했다. 실제 드라이브 여유 공간은 다른 작업의 영향을 받으므로 논리 파일 크기의 합계와 다를 수 있다.

## 보강 데이터

Open Images 전체 다운로드 대신, 기존 7클래스 의미와 맞는 COCO train2017 추가 이미지를 먼저 사용했다. 기존 분할 이미지 ID를 모두 제외했고, COCO 메타데이터에 CC BY로 기록된 이미지로 제한했다. 출처 URL/Flickr URL/라이선스/해시를 audit.json에 보존했다. 개별 사진의 사용 조건은 해당 원 출처에서도 확인해야 한다.

- 다운로드: **604장, 93.25MiB**. 공식 공개 COCO S3 버킷의 HTTPS 경로 사용. 인증서 검증 유지.
- 추가 학습: **524장** = 음식 중심 및 컵/병 포함 양성 204장 + 대상 7종이 없다고 주석된 배경 320장.
- 별도 추가 배경 평가: **80장**, 학습 제외.
- 기존 학습 23,203장 → **총 23,727장**. 기존 이미지를 복사하지 않고 경로 목록으로 참조.
- 기존 val **3,315장**, test **6,629장** 유지.
- 신규 이미지와 기존 val/test의 SHA256 중복 **0장**. 기존 전체 데이터의 시각적 중복까지 검증한 것은 아니다.
- 모든 다운로드 이미지는 디코딩 및 원본 크기 검사 통과. 배경 일부만 직접 육안 확인했으며 전체를 사람이 재검수한 것은 아니다.
- 촬영 영상은 박스 정답이 준비되지 않아 이번 학습에서 제외했다. 증강은 학습 시 적용하며 이미지를 디스크에 대량 복제하지 않는다.

## 학습 조건

- 시작: `runs/detect/runs/tier2_v3_fresh/train/weights/best.pt`
- 기존 7클래스 유지. `dining table`을 사무용 책상까지 임의로 확장하지 않았다.
- 320 입력, batch 32, workers 2, RAM cache, AMP.
- backbone 앞 10개 모듈 고정. AdamW 0.00003, warmup 1 epoch.
- 검증 mAP50–95가 3 epoch 연속 개선되지 않으면 LR 절반 감소, 최저 0.000001.
- Early Stopping patience 10, 최대 60 epoch.
- 약한 HSV·평행 이동·크기 변화·좌우 반전. mosaic/mixup/copy-paste 미사용.
- 시작 가중치 파라미터 일치 검사 및 매 epoch 학습률 기록.

## 자동 후처리

학습 종료 후 원본과 새 best.pt를 같은 val/test에서 비교하고, 별도 배경 80장에서 confidence 0.20의 검출 이미지 수와 박스 수를 비교한다. COCO 주석 기반 배경 수치는 실제 매장 오탐률이 아니며 누락 주석의 영향이 있을 수 있다. 클래스별 정밀도/재현율도 저장한다. PyTorch 평가이며 기존 INT8와 C 앱 후처리 평가를 대신하지 않는다.

FP32 ONNX를 별도 내보내고 그래프 검사와 PyTorch 수치 비교 후 REPORT.md를 작성한다. 기존 배포 파일은 자동 교체하지 않는다. 앱의 16/7클래스 매핑 문제는 이번 학습에서 수정하지 않았다.

- [실행 로그](../runs/tier2-supplement-launch/stdout.log)
- [데이터 감사](../datasets/tier2_supplement_20260920/audit.json)
- 학습 결과 디렉터리: `runs/tier2-supplement/train/`
- 완료 후 보고서: `runs/tier2-supplement/train/REPORT.md`
- [데이터 준비 스크립트](../scripts/prepare_tier2_supplement.py)
- [학습·평가 스크립트](../scripts/finetune_tier2_supplement.py)

이 문서는 실행 조건 기록이며 성능 향상을 선언하는 결과 보고서가 아니다.
