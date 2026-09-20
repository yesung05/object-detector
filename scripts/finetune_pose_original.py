"""Fine-tune the original pose model, retaining a separate held-out image split."""
from __future__ import annotations

import argparse
import hashlib
import json
import random
import shutil
import traceback
from collections import defaultdict
from pathlib import Path

import torch
from ultralytics import YOLO
from ultralytics.models.yolo.pose import PoseTrainer
from ultralytics.utils import YAML
from ultralytics.utils.torch_utils import unwrap_model

from distill_pose_fp32 import _link_or_copy, _write_pose_label

ROOT = Path(__file__).resolve().parents[1]


def prepare_dataset(root: Path, maximum: int):
    if (root / 'split-audit.json').exists():
        audit = json.loads((root / 'split-audit.json').read_text(encoding='utf-8'))
        assert audit['requested_train'] == maximum
        return root / 'pose.yaml'
    assert not root.exists(), f'Use a fresh dataset directory: {root}'
    root.mkdir(parents=True)
    old_val = {p.name for p in (ROOT / 'datasets/pose_distill_4000/images/val').glob('*.jpg')}
    assert len(old_val) == 500
    splits = {}
    zero_kpt_train = 0
    for split in ('train', 'val'):
        source = ROOT / 'datasets/tier2'
        print('[PREPARE] loading', split, 'annotations', flush=True)
        payload = json.loads((source / f'annotations/person_keypoints_{split}2017.json').read_text(encoding='utf-8'))
        annotations = defaultdict(list)
        for ann in payload['annotations']:
            if not ann.get('iscrowd') and ann.get('category_id') == 1 and len(ann.get('keypoints', [])) == 51:
                annotations[ann['image_id']].append(ann)
        available = {p.name for p in (source / 'images' / split).glob('*.jpg')}
        candidates = [im for im in payload['images']
                      if im['file_name'] in available
                      and any(a.get('num_keypoints', 0) > 0 for a in annotations[im['id']])]
        candidates.sort(key=lambda im: im['file_name'])
        random.Random(4200 if split == 'train' else 4201).shuffle(candidates)
        if split == 'train':
            candidates = candidates[:maximum]
        paths = []
        for im in candidates:
            anns = annotations[im['id']]
            if split == 'val':
                # Preserve the previous evaluation's labelled-person definition.
                anns = [a for a in anns if a.get('num_keypoints', 0) > 0]
            else:
                # Train boxes for annotated people even when their keypoints are invisible.
                zero_kpt_train += sum(a.get('num_keypoints', 0) == 0 for a in anns)
            target = root / 'images' / split / im['file_name']
            assert _write_pose_label(im, anns, root / 'labels' / split / f'{target.stem}.txt')
            _link_or_copy(source / 'images' / split / im['file_name'], target)
            paths.append(target)
        splits[split] = paths
    train = splits['train']
    val = [p for p in splits['val'] if p.name in old_val]
    test = [p for p in splits['val'] if p.name not in old_val]
    assert len(val) == 500 and len(test) > 0 and len(train) == maximum
    hashes = {}
    for key, paths in [('train', train), ('val', val), ('test', test)]:
        hashes[key] = {hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
        (root / f'{key}.txt').write_text('\n'.join(p.as_posix() for p in paths) + '\n', encoding='utf-8')
    assert not (hashes['train'] & hashes['val'] or hashes['train'] & hashes['test'] or hashes['val'] & hashes['test'])
    cfg = YAML.load(ROOT / 'datasets/pose_distill_4000/pose.yaml')
    cfg.update(path=root.as_posix(), train='train.txt', val='val.txt', test='test.txt')
    YAML.save(root / 'pose.yaml', cfg)
    YAML.save(root / 'heldout.yaml', {**cfg, 'val': 'test.txt'})
    audit = {'requested_train': maximum, 'train': len(train), 'val': len(val), 'test': len(test),
             'cross_split_sha256_overlap': 0, 'zero_keypoint_people_kept_in_training': zero_kpt_train,
             'seed': 4200, 'evaluation_policy': 'non-crowd people with >0 annotated keypoints'}
    (root / 'split-audit.json').write_text(json.dumps(audit, indent=2), encoding='utf-8')
    print('[DATA]', json.dumps(audit), flush=True)
    return root / 'pose.yaml'


class PoseAPTrainer(PoseTrainer):
    def validate(self):
        metrics = self.validator(self)
        if metrics is None:
            return None, None
        metrics.pop('fitness', None)
        fitness = float(metrics['metrics/mAP50-95(P)'])
        if self.best_fitness is None or self.best_fitness < fitness:
            self.best_fitness = fitness
        return metrics, fitness


class ValidationPlateau(torch.optim.lr_scheduler.ReduceLROnPlateau):
    """Ignore the trainer's metric-free epoch tick; step only after validation."""
    def step(self, metrics=None, epoch=None):
        if metrics is not None:
            return super().step(metrics, epoch)


class PlateauPoseTrainer(PoseAPTrainer):
    def _setup_scheduler(self):
        self.lf = lambda epoch: 1.0  # constant target during warmup only
        # Unlike LambdaLR, ReduceLROnPlateau does not populate this warmup field.
        for group in self.optimizer.param_groups:
            group.setdefault('initial_lr', group['lr'])
        self.scheduler = ValidationPlateau(
            self.optimizer, mode='max', factor=0.5, patience=2,
            threshold=0.0, threshold_mode='abs', min_lr=1e-6)

    def validate(self):
        metrics, fitness = super().validate()
        if fitness is not None:
            before = [group['lr'] for group in self.optimizer.param_groups]
            self.scheduler.step(fitness)
            after = [group['lr'] for group in self.optimizer.param_groups]
            record = {'epoch': self.epoch + 1, 'pose_map50_95': fitness,
                      'lr_before': before, 'lr_next': after,
                      'scheduler': self.scheduler.state_dict()}
            with (Path(self.save_dir) / 'lr-schedule.jsonl').open('a', encoding='utf-8') as f:
                f.write(json.dumps(record) + '\n')
            if before != after:
                print('[PLATEAU LR REDUCED]', json.dumps(record), flush=True)
        return metrics, fitness


def audit_initial_weights(trainer):
    model = unwrap_model(trainer.model)
    original = YOLO(str(trainer.args.model)).model
    source = dict(original.named_parameters())
    count = 0
    for name, param in model.named_parameters():
        assert name in source and param.shape == source[name].shape, name
        torch.testing.assert_close(param.detach().cpu(), source[name].detach().cpu(), rtol=0, atol=0)
        count += param.numel()
    result = {'parameters': count, 'all_initial_parameters_equal_source': True,
              'source_checkpoint': str(trainer.args.model),
              'batch': trainer.args.batch, 'learning_rate': trainer.args.lr0}
    (Path(trainer.save_dir) / 'initialization-audit.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('[INITIALIZATION PASSED]', json.dumps(result), flush=True)


def finish_report(run, data, baseline_metrics, heldout):
    """Complete fixed-shape validation and the Markdown report without supervision."""
    from evaluate_pose_onnx import evaluate
    from benchmark_quantization import measure
    import numpy as np
    import onnx
    from benchmark_quantization import make_session
    results = {}
    reference = YOLO(str(run / 'weights/best.pt')).model.fuse().float().cpu().eval()
    for h in (224, 288, 416):
        candidate = run / f'yolo11n-pose-finetuned-416x{h}.onnx'
        graph = onnx.load(str(candidate))
        onnx.checker.check_model(graph)
        assert not any(t.data_type == onnx.TensorProto.FLOAT16 for t in graph.graph.initializer)
        session = make_session(candidate, 2)
        x = np.random.default_rng(4200).random((1, 3, h, 416), dtype=np.float32)
        with torch.no_grad():
            y = reference(torch.from_numpy(x))[0].numpy()
        actual = session.run(None, {session.get_inputs()[0].name: x})[0]
        np.testing.assert_allclose(actual, y, rtol=0.001, atol=0.002)
        del session
        for label, path in [('original', ROOT / f'models/yolo11n-pose-416x{h}.onnx'),
                            ('finetuned', candidate)]:
            results[f'{h}-{label}'] = {
                'model': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                'accuracy': evaluate(path, data, run),
                'cpu': measure(path, 2, 20, 300, 4200)}
            (run / 'onnx-comparison.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    pose_key = 'metrics/mAP50-95(P)'
    delta = heldout['finetuned'][pose_key] - heldout['original'][pose_key]
    verdict = '별도 평가 세트에서 포즈 mAP50–95가 상승했다.' if delta > 0 else '별도 평가 세트에서 포즈 mAP50–95 개선을 확인하지 못했다.'
    lines = ['# 원본 Pose 추가 학습 결과', '', verdict, '',
             f'별도 평가 세트 변화: {100 * delta:+.3f}%p. 단일 실험 결과이며 통계적 유의성을 검증하지 않았다.', '',
             '모델 구조와 파라미터 수는 유지했고 기존 배포 모델은 교체하지 않았다.', '',
             '## 동일 고정 입력 ONNX 비교', '',
             '| 입력 | 모델 | Pose mAP50 | Pose mAP50–95 | 평균 ms | CPU ms/추론 |',
             '|---|---|---:|---:|---:|---:|']
    for h in (224, 288, 416):
        for label in ('original', 'finetuned'):
            r = results[f'{h}-{label}']
            lines.append(f"| 416×{h} | {label} | {r['accuracy']['metrics/mAP50(P)']:.4f} | {r['accuracy'][pose_key]:.4f} | {r['cpu']['latency_ms']['mean']:.3f} | {r['cpu']['cpu_ms_per_inference']:.3f} |")
    lines += ['', '## 원본 / 추가 학습: 별도 평가 세트', '',
              '| 모델 | Pose mAP50 | Pose mAP50–95 |', '|---|---:|---:|']
    for label, metrics in heldout.items():
        lines.append(f"| {label} | {metrics['metrics/mAP50(P)']:.4f} | {metrics[pose_key]:.4f} |")
    lines += ['', '## 조건과 원본 기록', '',
              '- COCO 기반 train 12,000장, 기존 val 500장, 남은 val 이미지를 별도 평가에 사용했다. 이미지 해시로 세트 간 중복을 검사했다.',
              '- 별도 평가 세트는 이번 추가 학습의 체크포인트 선택에 사용하지 않았다. COCO 원본 모델의 기존 학습·개발 과정으로부터 독립된 새 현장 테스트 세트는 아니다.',
              '- Train에는 관절이 모두 비가시인 사람의 box도 유지했다. 평가에는 기존 비교와 동일하게 num_keypoints>0인 사람만 포함했다. 공식 COCO AP와 구분해야 한다.',
              '- 지정 시작 체크포인트와 초기 파라미터 일치 검사 통과. 시작 경로는 initialization-audit.json 참조. AdamW, rect=True, 416 학습, 포즈 mAP50–95로 best 선택.',
              '- ONNX 세 입력 크기에서 checker 및 PyTorch 수치 일치 검사 통과. CPU 수치는 i7-8700, ORT CPU 2 threads, warmup 20, 300회 모델 단독 측정이다. i5-4200U 직접 측정은 아니다.',
              '- Batch·학습률·epoch·메모리 등의 실제 설정과 결과는 아래 원본 파일을 참조한다.', '',
              '[학습 설정](args.yaml) · [학습 로그](results.csv) · [초기 가중치 검사](initialization-audit.json) · [분할 검사](../../../datasets/pose_original_12000/split-audit.json)', '',
              '[ONNX 정확도·CPU·SHA256](onnx-comparison.json) · [별도 평가 원본](heldout-comparison.json) · [원본 검증](baseline-validation.json)', '',
              '[Best checkpoint](weights/best.pt)', '']
    for h in (224, 288, 416):
        lines.append(f'- [416×{h} FP32 ONNX](yolo11n-pose-finetuned-416x{h}.onnx)')
    (run / 'REPORT.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--train-images', type=int, default=12000)
    parser.add_argument('--epochs', type=int, default=12)
    parser.add_argument('--patience', type=int, default=4)
    parser.add_argument('--lr-schedule', choices=('linear', 'plateau'), default='linear')
    parser.add_argument('--checkpoint', type=Path, default=ROOT / 'yolo11n-pose.pt')
    parser.add_argument('--batch', type=int, default=32)
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--lr', type=float, default=0.00005)
    parser.add_argument('--name', default='pose-original-finetune')
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    torch.set_num_threads(4)
    data = prepare_dataset(ROOT / f'datasets/pose_original_{args.train_images}', args.train_images)
    if args.prepare_only:
        return
    original = ROOT / 'yolo11n-pose.pt'
    baseline = YOLO(str(original)).val(data=str(data), imgsz=416, rect=True, batch=args.batch,
                                     device=0, workers=0, plots=False,
                                     project=str(ROOT / 'runs/pose'), name=args.name + '-baseline')
    baseline_metrics = baseline.results_dict
    model = YOLO(str(args.checkpoint.resolve()))
    model.add_callback('on_train_start', audit_initial_weights)
    trainer_type = PlateauPoseTrainer if args.lr_schedule == 'plateau' else PoseAPTrainer
    model.train(trainer=trainer_type, data=str(data), epochs=args.epochs, batch=args.batch,
                imgsz=416, rect=True, device=0, workers=args.workers, cache='ram',
                optimizer='AdamW', lr0=args.lr, lrf=0.1, warmup_epochs=1,
                warmup_bias_lr=args.lr, weight_decay=0.0001, patience=args.patience,
                mosaic=0.0, mixup=0.0, scale=0.15, translate=0.05, degrees=0,
                hsv_h=0.01, hsv_s=0.3, hsv_v=0.2, seed=4200,
                amp=True, plots=True, project=str(ROOT / 'runs/pose'), name=args.name)
    run = Path(model.trainer.save_dir)
    (run / 'baseline-validation.json').write_text(json.dumps(baseline_metrics, indent=2), encoding='utf-8')
    best = run / 'weights/best.pt'
    for h in (224, 288, 416):
        exported = YOLO(str(best)).export(format='onnx', imgsz=[h, 416], dynamic=False, simplify=True)
        shutil.copy2(exported, run / f'yolo11n-pose-finetuned-416x{h}.onnx')
    heldout = {}
    for label, path in [('original', original), ('finetuned', best)]:
        metrics = YOLO(str(path)).val(data=str(data.parent / 'heldout.yaml'), imgsz=416, rect=True,
                                      batch=args.batch, device=0, workers=0, plots=False,
                                      project=str(run), name='heldout-' + label)
        heldout[label] = metrics.results_dict
    (run / 'heldout-comparison.json').write_text(json.dumps(heldout, indent=2), encoding='utf-8')
    finish_report(run, data, baseline_metrics, heldout)
    print('[COMPLETE]', run, flush=True)


if __name__ == '__main__':
    try:
        main()
    except Exception:
        traceback.print_exc()
        raise
