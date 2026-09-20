"""Evaluate fixed rectangular pose ONNX files with identical labels and letterboxing.

Uses the installed Ultralytics dataset, NMS and OKS/AP implementation, bypassing
its standalone validator's square-only ONNX warmup/input assumption.
"""
import argparse
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import torch
from torch.utils.data import DataLoader
from ultralytics.data.utils import check_det_dataset
from ultralytics.models.yolo.pose import PoseValidator

from benchmark_quantization import make_session, measure


@torch.inference_mode()
def evaluate(model_path, data_path, output_dir, limit=0):
    session = make_session(model_path, 2)
    info = session.get_inputs()[0]
    _, _, h, w = info.shape
    validator = PoseValidator(args={'task': 'pose', 'data': str(data_path), 'imgsz': max(h, w),
                                   'batch': 1, 'rect': True, 'workers': 0, 'plots': False,
                                   'conf': 0.001, 'iou': 0.7, 'save_json': False},
                              save_dir=output_dir)
    validator.data = check_det_dataset(str(data_path))
    validator.device = torch.device('cpu')
    validator.training = False
    validator.stride = 32
    dataset = validator.build_dataset(validator.data['val'], batch=1)
    dataset.batch_shapes[:] = np.array([h, w])
    validator.dataloader = DataLoader(dataset, batch_size=1, shuffle=False,
                                      num_workers=0, collate_fn=dataset.collate_fn)
    validator.init_metrics(SimpleNamespace(names={0: 'person'}, end2end=False))
    for index, batch in enumerate(validator.dataloader):
        validator.batch_i = index
        batch = validator.preprocess(batch)
        assert list(batch['img'].shape) == info.shape
        raw = session.run(None, {info.name: batch['img'].numpy()})[0]
        validator.update_metrics(validator.postprocess(torch.from_numpy(raw)), batch)
        if (index + 1) % 100 == 0:
            print(f'[VAL] {model_path.name}: {index + 1}/{len(dataset)}', flush=True)
        if limit and index + 1 >= limit:
            break
    result = validator.get_stats()
    result['images'] = validator.seen
    return {k: float(v) if isinstance(v, (float, np.floating)) else v for k, v in result.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--models', nargs='+', type=Path, required=True)
    parser.add_argument('--data', type=Path, default=Path('datasets/pose_distill_4000/pose.yaml'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--limit', type=int, default=0)
    args = parser.parse_args()
    torch.set_num_threads(2)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    results = {}
    for path in args.models:
        results[path.name] = {'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                              'accuracy': evaluate(path, args.data, args.output.parent, args.limit),
                              'cpu': measure(path, 2, 20, 300, 4200)}
        args.output.write_text(json.dumps(results, indent=2), encoding='utf-8')
        print(json.dumps({path.name: results[path.name]}, indent=2), flush=True)


if __name__ == '__main__':
    main()
