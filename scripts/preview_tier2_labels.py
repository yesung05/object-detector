"""
라벨 육안 확인용 시트 생성 (개발 PC 전용)

자동(교사) 라벨은 학습 데이터를 직접 오염시키므로 숫자만 믿지 않고 눈으로 확인한다.
cat/dog/bottle/cup/food 중 하나라도 가진 HomeObjects 추가분(ho_*)에서 무작위 N장을 골라
박스·클래스명을 그려 한 장의 JPG 시트로 묶는다. 시드를 고정해 같은 시트를 다시 만들 수 있다.

사용:
  .venv-train\\Scripts\\python.exe -I scripts\\preview_tier2_labels.py --out runs\\tier2-v4\\teacher_preview.jpg
"""

import argparse
import random
from pathlib import Path

import cv2

ROOT = Path(__file__).resolve().parents[1]
NAMES = ["cat", "dog", "bottle", "cup", "food", "chair", "dining table"]
COLORS = [(0, 128, 255), (255, 0, 255), (0, 200, 0), (0, 0, 255), (0, 215, 255), (255, 128, 0), (200, 200, 0)]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--data", default="datasets/tier2_v4")
    p.add_argument("--n", type=int, default=12)
    p.add_argument("--cols", type=int, default=4)
    p.add_argument("--seed", type=int, default=0)
    p.add_argument("--out", default="runs/tier2-v4/teacher_preview.jpg")
    p.add_argument("--only-teacher", action="store_true", default=True)
    a = p.parse_args()
    root = ROOT / a.data
    cands = []
    for lab in (root / "labels/train").glob("ho_*.txt"):
        cls = {int(l.split()[0]) for l in lab.read_text().splitlines() if l.strip()}
        if cls & {0, 1, 2, 3, 4}:
            cands.append(lab)
    random.Random(a.seed).shuffle(cands)
    tiles = []
    for lab in cands[: a.n]:
        img = next((root / "images/train").glob(lab.stem + ".*"))
        im = cv2.imread(str(img))
        h, w = im.shape[:2]
        for line in lab.read_text().splitlines():
            c, cx, cy, bw, bh = line.split()[:5]
            c = int(c)
            x1, y1 = int((float(cx) - float(bw) / 2) * w), int((float(cy) - float(bh) / 2) * h)
            x2, y2 = int((float(cx) + float(bw) / 2) * w), int((float(cy) + float(bh) / 2) * h)
            cv2.rectangle(im, (x1, y1), (x2, y2), COLORS[c], 3)
            cv2.putText(im, NAMES[c], (x1, max(y1 - 6, 14)), cv2.FONT_HERSHEY_SIMPLEX, 0.9, COLORS[c], 2)
        tiles.append(cv2.resize(im, (480, 360)))
    while len(tiles) % a.cols:
        tiles.append(tiles[0] * 0)
    rows = [cv2.hconcat(tiles[i:i + a.cols]) for i in range(0, len(tiles), a.cols)]
    out = ROOT / a.out
    out.parent.mkdir(parents=True, exist_ok=True)
    cv2.imwrite(str(out), cv2.vconcat(rows))
    print(f"{len(cands)}장 중 {min(a.n, len(cands))}장 시트 저장: {out}")


if __name__ == "__main__":
    main()
