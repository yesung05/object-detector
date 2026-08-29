"""
YOLO ONNX 모델을 INT8 로 양자화합니다.

사전 요구 사항:
  pip install onnxruntime onnxruntime-tools onnx Pillow

CNN 모델에는 static 양자화가 효과적입니다.
dynamic 양자화는 MatMul 위주(RNN, Transformer)에서 유리하며 CNN에는 거의 효과가 없습니다.

i5-4200U (Haswell, AVX2, VNNI 없음) 기대 성능:
  - VNNI 없는 AVX2 경로라 실제 속도 향상은 약 1.5~2.5x 입니다.
  - 문서에 "2–4x" 가 적혀 있다면 VNNI 장비(Cascade Lake+) 기준입니다.

사용법:
  # static (권장, 캘리브레이션 이미지 필요)
  python scripts/quantize.py --model yolo11n-pose-416.onnx --calib-dir samples/

  # dynamic (캘리브레이션 이미지 불필요, CNN에는 거의 효과 없음)
  python scripts/quantize.py --model yolo11n-416.onnx --mode dynamic
"""

import argparse
import os
import sys


def _build_static(model_path: str, calib_dir: str, output_path: str) -> None:
    try:
        import onnx
        from onnxruntime.quantization import (
            CalibrationDataReader,
            QuantType,
            quantize_static,
        )
        from onnxruntime.quantization.quant_pre_process import quant_pre_process
    except ImportError:
        sys.exit("필요 패키지 없음. pip install onnxruntime onnxruntime-tools onnx Pillow")

    # shape inference + 그래프 최적화를 먼저 수행합니다.
    # 이 단계를 생략하면 ORT 가 경고를 내고 양자화 품질이 저하됩니다.
    preprocessed_path = model_path.replace(".onnx", "-preprocessed.onnx")
    print(f"전처리 중: {model_path} → {preprocessed_path}")
    quant_pre_process(model_path, preprocessed_path, skip_optimization=False)

    class YoloCalibReader(CalibrationDataReader):
        """C 코드와 동일한 letterbox 전처리로 캘리브레이션 데이터를 공급합니다."""

        def __init__(self, calib_dir: str, input_shape: tuple, input_name: str):
            from PIL import Image
            import numpy as np

            self._input_name = input_name
            _, _, h, w = input_shape
            extensions = (".jpg", ".jpeg", ".png", ".bmp")
            files = [
                os.path.join(calib_dir, f)
                for f in sorted(os.listdir(calib_dir))
                if f.lower().endswith(extensions)
            ]
            if not files:
                sys.exit(f"캘리브레이션 이미지가 없습니다: {calib_dir}")
            print(f"캘리브레이션 이미지 {len(files)}장 로드 중...")

            tensors = []
            for path in files[:300]:  # 최대 300장으로 제한
                img = Image.open(path).convert("RGB")
                iw, ih = img.size
                scale = min(w / iw, h / ih)
                rw, rh = int(iw * scale), int(ih * scale)
                pad_x = (w - rw) // 2
                pad_y = (h - rh) // 2
                # 114 패딩 — C 코드의 letterbox_to_nchw 와 동일
                canvas = np.full((h, w, 3), 114, dtype=np.uint8)
                resized = img.resize((rw, rh))
                canvas[pad_y : pad_y + rh, pad_x : pad_x + rw] = np.array(resized)
                # NCHW float32, /255.0
                tensor = canvas.astype(np.float32) / 255.0
                tensor = tensor.transpose(2, 0, 1)[np.newaxis]  # (1,3,H,W)
                tensors.append(tensor)

            self._data = iter(tensors)

        def get_next(self):
            try:
                return {self._input_name: next(self._data)}
            except StopIteration:
                return None

    import onnx
    import onnxruntime as ort

    model = onnx.load(preprocessed_path)
    input_name = model.graph.input[0].name
    t = model.graph.input[0].type.tensor_type
    shape = tuple(d.dim_value for d in t.shape.dim)
    print(f"입력 이름: {input_name}, 형상: {shape}")

    reader = YoloCalibReader(calib_dir, shape, input_name)
    print(f"static INT8 양자화 중: {preprocessed_path} → {output_path}")
    quantize_static(
        preprocessed_path,
        output_path,
        reader,
        weight_type=QuantType.QInt8,
    )
    os.remove(preprocessed_path)
    print(f"완료: {output_path}")
    _print_size_comparison(model_path, output_path)


def _build_dynamic(model_path: str, output_path: str) -> None:
    try:
        from onnxruntime.quantization import QuantType, quantize_dynamic
    except ImportError:
        sys.exit("필요 패키지 없음. pip install onnxruntime onnxruntime-tools onnx")

    print(f"dynamic INT8 양자화 중: {model_path} → {output_path}")
    print("주의: CNN 모델에는 dynamic 양자화 효과가 거의 없습니다.")
    quantize_dynamic(model_path, output_path, weight_type=QuantType.QUInt8)
    print(f"완료: {output_path}")
    _print_size_comparison(model_path, output_path)


def _print_size_comparison(original: str, quantized: str) -> None:
    orig_mb = os.path.getsize(original) / 1024 / 1024
    quant_mb = os.path.getsize(quantized) / 1024 / 1024
    ratio = orig_mb / quant_mb if quant_mb > 0 else 0
    print(f"크기: {orig_mb:.1f} MB → {quant_mb:.1f} MB ({ratio:.1f}x 압축)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="ONNX 모델 INT8 양자화")
    parser.add_argument("--model", required=True, help="입력 FP32 .onnx 파일")
    parser.add_argument(
        "--mode",
        choices=["static", "dynamic"],
        default="static",
        help="양자화 방식 (기본: static)",
    )
    parser.add_argument(
        "--calib-dir",
        default="samples",
        help="static 모드용 캘리브레이션 이미지 폴더",
    )
    parser.add_argument("--output", default=None, help="출력 파일명 (기본: 자동)")
    args = parser.parse_args()

    suffix = "-int8"
    output = args.output or args.model.replace(".onnx", f"{suffix}.onnx")

    if args.mode == "static":
        _build_static(args.model, args.calib_dir, output)
    else:
        _build_dynamic(args.model, output)
