"""Read-only model audit; writes evaluation artifacts, never deployment changes."""
import json
from pathlib import Path
import numpy as np
import torch
from ultralytics import YOLO

ROOT = Path(__file__).resolve().parents[1]

def main():
    torch.set_num_threads(2)
    out = ROOT / 'runs/tier2-accuracy-audit'
    out.mkdir(parents=True, exist_ok=True)
    results = {}
    for kind in ('fp32', 'int8'):
        model = ROOT / f'models/yolo11n_tier2_{kind}.onnx'
        metrics = YOLO(str(model), task='detect').val(
            data=str(ROOT / 'datasets/tier2_v3/tier2_v2.yaml'),
            imgsz=320, batch=1, device='cpu', workers=0, conf=0.001,
            iou=0.45, rect=False, plots=False, project=str(out), name=kind)
        box = metrics.box
        rows = []
        for i, cls in enumerate(box.ap_class_index):
            row = {'class': metrics.names[int(cls)], 'AP50': float(box.ap50[i]),
                   'AP50_95': float(box.ap[i]), 'thresholds': {}}
            for threshold in (0.2, 0.35, 0.5):
                idx = int(np.abs(box.px - threshold).argmin())
                row['thresholds'][str(threshold)] = {
                    'precision': float(box.p_curve[i, idx]),
                    'recall': float(box.r_curve[i, idx])}
            rows.append(row)
        results[kind] = {'metrics': metrics.results_dict, 'classes': rows}
        (out / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
        print('[AUDIT]', kind, json.dumps(results[kind]), flush=True)

if __name__ == '__main__':
    main()
