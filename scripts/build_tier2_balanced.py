"""
Tier 2 데이터셋 재구축 — 카테고리 ID 정정 + 클래스 병합 + 클래스별 균형 조정

기존 train_tier2.py에는 두 가지 문제가 있었습니다.

1. 카테고리 ID 오류
   COCO 80-클래스 인덱스(cat=15, dog=16, ...)를 instances_*.json의
   91-카테고리 ID로 그대로 썼습니다. 두 체계는 다릅니다(cat의 실제 ID는 17).
   그 결과 bench를 cat으로, skateboard를 cup으로 학습시켰습니다.
   여기서는 하드코딩 대신 JSON의 categories 배열에서 '이름으로' ID를 조회합니다.

2. 클래스 불균형
   "16클래스 중 하나라도 포함된 이미지"를 전부 받으면 COCO에 흔한 클래스가
   희귀 클래스를 수십 배 압도해 희귀 클래스 recall이 무너집니다.

균형 전략은 '클래스별 이미지 상한(cap) 기반 그리디 선택'입니다.
  - 오버샘플링이 아닌 언더샘플링을 택한 이유: 이미지 복제는 320px 저해상도에서
    과적합만 키우고 에폭 시간을 늘립니다.
  - 손실 함수 class weight가 아닌 데이터셋 레벨 조정을 택한 이유: Ultralytics가
    클래스 가중치 주입을 공식 지원하지 않아, 포크 없이 쓸 수 있는 레버가 샘플 구성뿐입니다.
  - 희귀 클래스부터 채우는 이유: COCO는 다중 레이블이라 흔한 클래스는 희귀 클래스
    이미지에 덤으로 딸려옵니다. 희귀부터 채워야 흔한 클래스 예산이 낭비되지 않습니다.

v2 스펙에서 바뀐 점 (v1 실측 결과를 근거로):
  - 세분화된 음식 10종을 food 하나로 병합.
    감지 목적이 "외부 음식 반입"이라 바나나인지 당근인지 구분할 필요가 없습니다.
    v1에서 apple 0.114, carrot 0.178처럼 개별 AP가 낮았는데, 병합하면 클래스당
    학습 데이터가 10배가 되어 훨씬 안정적으로 잡힙니다.
  - chair / dining table 상한 해제.
    v1에서 chair를 12,774장 → 2,500장으로 깎았더니 AP가 0.094로, COCO 전체를
    학습한 사전학습 모델(0.160)보다 나빠졌습니다. 착석 감지·장기 체류·반려동물
    의자 착석 판정이 전부 chair에 의존하므로 이 클래스는 깎으면 안 됩니다.

v3 분할 방식:
  COCO train2017에서 균형 선택한 뒤 --ratios(기본 0.70 0.20 0.10)로
  train/test/val 세 벌로 분할합니다. test 셋은 학습 중 일절 사용하지 않으며
  최종 평가에만 씁니다. COCO val2017은 사용하지 않습니다.

이미지는 기존 데이터셋 디렉터리에서 하드링크로 재사용하고(디스크 중복 회피),
없는 것만 COCO에서 내려받습니다.
"""

import argparse
import json
import os
import random
import shutil
import urllib.request
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

FOODS = ["banana", "apple", "sandwich", "orange", "broccoli",
         "carrot", "hot dog", "pizza", "donut", "cake"]

# (출력 클래스명, 포함할 COCO 클래스명, 선택 상한). 상한 None = 무제한.
PRESETS = {
    # 무인 카페 감지 대상: 반려동물(2) + 음료 용기(2) + 반입 음식(1) + 착석 관련(2)
    "v2": [
        ("cat",          ["cat"],          None),
        ("dog",          ["dog"],          None),
        ("bottle",       ["bottle"],       6000),
        ("cup",          ["cup"],          6000),
        ("food",         FOODS,            8000),
        ("chair",        ["chair"],        None),
        ("dining table", ["dining table"], None),
    ],
    # v1: 16클래스 세분화 스펙 (재현용으로 남겨둡니다)
    "v1": [(n, [n], 2500) for n in
           ["cat", "dog", "bottle", "cup"] + FOODS + ["chair", "dining table"]],
}

IMG_BASE = "http://images.cocodataset.org/train2017/"
ANN_FILE = "instances_train2017.json"

# 200바이트 미만이면 중단된 다운로드로 간주합니다. JPEG 헤더조차 안 되는 크기입니다.
MIN_VALID_BYTES = 200


def parse_args():
    p = argparse.ArgumentParser(description="Tier 2 데이터셋 재구축")
    p.add_argument("--preset", default="v2", choices=sorted(PRESETS))
    p.add_argument("--ann", default="datasets/tier2/annotations", help="COCO 어노테이션 위치")
    p.add_argument("--cache", nargs="*", default=["datasets/tier2", "datasets/tier2_bal"],
                   help="이미지 재사용원 (하드링크). 앞에서부터 탐색합니다")
    p.add_argument("--dst", default="datasets/tier2_v2", help="출력 위치")
    p.add_argument("--ratios", nargs=3, type=float, default=[0.70, 0.20, 0.10],
                   metavar=("TRAIN", "TEST", "VAL"),
                   help="train/test/val 비율 (합=1.0). 기본: 0.70 0.20 0.10")
    p.add_argument("--seed", type=int, default=42, help="분할 재현성을 위한 난수 시드")
    p.add_argument("--workers", type=int, default=24, help="다운로드 병렬 수")
    p.add_argument("--report-only", action="store_true", help="분포만 출력하고 종료")
    return p.parse_args()


def load_train(ann_dir: Path, spec):
    """COCO train2017 JSON을 읽어 (이미지메타, 이미지별 어노테이션, 이미지별 클래스집합)을 만듭니다."""
    with open(ann_dir / ANN_FILE, encoding="utf-8") as f:
        coco = json.load(f)

    name_to_id = {c["name"]: c["id"] for c in coco["categories"]}
    cat_to_our = {}
    for our_idx, (_, coco_names, _) in enumerate(spec):
        for cn in coco_names:
            if cn not in name_to_id:
                raise SystemExit(f"COCO에 없는 클래스명: {cn}")
            cat_to_our[name_to_id[cn]] = our_idx

    ann_by_img = defaultdict(list)
    cls_by_img = defaultdict(set)
    for ann in coco["annotations"]:
        our = cat_to_our.get(ann["category_id"])
        if our is None:
            continue
        # iscrowd 영역은 bbox가 군집 전체를 감싸 학습에 해로우므로 제외합니다.
        if ann.get("iscrowd", 0):
            continue
        w, h = ann["bbox"][2], ann["bbox"][3]
        # 320px 학습에서 1px 미만이 되는 극소 박스는 노이즈입니다.
        if w < 2 or h < 2:
            continue
        ann_by_img[ann["image_id"]].append((our, ann["bbox"]))
        cls_by_img[ann["image_id"]].add(our)

    meta = {im["id"]: im for im in coco["images"] if im["id"] in cls_by_img}
    return meta, ann_by_img, cls_by_img


def report(title, names, cls_by_img, ann_by_img):
    img_cnt, inst_cnt = Counter(), Counter()
    for iid, cset in cls_by_img.items():
        for c in cset:
            img_cnt[c] += 1
        for our, _ in ann_by_img[iid]:
            inst_cnt[our] += 1
    print(f"\n--- {title} (이미지 {len(cls_by_img)}장) ---")
    print(f"{'class':<14}{'images':>8}{'instances':>11}")
    for i, nm in enumerate(names):
        print(f"{nm:<14}{img_cnt[i]:>8}{inst_cnt[i]:>11}")
    print(f"{'TOTAL':<14}{'':>8}{sum(inst_cnt.values()):>11}")
    if img_cnt:
        lo, hi = min(img_cnt.values()), max(img_cnt.values())
        print(f"불균형비(max/min 이미지수): {hi / max(lo, 1):.1f}x")


def select_balanced(cls_by_img, caps):
    """희귀 클래스 우선 그리디 선택. caps[c] is None이면 해당 클래스는 전량 사용합니다."""
    imgs_of = defaultdict(list)
    for iid, cset in cls_by_img.items():
        for c in cset:
            imgs_of[c].append(iid)

    order = sorted(imgs_of, key=lambda c: len(imgs_of[c]))
    chosen, filled = set(), Counter()

    for c in order:
        cap = caps[c]
        if cap is not None and filled[c] >= cap:
            continue
        # 같은 클래스 후보 중에서는 레이블 종류가 적은 이미지를 먼저 씁니다.
        for iid in sorted(imgs_of[c], key=lambda i: len(cls_by_img[i])):
            if cap is not None and filled[c] >= cap:
                break
            if iid in chosen:
                continue
            chosen.add(iid)
            for cc in cls_by_img[iid]:
                filled[cc] += 1
    return chosen


def split_ids(chosen, ratios, seed):
    """선택된 이미지 ID를 train/test/val로 무작위 분할합니다."""
    ids = sorted(chosen)  # 정렬 후 셔플해야 seed가 같으면 항상 동일한 결과를 냅니다.
    rng = random.Random(seed)
    rng.shuffle(ids)
    n = len(ids)
    n_train = round(n * ratios[0])
    n_test = round(n * ratios[1])
    return {
        "train": ids[:n_train],
        "test":  ids[n_train:n_train + n_test],
        "val":   ids[n_train + n_test:],
    }


def link_or_copy(src: Path, dst: Path) -> bool:
    """하드링크 우선. 볼륨이 다르거나 FS가 거부하면 복사로 폴백합니다."""
    try:
        os.link(src, dst)
        return True
    except OSError:
        try:
            shutil.copy2(src, dst)
            return True
        except OSError:
            return False


def fetch(task):
    url, dst = task
    try:
        tmp = dst.with_suffix(".part")
        urllib.request.urlretrieve(url, tmp)
        tmp.replace(dst)
        return True
    except Exception:
        return False


def materialize(split_name, ids, meta, ann_by_img, caches, dst_root: Path, workers: int):
    img_dir = dst_root / "images" / split_name
    lbl_dir = dst_root / "labels" / split_name
    img_dir.mkdir(parents=True, exist_ok=True)
    lbl_dir.mkdir(parents=True, exist_ok=True)

    # 재분할 전 데이터셋의 어느 split에 있든 원본 이미지를 재사용합니다.
    # train만 검색하면 이전 test/val에 있던 파일을 불필요하게 다시 내려받게 됩니다.
    cache_dirs = [
        Path(cache) / "images" / split
        for cache in caches
        for split in ("train", "test", "val")
        if (Path(cache) / "images" / split).is_dir()
    ]

    need, linked = [], 0
    for iid in ids:
        fn = meta[iid]["file_name"]
        dst = img_dir / fn
        if dst.exists() and dst.stat().st_size >= MIN_VALID_BYTES:
            continue
        for cd in cache_dirs:
            cached = cd / fn
            if cached.exists() and cached.stat().st_size >= MIN_VALID_BYTES and link_or_copy(cached, dst):
                linked += 1
                break
        else:
            need.append((IMG_BASE + fn, dst))

    print(f"[{split_name}] 캐시 재사용 {linked}장 / 다운로드 {len(need)}장")
    failed = 0
    if need:
        done = 0
        with ThreadPoolExecutor(max_workers=workers) as ex:
            for fut in as_completed([ex.submit(fetch, t) for t in need]):
                done += 1
                if not fut.result():
                    failed += 1
                if done % 500 == 0 or done == len(need):
                    print(f"  {done}/{len(need)} (실패 {failed})", flush=True)
        if failed:
            print(f"[{split_name}] 경고: {failed}장 다운로드 실패 — 해당 이미지는 제외됩니다")

    written = 0
    for iid in ids:
        fn = meta[iid]["file_name"]
        if not (img_dir / fn).exists():
            continue
        W, H = meta[iid]["width"], meta[iid]["height"]
        lines = []
        for our, (x, y, w, h) in ann_by_img[iid]:
            cx = min(max((x + w / 2) / W, 0.0), 1.0)
            cy = min(max((y + h / 2) / H, 0.0), 1.0)
            lines.append(f"{our} {cx:.6f} {cy:.6f} {min(w / W, 1.0):.6f} {min(h / H, 1.0):.6f}")
        (lbl_dir / (Path(fn).stem + ".txt")).write_text("\n".join(lines))
        written += 1
    print(f"[{split_name}] 레이블 {written}개 작성")


def main():
    args = parse_args()
    ratios = args.ratios
    if abs(sum(ratios) - 1.0) > 1e-6:
        raise SystemExit(f"--ratios 합이 1.0이어야 합니다: {ratios} (합={sum(ratios):.4f})")

    spec = PRESETS[args.preset]
    names = [s[0] for s in spec]
    ann_dir = Path(args.ann)
    dst_root = Path(args.dst)
    if not ann_dir.exists():
        raise SystemExit(f"어노테이션이 없습니다: {ann_dir}")

    print(f"\n===== COCO train2017 로드 =====")
    meta, ann_by_img, cls_by_img = load_train(ann_dir, spec)
    report("원본 (train2017 전체)", names, cls_by_img, ann_by_img)

    caps = {i: s[2] for i, s in enumerate(spec)}
    chosen = select_balanced(cls_by_img, caps)
    report("균형 조정 후 (선택 완료)", names,
           {i: cls_by_img[i] for i in chosen}, ann_by_img)

    splits = split_ids(chosen, ratios, args.seed)
    print(f"\n분할 결과 (seed={args.seed}): "
          f"train {len(splits['train'])}장 / "
          f"test {len(splits['test'])}장 / "
          f"val {len(splits['val'])}장")
    print(f"비율: {ratios[0]:.0%} / {ratios[1]:.0%} / {ratios[2]:.0%}")

    if args.report_only:
        return

    for split_name, ids in splits.items():
        materialize(split_name, ids, meta, ann_by_img, args.cache, dst_root, args.workers)

    yaml_path = dst_root / f"tier2_{args.preset}.yaml"
    yaml_path.write_text("\n".join([
        f"train: {(dst_root / 'images' / 'train').resolve()}",
        f"val:   {(dst_root / 'images' / 'val').resolve()}",
        f"test:  {(dst_root / 'images' / 'test').resolve()}",
        f"nc: {len(names)}",
        f"names: {names}",
        "",
    ]))
    print(f"\nYAML: {yaml_path}")


if __name__ == "__main__":
    main()
