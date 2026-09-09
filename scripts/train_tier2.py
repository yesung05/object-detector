"""
YOLO11n Tier 2 파인튜닝 스크립트 — 16클래스 COCO 서브셋

coco.yaml 전체 다운로드(18GB) 대신, 필요한 16클래스 이미지만 선별 다운로드합니다.

흐름:
  1. COCO 어노테이션 ZIP 다운로드 (~241MB)
  2. 16개 카테고리 포함 이미지 ID 추출
  3. 이미지 병렬 다운로드
  4. YOLO 형식 레이블 변환
  5. tier2_data.yaml 생성 후 학습

출력:
  runs/tier2/train/weights/best.pt
  runs/tier2/train/weights/best_int8.onnx
  runs/tier2/train/weights/best_fp32.onnx
"""

import argparse
import json
import os
import sys
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

# 우리가 필요한 COCO 원본 카테고리 ID (1-indexed)
COCO_CLASSES = [15, 16, 39, 41, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 60]

# COCO 원본 ID → 우리 리매핑 ID (0-15)
COCO_ID_TO_OUR = {cid: i for i, cid in enumerate(COCO_CLASSES)}

CLASS_NAMES = [
    "cat", "dog", "bottle", "cup",
    "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza",
    "donut", "cake", "chair", "dining table",
]

# 320×320: 3-5m 거리 컵/병 감지 가능한 최소 해상도
IMGSZ = 320

ANNOTATIONS_URL = "http://images.cocodataset.org/annotations/annotations_trainval2017.zip"
TRAIN_IMG_BASE  = "http://images.cocodataset.org/train2017/"
VAL_IMG_BASE    = "http://images.cocodataset.org/val2017/"


def parse_args():
    p = argparse.ArgumentParser(description="YOLO11n Tier 2 파인튜닝 (16클래스 COCO 서브셋)")
    p.add_argument("--epochs",     type=int,  default=50,           help="학습 에폭 수")
    p.add_argument("--batch",      type=int,  default=32,           help="배치 크기")
    p.add_argument("--device",     type=str,  default="0",          help="GPU 번호 또는 cpu")
    p.add_argument("--patience",   type=int,  default=10,           help="조기종료 patience")
    p.add_argument("--weights",    type=str,  default="yolo11n.pt", help="시작 가중치")
    p.add_argument("--project",    type=str,  default="runs/tier2", help="출력 디렉터리")
    p.add_argument("--data-dir",   type=str,  default="datasets/tier2", help="데이터셋 저장 위치")
    p.add_argument("--workers",    type=int,  default=8,            help="이미지 다운로드 병렬 수")
    p.add_argument("--fp32-only",  action="store_true",             help="INT8 export 없이 FP32만")
    p.add_argument("--skip-download", action="store_true",          help="다운로드 건너뛰기(이미 존재 시)")
    return p.parse_args()


def download_file(url, dest: Path, desc=""):
    """파일을 다운로드합니다. 이미 존재하면 건너뜁니다."""
    if dest.exists():
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".tmp")
    def progress(count, block, total):
        if total > 0:
            pct = count * block * 100 // total
            print(f"\r  {desc}: {pct}%", end="", flush=True)
    urllib.request.urlretrieve(url, tmp, reporthook=progress)
    tmp.rename(dest)
    print()


def download_image(args):
    url, dest = args
    if dest.exists():
        return True
    try:
        dest.parent.mkdir(parents=True, exist_ok=True)
        tmp = dest.with_suffix(".tmp")
        urllib.request.urlretrieve(url, tmp)
        tmp.rename(dest)
        return True
    except Exception:
        return False


def prepare_dataset(data_dir: Path, workers: int):
    """COCO 어노테이션 다운로드 → 필터링 → 이미지 다운로드 → YOLO 레이블 변환"""
    ann_zip = data_dir / "annotations_trainval2017.zip"
    ann_dir = data_dir / "annotations"

    # 1. 어노테이션 다운로드 (~241MB)
    if not ann_zip.exists():
        print("COCO 어노테이션 다운로드 중 (~241MB)...")
        download_file(ANNOTATIONS_URL, ann_zip, "annotations")
    if not ann_dir.exists():
        print("압축 해제 중...")
        with zipfile.ZipFile(ann_zip) as z:
            z.extractall(data_dir)

    results = {}
    for split, json_name, img_base in [
        ("train", "instances_train2017.json", TRAIN_IMG_BASE),
        ("val",   "instances_val2017.json",   VAL_IMG_BASE),
    ]:
        print(f"\n[{split}] 어노테이션 파싱 중...")
        with open(ann_dir / json_name, encoding="utf-8") as f:
            coco = json.load(f)

        # 2. 16클래스 포함 이미지 ID 수집
        img_ids = set()
        ann_by_img: dict[int, list] = {}
        for ann in coco["annotations"]:
            if ann["category_id"] in COCO_ID_TO_OUR:
                img_ids.add(ann["image_id"])
                ann_by_img.setdefault(ann["image_id"], []).append(ann)

        img_meta = {img["id"]: img for img in coco["images"] if img["id"] in img_ids}
        print(f"  대상 이미지: {len(img_meta)}장 (전체 {len(coco['images'])}장 중)")

        # 3. 이미지 병렬 다운로드
        img_dir = data_dir / "images" / split
        img_dir.mkdir(parents=True, exist_ok=True)
        tasks = [
            (img_base + meta["file_name"], img_dir / meta["file_name"])
            for meta in img_meta.values()
        ]
        need = [(url, dest) for url, dest in tasks if not dest.exists()]
        if need:
            print(f"  이미지 다운로드: {len(need)}장 남음 (workers={workers})")
            done = 0
            with ThreadPoolExecutor(max_workers=workers) as ex:
                futs = {ex.submit(download_image, t): t for t in need}
                for fut in as_completed(futs):
                    done += 1
                    if done % 100 == 0 or done == len(need):
                        print(f"\r  {done}/{len(need)}", end="", flush=True)
            print()
        else:
            print("  이미지 이미 존재, 건너뜀")

        # 4. YOLO 형식 레이블 변환
        label_dir = data_dir / "labels" / split
        label_dir.mkdir(parents=True, exist_ok=True)
        converted = 0
        for img_id, meta in img_meta.items():
            label_path = label_dir / Path(meta["file_name"]).with_suffix(".txt").name
            if label_path.exists():
                continue
            W, H = meta["width"], meta["height"]
            lines = []
            for ann in ann_by_img.get(img_id, []):
                cls = COCO_ID_TO_OUR[ann["category_id"]]
                x, y, w, h = ann["bbox"]
                cx = (x + w / 2) / W
                cy = (y + h / 2) / H
                nw = w / W
                nh = h / H
                lines.append(f"{cls} {cx:.6f} {cy:.6f} {nw:.6f} {nh:.6f}")
            label_path.write_text("\n".join(lines))
            converted += 1
        if converted:
            print(f"  레이블 변환: {converted}개")

        results[split] = str((data_dir / "images" / split).resolve())

    return results


def write_yaml(data_dir: Path, split_paths: dict) -> Path:
    yaml_path = data_dir / "tier2_data.yaml"
    lines = [
        f"train: {split_paths['train']}",
        f"val:   {split_paths['val']}",
        f"nc: {len(CLASS_NAMES)}",
        f"names: {CLASS_NAMES}",
    ]
    yaml_path.write_text("\n".join(lines))
    return yaml_path


def main():
    args = parse_args()

    try:
        from ultralytics import YOLO
    except ImportError:
        sys.exit("ultralytics 패키지가 필요합니다: pip install ultralytics")

    data_dir = Path(args.data_dir)

    if not args.skip_download:
        split_paths = prepare_dataset(data_dir, args.workers)
    else:
        split_paths = {
            "train": str((data_dir / "images" / "train").resolve()),
            "val":   str((data_dir / "images" / "val").resolve()),
        }

    yaml_path = write_yaml(data_dir, split_paths)
    print(f"\nYAML: {yaml_path}")

    print("\n학습 시작...")
    model = YOLO(args.weights)
    model.train(
        data=str(yaml_path),
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
        sys.exit(f"best.pt를 찾을 수 없습니다: {best_pt}")

    best = YOLO(str(best_pt))

    best.export(format="onnx", imgsz=IMGSZ, simplify=True)
    print(f"\nFP32 ONNX: {best_pt.with_suffix('.onnx')}")

    if not args.fp32_only:
        best.export(format="onnx", imgsz=IMGSZ, int8=True, simplify=True)
        int8_path = best_pt.parent / "best_int8.onnx"
        print(f"INT8 ONNX: {int8_path}")
        print(f"\n배포: cp {int8_path} models/yolo11n_tier2_int8.onnx")


if __name__ == "__main__":
    main()
