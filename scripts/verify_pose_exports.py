"""Check FP32 export validity and numerical parity against the pose checkpoint."""
import argparse
import json
from pathlib import Path

import numpy as np
import onnx
import torch
from ultralytics import YOLO

from benchmark_quantization import make_session


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    args = parser.parse_args()
    torch.set_num_threads(2)
    model = YOLO(str(args.run / 'weights/best.pt')).model.fuse().float().eval()
    results = {'fused_parameters': sum(p.numel() for p in model.parameters()), 'exports': {}}
    for h in (224, 288, 416):
        path = args.run / f'yolo11n-pose-2m-416x{h}.onnx'
        graph = onnx.load(str(path))
        onnx.checker.check_model(graph)
        assert not any(t.data_type == onnx.TensorProto.FLOAT16 for t in graph.graph.initializer)
        assert not any(n.op_type in {'QuantizeLinear', 'DequantizeLinear', 'QLinearConv', 'ConvInteger'}
                       for n in graph.graph.node)
        session = make_session(path, 2)
        x = np.random.default_rng(4200).random((1, 3, h, 416), dtype=np.float32)
        with torch.no_grad():
            reference = model(torch.from_numpy(x))[0].numpy()
        actual = session.run(None, {session.get_inputs()[0].name: x})[0]
        np.testing.assert_allclose(actual, reference, rtol=0.001, atol=0.002)
        results['exports'][str(h)] = {'fp32': True, 'parity_passed': True,
                                     'max_abs_error': float(np.max(np.abs(actual - reference)))}
    (args.run / 'export-verification.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
