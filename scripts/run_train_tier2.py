"""Tier 2 학습 실행 스크립트 — 단독 프로세스로 실행"""
from ultralytics import YOLO
import pathlib, shutil

if __name__ == '__main__':
    yaml_path = 'datasets/tier2/tier2_data.yaml'
    model = YOLO('yolo11n.pt')
    model.train(
        data=yaml_path,
        epochs=50,
        imgsz=320,
        batch=128,
        device=0,
        patience=10,
        project='runs/tier2_fast',
        name='train',
        exist_ok=True,
        workers=4,
    )

    best_pt = pathlib.Path('runs/tier2_fast/train/weights/best.pt')
    best = YOLO(str(best_pt))
    best.export(format='onnx', imgsz=320, simplify=True)
    best.export(format='onnx', imgsz=320, int8=True, simplify=True)

    pathlib.Path('models').mkdir(exist_ok=True)
    for f in best_pt.parent.glob('*.onnx'):
        dst = pathlib.Path('models') / ('yolo11n_tier2_' + f.stem.split('_')[-1] + '.onnx')
        shutil.copy(f, dst)
        print('복사:', f.name, '->', str(dst))

    print('EXPORT_DONE')
