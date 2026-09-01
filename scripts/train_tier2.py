"""
YOLO11n Tier 2 파인튜닝 스크립트 — 16클래스 COCO 서브셋

대상 기기(i5-4200U)에서 직접 학습은 불가능하므로 GPU 서버(또는 Colab)에서 실행합니다.
학습 결과 ONNX 파일을 배포 기기의 models/ 디렉터리에 배치하고
--obj-model 플래그로 경로를 지정합니다.

학습 후 출력:
  runs/tier2/train/weights/best.pt
  runs/tier2/train/weights/best_int8.onnx  (INT8 quantized, ~7MB)
  runs/tier2/train/weights/best_fp32.onnx  (FP32 fallback, ~28MB)

INT8 정확도 저하 시 best_fp32.onnx를 --obj-model로 대신 지정하세요.
"""

import argparse
import sys
from pathlib import Path

# Ultralytics YOLO11n 사전학습 가중치에서 파인튜닝합니다.
# COCO 80클래스 전체 대신 아래 16클래스만 학습하면 출력 텐서가 [1,20,N]으로 축소됩니다.
# Ultralytics의 classes= 파라미터가 자동으로 원본 COCO ID를 0-15로 리매핑합니다.
COCO_CLASSES = [
    15,  # cat         → 리매핑 0  (OBJ_CAT)
    16,  # dog         → 리매핑 1  (OBJ_DOG)
    39,  # bottle      → 리매핑 2  (OBJ_BOTTLE)
    41,  # cup         → 리매핑 3  (OBJ_CUP)
    46,  # banana      → 리매핑 4  (OBJ_FOOD_FIRST)
    47,  # apple       → 리매핑 5
    48,  # sandwich    → 리매핑 6
    49,  # orange      → 리매핑 7
    50,  # broccoli    → 리매핑 8
    51,  # carrot      → 리매핑 9
    52,  # hot dog     → 리매핑 10
    53,  # pizza       → 리매핑 11
    54,  # donut       → 리매핑 12
    55,  # cake        → 리매핑 13 (OBJ_FOOD_LAST)
    56,  # chair       → 리매핑 14 (OBJ_CHAIR)
    60,  # dining table→ 리매핑 15 (OBJ_DININGTABLE)
]

# 320×320이 최적 해상도입니다.
# 256×256으로 줄이면 추론이 ~25ms로 빨라지지만 3-5m 거리의 컵/병 감지율이 떨어집니다.
# 416×416은 불필요한 오버스펙이며 CPU 부하를 70ms 수준으로 올립니다.
IMGSZ = 320


def parse_args():
    p = argparse.ArgumentParser(description="YOLO11n Tier 2 파인튜닝 (16클래스 COCO 서브셋)")
    p.add_argument("--epochs",   type=int,   default=50,       help="학습 에폭 수 (기본 50)")
    p.add_argument("--batch",    type=int,   default=32,       help="배치 크기 (기본 32)")
    p.add_argument("--device",   type=str,   default="0",      help="학습 장치 (GPU 번호 또는 cpu)")
    p.add_argument("--patience", type=int,   default=10,       help="조기종료 patience (기본 10)")
    p.add_argument("--data",     type=str,   default="coco.yaml",
                   help="Ultralytics COCO YAML 경로 (기본 coco.yaml)")
    p.add_argument("--weights",  type=str,   default="yolo11n.pt",
                   help="시작 가중치 (기본 yolo11n.pt; COCO 사전학습)")
    p.add_argument("--project",  type=str,   default="runs/tier2", help="출력 프로젝트 디렉터리")
    p.add_argument("--fp32-only", action="store_true",
                   help="INT8 export 없이 FP32 ONNX만 export합니다")
    return p.parse_args()


def main():
    args = parse_args()

    try:
        from ultralytics import YOLO
    except ImportError:
        sys.exit("ultralytics 패키지가 필요합니다: pip install ultralytics")

    # 파인튜닝: COCO 전체에서 16클래스만 필터해 학습합니다.
    # Ultralytics는 classes= 리스트를 받아 내부적으로 COCO ID를 0-15로 재매핑합니다.
    # 이 재매핑 결과가 C 코드의 OBJ_* 상수(rules.h)와 일치해야 합니다.
    model = YOLO(args.weights)
    model.train(
        data=args.data,
        classes=COCO_CLASSES,
        epochs=args.epochs,
        imgsz=IMGSZ,
        batch=args.batch,
        device=args.device,
        patience=args.patience,
        project=args.project,
        name="train",
        exist_ok=True,
    )

    best_pt = Path(args.project) / "train" / "weights" / "best.pt"
    if not best_pt.exists():
        sys.exit(f"학습 완료 후 best.pt를 찾을 수 없습니다: {best_pt}")

    best = YOLO(str(best_pt))

    # FP32 ONNX — 정확도 기준선. INT8 오탐 발생 시 이쪽을 --obj-model로 지정합니다.
    best.export(
        format="onnx",
        imgsz=IMGSZ,
        simplify=True,
    )
    print(f"FP32 ONNX: {best_pt.with_suffix('.onnx')}")

    if not args.fp32_only:
        # INT8 quantized ONNX — Haswell(i5-4200U)에서 FP32 대비 약 10-20% 빠릅니다.
        # VNNI 미지원으로 30% 가속은 기대하기 어렵습니다.
        # 모델 크기는 ~7MB (FP32 ~28MB).
        best.export(
            format="onnx",
            imgsz=IMGSZ,
            int8=True,
            simplify=True,
            # INT8 calibration은 COCO val 데이터셋을 사용합니다.
        )
        int8_path = best_pt.parent / "best_int8.onnx"
        print(f"INT8 ONNX: {int8_path}")
        print()
        print("배포 방법:")
        print(f"  cp {int8_path} models/yolo11n_tier2_int8.onnx")
        print("  ./yolo11-person --input rtsp://... \\")
        print("    --obj-model models/yolo11n_tier2_int8.onnx \\")
        print("    --detect-every 6 --detect-every-obj 90")
    else:
        print()
        print("배포 방법 (FP32):")
        fp32_path = best_pt.with_suffix(".onnx")
        print(f"  cp {fp32_path} models/yolo11n_tier2_fp32.onnx")
        print("  ./yolo11-person --input rtsp://... \\")
        print("    --obj-model models/yolo11n_tier2_fp32.onnx \\")
        print("    --detect-every 6 --detect-every-obj 90")


if __name__ == "__main__":
    main()
