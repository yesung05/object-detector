"""
Tier 2 학습 실행 스크립트 — 균형 데이터셋 + GTX 1050(Pascal) CUDA

이 개발 PC의 GPU는 GTX 1050(sm_61, VRAM 2GB)입니다. 두 가지 제약이 학습 설정을 지배합니다.

1. amp=False
   Pascal 소비자형 GPU(GP107)는 FP16 연산 처리율이 FP32의 1/64입니다.
   AMP를 켜면 메모리는 줄지만 연산이 오히려 느려집니다. 텐서코어가 없는 세대라
   AMP의 이득이 없습니다. Ampere 이상 GPU로 옮기면 amp=True가 정답입니다.

2. batch 자동 축소
   VRAM 2GB 중 데스크톱 컴포지터가 이미 ~500MB를 점유합니다. 가용분이
   1.5GB 남짓이라 OOM 여지가 큽니다. AutoBatch(batch=-1)는 측정 시점의
   여유 메모리에 맞춰 과대 추정하는 경향이 있어, 고정 배치에서 시작해
   OOM 시 절반으로 낮추는 재시도를 씁니다.

주의: torch는 반드시 Pascal(sm_61)을 포함하는 빌드여야 합니다.
cu128 이상 빌드는 sm_61을 드롭해 'no kernel image is available'로 실패합니다.
  pip install torch==2.7.1 torchvision==0.22.1 --index-url https://download.pytorch.org/whl/cu126
"""

import argparse
import pathlib
import shutil
import sys

IMGSZ = 320  # 320×320: 3-5m 거리 컵/병 감지 가능한 최소 해상도


def parse_args():
    p = argparse.ArgumentParser(description="Tier 2 학습 (균형 데이터셋)")
    p.add_argument("--data", default="datasets/tier2_bal/tier2_bal.yaml")
    p.add_argument("--weights", default="yolo11n.pt")
    p.add_argument("--epochs", type=int, default=80)
    p.add_argument("--batch", type=int, default=16, help="OOM 시 자동으로 절반씩 낮춥니다")
    p.add_argument("--device", default="0", help="GPU 번호 또는 cpu")
    p.add_argument("--patience", type=int, default=15)
    p.add_argument("--workers", type=int, default=8, help="데이터로더 워커 (i7-12700 기준)")
    p.add_argument("--project", default="runs/tier2_bal")
    p.add_argument("--amp", action="store_true", help="Ampere 이상에서만 켜세요")
    p.add_argument("--optimizer", default="auto")
    p.add_argument("--lr0", type=float, default=0.01)
    p.add_argument("--lrf", type=float, default=0.01)
    p.add_argument("--warmup-epochs", type=float, default=3.0)
    p.add_argument("--warmup-bias-lr", type=float, default=0.1)
    p.add_argument("--weight-decay", type=float, default=0.0005)
    p.add_argument("--no-export", action="store_true")
    p.add_argument("--no-test", action="store_true", help="학습 후 보류 test split 평가를 생략합니다")
    p.add_argument("--resume", metavar="LAST_PT",
                   help="중단된 학습을 last.pt에서 이어서 진행합니다")
    p.add_argument("--export-only", metavar="SAVE_DIR",
                   help="학습 없이 기존 결과 디렉터리에서 ONNX만 다시 만듭니다")
    return p.parse_args()


def preflight(device: str):
    """학습을 몇 시간 돌린 뒤가 아니라 시작 전에 GPU 사용 가능 여부를 확정합니다."""
    import torch

    if device == "cpu":
        print("[preflight] CPU 모드")
        return
    if not torch.cuda.is_available():
        sys.exit("[preflight] CUDA를 쓸 수 없습니다. --device cpu 로 실행하거나 드라이버를 확인하세요.")
    name = torch.cuda.get_device_name(0)
    cap = torch.cuda.get_device_capability(0)
    total = torch.cuda.get_device_properties(0).total_memory / 1024**3
    print(f"[preflight] {name} sm_{cap[0]}{cap[1]} VRAM {total:.1f}GB torch {torch.__version__}")
    try:
        # 커널을 실제로 하나 띄워봅니다. is_available()은 아키텍처 미지원을 걸러내지 못합니다.
        a = torch.randn(256, 256, device="cuda")
        (a @ a).sum().item()
        torch.cuda.synchronize()
    except Exception as e:
        sys.exit(
            f"[preflight] CUDA 커널 실행 실패: {e}\n"
            f"  이 torch 빌드가 sm_{cap[0]}{cap[1]}을 포함하지 않습니다. cu126 빌드로 재설치하세요."
        )
    print("[preflight] CUDA 커널 실행 확인")


def train(args):
    from ultralytics import YOLO

    if args.resume:
        # resume=True일 때 Ultralytics는 체크포인트에 저장된 원래 하이퍼파라미터를
        # 그대로 복원합니다. 여기서 batch/epochs를 다시 넘기면 무시되거나 충돌하므로
        # 인자 없이 resume만 지정합니다.
        model = YOLO(args.resume)
        model.train(resume=True)
        save_dir = pathlib.Path(getattr(model.trainer, "save_dir", args.project))
        return args.batch, save_dir

    batch = args.batch
    while True:
        model = YOLO(args.weights)
        try:
            res = model.train(
                data=args.data,
                epochs=args.epochs,
                imgsz=IMGSZ,
                batch=batch,
                device=args.device,
                patience=args.patience,
                workers=args.workers,
                project=args.project,
                name="train",
                exist_ok=True,
                amp=args.amp,
                optimizer=args.optimizer,
                lr0=args.lr0,
                lrf=args.lrf,
                warmup_epochs=args.warmup_epochs,
                warmup_bias_lr=args.warmup_bias_lr,
                weight_decay=args.weight_decay,
                plots=True,
            )
            # save_dir을 project 인자로 재구성하지 않고 trainer에서 받아옵니다.
            # Ultralytics 전역 설정의 runs_dir와 project가 합쳐지면서 실제 출력이
            # runs/detect/<project>/ 처럼 한 단계 더 들어가는 경우가 있기 때문입니다.
            save_dir = pathlib.Path(getattr(model.trainer, "save_dir", args.project))
            return batch, save_dir
        except RuntimeError as e:
            if "out of memory" not in str(e).lower() or batch <= 2:
                raise
            import torch

            torch.cuda.empty_cache()
            batch //= 2
            print(f"\n[oom] VRAM 부족 — batch를 {batch}로 낮춰 재시도합니다\n")


def export(save_dir: pathlib.Path, data: str):
    from ultralytics import YOLO

    best_pt = save_dir / "weights" / "best.pt"
    if not best_pt.exists():
        sys.exit(f"best.pt를 찾을 수 없습니다: {best_pt}")

    best = YOLO(str(best_pt))
    best.export(format="onnx", imgsz=IMGSZ, simplify=True)
    fp32 = best_pt.with_suffix(".onnx")
    fp32_named = best_pt.parent / "best_fp32.onnx"
    if fp32.exists():
        fp32.replace(fp32_named)

    # INT8은 배포 대상 i5-4200U에서 실질 추론 속도를 좌우하므로 항상 함께 만듭니다.
    #
    # data= 를 반드시 넘겨야 합니다. 생략하면 Ultralytics가 기본값 coco8.yaml로
    # 캘리브레이션하는데 이는 이미지 4장짜리 샘플 데이터셋입니다. 활성값 분포가
    # 실제 입력과 전혀 달라 양자화 스케일이 어긋나고, 배포 모델의 정확도가
    # 조용히 무너집니다(경고만 뜨고 export는 성공합니다).
    best.export(format="onnx", imgsz=IMGSZ, int8=True, simplify=True, data=data)

    models = pathlib.Path("models")
    models.mkdir(exist_ok=True)
    for f in best_pt.parent.glob("best_*.onnx"):
        dst = models / f"yolo11n_tier2_{f.stem.rsplit('_', 1)[-1]}.onnx"
        shutil.copy(f, dst)
        print(f"복사: {f.name} -> {dst}")


def evaluate_test(save_dir: pathlib.Path, args):
    """Evaluate best.pt once on the held-out test split after model selection."""
    from ultralytics import YOLO

    best_pt = save_dir / "weights" / "best.pt"
    if not best_pt.exists():
        sys.exit(f"test 평가용 best.pt를 찾을 수 없습니다: {best_pt}")
    print("\n[test] 보류 test split 최종 평가 시작")
    model = YOLO(str(best_pt))
    metrics = model.val(
        data=args.data,
        split="test",
        imgsz=IMGSZ,
        batch=args.batch,
        device=args.device,
        workers=args.workers,
        project=str(save_dir),
        name="test",
        plots=True,
    )
    print(
        f"[test] mAP50={metrics.box.map50:.6f} "
        f"mAP50-95={metrics.box.map:.6f}"
    )


def main():
    args = parse_args()
    if not pathlib.Path(args.data).exists():
        sys.exit(f"데이터 YAML이 없습니다: {args.data}\n  먼저 scripts/build_tier2_balanced.py 를 실행하세요.")

    if args.export_only:
        export(pathlib.Path(args.export_only), args.data)
        print("EXPORT_DONE")
        return

    preflight(args.device)
    used_batch, save_dir = train(args)
    print(f"\n학습 완료 (batch={used_batch}) — 출력: {save_dir}")
    args.batch = used_batch
    if not args.no_test:
        evaluate_test(save_dir, args)
    if not args.no_export:
        export(save_dir, args.data)
    print("EXPORT_DONE")


if __name__ == "__main__":
    main()
