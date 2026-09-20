"""Train aspect-ratio-specific, narrower FP32 YOLO-pose students.

The script converts the local COCO person-keypoint annotations into a small
YOLO-pose dataset, then trains a narrower student with ordinary pose loss plus
native score-weighted feature distillation from the existing FP32 teacher. It intentionally
keeps one output model per camera aspect ratio.

Example:
  .venv-train\\Scripts\\python.exe scripts\\distill_pose_fp32.py `
    --ratios 224,288,416 --max-train 4000 --max-val 500 --epochs 10
"""

from __future__ import annotations

import argparse
import json
import random
import shutil
from collections import defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TEACHER = ROOT / "yolo11n-pose.pt"
DEFAULT_CONFIG = ROOT / "scripts" / "yolo11n-distill-pose.yaml"
DEFAULT_DATASET = ROOT / "datasets" / "pose_distill"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--teacher", type=Path, default=DEFAULT_TEACHER)
    parser.add_argument("--student-config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--dataset-root", type=Path, default=DEFAULT_DATASET)
    parser.add_argument("--project", type=Path, default=ROOT / "runs" / "pose-distill")
    parser.add_argument("--ratios", default="224,288,416", help="model heights; width is always 416")
    parser.add_argument("--max-train", type=int, default=4000)
    parser.add_argument("--max-val", type=int, default=500)
    parser.add_argument("--seed", type=int, default=4200)
    parser.add_argument("--epochs", type=int, default=10)
    parser.add_argument("--batch", type=int, default=8)
    parser.add_argument("--workers", type=int, default=0)
    parser.add_argument("--device", default="0")
    parser.add_argument("--distill-weight", type=float, default=0.05)
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--skip-prepare", action="store_true")
    parser.add_argument("--no-amp", action="store_true")
    return parser.parse_args()


def _annotation_index(annotation_path: Path) -> tuple[dict[str, dict], dict[int, list[dict]]]:
    payload = json.loads(annotation_path.read_text(encoding="utf-8"))
    images = {item["file_name"]: item for item in payload["images"]}
    annotations: dict[int, list[dict]] = defaultdict(list)
    for item in payload["annotations"]:
        if item.get("iscrowd", 0) == 0 and item.get("num_keypoints", 0) > 0:
            annotations[item["image_id"]].append(item)
    return images, annotations


def _write_pose_label(image: dict, items: list[dict], destination: Path) -> bool:
    width, height = image["width"], image["height"]
    lines: list[str] = []
    for item in items:
        x, y, bw, bh = item["bbox"]
        x = max(0.0, min(float(x), width))
        y = max(0.0, min(float(y), height))
        bw = max(1.0, min(float(bw), width - x))
        bh = max(1.0, min(float(bh), height - y))
        values = [0, (x + bw / 2) / width, (y + bh / 2) / height, bw / width, bh / height]
        keypoints = item.get("keypoints", [])
        if len(keypoints) != 51:
            continue
        for index in range(17):
            kx, ky, visibility = keypoints[index * 3 : index * 3 + 3]
            values.extend([float(kx) / width, float(ky) / height, int(visibility)])
        lines.append(" ".join(f"{value:.6f}" if isinstance(value, float) else str(value) for value in values))
    if not lines:
        return False
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return True


def _link_or_copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        return
    try:
        destination.hardlink_to(source)
    except OSError:
        shutil.copy2(source, destination)


def prepare_split(
    source_images: Path,
    annotation_path: Path,
    output_root: Path,
    split: str,
    maximum: int,
    seed: int,
) -> int:
    images, annotations = _annotation_index(annotation_path)
    candidates = []
    for source in sorted(source_images.glob("*.jpg")):
        image = images.get(source.name)
        if image and annotations.get(image["id"]):
            candidates.append((source, image, annotations[image["id"]]))
    random.Random(seed + (0 if split == "train" else 1)).shuffle(candidates)
    selected = candidates[:maximum]
    count = 0
    for source, image, items in selected:
        image_destination = output_root / "images" / split / source.name
        label_destination = output_root / "labels" / split / f"{source.stem}.txt"
        if _write_pose_label(image, items, label_destination):
            _link_or_copy(source, image_destination)
            count += 1
    return count


def prepare_dataset(args: argparse.Namespace) -> Path:
    root = args.dataset_root.resolve()
    train_count = prepare_split(
        ROOT / "datasets" / "tier2" / "images" / "train",
        ROOT / "datasets" / "tier2" / "annotations" / "person_keypoints_train2017.json",
        root,
        "train",
        args.max_train,
        args.seed,
    )
    val_count = prepare_split(
        ROOT / "datasets" / "tier2" / "images" / "val",
        ROOT / "datasets" / "tier2" / "annotations" / "person_keypoints_val2017.json",
        root,
        "val",
        args.max_val,
        args.seed,
    )
    data_path = root / "pose.yaml"
    data_path.parent.mkdir(parents=True, exist_ok=True)
    data_path.write_text(
        "\n".join(
            [
                f"path: {root.as_posix()}",
                "train: images/train",
                "val: images/val",
                "nc: 1",
                "names: ['person']",
                "kpt_shape: [17, 3]",
                "flip_idx: [0, 2, 1, 4, 3, 6, 5, 8, 7, 10, 9, 12, 11, 14, 13, 16, 15]",
                "kpt_names: [['nose', 'left_eye', 'right_eye', 'left_ear', 'right_ear', 'left_shoulder', 'right_shoulder', 'left_elbow', 'right_elbow', 'left_wrist', 'right_wrist', 'left_hip', 'right_hip', 'left_knee', 'right_knee', 'left_ankle', 'right_ankle']]",
                "",
            ]
        ),
        encoding="utf-8",
    )
    print(f"[dataset] train={train_count} val={val_count} yaml={data_path}")
    return data_path


def train_ratio(args: argparse.Namespace, data_path: Path, height: int) -> Path:
    from ultralytics import YOLO
    from distill_pose_2m import audit_distillation

    width = 416
    tag = f"416x{height}"
    student = YOLO(str(args.student_config))
    student.add_callback('on_train_start', audit_distillation)
    # Ultralytics training currently accepts only an integer imgsz. Train each
    # student at its target short-side size, then export the deployment model
    # as the rectangular 416x{height} variant used by model_select.c.
    student.train(
        data=str(data_path),
        imgsz=height,
        epochs=args.epochs,
        batch=args.batch,
        workers=args.workers,
        device=args.device,
        project=str(args.project),
        name=tag,
        exist_ok=True,
        pretrained=str(args.teacher),
        distill_model=str(args.teacher),
        dis=args.distill_weight,
        amp=not args.no_amp,
        patience=max(2, min(args.epochs, 10)),
        plots=True,
    )
    save_dir = Path(student.trainer.save_dir)
    best = save_dir / "weights" / "best.pt"
    if not best.exists():
        raise FileNotFoundError(best)
    exported = YOLO(str(best)).export(
        format="onnx", imgsz=[height, width], simplify=True, dynamic=False
    )
    destination = ROOT / "models" / f"yolo11n-pose-distilled-{tag}.onnx"
    shutil.copy2(Path(exported), destination)
    print(f"[export] {destination}")
    return destination


def main() -> None:
    args = parse_args()
    args.teacher = args.teacher.resolve()
    args.student_config = args.student_config.resolve()
    if not args.teacher.exists() or not args.student_config.exists():
        raise SystemExit("teacher 또는 student config 파일을 찾지 못했습니다")
    heights = [int(value.strip()) for value in args.ratios.split(",") if value.strip()]
    if any(height not in (224, 288, 416) for height in heights):
        raise SystemExit("ratios는 224, 288, 416 중에서 지정하세요")
    data_path = args.dataset_root / "pose.yaml"
    if not args.skip_prepare:
        data_path = prepare_dataset(args)
    if not data_path.exists():
        raise SystemExit(f"데이터 YAML이 없습니다: {data_path}")
    if args.prepare_only:
        return
    for height in heights:
        train_ratio(args, data_path, height)


if __name__ == "__main__":
    main()
