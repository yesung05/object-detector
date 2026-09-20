"""Bounded COCO train2017 supplement, preserving existing validation/test splits."""
import argparse
import hashlib
import io
import json
import random
import shutil
import urllib.request
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from PIL import Image
import yaml

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'datasets/tier2_supplement_20260920'
NAMES = ['cat', 'dog', 'bottle', 'cup', 'food', 'chair', 'dining table']
FOODS = {'banana', 'apple', 'sandwich', 'orange', 'broccoli', 'carrot', 'hot dog', 'pizza', 'donut', 'cake'}

def prepare(download=False):
    OUT.mkdir(parents=True, exist_ok=True)
    old = ROOT / 'datasets/tier2_v3'
    print('[PLAN] reading COCO annotations', flush=True)
    coco = json.loads((ROOT / 'datasets/tier2/annotations/instances_train2017.json').read_text(encoding='utf-8'))
    catnames = {c['id']: c['name'] for c in coco['categories']}
    mapping = {cid: (4 if name in FOODS else NAMES.index(name))
               for cid, name in catnames.items() if name in FOODS or name in NAMES}
    anns = defaultdict(list)
    for ann in coco['annotations']:
        anns[ann['image_id']].append(ann)
    split_paths = {s: sorted((old / 'images' / s).glob('*.jpg')) for s in ('train', 'val', 'test')}
    ids = {s: {int(p.stem) for p in paths} for s, paths in split_paths.items()}
    assert all(ids.values())
    assert not (ids['train'] & ids['val'] or ids['train'] & ids['test'] or ids['val'] & ids['test'])
    excluded = set.union(*ids.values())
    licenses = {x['id']: x for x in coco['licenses']}
    groups = defaultdict(list)
    for im in coco['images']:
        if im['id'] in excluded:
            continue
        # Limit new downloads to images listed by COCO as CC BY, retaining provenance.
        if '/licenses/by/' not in licenses[im['license']]['url']:
            continue
        aa = anns[im['id']]
        target = [a for a in aa if a['category_id'] in mapping]
        if any(a.get('iscrowd') for a in target):
            continue
        present = {mapping[a['category_id']] for a in target}
        for cls in ('bottle', 'cup', 'food'):
            if NAMES.index(cls) in present:
                groups[cls].append(im)
        if not target and any(catnames[a['category_id']] in
                              {'backpack', 'handbag', 'laptop', 'book', 'vase', 'teddy bear', 'keyboard', 'couch'} for a in aa):
            groups['negative'].append(im)
    rng = random.Random(20260920)
    selected, used = [], set()
    for group, count in [('bottle', 200), ('cup', 200), ('food', 200), ('negative', 400)]:
        pool = groups[group]
        rng.shuffle(pool)
        chosen = [im for im in pool if im['id'] not in used][:count]
        for im in chosen:
            used.add(im['id'])
            selected.append({'image': im, 'group': group, 'license': licenses[im['license']]})
        print('[CANDIDATES]', group, 'available', len(pool), 'selected', len(chosen), flush=True)
    assert len(selected) >= 400
    (OUT / 'selection.json').write_text(json.dumps(selected, indent=2), encoding='utf-8')
    if not download:
        return
    assert shutil.disk_usage(ROOT).free > 3 * 2**30, 'Need at least 3 GiB free'
    image_dir, label_dir = OUT / 'images/train', OUT / 'labels/train'
    image_dir.mkdir(parents=True, exist_ok=True)
    label_dir.mkdir(parents=True, exist_ok=True)

    def fetch(row):
        im = row['image']
        dest = image_dir / im['file_name']
        url = 'https://s3.amazonaws.com/images.cocodataset.org/train2017/' + im['file_name']
        if dest.exists():
            blob = dest.read_bytes()
        else:
            with urllib.request.urlopen(url, timeout=45) as response:
                blob = response.read(5 * 2**20 + 1)
            assert len(blob) <= 5 * 2**20, 'Image exceeds 5 MiB cap'
        with Image.open(io.BytesIO(blob)) as image:
            image.load()
            assert image.size == (im['width'], im['height'])
        if not dest.exists():
            dest.write_bytes(blob)
        lines = []
        for a in anns[im['id']]:
            if a['category_id'] not in mapping or a.get('iscrowd'):
                continue
            x, y, w, h = a['bbox']
            if w < 2 or h < 2:
                continue
            x1, y1 = max(0, x), max(0, y)
            x2, y2 = min(im['width'], x+w), min(im['height'], y+h)
            assert x2 > x1 and y2 > y1
            lines.append(f"{mapping[a['category_id']]} {(x1+x2)/2/im['width']:.6f} {(y1+y2)/2/im['height']:.6f} {(x2-x1)/im['width']:.6f} {(y2-y1)/im['height']:.6f}")
        if row['group'] != 'negative':
            assert lines, 'Positive candidate has no usable target labels'
        (label_dir / (dest.stem + '.txt')).write_text('\n'.join(lines), encoding='utf-8')
        return {**row, 'url': url, 'sha256': hashlib.sha256(blob).hexdigest(),
                'path': dest.as_posix(), 'bytes': len(blob), 'labels': len(lines)}

    rows = []
    with ThreadPoolExecutor(max_workers=4) as pool:
        for row in pool.map(fetch, selected):
            rows.append(row)
            if len(rows) % 50 == 0:
                print('[DOWNLOAD]', len(rows), '/', len(selected), flush=True)
    # Check new images against existing held-out content, beyond COCO ID checks.
    heldout_hashes = {hashlib.sha256(p.read_bytes()).hexdigest() for s in ('val', 'test') for p in split_paths[s]}
    seen_hashes = set()
    accepted = []
    for row in rows:
        if row['sha256'] not in heldout_hashes and row['sha256'] not in seen_hashes:
            accepted.append(row)
            seen_hashes.add(row['sha256'])
    negative_eval = [r for r in accepted if r['group'] == 'negative'][:80]
    evaluation_ids = {r['image']['id'] for r in negative_eval}
    added_train = [r for r in accepted if r['image']['id'] not in evaluation_ids]
    assert negative_eval and added_train
    (OUT / 'negative_eval.txt').write_text('\n'.join(r['path'] for r in negative_eval) + '\n', encoding='utf-8')
    train_paths = [p.as_posix() for p in split_paths['train']] + [r['path'] for r in added_train]
    (OUT / 'train.txt').write_text('\n'.join(train_paths) + '\n', encoding='utf-8')
    cfg = {'path': OUT.as_posix(), 'train': (OUT/'train.txt').as_posix(),
           'val': (old/'images/val').as_posix(), 'test': (old/'images/test').as_posix(), 'names': NAMES, 'nc': 7}
    (OUT / 'data.yaml').write_text(yaml.safe_dump(cfg, sort_keys=False), encoding='utf-8')
    audit = {'existing_train': len(split_paths['train']), 'added_train': len(added_train),
             'added_negative': sum(r['group']=='negative' for r in added_train),
             'negative_eval': len(negative_eval),
             'train_total': len(train_paths), 'val_unchanged': len(split_paths['val']),
             'test_unchanged': len(split_paths['test']), 'download_bytes': sum(r['bytes'] for r in rows),
             'new_vs_heldout_sha256_overlap': 0, 'excluded_by_hash': len(rows)-len(accepted),
             'note': 'COCO-annotation-derived labels; not manually reviewed field negatives', 'images': accepted}
    (OUT/'audit.json').write_text(json.dumps(audit, indent=2), encoding='utf-8')
    print('[DATA READY]', json.dumps({k:v for k,v in audit.items() if k!='images'}), flush=True)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--download', action='store_true')
    prepare(parser.parse_args().download)
