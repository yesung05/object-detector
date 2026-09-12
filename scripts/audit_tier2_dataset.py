"""Audit YOLO dataset split ratios, class balance, and cross-split leakage."""

import argparse
from collections import Counter
from pathlib import Path

import yaml


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True, help="Ultralytics dataset YAML")
    return parser.parse_args()


def resolve_path(value: str, yaml_path: Path) -> Path:
    path = Path(value)
    return path if path.is_absolute() else (yaml_path.parent / path).resolve()


def main():
    args = parse_args()
    yaml_path = Path(args.data).resolve()
    data = yaml.safe_load(yaml_path.read_text(encoding="utf-8"))
    names = data["names"]
    if isinstance(names, dict):
        names = [names[i] for i in sorted(names)]

    split_stems = {}
    split_stats = {}
    for split in ("train", "test", "val"):
        image_dir = resolve_path(data[split], yaml_path)
        label_dir = Path(str(image_dir).replace("\\images\\", "\\labels\\"))
        if label_dir == image_dir:
            label_dir = image_dir.parent.parent / "labels" / image_dir.name

        images = {p.stem for p in image_dir.iterdir() if p.is_file()}
        labels = {p.stem for p in label_dir.glob("*.txt")}
        image_counts, instance_counts = Counter(), Counter()
        invalid = 0
        for label_path in label_dir.glob("*.txt"):
            present = set()
            for line in label_path.read_text(encoding="utf-8").splitlines():
                fields = line.split()
                try:
                    class_id = int(fields[0])
                    coords = [float(value) for value in fields[1:]]
                    valid = len(coords) == 4 and all(0.0 <= value <= 1.0 for value in coords)
                except (IndexError, ValueError):
                    invalid += 1
                    continue
                if not 0 <= class_id < len(names) or not valid:
                    invalid += 1
                    continue
                present.add(class_id)
                instance_counts[class_id] += 1
            for class_id in present:
                image_counts[class_id] += 1

        split_stems[split] = images
        split_stats[split] = (image_counts, instance_counts)
        print(
            f"{split}: images={len(images)} labels={len(labels)} "
            f"missing_labels={len(images - labels)} orphan_labels={len(labels - images)} "
            f"invalid_rows={invalid}"
        )
        for class_id, name in enumerate(names):
            print(
                f"  {name:<13} images={image_counts[class_id]:>6} "
                f"instances={instance_counts[class_id]:>7}"
            )

    total = sum(len(stems) for stems in split_stems.values())
    ratios = {split: len(stems) / total for split, stems in split_stems.items()}
    print(
        "ratios: "
        + " / ".join(f"{split}={ratios[split]:.2%}" for split in ("train", "test", "val"))
    )
    for left, right in (("train", "test"), ("train", "val"), ("test", "val")):
        print(f"overlap {left}/{right}: {len(split_stems[left] & split_stems[right])}")

    print("class split ratios (train/test/val, by instances):")
    for class_id, name in enumerate(names):
        counts = [split_stats[s][1][class_id] for s in ("train", "test", "val")]
        class_total = sum(counts)
        values = [count / class_total if class_total else 0.0 for count in counts]
        print(f"  {name:<13} " + " / ".join(f"{value:.2%}" for value in values))


if __name__ == "__main__":
    main()
