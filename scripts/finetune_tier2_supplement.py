"""Separate Tier 2 experiment with unchanged class semantics and automatic evaluation."""
import argparse
import json
import subprocess
import sys
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / 'datasets/tier2_supplement_20260920/data.yaml'
SOURCE = ROOT / 'runs/detect/runs/tier2_v3_fresh/train/weights/best.pt'


def launch():
    import psutil
    folder = ROOT / 'runs/tier2-supplement-launch'
    folder.mkdir(parents=True, exist_ok=True)
    manifest = folder / 'process.json'
    if manifest.exists():
        previous = json.loads(manifest.read_text())
        if psutil.pid_exists(previous['pid']):
            raise RuntimeError('Previous process exists; inspect before restarting')
    command = [sys.executable, '-u', str(Path(__file__).resolve())]
    with (folder/'stdout.log').open('ab') as stdout, (folder/'stderr.log').open('ab') as stderr:
        p = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL,
                             stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW)
    result = {'pid': p.pid, 'started_utc': datetime.now(timezone.utc).isoformat(), 'command': command}
    manifest.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result))


def train():
    import numpy as np
    import torch
    from ultralytics import YOLO
    from ultralytics.models.yolo.detect import DetectionTrainer
    from finetune_pose_original import PlateauPoseTrainer, audit_initial_weights
    torch.set_num_threads(4)
    assert DATA.exists()
    audit = json.loads((DATA.parent/'audit.json').read_text())
    assert audit['added_train'] > 0 and audit['negative_eval'] == 80

    class DetectionPlateau(DetectionTrainer):
        _setup_scheduler = PlateauPoseTrainer._setup_scheduler

        def validate(self):
            metrics, fitness = super().validate()
            if fitness is not None:
                self.scheduler.step(float(metrics['metrics/mAP50-95(B)']))
                record = {'epoch': self.epoch+1, 'map50_95': float(metrics['metrics/mAP50-95(B)']),
                          'lr_next': [g['lr'] for g in self.optimizer.param_groups]}
                with (Path(self.save_dir)/'lr-schedule.jsonl').open('a', encoding='utf-8') as f:
                    f.write(json.dumps(record)+'\n')
            return metrics, fitness

    source = YOLO(str(SOURCE))
    assert list(source.names.values()) == ['cat','dog','bottle','cup','food','chair','dining table']
    source.add_callback('on_train_start', audit_initial_weights)
    source.train(trainer=DetectionPlateau, data=str(DATA), project=str(ROOT/'runs/tier2-supplement'),
                 name='train', epochs=60, patience=10, batch=32, imgsz=320, workers=2, device=0,
                 cache='ram', optimizer='AdamW', lr0=3e-5, warmup_epochs=1, warmup_bias_lr=3e-5,
                 weight_decay=1e-4, freeze=10, amp=True, seed=20260920,
                 hsv_h=.01, hsv_s=.25, hsv_v=.25, degrees=0, translate=.05, scale=.15,
                 fliplr=.5, flipud=0, mosaic=0, mixup=0, copy_paste=0, plots=True)
    run = Path(source.trainer.save_dir)
    best = run/'weights/best.pt'
    results = {}

    def evaluate(path, split, tag):
        m = YOLO(str(path)).val(data=str(DATA), split=split, imgsz=320, rect=False,
                               batch=32, device=0, workers=0, conf=.001, iou=.45,
                               project=str(run), name=tag, plots=False)
        rows = []
        for i, cid in enumerate(m.box.ap_class_index):
            t = int(np.abs(m.box.px-.2).argmin())
            rows.append({'class':m.names[int(cid)], 'AP50':float(m.box.ap50[i]),
                         'AP50_95':float(m.box.ap[i]), 'precision_at_020':float(m.box.p_curve[i,t]),
                         'recall_at_020':float(m.box.r_curve[i,t])})
        return {'metrics': m.results_dict, 'classes':rows}

    for split in ('val','test'):
        for tag, path in [('original',SOURCE),('finetuned',best)]:
            results[f'{split}-{tag}'] = evaluate(path,split,f'{split}-{tag}')
            (run/'comparison.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    negatives = (DATA.parent/'negative_eval.txt').read_text().splitlines()
    negative_results = {}
    for tag, path in [('original',SOURCE),('finetuned',best)]:
        detector = YOLO(str(path))
        count, images, classes = 0, 0, Counter()
        for pred in detector.predict(negatives, stream=True, imgsz=320, batch=16, rect=False,
                                     device=0, conf=.2, iou=.45, verbose=False):
            count += len(pred.boxes)
            images += bool(len(pred.boxes))
            classes.update(detector.names[int(c)] for c in pred.boxes.cls.cpu().tolist())
        negative_results[tag] = {'images':len(negatives), 'images_with_predictions':images,
                                 'false_positive_boxes':count, 'classes':dict(classes)}
    (run/'negative-comparison.json').write_text(json.dumps(negative_results,indent=2),encoding='utf-8')
    export = YOLO(str(best)).export(format='onnx',imgsz=320,batch=1,dynamic=False,half=False,simplify=True)
    import onnx
    onnx.checker.check_model(onnx.load(export))
    import onnxruntime as ort
    options = ort.SessionOptions()
    options.intra_op_num_threads=2
    session=ort.InferenceSession(str(export),sess_options=options,providers=['CPUExecutionProvider'])
    x=np.random.default_rng(42).random((1,3,320,320),dtype=np.float32)
    reference=YOLO(str(best)).model.fuse().float().cpu().eval()
    with torch.no_grad():
        expected=reference(torch.from_numpy(x))[0].numpy()
    np.testing.assert_allclose(session.run(None,{session.get_inputs()[0].name:x})[0],expected,rtol=.001,atol=.002)
    lines=['# Tier 2 보강 데이터 추가 학습 결과','',
           '배포 모델과 앱 코드는 변경하지 않았다. 기존 7클래스 의미를 유지했다.',
           '기존 best.pt에서 새 옵티마이저로 시작. backbone 10개 모듈 고정, AdamW 3e-5,',
           '검증 mAP50–95가 3 epoch 정체되면 LR 절반 감소(최저 1e-6), Early Stopping 10, 최대 60 epoch.',
           '320 입력 및 약한 색상·이동·크기 증강을 사용했다. 자세한 설정은 args.yaml 참조.','',
           '| split | model | mAP50 | mAP50–95 |','|---|---|---:|---:|']
    for key,value in results.items():
        split,tag=key.split('-')
        m=value['metrics']
        lines.append(f"| {split} | {tag} | {m['metrics/mAP50(B)']:.4f} | {m['metrics/mAP50-95(B)']:.4f} |")
    delta=results['test-finetuned']['metrics']['metrics/mAP50-95(B)']-results['test-original']['metrics']['metrics/mAP50-95(B)']
    lines += ['',f'test mAP50–95 변화: {delta*100:+.3f}%p. 단일 실험이며 통계적 유의성은 검증하지 않았다.','',
              '| 모델 | 추가 배경 평가 이미지 | 검출 발생 이미지 | 검출 박스 |','|---|---:|---:|---:|']
    for tag,r in negative_results.items():
        lines.append(f"| {tag} | {r['images']} | {r['images_with_predictions']} | {r['false_positive_boxes']} |")
    lines += ['', '배경 평가는 COCO 주석상 대상 7종이 없는 80장, confidence 0.20 기준이다.',
              '모든 배경을 사람이 재검수한 것은 아니므로 주석 누락 가능성이 있으며 현장 오탐률이 아니다.',
              '기존 val/test 분할을 유지했다. 최종 test 평가는 모델 선택에 사용하지 않았다.',
              '새 이미지와 기존 val/test의 SHA256 중복은 제외했다. 학습 데이터의 전체 시각적 유사성까지 보장하지 않는다.',
              '수치는 PyTorch 비교이며 배포 INT8/C 후처리의 평가와 동일하지 않다.',
              'FP32 ONNX checker 및 PyTorch 수치 일치 검사 통과. 모델 교체는 하지 않았다.', '',
              '[정확도 원자료](comparison.json) · [배경 평가](negative-comparison.json) · [학습 곡선](results.csv)',
              '[Best checkpoint](weights/best.pt) · [FP32 ONNX](weights/best.onnx)',
              '[데이터 감사](../../../datasets/tier2_supplement_20260920/audit.json)','']
    (run/'REPORT.md').write_text('\n'.join(lines),encoding='utf-8')
    print('[COMPLETE]',run,flush=True)


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--launch',action='store_true')
    if parser.parse_args().launch:
        launch()
    else:
        train()
