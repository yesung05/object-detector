"""
Ultralytics 체크포인트의 저장된 학습 인자를 수정합니다.

resume=True로 재개하면 Ultralytics는 체크포인트 안의 train_args를 그대로 복원하고
호출 시 넘긴 인자를 무시합니다. 중단 원인이 하이퍼파라미터 자체(여기서는 workers)일 때
처음부터 다시 학습하지 않으려면 체크포인트를 직접 고치는 수밖에 없습니다.

workers를 낮춰야 하는 이유:
Windows는 dataloader 워커를 fork가 아닌 spawn으로 띄웁니다. 워커마다 torch를 새로
import하므로 프로세스당 약 500MB가 듭니다. train/val 로더에 workers=8이면 실측 19개
프로세스가 떠 9GB를 점유했고, 16GB RAM 환경에서 시스템이 학습을 강제 종료했습니다.
GTX 1050이 병목이라 워커를 줄여도 학습 속도 손해는 거의 없습니다.
"""

import argparse
import shutil
from pathlib import Path

import torch


def main():
    p = argparse.ArgumentParser(description="체크포인트의 train_args 수정")
    p.add_argument("ckpt", help="last.pt 경로")
    p.add_argument("--workers", type=int, default=4)
    p.add_argument("--no-backup", action="store_true")
    args = p.parse_args()

    path = Path(args.ckpt)
    if not args.no_backup:
        backup = path.with_suffix(".pt.bak")
        if not backup.exists():
            shutil.copy2(path, backup)
            print(f"백업: {backup}")

    # weights_only=False: Ultralytics 체크포인트는 텐서 외에 train_args dict와
    # 모델 객체를 담고 있어 weights_only=True로는 읽히지 않습니다.
    ckpt = torch.load(path, map_location="cpu", weights_only=False)
    ta = ckpt.get("train_args")
    if ta is None:
        raise SystemExit("train_args가 없는 체크포인트입니다")

    print(f"epoch={ckpt.get('epoch')}  workers: {ta.get('workers')} -> {args.workers}")
    ta["workers"] = args.workers
    torch.save(ckpt, path)
    print(f"저장 완료: {path}")


if __name__ == "__main__":
    main()
