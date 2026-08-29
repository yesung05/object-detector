"""
YOLO11n-pose 모델을 ONNX 형식으로 내보냅니다.

사전 요구 사항:
  pip install ultralytics onnx onnxruntime

사용법:
  python scripts/export_pose.py [--size 416] [--pt yolo11n-pose.pt]

출력:
  yolo11n-pose-416.onnx  (입력 크기를 --size 로 변경하면 파일명도 바뀝니다)
"""

import argparse
import sys


def export(pt_path: str, size: int) -> str:
    try:
        from ultralytics import YOLO
    except ImportError:
        sys.exit("ultralytics 가 설치되어 있지 않습니다. pip install ultralytics 를 실행하세요.")

    model = YOLO(pt_path)

    # nms=False 가 핵심입니다.
    # nms=True (end-to-end) 로 내보내면 출력 형식이 [1, N, 57] xyxy 가 되어
    # postprocess.c 의 cxcywh 가정과 충돌하고 박스가 완전히 망가집니다.
    # opset=12 는 ORT 1.x 와의 호환성을 보장하는 가장 낮은 안전한 버전입니다.
    out = model.export(
        format="onnx",
        imgsz=size,
        opset=12,
        simplify=True,
        nms=False,
        dynamic=False,
    )

    print(f"내보내기 완료: {out}")
    print(f"기대 출력 shape: [1, 56, {_anchor_count(size)}]")
    print("  채널 구성: 4(box cx/cy/w/h) + 1(conf) + 17×3(keypoint x/y/score)")
    return str(out)


def _anchor_count(size: int) -> int:
    # YOLO 출력 앵커 수: 각 스케일(size/8, size/16, size/32)의 그리드 셀 합계
    return (size // 8) ** 2 + (size // 16) ** 2 + (size // 32) ** 2


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="YOLO11n-pose → ONNX 내보내기")
    parser.add_argument("--pt", default="yolo11n-pose.pt", help="입력 .pt 파일 경로")
    parser.add_argument("--size", type=int, default=416, help="입력 이미지 크기 (기본: 416)")
    args = parser.parse_args()

    export(args.pt, args.size)
