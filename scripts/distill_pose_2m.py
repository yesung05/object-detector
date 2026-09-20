"""Audited native feature distillation for a ~2M-parameter FP32 pose student."""
from __future__ import annotations

import argparse
import copy
import json
import shutil
from pathlib import Path

import torch
from ultralytics import YOLO
from ultralytics.nn.distill_model import DistillationModel
from ultralytics.utils.torch_utils import unwrap_model

ROOT = Path(__file__).resolve().parents[1]


def audit_distillation(trainer):
    """Fail before training if teacher, KD loss or student gradient is disconnected."""
    model = unwrap_model(trainer.model)
    assert isinstance(model, DistillationModel), 'Trainer did not install distillation wrapper'
    model.train()
    assert not model.teacher_model.training
    assert not any(p.requires_grad for p in model.teacher_model.parameters())
    batch = trainer.preprocess_batch(next(iter(trainer.train_loader)))
    # Preserve running statistics: the diagnostic must not change BN buffers.
    buffers = {k: v.clone() for k, v in model.named_buffers()}
    losses, items = model(batch)
    kd = losses[-1]
    assert torch.isfinite(kd) and kd.item() > 0, 'KD loss is absent or non-finite'
    params = [p for p in model.student_model.parameters() if p.requires_grad]
    grads = torch.autograd.grad(kd, params, allow_unused=True)
    norm = sum(float(g.detach().float().square().sum()) for g in grads if g is not None) ** 0.5
    assert norm > 0 and all(torch.isfinite(g).all() for g in grads if g is not None)
    assert all(p.grad is None for p in model.teacher_model.parameters())
    for key, value in model.named_buffers():
        value.copy_(buffers[key])
    result = {'student_parameters': sum(p.numel() for p in model.student_model.parameters()),
              'distillation_loss': float(items['dis_loss']), 'kd_student_gradient_norm': norm,
              'teacher_frozen': True, 'feature_layers': model.feats_idx,
              'input_shape': list(batch['img'].shape), 'weight': model.dis}
    (Path(trainer.save_dir) / 'distillation-audit.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print('[KD AUDIT PASSED]', json.dumps(result), flush=True)
    model._teacher_feats.clear()
    model._student_feats.clear()
    trainer.optimizer.zero_grad(set_to_none=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--epochs', type=int, default=20)
    parser.add_argument('--batch', type=int, default=32,
                        help='Next-run default: 32; pass 16 to reproduce the initial 2M experiment')
    parser.add_argument('--data', default=str(ROOT / 'datasets/pose_distill_4000/pose.yaml'))
    parser.add_argument('--name', default='pose-distill-2m')
    parser.add_argument('--device', default='0')
    parser.add_argument('--dis', type=float, default=0.5)
    args = parser.parse_args()
    torch.set_num_threads(4)
    teacher_path = ROOT / 'yolo11n-pose.pt'
    teacher = YOLO(str(teacher_path))
    # Keep stock depth and early channels. Only cap the wider later stages.
    cfg = copy.deepcopy(teacher.model.yaml)
    cfg.update(nc=1, scales={'n': [0.5, 0.25, 672]}, scale='n')
    from ultralytics.nn.tasks import PoseModel
    from ultralytics.utils import YAML
    config_path = ROOT / 'scripts/yolo11n-pose-2m.yaml'
    YAML.save(config_path, cfg)
    probe = PoseModel(copy.deepcopy(cfg), verbose=False)
    count = sum(p.numel() for p in probe.parameters())
    assert 1_950_000 <= count <= 2_100_000, count
    print(f'[STUDENT] {count:,} parameters', flush=True)
    del probe, teacher
    student = YOLO(str(config_path))
    student.add_callback('on_train_start', audit_distillation)
    student.train(data=args.data, pretrained=str(teacher_path), distill_model=str(teacher_path),
                  dis=args.dis, epochs=args.epochs, imgsz=416, rect=True, batch=args.batch,
                  workers=0, device=args.device, project=str(ROOT / 'runs/pose'), name=args.name,
                  exist_ok=False, optimizer='AdamW', lr0=0.001, lrf=0.05, warmup_epochs=2,
                  patience=args.epochs, seed=4200, amp=True, plots=True,
                  mosaic=0.0, mixup=0.0, degrees=0, scale=0.25, translate=0.1)
    run = Path(student.trainer.save_dir)
    best = run / 'weights/best.pt'
    outputs = []
    for height in (224, 288, 416):
        exported = YOLO(str(best)).export(format='onnx', imgsz=[height, 416],
                                         half=False, dynamic=False, simplify=True)
        # Isolate experimental candidates from the application's default model directory.
        dest = run / f'yolo11n-pose-2m-416x{height}.onnx'
        shutil.copy2(exported, dest)
        outputs.append(str(dest))
    (run / 'exports.json').write_text(json.dumps(outputs, indent=2), encoding='utf-8')
    print('[COMPLETE]', run, flush=True)


if __name__ == '__main__':
    main()
