"""
HomeObjects-3K → Tier 2 v4 데이터셋 (개발 PC 전용, 현장 기기에 배포하지 않음)

Tier 2 는 7클래스(cat, dog, bottle, cup, food, chair, dining table)다. HomeObjects-3K 에서
쓸 수 있는 것은 chair 와 table 뿐이고, 그대로 학습에 넣으면 문제가 둘 생긴다.

1. 라벨 누락
   HomeObjects 이미지에는 cup/bottle/food/cat/dog 라벨이 없다. 그 물체가 사진에 있어도
   "배경"으로 학습되어 해당 클래스 재현율이 깎인다(docs/tier2-data-plan 의 "빈 라벨 금지").
   → 더 큰 COCO 사전학습 교사(yolo11x)로 그 5클래스만 자동 라벨한다. 교사 클래스는 ID가
     아니라 *이름*으로 조회한다. docs/2026-09-10-tier2-dataset-labeling-issue.md 의 사고
     (COCO 80-인덱스와 91-category_id 혼동으로 클래스가 통째로 밀림)를 반복하지 않기 위해서다.
   → chair/table 은 사람이 단 라벨이 있으므로 교사를 쓰지 않는다.

2. 도메인
   전부 거실 사진이다(이름이 living_room_*). table 은 대부분 소파 앞 커피·사이드 테이블이라
   카페의 dining table 과 다르다. 병합이 도움이 되는지는 가정이 아니라 실험으로 가린다:
   --table merge (chair+table 추가) / --table skip (chair 만 추가) 두 변형을 만든다.

평가용 분리
   HomeObjects val 404장은 학습에 절대 넣지 않고 --eval-out 에 chair/table 만 리맵해 둔다.
   (a) tier2_v3 val 은 학습 중 모델 선택에 쓰인 낙관적 세트이고, (b) 이쪽이 도메인 밖 세트다.

v3 파일은 복사하지 않고 하드링크한다. 약 2만 6천 장·수 GB 라 복사하면 디스크와 시간이 낭비되고,
v3 는 읽기 전용으로만 쓰기 때문에 링크가 안전하다(같은 볼륨이 아니면 복사로 대체).

사용:
  .venv-train\\Scripts\\python.exe -I scripts\\build_tier2_homeobjects.py --table merge --out datasets\\tier2_v4
  .venv-train\\Scripts\\python.exe -I scripts\\build_tier2_homeobjects.py --table skip  --out datasets\\tier2_v4c
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Tier 2 v3 클래스 순서(datasets/tier2_v3/tier2_v2.yaml). 이 순서가 곧 모델 출력 채널이다.
T2_NAMES = ["cat", "dog", "bottle", "cup", "food", "chair", "dining table"]
T2_CHAIR, T2_TABLE = 5, 6

# HomeObjects 클래스 인덱스(HomeObjects-3K.yaml).
HO_CHAIR, HO_TABLE = 2, 3

# food 는 build_tier2_balanced.py 와 같은 병합 규칙(COCO 음식 10종)을 쓴다.
TEACHER_GROUPS = {
    0: ["cat"],
    1: ["dog"],
    2: ["bottle"],
    3: ["cup"],
    4: ["banana", "apple", "sandwich", "orange", "broccoli", "carrot",
        "hot dog", "pizza", "donut", "cake"],
}


def parse_args():
    p = argparse.ArgumentParser(description="HomeObjects-3K → Tier 2 v4")
    p.add_argument("--src", default="datasets/homeobjects-3K")
    p.add_argument("--v3", default="datasets/tier2_v3")
    p.add_argument("--out", default="datasets/tier2_v4")
    p.add_argument("--eval-out", default="datasets/homeobjects-eval")
    p.add_argument("--table", choices=["merge", "skip"], default="merge",
                   help="merge: HomeObjects table 을 dining table 로 추가, skip: chair 만")
    p.add_argument("--teacher", default="weights/yolo11x.pt")
    p.add_argument("--teacher-conf", type=float, default=0.5,
                   help="높게 잡는 이유: 교사 오탐이 곧 학습 라벨 오염이라 재현율보다 정밀도가 중요")
    p.add_argument("--teacher-cache", default="datasets/homeobjects-teacher-labels")
    p.add_argument("--device", default="0")
    p.add_argument("--teacher-batch", type=int, default=8,
                   help="작게 둔 이유: 개발 PC GPU 를 다른 프로세스가 5GB 이상 쓰고 있어 배치 32 는 "
                        "VRAM 이 넘쳐 시스템 메모리로 밀리며 분당 1배치로 느려졌다(실측)")
    p.add_argument("--stats", default=None, help="통계 JSON 저장 경로(기본: <out>/build_stats.json)")
    p.add_argument("--skip-dedup", action="store_true")
    return p.parse_args()


def link_or_copy(src: Path, dst: Path):
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        return
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def safe_stem(name: str) -> str:
    """'living_room_1p (1)' 같은 공백·괄호 이름을 파일 시스템·Ultralytics 에 안전한 형태로."""
    return re.sub(r"[^0-9A-Za-z_.-]+", "_", name)


def read_labels(path: Path):
    rows = []
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            p = line.split()
            if len(p) >= 5:
                rows.append((int(p[0]), *map(float, p[1:5])))
    return rows


def write_labels(path: Path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(f"{c} {x:.6f} {y:.6f} {w:.6f} {h:.6f}\n" for c, x, y, w, h in rows),
                    encoding="utf-8")


def remap_homeobjects(rows, table_mode):
    out = []
    for c, x, y, w, h in rows:
        if c == HO_CHAIR:
            out.append((T2_CHAIR, x, y, w, h))
        elif c == HO_TABLE and table_mode == "merge":
            out.append((T2_TABLE, x, y, w, h))
    return out


def md5(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def teacher_labels(images, args):
    """교사 라벨을 캐시에 저장하고 {이미지 이름: [(class,x,y,w,h)...]} 를 돌려준다.

    캐시를 두는 이유: 교사 추론이 이 스크립트에서 가장 비싼 단계인데, --table merge/skip 두
    변형이 같은 라벨을 쓰므로 한 번만 돌리면 된다.
    """
    cache = ROOT / args.teacher_cache
    cache.mkdir(parents=True, exist_ok=True)
    todo = [im for im in images if not (cache / (im.stem + ".txt")).exists()]
    stats = {"teacher": args.teacher, "conf": args.teacher_conf, "inferred": len(todo)}
    if todo:
        from ultralytics import YOLO
        from ultralytics.utils.downloads import attempt_download_asset

        weights = ROOT / args.teacher
        weights.parent.mkdir(parents=True, exist_ok=True)
        if not weights.exists():
            attempt_download_asset(str(weights))
        model = YOLO(str(weights))
        name_to_id = {n: i for i, n in model.names.items()}
        group_ids = {}
        for t2, names in TEACHER_GROUPS.items():
            for n in names:
                if n not in name_to_id:  # 이름이 없으면 조용히 빠지지 않고 즉시 중단
                    sys.exit(f"교사 모델에 클래스 '{n}' 이 없습니다: {sorted(name_to_id)[:10]}...")
                group_ids[name_to_id[n]] = t2
        tb = args.teacher_batch
        for start in range(0, len(todo), tb):
            batch = todo[start:start + tb]
            # half: 라벨링 정밀도에 영향이 작고 VRAM·시간을 절반으로 줄인다(CPU 에서는 무시됨)
            results = model.predict([str(p) for p in batch], conf=args.teacher_conf, imgsz=640,
                                    device=args.device, half=args.device != "cpu", verbose=False)
            for im, r in zip(batch, results):
                rows = []
                for cls, xywhn in zip(r.boxes.cls.tolist(), r.boxes.xywhn.tolist()):
                    t2 = group_ids.get(int(cls))
                    if t2 is not None:
                        rows.append((t2, *xywhn))
                write_labels(cache / (im.stem + ".txt"), rows)
            print(f"  교사 추론 {min(start + tb, len(todo))}/{len(todo)}", flush=True)
    out = {im.name: read_labels(cache / (im.stem + ".txt")) for im in images}
    return out, stats


def main():
    args = parse_args()
    src, v3 = ROOT / args.src, ROOT / args.v3
    out, eval_out = ROOT / args.out, ROOT / args.eval_out
    stats = {"table_mode": args.table, "src_sha256_note": "datasets/homeobjects-3K-raw/*.sha256"}

    # 1) 평가 세트: val 404장, chair/table 만 리맵(교사 라벨 없음). 두 변형이 같은 평가를 쓰도록 항상 table 포함.
    ev = Counter()
    for img in sorted((src / "images/val").iterdir()):
        stem = safe_stem(img.stem)
        rows = remap_homeobjects(read_labels(src / "labels/val" / (img.stem + ".txt")), "merge")
        link_or_copy(img, eval_out / "images/val" / (stem + img.suffix.lower()))
        write_labels(eval_out / "labels/val" / (stem + ".txt"), rows)
        for r in rows:
            ev[T2_NAMES[r[0]]] += 1
    (eval_out / "eval.yaml").write_text(
        f"path: {eval_out.as_posix()}\nval: images/val\ntrain: images/val\n"
        f"nc: {len(T2_NAMES)}\nnames: {T2_NAMES}\n", encoding="utf-8")
    stats["eval_instances"] = dict(ev)

    # 2) v3 를 그대로(하드링크) 가져온다. val/test 는 v3 와 같아야 v3 대비 회귀를 비교할 수 있다.
    n_v3 = Counter()
    for split in ("train", "val", "test"):
        for sub in ("images", "labels"):
            d = v3 / sub / split
            if not d.is_dir():
                continue
            for f in d.iterdir():
                link_or_copy(f, out / sub / split / f.name)
                if sub == "images":
                    n_v3[split] += 1
    stats["v3_images"] = dict(n_v3)

    # 3) HomeObjects train 추가분: 중복 제거 → 리맵 → 교사 라벨 병합
    train_imgs = sorted((src / "images/train").iterdir())
    dup = 0
    if not args.skip_dedup:
        # 정확히 같은 바이트만 중복으로 본다. 재인코딩·리사이즈된 같은 사진은 못 잡는 한계가 있다.
        v3_hashes = {md5(f) for f in (v3 / "images/train").iterdir()}
        keep = []
        for im in train_imgs:
            if md5(im) in v3_hashes:
                dup += 1
            else:
                keep.append(im)
        train_imgs = keep
    stats["dedup_removed"] = dup
    teach, tstats = teacher_labels(train_imgs, args)
    stats["teacher"] = tstats

    added, inst = 0, Counter()
    for im in train_imgs:
        rows = remap_homeobjects(read_labels(src / "labels/train" / (im.stem + ".txt")), args.table)
        rows += teach[im.name]
        # chair/table 라벨이 하나도 없고 교사 라벨도 없으면 정보가 없는 이미지라 제외한다.
        if not rows:
            continue
        stem = "ho_" + safe_stem(im.stem)
        link_or_copy(im, out / "images/train" / (stem + im.suffix.lower()))
        write_labels(out / "labels/train" / (stem + ".txt"), rows)
        added += 1
        for r in rows:
            inst[T2_NAMES[r[0]]] += 1
    stats["added_images"] = added
    stats["added_instances"] = dict(inst)

    (out / "tier2_v4.yaml").write_text(
        f"train: {(out / 'images/train').as_posix()}\n"
        f"val:   {(out / 'images/val').as_posix()}\n"
        f"test:  {(out / 'images/test').as_posix()}\n"
        f"nc: {len(T2_NAMES)}\nnames: {T2_NAMES}\n", encoding="utf-8")
    stats_path = Path(args.stats) if args.stats else out / "build_stats.json"
    stats_path.write_text(json.dumps(stats, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(stats, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
