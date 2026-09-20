"""
Tier 2 모델 성능 진단 — 클래스별 AP + 사전학습 yolo11n 대비 비교

"mAP50 0.42가 낮은가"는 절대값으로는 답할 수 없습니다. 비교 기준이 두 개 필요합니다.

  1. 클래스별 AP — 평균을 깎아먹는 클래스가 어디인지. 320px에서 작은 객체
     (apple/orange/carrot/donut)는 구조적으로 낮게 나옵니다.
  2. 동일 조건 사전학습 yolo11n — 같은 이미지, 같은 16클래스, 같은 320px에서
     COCO 전체(118k장)로 학습된 모델이 몇 점을 내는지. 이게 실질 상한선입니다.

2번을 위해 같은 val 이미지에 대해 COCO 80클래스 인덱스 레이블을 따로 만듭니다.
사전학습 모델은 80클래스 출력이라 우리 16클래스 인덱스로는 평가할 수 없기 때문입니다.

GPU는 학습이 점유 중이라 기본 device는 cpu입니다.
"""

import argparse
import json
from pathlib import Path

CLASS_NAMES = [
    "cat", "dog", "bottle", "cup",
    "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza",
    "donut", "cake", "chair", "dining table",
]


def parse_args():
    p = argparse.ArgumentParser(description="Tier 2 성능 진단")
    p.add_argument("--model", default="runs/detect/runs/tier2_bal/train/weights/best.pt")
    p.add_argument("--data", default="datasets/tier2_bal/tier2_bal.yaml")
    p.add_argument("--src", default="datasets/tier2", help="COCO 어노테이션 위치")
    p.add_argument("--bal", default="datasets/tier2_bal")
    p.add_argument("--imgsz", type=int, default=320)
    p.add_argument("--device", default="cpu", help="학습이 GPU를 쓰는 동안은 cpu")
    p.add_argument("--batch", type=int, default=8)
    p.add_argument("--skip-baseline", action="store_true")
    return p.parse_args()


def per_class_table(metrics, names, title):
    print(f"\n===== {title} =====")
    print(f"{'class':<14}{'AP50':>8}{'AP50-95':>10}")
    box = metrics.box
    rows = []
    for i, c in enumerate(box.ap_class_index):
        rows.append((names[int(c)], box.ap50[i], box.ap[i]))
    for nm, ap50, ap in sorted(rows, key=lambda r: r[2]):
        print(f"{nm:<14}{ap50:>8.3f}{ap:>10.3f}")
    print(f"{'mAP':<14}{box.map50:>8.3f}{box.map:>10.3f}")
    return {nm: (ap50, ap) for nm, ap50, ap in rows}


def build_baseline_labels(src: Path, bal: Path):
    """같은 val 이미지에 COCO 80클래스 인덱스 레이블을 생성합니다.

    사전학습 yolo11n의 출력 인덱스 체계에 맞춰야 평가가 성립합니다.
    91-카테고리 ID가 아니라 80클래스 연속 인덱스라는 점이 핵심입니다.
    """
    with open(src / "annotations" / "instances_val2017.json", encoding="utf-8") as f:
        coco = json.load(f)

    # 91-카테고리 ID를 정렬 순서대로 0..79에 대응시킨 것이 YOLO의 80클래스 인덱스입니다.
    cat_ids = sorted(c["id"] for c in coco["categories"])
    id_to_yolo = {cid: i for i, cid in enumerate(cat_ids)}
    id_to_name = {c["id"]: c["name"] for c in coco["categories"]}
    yolo_names = [id_to_name[cid] for cid in cat_ids]

    stems = {p.stem for p in (bal / "images" / "val").glob("*.jpg")}
    meta = {im["id"]: im for im in coco["images"] if Path(im["file_name"]).stem in stems}

    # Ultralytics는 이미지 경로의 '/images/' 세그먼트를 '/labels/'로 치환해 레이블을
    # 찾습니다. 따라서 별도 데이터셋 루트를 만들되 images/labels 이름은 그대로 써야 합니다.
    out_img = bal / "images" / "val"          # 원본 이미지 재사용원
    base_root = bal.parent / "tier2_base80"
    out_lbl = base_root / "labels" / "val"
    out_lbl.mkdir(parents=True, exist_ok=True)
    lines_by_img = {iid: [] for iid in meta}
    for ann in coco["annotations"]:
        if ann["image_id"] not in meta or ann.get("iscrowd", 0):
            continue
        m = meta[ann["image_id"]]
        W, H = m["width"], m["height"]
        x, y, w, h = ann["bbox"]
        if w < 2 or h < 2:
            continue
        c = id_to_yolo[ann["category_id"]]
        lines_by_img[ann["image_id"]].append(
            f"{c} {(x + w / 2) / W:.6f} {(y + h / 2) / H:.6f} {w / W:.6f} {h / H:.6f}"
        )
    for iid, lines in lines_by_img.items():
        (out_lbl / (Path(meta[iid]["file_name"]).stem + ".txt")).write_text("\n".join(lines))

    alt_img = base_root / "images" / "val"
    alt_img.mkdir(parents=True, exist_ok=True)
    import os
    for p in out_img.glob("*.jpg"):
        d = alt_img / p.name
        if not d.exists():
            try:
                os.link(p, d)
            except OSError:
                import shutil
                shutil.copy2(p, d)

    yaml_path = base_root / "baseline80.yaml"
    yaml_path.write_text("\n".join([
        f"train: {alt_img.resolve()}",
        f"val:   {alt_img.resolve()}",
        f"nc: {len(yolo_names)}",
        f"names: {yolo_names}",
        "",
    ]))
    return yaml_path, yolo_names


def main():
    args = parse_args()
    from ultralytics import YOLO

    ours = YOLO(args.model)
    m1 = ours.val(data=args.data, imgsz=args.imgsz, device=args.device,
                  batch=args.batch, workers=0, plots=False, verbose=False)
    t1 = per_class_table(m1, CLASS_NAMES, f"우리 모델 @{args.imgsz} (16클래스 전용)")

    if args.skip_baseline:
        return

    yaml80, names80 = build_baseline_labels(Path(args.src), Path(args.bal))
    base = YOLO("weights/yolo11n.pt")
    m2 = base.val(data=str(yaml80), imgsz=args.imgsz, device=args.device,
                  batch=args.batch, workers=0, plots=False, verbose=False)
    t2 = per_class_table(m2, names80, f"사전학습 yolo11n @{args.imgsz} (COCO 118k 학습, 80클래스)")

    print(f"\n===== 공통 16클래스 비교 (AP50-95) =====")
    print(f"{'class':<14}{'우리':>8}{'사전학습':>10}{'차이':>9}")
    d_sum = 0.0
    n = 0
    for nm in CLASS_NAMES:
        if nm in t1 and nm in t2:
            a, b = t1[nm][1], t2[nm][1]
            d_sum += a - b
            n += 1
            print(f"{nm:<14}{a:>8.3f}{b:>10.3f}{a - b:>+9.3f}")
    if n:
        print(f"{'평균 차이':<14}{'':>8}{'':>10}{d_sum / n:>+9.3f}")


if __name__ == "__main__":
    main()
